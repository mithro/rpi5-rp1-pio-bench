# Cyclic DMA Throughput -- Results

## Test Configuration

- Date: 2026-03-17 (fresh boot)
- Hardware: Raspberry Pi 5
- Kernel: 6.12+ with PR #6994 (Heavy DMA channel reservation) and PR #7190
  (threshold/burst alignment fix)
- PIO program: loopback (pull, NOT, push) on SM0 unless noted
- Duration: 3 seconds per test

## Throughput Comparison

| Approach | TX MB/s | RX MB/s | Reliability | Bottleneck |
|----------|---------|---------|-------------|------------|
| RX-only DMA, DRAM | -- | 55.97 | PASS | DMA handshake (burst=8) |
| Cyclic DMA, SRAM bidirectional | 54.13 | 45.10 | PASS (repeatable) | APB DREQ handshake |
| TX-only DMA, DRAM | 40.93 | -- | PASS | PCIe posted writes |
| Cyclic DMA, DRAM bidirectional | 40.35 | 35.87 | PASS | PCIe read completions |
| Standard kernel DMA (baseline) | ~42 | ~42 | -- | PCIe + APB handshake |
| piolib ioctl DMA | 17.51 | 17.51 | PASS | ioctl overhead per xfer |
| M3 Core 1 CPU-polled bridge | 6.89 | 6.89 | ~91% (index 62) | APB bridge latency |
| cleverca22 custom driver (ref) | -- | ~66 | dropping samples | Direct register DMA |

## DMA Configuration

| Setting | Value | Notes |
|---------|-------|-------|
| TX/RX `maxburst` | 8 | Heavy channels (0, 1) support MSIZE=8 |
| DMACTRL threshold | 8 | Must match burst size (PR #7190) |
| DMA period size | 4096 B | Balances interrupt overhead vs latency |

## Burst Tuning

| Burst Size | TX MB/s (approx) | Notes |
|------------|-------------------|-------|
| 4 | ~33 | Confirms handshake overhead is bottleneck |
| 8 | ~54 | 70% improvement over burst=4 |

Buffer size has no measurable effect on throughput. pelwell's estimate of ~70
bus cycles per handshake is confirmed.

## Reliability

- DRAM mode: 100/100 passes with data verification (reliability sweep).
- SRAM mode: 3/3 back-to-back passes verified.
- SRAM ring placement: `0xA200+`, past the firmware dynamic region
  (`0x9F48-0xA150`). DMA rings placed in the dynamic region cause firmware
  hangs during PIO SM lifecycle operations.

## TX/RX Asymmetry

TX uses posted writes (fire-and-forget). RX requires read completions (wait for
data return). Removing TX contention boosts RX from 35.87 to 55.97 MB/s (56%
improvement), confirming the asymmetry is inherent to PCIe/AXI bus protocol.

## Hardware Limitations Discovered

| Finding | Impact |
|---------|--------|
| PIO FIFO access from M3 is ~54 cycles, not 1 | Core 1 bridge limited to ~7 MB/s |
| FSTAT at 0xF0000000 does not dynamically update | Cannot poll RXEMPTY from Core 1 |
| 0xF0000000 is 1.41x faster than 0x40178000 | Use 0xF0 alias for all M3 PIO access |
| Core 0 firmware interference at word 62 | ~1 error per 10 passes on Core 1 path |
| SRAM 0x9F48-0xA150 is firmware dynamic state | DMA rings must be placed at 0xA200+ |
| DREQ must be disabled before DMA terminate | Prevents dw-axi-dma descriptor pool corruption |
