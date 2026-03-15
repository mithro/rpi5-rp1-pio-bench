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

### RP1 M3 Firmware Overview

The RP1 contains two ARM Cortex-M3 cores. Core 0 runs closed-source firmware
from Raspberry Pi's internal `rp1-sdk` repository. The firmware is:

- **Embedded in the BCM2712 SPI EEPROM** (not a filesystem file)
- **Loaded by the VPU bootloader** (`start_cd.elf`) before Linux starts
- **Stored as two LZ4-compressed blobs:**
  - `rp1c0fw1.bin` (13,942 → 16,384 bytes): Main system firmware
  - `rp1c0fw2.bin` (8,512 → 14,864 bytes): PCIe link management / encrypted blob
- **Runs directly from shared SRAM** at `0x20000000`
- **Source paths in binary:** `/home/phil/pi/rp1-sdk/src/drivers/rp1_platform/`
- **Built with Pico SDK framework** (same as RP2040/RP2350)
- **Core 1 is unused** by stock firmware (available for custom code)

The firmware handles: clock/PLL configuration, PCIe link management (LTSSM state
machine), peripheral initialization, and PIO/DMA RPC services via mailbox IPC.

### Actual Firmware SRAM Layout (Measured)

**WARNING:** The device tree only reserves 0xFF00–0xFFFF (256 bytes) as the firmware
mailbox, but the firmware's code and data occupy the lower ~36 KB, plus descriptors
and an encrypted blob. Writing to firmware-critical regions corrupts RP1 PIO state.
Recovery requires a full power cycle (Linux reboot does NOT reset RP1).

Measured via `sram_dump`, `sram_monitor`, and `sram_region_test`:

```
Offset    Size      Purpose                                    Writable?
──────────────────────────────────────────────────────────────────────────
0x0000    320 B     Vector table (80 entries)                   NO (crash)
0x0140    60 B      VTOR relocation code                        NO
0x017C    152 B     BKPT stubs for unused IRQ handlers          NO
0x0214    224 B     Reset handler + CRT startup                 NO
0x0304    112 B     Boot config table (magic words, addresses)  NO
0x0378    328 B     Platform init functions                     NO
0x04C4    17,528 B  Main firmware code                          NO
0x4940    10,692 B  Code: printf, PCIe LTSSM state machine      NO
0x7304    1,532 B   PCIe state strings, bus master names        NO
0x7900    4,352 B   Debug strings, assert messages, paths       NO
──────────────────────────────────────────────────────────────────────────
0x8000    2,560 B   Firmware data (safe to overwrite)           YES ✓
──────────────────────────────────────────────────────────────────────────
0x8A00    256 B     PIO device descriptors + pad register map   NO (PIO crash)
──────────────────────────────────────────────────────────────────────────
0x8B00    29,952 B  Clock/peripheral config, encrypted blob     YES ✓
                    (includes rp1c0fw2.bin at ~0xB728)
──────────────────────────────────────────────────────────────────────────
0xFF00    256 B     Firmware mailbox (host↔firmware IPC)        NO (mailbox)
```

**Available SRAM: ~32,512 bytes (31.75 KB)** in two regions:
- `0x8000 - 0x89FF` (2,560 bytes)
- `0x8B00 - 0xFEFF` (29,952 bytes) — largest contiguous block

The PIO descriptor block at `0x8A00` contains the string "PIO " followed by
GPIO pad control register addresses (0x400C4xxx). Overwriting it crashes PIO.

Note: regions marked "YES" contain non-zero firmware data but PIO continues to
function after overwriting them. The data may be boot-time configuration that
the firmware caches internally, or data only used during specific operations.

### Dynamic Firmware State

Only ~520 bytes at `0x9F48-0xA14F` are actively written by the firmware at
runtime (counter/timestamp updates every ~2 seconds). All other non-zero data
is static after boot. The dynamic region is within the safe-to-overwrite zone,
but overwriting it may affect firmware diagnostics or statistics.

### Implications for SRAM Usage

**SRAM CAN be used as DMA source/destination** at DMA address `0xc020000000`.
The RP1 DMA controller reaches M3 TCM via the `0xc0` internal bus prefix.
However, SRAM DMA does not improve throughput over host DRAM DMA because
the APB→PIO FIFO handshake is the bottleneck, not memory access latency.

SRAM can be used as:
1. **DMA ring buffers** — accessible at `0xc020000000` + offset (verified)
2. **M3 Core 1 data buffers** — single-cycle access from Cortex-M3 cores
3. **Host CPU data staging** — accessible via PCIe BAR2 mmap
4. **Shared memory IPC** — between host CPU and M3 Core 1

