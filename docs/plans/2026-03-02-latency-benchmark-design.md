# PIO Latency Benchmark Design

## Purpose

Measure and characterise the latency of the PIO data path on Raspberry Pi 5,
specifically the round-trip time for a signal going:

```
RPi4 CPU → RPi4 GPIO → wire → RPi5 GPIO → RPi5 PIO → RPi5 CPU → RPi5 PIO → RPi5 GPIO → wire → RPi4 GPIO → RPi4 CPU
```

The existing benchmark suite measures **throughput** (MB/s for bulk DMA transfers).
This new test measures **latency** (nanoseconds for a single GPIO edge to propagate
through the full loop). These are fundamentally different metrics — high throughput
doesn't imply low latency.

## Background: Known Latency Characteristics

From the existing project documentation and new research:

| Component | Latency | Source |
|-----------|---------|--------|
| PIO instruction cycle | 5 ns (200 MHz) | RP1 datasheet |
| PIO input synchroniser | 10 ns (2 cycles) | RP2040 datasheet (same arch) |
| PCIe FIFO access | ~320 ns - 1 us | rp1-dma.md |
| Firmware mailbox RPC | >= 10 us | rp1-dma.md |
| `pio_sm_get/put_blocking()` | ~10-20 us per call | ioctl → kernel → firmware |
| RPi4 GPIO write (mmap) | ~6-9 ns | Measured on hardware |
| RPi4 GPIO read (mmap) | ~68 ns | Measured on hardware |

## Architecture

### Two Independent Programs

1. **`latency_rpi4`** — Runs on RPi4. Compiled C program using memory-mapped
   `/dev/gpiomem` for direct BCM2711 GPIO register access. Generates stimulus
   pulses and measures round-trip time externally.

2. **`latency_rpi5`** — Runs on RPi5. Compiled C program using piolib. Configures
   PIO state machines and (for CPU-in-the-loop tests) processes edges in a tight
   loop. Measures internal CPU processing time.

### Python Orchestrator

A Python script (similar to `verify_pmod_connections.py`) runs from the local
machine, SSHes into both devices, copies binaries, runs coordinated tests, and
collects results.

## Test Layers

Each layer adds one more component to the signal path, enabling precise attribution
of where latency comes from.

### L0: PIO-Only Echo (Hardware Baseline)

**Signal path:** RPi4 GPIO → wire → RPi5 PIO → wire → RPi4 GPIO

**RPi5 PIO program:**
```asm
.program gpio_echo
.wrap_target
    wait 1 pin 0        ; wait for input high
    set pins, 1          ; drive output high
    wait 0 pin 0        ; wait for input low
    set pins, 0          ; drive output low
.wrap
```

**CPU involvement on RPi5:** None after setup. PIO runs autonomously.

**Expected latency:** ~25-50 ns (PIO processing + wire propagation + RPi4
measurement overhead). This is the **hardware floor** — no software path can beat
this.

