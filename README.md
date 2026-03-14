> **Note:** This repository was generated with the assistance of AI tools
> (Claude). Content should be independently verified before relying on it
> for hardware decisions or production use.

# RP1 PIO Performance Exploration on Raspberry Pi 5

The RP1 is the southbridge chip on the Raspberry Pi 5, connected to the BCM2712 ARM host via PCIe 2.0 x4. Among other peripherals, it contains a PIO block compatible with the RP2040 instruction set — 4 state machines, 200 MHz clock, and 8-deep FIFOs.

This repository investigates how much DMA throughput is achievable between the ARM host and the RP1's PIO block, and documents the kernel patches that affect it.

## Benchmark

Measures round-trip DMA throughput between the Raspberry Pi 5's ARM CPU and the RP1 southbridge's PIO block. Data flows from host memory through DMA to the PIO TX FIFO, gets transformed by a 3-instruction PIO program (bitwise NOT), and returns through the RX FIFO back to host memory.

### Prerequisites

- Raspberry Pi 5 (RPi4 is not supported — RP1 is specific to RPi5)
- Kernel 6.12+ with both DMA patches applied:
  - PR #6994 (2025-08): Heavy DMA channel reservation (~27 MB/s baseline)
  - PR #7190 (2026-01): FIFO threshold fix (corrects data corruption)
- Root access (`/dev/pio0` requires `sudo`)

### Running the Benchmark

**On RPi5:**

```bash
sudo apt install libpio-dev
cd benchmark
make benchmark
sudo ./pio_loopback
```

**Additional build targets:**

```bash
make check-deps     # Verify dependencies are installed
make install-deps   # Install libpio-dev (requires sudo)
```

**Tests only (any platform):**

```bash
cd benchmark
make run-test       # Build and run portable test binary
```

The test binary covers statistics, verification, and formatting — no hardware required.

### Architecture

The benchmark exercises the complete DMA data path:

```
ARM DRAM ──→ BCM2712 PCIe Root Complex ──→ PCIe 2.0 x4 Link (2 GB/s)
    ──→ RP1 PCIe Endpoint ──→ RP1 AXI Fabric ──→ RP1 DMA Controller
        ──→ PIO TX FIFO ──→ State Machine (bitwise NOT) ──→ PIO RX FIFO
            ──→ RP1 DMA Controller ──→ PCIe ──→ ARM DRAM
```

The PIO program is minimal — 3 instructions in a tight loop:

```asm
.wrap_target
    out x, 32           ; autopull: TX FIFO → OSR → scratch X
    mov y, ~x           ; bitwise NOT of X → Y
    in y, 32            ; autopush: Y → ISR → RX FIFO
.wrap
```

Since `pio_sm_xfer_data()` is a blocking call, TX and RX DMA transfers run concurrently in separate pthreads to avoid deadlock (the PIO needs TX data to produce RX data, so the TX FIFO would fill and block without concurrent RX draining).

### Theoretical Performance Calculations

#### PIO Internal Throughput

- Clock: 200 MHz (divider 1.0)
- Instructions per 32-bit word: 3 (`out` + `mov` + `in`)
- Internal throughput: 200 MHz / 3 = **66.7 Mwords/s = 266.7 MB/s**
- PIO is never the bottleneck — DMA is

#### DMA Throughput Ceiling

All throughput values below are in MB/s (megabytes/s). The RP1 datasheet states bandwidth in Mbps (megabits/s); datasheet figures have been divided by 8.

The RP1 contains an 8-channel Synopsys DesignWare AXI DMA controller:

- Internal AXI bus: 128-bit wide, 100 MHz = 1.6 GB/s raw
- PIO FIFO interface: 32-bit wide → 75% DMA bandwidth wasted on padding
- DMA handshake overhead: ~70 bus cycles per transfer
- Heavy channels 0/1 support 8-beat bursts (32 bytes per burst)
- RP1 datasheet per-channel read bandwidth: 500–600 Mbps (62–75 MB/s)

