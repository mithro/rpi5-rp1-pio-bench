#!/bin/bash
# glasgow_capture_remote.sh — Run on RPi5 to capture Glasgow trace
# Usage: ./glasgow_capture_remote.sh <pin> <duration_ms> <output_file>

PIN="${1:-A7}"
DURATION_MS="${2:-500}"
OUTPUT="${3:-/tmp/glasgow_trace.vcd}"

export PATH="$HOME/.local/bin:$PATH"

# Remove any stale output
rm -f "$OUTPUT"

# Start Glasgow analyzer in background
glasgow run analyzer -V 3.3 --i "$PIN" "$OUTPUT" &
GLASGOW_PID=$!

# Wait for Glasgow to initialize (bitstream generation)
sleep 5

# Capture for specified duration (use python3 to avoid bc dependency)
DURATION_SEC=$(python3 -c "print($DURATION_MS / 1000.0)")
sleep "$DURATION_SEC"

# Send SIGINT to Glasgow for graceful shutdown
kill -INT "$GLASGOW_PID" 2>&1 || true
wait "$GLASGOW_PID" 2>&1 || true

# Report result
if [ -f "$OUTPUT" ]; then
    LINES=$(wc -l < "$OUTPUT")
    echo "Captured $LINES lines to $OUTPUT"
else
    echo "ERROR: No output file created"
    exit 1
fi