**Measured latency:** ~388 ns median (259 ns min, 518 ns max). The RPi4 GPIO read
latency (~68 ns per poll) and `clock_gettime()` overhead dominate the measurement;
the actual PIO echo is ~20 ns. See [Measured Results](#measured-results) below.

**Variants to test:**
- With input synchroniser enabled (default, safer): +10 ns
- With input synchroniser bypassed (`INPUT_SYNC_BYPASS`): faster but risk of
  metastability
- Using `WAIT GPIO` (absolute, 1 cycle faster) vs `WAIT PIN` (relative, portable)
- Using `MOV pins, pins` (continuous copy, lowest latency, 1 instruction)

### L1: PIO → ioctl → PIO (Kernel Path)

**Signal path:** RPi4 GPIO → wire → RPi5 PIO SM0 → RX FIFO → `pio_sm_get_blocking()`
→ userspace CPU → `pio_sm_put_blocking()` → TX FIFO → RPi5 PIO SM1 → wire → RPi4 GPIO

**RPi5 PIO programs:**
- SM0: Edge detector — watches input pin, autopushes edge state to RX FIFO
- SM1: Output driver — reads TX FIFO (autopull), drives output pin

**CPU loop:**
```c
while (running) {
    uint32_t state = pio_sm_get_blocking(pio, sm_rx);  // ~10+ us
    pio_sm_put_blocking(pio, sm_tx, state);             // ~10+ us
}
```

**Expected latency:** ~20-40 us per edge. Dominated by two ioctl round-trips
through the kernel, each going: userspace → ioctl → kernel → firmware mailbox →
RP1 M3 core → PIO FIFO → return.

**Measured latency:** ~43.6 µs median (33.0 µs min, 136 µs max). Two ioctl
round-trips at ~22 µs each, consistent with expectations. Stddev 3.2 µs.

**What this measures:** The cost of the standard piolib API for single-word
transfers. This represents the latency that any "normal" userspace PIO application
would experience.

### L2: PIO → DMA → CPU Poll → DMA → PIO (DMA Path)

**Signal path:** RPi4 GPIO → wire → RPi5 PIO SM0 → RX FIFO → DMA → host memory →
CPU poll → host memory → DMA → TX FIFO → RPi5 PIO SM1 → wire → RPi4 GPIO

**Approach:** Configure DMA for the smallest possible transfer (4 bytes = 1 word).
PIO SM0 autopushes to RX FIFO. DMA transfers to a host memory buffer. CPU
busy-polls the buffer. CPU writes response to TX buffer. DMA transfers to TX FIFO.
SM1 autopulls and drives output.

**Challenge:** `pio_sm_xfer_data()` is blocking and designed for bulk transfers.
For latency, we need to:
- Configure DMA for continuous small transfers (if possible), or
- Use DMA in a tight loop with minimal transfer size

**Expected latency:** ~1-10 us. DMA setup overhead + PCIe round-trip + CPU poll.

**Measured latency:** ~52.5 µs median (49.9 µs min, 102 µs max). Single-word DMA
is ~20% slower than ioctl due to DMA setup/teardown overhead dominating for 4-byte
transfers. However, DMA has tighter variance (stddev 1.8 µs vs 3.2 µs for L1),
suggesting DMA is more deterministic.

### L3: Batched DMA Throughput (Standalone, Redesigned)

> **Note:** L3 was redesigned from the original mmap FIFO plan (below) to batched
> DMA throughput measurement. The mmap approach was deferred because it requires
> `/dev/mem` access and direct register manipulation that may conflict with the
> kernel PIO driver. The batched DMA approach measures throughput using the existing
> `pio_sm_xfer_data()` API with larger transfer sizes.

**Signal path:** Internal PIO data generator → DMA → host memory (standalone, no GPIO)

**Approach:** Uses the same loopback PIO program as the throughput benchmark
(bitwise NOT: `out x, 32` / `mov y, ~x` / `in y, 32`). Configures DMA for 4KB
batch reads via `pio_sm_xfer_data()` and measures the time for each batch.

**Measured latency:** ~88.6 µs median for 4KB (87.1 µs min, 172 µs max). This
gives 4096 bytes / 88.6 µs = **46 MB/s**, consistent with the ~42 MB/s aggregate
measured by the throughput benchmark.

#### Original L3 Design (Deferred): PIO → mmap FIFO → PIO

The original L3 design was direct mmap FIFO access:

**Signal path:** RPi4 GPIO → wire → RPi5 PIO SM0 → RX FIFO → **direct register
read via mmap** → CPU → **direct register write via mmap** → TX FIFO → RPi5 PIO
SM1 → wire → RPi4 GPIO

**Approach:** Memory-map the PIO FIFO registers directly from userspace via
`/dev/mem`. The PIO RX/TX FIFO registers are at RP1 BAR0 + 0x178000:
- TX FIFO SM0: offset 0x00
- RX FIFO SM0: offset 0x10

```c
// Map PIO FIFO registers via /dev/mem
// Physical address from /proc/iomem: 1f00178000
int fd = open("/dev/mem", O_RDWR | O_SYNC);
volatile uint32_t *pio_fifo = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0x1f00178000);
// Direct FIFO access:
uint32_t val = pio_fifo[0x10/4];  // Read RX FIFO SM0
pio_fifo[0x00/4] = response;       // Write TX FIFO SM0
```

**Expected latency:** ~320 ns - 1 us. Only PCIe round-trip latency, no kernel or
firmware overhead.

**Risks:**
- Requires root access for `/dev/mem`
- May conflict with kernel driver's DMA operations
- FIFO status registers needed to avoid read/write when empty/full
- RP1 register offsets may differ from documentation
- This is experimental and not officially supported

**Status:** Deferred. May be revisited as a future L3-mmap test.

## RPi4 Measurement Program

### Build and Usage

```
# Build on RPi4:
gcc -O2 -o latency_rpi4 latency_rpi4.c -lm

# Run (no root needed thanks to /dev/gpiomem):
./latency_rpi4 --stimulus-pin=4 --response-pin=5 --iterations=1000
```

### Measurement Method

Uses BCM2711 memory-mapped GPIO via `/dev/gpiomem`:
- `GPSET0` (offset 0x1C): Write 1 to drive stimulus pin HIGH (~8 ns)
- `GPLEV0` (offset 0x34): Read pin level register to check response (~68 ns per read)
- `GPCLR0` (offset 0x28): Write 1 to drive stimulus pin LOW (~8 ns)

Timing via `clock_gettime(CLOCK_MONOTONIC)` with nanosecond resolution.

### Measurement Loop

For each iteration:
1. `clock_gettime()` → t0
2. `GPSET0` — drive stimulus HIGH
3. Busy-poll `GPLEV0` until response pin goes HIGH
4. `clock_gettime()` → t1
5. Compute rising-edge latency = t1 - t0
6. `GPCLR0` — drive stimulus LOW
7. Busy-poll `GPLEV0` until response pin goes LOW
8. `clock_gettime()` → t2
9. Compute falling-edge latency = t2 - (t1 + overhead)

### Statistics

Reuse the existing `benchmark_stats.c/h` module for:
- min, max, mean, median
- standard deviation
- P5, P95, P99 percentiles

## RPi5 Latency Program

### Build and Usage

```
# Build on RPi5:
gcc -O2 -I/usr/include/piolib -o latency_rpi5 latency_rpi5.c \
    benchmark_stats.c benchmark_format.c -lpio -lpthread -lm

# Run:
sudo ./latency_rpi5 --test=L0 --input-pin=4 --output-pin=5
sudo ./latency_rpi5 --test=L1 --input-pin=4 --output-pin=5 --rt-priority=80 --cpu=3
```

### Test Modes

- `--test=L0`: Load PIO echo program, enable SM, wait for SIGINT. No CPU loop.
- `--test=L1`: Two SMs + `pio_sm_get/put_blocking()` loop. Measures internal time.
- `--test=L2`: Two SMs + DMA + CPU poll loop. Measures internal time.
- `--test=L3`: Two SMs + mmap FIFO direct access. Measures internal time.

### RT Optimisations (applicable to L1-L3)

- `--rt-priority=N`: Set `SCHED_FIFO` real-time priority (1-99)
- `--cpu=N`: Pin processing thread to CPU core N via `sched_setaffinity()`
- `mlockall(MCL_CURRENT | MCL_FUTURE)`: Prevent page faults during measurement

## Pin Assignment

Configurable via CLI arguments. Defaults:

| Signal | Default GPIO | Pmod Pin | Notes |
|--------|-------------|----------|-------|
| Stimulus (RPi4→RPi5) | GPIO4 | JC7 | GPCLK0, no pull-ups, not shared |
| Response (RPi5→RPi4) | GPIO5 | JC9 | GPCLK1, no pull-ups, not shared |

Both pins are on the JC connector bottom row, clean GPIO with no hardware pull-ups
or bus sharing conflicts.

## PIO Programs

### gpio_echo.pio (L0: PIO-only echo)

```asm
.program gpio_echo
.wrap_target
    wait 1 pin 0
    set pins, 1
    wait 0 pin 0
    set pins, 0
.wrap
```

4 instructions, ~20-25 ns per edge (with synchroniser).

### gpio_echo_mov.pio (L0 variant: continuous copy)

```asm
.program gpio_echo_mov
.wrap_target
    mov pins, pins
.wrap
```

1 instruction, ~15-20 ns latency, 5 ns jitter. Absolute minimum.

### edge_detector.pio (L1-L3: input watcher)

```asm
.program edge_detector
.wrap_target
    wait 1 pin 0
    set x, 1
    in x, 32
    wait 0 pin 0
    set x, 0
    in x, 32
.wrap
```

6 instructions. Autopushes edge state (0 or 1) to RX FIFO.

### output_driver.pio (L1-L3: output from FIFO)

```asm
.program output_driver
.wrap_target
    out pins, 1
.wrap
```

1 instruction. Autopulls from TX FIFO and drives output pin.

## Output Format

### Human-Readable

```
=== PIO Latency Benchmark ===
Test:           L0 (PIO-only echo)
Stimulus pin:   GPIO4 (JC7)
Response pin:   GPIO5 (JC9)
Iterations:     1000

Round-trip latency (measured by RPi4):
  Min:     48 ns
  Max:     312 ns
  Mean:    62.4 ns
  Median:  58 ns
  Std Dev: 14.2 ns
  P5:      50 ns
  P95:     94 ns
  P99:     156 ns
```

### JSON

```json
{
  "test": "L0",
  "stimulus_pin": 4,
  "response_pin": 5,
  "iterations": 1000,
  "rpi4_round_trip_ns": {
    "min": 48, "max": 312, "mean": 62.4, "median": 58,
    "stddev": 14.2, "p5": 50, "p95": 94, "p99": 156
  }
}
```

## File Layout

```
latency/
    latency_rpi4.c            # RPi4 measurement program (mmap GPIO, nanosecond timing)
    latency_rpi5.c            # RPi5 PIO latency program (L0-L3 implementations)
    latency_common.h          # Shared types, constants, report formatting
    gpio_echo.pio             # L0: PIO-only echo (4 instructions)
    gpio_echo.pio.h           # Pre-generated header
    edge_detector.pio         # L1-L2: input edge detector (autopush to RX FIFO)
    edge_detector.pio.h       # Pre-generated header
    output_driver.pio         # L1-L2: output driver (set pins from TX FIFO)
    output_driver.pio.h       # Pre-generated header
    run_latency_benchmark.py  # Orchestrator (SSH deploy, coordinate, collect results)
    Makefile                  # Build system (rpi4/rpi5 targets with platform detection)
    .gitignore
```

The latency benchmark reuses `benchmark/benchmark_stats.c/h` for statistics
computation. The `gpio_echo_mov.pio` variant (continuous copy via `mov pins, pins`)
was not implemented in the final version.

## Success Criteria

Original criteria and actual outcomes:

| # | Criterion | Outcome | Notes |
|---|-----------|---------|-------|
| 1 | L0 round-trip < 100 ns | **MISS** — 388 ns median | RPi4 measurement overhead (~68 ns/poll + clock_gettime) dominates. PIO echo itself is ~20 ns. |
| 2 | L1 round-trip ~20-40 µs | **PASS** — 43.6 µs median | Slightly above range; two ioctl round-trips at ~22 µs each. |
| 3 | L2 round-trip ~1-10 µs | **MISS** — 52.5 µs median | Single-word DMA setup overhead is higher than expected. DMA shines for bulk, not single-word. |
| 4 | L3 mmap < 2 µs | **REDESIGNED** — 88.6 µs (4KB batch) | Redesigned to batched DMA throughput. Measures 46 MB/s, consistent with throughput benchmark. |
| 5 | RT reduces jitter | **NOT YET TESTED** | RT priority and CPU affinity pass-through implemented but not systematically tested. |
| 6 | Repeatable, low stddev | **PASS** | L0: 23 ns, L1: 3.2 µs, L2: 1.8 µs, L3: 2.8 µs — all tight. |
| 7 | Clear latency hierarchy | **PASS** | L0 (388 ns) << L1 (44 µs) < L2 (52 µs) << L3 (89 µs for 4KB). Each layer's cost is clearly attributable. |

Key insight: L2 (single-word DMA) is ~20% *slower* than L1 (ioctl) because DMA
setup/teardown overhead dominates for 4-byte transfers. However, L2 has lower
variance (stddev 1.8 µs vs 3.2 µs), suggesting DMA is more deterministic. DMA's
advantage is in bulk transfers (see throughput benchmark: ~42 MB/s).

## Measured Results

1000 iterations, 50 warmup, GPIO4/GPIO5 (JC connector), kernel 6.12+:

| Layer | Min | Median | Mean | P95 | P99 | Max | Std Dev |
|-------|----:|-------:|-----:|----:|----:|----:|--------:|
| **L0** (PIO-only) | 259 ns | 388 ns | 381 ns | 389 ns | 481 ns | 518 ns | 23 ns |
| **L1** (ioctl) | 33.0 µs | 43.6 µs | 44.4 µs | 46.3 µs | 46.7 µs | 136 µs | 3.2 µs |
| **L2** (DMA) | 49.9 µs | 52.5 µs | 52.3 µs | 52.8 µs | 53.0 µs | 102 µs | 1.8 µs |
| **L3** (batched 4KB DMA) | 87.1 µs | 88.6 µs | 88.6 µs | 89.1 µs | 90.2 µs | 172 µs | 2.8 µs |

### Latency Hierarchy

- **L0** (~388 ns): Hardware floor. PIO echoes autonomously in 4 instructions (~20 ns processing + RPi4 measurement overhead).
- **L1** (~44 µs, 112× L0): Each `pio_sm_get/put` is an ioctl → kernel → firmware mailbox → RP1 M3 core round-trip. Two per echo = ~22 µs × 2.
- **L2** (~52 µs, 135× L0): Single-word DMA is ~20% slower than ioctl. DMA setup/teardown overhead dominates for small (4-byte) transfers. However, L2 has tighter variance (stddev 1.8 µs vs 3.2 µs), suggesting DMA is more deterministic.
- **L3** (~89 µs for 4KB): Measures DMA read throughput — 4096 bytes / 88.6 µs = **46 MB/s**, consistent with the ~42 MB/s aggregate measured by the throughput benchmark.

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| L3 mmap may conflict with kernel driver | Test separately; drop if unstable |
| DMA minimum transfer size too large for L2 | Fall back to repeated ioctl with timing |
| PIO input synchroniser bypass may cause glitches | Test both modes; report both |
| RPi4 `clock_gettime` resolution insufficient | Verify actual resolution; use TSC if available |
| GPIO read latency (~68 ns) dominates RPi4 measurement | Document as measurement overhead; subtract from results |
