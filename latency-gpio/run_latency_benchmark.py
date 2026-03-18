#!/usr/bin/env python3
"""PIO Latency Benchmark — orchestrator script.

Runs on the local (development) machine and coordinates L0/L1/L2/L3 latency
tests between an RPi5 (PIO device-under-test) and an RPi4 (external
stimulus/measurement device) via SSH.

Typical usage:
    uv run latency/run_latency_benchmark.py
    uv run latency/run_latency_benchmark.py --tests L0
    uv run latency/run_latency_benchmark.py --tests L0 L1 L2 L3
    uv run latency/run_latency_benchmark.py --iterations 5000 --json
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# ─── Defaults ───────────────────────────────────────────────
DEFAULT_RPI5_HOST = "rpi5-pmod.iot.welland.mithis.com"
DEFAULT_RPI4_HOST = "rpi4-pmod.iot.welland.mithis.com"
DEFAULT_REMOTE_DIR = "/home/tim/rpi5-rp1-pio-bench"
DEFAULT_INPUT_PIN = 4
DEFAULT_OUTPUT_PIN = 5
DEFAULT_ITERATIONS = 1000
DEFAULT_WARMUP = 50
DEFAULT_SETTLE_SECS = 3
DEFAULT_MEASUREMENT_TIMEOUT = 120
SSH_CONNECT_TIMEOUT = 5

# When --json is used, progress goes to stderr so stdout has only clean JSON.
# Set by main() before any output.
_log_file = sys.stderr


def log(msg: str = "") -> None:
    """Print progress message (to stderr in JSON mode, stdout otherwise)."""
    print(msg, file=_log_file)


# ─── Test layer metadata ────────────────────────────────────
TEST_LAYER_MAP = {"L0": 0, "L1": 1, "L2": 2, "L3": 3}
TEST_NAME_MAP = {
    "L0": "L0 (PIO-only echo)",
    "L1": "L1 (PIO -> ioctl -> PIO)",
    "L2": "L2 (PIO -> DMA -> poll -> PIO)",
    "L3": "L3 (batched DMA, 4KB reads)",
}


@dataclass
class TestResult:
    """Result of a single latency test."""
    test_mode: str
    passed: bool
    exit_code: int
    raw_output: str = ""
    raw_stderr: str = ""
    json_data: dict[str, Any] = field(default_factory=dict)
    error: str = ""


# ─── SSH helpers ────────────────────────────────────────────

def ssh_run(
    host: str,
    cmd: str,
    timeout: int = 30,
    check: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Run a command on a remote host via SSH.

    Uses ControlMaster=no to avoid stale connection issues during
    long-running benchmark sessions.
    """
    full_cmd = [
        "ssh",
        "-o", f"ConnectTimeout={SSH_CONNECT_TIMEOUT}",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
        host,
        cmd,
    ]
    return subprocess.run(
        full_cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=check,
    )


def ssh_check(host: str, label: str) -> bool:
    """Verify SSH connectivity to a host."""
    try:
        r = ssh_run(host, "echo ok", timeout=10)
        if r.returncode == 0 and "ok" in r.stdout:
            return True
        log(f"  {label}: SSH connected but unexpected output: {r.stdout!r}")
        return False
    except subprocess.TimeoutExpired:
        log(f"  {label}: SSH connection timed out")
        return False
    except FileNotFoundError:
        log(f"  {label}: ssh command not found")
        return False


# ─── Pin management ─────────────────────────────────────────

def _ssh_run_ignore_timeout(host: str, cmd: str, timeout: int = 10) -> None:
    """Run SSH command, ignoring timeouts (best-effort cleanup)."""
    try:
        ssh_run(host, cmd, timeout=timeout)
    except subprocess.TimeoutExpired:
        log(f"  WARNING: SSH command timed out on {host}: {cmd}")


