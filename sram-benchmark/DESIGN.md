# SRAM + Cyclic DMA Benchmark — Design Document

## Overview

This benchmark suite measures RP1 PIO throughput using SRAM-local DMA and
M3 Core 1 direct FIFO access, bypassing the PCIe round-trip per DMA burst
that limits the standard kernel DMA path to ~42 MB/s.

## RP1 Memory Map

### Key Resources

| Resource | RP1 Internal Address | BCM2712 Physical Address | BAR | Access |
|----------|---------------------|--------------------------|-----|--------|
| Shared SRAM (64 KB) | `0x2000_0000` | `0x1F_0040_0000` | BAR2 | Host via PCIe, M3 single-cycle |
| PIO block | `0x4017_8000` | `0x1F_0017_8000` | BAR1 | Host via PCIe, M3 via `0xF000_0000` |
| DMA controller | `0x4018_8000` | `0x1F_0018_8000` | BAR1 | Host via PCIe |

### PIO FIFO Registers (offsets from PIO base `0x4017_8000`)

| Register | Offset | BCM2712 Physical | Purpose |
|----------|--------|-------------------|---------|
| FSTAT | `0x004` | `0x1F_0017_8004` | FIFO status (TX full, RX empty bits) |
| TXF0 | `0x010` | `0x1F_0017_8010` | TX FIFO for SM0 (write-only) |
| TXF1 | `0x014` | `0x1F_0017_8014` | TX FIFO for SM1 |
| TXF2 | `0x018` | `0x1F_0017_8018` | TX FIFO for SM2 |
| TXF3 | `0x01C` | `0x1F_0017_801C` | TX FIFO for SM3 |
| RXF0 | `0x020` | `0x1F_0017_8020` | RX FIFO for SM0 (read-only) |
| RXF1 | `0x024` | `0x1F_0017_8024` | RX FIFO for SM1 |
| RXF2 | `0x028` | `0x1F_0017_8028` | RX FIFO for SM2 |
| RXF3 | `0x02C` | `0x1F_0017_802C` | RX FIFO for SM3 |

### FSTAT Register Layout (offset `0x004`)

```
Bit  Field         Description
31:27  -           Reserved
26:24  RXFULL      RX FIFO full flags (1 bit per SM, SM0 = bit 24)
23:19  -           Reserved
18:16  RXEMPTY     RX FIFO empty flags (1 bit per SM, SM0 = bit 16)
15:11  -           Reserved
10:8   TXFULL      TX FIFO full flags (1 bit per SM, SM0 = bit 8)
 7:3   -           Reserved
 2:0   TXEMPTY     TX FIFO empty flags (1 bit per SM, SM0 = bit 0)
```

For SM0:
- TX full:  `FSTAT & (1 << 8)`  — stop writing when set
- TX empty: `FSTAT & (1 << 0)`  — FIFO drained
- RX full:  `FSTAT & (1 << 24)` — FIFO overflow risk
- RX empty: `FSTAT & (1 << 16)` — nothing to read

## SRAM Layout

### Device Tree

```dts
sram: sram@400000 {
    compatible = "mmio-sram";
    reg = <0xc0 0x40400000  0x0 0x10000>;  /* 64 KB */
    rp1_fw_shmem: shmem@ff00 {
        compatible = "raspberrypi,rp1-shmem";
        reg = <0xff00 0x100>;  /* Firmware mailbox: last 256 bytes */
    };
};
```

### Memory Regions

```
Offset    Size      Purpose
0x0000    16 KB     TX ring buffer (Phase 3: 4 periods x 4 KB)
0x4000    16 KB     RX ring buffer (Phase 3: 4 periods x 4 KB)
0x8000    28,416 B  Reserved / scratch / future use
0xFEFF    -         End of safe region
0xFF00    256 B     Firmware mailbox (DO NOT TOUCH)
0xFFFF    -         End of SRAM
```

**Available SRAM:** 65,280 bytes (0x0000–0xFEFF). Firmware uses 0xFF00–0xFFFF.

### Host Access

- Physical address: `0x1F_0040_0000` (BAR2 base)
- mmap via `/dev/mem` with offset `0x1F00400000`
- Map size: `0x10000` (64 KB)
- Access type: 32-bit aligned reads/writes

## DMA Configuration

### DREQ IDs

| Channel | DREQ ID | Direction |
|---------|---------|-----------|
| PIO CH0 TX | 0x38 (56) | Memory → PIO FIFO (write to TXF) |
| PIO CH0 RX | 0x39 (57) | PIO FIFO → Memory (read from RXF) |

### Heavy DMA Channels

