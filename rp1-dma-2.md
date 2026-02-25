# High-speed DMA transfers with RP1 PIO on Raspberry Pi 5

The Raspberry Pi 5's RP1 southbridge chip contains a PIO (Programmable I/O) block nearly identical to the RP2040's — same instruction set, same 4 state machines — but with **8-deep FIFOs** (doubled from 4) and a **200 MHz system clock**. The critical architectural difference: PIO configuration registers are accessible only from RP1's internal Cortex-M3 cores, not directly from the ARM host over PCIe. Data enters and exits PIO FIFOs via RP1's own 8-channel DMA controller (not BCM2712's DMA), traversing a PCIe 2.0 x4 link. Practical DMA throughput currently sits at **10–27 MB/s** depending on burst configuration, well below the ~60–75 MB/s theoretical per-channel DMA read bandwidth — a bottleneck rooted in RP1's DMA handshake overhead of ~70 bus cycles per transfer and a 32-bit APB bus feeding a 128-bit DMA engine. This report covers the full architecture, DMA setup, register-level details, working code examples, and performance optimization strategies.

## RP1's PIO block: half an RP2040, behind a PCIe wall

RP1 is a custom ASIC fabricated on TSMC 40nm, containing dual Cortex-M3 cores, an 8-channel DMA controller, and a rich peripheral set including one PIO instance. It connects to the BCM2712 SoC via **PCIe 2.0 x4** (20 Gbps (gigabits/s) raw, ~2 GB/s (gigabytes/s) effective after 8b/10b encoding, ~1 μs one-way latency). The chip is enumerated as `[1de4:0001] Raspberry Pi Ltd RP1 PCIe 2.0 South Bridge`.

| Feature | RP2040 | RP1 |
|---------|--------|-----|
| PIO instances | 2 | **1** |
| State machines per instance | 4 | 4 |
| Instruction memory | 32 words × 16-bit | 32 words × 16-bit |
| TX FIFO depth per SM | 4 entries × 32-bit | **8 entries × 32-bit** |
| RX FIFO depth per SM | 4 entries × 32-bit | **8 entries × 32-bit** |
| Joined FIFO depth | 8 entries | **16 entries** |
| Instruction set | 9 instructions, 1 cycle each | **Identical** |
| System clock | 125–133 MHz | **200 MHz** |
| Host register access | Direct (single-cycle) | **FIFOs only** over PCIe; config via M3 firmware mailbox |
| DMA handshake latency | ~4 cycles | **~70 bus cycles** |

The PIO instruction set — `JMP`, `WAIT`, `IN`, `OUT`, `PUSH`, `PULL`, `MOV`, `IRQ`, `SET` — is binary-compatible at the assembly level. RP1 adds two new per-SM registers (`SMx_DMATX` and `SMx_DMARX`) for DMA request control that don't exist on RP2040. The register layout otherwise mirrors RP2040 datasheet section 3.7, with wider FIFO-level fields in `FSTAT`/`FLEVEL` to accommodate the 8-deep FIFOs.

**The PCIe access model** is the single most important architectural distinction. On RP2040, user code directly writes PIO configuration registers. On RP1, the ARM Cortex-A76 host can only access PIO FIFO read/write ports (at `0x40178000` in RP1's address space, mapped to `0x1F_0017_8000` in BCM2712's 40-bit physical address space via PCIe BAR1). All configuration — loading programs into instruction memory, setting clock dividers, configuring pin mappings — must be proxied through RP1 firmware via a mailbox interface. This adds **≥10 μs latency per configuration operation**.

## Memory map and the PCIe BAR topology

Three PCIe BARs expose RP1's internals to the host:

| BAR | BCM2712 Physical Address | Size | Maps To | Purpose |
|-----|-------------------------|------|---------|---------|
| BAR0 | `0x1F_0041_0000` | 16 KB | PCIe EP config | Endpoint configuration |
| BAR1 | `0x1F_0000_0000` | 4 MB | `0x4000_0000–0x4040_0000` | **Peripheral registers** |
| BAR2 | `0x1F_0040_0000` | 64 KB | `0x2000_0000–0x2001_0000` | Shared SRAM |

The address translation from RP1 peripheral addresses to BCM2712 physical addresses follows: `BCM2712_phys = 0x1F_0000_0000 + (RP1_addr - 0x4000_0000)`. Key peripheral mappings include PIO at `0x1F_0017_8000`, the DMA controller at `0x1F_0018_8000`, and GPIO (IO_BANK0) at `0x1F_000D_0000`.

RP1 supports **RP2040-style atomic register access** through address aliasing: normal read/write at `+0x0000`, atomic XOR at `+0x1000`, atomic SET at `+0x2000`, atomic CLEAR at `+0x3000`. This is critical for avoiding costly PCIe read-modify-write round-trips that would double latency.

Internally, RP1's Cortex-M3 cores access PIO at a private address `0xF000_0000` with single-cycle latency. This private address space is inaccessible over PCIe — it's where the firmware performs all PIO configuration on behalf of host requests.

```
BCM2712 (SoC)                              RP1 (Southbridge)
┌──────────────────────┐                   ┌────────────────────────────────┐
│ 4× Cortex-A76        │                   │  2× Cortex-M3 (200 MHz)       │
│ (2.4 GHz)            │                   │   └─ PIO regs @ 0xF000_0000   │
│                      │   PCIe 2.0 x4     │      (single-cycle access)     │
│ LPDDR4X DRAM    ◄────┼───────────────────┼── RP1 DMA (8-ch, AXI)         │
│                      │   ~2 GB/s bw      │      ↕ DREQ handshake         │
│ BCM2712 DMA40        │   ~1 μs latency   │  PIO Block (1 instance)       │
│ (NOT used for PIO)   │                   │   ├─ 4 State Machines          │
│                      │                   │   ├─ TX FIFOs (8-deep each)    │
│                      │  BAR1 (4MB) ──────┤   ├─ RX FIFOs (8-deep each)   │
│                      │                   │   └─ 32-word instr memory      │
│                      │  BAR2 (64KB) ─────┤  Shared SRAM (64 KB)          │
│                      │                   │  28× GPIO (header pins)        │
└──────────────────────┘                   └────────────────────────────────┘
```

## DMA architecture: RP1's internal engine does all the work

A common misconception is that BCM2712's DMA40 controller handles PIO transfers. In fact, **RP1's own 8-channel DMA controller** (a Synopsys DesignWare AXI DMAC at `0x40188000`) performs all PIO FIFO transfers. DREQ signals from PIO FIFOs are wired internally within RP1 — they never cross the PCIe link.

The DREQ assignments for PIO are defined in the kernel header `include/dt-bindings/mfd/rp1.h`:

| DREQ Name | ID | Description |
|-----------|----|-------------|
| `RP1_DMA_PIO_CH0_TX` | 0x38 (56) | SM0 TX FIFO |
| `RP1_DMA_PIO_CH0_RX` | 0x39 (57) | SM0 RX FIFO |
| `RP1_DMA_PIO_CH1_TX` | 0x3A (58) | SM1 TX FIFO |
| `RP1_DMA_PIO_CH1_RX` | 0x3B (59) | SM1 RX FIFO |
| `RP1_DMA_PIO_CH2_TX` | 0x3C (60) | SM2 TX FIFO |
| `RP1_DMA_PIO_CH2_RX` | 0x3D (61) | SM2 RX FIFO |
| `RP1_DMA_PIO_CH3_TX` | 0x3E (62) | SM3 TX FIFO |
| `RP1_DMA_PIO_CH3_RX` | 0x3F (63) | SM3 RX FIFO |

The DesignWare AXI DMAC has **heterogeneous channels**: channels 0 and 1 are "heavy" channels supporting **8-beat bursts** (MSIZE=8), while channels 2–7 support only 4-beat bursts. A key kernel optimization (PR #6994, August 2025) reserves channels 0 and 1 for PIO use, **more than doubling throughput** compared to using lighter channels.

Each PIO state machine has dedicated DMA control registers (RP1-specific, not present on RP2040):

- **`SMx_DMATX`** (offset `0x0E0 + sm×0x20`): Bit 31 = DREQ enable, bits 4:0 = threshold (TX DREQ asserted when FIFO level drops below this value)
- **`SMx_DMARX`** (offset `0x0E4 + sm×0x20`): Bit 31 = DREQ enable, bits 4:0 = threshold (RX DREQ asserted when FIFO level reaches or exceeds this value)

**The FIFO threshold must match the DMA burst size.** A January 2026 bug fix (PR #7190) resolved data corruption caused by the RX threshold being set to 1 while the DMA burst size was 8. The corrected `dmactrl` value `0xC0000108` sets threshold=8 with DREQ enabled. The device tree binding in `rp1.dtsi` declares all eight DMA channels:

```dts
rp1_pio: pio@178000 {
    reg = <0xc0 0x40178000  0x0 0x20>;
    compatible = "raspberrypi,rp1-pio";
    firmware = <&rp1_firmware>;
    dmas = <&rp1_dma RP1_DMA_PIO_CH0_TX>, <&rp1_dma RP1_DMA_PIO_CH0_RX>,
           <&rp1_dma RP1_DMA_PIO_CH1_TX>, <&rp1_dma RP1_DMA_PIO_CH1_RX>,
           <&rp1_dma RP1_DMA_PIO_CH2_TX>, <&rp1_dma RP1_DMA_PIO_CH2_RX>,
           <&rp1_dma RP1_DMA_PIO_CH3_TX>, <&rp1_dma RP1_DMA_PIO_CH3_RX>;
    dma-names = "tx0","rx0","tx1","rx1","tx2","rx2","tx3","rx3";
};
```

The full data path for a TX DMA transfer is: **Host DRAM → BCM2712 PCIe Root Complex → PCIe 2.0 x4 link (Posted Write TLP) → RP1 PCIe Endpoint → AXI fabric → RP1 DMA engine (reads from PCIe outbound window pointing to host DRAM) → APB bridge → PIO TX FIFO**. The RX path reverses this: **PIO RX FIFO → RP1 DMA engine → AXI fabric → PCIe outbound → host DRAM**.

## The software stack: PIOLib, kernel driver, and firmware mailbox

All PIO interaction flows through a multi-layer software stack:

```
Userspace application (piolib API)
    ↓ ioctl(/dev/pio0)
rp1-pio kernel driver (drivers/misc/rp1-pio.c)
    ↓ firmware interface
RP1 mailbox driver → doorbell + shared memory
    ↓ PCIe
RP1 Cortex-M3 firmware (writes PIO config registers at 0xF0000000)
```

**PIOLib** (`sudo apt install piolib`, source at `github.com/raspberrypi/utils/tree/master/piolib`) is the official userspace library that mirrors the Pico SDK's PIO API. Most function calls are proxied as RPC messages to RP1 firmware. The key DMA-related API functions are:

```c
// Configure DMA buffer parameters (must be called before xfer_data)
pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, buf_size_bytes, buf_count);
pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, buf_size_bytes, buf_count);

// Execute a DMA transfer (blocks until complete)
pio_sm_xfer_data(pio, sm, PIO_DIR_TO_SM, data_bytes, tx_buffer);
pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, data_bytes, rx_buffer);

// Manually set DMA control register (threshold, enable)
pio_sm_set_dmactrl(pio, sm, is_tx, ctrl_value);
```

For in-kernel usage, the header `include/linux/pio_rp1.h` provides the same API with `rp1_pio_` prefixed functions. The `pwm-pio-rp1.c` and `rp1_dpi_pio.c` kernel drivers serve as reference implementations.

## PIO register map and FIFO configuration details

The RP1 PIO register map (reverse-engineered by MichaelBell, confirmed compatible with RP2040 section 3.7) resides at base `0xF000_0000` from the M3 cores. Offsets for key registers:

| Offset | Register | Description |
|--------|----------|-------------|
| 0x000 | CTRL | SM enable/restart bits |
| 0x004 | FSTAT | FIFO status (full/empty per SM, 4-bit level fields) |
| 0x008 | FDEBUG | Sticky error flags: TXSTALL, TXOVER, RXUNDER, RXSTALL |
| 0x00C | FLEVEL | 4-bit FIFO level per SM (0–8 on RP1) |
| 0x010–0x01C | TXF0–TXF3 | TX FIFO write ports (PCIe-accessible) |
| 0x020–0x02C | RXF0–RXF3 | RX FIFO read ports (PCIe-accessible) |
| 0x048–0x0C4 | INSTR_MEM0–31 | Instruction memory (write-only, 16-bit entries) |
| 0x0C8 + sm×0x20 | SMx_CLKDIV | Clock divider: [31:16]=INT, [15:8]=FRAC |
| 0x0CC + sm×0x20 | SMx_EXECCTRL | Execution control (wrap boundaries, side-set config) |
| 0x0D0 + sm×0x20 | SMx_SHIFTCTRL | Shift control (autopush/pull, thresholds, FIFO join) |
| 0x0D4 + sm×0x20 | SMx_ADDR | Current program counter (read-only) |
| 0x0D8 + sm×0x20 | SMx_INSTR | Current/forced instruction |
| 0x0DC + sm×0x20 | SMx_PINCTRL | Pin mapping |
| 0x0E0 + sm×0x20 | **SMx_DMATX** | DMA TX control (RP1-only) |
| 0x0E4 + sm×0x20 | **SMx_DMARX** | DMA RX control (RP1-only) |

The **SHIFTCTRL** register controls autopush/autopull behavior critical for DMA throughput:

| Bits | Field | Function |
|------|-------|----------|
| 31 | FJOIN_RX | Join both FIFOs for 16-deep RX |
| 30 | FJOIN_TX | Join both FIFOs for 16-deep TX |
| 29 | OUT_SHIFTDIR | 0=left, 1=right |
| 28:25 | PULL_THRESH | Autopull threshold (0 means 32) |
| 24 | AUTOPULL | Enable automatic OSR refill from TX FIFO |
| 23 | IN_SHIFTDIR | 0=left, 1=right |
| 22:18 | PUSH_THRESH | Autopush threshold (0 means 32) |
| 17 | AUTOPUSH | Enable automatic ISR push to RX FIFO |

For maximum throughput, enable autopush and autopull with **threshold=32** and **clock divider=1.0** (full 200 MHz). Joining FIFOs doubles depth to 16 entries in one direction but eliminates the other direction entirely — incompatible with full-duplex operation.

## Constructing a full-duplex DMA loopback

No official loopback example exists in the Raspberry Pi repositories, but one can be constructed using PIO's internal data routing (no GPIO pins required). The PIO `MOV` instruction supports bitwise NOT (`~src`) and bit-reversal (`::src`), enabling data transformation without external connections:

```asm
; loopback.pio — reads TX FIFO, inverts all bits, writes to RX FIFO
; 3 cycles per 32-bit word → 66.7 Mwords/s at 200 MHz internally
.program loopback
.wrap_target
    out x, 32           ; autopull: TX FIFO → OSR → scratch X
    mov y, ~x           ; bitwise NOT of X → Y
    in y, 32            ; autopush: Y → ISR → RX FIFO
.wrap
```

The complete userspace program using PIOLib:

```c
#include "piolib.h"
#include "loopback.pio.h"   // generated by pioasm

int main(void) {
    PIO pio = pio0;
    int sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &loopback_program);

    // Configure state machine
    pio_sm_config c = loopback_program_get_default_config(offset);
    sm_config_set_out_shift(&c, false, true, 32);  // left-shift, autopull, thresh=32
    sm_config_set_in_shift(&c, false, true, 32);   // left-shift, autopush, thresh=32
    sm_config_set_clkdiv(&c, 1.0f);                // full 200 MHz
    // Do NOT join FIFOs — need both TX and RX (8-deep each)
    pio_sm_init(pio, sm, offset, &c);

    #define XFER_SIZE 1024
    uint32_t tx_buf[XFER_SIZE / 4];
    uint32_t rx_buf[XFER_SIZE / 4];
    for (int i = 0; i < XFER_SIZE / 4; i++) tx_buf[i] = i;

    // Configure DMA for both directions
    pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, XFER_SIZE, 1);
    pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, XFER_SIZE, 1);

    // Optionally fix FIFO threshold for heavy DMA channels
    pio_sm_set_dmactrl(pio, sm, true,  0xC0000108);  // TX threshold=8
    pio_sm_set_dmactrl(pio, sm, false, 0xC0000108);  // RX threshold=8

    pio_sm_set_enabled(pio, sm, true);

    // Start both DMA transfers (TX feeds PIO, PIO inverts, RX captures)
    pio_sm_xfer_data(pio, sm, PIO_DIR_TO_SM, XFER_SIZE, tx_buf);
    pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, XFER_SIZE, rx_buf);

    // Verify: each rx_buf[i] should equal ~tx_buf[i]
    for (int i = 0; i < XFER_SIZE / 4; i++) {
        if (rx_buf[i] != ~tx_buf[i]) {
            printf("Mismatch at %d: expected 0x%08X, got 0x%08X\n",
                   i, ~tx_buf[i], rx_buf[i]);
        }
    }
    printf("Loopback verification complete\n");
    return 0;
}
```

The data path exercised here: **ARM DRAM → PCIe → RP1 DMA → PIO TX FIFO → state machine (bit inversion) → PIO RX FIFO → RP1 DMA → PCIe → ARM DRAM**. Note that `pio_sm_xfer_data` is currently a blocking call — for true simultaneous full-duplex, you would need to either use separate threads or modify the kernel driver to support asynchronous transfers. An alternative approach uses a bit-reversal transformation (`mov y, ::x`) or shift operations for more complex processing.

## Measured performance and the 10–27 MB/s reality

The gap between PIO's internal bandwidth and achievable DMA throughput is stark. At 200 MHz with a 1-instruction autopull loop, PIO can internally consume **800 MB/s** of data. But the DMA path throttles this severely.

| Transfer Method | Measured Throughput | Notes |
|----------------|-------------------|-------|
| `pio_sm_get_blocking()` (no DMA) | **~250 KB/s** | Each call round-trips through PCIe + mailbox |
| PIOLib DMA (initial, default burst) | **~5–10 MB/s** | Pre-optimization, 4-beat DMA bursts |
| PIOLib DMA (heavy channels, burst=8) | **~27 MB/s** | After PR #6994 reserving channels 0/1 |
| Direct M3 core access (unofficial) | **~66 MB/s** | cleverca22's experiments; sample loss above 20 Mbit/s (2.5 MB/s) RX |
| 16-bit DAC output (fsphil) | **~10.8 MB/s** | 5.4 MS/s × 16-bit, with visible FIFO starvation pauses |

The fundamental bottleneck was identified by Raspberry Pi engineer jdb: **"each DMA handshake cycle takes of the order of 70 bus cycles to complete"**. The 32-bit PIO FIFO sits on an APB bus, but the DMA engine has a 128-bit internal data bus — effectively wasting 75% of DMA bandwidth on padding. The datasheet-specified per-channel DMA read bandwidth of **500–600 Mbit/s (megabits/s)** yields roughly `600 Mbit/s ÷ 4 (bus width waste) ÷ 8 (bits per byte) ≈ 18.75 MB/s (megabytes/s)` effective for 32-bit peripheral transfers, aligning with the measured ~10–27 MB/s range.

To reach or exceed **10 MB/s**, the minimum PIO-side configuration is straightforward: any PIO clock above ~2.5 MHz with a single-instruction autopull program saturates the DMA path. The bottleneck is entirely on the DMA/PCIe side. Optimization strategies that help:

- **Use DMA channels 0 or 1** (8-beat bursts): achieved automatically since PR #6994
- **Match FIFO threshold to burst size**: set `dmactrl` to `0xC0000108` for heavy channels
- **Use large transfer buffers**: reduces per-transfer setup overhead
- **Avoid FIFO joining in full-duplex mode**: keep 8-deep TX + 8-deep RX rather than 16-deep one-way
- **Future path — M3 core bounce buffer**: The M3 has single-cycle FIFO access and can theoretically copy PIO data into shared SRAM at full speed, then DMA from SRAM (wider bus) to host memory, eliminating the 70-cycle handshake penalty

## Key repositories, documentation, and community resources

The ecosystem spans official Raspberry Pi repositories, community reverse-engineering efforts, and practical application projects:

- **PIOLib (userspace SDK)**: `github.com/raspberrypi/utils/tree/master/piolib` — Pico SDK-compatible API wrapping ioctl calls to `/dev/pio0`. Includes WS2812, PWM, quadrature encoder, and DPI sync examples.
- **rp1-pio kernel driver**: `github.com/raspberrypi/linux` at `drivers/misc/rp1-pio.c` (branch `rpi-6.12.y`) — handles DMA allocation, firmware mailbox proxying, and ioctl interface.
- **In-kernel PIO API**: `include/linux/pio_rp1.h` — enables kernel modules to use PIO directly. Reference implementations in `drivers/pwm/pwm-pio-rp1.c` (PWM) and `drivers/gpu/drm/rp1/rp1-dpi/rp1_dpi_pio.c` (display sync).
- **RP1 peripherals datasheet**: `datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf` — covers address maps, GPIO, DMA, UART, SPI, I2C, PCIe endpoint, but **PIO section is notably absent** (listed only in the address table at `0x40178000`).
- **MichaelBell's rp1-hacking**: `github.com/MichaelBell/rp1-hacking` — the primary source for reverse-engineered PIO register documentation (`PIO.md`), including DMACTRL register format and address mappings.
- **librerpi/rp1-lk**: `github.com/librerpi/rp1-lk` — bare-metal RP1 code (Little Kernel) with direct PIO register manipulation examples running on M3 cores.
- **DMA performance PRs**: PR #6994 (heavy channel reservation), PR #7190 (FIFO threshold fix) in `raspberrypi/linux`.
- **Known issues**: `github.com/raspberrypi/utils/issues/116` tracks DMA throughput limitations.
- **Forum discussion hub**: `forums.raspberrypi.com/viewtopic.php?t=390556` is the most active thread on PIO DMA performance, featuring direct responses from Raspberry Pi engineers.

## Conclusion

Achieving high-speed DMA transfers between the ARM host and RP1 PIO is feasible but bounded by RP1's DMA controller architecture rather than PIO itself. The PIO block can internally process data at hundreds of MB/s, but the **DMA handshake overhead** (70 bus cycles), **32-bit APB bus width** mismatch, and **PCIe transaction overhead** create a practical ceiling around **27 MB/s** with current optimizations — comfortably exceeding the 10 MB/s target but far below theoretical maximums. The most impactful optimizations are using heavy DMA channels 0/1 with 8-beat bursts and correctly matching FIFO thresholds to burst sizes via `pio_sm_set_dmactrl()`. Full-duplex operation works by using independent TX/RX DMA channels with separate DREQ signals, but requires unjoined FIFOs (8-deep each). The most promising path to significantly higher throughput is leveraging RP1's spare Cortex-M3 core to bounce FIFO data through shared SRAM — eliminating the DMA handshake bottleneck entirely — but official support for user code on the M3 remains unavailable as of early 2026.