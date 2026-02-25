> **Note:** This repository was generated with the assistance of AI tools
> (Claude). Content should be independently verified before relying on it
> for hardware decisions or production use.

# RP1 PIO Performance Exploration on Raspberry Pi 5

Investigating DMA throughput between the BCM2712 ARM host and the RP1 southbridge's PIO block on the Raspberry Pi 5. The RP1 connects via PCIe 2.0 x4 and contains a PIO block similar to the RP2040 — same instruction set, 4 state machines, but with 8-deep FIFOs and a 200 MHz clock.

## Repository Structure

| Path | Description |
|------|-------------|
| [`hw.md`](hw.md) | Hardware setup — RPi5/RPi4 specs, Digilent Pmod HAT pin mapping, inter-device jumper connections |
| [`rp1-dma.md`](rp1-dma.md) | Deep dive into RP1 DMA architecture, the 10 MB/s wall, and kernel optimisations |
| [`rp1-dma-2.md`](rp1-dma-2.md) | RP1 PIO register map, DMA data path details, performance measurements, and code examples |
| [`resources.md`](resources.md) | Curated collection of datasheets, source repos, PRs, and community projects |
| [`benchmark/`](benchmark/) | PIO internal loopback benchmark source code and build system |
| [`verify_pmod_connections.py`](verify_pmod_connections.py) | Tests GPIO jumper connections between RPi5 and RPi4 via Pmod HAT |

## Benchmark

Measures round-trip DMA throughput between the Raspberry Pi 5's ARM CPU and the RP1 southbridge's PIO block. Data flows from host memory through DMA to the PIO TX FIFO, gets transformed by a 3-instruction PIO program (bitwise NOT), and returns through the RX FIFO back to host memory.

### Quick Start

**On RPi5:**

```bash
sudo apt install libpio-dev
cd benchmark
make benchmark
sudo ./pio_loopback
```

**Tests only (any platform):**

```bash
cd benchmark
make run-test
```

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

> **Units note:** The RP1 datasheet specifies bandwidth in **megabits per second
> (Mbps)**. All throughput numbers in this document use **megabytes per second
> (MB/s)** unless explicitly noted otherwise. 1 MB/s = 8 Mbps.

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

Actual output from `pio_loopback` on RPi5 hardware (kernel 6.12.47+rpt-rpi-2712):

```
================================================================
RP1 PIO Internal Loopback Benchmark
================================================================

Configuration:
  Transfer size:     262144 bytes (256.0 KB)
  Iterations:        20
  PIO clock:         200 MHz (divider 1.0)
  PIO program:       3 instructions (out x,32 / mov y,~x / in y,32)
  DMA channels:      heavy (threshold=8, burst=8)
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
  DMA ceiling:       ~27 MB/s (heavy channels, burst=8)
  Achieved:          42.20 MB/s (156.3% of DMA ceiling)

================================================================
Verdict: PASS (42.20 MB/s >= 10.00 MB/s threshold)
```

Results are consistent across multiple runs (41.7–42.2 MB/s aggregate, 43.3–43.8 MB/s per-iteration mean), with zero data errors in all cases.

### Performance Comparison

| Configuration | Throughput | Source |
|--------------|-----------|--------|
| Theoretical PIO internal | 266.7 MB/s | 200 MHz / 3 cycles * 4 bytes |
| Theoretical DMA ceiling | 62–75 MB/s | RP1 datasheet per-channel read bandwidth |
| Direct M3 core access (unofficial) | ~66 MB/s | cleverca22's experiments (bypasses kernel) |
| **This benchmark (loopback)** | **~42 MB/s** | **Concurrent TX+RX via pthreads** |
| PIOLib DMA after PR #6994 | ~27 MB/s | Heavy channels, burst=8, single-direction |
| PIOLib DMA pre-optimisation | ~10.75 MB/s | Default burst, any channel |
| `pio_sm_get_blocking()` (no DMA) | ~0.25 MB/s | PCIe + mailbox round-trip per word |

The ~42 MB/s throughput significantly exceeds the previously documented ~27 MB/s. This is likely due to:
1. Concurrent TX+RX transfers via pthreads overlapping DMA operations
2. Updated kernel DMA optimisations (PR #6994 + #7190)
3. Large transfer buffers (256 KB) amortising per-transfer overhead

### Command-Line Options

```
--size=BYTES       Transfer size per iteration (default 262144 = 256 KB)
--iterations=N     Number of measured iterations (default 100)
--warmup=N         Warmup iterations before measurement (default 3)
--pattern=ID       Test pattern: 0=sequential, 1=ones, 2=alternating, 3=random
--threshold=MB/S   Pass/fail threshold in MB/s (default 10.0)
--json             Output machine-readable JSON instead of table
--no-verify        Skip data verification (pure throughput measurement)
--help             Show help
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

### Prerequisites

- Raspberry Pi 5 with RP1 southbridge
- Kernel 6.12+ with DMA optimizations:
  - PR #6994 (August 2025): Heavy DMA channel reservation
  - PR #7190 (January 2026): FIFO threshold fix
- `libpio-dev` package: `sudo apt install libpio-dev`
- Root access for `/dev/pio0`

### Building

**Benchmark (RPi5 only):**

```bash
cd benchmark
make benchmark      # Builds pio_loopback binary
make check-deps     # Verify dependencies are installed
make install-deps   # Install libpio-dev (requires sudo)
```

**Tests (any platform):**

```bash
cd benchmark
make test           # Build test binary
make run-test       # Build and run tests
```

The test binary tests all non-hardware logic (statistics, verification, formatting) and runs on x86 and arm without piolib.

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

## Hardware

Two Raspberry Pi devices (RPi5 + RPi4) with Digilent Pmod HAT Adapters, connected via jumper cables across all three Pmod ports (JA, JB, JC — 21 unique GPIO connections).

See [`hw.md`](hw.md) for complete pin mapping and connection details.

## License

This project is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