def cleanup_pins(rpi5_host: str, rpi4_host: str, input_pin: int, output_pin: int) -> None:
    """Kill leftover processes and restore GPIO pins to safe state."""
    # Kill processes (ignore errors if nothing is running)
    _ssh_run_ignore_timeout(rpi5_host, "sudo pkill -9 -f latency_rpi5; true")
    _ssh_run_ignore_timeout(rpi4_host, "pkill -9 -f latency_rpi4; true")
    time.sleep(1)

    # Restore pins to inputs with pull-downs
    _ssh_run_ignore_timeout(
        rpi5_host,
        f"pinctrl set {input_pin} ip pd; pinctrl set {output_pin} ip pd",
    )
    _ssh_run_ignore_timeout(
        rpi4_host,
        f"pinctrl set {input_pin} ip; pinctrl set {output_pin} ip",
    )
    time.sleep(0.5)


def get_pin_state(host: str, pin: int) -> str:
    """Read pin state via pinctrl."""
    r = ssh_run(host, f"pinctrl get {pin}", timeout=10)
    return r.stdout.strip()


# ─── Build ──────────────────────────────────────────────────

def deploy_and_build(
    host: str,
    remote_dir: str,
    target: str,
    label: str,
) -> bool:
    """Build the binary on the remote host.

    Assumes the source tree is already present at remote_dir
    (synced via git or rsync separately).
    """
    log(f"  Building {target} on {label}...")
    r = ssh_run(
        host,
        f"cd {remote_dir}/latency-gpio && make clean {target}",
        timeout=60,
    )
    if r.returncode != 0:
        log(f"  ERROR: Build failed on {label}:")
        if r.stdout:
            log(f"    stdout: {r.stdout}")
        if r.stderr:
            log(f"    stderr: {r.stderr}")
        return False

    # Verify binary exists
    binary_name = f"latency_{target}"
    r = ssh_run(host, f"test -x {remote_dir}/latency-gpio/{binary_name}", timeout=10)
    if r.returncode != 0:
        log(f"  ERROR: Binary {binary_name} not found after build on {label}")
        return False

    log(f"  {label}: {binary_name} built successfully")
    return True


# ─── Sync source to remote hosts ────────────────────────────

