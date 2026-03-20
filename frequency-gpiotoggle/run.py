#!/usr/bin/env python3
"""run.py — Orchestrate PIO toggle frequency benchmark.

Coordinates RPi5 (PIO toggle generator) and RPi4 (GPIO edge counter) over SSH
to sweep clock divider and delay settings, collecting frequency measurements.

Requires:
  - SSH access to both RPi5 and RPi4 (key-based, no password)
  - frequency_gpiotoggle compiled on RPi5
  - frequency_gpiotoggle_rpi4 compiled on RPi4

Usage:
  uv run frequency-gpiotoggle/run.py [options]
"""

import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

# Default SSH hosts
DEFAULT_RPI5_HOST = "rpi5-pmod.iot.welland.mithis.com"
DEFAULT_RPI4_HOST = "rpi4-pmod.iot.welland.mithis.com"
DEFAULT_REMOTE_DIR = "/home/tim/rpi5-rp1-pio-bench/toggle"

# Default sweep parameters
DEFAULT_PIN = 4
DEFAULT_DURATION_MS = 3000
DEFAULT_SETTLE_MS = 2000
DEFAULT_CLKDIVS = [256, 128, 64, 32, 16, 8, 4, 2, 1]


@dataclass
class MeasurementResult:
    clkdiv: float
    delay: int
    expected_freq_hz: float
    measured_freq_hz: float
    edges_counted: int
    samples_taken: int
    elapsed_ns: int
    transition_ratio: float
    accuracy_pct: float = 0.0
    nyquist_limited: bool = False

    def __post_init__(self):
        if self.expected_freq_hz > 0:
            self.accuracy_pct = (self.measured_freq_hz / self.expected_freq_hz) * 100
        self.nyquist_limited = self.transition_ratio > 0.45


@dataclass
class BenchmarkConfig:
    pin: int = DEFAULT_PIN
    duration_ms: int = DEFAULT_DURATION_MS
    settle_ms: int = DEFAULT_SETTLE_MS
    clkdivs: list = field(default_factory=lambda: list(DEFAULT_CLKDIVS))
    delays: list = field(default_factory=lambda: [0])
    rpi5_host: str = DEFAULT_RPI5_HOST
    rpi4_host: str = DEFAULT_RPI4_HOST
    remote_dir: str = DEFAULT_REMOTE_DIR
    sys_clk_hz: int = 200_000_000


def ssh_cmd(host, cmd, timeout=30):
    """Run a command on a remote host via SSH."""
    result = subprocess.run(
        ["ssh", f"tim@{host}", cmd],
        capture_output=True, text=True, timeout=timeout
    )
    return result


def run_measurement(config, clkdiv, delay):
    """Run a single toggle + measurement cycle."""
    # Calculate expected frequency
    expected_hz = config.sys_clk_hz / (2.0 * (1 + delay) * clkdiv)

    # Total RPi5 runtime: settle + measurement + settle
    rpi5_duration = config.settle_ms + config.duration_ms + config.settle_ms

    # Start RPi5 toggle generator in background
    rpi5_cmd = (
        f"sudo {config.remote_dir}/frequency_gpiotoggle"
        f" --pin {config.pin}"
        f" --clkdiv {clkdiv}"
        f" --delay {delay}"
        f" --duration-ms {rpi5_duration}"
    )
    rpi5_proc = subprocess.Popen(
        ["ssh", f"tim@{config.rpi5_host}", rpi5_cmd],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )

    # Wait for PIO to start toggling
    time.sleep(config.settle_ms / 1000.0)

    # Run RPi4 edge counter
    rpi4_cmd = (
        f"{config.remote_dir}/frequency_gpiotoggle_rpi4"
        f" --pin {config.pin}"
        f" --duration-ms {config.duration_ms}"
        f" --json"
    )
    try:
        rpi4_result = ssh_cmd(
            config.rpi4_host, rpi4_cmd,
            timeout=config.duration_ms / 1000.0 + 15
        )
    except subprocess.TimeoutExpired:
        rpi5_proc.kill()
        return None

    # Wait for RPi5 to finish
    try:
        rpi5_proc.wait(timeout=rpi5_duration / 1000.0 + 10)
    except subprocess.TimeoutExpired:
        rpi5_proc.kill()

    # Parse RPi4 JSON output
    try:
        data = json.loads(rpi4_result.stdout)
        results = data["results"]
        return MeasurementResult(
            clkdiv=clkdiv,
            delay=delay,
            expected_freq_hz=expected_hz,
            measured_freq_hz=results["measured_freq_hz"],
            edges_counted=results["edges_counted"],
            samples_taken=results["samples_taken"],
            elapsed_ns=results["elapsed_ns"],
            transition_ratio=results["transition_ratio"],
        )
    except (json.JSONDecodeError, KeyError) as e:
        print(f"  ERROR parsing RPi4 output: {e}", file=sys.stderr)
        print(f"  stdout: {rpi4_result.stdout!r}", file=sys.stderr)
        print(f"  stderr: {rpi4_result.stderr!r}", file=sys.stderr)
        return None


