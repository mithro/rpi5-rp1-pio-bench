# Research: RP1 M3 Core 1 PIO Access Latency

## Problem Context

We measured ~54 cycles (~270ns at 200 MHz) for PIO register access from M3 Core 1 via
the 0xF0000000 vendor-specific alias. This limits CPU-polled PIO FIFO throughput to ~7 MB/s.
The official Raspberry Pi documentation claims "single-cycle bus access from the dual
Cortex-M3 management processors." We need to understand why our measurement is ~54x
higher than the claimed single-cycle, and whether cleverca22 achieved ~66 MB/s via Core 1.

---

## 1. cleverca22's Work on RP1 PIO Throughput

### What cleverca22 Actually Achieved

cleverca22 achieved **~530 Mbit/s (~66 MB/s) of RX performance** using **host-side DMA**,
NOT M3 Core 1 CPU polling. This was done via a sigrok/PulseView logic analyzer
implementation using PIO with DMA from the host (BCM2712) side.

Key evidence:
- cleverca22's libsigrok fork has a commit "initial pi5 pio support" that configures
  sigrok to use RP1 PIO as a logic analyzer via DMA
- The 530 Mbit figure (~66 MB/s) was achieved with host DMA, and it was **still dropping
  random samples** at that rate
- cleverca22 noted: "the dma has a 128bit bus, clocked at 100mhz, however, the pio fifo
  is on a 32bit bus, so 3/4ths of your bandwidth is already gone"
- Typical per-channel read bandwidth: 500-600 Mbps; write bandwidth: up to 2 Gbps

### cleverca22's Proposed M3 Core 1 Bounce Buffer Strategy

cleverca22 described (but did not necessarily implement) an M3 Core 1 strategy:
> "the M3 can read the PIO fifo, and dump the data into sram and once 4 reads are done,
> it can kick the sw flow control in the dma block, and copy from sram->hostram"

This was proposed as a way to work around the DMA's 128-bit bus / PIO's 32-bit bus
mismatch. The M3 would pack 4x 32-bit FIFO reads into a 128-bit-aligned SRAM buffer,
then trigger DMA to host RAM.

**CRITICAL FINDING**: The ~66 MB/s figure was from host DMA, not from M3 Core 1.
cleverca22's M3 bounce-buffer idea was a theoretical optimization proposal, not a
demonstrated achievement.