def sync_source(
    local_dir: Path,
    host: str,
    remote_dir: str,
    label: str,
) -> bool:
    """Rsync the latency source directory to a remote host."""
    log(f"  Syncing source to {label}...")

    # Sync the latency directory and shared benchmark stats
    for subdir in ["latency-gpio/", "throughput-piolib/"]:
        local_path = local_dir / subdir
        if not local_path.exists():
            log(f"  ERROR: Local path {local_path} does not exist")
            return False

        try:
            r = subprocess.run(
                [
                    "rsync", "-az", "--delete",
                    "--exclude", "latency_rpi4",
                    "--exclude", "latency_rpi5",
                    "--exclude", "__pycache__",
                    "--exclude", "*.o",
                    str(local_path),
                    f"{host}:{remote_dir}/",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if r.returncode != 0:
                log(f"  ERROR: rsync to {label} failed: {r.stderr}")
                return False
        except subprocess.TimeoutExpired:
            log(f"  ERROR: rsync to {label} timed out")
            return False

    log(f"  {label}: source synced")
    return True


# ─── Test execution ─────────────────────────────────────────

def run_test(
    test_mode: str,
    *,
    rpi5_host: str,
    rpi4_host: str,
    remote_dir: str,
    input_pin: int,
    output_pin: int,
    iterations: int,
    warmup: int,
    settle_secs: int,
    measurement_timeout: int,
    rt_priority: int = 0,
    cpu: int = -1,
) -> TestResult:
    """Run a single latency test (L0, L1, L2, or L3).

    1. Clean up any leftover state
    2. Start the RPi5 PIO program (background, via SSH)
    3. Wait for it to settle
    4. Run the RPi4 measurement program
    5. Collect and parse results
    6. Stop the RPi5 program
    7. Clean up
    """
    rpi5_binary = f"{remote_dir}/latency-gpio/latency_rpi5"
    rpi4_binary = f"{remote_dir}/latency-gpio/latency_rpi4"

    result = TestResult(test_mode=test_mode, passed=False, exit_code=1)

    log(f"\n{'='*64}")
    log(f"  {test_mode} Latency Test ({iterations} iterations, {warmup} warmup)")
    log(f"{'='*64}\n")

    # Step 1: Full cleanup
    cleanup_pins(rpi5_host, rpi4_host, input_pin, output_pin)

    # Verify clean state
    for label, host in [("RPi5", rpi5_host), ("RPi4", rpi4_host)]:
        pin_in = get_pin_state(host, input_pin)
        pin_out = get_pin_state(host, output_pin)
        log(f"  {label} GPIO{input_pin}: {pin_in}")
        log(f"  {label} GPIO{output_pin}: {pin_out}")

    # Step 2: Start RPi5 program
    rpi5_args = f"--test={test_mode} --input-pin={input_pin} --output-pin={output_pin}"
    if test_mode != "L0":
        # RPi5 counts individual edges; RPi4 counts round-trips.
        # Each RPi4 iteration produces 2 edges (rising + falling).
        # RPi5 warmup consumes edges from RPi4 warmup, RPi5 measurement
        # consumes edges from RPi4 measurement — they stay in sync because
        # each RPi5 edge-read blocks until RPi4 sends the next stimulus.
        rpi5_warmup = warmup * 2
        rpi5_iters = iterations * 2
        rpi5_args += f" --iterations={rpi5_iters} --warmup={rpi5_warmup}"
    if rt_priority > 0:
        rpi5_args += f" --rt-priority={rt_priority}"
    if cpu >= 0:
        rpi5_args += f" --cpu={cpu}"

    log(f"\n  Starting RPi5: sudo {rpi5_binary} {rpi5_args}")
    rpi5_proc = subprocess.Popen(
        [
            "ssh", "-o", "ControlMaster=no", "-o", "ControlPath=none",
            rpi5_host, f"sudo {rpi5_binary} {rpi5_args}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Step 3: Wait for PIO to settle
    time.sleep(settle_secs)

    if rpi5_proc.poll() is not None:
        _, stderr = rpi5_proc.communicate()
        result.error = f"RPi5 program exited early: {stderr}"
        log(f"  ERROR: {result.error}")
        cleanup_pins(rpi5_host, rpi4_host, input_pin, output_pin)
        return result

    # Verify output pin is being driven
    pin_state = get_pin_state(rpi5_host, output_pin)
    log(f"  RPi5 GPIO{output_pin} after PIO start: {pin_state}")

    # Step 4: Run RPi4 measurement (always request JSON from RPi4)
    test_layer_num = TEST_LAYER_MAP.get(test_mode, 0)
    rpi4_args = (
        f"--stimulus-pin={input_pin} --response-pin={output_pin} "
        f"--iterations={iterations} --warmup={warmup} "
        f"--test-layer={test_layer_num} --json"
    )
    if rt_priority > 0:
        rpi4_args += f" --rt-priority={rt_priority}"
    if cpu >= 0:
        rpi4_args += f" --cpu={cpu}"
    rpi4_cmd = f"{rpi4_binary} {rpi4_args}"
    log(f"  Running RPi4: {rpi4_cmd}\n")

    try:
        rpi4_result = ssh_run(rpi4_host, rpi4_cmd, timeout=measurement_timeout)
        result.raw_output = rpi4_result.stdout
        result.raw_stderr = rpi4_result.stderr
        result.exit_code = rpi4_result.returncode

        if rpi4_result.returncode != 0:
            result.error = f"RPi4 measurement failed (exit {rpi4_result.returncode})"
            if rpi4_result.stderr:
                log(f"  stderr: {rpi4_result.stderr}")
        else:
            # Parse JSON output from RPi4
            try:
                result.json_data = json.loads(rpi4_result.stdout)
                result.passed = True
            except json.JSONDecodeError as e:
                result.error = f"Failed to parse RPi4 JSON output: {e}"
                log(f"  ERROR: {result.error}")
                log(f"  Raw output: {rpi4_result.stdout[:500]}")

    except subprocess.TimeoutExpired:
        result.error = f"RPi4 measurement timed out ({measurement_timeout}s)"
        log(f"  ERROR: {result.error}")

    # Step 5: Stop RPi5
    log("\n  Stopping RPi5...")
    ssh_run(rpi5_host, "sudo pkill -SIGINT -f latency_rpi5; true", timeout=10)
    time.sleep(2)

    try:
        rpi5_stdout, rpi5_stderr = rpi5_proc.communicate(timeout=10)
        if rpi5_stderr:
            lines = rpi5_stderr.strip().split("\n")
            for line in lines[-5:]:
                log(f"  RPi5: {line}")
    except subprocess.TimeoutExpired:
        rpi5_proc.kill()
        rpi5_proc.communicate()
        log("  RPi5 process killed (timeout)")

    # Step 6: Final cleanup
    cleanup_pins(rpi5_host, rpi4_host, input_pin, output_pin)

    # Print per-test summary
    _print_test_summary(result)

    return result


def _print_test_summary(result: TestResult) -> None:
    """Print results summary for a single test."""
    if result.passed and result.json_data:
        stats = result.json_data.get("results", {}).get("round_trip_ns", {})
        if stats:
            log(f"\n  Results for {result.test_mode}:")
            log(f"    Min:    {stats.get('min', 0):>10.0f} ns")
            log(f"    Median: {stats.get('median', 0):>10.0f} ns")
            log(f"    Mean:   {stats.get('mean', 0):>10.1f} ns")
            log(f"    P95:    {stats.get('p95', 0):>10.0f} ns")
            log(f"    P99:    {stats.get('p99', 0):>10.0f} ns")
            log(f"    Max:    {stats.get('max', 0):>10.0f} ns")
    elif result.error:
        log(f"\n  FAILED: {result.error}")


def run_test_standalone(
    test_mode: str,
    *,
    rpi5_host: str,
    remote_dir: str,
    iterations: int,
    warmup: int,
    measurement_timeout: int,
    rt_priority: int = 0,
    cpu: int = -1,
) -> TestResult:
    """Run an RPi5-standalone test (no RPi4 involvement).

    Used for L3 (batched DMA throughput) which uses an internal PIO
    data generator and measures DMA read performance directly.
    """
    rpi5_binary = f"{remote_dir}/latency-gpio/latency_rpi5"

    result = TestResult(test_mode=test_mode, passed=False, exit_code=1)

    log(f"\n{'='*64}")
    log(f"  {test_mode} Standalone Test ({iterations} iterations, {warmup} warmup)")
    log(f"{'='*64}\n")

    # Build RPi5 command — request JSON output, pass iterations directly
    rpi5_args = (
        f"--test={test_mode} --iterations={iterations} "
        f"--warmup={warmup} --json"
    )
    if rt_priority > 0:
        rpi5_args += f" --rt-priority={rt_priority}"
    if cpu >= 0:
        rpi5_args += f" --cpu={cpu}"
    rpi5_cmd = f"sudo {rpi5_binary} {rpi5_args}"
    log(f"  Running RPi5: {rpi5_cmd}\n")

    try:
        rpi5_result = ssh_run(
            rpi5_host, rpi5_cmd, timeout=measurement_timeout,
        )
        result.raw_output = rpi5_result.stdout
        result.raw_stderr = rpi5_result.stderr
        result.exit_code = rpi5_result.returncode

        if rpi5_result.stderr:
            for line in rpi5_result.stderr.strip().split("\n"):
                log(f"  RPi5: {line}")

        if rpi5_result.returncode != 0:
            result.error = f"RPi5 standalone test failed (exit {rpi5_result.returncode})"
        else:
            try:
                result.json_data = json.loads(rpi5_result.stdout)
                result.passed = True
            except json.JSONDecodeError as e:
                result.error = f"Failed to parse RPi5 JSON output: {e}"
                log(f"  ERROR: {result.error}")
                log(f"  Raw output: {rpi5_result.stdout[:500]}")

    except subprocess.TimeoutExpired:
        result.error = f"RPi5 standalone test timed out ({measurement_timeout}s)"
        log(f"  ERROR: {result.error}")

    _print_test_summary(result)

    return result


# ─── Summary ────────────────────────────────────────────────

def print_summary(results: list[TestResult], json_output: bool) -> int:
    """Print final summary and return exit code."""
    if json_output:
        combined: dict[str, Any] = {
            "benchmark": "rp1-pio-latency",
            "version": "1.0.0",
            "tests": {},
        }
        for r in results:
            entry: dict[str, Any] = {
                "passed": r.passed,
                "exit_code": r.exit_code,
            }
            if r.json_data:
                config = dict(r.json_data.get("config", {}))
                # RPi4 doesn't know the test layer — override with actual mode
                config["test"] = TEST_NAME_MAP.get(r.test_mode, r.test_mode)
                config["test_layer"] = TEST_LAYER_MAP.get(r.test_mode, -1)
                entry["config"] = config
                entry["results"] = r.json_data.get("results", {})
            if r.error:
                entry["error"] = r.error
            combined["tests"][r.test_mode] = entry

        # JSON goes to stdout (even in --json mode)
        print(json.dumps(combined, indent=2))
    else:
        log(f"\n{'='*64}")
        log("  Summary")
        log(f"{'='*64}")
        all_passed = True
        for r in results:
            status = "PASS" if r.passed else "FAIL"
            detail = ""
            if r.passed and r.json_data:
                stats = r.json_data.get("results", {}).get("round_trip_ns", {})
                if stats:
                    detail = f"  (median: {stats.get('median', 0):.0f} ns)"
            elif r.error:
                detail = f"  ({r.error})"
            log(f"  {r.test_mode}: {status}{detail}")
            if not r.passed:
                all_passed = False
        log(f"{'='*64}")

        if all_passed:
            log("\n  All tests PASSED")
        else:
            log("\n  Some tests FAILED")

    return 0 if all(r.passed for r in results) else 1


# ─── Main ───────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="PIO Latency Benchmark — orchestrator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
examples:
  %(prog)s                        Run L0 and L1 tests with defaults
  %(prog)s --tests L0             Run only the L0 (PIO-only) test
  %(prog)s --tests L1             Run only the L1 (CPU-in-loop) test
  %(prog)s --tests L2             Run only the L2 (DMA) test
  %(prog)s --tests L3             Run only the L3 (mmap) test
  %(prog)s --tests L0 L1 L2 L3   Run all four test layers
  %(prog)s --iterations 5000      Run with 5000 measurement iterations
  %(prog)s --json                 Output combined JSON results
  %(prog)s --no-build             Skip building (use existing binaries)
  %(prog)s --no-sync              Skip syncing source (already deployed)
""",
    )

    parser.add_argument(
        "--tests",
        nargs="+",
        default=["L0", "L1"],
        choices=["L0", "L1", "L2", "L3"],
        help="Test modes to run (default: L0 L1)",
    )
    parser.add_argument(
        "--iterations", type=int, default=DEFAULT_ITERATIONS,
        help=f"Number of measurement iterations (default: {DEFAULT_ITERATIONS})",
    )
    parser.add_argument(
        "--warmup", type=int, default=DEFAULT_WARMUP,
        help=f"Warmup iterations (default: {DEFAULT_WARMUP})",
    )
    parser.add_argument(
        "--input-pin", type=int, default=DEFAULT_INPUT_PIN,
        help=f"Input/stimulus GPIO pin (default: {DEFAULT_INPUT_PIN})",
    )
    parser.add_argument(
        "--output-pin", type=int, default=DEFAULT_OUTPUT_PIN,
        help=f"Output/response GPIO pin (default: {DEFAULT_OUTPUT_PIN})",
    )
    parser.add_argument(
        "--rpi5-host", default=DEFAULT_RPI5_HOST,
        help=f"RPi5 hostname (default: {DEFAULT_RPI5_HOST})",
    )
    parser.add_argument(
        "--rpi4-host", default=DEFAULT_RPI4_HOST,
        help=f"RPi4 hostname (default: {DEFAULT_RPI4_HOST})",
    )
    parser.add_argument(
        "--remote-dir", default=DEFAULT_REMOTE_DIR,
        help=f"Remote project directory (default: {DEFAULT_REMOTE_DIR})",
    )
    parser.add_argument(
        "--json", action="store_true",
        help="Output combined results as JSON (progress goes to stderr)",
    )
    parser.add_argument(
        "--no-build", action="store_true",
        help="Skip building binaries (use existing)",
    )
    parser.add_argument(
        "--no-sync", action="store_true",
        help="Skip syncing source to remote hosts",
    )
    parser.add_argument(
        "--settle-secs", type=int, default=DEFAULT_SETTLE_SECS,
        help=f"Seconds to wait after starting RPi5 PIO (default: {DEFAULT_SETTLE_SECS})",
    )
    parser.add_argument(
        "--measurement-timeout", type=int, default=DEFAULT_MEASUREMENT_TIMEOUT,
        help=f"Timeout for RPi4 measurement in seconds (default: {DEFAULT_MEASUREMENT_TIMEOUT})",
    )
    parser.add_argument(
        "--rt-priority", type=int, default=0,
        help="SCHED_FIFO real-time priority 1-99 (default: off)",
    )
    parser.add_argument(
        "--cpu", type=int, default=-1,
        help="Pin measurement process to CPU core N (default: no affinity)",
    )

    return parser.parse_args()


def main() -> int:
    global _log_file
    args = parse_args()

    # In JSON mode, progress goes to stderr so stdout has only clean JSON.
    _log_file = sys.stderr if args.json else sys.stdout

    log("PIO Latency Benchmark")
    log("=" * 64)
    log(f"  Tests:      {', '.join(args.tests)}")
    log(f"  Iterations: {args.iterations}")
    log(f"  Warmup:     {args.warmup}")
    log(f"  Pins:       GPIO{args.input_pin} (input) / GPIO{args.output_pin} (output)")
    log(f"  RPi5:       {args.rpi5_host}")
    log(f"  RPi4:       {args.rpi4_host}")
    log(f"  Remote dir: {args.remote_dir}")
    log("=" * 64)

    # Tests that require RPi4 (external stimulus/measurement)
    STANDALONE_TESTS = {"L3"}
    needs_rpi4 = bool(set(args.tests) - STANDALONE_TESTS)

    # Pre-flight: verify SSH connectivity
    log("\nChecking SSH connectivity...")
    rpi5_ok = ssh_check(args.rpi5_host, "RPi5")
    if not rpi5_ok:
        log("ERROR: Cannot reach RPi5. Aborting.")
        return 1
    if needs_rpi4:
        rpi4_ok = ssh_check(args.rpi4_host, "RPi4")
        if not rpi4_ok:
            log("ERROR: Cannot reach RPi4. Aborting.")
            return 1
    log("  Hosts reachable.\n")

    # Determine local project root (parent of latency/)
    local_dir = Path(__file__).resolve().parent.parent

    # Sync source to remote hosts
    if not args.no_sync:
        log("Syncing source to remote hosts...")
        if not sync_source(local_dir, args.rpi5_host, args.remote_dir, "RPi5"):
            return 1
        if needs_rpi4:
            if not sync_source(local_dir, args.rpi4_host, args.remote_dir, "RPi4"):
                return 1
        log()

    # Build on each host
    if not args.no_build:
        log("Building binaries...")
        if not deploy_and_build(args.rpi5_host, args.remote_dir, "rpi5", "RPi5"):
            return 1
        if needs_rpi4:
            if not deploy_and_build(args.rpi4_host, args.remote_dir, "rpi4", "RPi4"):
                return 1
        log()

    # Initial cleanup (only if using GPIO-based tests)
    if needs_rpi4:
        cleanup_pins(args.rpi5_host, args.rpi4_host, args.input_pin, args.output_pin)

    # Run each test
    results: list[TestResult] = []
    for test_mode in args.tests:
        if test_mode in STANDALONE_TESTS:
            result = run_test_standalone(
                test_mode,
                rpi5_host=args.rpi5_host,
                remote_dir=args.remote_dir,
                iterations=args.iterations,
                warmup=args.warmup,
                measurement_timeout=args.measurement_timeout,
                rt_priority=args.rt_priority,
                cpu=args.cpu,
            )
        else:
            result = run_test(
                test_mode,
                rpi5_host=args.rpi5_host,
                rpi4_host=args.rpi4_host,
                remote_dir=args.remote_dir,
                input_pin=args.input_pin,
                output_pin=args.output_pin,
                iterations=args.iterations,
                warmup=args.warmup,
                settle_secs=args.settle_secs,
                measurement_timeout=args.measurement_timeout,
                rt_priority=args.rt_priority,
                cpu=args.cpu,
            )
        results.append(result)

    # Print summary
    return print_summary(results, args.json)


if __name__ == "__main__":
    sys.exit(main())
