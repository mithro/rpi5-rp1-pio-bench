# GPIO Loopback Throughput -- Results

## Test Configuration

- Hardware: RPi5 with RP1 PIO, GPIO5 output looped back to GPIO5 input (single-pin loopback)
- PIO clock: 200 MHz, clkdiv=1.0
- Source data: 8 KB per iteration, sequential pattern
- Iterations: 100 (3 warmup)
- Kernel: 6.12+ with PR #6994 (heavy DMA channels) and PR #7190 (FIFO threshold fix)
- DMACTRL: TX threshold=1 (`0x80000101`), RX threshold=4 (`0x80000104`)

## Measured Throughput

| Metric | Value |
|--------|-------|
| Source data throughput | ~1.5 MB/s per iteration |
| Theoretical source ceiling | 3.125 MB/s |
| Efficiency | ~49% of ceiling |
| DMA word rate | 25 MW/s |
| Total DMA traffic | ~100 MB/s (both directions) |

The ~49% efficiency is consistent with DMA bus contention: both TX and RX compete for DMA bandwidth at 25 MW/s each.

## Comparison with Internal Loopback

| Property | Internal Loopback | GPIO Loopback |
|----------|-------------------|---------------|
| PIO program | `out x,32` / `mov y,~x` / `in y,32` | `out x,1` / `jmp` / `set` / 3x nop / `in pins,1` / `jmp` |
| Cycles per source word | 3 | 256 (8 per bit x 32 bits) |
| DMA word rate | 66.7 MW/s (PIO words) | 25 MW/s (DMA words) |
| DMA expansion | 1x | 32x |
| TX DMACTRL threshold | 8 | 1 |
| RX DMACTRL threshold | 8 | 4 |
| Measured throughput | ~42 MB/s aggregate | ~1.5 MB/s source (~100 MB/s DMA) |
| Bottleneck | DMA (PIO 15x faster) | PIO-limited (DMA moves 32x more than source) |
| Data verification | bitwise NOT | identity (TX = RX) |

## Data Verification

- 2100/2100 diagnostic tests passed (7 patterns x 6 sizes x 50 iterations)
- 200/200 benchmark iterations passed (4 patterns x 50 iterations)
- 0 data errors across all configurations

## DMACTRL Threshold Sweep

| TX Threshold | RX Threshold | Result |
|:---:|:---:|---|
| 1 | 4 | 140/140 PERFECT |
| 2 | 4 | 140/140 PERFECT |
| 4 | 4 | DATA_ERR on all patterns |
| 8 | 4 | DMA_FAIL (ETIMEDOUT) |
| 1 | 1 | Massive corruption |
| 1 | 2 | Massive corruption |