def format_freq(hz):
    """Format frequency in human-readable form."""
    if hz >= 1e9:
        return f"{hz/1e9:.3f} GHz"
    elif hz >= 1e6:
        return f"{hz/1e6:.3f} MHz"
    elif hz >= 1e3:
        return f"{hz/1e3:.3f} kHz"
    else:
        return f"{hz:.1f} Hz"


def print_results_table(results):
    """Print results as a formatted table."""
    print()
    print("=" * 90)
    print("PIO Toggle Frequency Benchmark Results")
    print("=" * 90)
    print(f"{'clkdiv':>8} {'delay':>5} {'expected':>14} {'measured':>14}"
          f" {'accuracy':>9} {'ratio':>8} {'notes'}")
    print("-" * 90)

    for r in results:
        notes = ""
        if r.nyquist_limited:
            notes = "NYQUIST ALIASED"
        elif r.edges_counted <= 1:
            notes = "NO EDGES"
        elif r.accuracy_pct > 98:
            notes = "ACCURATE"

        print(f"{r.clkdiv:>8.0f} {r.delay:>5d} {format_freq(r.expected_freq_hz):>14}"
              f" {format_freq(r.measured_freq_hz):>14}"
              f" {r.accuracy_pct:>8.1f}% {r.transition_ratio:>8.4f} {notes}")

    print("=" * 90)


def print_json_results(results, config):
    """Print results as JSON."""
    output = {
        "benchmark": "rp1-pio-toggle-freq",
        "config": {
            "pin": config.pin,
            "sys_clk_hz": config.sys_clk_hz,
            "duration_ms": config.duration_ms,
            "rpi5_host": config.rpi5_host,
            "rpi4_host": config.rpi4_host,
        },
        "measurements": [
            {
                "clkdiv": r.clkdiv,
                "delay": r.delay,
                "expected_freq_hz": r.expected_freq_hz,
                "measured_freq_hz": r.measured_freq_hz,
                "accuracy_pct": round(r.accuracy_pct, 2),
                "edges_counted": r.edges_counted,
                "samples_taken": r.samples_taken,
                "elapsed_ns": r.elapsed_ns,
                "transition_ratio": round(r.transition_ratio, 6),
                "nyquist_limited": r.nyquist_limited,
            }
            for r in results
        ],
        "conclusion": analyse_results(results),
    }
    print(json.dumps(output, indent=2))


