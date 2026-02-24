#!/usr/bin/env python3
"""Verify all Pmod jumper cable connections between two Raspberry Pi devices.

This script tests each GPIO connection between the RPi5 and RPi4 Pmod HAT
adapters in both directions by:
  1. Setting one side's GPIO as output (drive high, then low)
  2. Reading the other side's GPIO as input
  3. Checking the read value matches the driven value
  4. Repeating in the reverse direction

Requires: SSH access to both RPi devices as user 'tim' with sudo access.
Uses 'pinctrl' command available on Raspberry Pi OS for GPIO control.

Usage:
    uv run verify_pmod_connections.py [--verbose]
"""

import argparse
import subprocess
import sys
import time
from dataclasses import dataclass


# -- Device hostnames --

RPI5_HOST = "rpi5-pmod.iot.welland.mithis.com"
RPI4_HOST = "rpi4-pmod.iot.welland.mithis.com"
SSH_USER = "tim"


# -- Pin definitions --
# From DesignSpark.Pmod HAT.py (BCM GPIO numbering)
# See: https://github.com/DesignSparkRS/DesignSpark.Pmod/blob/master/DesignSpark/Pmod/HAT.py

@dataclass
class PmodPin:
    """A pin on a Pmod connector mapped to a BCM GPIO number."""
    connector: str   # "JA", "JB", or "JC"
    pin_num: int     # 1-12 (excluding 5,6,11,12 which are GND/VCC)
    gpio: int        # BCM GPIO number
    function: str    # Alternate function description
    shared_with: str # Other Pmod pin(s) sharing this GPIO, or ""


# All signal pins on the Pmod HAT (excluding GND/VCC pins 5,6,11,12)
PMOD_PINS = [
    # JA - SPI + GPIO
    PmodPin("JA",  1,  8, "SPI0_CE0",               ""),
    PmodPin("JA",  2, 10, "SPI0_MOSI",              "JB2"),
    PmodPin("JA",  3,  9, "SPI0_MISO",              "JB3"),
    PmodPin("JA",  4, 11, "SPI0_SCLK",              "JB4"),
    PmodPin("JA",  7, 19, "PCM_FS/PWM1/SPI1_MISO",  ""),
    PmodPin("JA",  8, 21, "PCM_DOUT/SPI1_SCLK",     ""),
    PmodPin("JA",  9, 20, "PCM_DIN/SPI1_MOSI",      ""),
    PmodPin("JA", 10, 18, "PCM_CLK/PWM0/SPI1_CE0",  ""),
    # JB - SPI + I2C + GPIO
    PmodPin("JB",  1,  7, "SPI0_CE1",               ""),
    PmodPin("JB",  2, 10, "SPI0_MOSI",              "JA2"),
    PmodPin("JB",  3,  9, "SPI0_MISO",              "JA3"),
    PmodPin("JB",  4, 11, "SPI0_SCLK",              "JA4"),
    PmodPin("JB",  7, 26, "GPIO",                    ""),
    PmodPin("JB",  8, 13, "PWM1",                    ""),
    PmodPin("JB",  9,  3, "I2C1_SCL",               ""),
    PmodPin("JB", 10,  2, "I2C1_SDA",               ""),
    # JC - UART + GPIO
    PmodPin("JC",  1, 16, "UART0_CTS/SPI1_CE2",     ""),
    PmodPin("JC",  2, 14, "UART0_TXD",              ""),
    PmodPin("JC",  3, 15, "UART0_RXD",              ""),
    PmodPin("JC",  4, 17, "UART0_RTS/SPI1_CE1",     ""),
    PmodPin("JC",  7,  4, "GPCLK0",                 ""),
    PmodPin("JC",  8, 12, "PWM0",                    ""),
    PmodPin("JC",  9,  5, "GPCLK1",                 ""),
    PmodPin("JC", 10,  6, "GPCLK2",                 ""),
]


def get_unique_pins():
    """Return de-duplicated list of pins (shared SPI bus pins only tested once)."""
    seen_gpios = set()
    unique = []
    for pin in PMOD_PINS:
        if pin.gpio not in seen_gpios:
            seen_gpios.add(pin.gpio)
            unique.append(pin)
    return unique