**Theoretical DMA ceiling:** ~62–75 MB/s per direction (500–600 Mbps from RP1 datasheet ÷ 8)

**Previous measured results (community):**

| Configuration | Throughput |
|--------------|-----------|
| Pre-optimization (default burst) | ~10.75 MB/s |
| After PR #6994 (heavy channels, burst=8) | ~27 MB/s |
| Direct M3 core access (unofficial) | ~66 MB/s |

### Measured Results

Sample output from `pio_loopback --iterations=20` on RPi5 (kernel 6.12.47+rpt-rpi-2712):

```
================================================================
RP1 PIO Internal Loopback Benchmark
================================================================

Configuration:
  Transfer size:     262144 bytes (256.0 KB)
  Iterations:        20
  PIO clock:         200 MHz (divider 1.0)
  PIO program:       3 instructions (out x,32 / mov y,~x / in y,32)
  Transfer mode:     DMA (threshold=8, priority=2)
  FIFO depth:        8 TX + 8 RX (unjoined)

Results:
  ----------------------------------------------------------------
  Throughput (MB/s):
    Aggregate:       42.20
    Min:             43.38
    Max:             43.69
    Mean:            43.54
    Median:          43.50
    Std Dev:         0.11
    P5:              43.38
    P95:             43.69
    P99:             43.69
  ----------------------------------------------------------------
  Data integrity:    PASS (0 errors in 5242880 bytes)
  Total transferred: 5.00 MB in 0.118 s
  ----------------------------------------------------------------

Theoretical analysis:
  PIO internal:      254.3 MB/s (200 MHz * 4 bytes / 3 cycles)
  Tput ceiling:      ~27.0 MB/s
  Achieved:          42.20 MB/s (156.3% of ceiling)

================================================================
Verdict: PASS (42.20 MB/s >= 10.00 MB/s threshold)
```

Results are consistent across multiple runs (41.7–42.2 MB/s aggregate, 43.3–43.8 MB/s per-iteration mean), with zero data errors in all cases.

### Performance Comparison

| Configuration | Throughput | Source |
|--------------|-----------|--------|
| Theoretical PIO internal | 266.7 MB/s | 200 MHz clock, 3 cycles/word, 4 bytes/word |
| Theoretical DMA ceiling | 62–75 MB/s | RP1 datasheet per-channel read bandwidth |
| Direct M3 core access (unofficial) | ~66 MB/s | cleverca22's experiments (bypasses kernel) |
| **This benchmark (loopback)** | **~42 MB/s** | **Concurrent TX+RX via pthreads** |
| PIOLib DMA after PR #6994 | ~27 MB/s | Heavy channels, burst=8, single-direction |
| PIOLib DMA pre-optimisation | ~10.75 MB/s | Default burst, any channel |
| `pio_sm_get_blocking()` (no DMA) | ~0.25 MB/s | PCIe + mailbox round-trip per word |