For Phase 4 (M3 Core 1 bridge), SRAM serves as the shared ring buffer between
host CPU and Core 1's tight FIFO polling loop. Available safe regions:
```
0x8000    2,560 B   Available (firmware data, safe to overwrite)
0x8A00    256 B     *** PIO DESCRIPTORS — DO NOT TOUCH ***
0x8B00    29,952 B  Available (largest contiguous block)
0xFF00    256 B     *** FIRMWARE MAILBOX — DO NOT TOUCH ***
```

### References

- [MichaelBell/rp1-hacking](https://github.com/MichaelBell/rp1-hacking) — Custom M3 code at 0x8000+
- [librerpi/rp1-lk](https://github.com/librerpi/rp1-lk) — Uses MEMBASE=0x20008000
- [G33KatWork/RP1-Reverse-Engineering](https://github.com/G33KatWork/RP1-Reverse-Engineering) — Firmware extraction
- rp1-sdk source (closed, Raspberry Pi internal): `/home/phil/pi/rp1-sdk/`

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

### Phase 3: Cyclic DMA (~9.1 MB/s achieved)

**SRAM DMA ADDRESS FOUND:** The RP1 DMA controller CAN access shared SRAM at
DMA address **`0xc020000000`** — the M3 TCM address (`0x20000000`) with the
`0xc0` RP1 internal bus prefix. Verified with sram_addr_probe: writing a known
pattern to SRAM via BAR2, then DMA reading from `0xc020008B00` through PIO
loopback (NOT), produces the expected bitwise-NOT pattern in the RX buffer.
1024/1024 words matched perfectly.

**However, SRAM DMA does NOT improve throughput over host DRAM DMA:**

| Mode | Throughput | Data Path |
|------|-----------|-----------|
| DRAM (host, via PCIe) | **9.28 MB/s** | Host DRAM → PCIe → DMA → APB → PIO |
| SRAM (RP1-internal) | **8.84 MB/s** | SRAM → DMA → APB → PIO |

The bottleneck is the DMA→APB→PIO FIFO handshake (~70 bus cycles per word),
not the memory source. SRAM eliminates the PCIe round-trip for data fetches,
but the APB bridge to PIO FIFOs dominates the transfer time.

**SRAM DMA address probe results:**

| DMA Address | Source | Result |
|-------------|--------|--------|
| `0xc0401c0000` | RP1_RAM_BASE+0xc040 | TX=1 RX=0 (stall) |
| `0xc040400000` | DT sram@400000 | Garbage (0xFFFFFFF0) |
| `0x401c0000` | RP1_RAM_BASE raw | Garbage (0x21522152) |
| `0x40400000` | BAR2 offset raw | Garbage (0x21522152) |
| `0x20000000` | M3 TCM raw | Garbage (0x21522152) |
| **`0xc020000000`** | **M3 TCM + 0xc0 prefix** | **PERFECT MATCH** |
| `0x1f00400000` | CPU phys BAR2 | All 0xFFFFFFFF |

**Implementation** uses both host DRAM and SRAM ring buffers with cyclic DMA.
DRAM mode achieves **9.28 MB/s**, SRAM mode achieves **8.84 MB/s**, both with
0 data errors:

```
Host CPU ──PCIe──→ Host DRAM ring buffer (dma_alloc_coherent, 16 KB)
                      ↓ (PCIe read by RP1 DMA, ~70 cycle handshake per burst)
                 RP1 DMA (cyclic) ──APB──→ PIO FIFO
                                              ↓ (PIO program: NOT)
                                           PIO FIFO
                 RP1 DMA (cyclic) ──APB──← PIO FIFO
                      ↑ (PCIe write by RP1 DMA)
                   Host DRAM ring buffer
Host CPU ←─PCIe──── Host DRAM ring buffer (dma_mmap_coherent)
```

Ring buffer layout (host DRAM, one 16 KB page):
```
Offset 0x0000   8,192 B   TX ring (8 × 1 KB periods)
Offset 0x2000   8,192 B   RX ring (8 × 1 KB periods)
```

**Result: 9.1 MB/s — below 42 MB/s target.** The per-word PCIe+APB round-trip
bottleneck remains because DMA source/destination is still host DRAM via PCIe.
Cyclic DMA only eliminates userspace ioctl overhead between bursts.

Implementation: `kmod/rp1_pio_sram.ko` + `sram_dma_bench.c`

**Conclusion:** The only path to >42 MB/s is Phase 4 (M3 Core 1), which can
access both SRAM and PIO FIFOs in single clock cycles.

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