def ssh_cmd(host, command, timeout=10):
    """Run a single command on a remote host via SSH.

    Returns (returncode, stdout, stderr).
    """
    full_cmd = [
        "ssh",
        f"{SSH_USER}@{host}",
        command,
    ]
    try:
        result = subprocess.run(
            full_cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return result.returncode, result.stdout.strip(), result.stderr.strip()
    except subprocess.TimeoutExpired:
        return -1, "", f"SSH command timed out after {timeout}s"


def set_gpio_output(host, gpio, high):
    """Set a GPIO pin as output, driven high or low."""
    level = "dh" if high else "dl"
    cmd = f"sudo pinctrl set {gpio} op {level}"
    rc, stdout, stderr = ssh_cmd(host, cmd)
    if rc != 0:
        raise RuntimeError(
            f"Failed to set GPIO{gpio} on {host}: {stderr}"
        )


def set_gpio_input(host, gpio):
    """Set a GPIO pin as input with pull-up disabled."""
    # Use 'pn' (pull none) to avoid pull-up/pull-down influencing the read
    cmd = f"sudo pinctrl set {gpio} ip pn"
    rc, stdout, stderr = ssh_cmd(host, cmd)
    if rc != 0:
        raise RuntimeError(
            f"Failed to set GPIO{gpio} as input on {host}: {stderr}"
        )


def read_gpio(host, gpio):
    """Read the current level of a GPIO pin. Returns True for high, False for low."""
    cmd = f"sudo pinctrl get {gpio}"
    rc, stdout, stderr = ssh_cmd(host, cmd)
    if rc != 0:
        raise RuntimeError(
            f"Failed to read GPIO{gpio} on {host}: {stderr}"
        )
    # pinctrl output looks like:
    # " 8: ip    pu | hi // GPIO8 = input"
    # or
    # " 8: ip    pu | lo // GPIO8 = input"
    if "| hi" in stdout:
        return True
    elif "| lo" in stdout:
        return False
    else:
        raise RuntimeError(
            f"Unexpected pinctrl output for GPIO{gpio} on {host}: {stdout}"
        )


def reset_gpio_input_pullup(host, gpio):
    """Reset a GPIO pin back to input with pull-up (safe default)."""
    cmd = f"sudo pinctrl set {gpio} ip pu"
    ssh_cmd(host, cmd)


def test_connection(sender_host, sender_name, receiver_host, receiver_name,
                    gpio, verbose=False):
    """Test a single GPIO connection in one direction.

    Returns (pass_high, pass_low) tuple of booleans.
    """
    errors = []

    # Set receiver as input (no pull)
    set_gpio_input(receiver_host, gpio)
    # Small delay for pin state to settle
    time.sleep(0.05)

    # Test driving HIGH
    set_gpio_output(sender_host, gpio, high=True)
    time.sleep(0.05)
    read_high = read_gpio(receiver_host, gpio)
    pass_high = read_high is True
    if not pass_high:
        errors.append(f"drove HIGH, read {'HIGH' if read_high else 'LOW'}")

    # Test driving LOW
    set_gpio_output(sender_host, gpio, high=False)
    time.sleep(0.05)
    read_low = read_gpio(receiver_host, gpio)
    pass_low = read_low is False
    if not pass_low:
        errors.append(f"drove LOW, read {'HIGH' if read_low else 'LOW'}")

    # Reset sender back to input
    reset_gpio_input_pullup(sender_host, gpio)
    # Reset receiver back to input
    reset_gpio_input_pullup(receiver_host, gpio)

    if verbose and errors:
        for e in errors:
            print(f"    DETAIL: {sender_name}→{receiver_name} GPIO{gpio}: {e}")

    return pass_high, pass_low


def verify_ssh_access(host):
    """Verify SSH connectivity to a host."""
    rc, stdout, stderr = ssh_cmd(host, "hostname")
    if rc != 0:
        print(f"FATAL: Cannot SSH to {host}: {stderr}")
        return False
    print(f"  {host} -> {stdout}")
    return True


def verify_pinctrl(host):
    """Verify pinctrl command is available."""
    rc, stdout, stderr = ssh_cmd(host, "sudo pinctrl get 0")
    if rc != 0:
        print(f"FATAL: 'pinctrl' not working on {host}: {stderr}")
        return False
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Verify Pmod jumper cable connections between RPi5 and RPi4"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Show detailed output for each test"
    )
    args = parser.parse_args()

    print("=" * 70)
    print("Pmod Jumper Cable Connection Verification")
    print("=" * 70)
    print()

    # Step 1: Verify SSH access
    print("Checking SSH connectivity...")
    if not verify_ssh_access(RPI5_HOST):
        sys.exit(1)
    if not verify_ssh_access(RPI4_HOST):
        sys.exit(1)
    print()

    # Step 2: Verify pinctrl
    print("Checking pinctrl availability...")
    if not verify_pinctrl(RPI5_HOST):
        sys.exit(1)
    if not verify_pinctrl(RPI4_HOST):
        sys.exit(1)
    print("  pinctrl OK on both devices")
    print()

    # Step 3: Test each unique GPIO connection
    unique_pins = get_unique_pins()
    total_tests = len(unique_pins) * 2  # Both directions
    passed = 0
    failed = 0
    results = []

    print(f"Testing {len(unique_pins)} unique GPIO connections "
          f"in both directions ({total_tests} tests total)...")
    print("-" * 70)

    for pin in unique_pins:
        shared_note = f" (shared: {pin.shared_with})" if pin.shared_with else ""
        pin_label = f"{pin.connector} pin {pin.pin_num:2d} (GPIO{pin.gpio:2d}) " \
                    f"[{pin.function}]{shared_note}"

        # Test RPi5 → RPi4
        try:
            h5to4, l5to4 = test_connection(
                RPI5_HOST, "RPi5", RPI4_HOST, "RPi4",
                pin.gpio, verbose=args.verbose
            )
        except RuntimeError as e:
            h5to4, l5to4 = False, False
            if args.verbose:
                print(f"    ERROR: RPi5→RPi4 GPIO{pin.gpio}: {e}")

        # Test RPi4 → RPi5
        try:
            h4to5, l4to5 = test_connection(
                RPI4_HOST, "RPi4", RPI5_HOST, "RPi5",
                pin.gpio, verbose=args.verbose
            )
        except RuntimeError as e:
            h4to5, l4to5 = False, False
            if args.verbose:
                print(f"    ERROR: RPi4→RPi5 GPIO{pin.gpio}: {e}")

        fwd_ok = h5to4 and l5to4
        rev_ok = h4to5 and l4to5

        if fwd_ok:
            passed += 1
        else:
            failed += 1

        if rev_ok:
            passed += 1
        else:
            failed += 1

        fwd_status = "PASS" if fwd_ok else "FAIL"
        rev_status = "PASS" if rev_ok else "FAIL"

        status_line = (
            f"  {pin_label}\n"
            f"    RPi5→RPi4: {fwd_status}  |  RPi4→RPi5: {rev_status}"
        )
        print(status_line)
        results.append((pin, fwd_ok, rev_ok))

    # Summary
    print()
    print("=" * 70)
    print(f"RESULTS: {passed}/{total_tests} tests passed, "
          f"{failed}/{total_tests} tests failed")
    print("=" * 70)

    if failed > 0:
        print()
        print("FAILED connections:")
        for pin, fwd_ok, rev_ok in results:
            if not fwd_ok or not rev_ok:
                dirs = []
                if not fwd_ok:
                    dirs.append("RPi5→RPi4")
                if not rev_ok:
                    dirs.append("RPi4→RPi5")
                print(f"  {pin.connector} pin {pin.pin_num} (GPIO{pin.gpio}) "
                      f"[{pin.function}]: {', '.join(dirs)}")
        sys.exit(1)
    else:
        print()
        print("All connections verified successfully!")
        sys.exit(0)


if __name__ == "__main__":
    main()
