# M3 Core 1 PIO Bridge -- Results

## Test Configuration

- Date: 2026-03-17 (fresh boot)
- Hardware: Raspberry Pi 5
- Kernel: 6.12+
- PIO program: loopback (pull, NOT, push) on SM3

## Bridge Throughput

| Metric               | Value         |
|----------------------|---------------|
| Bidirectional throughput | 6.89 MB/s  |
| Cycles per word (full loop) | ~116    |
| Cycles per PIO access | ~54 (~270 ns) |

The full loop includes: SRAM load, TXF3 write, DSB, delay, RXF3 read, SRAM
store. At ~116 cycles per word and ~200 MHz clock, this yields ~1.72M words/sec
= 6.89 MB/s.

## PIO Address Comparison

Measured by `pio_addr_test.s`, reading FSTAT in a tight loop:

| Base Address  | Read Rate       | FSTAT Value   | Relative Speed |
|---------------|-----------------|---------------|----------------|
| 0xF0000000    | 6.7M reads/sec  | 0x0F000F00 (correct) | 1.41x   |
| 0x40178000    | 4.76M reads/sec | 0x00000000 (wrong)   | 1.00x   |

The 0xF0000000 vendor-specific alias is faster and returns correct register
values. The 0x40178000 standard peripheral address returns incorrect FSTAT,
indicating a different bus route.

## M3 Clock Speed

Measured by `clock_test.s`, running a 3-instruction tight loop in SRAM:

| Metric          | Value      |
|-----------------|------------|
| Loop rate       | 8.7 Mloops/sec |
| Estimated clock | ~200 MHz   |

The 3-instruction loop (subtract, compare, branch) at 8.7 Mloops/sec implies
~23 cycles/iteration. Single-cycle execution would give ~67 Mloops/sec,
confirming that even SRAM access incurs bus overhead (~7 cycles per access via
the AXI bus).

## Core 0 Interference

| Metric             | Value                |
|--------------------|----------------------|
| Error frequency    | ~1 per 10 passes     |
| Error location     | Buffer index 62      |
| Cause              | Core 0 firmware bus contention |

Core 0 periodically accesses shared bus resources, causing single-word data
corruption at a consistent buffer offset.

## Comparison with DMA Approaches

| Approach                        | TX MB/s | RX MB/s | Relative to M3 |
|---------------------------------|---------|---------|-----------------|
| M3 Core 1 CPU polling           | 6.89    | 6.89    | 1.0x            |
| Cyclic DMA, DRAM rings          | 40.35   | 35.87   | 5.2-5.9x        |
| Cyclic DMA, SRAM rings          | 54.13   | 45.10   | 6.5-7.9x        |
| RX-only DMA, DRAM               | --      | 55.97   | 8.1x            |

Cyclic DMA with SRAM ring buffers achieves 6-8x higher throughput than M3 Core 1
CPU polling. The M3 approach is limited by the ~54-cycle APB bridge latency per
PIO register access, while DMA benefits from burst transfers and a 128-bit bus
(despite its own 70-cycle handshake overhead per burst).
