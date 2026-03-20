#!/bin/bash
# run_tests.sh — Automated end-to-end test for throughput_pioloop_cyclic
#
# Runs DRAM DMA benchmark multiple times, collects JSON results,
# and reports summary statistics.
#
# Usage: sudo ./run_tests.sh [--iterations=N] [--duration=S] [--json]
# Default: 10 iterations, 1 second each

set -euo pipefail

ITERATIONS=10
DURATION=1
JSON_SUMMARY=0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH="$SCRIPT_DIR/throughput_pioloop_cyclic"

for arg in "$@"; do
    case "$arg" in
        --iterations=*) ITERATIONS="${arg#--iterations=}" ;;
        --duration=*)   DURATION="${arg#--duration=}" ;;
        --json)         JSON_SUMMARY=1 ;;
        --help|-h)
            echo "Usage: sudo $0 [--iterations=N] [--duration=S] [--json]"
            echo "  --iterations=N  Number of test runs (default: 10)"
            echo "  --duration=S    Duration per run in seconds (default: 1)"
            echo "  --json          Output summary as JSON"
            exit 0
            ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

if [ ! -x "$BENCH" ]; then
    echo "ERROR: $BENCH not found or not executable. Run 'make' first."
    exit 1
fi

if [ ! -c /dev/rp1_pio_sram ]; then
    echo "ERROR: /dev/rp1_pio_sram not found. Load rp1_pio_sram.ko first."
    exit 1
fi

pass=0
fail=0
tx_sum=0
rx_sum=0
tx_min=""
tx_max=""
rx_min=""
rx_max=""

echo "=== DRAM DMA Reliability Test ==="
echo "Iterations: $ITERATIONS, Duration: ${DURATION}s each"
echo ""

for i in $(seq 1 "$ITERATIONS"); do
    json_line=$("$BENCH" --dram --duration="$DURATION" --json 2>&1 | tail -1)

    result=$(echo "$json_line" | sed -n 's/.*"result":"\([^"]*\)".*/\1/p')
    tx_mbps=$(echo "$json_line" | sed -n 's/.*"tx_mbps":\([0-9.]*\).*/\1/p')
    rx_mbps=$(echo "$json_line" | sed -n 's/.*"rx_mbps":\([0-9.]*\).*/\1/p')

    if [ "$result" = "PASS" ]; then
        pass=$((pass + 1))
        status="PASS"

        # Accumulate stats (integer arithmetic: multiply by 100 for 2 decimal places)
        tx_int=$(echo "$tx_mbps" | awk '{printf "%d", $1 * 100}')
        rx_int=$(echo "$rx_mbps" | awk '{printf "%d", $1 * 100}')
        tx_sum=$((tx_sum + tx_int))
        rx_sum=$((rx_sum + rx_int))

        if [ -z "$tx_min" ] || [ "$tx_int" -lt "$tx_min" ]; then tx_min=$tx_int; fi
        if [ -z "$tx_max" ] || [ "$tx_int" -gt "$tx_max" ]; then tx_max=$tx_int; fi
        if [ -z "$rx_min" ] || [ "$rx_int" -lt "$rx_min" ]; then rx_min=$rx_int; fi
        if [ -z "$rx_max" ] || [ "$rx_int" -gt "$rx_max" ]; then rx_max=$rx_int; fi
    else
        fail=$((fail + 1))
        status="FAIL"
    fi

    printf "  Run %3d/%d: %s  TX=%.2f MB/s  RX=%.2f MB/s\n" \
           "$i" "$ITERATIONS" "$status" "$tx_mbps" "$rx_mbps"
done

echo ""
echo "=== Summary ==="
echo "  Pass: $pass / $ITERATIONS"
echo "  Fail: $fail / $ITERATIONS"

if [ "$pass" -gt 0 ]; then
    tx_avg=$((tx_sum / pass))
    rx_avg=$((rx_sum / pass))
    printf "  TX throughput: min=%.2f avg=%.2f max=%.2f MB/s\n" \
           "$(echo "$tx_min" | awk '{printf "%.2f", $1/100}')" \
           "$(echo "$tx_avg" | awk '{printf "%.2f", $1/100}')" \
           "$(echo "$tx_max" | awk '{printf "%.2f", $1/100}')"
    printf "  RX throughput: min=%.2f avg=%.2f max=%.2f MB/s\n" \
           "$(echo "$rx_min" | awk '{printf "%.2f", $1/100}')" \
           "$(echo "$rx_avg" | awk '{printf "%.2f", $1/100}')" \
           "$(echo "$rx_max" | awk '{printf "%.2f", $1/100}')"
fi

if [ "$JSON_SUMMARY" -eq 1 ]; then
    if [ "$pass" -gt 0 ]; then
        printf '{"test":"dram_reliability","iterations":%d,"pass":%d,"fail":%d,' \
               "$ITERATIONS" "$pass" "$fail"
        printf '"tx_min":%.2f,"tx_avg":%.2f,"tx_max":%.2f,' \
               "$(echo "$tx_min" | awk '{printf "%.2f", $1/100}')" \
               "$(echo "$tx_avg" | awk '{printf "%.2f", $1/100}')" \
               "$(echo "$tx_max" | awk '{printf "%.2f", $1/100}')"
        printf '"rx_min":%.2f,"rx_avg":%.2f,"rx_max":%.2f}\n' \
               "$(echo "$rx_min" | awk '{printf "%.2f", $1/100}')" \
               "$(echo "$rx_avg" | awk '{printf "%.2f", $1/100}')" \
               "$(echo "$rx_max" | awk '{printf "%.2f", $1/100}')"
    else
        printf '{"test":"dram_reliability","iterations":%d,"pass":0,"fail":%d}\n' \
               "$ITERATIONS" "$fail"
    fi
fi

if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