The ~42 MB/s aggregate throughput exceeds the previously documented ~27 MB/s for three reasons:
1. Concurrent TX+RX transfers via pthreads allow DMA operations to overlap, roughly doubling effective throughput over single-direction measurements
2. Updated kernel patches (PR #6994 and #7190) are present in kernel 6.12.47
3. Large transfer buffers (256 KB) amortise per-transfer DMA overhead

### Command-Line Options

```
--size=BYTES        Transfer size per iteration (default 262144 = 256 KB)
--iterations=N      Number of measured iterations (default 100)
--warmup=N          Warmup iterations before measurement (default 3)
--pattern=ID        Test pattern: 0=sequential, 1=ones, 2=alternating, 3=random
--threshold=MB/S    Pass/fail threshold in MB/s (default 10.0)
--json              Output machine-readable JSON instead of table
--no-verify         Skip data verification (pure throughput measurement)
--mode=MODE         Transfer mode: dma or blocking (default dma)
--dma-threshold=N   FIFO threshold 1-8, DMA mode only (default 8)
--dma-priority=N    DMA priority 0-31, DMA mode only (default 2)
--help              Show help
```

### Transfer Modes

The benchmark supports two transfer modes:

| Mode | Method | Throughput | Use case |
|------|--------|-----------|----------|
| `dma` (default) | `pio_sm_xfer_data()` via pthreads | ~42 MB/s | Production performance measurement |
| `blocking` | `pio_sm_put_blocking()`/`pio_sm_get_blocking()` | ~0.2 MB/s | Baseline comparison without DMA |

#### DMA parameters

In DMA mode, the `--dma-threshold` and `--dma-priority` options control the RP1 PIO `dmactrl` register:

| Bits | Field | CLI option | Description |
|------|-------|-----------|-------------|
| 31 | DREQ_EN | (always set) | Enable DMA request signal |
| 11:7 | PRIORITY | `--dma-priority` | DMA priority (lower = faster) |
| 4:0 | THRESHOLD | `--dma-threshold` | FIFO threshold (must match burst size) |

**Warning:** Setting `--dma-threshold` to a value other than the kernel's DMA burst size (8 for heavy channels) may cause data corruption. See [PR #7190](https://github.com/raspberrypi/linux/pull/7190) for details.

#### Example parameter sweeps

```bash
# Default DMA (baseline)
sudo ./pio_loopback --iterations=20

# Threshold sweep (varying FIFO threshold with burst=8)
sudo ./pio_loopback --iterations=20 --dma-threshold=1
sudo ./pio_loopback --iterations=20 --dma-threshold=4

# Priority sweep
sudo ./pio_loopback --iterations=20 --dma-priority=0
sudo ./pio_loopback --iterations=20 --dma-priority=16
sudo ./pio_loopback --iterations=20 --dma-priority=31

# Blocking mode (use small size — ~0.2 MB/s is slow)
sudo ./pio_loopback --mode=blocking --size=4096 --iterations=20
```

### Interpreting Results

**Good results:** Aggregate throughput ≥27 MB/s, 0 data errors, stddev <1 MB/s.

**Common issues:**

| Symptom | Likely cause |
|---------|-------------|
| <10 MB/s | Kernel missing PR #6994 (heavy DMA channels) |
| Data errors | Kernel missing PR #7190 (FIFO threshold fix) |
| DMA transfer failures | Another process using PIO, or kernel driver issue |
| Permission denied | Need `sudo` to access `/dev/pio0` |

### Benchmark Source Structure

```
benchmark/
  loopback.pio          PIO assembly source (3 instructions)
  loopback.pio.h        Pre-generated header (checked into git)
  benchmark_main.c      RPi5-only benchmark binary
  benchmark_stats.c/h   Portable statistics (min/max/mean/median/stddev/percentiles)
  benchmark_verify.c/h  Portable data verification (pattern gen, bitwise-NOT check)
  benchmark_format.c/h  Portable output formatting (table + JSON)
  benchmark_test.c      Portable test binary
  Makefile              Build system
```

## Latency Benchmark

Measures round-trip GPIO latency through the RPi5's RP1 PIO block at various abstraction layers, isolating the cost of each layer in the software stack.

### Architecture

An RPi4 generates stimulus pulses and measures the external round-trip time using memory-mapped GPIO (`/dev/gpiomem`, ~68 ns read resolution). An RPi5 echoes the signal back through PIO state machines, with progressively more software involvement at each layer:

```
RPi4 GPIO (stimulus) → wire → RPi5 PIO input → [processing layer] → RPi5 PIO output → wire → RPi4 GPIO (measurement)
```

A Python orchestrator (`latency/run_latency_benchmark.py`) coordinates both devices over SSH, deploying binaries, running tests, and collecting JSON results.

### Test Layers

| Layer | Description | Signal Path |
|-------|-------------|-------------|
| **L0** | PIO-only echo | GPIO → PIO SM → GPIO (no CPU) |
| **L1** | PIO → ioctl → PIO | GPIO → PIO RX FIFO → `pio_sm_get()` → CPU → `pio_sm_put()` → PIO TX FIFO → GPIO |
| **L2** | PIO → DMA → PIO | GPIO → PIO RX FIFO → `pio_sm_xfer_data()` → CPU → `pio_sm_xfer_data()` → PIO TX FIFO → GPIO |
| **L3** | Batched DMA throughput | Internal PIO data generator → DMA → host memory (standalone, no GPIO) |

### Measured Results

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

### Running the Latency Benchmark

**From a development machine with SSH access to both RPi5 and RPi4:**

```bash
# Run default tests (L0 + L1)
uv run python latency/run_latency_benchmark.py

# Run all four layers
uv run python latency/run_latency_benchmark.py --tests L0 L1 L2 L3

# Custom parameters
uv run python latency/run_latency_benchmark.py --tests L1 L2 --iterations 5000 --warmup 100

# JSON output (progress goes to stderr)
uv run python latency/run_latency_benchmark.py --tests L0 L1 L2 L3 --json

# With RT optimisations
uv run python latency/run_latency_benchmark.py --tests L1 --rt-priority=80 --cpu=3
```

**Prerequisites:**
- RPi5 with `libpio-dev` installed and root access (`/dev/pio0`)
- RPi4 with `/dev/gpiomem` access (GPIO group membership, no root needed)
- GPIO4 and GPIO5 connected between devices (JC connector, bottom row)
- SSH access from development machine to both devices

### Latency Source Structure

```
latency/
  latency_rpi4.c          RPi4 measurement program (mmap GPIO, nanosecond timing)
  latency_rpi5.c          RPi5 PIO latency program (L0-L3 implementations)
  latency_common.h        Shared types, constants, report formatting
  gpio_echo.pio           L0: PIO-only echo (4 instructions)
  edge_detector.pio       L1-L2: input edge detector (autopush to RX FIFO)
  output_driver.pio       L1-L2: output driver (set pins from TX FIFO)
  *.pio.h                 Pre-generated PIO headers
  run_latency_benchmark.py  Orchestrator (SSH deploy, coordinate, collect results)
  Makefile                Build system (rpi4/rpi5 targets with platform detection)
```

## Hardware

Two Raspberry Pi devices (RPi5 + RPi4) with Digilent Pmod HAT Adapters, connected via jumper cables across Pmod ports JA, JB, and JC (21 GPIO connections total). The RPi4 serves as both a loopback target for the GPIO verification script and the external stimulus/measurement device for the latency benchmark.

See [`hw.md`](hw.md) for the full pin mapping.

## Repository Structure

| Path | Description |
|------|-------------|
| [`hw.md`](hw.md) | Hardware setup — RPi5/RPi4 specs, Digilent Pmod HAT pin mapping, inter-device jumper connections |
| [`rp1-dma.md`](rp1-dma.md) | RP1 DMA architecture, the 10 MB/s throughput wall, and the kernel patches that address it |
| [`rp1-dma-2.md`](rp1-dma-2.md) | RP1 PIO register map, DMA data path internals, performance measurements, and worked code examples |
| [`resources.md`](resources.md) | Curated collection of datasheets, source repos, PRs, and community projects |
| [`benchmark/`](benchmark/) | PIO internal loopback throughput benchmark (DMA round-trip, ~42 MB/s) |
| [`gpio-loopback/`](gpio-loopback/) | GPIO loopback throughput benchmark (1-bit serial over GPIO, ~1.5 MB/s measured) |
| [`latency/`](latency/) | PIO latency benchmark (L0–L3 layers, RPi4 stimulus + RPi5 echo) |
| [`verify_pmod_connections.py`](verify_pmod_connections.py) | Tests GPIO jumper connections between RPi5 and RPi4 via Pmod HAT |

## License

This project is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
