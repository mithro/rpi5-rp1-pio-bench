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

**WARNING:** RP1 PIO register offsets differ from RP2040/RP2350!
These offsets come from the `rp1-pio` kernel driver (`drivers/misc/rp1-pio.c`).

| Register | Offset | BCM2712 Physical | Purpose |
|----------|--------|-------------------|---------|
| TXF0 | `0x000` | `0x1F_0017_8000` | TX FIFO for SM0 (write-only) |
| TXF1 | `0x004` | `0x1F_0017_8004` | TX FIFO for SM1 |
| TXF2 | `0x008` | `0x1F_0017_8008` | TX FIFO for SM2 |
| TXF3 | `0x00C` | `0x1F_0017_800C` | TX FIFO for SM3 |
| RXF0 | `0x010` | `0x1F_0017_8010` | RX FIFO for SM0 (read-only) |
| RXF1 | `0x014` | `0x1F_0017_8014` | RX FIFO for SM1 |
| RXF2 | `0x018` | `0x1F_0017_8018` | RX FIFO for SM2 |
| RXF3 | `0x01C` | `0x1F_0017_801C` | RX FIFO for SM3 |

### FSTAT and Control Registers

**FSTAT, CTRL, and other PIO control registers are NOT directly accessible
from the host.** They are accessed through the RP1 firmware RPC layer
(via `rp1_firmware_message()` in the kernel driver). Use piolib ioctl
functions (e.g., `pio_sm_is_tx_fifo_full()`) for FIFO status checks.

**Critical constraint:** Mixing direct FIFO register writes with piolib
ioctl-based FSTAT polling can desynchronize the firmware's view of FIFO
state, causing ioctl calls to hang. For throughput-critical paths, use
pure direct register access (lockstep write TX → read RX).

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

### Actual Firmware SRAM Usage (Measured)

**WARNING:** The device tree only reserves 0xFF00–0xFFFF (256 bytes) as the firmware
mailbox, but the RP1 firmware actually uses **94.9% of shared SRAM** (61,676 bytes).
Writing to firmware-occupied SRAM regions corrupts RP1 PIO state, causing all SM
claims to return ETIMEDOUT. Recovery requires a full power cycle (Linux reboot does
NOT reset RP1).

Measured SRAM usage after fresh boot (via `sram_dump` and `sram_monitor`):

```
Category           Words   Bytes    %     Notes
Static non-zero    15,419  61,676   94.1  Firmware code/data loaded at boot
Always zero           835   3,340    5.1  Scattered in tiny gaps (largest: 872 B)
Dynamic               130     520    0.8  Counters/timestamps at 0x9F48-0xA14F
```

Key non-zero regions:
- `0x0000 - 0x04BF`: Firmware data structures (~1.2 KB)
- `0x04C4 - 0x493B`: Massive firmware block (~17.5 KB, likely code)
- `0x4940 - 0x7303`: More firmware data (~10 KB)
- `0x7308 - 0x8BEF`: Sparse firmware structures (~5 KB)
- `0x8BF0 - 0xA14F`: Mix of static and dynamic data (~5.5 KB)
- `0xA178 - 0xB68F`: Firmware data (~5.6 KB)
- `0xB728 - 0xFFFF`: Dense firmware data through mailbox (~18 KB)

**Available SRAM for user buffers: effectively ZERO.**
The 3,340 bytes of zero space is scattered in gaps too small for DMA ring buffers.

### Implications for DMA Buffer Placement

The Phase 3 plan assumed 32 KB of SRAM was available for DMA ring buffers.
This is NOT the case. Alternative approaches:
1. Use host DRAM for DMA buffers (current approach, ~42 MB/s)
2. M3 Core 1 bridge (Phase 4) — avoids SRAM buffers entirely
3. Negotiate SRAM allocation with firmware (requires firmware modification)
4. Find a firmware version/config that uses less SRAM

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

### Phase 3: SRAM + Cyclic DMA — BLOCKED

**STATUS:** Cannot implement as designed. RP1 firmware occupies 94.9% of shared
SRAM. No contiguous free region large enough for DMA ring buffers (need 32 KB,
largest zero gap is 872 bytes). Writing to firmware-occupied SRAM regions crashes
RP1 PIO subsystem.

Original plan required:
```
Host CPU ──PCIe──→ SRAM ring buffer (16 KB TX + 16 KB RX)
                      ↓ (RP1-internal AXI, no PCIe per burst)
                 RP1 DMA (cyclic) ──APB──→ PIO FIFO
```

Possible unblocking paths:
- Firmware modification to free SRAM regions
- Different firmware configuration that uses less SRAM
- Negotiate SRAM allocation via firmware RPC

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
