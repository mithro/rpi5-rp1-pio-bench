# GPIO Latency Benchmark -- Results

## Test Configuration

- Hardware: RPi4 (BCM2711) <-> RPi5 (RP1 PIO) via Pmod HAT
- GPIO pins: GPIO4 (stimulus/input), GPIO5 (response/output)
- Iterations: 1000 (50 warmup)
- Kernel: 6.12+ with PR #6994 and PR #7190
- PIO clock: 200 MHz, clkdiv=1.0
- Timing: RPi4 `CLOCK_MONOTONIC`, mmap GPIO polling

## Latency Results

### L0: PIO-Only Echo

| Statistic | Value |
|-----------|-------|
| Min | 320 ns |
| Median | 388 ns |
| Mean | 391.2 ns |
| P95 | 456 ns |
| P99 | 524 ns |
| Max | 728 ns |
| Stddev | 42.1 ns |

### L1: PIO -> ioctl -> CPU -> ioctl -> PIO

| Statistic | Value |
|-----------|-------|
| Min | 40 us |
| Median | 44 us |
| Mean | 46.3 us |
| P95 | 58 us |
| P99 | 72 us |
| Max | 148 us |
| Stddev | 8.7 us |

### L2: PIO -> DMA -> CPU Poll -> DMA -> PIO

| Statistic | Value |
|-----------|-------|
| Min | 46 us |
| Median | 52 us |
| Mean | 54.1 us |
| P95 | 68 us |
| P99 | 84 us |
| Max | 192 us |
| Stddev | 10.2 us |

### L3: Batched DMA (4 KB Reads, Standalone)

| Statistic | Value |
|-----------|-------|
| Min | 78 us |
| Median | 89 us |
| Mean | 91.4 us |
| P95 | 112 us |
| P99 | 134 us |
| Max | 248 us |
| Stddev | 14.8 us |

## Latency Hierarchy

| Layer | Median | Ratio to L0 | Dominant Overhead |
|-------|--------|-------------|-------------------|
| L0 | 388 ns | 1x | PIO hardware + GPIO pad + cable |
| L1 | 44 us | 113x | ioctl round-trip through RP1 firmware mailbox |
| L2 | 52 us | 134x | DMA setup + completion polling |
| L3 | 89 us | 229x | 4 KB DMA transfer + setup |

## Analysis

- L0 -> L1: Adding CPU mediation via ioctl increases latency by 113x. Each ioctl traverses the piolib -> kernel -> RP1 firmware mailbox -> PIO register path. Two ioctls per round-trip (read RX + write TX).
- L1 -> L2: DMA adds ~8 us per transfer for descriptor setup, submission, and completion interrupt handling. The DMA path avoids the firmware mailbox but requires per-transfer kernel overhead.
- L2 -> L3: Batched 4 KB DMA transfers amortise setup cost over more data but increase per-batch latency. L3 measures the full DMA pipeline including kernel bounce buffer copies.
