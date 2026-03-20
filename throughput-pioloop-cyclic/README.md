# Cyclic DMA Throughput Benchmark

## Overview

This benchmark measures RP1 PIO DMA throughput using cyclic DMA with SRAM and
DRAM ring buffers, plus unidirectional (TX-only, RX-only) modes. It also
includes a piolib ioctl baseline for comparison.

## Architecture

A custom kernel module (`kmod/rp1_pio_sram.ko`) sets up cyclic DMA between ring
buffers and PIO FIFOs using the RP1's dw-axi-dmac controller. The userspace tool
(`throughput_pioloop_cyclic`) configures the PIO state machine via piolib, then controls
DMA through the module's ioctl interface on `/dev/rp1_pio_sram`.

Two buffer modes are supported:

- **DRAM mode**: Ring buffers in host DRAM (`dma_alloc_coherent`), accessed by
  DMA via PCIe.
- **SRAM mode**: Ring buffers in RP1 shared SRAM (64 KB at BAR2), accessed by
  DMA via RP1-internal AXI bus at `0xc020000000` -- no PCIe per DMA burst.

The PIO program performs a bitwise NOT loopback (pull, NOT, push) on SM0, so
data integrity can be verified by checking `TX_word ^ RX_word == 0xFFFFFFFF`.

## Key Result

All measurements from fresh boot on 2026-03-17, 3-second duration, kernel 6.12+.

| Mode | TX MB/s | RX MB/s |
|------|---------|---------|
| SRAM bidirectional | 54.13 | 45.10 |
| RX-only (DRAM) | -- | 55.97 |
| TX-only (DRAM) | 40.93 | -- |
| DRAM bidirectional | 40.35 | 35.87 |
| piolib ioctl baseline | 17.51 | 17.51 |

SRAM bidirectional TX throughput exceeds the standard kernel DMA baseline (~42
MB/s) by 29%. Unidirectional RX-only reaches 56 MB/s, 85% of cleverca22's
custom driver result (~66 MB/s).

## See Also

- [Design](DESIGN.md) -- SRAM memory map, firmware analysis, DMA
  configuration, data path diagrams, M3 Core 1 architecture
- [Results](RESULTS.md) -- Full measurement tables, reliability data, burst
  tuning, hardware limitations
- [Usage](USAGE.md) -- Build instructions, run modes, tool descriptions
