# RP1 PIO Register Layout from M3 Cortex-M3 Perspective

Research compiled from MichaelBell/rp1-hacking and RP2040 datasheet cross-reference.

## PIO Base Addresses

The RP1 has **two different PIO base addresses** depending on who is accessing:

| Accessor | Base Address | What's Accessible |
|----------|-------------|-------------------|
| M3 Core 0/1 (RP1 internal bus) | `0xF0000000` | Full PIO register set (CTRL, FSTAT, SMs, instruction memory, etc.) |
| M3 Core 0/1 (RP1 internal bus) | `0x40178000` | FIFOs only (TX + RX) |
| Linux host (PCIe BAR) | `0xc0_40178000` | FIFOs only (TX + RX) |

**Key findings:**
- `0xF0000000` is the full PIO register space, accessible only from RP1 M3 cores.
- `0x40178000` is a FIFO-only window accessible from both M3 and the host (via PCIe).
- The host (Linux/piolib) cannot access CTRL, FSTAT, SM configuration, or instruction memory directly -- those go through the M3 firmware mailbox protocol.
- `0xF0000000` is **1.41× faster** than `0x40178000` for register reads from M3 (6.7M vs 4.76M reads/sec). Source: `throughput-pioloop-m3poll/test_pio_addr.s` benchmark.
- Each PIO register access from M3 takes **~54 cycles (~270 ns at 200 MHz)** via the APB bus bridge — NOT single-cycle. Source: `throughput-pioloop-m3poll/pio_bridge.s` throughput measurements.
- **FSTAT at `0xF0000000` does NOT dynamically update** when FIFO state changes. Polling RXEMPTY/TXFULL from Core 1 hangs indefinitely. FSTAT only reflects a static snapshot. Source: `throughput-pioloop-m3poll/pio_bridge.s` FSTAT polling test (2026-03-17).

## Complete Register Map at 0xF0000000

Based on MichaelBell's PIO.md documentation, cross-referenced with RP2040 Section 3.7:

### Control and Status Registers

| Offset | Register | Description | RP2040 Differences |
|--------|----------|-------------|-------------------|
| `0x00` | CTRL | PIO control (enable SMs, restart, clkdiv_restart) | None known |
| `0x04` | FSTAT | FIFO status (RXEMPTY, TXFULL per SM) | None known |
| `0x08` | FDEBUG | FIFO debug (sticky overflow/underflow) | None known |
| `0x0C` | FLEVEL | FIFO levels (4 bits per FIFO direction per SM) | Max level is 16 (8-deep FIFOs joined = 16), reads 15 when level is 15 or 16 |
| `0x10` | FLEVEL2 | **RP1-only**: Top bit of FIFO levels (1 when level is 16, 0 otherwise) | New register, not in RP2040 |

### FIFO Data Registers

| Offset | Register | Also at | Description |
|--------|----------|---------|-------------|
| `0x14` | TXF0 | `0x40178000` | TX FIFO for SM0 |
| `0x18` | TXF1 | `0x40178004` | TX FIFO for SM1 |
| `0x1C` | TXF2 | `0x40178008` | TX FIFO for SM2 |
| `0x20` | TXF3 | `0x4017800C` | TX FIFO for SM3 |
| `0x24` | RXF0 | `0x40178010` | RX FIFO for SM0 |
| `0x28` | RXF1 | `0x40178014` | RX FIFO for SM1 |
| `0x2C` | RXF2 | `0x40178018` | RX FIFO for SM2 |
| `0x30` | RXF3 | `0x4017801C` | RX FIFO for SM3 |

### Interrupt Registers

| Offset | Register | Description |
|--------|----------|-------------|
| `0x34` | IRQ | State machine IRQ flags |
| `0x38` | IRQ_FORCE | Force IRQ flags |
| `0x3C` | INPUT_SYNC_BYPASS | Bypass input synchronizer |

### Debug Registers

| Offset | Register | Description |
|--------|----------|-------------|
| `0x40` | DBG_PADOUT | GPIO output values |
| `0x44` | DBG_PADOE | GPIO output enable values |
| `0x48` | DBG_CFGINFO | PIO configuration info (FIFO depth, SM count, IMEM size) |

### Instruction Memory

