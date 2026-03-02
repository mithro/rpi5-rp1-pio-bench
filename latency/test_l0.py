#!/usr/bin/env python3
"""Test script for L0 and L1 PIO latency - runs on local machine via SSH."""

import subprocess
import sys
import time


RPi5 = "rpi5-pmod.iot.welland.mithis.com"
RPi4 = "rpi4-pmod.iot.welland.mithis.com"
RPi5_BINARY = "/home/tim/rpi5-rp1-pio-bench/latency/latency_rpi5"
RPi4_BINARY = "/home/tim/rpi5-rp1-pio-bench/latency/latency_rpi4"
INPUT_PIN = 4
OUTPUT_PIN = 5


def ssh_run(host, cmd, timeout=10):
    """Run command on remote host via SSH."""
    result = subprocess.run(
        ["ssh", "-o", "ConnectTimeout=5", "-o", "ControlMaster=no",
         "-o", "ControlPath=none", host, cmd],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout.strip(), result.stderr.strip(), result.returncode


def cleanup_all():
    """Kill processes and restore pins to clean state."""
    # Kill any leftover processes
    ssh_run(RPi5, "sudo pkill -9 -f latency_rpi5 2>&1; true")
    ssh_run(RPi4, "pkill -9 -f latency_rpi4 2>&1; true")
    time.sleep(1)

    # Restore pins
    ssh_run(RPi5, f"pinctrl set {INPUT_PIN} ip pd; pinctrl set {OUTPUT_PIN} ip pd")
    ssh_run(RPi4, f"pinctrl set {INPUT_PIN} ip; pinctrl set {OUTPUT_PIN} ip")
    time.sleep(0.5)


def run_test(test_mode, iterations=1000, warmup=50):
    """Run a latency test and return results."""
    print(f"\n{'='*64}")
    print(f"  {test_mode} Latency Test ({iterations} iterations, {warmup} warmup)")
    print(f"{'='*64}\n")

    # Full cleanup before starting
    cleanup_all()

    # Verify clean state
    out, _, _ = ssh_run(RPi5, f"pinctrl get {INPUT_PIN}; pinctrl get {OUTPUT_PIN}")
    print(f"  RPi5 pins: {out}")
    out, _, _ = ssh_run(RPi4, f"pinctrl get {INPUT_PIN}; pinctrl get {OUTPUT_PIN}")
    print(f"  RPi4 pins: {out}")

    # Start RPi5 program
    rpi5_args = f"--test={test_mode} --input-pin={INPUT_PIN} --output-pin={OUTPUT_PIN}"
    if test_mode != "L0":
        # RPi5 counts individual edges, RPi4 counts round-trips.
        # Each RPi4 iteration sends 2 edges (rising + falling).
        # RPi5 needs enough capacity for all RPi4 edges plus margin.
        rpi5_warmup = warmup * 2
        rpi5_iters = (warmup + iterations) * 2 + 100  # total edges with margin
        rpi5_args += f" --iterations={rpi5_iters} --warmup={rpi5_warmup}"
    print(f"\n  Starting RPi5: {rpi5_args}")
    rpi5_proc = subprocess.Popen(
        ["ssh", "-o", "ControlMaster=no", "-o", "ControlPath=none",
         RPi5, f"sudo {RPi5_BINARY} {rpi5_args}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    time.sleep(3)

    if rpi5_proc.poll() is not None:
        _, stderr = rpi5_proc.communicate()
        print(f"  ERROR: RPi5 program exited: {stderr}")
        cleanup_all()
        return 1

    # Check pin state
    out, _, _ = ssh_run(RPi5, f"pinctrl get {OUTPUT_PIN}")
    print(f"  RPi5 GPIO{OUTPUT_PIN} after PIO start: {out}")

    # Run RPi4 measurement
    rpi4_cmd = (
        f"{RPi4_BINARY} --stimulus-pin={INPUT_PIN} --response-pin={OUTPUT_PIN} "
        f"--iterations={iterations} --warmup={warmup}"
    )
    print(f"  Running RPi4: {rpi4_cmd}\n")
    try:
        out, err, rc = ssh_run(RPi4, rpi4_cmd, timeout=120)
        if out:
            print(out)
        if err:
            print(f"  stderr: {err}")
        print(f"  exit code: {rc}")
    except subprocess.TimeoutExpired:
        print("  ERROR: RPi4 measurement timed out (120s)")
        rc = 1

    # Stop RPi5
    print("\n  Stopping RPi5...")
    ssh_run(RPi5, "sudo pkill -SIGINT -f latency_rpi5; true")
    time.sleep(2)
    try:
        _, stderr = rpi5_proc.communicate(timeout=10)
        if stderr:
            # Print last few lines
            lines = stderr.strip().split('\n')
            for line in lines[-5:]:
                print(f"  RPi5: {line}")
    except subprocess.TimeoutExpired:
        rpi5_proc.kill()
        rpi5_proc.communicate()
        print("  RPi5 process killed")

    cleanup_all()
    return rc


def main():
    print("PIO Latency Benchmark Test Suite")
    print("================================\n")

    # L0: PIO-only echo (hardware baseline)
    rc0 = run_test("L0", iterations=1000, warmup=50)

    # L1: PIO -> ioctl -> PIO (CPU in the loop)
    rc1 = run_test("L1", iterations=1000, warmup=50)

    print(f"\n{'='*64}")
    print(f"  Results: L0={'PASS' if rc0 == 0 else 'FAIL'}, "
          f"L1={'PASS' if rc1 == 0 else 'FAIL'}")
    print(f"{'='*64}")

    return 0 if rc0 == 0 and rc1 == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