Channels 0 and 1 support 8-beat AXI bursts (32 bytes per burst).
Other channels are limited to single-beat transfers.

### DMA Controller (dw-axi-dmac)

- RP1 internal: `0x4018_8000`
- BCM2712 physical: `0x1F_0018_8000`
- Supports cyclic descriptors for ring buffer operation

### Cyclic DMA Operation

```
TX path:  SRAM ring (src, fixed stride) → PIO TXF0 (dst, fixed addr)
RX path:  PIO RXF0 (src, fixed addr) → SRAM ring (dst, fixed stride)

Ring buffer: 16 KB, divided into 4 x 4 KB periods
DMA cycles through periods continuously, raising interrupt per period.
Host advances producer/consumer pointer on period completion.
```

## Data Paths Compared

### Current: Standard Kernel DMA (~42 MB/s internal loopback)

```
Host CPU ──write──→ DRAM buffer
                      ↓ (PCIe read by RP1 DMA, ~70 cycle handshake per burst)
                 RP1 DMA ──APB──→ PIO FIFO
                                    ↓ (PIO program)
                                 PIO FIFO
                      ↑ (PCIe write by RP1 DMA)
                 RP1 DMA ──APB──← PIO FIFO
Host CPU ←─read──── DRAM buffer
```

### Phase 3: SRAM + Cyclic DMA (target >42 MB/s)

```
Host CPU ──PCIe──→ SRAM ring buffer
                      ↓ (RP1-internal AXI, no PCIe per burst)
                 RP1 DMA (cyclic) ──APB──→ PIO FIFO
                                              ↓ (PIO program)
                                           PIO FIFO
                 RP1 DMA (cyclic) ──APB──← PIO FIFO
                      ↑ (RP1-internal AXI)
                   SRAM ring buffer
Host CPU ←─PCIe──── SRAM ring buffer
```

Key advantage: DMA reads/writes SRAM over internal AXI bus (single-cycle for
M3, low-latency for DMA), eliminating per-burst PCIe round-trips.

### Phase 4: M3 Core 1 Bridge (target ~66 MB/s)

```
Host CPU ──PCIe──→ SRAM ring buffer
                      ↑↓ (single-cycle, 32-bit, 200 MHz)
                 M3 Core 1 (custom firmware, tight polling loop)
                      ↑↓ (single-cycle via 0xF0000000 alias)
                   PIO FIFOs
```

Key advantage: M3 Core 1 accesses both SRAM and PIO FIFOs in single cycles,
with zero handshake overhead. cleverca22 demonstrated ~66 MB/s this way.

## M3 Core 1

### Bootstrap Sequence

```c
/* Write entry point XOR magic to SYSCFG */
*(volatile uint32_t*)(0x40154014) = 0x4FF83F2D ^ entrypoint;
/* Write stack pointer */
*(volatile uint32_t*)(0x4015401C) = stack_pointer;
/* Send event to wake Core 1 */
asm volatile("sev");
```

### Memory Map (M3 perspective)

| Address | Resource |
|---------|----------|
| `0x2000_0000` | Shared SRAM (64 KB, single-cycle) |
| `0xF000_0000` | PIO peripheral alias (single-cycle) |
| `0x4017_8000` | PIO block (APB, slower) |

### Constraints

- 8 KB instruction RAM, 8 KB data RAM (for firmware)
- No official support — community-proven approaches only
- Recovery may require debug UART if firmware hangs

## References

- [librerpi/rp1-lk](https://github.com/librerpi/rp1-lk) — Bare-metal M3 OS
- [G33KatWork/RP1-Reverse-Engineering](https://github.com/G33KatWork/RP1-Reverse-Engineering) — Core 1 bootstrap
- [raspberrypi/rp1-pio kernel driver](https://github.com/raspberrypi/linux) — DMA/PIO interface
- pelwell's optimization guidance: "move DMA buffers to shared SRAM, cyclic DMA"
- cleverca22's M3 Core 1 throughput measurement: ~66 MB/s

## Existing Code to Reuse

| Module | Path | Purpose |
|--------|------|---------|
| `benchmark_stats.{c,h}` | `benchmark/` | Statistics (min/max/mean/median/percentiles) |
| `benchmark_format.{c,h}` | `benchmark/` | Human-readable + JSON output |
| `benchmark_verify.{c,h}` | `benchmark/` | Pattern generation + verification |
| mmap pattern | `toggle/toggle_rpi4.c` | `/dev/mem` GPIO mmap (adapt for BAR2) |
| DMA thread pattern | `gpio-loopback/gpio_loopback.c` | Pthread-based DMA transfers |