| Offset | Register | Description |
|--------|----------|-------------|
| `0x4C` - `0xC8` | INSTR_MEM0-31 | 32 instruction words (same as RP2040) |

### State Machine Registers (SM0)

Each SM has a stride of `0x20` (32 bytes). SM0 starts at `0xCC`, SM1 at `0xEC`, SM2 at `0x10C`, SM3 at `0x12C`.

| Offset (SM0) | Register | Description | RP2040 Differences |
|--------------|----------|-------------|-------------------|
| `0xCC` | SM0_CLKDIV | Clock divider (16.8 fractional) | None known |
| `0xD0` | SM0_EXECCTRL | Execution control (wrap, side_en, etc.) | Bit 5 is settable (reserved in RP2040), unknown function |
| `0xD4` | SM0_SHIFTCTRL | Shift control (autopush/pull, thresholds, join) | None known |
| `0xD8` | SM0_ADDR | Current instruction address | None known |
| `0xDC` | SM0_INSTR | Current/exec instruction | None known |
| `0xE0` | SM0_PINCTRL | Pin control (set/out/in/sideset bases and counts) | None known |
| `0xE4` | SM0_DMATX | **RP1-only**: DMA TX control | See below |
| `0xE8` | SM0_DMARX | **RP1-only**: DMA RX control | See below |

### SM Register Offsets Summary (all SMs)

| Register | SM0 | SM1 | SM2 | SM3 |
|----------|------|------|------|------|
| CLKDIV | `0xCC` | `0xEC` | `0x10C` | `0x12C` |
| EXECCTRL | `0xD0` | `0xF0` | `0x110` | `0x130` |
| SHIFTCTRL | `0xD4` | `0xF4` | `0x114` | `0x134` |
| ADDR | `0xD8` | `0xF8` | `0x118` | `0x138` |
| INSTR | `0xDC` | `0xFC` | `0x11C` | `0x13C` |
| PINCTRL | `0xE0` | `0x100` | `0x120` | `0x140` |
| DMATX | `0xE4` | `0x104` | `0x124` | `0x144` |
| DMARX | `0xE8` | `0x108` | `0x128` | `0x148` |

### DMA Control Registers (RP1-only)

**SM0_DMATX (offset 0xE4):**
| Bits | Field | Description |
|------|-------|-------------|
| 31 | Enable | Enable DREQ |
| 30 | Status | Current DREQ status (read-only) |
| 11:7 | Priority? | Lower seems to not affect much |
| 4:0 | Threshold | DREQ asserted when TX FIFO level below this value |

**SM0_DMARX (offset 0xE8):**
| Bits | Field | Description |
|------|-------|-------------|
| 31 | Enable | Enable DREQ |
| 30 | Status | Current DREQ status (read-only) |
| 11:7 | Priority? | Lower value seems faster |
| 4:0 | Threshold | DREQ asserted when RX FIFO level >= this value |

### Interrupt Control

| Offset | Register | Description |
|--------|----------|-------------|
| `0x14C` | INTR | Raw interrupt status |
| `0x150` - `0x168` | IRQ0_INTE/S/F, IRQ1_INTE/S/F | Interrupt enable/status/force (2 IRQ sets) |

### Identification

`0x16C` reads as `pio2` in ASCII (identifier string).

## FSTAT Bit Definitions

FSTAT at offset `0x04` follows the RP2040 layout:

```
Bit  Field       Description
31:28  (reserved)
27:24  TXEMPTY    TX FIFO empty, one bit per SM (bit 24=SM0, 25=SM1, 26=SM2, 27=SM3)
23:20  (reserved)
19:16  TXFULL     TX FIFO full, one bit per SM (bit 16=SM0, 17=SM1, 18=SM2, 19=SM3)
15:12  (reserved)
11:8   RXEMPTY    RX FIFO empty, one bit per SM (bit 8=SM0, 9=SM1, 10=SM2, 11=SM3)
7:4    (reserved)
3:0    RXFULL     RX FIFO full, one bit per SM (bit 0=SM0, 1=SM1, 2=SM2, 3=SM3)
```

**For SM0 specifically:**
- **TXFULL for SM0**: FSTAT bit 16 (`1 << 16 = 0x00010000`)
- **TXEMPTY for SM0**: FSTAT bit 24 (`1 << 24 = 0x01000000`)
- **RXEMPTY for SM0**: FSTAT bit 8 (`1 << 8 = 0x00000100`)
- **RXFULL for SM0**: FSTAT bit 0 (`1 << 0 = 0x00000001`)

