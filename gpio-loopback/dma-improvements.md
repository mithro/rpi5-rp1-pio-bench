# RP1 PIO DMA: Potential Improvements and Optimization Paths

This document surveys all known approaches for improving DMA throughput between
the ARM host and RP1's PIO block on Raspberry Pi 5, from low-hanging kernel
driver changes to running custom firmware on RP1's spare Cortex-M3 core.

All performance numbers are from published measurements by Raspberry Pi
engineers, community members, or our own GPIO loopback benchmark.

---

## Table of Contents

1. [Current Architecture and Bottlenecks](#1-current-architecture-and-bottlenecks)
2. [Already-Applied Kernel Fixes](#2-already-applied-kernel-fixes)
3. [Kernel Driver Improvements](#3-kernel-driver-improvements)
4. [Shared SRAM + Cyclic DMA](#4-shared-sram--cyclic-dma)
5. [M3 Core 1 as PIO Bridge](#5-m3-core-1-as-pio-bridge)
6. [PCIe Tuning](#6-pcie-tuning)
7. [Comparison Matrix](#7-comparison-matrix)
8. [Recommendations](#8-recommendations)
9. [Sources](#9-sources)

---

## 1. Current Architecture and Bottlenecks

### The DMA data path

```
Host SDRAM
  → BCM2712 PCIe Root Complex
    → PCIe 2.0 x4 link (~2 GB/s, ~1 µs latency)
      → RP1 PCIe Endpoint
        → RP1 AXI Fabric (128-bit, ~200 MHz, ~3.2 GB/s raw)
          → RP1 DMA Controller (Synopsys DesignWare AXI DMAC, 8 channels)
            → APB Bridge (128-bit → 32-bit narrowing)
              → PIO TX/RX FIFO (32-bit, 8-deep per SM)
```

### Where the bandwidth is lost

| Stage | Bandwidth | Bottleneck? |
|-------|-----------|-------------|
| PCIe 2.0 x4 | ~2 GB/s | No (200× headroom) |
| AXI fabric | ~3.2 GB/s | No |
| DMA controller | ~45 MB/s per channel (burst=8) | **Partial** — 70-cycle handshake overhead |
| APB bridge | 32-bit interface | **Yes** — wastes 75% of 128-bit bus |
| PIO FIFO consumption | Up to 800 MB/s internal | No |
| Bounce buffer copy | CPU-limited | **Partial** — `copy_from_user`/`copy_to_user` |
| Kernel ioctl overhead | ~10 µs per call | **Partial** — for small transfers |

### The fundamental constraint

Raspberry Pi engineer jdb:
> "Each DMA handshake cycle takes of the order of 70 bus cycles to complete."

At the AXI clock rate, 70 cycles ≈ 350-700 ns per handshake. With 8-beat
bursts (32 bytes), this gives ~45 MB/s theoretical per channel. **The DMA
handshake overhead is the primary hardware bottleneck that cannot be fixed
in software** — only bypassed.

### Current measured throughput

| Configuration | Throughput | Source |
|---|---|---|
| Pre-optimization (default burst) | ~10.75 MB/s | Jeff Epler (Adafruit), Issue #116 |
| After PR #6994 (heavy channels, burst=8) | ~27 MB/s | pelwell, PR #6994 |
| This benchmark (internal loopback, concurrent TX+RX) | ~42 MB/s | benchmark/ |
| Direct M3 core access (unofficial) | ~66 MB/s | cleverca22 |
| Theoretical DMA ceiling (per RP1 datasheet) | 62-75 MB/s | RP1 datasheet |

---

## 2. Already-Applied Kernel Fixes

These are merged/available and should be applied before any further work.

### PR #6994: Heavy DMA Channel Reservation (Aug 2025)

**What it does:**
- Reserves DMA channels 0 and 1 ("heavy" channels with MSIZE=8 support)
  exclusively for PIO
- Increases DMA burst size from 4 to 8 beats (16 → 32 bytes per burst)
- Adds per-channel burst length configuration via `snps,axi-max-burst-len`
  array in device tree

**Impact:** Throughput jumped from ~10 MB/s to ~27 MB/s (2.5× improvement).

**How to apply:** `sudo rpi-update` on recent kernels, or
`sudo rpi-update pulls/6994` for the specific PR.

### PR #7190: FIFO Threshold Fix (Jan 2026)

**What it does:**
- Sets FIFO DREQ threshold equal to DMA burst size (was mismatched)
- Fixes data corruption caused by DMA reading more words than available
  (underflow) or writing more than space exists (overflow)
- Increases DMA completion timeout from 1s to 10s

**Impact:** Fixes data corruption after ~16 words, prevents false ETIMEDOUT
on slow transfers.

### Default DMACTRL after both PRs

```c
// The kernel now sets (in rp1_pio_sm_config_xfer):
#define RP1_PIO_DMACTRL_DEFAULT  0x80000104  // DREQ_EN, priority=2, threshold=4

// For TX: threshold = burst_size (usually 8)
// For RX: threshold = burst_size (usually 8)
// But the user can override via pio_sm_set_dmactrl()
```

---

## 3. Kernel Driver Improvements

These are changes to `drivers/misc/rp1-pio.c` and its DMA usage that don't
require firmware modification.

### 3A. Larger Bounce Buffers

**Current state:**
```c
#define DMA_BOUNCE_BUFFER_SIZE   0x1000  // 4 KB per buffer
#define DMA_BOUNCE_BUFFER_COUNT  4       // 4 buffers in ring
```

The buffer size is already configurable via `pio_sm_config_xfer(pio, sm, dir,
buf_size, buf_count)`. Users can set `buf_size` up to 65,532 bytes.

**What to do:** Use `pio_sm_config_xfer(pio, sm, dir, 65536, 4)` instead of
the default 4096. This reduces per-buffer DMA setup overhead by 16×.

**Caveat:** Kernel 6.12 had a regression where transfers > 65,532 bytes
failed with ETIMEDOUT (fixed by PR #6729 in the DW AXI DMAC driver). Ensure
this fix is present.

**Effort:** Trivial (userspace change only).
**Expected impact:** Moderate (~2-3× for small transfers that were setup-limited).

### 3B. Eliminate Bounce Buffers (Zero-Copy DMA)

**Current flow:**
```
copy_from_user(bounce_buf, user_buf, 4096)
→ dmaengine_prep_slave_sg(dma_chan, bounce_sgl, 1, ...)
→ DMA: bounce_buf → PIO FIFO
```

**Proposed flow:**
```
pages = pin_user_pages(user_buf, nr_pages)
dma_map_sg(dev, user_sgl, nr_pages, DMA_TO_DEVICE)
→ dmaengine_prep_slave_sg(dma_chan, user_sgl, nr_pages, ...)
→ DMA: user_buf → PIO FIFO (zero-copy)
```

**The in-kernel API already supports this.** `rp1_pio_sm_xfer_data()` accepts
a `dma_addr` parameter — if non-zero, it uses the caller's DMA address
directly without bounce buffers. The missing piece is exposing this to
userspace.

**Implementation:**
1. Add a new ioctl `PIO_IOC_SM_XFER_DATA_DMABUF` that accepts a userspace
   pointer + length
2. In the kernel, use `pin_user_pages_fast()` + `dma_map_sg()` to create
   scatter-gather lists pointing directly at user pages
3. Submit the entire SG list as one DMA operation

**Effort:** Medium (kernel driver change, ~200 lines).
**Expected impact:** Moderate — eliminates one memcpy per buffer but doesn't
address the fundamental DMA-to-FIFO bottleneck.

### 3C. Multi-Descriptor Scatter-Gather

**Current:** Each bounce buffer is submitted as a **separate** DMA operation
with its own `dmaengine_prep_slave_sg()` call, descriptor allocation,
submission, and completion callback.

**Proposed:** Submit all 4 bounce buffers as a single multi-element
scatter-gather list. The DesignWare AXI DMAC uses Linked List Items (LLI)
internally and can chain descriptors:

```c
// Current: 4 separate DMA operations
for (i = 0; i < 4; i++) {
    desc = dmaengine_prep_slave_sg(chan, &bufs[i].sgl, 1, ...);
    dmaengine_submit(desc);
}

// Proposed: 1 DMA operation with 4-element SG list
sg_init_table(sgl, 4);
for (i = 0; i < 4; i++)
    sg_set_buf(&sgl[i], bufs[i].buf, bufs[i].size);
desc = dmaengine_prep_slave_sg(chan, sgl, 4, ...);
dmaengine_submit(desc);
```

**Effort:** Low-Medium (kernel driver change, ~50 lines).
**Expected impact:** Low-Moderate — fewer interrupts and less CPU overhead,
but the bottleneck is DMA handshake speed, not descriptor management.

### 3D. Async/Non-Blocking Transfers

**Current:** `pio_sm_xfer_data()` blocks until all data is transferred. The
ioctl returns only after the final DMA completion.

**The in-kernel path already supports async** via a callback parameter:
```c
int rp1_pio_sm_xfer_data(struct rp1_pio_client *client, uint sm, uint dir,
                         uint data_bytes, void *data, dma_addr_t dma_addr,
                         void (*callback)(void *param), void *param);
```

**Proposed userspace API:**
```c
// New ioctls:
pio_sm_xfer_start(pio, sm, dir, size, buf);  // non-blocking, returns token
pio_sm_xfer_poll(pio, token);                // check completion
pio_sm_xfer_wait(pio, token);                // block until done
```

This would enable overlapping computation with DMA and driving multiple SMs
simultaneously from a single thread.

**Effort:** Medium (kernel driver + piolib changes).
**Expected impact:** Moderate for multi-SM workloads; low for single-SM.

---

## 4. Shared SRAM + Cyclic DMA

This is the approach explicitly endorsed by pelwell (Phil Elwell, the RP1 PIO
driver author):

> "To get even higher throughputs would require **moving the DMA buffers to
> shared SRAM**, and probably configuring the DMA controller for **cyclic
> usage**."
>
> — pelwell, [raspberrypi/utils Issue #116](https://github.com/raspberrypi/utils/issues/116)

### The idea

Instead of DMA transferring data from **host memory** (across PCIe) to the
PIO FIFO, use RP1's **64 KB shared SRAM** (`0x20000000`, accessible from both
host and M3 cores via PCIe BAR2) as an intermediate buffer:

```
Current:
  Host DRAM ──PCIe──→ RP1 DMA ──APB──→ PIO FIFO
                      ^^^^^^^^^^^^^^
                      70-cycle handshake per burst

Proposed:
  Host DRAM ──PCIe──→ RP1 Shared SRAM (64 KB)
                          ↓
                      RP1 DMA ──local──→ PIO FIFO
                      ^^^^^^^^^^^^^^^^
                      Much shorter path (no PCIe per burst)
```

### Why it helps

The current DMA path crosses PCIe for **every burst**. A burst of 8 words
(32 bytes) at ~1 µs PCIe latency means each burst carries a PCIe overhead.
With SRAM buffers:

- DMA reads from local SRAM instead of host memory — no PCIe latency per burst
- The SRAM is on RP1's internal bus, reducing path length
- PCIe is only used to bulk-fill the SRAM ring buffer, amortizing latency

### Cyclic DMA

The DesignWare AXI DMAC supports cyclic DMA via `dmaengine_prep_dma_cyclic()`:

```c
// Instead of one-shot scatter-gather:
desc = dmaengine_prep_slave_sg(chan, sgl, nents, ...);

// Use cyclic mode:
desc = dmaengine_prep_dma_cyclic(chan,
    sram_dma_addr,      // RP1 shared SRAM physical address
    ring_buffer_size,    // e.g., 32 KB ring in SRAM
    period_size,         // e.g., 4 KB per period
    DMA_MEM_TO_DEV,      // SRAM → PIO FIFO
    DMA_PREP_INTERRUPT); // interrupt per period
```

Cyclic DMA loops the descriptor chain, so the DMA controller continuously
reads from the SRAM ring buffer and writes to the PIO FIFO. The host CPU
only needs to refill the SRAM ring buffer (via PCIe) faster than DMA drains
it.

### Implementation sketch

1. **Reserve a portion of shared SRAM** (e.g., 32 KB of the 64 KB) for PIO
   DMA buffers. The rest stays available for firmware use.

2. **Map the SRAM into kernel space** via PCIe BAR2 (`0x1F_0040_0000` in
   BCM2712's physical space → `0x20000000` in RP1's space).

3. **Configure cyclic DMA** from SRAM to PIO FIFO. The DMA runs continuously,
   draining the ring buffer.

4. **Host-side producer loop:** The host CPU writes data into the SRAM ring
   buffer via PCIe (this is a simple memcpy to the BAR2 mapping). A
   producer/consumer index in SRAM coordinates with the DMA consumer.

5. **Period completion interrupts** advance the consumer index and wake the
   host to fill more data.

### Constraints

- **Only 64 KB shared SRAM total.** Core 0's firmware uses some of it.
  Partitioning must be coordinated carefully.
- **SRAM organisation:** 4 banks of 4 KB, 32-bit data width. Bank contention
  between DMA reads and PCIe writes could reduce effective bandwidth.
- **No official API** for SRAM partitioning. Requires kernel driver changes
  and possibly firmware awareness.

### Expected impact

pelwell's early test with burst=16 showed **~50 MB/s** (with kernel warnings).
SRAM buffers + cyclic DMA could plausibly reach **50-80 MB/s** by eliminating
the PCIe-per-burst overhead.

**Effort:** High (kernel driver + device tree + SRAM coordination).

---

## 5. M3 Core 1 as PIO Bridge

This is the highest-performance approach, proven by cleverca22 to achieve
**~66 MB/s** — close to the theoretical DMA ceiling.

### Architecture

```
Current path (all DMA):
  Host Memory ←─PCIe─→ [RP1 DMA] ←─AXI/APB─→ [PIO FIFOs]
                                    ^^^^^^^^
                                    70-cycle handshake + APB narrowing

M3 bridge path:
  Host Memory ←─PCIe─→ [RP1 DMA] ←─AXI─→ [Shared SRAM (64KB)]
                                               ↑↓ (single-cycle, 32-bit)
                                          [M3 Core 1]
                                               ↑↓ (single-cycle, 32-bit)
                                          [PIO FIFOs @ 0xF0000000]
```

### Why it's faster

The M3 core has **single-cycle access** to PIO FIFOs at address `0xF0000000`.
This is fundamentally faster than the DMA controller's path through the APB
bridge:

| Access method | Latency per 32-bit word |
|---|---|
| DMA handshake → APB → FIFO | ~70 bus cycles (~350-700 ns) |
| M3 register write → FIFO | 1 cycle (~5 ns) |

A tight M3 loop reading PIO FIFO and writing to SRAM can move data at
**~200 MB/s** (200 MHz × 4 bytes, minus loop overhead → realistic ~100-200 MB/s).
The SRAM-to-host DMA path has much higher bandwidth since SRAM is on the AXI
bus without the APB bottleneck.

### How cleverca22 achieved ~66 MB/s

From [Raspberry Pi Forums](https://forums.raspberrypi.com/viewtopic.php?t=390556):

1. Custom firmware running on an M3 core
2. M3 reads PIO FIFO entries directly (single-cycle)
3. Dumps data into shared SRAM
4. Uses DMA software flow control to transfer from SRAM to host

> "The M3 can read the PIO FIFO and dump the data into SRAM, and once 4 reads
> are done, it can kick the SW flow control in the DMA block and copy from
> SRAM to host RAM."

### Existing code and tools

| Project | What it provides |
|---|---|
| [librerpi/rp1-lk](https://github.com/librerpi/rp1-lk) | Full Little Kernel OS on M3: PIO, DMA, GPIO, UART. Build: `make PROJECT=rp1-test` |
| [G33KatWork/RP1-Reverse-Engineering](https://github.com/G33KatWork/RP1-Reverse-Engineering) | Firmware loading, Core 1 bootstrap, blinky demos |
| [MichaelBell/rp1-hacking](https://github.com/MichaelBell/rp1-hacking) | PIO register map, DMACTRL docs, Core 1 bootstrap via firmware hook |

### Core 1 bootstrap mechanism

From G33KatWork's reverse engineering:

```c
// Core 1 waits for a jump pointer in watchdog scratch registers:
// Set entrypoint (XOR'd with magic):
*(volatile uint32_t*)(0x40154014) = 0x4FF83F2D ^ entrypoint;
// Set stack pointer:
*(volatile uint32_t*)(0x4015401C) = stack_pointer;
// Wake Core 1:
asm volatile("sev");
```

### Key challenges

1. **Shared SRAM coordination:** Core 0's firmware uses shared SRAM
   extensively. Custom Core 1 code must partition SRAM carefully or risk
   corrupting USB/Ethernet/mailbox functionality.

2. **No official support:** Raspberry Pi has not published an API for Core 1.
   All approaches are based on reverse engineering.

3. **Recovery:** Once custom firmware corrupts RP1 state, the only recovery
   path is via the debug UART (between the HDMI ports). "Reloading the
   original firmware doesn't work" — G33KatWork.

4. **Limited resources:** Core 1 has 8 KB instruction RAM + 8 KB data RAM
   (private). Complex bridge firmware must fit in this space.

5. **Firmware version dependence:** The Core 1 bootstrap mechanism depends on
   firmware internals that may change between RP1 EEPROM versions.

### Implementation outline

```
Core 1 firmware (< 8 KB):
  1. Configure PIO SM (load program, set pins, etc.)
  2. Loop:
     a. Wait for PIO RX FIFO non-empty (poll FSTAT or use IRQ)
     b. Read word from PIO RX FIFO (single-cycle at 0xF0000024)
     c. Write word to SRAM ring buffer
     d. If ring buffer period is full:
        - Trigger DMA from SRAM to host via software flow control
        - Or: signal host via mailbox/interrupt

Host-side (kernel driver):
  1. Map shared SRAM via PCIe BAR2
  2. Load Core 1 firmware into SRAM
  3. Bootstrap Core 1
  4. Configure cyclic DMA from SRAM ring buffer to host memory
  5. Consume data from host DMA ring buffer
```

**Effort:** Very high (custom firmware + kernel driver + SRAM coordination).
**Expected impact:** ~66 MB/s proven, potentially higher with optimization.

---

## 6. PCIe Tuning

### Is PCIe a bottleneck?

**No.** The PCIe 2.0 x4 link provides ~2 GB/s. Current PIO DMA throughput
is ~42 MB/s (2% utilization). Even at the theoretical DMA ceiling of ~75 MB/s,
PCIe utilization would be ~4%.

### What could be tuned (but probably won't help)

| Parameter | Current | Possible | Impact |
|---|---|---|---|
| Max Payload Size (MPS) | 128 bytes | 256 bytes | Marginal for large transfers |
| Max Read Request Size (MRRS) | 128/256 bytes | 512 bytes | Marginal |
| Read Completion Boundary (RCB) | 64 bytes | 128 bytes | Marginal |
| PCIe Gen | 2.0 (fixed) | Cannot change (internal link) | None |
| Lane width | x4 (fixed) | Cannot change | None |

pelwell noted that "BCM2712 has a mode where you make it ignore the completion
boundary and return packets up to MPS on the RP1 link" — but this is only
relevant if PCIe bandwidth became the bottleneck, which it won't.

**Recommendation:** Don't pursue PCIe tuning. The bottleneck is inside RP1.

---

## 7. Comparison Matrix

| Approach | Throughput | Effort | Risk | Maturity |
|---|---|---|---|---|
| **Baseline (stock kernel)** | ~10 MB/s | — | — | Shipping |
| **PR #6994 + #7190** | ~27 MB/s | Trivial | None | Merged |
| **This benchmark (concurrent TX+RX)** | ~42 MB/s | Done | None | Working |
| **Larger bounce buffers (64 KB)** | ~42+ MB/s | Trivial | Low | Configurable now |
| **Multi-descriptor scatter-gather** | ~45 MB/s | Low-Med | Low | Not attempted |
| **Zero-copy DMA (eliminate bounce)** | ~45 MB/s | Medium | Medium | In-kernel path exists |
| **Async transfer ioctls** | ~42 MB/s (better latency) | Medium | Low | In-kernel callback exists |
| **Shared SRAM + cyclic DMA** | ~50-80 MB/s | High | Medium | Endorsed by pelwell |
| **M3 Core 1 bridge** | ~66+ MB/s | Very High | High | Proven by cleverca22 |
| **M3 bridge + cyclic DMA** | ~100+ MB/s | Very High | High | Theoretical |
| **PCIe tuning** | ~42 MB/s (no change) | Low | Low | Not a bottleneck |

### Impact vs. effort visualisation

```
Throughput
  100+ │                                          ● M3 + cyclic (theoretical)
       │
   80  │                              ● SRAM + cyclic
       │                                      ● M3 bridge (proven)
   60  │
       │
   42  │──●── Current best (concurrent TX+RX) ─────── ● Zero-copy ─── ● SG batch
       │
   27  │── ● PR #6994+#7190 ────────────────────────────────────────────────────
       │
   10  │── ● Stock kernel ──────────────────────────────────────────────────────
       └──────────────────────────────────────────────────────────────────────→
         Trivial    Low      Medium     High      Very High        Effort
```

---

## 8. Recommendations

### Tier 1: Do now (trivial, no risk)

1. **Ensure PR #6994 and #7190 are applied.** These are prerequisites for
   everything else. `sudo rpi-update` on a recent kernel should include both.

2. **Use large bounce buffers.** In your benchmark/application code:
   ```c
   pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, 65536, 4);
   pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, 65536, 4);
   ```

### Tier 2: Worth investigating (medium effort, medium impact)

3. **Multi-element scatter-gather submission.** Modify the kernel driver to
   submit all 4 bounce buffers as one SG list instead of 4 separate DMA
   operations. Reduces interrupt overhead and CPU involvement.

4. **Async transfer API.** Expose the existing in-kernel async callback
   mechanism to userspace via new ioctls. Enables overlapping computation
   with DMA.

### Tier 3: The big win (high effort, high impact)

5. **Shared SRAM + cyclic DMA.** This is the optimization path pelwell himself
   identified. It eliminates PCIe round-trips from the per-burst DMA path.
   Requires kernel driver changes, device tree modifications, and careful
   SRAM partitioning to avoid breaking Core 0's firmware.

### Tier 4: Maximum performance (very high effort, proven viable)

6. **M3 Core 1 bridge firmware.** Achieves ~66 MB/s (proven). Requires custom
   bare-metal firmware, SRAM partitioning, and has no official support.
   The [librerpi/rp1-lk](https://github.com/librerpi/rp1-lk) project provides
   a solid starting point. Main risks: firmware version dependence, potential
   to brick RP1 (recoverable via debug UART only).

### Don't bother

7. **PCIe tuning.** Not the bottleneck. Would need 10-20× improvement in
   RP1-internal throughput before PCIe matters.

---

## 9. Sources

### Kernel PRs
- [PR #6994: Improve PIO DMA performance](https://github.com/raspberrypi/linux/pull/6994) — Heavy channel reservation, burst=8
- [PR #7190: More RP1 PIO DMA tweaks](https://github.com/raspberrypi/linux/pull/7190) — FIFO threshold fix, 10s timeout
- [PR #6470: RP1 PIO support](https://github.com/raspberrypi/linux/pull/6470) — Original driver submission
- [PR #6729: DW AXI DMAC fix](https://github.com/raspberrypi/linux/pull/6729) — Fix for large transfer regression

### GitHub Issues
- [raspberrypi/utils #116: data transfers slower than expected](https://github.com/raspberrypi/utils/issues/116) — The 10 MB/s wall discussion
- [raspberrypi/utils #123: large transfer regression](https://github.com/raspberrypi/utils/issues/123) — 6.12 kernel regression

### Community Projects
- [librerpi/rp1-lk](https://github.com/librerpi/rp1-lk) — Bare-metal Little Kernel on RP1 M3 cores (PIO, DMA, GPIO)
- [G33KatWork/RP1-Reverse-Engineering](https://github.com/G33KatWork/RP1-Reverse-Engineering) — Firmware loading, Core 1 bootstrap
- [MichaelBell/rp1-hacking](https://github.com/MichaelBell/rp1-hacking) — PIO register map, DMACTRL docs, Core 1 hooks

### Forum Discussions
- [RP1 PIO DMA speed unexpectedly slow](https://forums.raspberrypi.com/viewtopic.php?t=390556) — cleverca22's ~66 MB/s experiments
- [Pi 5 and RP1 M3 cores](https://forums.raspberrypi.com/viewtopic.php?t=358827) — Core 1 availability
- [Using RP1 and PIO on Raspberry Pi 5](https://forums.raspberrypi.com/viewtopic.php?t=363644) — General PIO discussion
- [RP1 Documentation](https://forums.raspberrypi.com/viewtopic.php?t=357292) — Register maps, architecture

### Official Documentation
- [RP1 Peripherals Datasheet](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf) — Address maps, DMA, GPIO (no PIO section)
- [PIOLib announcement](https://www.raspberrypi.com/news/piolib-a-userspace-library-for-pio-control/) — Mailbox architecture
- [Synopsys DW_axi_dmac](https://www.synopsys.com/dw/ipdir.php?c=DW_axi_dmac) — DMA controller IP documentation

### Driver Source
- [rp1-pio.c](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/misc/rp1-pio.c) — Main PIO driver
- [dw-axi-dmac-platform.c](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/dma/dw-axi-dmac/dw-axi-dmac-platform.c) — DMA controller driver
- [piolib](https://github.com/raspberrypi/utils/tree/master/piolib) — Userspace library
