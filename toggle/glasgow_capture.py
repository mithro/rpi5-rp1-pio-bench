#!/usr/bin/env python3
"""glasgow_capture.py — Capture Glasgow analyzer data for a fixed duration.

Starts the Glasgow analyzer, waits for the specified duration, then sends
SIGINT to gracefully stop capture and flush the VCD file.

Usage:
  uv run toggle/glasgow_capture.py --pin A7 --duration-ms 500 --output trace.vcd [--host HOST]
"""

import argparse
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description="Glasgow analyzer timed capture")
    parser.add_argument("--pin", default="A7", help="Glasgow pin(s) to capture (default: A7)")
    parser.add_argument("--duration-ms", type=int, default=500, help="Capture duration in ms")
    parser.add_argument("--output", default="trace.vcd", help="Output VCD file path on remote")
    parser.add_argument("--host", default="rpi5-pmod.iot.welland.mithis.com", help="SSH host")
    parser.add_argument("--voltage", default="3.3", help="I/O voltage (default: 3.3)")
    args = parser.parse_args()

    glasgow_cmd = (
        f"export PATH=$HOME/.local/bin:$PATH && "
        f"glasgow run analyzer -V {args.voltage} --i {args.pin} {args.output}"
    )

    print(f"Starting Glasgow capture on {args.pin} for {args.duration_ms} ms...", file=sys.stderr)

    proc = subprocess.Popen(
        ["ssh", f"tim@{args.host}", glasgow_cmd],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )

    # Wait for Glasgow to initialize (bitstream generation takes a moment)
    # Monitor stderr for the "generating bitstream" message
    time.sleep(5)  # Glasgow needs time to generate bitstream and start capture

    # Wait for capture duration
    print(f"Capturing for {args.duration_ms} ms...", file=sys.stderr)
    time.sleep(args.duration_ms / 1000.0)

    # Send SIGINT to gracefully stop
    print("Stopping capture (SIGINT)...", file=sys.stderr)
    proc.send_signal(signal.SIGINT)

    try:
        stdout, stderr = proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()

    print(f"Glasgow stderr: {stderr}", file=sys.stderr)
    print(f"Glasgow exit code: {proc.returncode}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