**FIFO access pattern (from M3 Core 1 firmware):**

**WARNING:** Do NOT poll FSTAT — it does not dynamically update from Core 1.
Write to TXF and read from RXF directly. The ~270 ns APB bus latency per
register access provides sufficient delay for PIO to process between accesses.

```c
#define PIO_BASE        0xF0000000
#define TXF0_OFFSET     0x14
#define RXF0_OFFSET     0x24

volatile uint32_t *pio = (volatile uint32_t *)PIO_BASE;

// Write to TX FIFO, then read from RX FIFO
// PIO processes in 3 cycles (15 ns), well within the ~270 ns bus latency
pio[TXF0_OFFSET/4] = data;
uint32_t val = pio[RXF0_OFFSET/4];
```

Source: Verified in `throughput-pioloop-m3poll/pio_bridge.s` — FSTAT polling hangs Core 1;
direct write-then-read achieves 6.89 MB/s (2026-03-17 benchmark).

## RP1 vs RP2040 Differences Summary

| Feature | RP2040 | RP1 |
|---------|--------|-----|
| PIO instances | 2 (PIO0, PIO1) | 1 |
| Instruction memory | 32 words | 32 words (same) |
| State machines | 4 per PIO | 4 (same) |
| FIFO depth | 4 per direction per SM | **8 per direction per SM** |
| Joined FIFO depth | 8 | **16** |
| FLEVEL2 register | N/A | New (top bit of level for deep FIFOs) |
| SM stride | 0x18 (24 bytes) | **0x20 (32 bytes)** due to DMATX/DMARX |
| DMATX/DMARX registers | N/A (DMA configured externally) | New per-SM DMA control registers |
| EXECCTRL bit 5 | Reserved | Settable, unknown function |
| FIFO-only window | N/A | `0x40178000` (accessible from PCIe) |
| Configuration access | Direct register access | Only from M3 (0xF0000000); host uses firmware mailbox |

## Host-Configures, Core 1 Accesses FIFOs: The Recommended Pattern

Based on architecture analysis and empirical testing (`throughput-pioloop-m3poll/pio_bridge.s`):

1. **Host configures PIO via piolib** (ioctls → firmware mailbox → Core 0 firmware writes to `0xF0000000` registers)
   - Load PIO program (instruction memory)
   - Configure SM (CLKDIV, EXECCTRL, SHIFTCTRL, PINCTRL)
   - Configure GPIO pins
   - Enable SM
   - **PIO setup must happen AFTER Core 1 launch** — the `trigger_pio_mailbox()` call during SEV bootstrap causes Core 0 firmware to refresh INSTR_MEM, overwriting any prior program loads.

2. **Core 1 firmware accesses FIFOs directly** at `0xF0000000` (preferred, 1.41× faster):
   - `0xF0000000 + 0x14` (TXF0) / `0xF0000000 + 0x24` (RXF0)
   - Each FIFO read/write takes ~54 cycles (~270 ns) via the APB bus bridge
   - Achieves ~6.89 MB/s CPU-polled throughput (hardware-limited)

3. **WARNING: Do NOT poll FSTAT from Core 1.** FSTAT at `0xF0000004` does not dynamically update when FIFO state changes. Polling RXEMPTY or TXFULL causes Core 1 to hang indefinitely. The APB bus latency (~270 ns per register access) provides sufficient implicit delay for PIO to complete processing between a TXF write and RXF read.

**Important:** Core 1 CPU-polled FIFO access (6.89 MB/s) is 6× slower than cyclic DMA (40-54 MB/s). Use DMA for high-throughput applications. Core 1 FIFO access is useful for low-latency control or data that must be preprocessed before DMA.

## blink_core1.s Analysis (MichaelBell)

The `blink_core1.s` example does NOT use PIO -- it directly manipulates GPIO via:
- `0x400D008C` -- SYS_RIO function select (output enable for GPIO)
- `0x400F0048` -- Pad control
- `0x400E2000` -- GPIO output enable / output value (SYS_RIO block)
- Initial SP: `0x10003FFC` (PROC-local SRAM, not shared SRAM)