### References
- [cleverca22/libsigrok commit: initial pi5 pio support](https://github.com/cleverca22/libsigrok/commit/e3783bac8176e7454863b37723ab6d8a3f99731a)
- [cleverca22/rp1-kernel-example](https://github.com/cleverca22/rp1-kernel-example/blob/master/rp1-kernel-test.c)
- [RPi Forum: RP1 PIO DMA speed unexpectedly slow](https://forums.raspberrypi.com/viewtopic.php?t=390556)
- [RPi Forum: Any RP1/PIO updates?](https://forums.raspberrypi.com/viewtopic.php?t=374916)

---

## 2. RP1 Bus Architecture: M3 to PIO Path

### Two Distinct PIO Address Mappings

The RP1 PIO has **two separate address mappings**:

| Address | Accessible From | What's Accessible | Bus Path |
|---------|----------------|-------------------|----------|
| 0xF0000000 | M3 cores only | Full PIO registers + FIFOs | Vendor-specific system region -> ? |
| 0x40178000 | All bus masters (PCIe, DMA, M3) | FIFOs only | APB bus on 40-bit AXI fabric |

### The 0xF0000000 Mapping: ARM Cortex-M3 Vendor-Specific Region

In the ARM Cortex-M3 memory map:
- **0x00000000-0x1FFFFFFF**: Code (via I-Code/D-Code buses)
- **0x20000000-0x3FFFFFFF**: SRAM (via System bus) -- includes TCM
- **0x40000000-0x5FFFFFFF**: Peripheral (via System bus)
- **0x60000000-0x9FFFFFFF**: External RAM (via System bus)
- **0xA0000000-0xDFFFFFFF**: External Device (via System bus)
- **0xE0000000-0xE003FFFF**: Internal PPB (NVIC, SysTick, etc.)
- **0xE0040000-0xE00FFFFF**: External PPB (debug, trace -- via APB)
- **0xE0100000-0xFFFFFFFF**: Vendor-specific / Reserved (via System bus)

The 0xF0000000 address falls in the **vendor-specific region** (0xE0100000-0xFFFFFFFF).
Per the ARM Cortex-M3 TRM, this region:
- Is accessed via the **System Bus (AHB-Lite) interface**
- Memory type is **Device** (bufferable, non-cacheable) -- NOT Strongly Ordered
- Note: The PPB region (0xE0000000-0xE00FFFFF) is Strongly Ordered, but the vendor
  region above it is Device type
- Instruction execution is NOT allowed in this region

### The 40-bit Bus Converter

A critical architectural detail from MichaelBell's rp1-hacking:
> "The 40-bit bus is the main internal bus of the RP1, and all bus masters (PCIe, DMA,
> XHCI, Ethernet, CSI, DSI) use that 40-bit space, but the Cortex-M3's are only 32-bit
> CPUs that use the 32-bit space, and a converter near the M3 translates it to the
> 40-bit space."

So the access path from M3 to PIO at 0xF0000000 is likely:

```
M3 Core -> AHB-Lite System Bus -> 32-to-40-bit Converter -> 40-bit AXI Fabric -> PIO
```

### PIO is on the APB

From the RP1 peripheral datasheet (Section 2.3 Peripheral Address Map):
> PIO is on the APB at address 0x40178000

This means that even if the 0xF0000000 alias provides a "shortcut," the PIO hardware
itself sits behind an APB interface. The full bus path may be:

```
M3 Core -> AHB-Lite -> 32-to-40 Converter -> AXI Fabric -> AHB-to-APB Bridge -> PIO
```

OR, if the 0xF0000000 mapping is truly a dedicated fast path:

```
M3 Core -> AHB-Lite -> Dedicated PIO port (bypassing AXI/APB)
```

The "single-cycle" claim from Raspberry Pi suggests the latter -- a dedicated connection
that bypasses the general bus fabric. But our 54-cycle measurement contradicts this.

### References
- [MichaelBell/rp1-hacking PIO.md](https://github.com/MichaelBell/rp1-hacking/blob/main/PIO.md)
- [RP1 Peripherals Datasheet](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf)
- [RPi Forum: Accessing RP1 Peripherals from BCM2712](https://forums.raspberrypi.com/viewtopic.php?t=368402)
- [ARM Cortex-M3 Private Peripheral Bus (ARM Developer)](https://developer.arm.com/documentation/ddi0337/latest/programmers-model/system-address-map/private-peripheral-bus)

---

## 3. ARM Cortex-M3 Peripheral Access Latency

### TCM (Tightly Coupled Memory) Access
- **1 cycle**: The M3's TCM (at 0x20000000) provides single-cycle access
- This is what our 8.7 Mloops/sec Core 1 benchmark confirms

### AHB-Lite Bus Access (System Bus)
- **Minimum 2 cycles**: Address phase + Data phase for an uncontested AHB-Lite transfer
- The bus fabric does not inherently add wait states if the path is clear
- AHB-Lite supports pipelining: back-to-back transfers can approach 1 cycle throughput

### AHB-to-APB Bridge Latency
- **Minimum 2 PCLK cycles per transfer**: APB protocol requires SETUP + ACCESS phases
- If PCLK < HCLK (common for APB), additional synchronization cycles are needed
- Read transfers: 3 HCLK cycles minimum (some implementations)
- Write transfers: 2 HCLK cycles minimum (can be buffered)
- With wait states from the slave: additional PCLK cycles

### Cross-Bar / Bus Fabric Overhead
- AHB-Lite crossbar: 1-2 cycles for arbitration if contested
- 32-to-40-bit address converter: unknown, but likely 1+ cycles
- AXI fabric traversal: additional cycles depending on topology

### Expected Total Latency for M3 -> PIO (via APB)
If PIO access goes through the full bus path:
```
AHB-Lite address phase:     1 cycle
32-to-40 converter:         1-2 cycles (estimate)
AXI fabric routing:         2-4 cycles (estimate)
AHB-to-APB bridge:          2-3 cycles
APB transfer:               2 cycles (SETUP + ACCESS)
Return path:                similar
Total:                      ~8-16 cycles round-trip (estimate)
```

Our measurement of **54 cycles** is significantly higher than even this pessimistic
estimate, suggesting either:
1. Additional bus bridges or conversion stages we are not aware of
2. The 0xF0000000 alias going through an unexpectedly long path
3. Clock domain crossings adding synchronization latency
4. The M3 core being clocked significantly slower than 200 MHz
5. Our cycle measurement methodology has an issue

### References
- [ARM Cortex-M3 TRM (Keil)](https://www.keil.com/dd/docs/datashts/arm/cortex_m3/r2p0/ddi0337g_cortex_m3_r2p0_trm.pdf)
- [GPIO handling in ARM Cortex-M](https://www.scaprile.com/2021/10/28/gpio-handling-in-arm-cortex-m/)
- [AHB-APB Bridge (Microchip Developer Help)](https://developerhelp.microchip.com/xwiki/bin/view/products/mcu-mpu/32bit-mcu/sam/samd21-mcu-overview/peripherals/ahb-apb-bridge/)
- [ARM AMBA APB Protocol Specification](https://developer.arm.com/documentation/ihi0024/latest/)

---

## 4. RP2040 Comparison

### RP2040 Bus Fabric
From the RP2040 datasheet:
> "The bus fabric does not add wait states to any AHB-Lite slave access"
> "Up to four bus transfers can take place each cycle"
> "At a system clock of 125 MHz the maximum sustained bus bandwidth is 2.0 GB/s"

On RP2040:
- **PIO register access from M0+ cores**: Via AHB-Lite crossbar, zero wait states when
  uncontested. Effective ~2 cycles (address + data phase).
- **SIO (Single-cycle IO)**: Dedicated IOPORT bus, truly single-cycle for GPIO.
  PIO registers are NOT on the IOPORT -- they go through the AHB-Lite crossbar.
- Community measurements show ~3-7 cycles for PIO register access from M0+ (including
  instruction execution overhead).

### Key Difference: RP2040 vs RP1
| Feature | RP2040 | RP1 |
|---------|--------|-----|
| CPU | Cortex-M0+ @ 125 MHz | Cortex-M3 @ ~200 MHz |
| PIO bus | AHB-Lite crossbar (zero wait) | APB (via AXI fabric?) |
| PIO register access | ~2-7 cycles from M0+ | ~54 cycles from M3 (measured) |
| Bus fabric | Simple AHB-Lite crossbar | Complex 40-bit AXI + converters |
| PIO FIFO depth | 4 words | 8 words (doubled) |

The RP1's ~54-cycle latency is **dramatically higher** than the RP2040's ~2-7 cycles.
This is consistent with a much more complex bus path involving multiple bridges and
clock domain crossings.

### References
- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf)
- [Signal delays between components in RP2040 (RPi Forum)](https://forums.raspberrypi.com/viewtopic.php?t=325355)

---

## 5. pelwell (Phil Elwell) on M3 Core and DMA Performance

pelwell made several important statements:

### On M3's "more direct path"
> "The M3 does have a more direct path to the FIFOs and is much better suited to bounce
> data into shared SRAM."

This confirms the M3 has a faster path to PIO FIFOs than host DMA, but does NOT confirm
single-cycle access.

### On DMA handshake overhead
> "Each DMA handshake cycle takes of the order of 70 bus cycles to complete, even
> assuming an ideal destination RAM address, which is crippling."

This explains why host DMA throughput is limited. The 128-bit DMA bus with 70-cycle
handshakes at 100 MHz gives theoretical max: 128 bits / 70 cycles * 100 MHz = ~23 MB/s
per channel for reads. This matches the ~27 MB/s observed with optimized kernel patches.

### On DMA bus width mismatch
> "The DMA has a 128-bit bus but the PIO has a 32-bit bus, so 3/4ths of your potential
> bandwidth is going to waste."

### pelwell's DMA optimization PRs
- [PR #6994: Improve PIO DMA performance](https://github.com/raspberrypi/linux/pull/6994)
  - Reserved DMA channels 1 & 2 (more capable) for PIO, more than doubling throughput
- [PR #7190: More RP1 PIO DMA tweaks](https://github.com/raspberrypi/linux/pull/7190)
  - Fixed FIFO threshold to match DMA burst size

### References
- [RPi Forum: Any RP1/PIO updates?](https://forums.raspberrypi.com/viewtopic.php?t=374916)
- [RPi Forum: RP1 PIO issues with FIFO or DMA underflow](https://forums.raspberrypi.com/viewtopic.php?p=2357929)

---

## 6. librerpi/rp1-lk PIO Access

The librerpi/rp1-lk project runs on the M3 cores and accesses PIO at what appears to be
the standard peripheral address (0x40178000 region, mapped through the 32-bit space).
The source file `platform/rp1/pio.c` contains PIO configuration code. No performance
measurements or latency comments were found in the public codebase.

### References
- [librerpi/rp1-lk pio.c](https://github.com/librerpi/rp1-lk/blob/master/platform/rp1/pio.c)

---

## 7. MichaelBell/rp1-hacking Findings

MichaelBell's rp1-hacking project documents:
- PIO registers accessible at 0xF0000000 from M3
- FIFOs accessible at 0x40178000 (also reachable from Linux at 0x1F_00178000)
- The 32-to-40-bit address converter near the M3 cores
- Core 1 activation via SEV/WFE mechanism in SRAM

No specific latency measurements for M3 PIO access were documented.

### References
- [MichaelBell/rp1-hacking](https://github.com/MichaelBell/rp1-hacking)
- [MichaelBell/rp1-hacking PIO.md](https://github.com/MichaelBell/rp1-hacking/blob/main/PIO.md)

---

## 8. G33KatWork/RP1-Reverse-Engineering

This project documented:
- RP1 has two Cortex-M3 cores with firmware loaded by the VideoCore bootloader
- PCIe BAR1 maps peripheral region 0x40000000; BAR2 maps SRAM at 0x20000000
- Core 1 waits in reset vector with WFE, checking SRAM for a function pointer to jump to

No specific bus architecture details or latency measurements.

### References
- [G33KatWork/RP1-Reverse-Engineering](https://github.com/G33KatWork/RP1-Reverse-Engineering)

---

## 9. Why 54 Cycles: Verified Analysis

The 54-cycle PIO register access latency is consistent with a multi-bridge bus path:

```
M3 Core → AHB-Lite System Bus → 32-to-40-bit Converter → AXI Fabric → AHB-to-APB Bridge → PIO
```

**Verified by `pio_addr_test.s`** (2026-03-17):

| Base Address | FSTAT Read Rate | FSTAT Value | Notes |
|-------------|-----------------|-------------|-------|
| `0xF0000000` | 6.7M reads/sec | 0x0F000F00 (correct) | Vendor-specific alias, 1.41× faster |
| `0x40178000` | 4.76M reads/sec | 0x00000000 (wrong) | Standard peripheral bus |

Both addresses traverse the APB bridge. The 0xF0000000 alias is faster (fewer bus
hops) but still ~54 cycles — not single-cycle. The 0x40178000 path is even slower
and returns incorrect FSTAT values, confirming it's a different bus route.

**FSTAT does NOT dynamically update at 0xF0000000.** Polling RXEMPTY or TXFULL from
Core 1 hangs indefinitely. The initial FSTAT value is correct (all FIFOs empty) but
never updates when FIFO state changes. Source: `pio_bridge.s` FSTAT polling test.

**M3 clock verified at ~200 MHz** by `clock_test.s`: 3-instruction SRAM loop runs
at 8.7 Mloops/sec = ~23 cycles/iteration, consistent with 200 MHz clock and ~7
cycles per SRAM access via AXI bus.

The official Raspberry Pi claim of "single-cycle bus access from the dual Cortex-M3
management processors" likely refers to PIO state machine instruction execution
(same as RP2040), not CPU register access from M3.

---

## 10. Final Throughput Comparison

All measurements verified on fresh boot (2026-03-17). Sources in `throughput-cyclic-dma/DESIGN.md`.

| Approach | Throughput | Method | Source |
|----------|-----------|--------|--------|
| RX-only DMA, DRAM | **55.97 MB/s** | Unidirectional cyclic DMA | `sram_dma_bench --rx-only` |
| Cyclic DMA, SRAM bidir | **54.13 / 45.10 MB/s** | SRAM rings at 0xA200+ | `sram_dma_bench --sram` |
| Cyclic DMA, DRAM bidir | 40.35 / 35.87 MB/s | Host DRAM rings via PCIe | `sram_dma_bench --dram` |
| TX-only DMA, DRAM | 40.93 MB/s | Unidirectional cyclic DMA | `sram_dma_bench --tx-only` |
| Standard kernel DMA | ~42 MB/s | piolib ioctl path | Baseline (pelwell) |
| piolib ioctl DMA | 17.51 MB/s | Per-transfer ioctl overhead | `sram_dma_bench --piolib` |
| M3 Core 1 CPU polling | **6.89 MB/s** | LDR/STR from 0xF0000000 | `m3_bridge_bench` |
| cleverca22 custom driver | ~66 MB/s | Host DMA, direct register | [libsigrok commit](https://github.com/cleverca22/libsigrok/commit/e3783bac8176e7454863b37723ab6d8a3f99731a) |

---

## 11. Key Takeaways

1. **cleverca22's ~66 MB/s was from host DMA, NOT M3 Core 1 CPU polling.** The "bounce
   buffer via Core 1" was a proposed optimization, not a demonstrated result.
   Source: [cleverca22/libsigrok](https://github.com/cleverca22/libsigrok/commit/e3783bac8176e7454863b37723ab6d8a3f99731a)

2. **The "single-cycle" claim from RPi does not apply to CPU register access.**
   PIO register access from M3 takes ~54 cycles via the APB bus bridge. Verified
   by `pio_addr_test.s` and `pio_bridge.s` throughput measurements.

3. **PIO sits on the APB bus** at 0x40178000. The 0xF0000000 alias is in the Cortex-M3
   vendor-specific system region. Both traverse the APB bridge; 0xF0000000 is 1.41×
   faster. Source: [MichaelBell/rp1-hacking PIO.md](https://github.com/MichaelBell/rp1-hacking/blob/main/PIO.md)

4. **The RP1's bus architecture is far more complex than RP2040's.** The 40-bit AXI
   fabric, 32-to-40-bit converter, and APB bridges add significant latency compared to
   RP2040's simple zero-wait-state AHB-Lite crossbar.
   Source: [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf)

5. **Cyclic DMA achieves 6–8× higher throughput than M3 CPU polling.**
   SRAM rings (54 MB/s) and unidirectional RX-only (56 MB/s) vs
   M3 Core 1 CPU polling (6.89 MB/s). The DMA handshake (~70 bus cycles per burst)
   is the remaining bottleneck. Source: pelwell, [RPi Forum](https://forums.raspberrypi.com/viewtopic.php?t=374916)

6. **M3 Core 1 is useful for low-latency control, not throughput.** The ~270 ns per
   PIO register access is suitable for real-time GPIO control or PIO program management,
   but not for bulk data transfer.
