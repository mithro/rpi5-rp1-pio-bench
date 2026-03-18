# GPIO Latency Benchmark -- Design

## Layer Descriptions

### L0: PIO-Only Echo (Hardware Baseline)

A single PIO state machine echoes the input pin state to the output pin with no CPU involvement:

```
.wrap_target
    wait 1 pin 0        ; stall until input goes HIGH
    set pins, 1          ; drive output HIGH
    wait 0 pin 0        ; stall until input goes LOW
    set pins, 0          ; drive output LOW
.wrap
```

Latency per edge at 200 MHz (5 ns/cycle):
- Input synchroniser: 2 cycles (10 ns) when enabled
- WAIT completion: 1 cycle (5 ns)
- SET execution: 1 cycle (5 ns)
- Output pad delay: ~2-5 ns
- Total: ~25-30 ns per edge (sync enabled)

The RPi5 runs this program continuously. The RPi4 measures the round-trip time (stimulus edge -> response edge) which includes RPi4's own GPIO pad delays and RPi5 cable propagation.

### L1: PIO -> ioctl -> CPU -> ioctl -> PIO

Two PIO state machines with CPU mediation:
1. **edge_detector**: monitors input pin, pushes edge events to RX FIFO
2. **output_driver**: reads TX FIFO, drives output pin accordingly

The host CPU reads edge events from RX FIFO via `pio_sm_get()` (ioctl to RP1 firmware) and writes responses to TX FIFO via `pio_sm_put()` (another ioctl). Each round-trip requires two ioctl calls through the RP1 firmware mailbox.

### L2: PIO -> DMA -> CPU Poll -> DMA -> PIO

Same two-SM architecture as L1, but uses single-word DMA transfers instead of ioctl:
1. DMA reads one word from RX FIFO into host memory
2. CPU polls for DMA completion
3. CPU writes response
4. DMA writes one word from host memory to TX FIFO

DMA adds setup overhead (~8 us per transfer) but avoids the firmware mailbox path.

### L3: Batched DMA (Standalone)

Uses an internal PIO data generator (no external GPIO). Measures the latency of 4 KB DMA read transfers from PIO RX FIFO. Runs on RPi5 only, no RPi4 involvement.

## Measurement Method

The RPi4 performs all timing measurements:

1. Memory-maps BCM2711 GPIO registers via `/dev/gpiomem` (no root required for GPIO)
2. Drives stimulus pin HIGH via `GPSET0` register
3. Busy-polls `GPLEV0` register until response pin goes HIGH
4. Records elapsed time via `clock_gettime(CLOCK_MONOTONIC)`
5. Drives stimulus pin LOW, waits for response LOW
6. Records round-trip latency (one iteration = one rising + falling edge pair)

Timing precision: ~68 ns per GPIO register read on BCM2711 (~14.7 MHz polling rate).

## Edge Counting

RPi4 counts round-trips (1 iteration = 2 edges: rising + falling). RPi5 counts individual edges. The orchestrator ensures the RPi5 edge budget is `(warmup + iterations) * 2 + margin`.

## Python Orchestrator

`run_latency_benchmark.py` coordinates the two devices:

1. Syncs source code to both RPis via rsync
2. Builds binaries on each RPi via SSH
3. For each test layer:
   - Cleans up leftover processes and GPIO state
   - Starts RPi5 PIO program (background SSH)
   - Waits for PIO to settle
   - Runs RPi4 measurement program
   - Collects JSON results
   - Stops RPi5 program
   - Restores GPIO pins to safe state
4. Aggregates results across all layers