This confirms M3 Core 1 can access peripherals at `0x40xxxxxx` addresses. PIO registers at `0xF0000000` are confirmed accessible from Core 1 (verified by `throughput-pioloop-m3poll/test_pio_fifo.s` and `throughput-pioloop-m3poll/test_pio_addr.s`).

## core1_test.c Analysis (MichaelBell)

The host-side launcher uses two address translation macros:
```c
#define REG32a(addr) ((volatile uint32_t*)(((addr) - 0x40000000) + mmio))
#define REG32b(addr) ((volatile uint32_t*)(((addr) - 0x20000000 + 0x400000) + mmio))
```

Where `mmio` = mmap of `/dev/mem` at physical `0x1F00000000` with size `0x10000000`.

- `REG32a`: For peripheral registers at `0x40xxxxxx` (M3 view) -> host physical address
- `REG32b`: For SRAM at `0x20xxxxxx` (M3 view) -> host physical address

This confirms:
- M3 SRAM is at `0x20000000` from M3's perspective
- M3 peripherals are at `0x40000000` from M3's perspective
- Host maps these via PCIe BAR at `0x1F00000000`

## C Header Defines for Core 1 Firmware

```c
/* PIO register base from M3 */
#define PIO_BASE            0xF0000000

/* Also accessible FIFO-only window */
#define PIO_FIFO_BASE       0x40178000

/* PIO register offsets */
#define PIO_CTRL            0x00
#define PIO_FSTAT           0x04
#define PIO_FDEBUG          0x08
#define PIO_FLEVEL          0x0C
#define PIO_FLEVEL2         0x10  /* RP1-only */

#define PIO_TXF0            0x14
#define PIO_TXF1            0x18
#define PIO_TXF2            0x1C
#define PIO_TXF3            0x20

#define PIO_RXF0            0x24
#define PIO_RXF1            0x28
#define PIO_RXF2            0x2C
#define PIO_RXF3            0x30

#define PIO_IRQ             0x34
#define PIO_IRQ_FORCE       0x38
#define PIO_INPUT_SYNC_BYPASS 0x3C

#define PIO_DBG_PADOUT      0x40
#define PIO_DBG_PADOE       0x44
#define PIO_DBG_CFGINFO     0x48

#define PIO_INSTR_MEM0      0x4C  /* through INSTR_MEM31 at 0xC8 */

/* SM0 registers */
#define PIO_SM0_CLKDIV      0xCC
#define PIO_SM0_EXECCTRL    0xD0
#define PIO_SM0_SHIFTCTRL   0xD4
#define PIO_SM0_ADDR        0xD8
#define PIO_SM0_INSTR       0xDC
#define PIO_SM0_PINCTRL     0xE0
#define PIO_SM0_DMATX       0xE4  /* RP1-only */
#define PIO_SM0_DMARX       0xE8  /* RP1-only */

/* SM register stride (0x20 = 32 bytes, not 0x18 like RP2040) */
#define PIO_SM_STRIDE       0x20

/* FSTAT bit definitions */
#define FSTAT_TXEMPTY(sm)   (1u << (24 + (sm)))
#define FSTAT_TXFULL(sm)    (1u << (16 + (sm)))
#define FSTAT_RXEMPTY(sm)   (1u << (8 + (sm)))
#define FSTAT_RXFULL(sm)    (1u << (0 + (sm)))

/* Convenience for SM0 */
#define FSTAT_TXEMPTY_SM0   (1u << 24)
#define FSTAT_TXFULL_SM0    (1u << 16)
#define FSTAT_RXEMPTY_SM0   (1u << 8)
#define FSTAT_RXFULL_SM0    (1u << 0)

/* CTRL register bits */
#define CTRL_SM_ENABLE(sm)  (1u << (sm))
#define CTRL_SM_RESTART(sm) (1u << (4 + (sm)))
#define CTRL_CLKDIV_RESTART(sm) (1u << (8 + (sm)))
```

## Sources

- MichaelBell/rp1-hacking: PIO.md (repo root)
- MichaelBell/rp1-hacking: launch_core1/blink_core1.s
- MichaelBell/rp1-hacking: launch_core1/core1_test.c
- RP2040 Datasheet Section 3.7 (PIO register definitions)
- Local: docs/rp1-pio-firmware-comms.md (mailbox protocol analysis)
