#!/usr/bin/env python3
"""Run Glasgow freq-counter sweep across PIO clkdiv settings.

Starts PIO toggle on RPi5 GPIO5 at each clkdiv, then measures
with the Glasgow freq-counter applet. Requires SSH access to RPi5.
"""

import json
import subprocess
import sys
import time

RPi5_HOST = "rpi5-pmod.iot.welland.mithis.com"
TOGGLE_BIN = "/home/tim/rpi5-rp1-pio-bench/toggle/toggle_rpi5"
GLASGOW_BIN = "/home/tim/.local/share/uv/tools/glasgow/bin/glasgow"
TOGGLE_PIN = 5
TOGGLE_DURATION_MS = 30000  # 30 seconds per measurement
GATE_MS = 1000  # 1 second gate time
TMP_DIR = "/home/tim/rpi5-rp1-pio-bench/toggle/tmp"

# clkdiv values to sweep (highest to lowest frequency)
CLKDIVS = [256, 128, 64, 32, 16, 8, 4, 2, 1]

# Also test clkdiv=1 with delay=15 (equivalent to clkdiv=16 delay=0)
EXTRA_TESTS = [
    {"clkdiv": 1, "delay": 15},
]

PIO_CLOCK = 200_000_000  # 200 MHz


def ssh_cmd(cmd, timeout=None):
    """Run a command on RPi5 via SSH."""
    full_cmd = ["ssh", RPi5_HOST, cmd]
    result = subprocess.run(full_cmd, capture_output=True, text=True, timeout=timeout)
    return result


def start_toggle(clkdiv, delay=0, duration_ms=TOGGLE_DURATION_MS):
    """Start PIO toggle on RPi5 in background."""
    cmd = (
        f"nohup {TOGGLE_BIN} --pin {TOGGLE_PIN} --clkdiv {clkdiv} "
        f"--delay {delay} --duration-ms {duration_ms} "
        f"> {TMP_DIR}/toggle_out.txt 2>&1 &"
    )
    ssh_cmd(cmd, timeout=10)


def stop_toggle():
    """Kill any running toggle process."""
    ssh_cmd(f"pkill -f toggle_rpi5 || true", timeout=10)


def run_freq_counter(gate_ms=GATE_MS, count=3):
    """Run Glasgow freq-counter and return parsed JSON results."""
    cmd = (
        f"{GLASGOW_BIN} run freq-counter -V 3.3 --i A7 "
        f"-t {gate_ms} -n {count} --json"
    )
    result = ssh_cmd(cmd, timeout=120)
    # JSON is on stdout, logs on stderr
    if result.returncode != 0:
        print(f"  ERROR: glasgow returned {result.returncode}", file=sys.stderr)
        print(f"  stderr: {result.stderr}", file=sys.stderr)
        return None
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        print(f"  ERROR: could not parse JSON: {result.stdout!r}", file=sys.stderr)
        return None


def expected_freq(clkdiv, delay=0):
    """Calculate expected toggle frequency."""
    cycles_per_toggle = 2 * (clkdiv + delay * clkdiv)
    if delay == 0:
        cycles_per_toggle = 2 * clkdiv
    else:
        # Each instruction takes clkdiv cycles, delay adds delay*clkdiv
        # set pins,1 [delay] / set pins,0 [delay] = 2 * clkdiv * (1 + delay)
        cycles_per_toggle = 2 * clkdiv * (1 + delay)
    return PIO_CLOCK / cycles_per_toggle