def analyse_results(results):
    """Analyse results and return conclusion about clock speed."""
    accurate = [r for r in results
                if not r.nyquist_limited and r.edges_counted > 1
                and r.accuracy_pct > 90]

    if not accurate:
        return {
            "pio_clock_hz": None,
            "confidence": "low",
            "summary": "No accurate measurements obtained.",
        }

    # Calculate average accuracy relative to 200 MHz model
    avg_accuracy = sum(r.accuracy_pct for r in accurate) / len(accurate)

    # If 200 MHz model matches (avg accuracy ~100%), PIO is 200 MHz
    # If 400 MHz model would match, accuracy would be ~50%
    if avg_accuracy > 95:
        return {
            "pio_clock_hz": 200_000_000,
            "confidence": "high",
            "avg_accuracy_pct": round(avg_accuracy, 2),
            "n_accurate_measurements": len(accurate),
            "summary": (
                f"PIO clock confirmed at 200 MHz. "
                f"{len(accurate)} measurements match 200 MHz model "
                f"with {avg_accuracy:.1f}% average accuracy. "
                f"The '400 MHz' claim is FALSE."
            ),
        }
    elif avg_accuracy > 45 and avg_accuracy < 55:
        return {
            "pio_clock_hz": 400_000_000,
            "confidence": "high",
            "avg_accuracy_pct": round(avg_accuracy, 2),
            "summary": "Measurements suggest PIO clock is 400 MHz (2x reported).",
        }
    else:
        return {
            "pio_clock_hz": None,
            "confidence": "low",
            "avg_accuracy_pct": round(avg_accuracy, 2),
            "summary": f"Inconclusive. Average accuracy: {avg_accuracy:.1f}%.",
        }


def main():
    parser = argparse.ArgumentParser(
        description="PIO toggle frequency benchmark orchestrator"
    )
    parser.add_argument("--pin", type=int, default=DEFAULT_PIN,
                        help=f"GPIO pin (default: {DEFAULT_PIN})")
    parser.add_argument("--duration-ms", type=int, default=DEFAULT_DURATION_MS,
                        help=f"Measurement duration per point (default: {DEFAULT_DURATION_MS})")
    parser.add_argument("--clkdivs", type=str, default=None,
                        help="Comma-separated clock dividers (default: 256,128,...,1)")
    parser.add_argument("--delays", type=str, default="0",
                        help="Comma-separated delay values (default: 0)")
    parser.add_argument("--rpi5-host", default=DEFAULT_RPI5_HOST,
                        help=f"RPi5 SSH host (default: {DEFAULT_RPI5_HOST})")
    parser.add_argument("--rpi4-host", default=DEFAULT_RPI4_HOST,
                        help=f"RPi4 SSH host (default: {DEFAULT_RPI4_HOST})")
    parser.add_argument("--json", action="store_true",
                        help="Output JSON instead of table")
    args = parser.parse_args()

    config = BenchmarkConfig(
        pin=args.pin,
        duration_ms=args.duration_ms,
        rpi5_host=args.rpi5_host,
        rpi4_host=args.rpi4_host,
    )

    if args.clkdivs:
        config.clkdivs = [int(x) for x in args.clkdivs.split(",")]
    if args.delays:
        config.delays = [int(x) for x in args.delays.split(",")]

    # Verify connectivity
    print("Checking connectivity...", file=sys.stderr)
    for host_name, host in [("RPi5", config.rpi5_host), ("RPi4", config.rpi4_host)]:
        result = ssh_cmd(host, "hostname", timeout=10)
        if result.returncode != 0:
            print(f"ERROR: Cannot reach {host_name} ({host})", file=sys.stderr)
            return 1
        print(f"  {host_name}: {result.stdout.strip()}", file=sys.stderr)

    # Run sweep
    results = []
    total = len(config.clkdivs) * len(config.delays)
    i = 0

    for delay in config.delays:
        for clkdiv in config.clkdivs:
            i += 1
            expected = config.sys_clk_hz / (2.0 * (1 + delay) * clkdiv)
            print(f"[{i}/{total}] clkdiv={clkdiv}, delay={delay}"
                  f" (expected: {format_freq(expected)})...",
                  file=sys.stderr)

            result = run_measurement(config, clkdiv, delay)
            if result:
                results.append(result)
                print(f"  -> {format_freq(result.measured_freq_hz)}"
                      f" ({result.accuracy_pct:.1f}%)", file=sys.stderr)
            else:
                print("  -> FAILED", file=sys.stderr)

    # Output results
    if args.json:
        print_json_results(results, config)
    else:
        print_results_table(results)
        conclusion = analyse_results(results)
        print(f"\nConclusion: {conclusion['summary']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
