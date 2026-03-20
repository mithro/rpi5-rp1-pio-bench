# PIO Loopback Throughput -- Design

## PIO program

The PIO state machine runs a 3-instruction program (`loopback.pio`) that
performs an internal loopback with a bitwise NOT transform:

```
.wrap_target
    out x, 32       ; autopull: TX FIFO -> OSR -> scratch X
    mov y, ~x       ; bitwise NOT of X -> Y
    in y, 32        ; autopush: Y -> ISR -> RX FIFO
.wrap
```

No GPIO pins are used. At 200 MHz with 3 instructions per word, the PIO
internal throughput is 200 MHz / 3 = 66.7 Mwords/s = 267 MB/s. This far
exceeds the DMA path capacity, so PIO is never the bottleneck.

## Data path

Each 32-bit word traverses the following path:

    ARM DRAM --> PCIe --> RP1 DMA --> TX FIFO --> PIO SM (NOT) --> RX FIFO --> RP1 DMA --> PCIe --> ARM DRAM

The data crosses the PCIe bus twice (once in each direction). The RP1's
internal DMA engine moves words between its PCIe endpoint and the PIO
FIFOs using DREQ-based flow control.

## Threading model

The piolib `pio_sm_xfer_data()` call is blocking -- it submits a DMA
transfer via ioctl and waits for completion. Because the PIO program
reads from the TX FIFO and writes to the RX FIFO, both directions must
be active simultaneously. If only TX runs, the TX FIFO fills and the
PIO stalls; if only RX runs, the RX FIFO is empty and the DMA stalls.

To avoid this deadlock, the benchmark spawns two pthreads per iteration:
one for the TX transfer (`PIO_DIR_TO_SM`) and one for the RX transfer
(`PIO_DIR_FROM_SM`). Both are `pthread_join`'d before the iteration
timing stops.

## DMA configuration

The benchmark configures the RP1 DMA via the `dmactrl` register, set
through `pio_sm_set_dmactrl()`:

| Field     | Bits  | Default | Description                              |
|-----------|-------|---------|------------------------------------------|
| DREQ_EN   | 31    | 1       | Enable DMA request signal                |
| PRIORITY  | 11:7  | 2       | DMA priority (lower value = higher prio) |
| THRESHOLD | 4:0   | 8       | FIFO threshold for DREQ assertion        |

The threshold must match the DMA burst size. Mismatched values cause
data corruption (see kernel PR #7190). Both TX and RX channels use the
same dmactrl settings.

Transfer size and repeat count are configured via `pio_sm_config_xfer()`.

## Data verification

After each iteration, the benchmark verifies data integrity by checking
that `rx_buf[i] == ~tx_buf[i]` for every 32-bit word. Four test patterns
are available:

| ID | Name        | Pattern                                    |
|----|-------------|--------------------------------------------|
| 0  | sequential  | 0, 1, 2, 3, ...                           |
| 1  | ones        | 0xFFFFFFFF repeated                        |
| 2  | alternating | 0xAAAAAAAA, 0x55555555, ...                |
| 3  | random      | Deterministic xorshift32 PRNG              |

Verification can be disabled with `--no-verify` to measure raw DMA
throughput without the comparison overhead.

## Source files

| File                  | Description                                      |
|-----------------------|--------------------------------------------------|
| `benchmark_main.c`    | Entry point, PIO setup, DMA transfer loop        |
| `benchmark_verify.c`  | Test pattern generation and NOT verification     |
| `benchmark_verify.h`  | Public API for pattern fill and verify functions  |
| `benchmark_stats.c`   | Statistics: mean, median, stddev, percentiles     |
| `benchmark_stats.h`   | Report struct and stats API                       |
| `benchmark_format.c`  | Human-readable and JSON output formatting         |
| `benchmark_format.h`  | Formatter API                                     |
| `loopback.pio`        | PIO assembly source (3-instruction NOT loopback)  |
| `loopback.pio.h`      | Generated C header from pioasm                    |
| `Makefile`            | Build system (benchmark + portable test targets)  |