def run_sweep():
    results = []

    print(f"Glasgow PLL Freq-Counter Sweep")
    print(f"{'='*70}")
    print(f"PIO clock: {PIO_CLOCK/1e6:.0f} MHz, Gate time: {GATE_MS} ms")
    print(f"Glasgow sample rate: 528 MHz (264 MHz PLL + DDR), Nyquist: 264 MHz")
    print()

    all_tests = [{"clkdiv": cd, "delay": 0} for cd in CLKDIVS] + EXTRA_TESTS

    for test in all_tests:
        clkdiv = test["clkdiv"]
        delay = test["delay"]
        exp_freq = expected_freq(clkdiv, delay)

        label = f"clkdiv={clkdiv}"
        if delay > 0:
            label += f", delay={delay}"

        print(f"--- {label} (expected {exp_freq/1e6:.3f} MHz) ---")

        # Stop any existing toggle
        stop_toggle()
        time.sleep(1)

        # Start toggle
        start_toggle(clkdiv, delay, TOGGLE_DURATION_MS)
        time.sleep(3)  # Wait for PIO to stabilize and Glasgow to synthesize

        # Measure
        measurements = run_freq_counter(gate_ms=GATE_MS, count=3)
        if measurements is None:
            print(f"  FAILED to measure")
            results.append({
                "clkdiv": clkdiv,
                "delay": delay,
                "expected_freq_hz": exp_freq,
                "measured_freq_hz": None,
                "error": "measurement failed",
            })
            continue

        # Average the measurements
        freqs = [m["freq_hz"] for m in measurements]
        edges_list = [m["edges"] for m in measurements]
        avg_freq = sum(freqs) / len(freqs)
        avg_edges = sum(edges_list) / len(edges_list)

        if exp_freq > 0 and avg_freq > 0:
            accuracy = avg_freq / exp_freq * 100
        else:
            accuracy = 0

        result = {
            "clkdiv": clkdiv,
            "delay": delay,
            "expected_freq_hz": exp_freq,
            "measured_freq_hz": avg_freq,
            "avg_edges": avg_edges,
            "accuracy_pct": accuracy,
            "measurements": measurements,
        }
        results.append(result)

        if avg_freq >= 1e6:
            freq_str = f"{avg_freq/1e6:.3f} MHz"
        elif avg_freq >= 1e3:
            freq_str = f"{avg_freq/1e3:.3f} kHz"
        else:
            freq_str = f"{avg_freq:.1f} Hz"

        print(f"  Measured: {freq_str} (accuracy: {accuracy:.2f}%)")
        print(f"  Edges: {[m['edges'] for m in measurements]}")

    # Stop toggle
    stop_toggle()

    # Print summary table
    print()
    print(f"{'='*70}")
    print(f"Summary Table")
    print(f"{'='*70}")
    print(f"{'clkdiv':>8} {'delay':>5} {'Expected':>14} {'Measured':>14} {'Accuracy':>10}")
    print(f"{'-'*8:>8} {'-'*5:>5} {'-'*14:>14} {'-'*14:>14} {'-'*10:>10}")

    for r in results:
        exp = r["expected_freq_hz"]
        meas = r.get("measured_freq_hz")
        if exp >= 1e6:
            exp_str = f"{exp/1e6:.3f} MHz"
        elif exp >= 1e3:
            exp_str = f"{exp/1e3:.3f} kHz"
        else:
            exp_str = f"{exp:.1f} Hz"

        if meas is None:
            meas_str = "FAILED"
            acc_str = "-"
        elif meas == 0:
            meas_str = "0 Hz"
            acc_str = "-"
        else:
            if meas >= 1e6:
                meas_str = f"{meas/1e6:.3f} MHz"
            elif meas >= 1e3:
                meas_str = f"{meas/1e3:.3f} kHz"
            else:
                meas_str = f"{meas:.1f} Hz"
            acc_str = f"{r.get('accuracy_pct', 0):.2f}%"

        delay_str = str(r["delay"])
        print(f"{r['clkdiv']:>8} {delay_str:>5} {exp_str:>14} {meas_str:>14} {acc_str:>10}")

    # Save JSON results
    output_file = "toggle/results/glasgow-freq-counter-sweep.json"
    with open(output_file, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {output_file}")

    return results


if __name__ == "__main__":
    run_sweep()
