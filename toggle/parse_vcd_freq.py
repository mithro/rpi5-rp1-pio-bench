#!/usr/bin/env python3
"""parse_vcd_freq.py — Extract toggle frequency from Glasgow VCD trace.

Parses a VCD file captured by Glasgow analyzer and computes the toggle
frequency from the timing of signal transitions.

Usage:
  uv run toggle/parse_vcd_freq.py <vcd_file>
  ssh host "cat trace.vcd" | uv run toggle/parse_vcd_freq.py -
"""

import json
import re
import sys


def parse_vcd_transitions(vcd_text):
    """Extract (timestamp_ns, value) pairs from VCD text."""
    transitions = []
    current_time = None
    in_defs = True

    for line in vcd_text.splitlines():
        line = line.strip()
        if not line:
            continue

        if line == "$enddefinitions $end":
            in_defs = False
            continue
        if in_defs:
            continue

        # Timestamp line: #<number>
        if line.startswith("#"):
            current_time = int(line[1:])
            continue

        # Value change: <value><identifier> (e.g., "1!" or "0!")
        # Skip $dumpvars/$end and 'x' (unknown) values
        m = re.match(r"^([01])(.+)$", line)
        if m and current_time is not None:
            value = int(m.group(1))
            transitions.append((current_time, value))

    return transitions


def compute_frequency(transitions):
    """Compute toggle frequency from transitions."""
    if len(transitions) < 3:
        return None

    # Filter out initial setup — use transitions after the first few
    # to avoid measuring initial glitches
    edges = transitions[2:]  # skip first couple transitions

    if len(edges) < 4:
        return None

    # Compute half-periods (time between consecutive edges)
    half_periods = []
    for i in range(1, len(edges)):
        dt = edges[i][0] - edges[i - 1][0]
        if dt > 0:
            half_periods.append(dt)

    if not half_periods:
        return None

    # Full periods (pairs of half-periods)
    full_periods = []
    for i in range(0, len(half_periods) - 1, 2):
        full_periods.append(half_periods[i] + half_periods[i + 1])

    avg_half_period_ns = sum(half_periods) / len(half_periods)
    avg_full_period_ns = sum(full_periods) / len(full_periods) if full_periods else avg_half_period_ns * 2

    # Toggle frequency = 1 / (2 * half_period)
    toggle_freq_hz = 1e9 / avg_full_period_ns

    # Statistics
    min_hp = min(half_periods)
    max_hp = max(half_periods)
    jitter_ns = max_hp - min_hp

    return {
        "toggle_freq_hz": toggle_freq_hz,
        "avg_half_period_ns": avg_half_period_ns,
        "avg_full_period_ns": avg_full_period_ns,
        "min_half_period_ns": min_hp,
        "max_half_period_ns": max_hp,
        "jitter_ns": jitter_ns,
        "num_transitions": len(transitions),
        "num_half_periods": len(half_periods),
        "num_full_periods": len(full_periods),
        "capture_duration_ns": transitions[-1][0] - transitions[0][0],
    }


def format_freq(hz):
    if hz >= 1e9:
        return f"{hz / 1e9:.3f} GHz"
    elif hz >= 1e6:
        return f"{hz / 1e6:.3f} MHz"
    elif hz >= 1e3:
        return f"{hz / 1e3:.3f} kHz"
    else:
        return f"{hz:.1f} Hz"


def main():
    if len(sys.argv) < 2:
        print("Usage: parse_vcd_freq.py <vcd_file|->", file=sys.stderr)
        return 1

    if sys.argv[1] == "-":
        vcd_text = sys.stdin.read()
    else:
        with open(sys.argv[1]) as f:
            vcd_text = f.read()

    transitions = parse_vcd_transitions(vcd_text)
    if not transitions:
        print("ERROR: No transitions found in VCD", file=sys.stderr)
        return 1

    result = compute_frequency(transitions)
    if result is None:
        print("ERROR: Not enough transitions to compute frequency", file=sys.stderr)
        return 1

    json_mode = "--json" in sys.argv
    if json_mode:
        print(json.dumps(result, indent=2))
    else:
        print(f"Glasgow VCD Analysis")
        print(f"  Transitions:     {result['num_transitions']}")
        print(f"  Half-periods:    {result['num_half_periods']}")
        print(f"  Full periods:    {result['num_full_periods']}")
        print(f"  Capture span:    {result['capture_duration_ns'] / 1e6:.3f} ms")
        print(f"  Avg half-period: {result['avg_half_period_ns']:.1f} ns")
        print(f"  Avg full period: {result['avg_full_period_ns']:.1f} ns")
        print(f"  Jitter:          {result['jitter_ns']:.1f} ns")
        print(f"  Toggle freq:     {format_freq(result['toggle_freq_hz'])}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
