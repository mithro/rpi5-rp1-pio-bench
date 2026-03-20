# M3 Core 1 PIO FIFO Throughput Benchmark

## Overview

This benchmark measures PIO FIFO throughput using direct CPU polling from the
RP1's M3 Core 1 processor. Unlike DMA-based approaches that transfer data via
the host (BCM2712), this method runs firmware directly on the RP1's secondary
Cortex-M3 core to read and write PIO FIFOs.

## Design

1. M3 Core 1 is bootstrapped via an SEV issued through an IRQ vector table hook
   on Core 0 (direct SEV from the host is not possible).
2. The host loads a PIO loopback program (pull -> NOT -> push) onto SM3 via
   piolib.
3. Core 1 firmware writes words from a SRAM TX buffer to TXF3 at 0xF0000000,
   then reads the processed result from RXF3 into a SRAM RX buffer.
4. The host monitors throughput by polling SRAM status words via BAR2.

## Key Result

**6.89 MB/s** bidirectional throughput (verified 2026-03-17).

This is limited by ~54 cycle (~270 ns) APB bridge latency per PIO register
access from M3. Cyclic DMA approaches achieve 6-8x higher throughput (40-55
MB/s) but with higher per-transfer latency. M3 Core 1 is better suited to
low-latency control tasks than bulk data transfer.

## See Also

- [Design](DESIGN.md) -- Architecture and implementation details
- [Results](RESULTS.md) -- Measured performance numbers
- [Usage](USAGE.md) -- Build and run instructions
- [PIO Access Latency Research](PIO_ACCESS_LATENCY_RESEARCH.md) -- Bus
  architecture research explaining the 54-cycle PIO access latency
