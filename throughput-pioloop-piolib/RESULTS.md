# PIO Loopback Throughput -- Results

## Measured throughput

With default settings (256 KB per iteration, 100 iterations, DMA
threshold=8, priority=2):

| Metric              | Value         |
|---------------------|---------------|
| Aggregate           | ~42 MB/s      |
| Per-iteration mean  | 43.3-43.8 MB/s|
| Data errors         | 0             |

The aggregate throughput (total bytes / total wall time) is slightly
lower than the per-iteration mean because it includes inter-iteration
overhead (pattern fill, buffer clear, thread creation/join).

## Test Configuration

- Hardware: Raspberry Pi 5
- Kernel: 6.12+ with PR #6994 (Heavy DMA channel reservation) and PR #7190
  (threshold/burst alignment fix)
- PIO clock: 200 MHz (divider 1.0)
- FIFO: 8 TX + 8 RX (unjoined)
- Transfer mode: DMA via piolib ioctl (`pio_sm_xfer_data`)

## Theoretical comparison

| Level                    | Throughput | Notes                                      |
|--------------------------|------------|--------------------------------------------|
| PIO internal             | 267 MB/s   | 200 MHz / 3 cycles * 4 bytes               |
| DMA ceiling (estimated)  | 62-75 MB/s | Single-direction RP1 DMA limit             |
| Bidirectional DMA ceiling| ~27 MB/s   | Both TX and RX sharing PCIe bandwidth       |
| Measured (this benchmark)| ~42 MB/s   | Exceeds naive bidirectional estimate        |

The measured ~42 MB/s exceeds the 27 MB/s estimate used in the code's
theoretical analysis section, suggesting the PCIe link handles
concurrent TX and RX transfers more efficiently than a simple
bandwidth-halving model predicts.

## Blocking mode comparison

The benchmark also supports a blocking (non-DMA) mode using
`pio_sm_put_blocking` / `pio_sm_get_blocking`. This mode uses
ioctl-based mailbox round-trips for every word. Expected throughput
in blocking mode is approximately 0.2-0.4 MB/s.
