# RP1 PIO DMA Word Rate Requirements: Theories and Evidence

This document records our current understanding of how the RP1 DMA controller
interacts with PIO FIFO thresholds and autopull/autopush, with a focus on the
empirical discoveries made during development of the GPIO loopback benchmark.

All findings are from experiments on RPi5 (kernel 6.12+, both PR #6994 and
PR #7190 applied) using single-pin GPIO loopback on GPIO5 via Pmod HAT.

---

## 1. The Core Problem

When building a GPIO loopback benchmark — where a single PIO state machine
serialises data out a GPIO pin one bit at a time and reads it back — we
encountered persistent data corruption that depended on DMACTRL threshold
settings and PIO word rate. The solution required understanding the interaction
between three independent mechanisms:

1. **PIO autopull/autopush** — hardware that automatically refills the OSR
   from the TX FIFO (or pushes the ISR to the RX FIFO)
2. **The FIFO DREQ threshold** — the level at which the FIFO asserts a DMA
   request to the RP1 DMA controller
3. **The RP1 DMA controller's burst transfer behaviour** — how many words it
   moves per handshake cycle

The key insight: **these three mechanisms must be coordinated, and their
interaction behaves differently depending on how fast the PIO program
consumes/produces data.**

---

## 2. Background: How RP1 DMA Works

### 2.1 The DMA Data Path

```
Host SDRAM → BCM2712 PCIe Root Complex → PCIe 2.0 x4 → RP1 PCIe Endpoint
  → RP1 AXI Fabric (128-bit, 100 MHz) → RP1 DMA Controller
    → APB Bridge → PIO TX/RX FIFO (32-bit interface)
```

The RP1's DMA controller is a Synopsys DesignWare AXI DMAC with 8 channels.
Channels 0 and 1 are "heavy" channels supporting 8-beat bursts (32 bytes);
channels 2–7 support only 4-beat bursts.

Key constraint from Raspberry Pi engineer jdb:
> "Each DMA handshake cycle takes of the order of 70 bus cycles to complete."

At 100 MHz AXI clock, 70 bus cycles = 700 ns per handshake. With 8-beat bursts
of 32-bit words, each handshake moves 32 bytes, giving a theoretical ceiling
of ~45 MB/s per channel — though measured throughput is ~27 MB/s per direction
(~42 MB/s aggregate for full-duplex).

### 2.2 The DMACTRL Register

Each PIO state machine has two RP1-specific registers not present on RP2040:

- **SMx_DMATX** (offset `0x0E0 + sm×0x20`): Controls TX FIFO → DMA
- **SMx_DMARX** (offset `0x0E4 + sm×0x20`): Controls RX FIFO → DMA

Layout (from MichaelBell's reverse engineering):

| Bits | Field | Description |
|------|-------|-------------|
| 31 | DREQ_EN | Enable DMA request signal |
| 30 | DREQ_STATUS | Current DREQ status (read-only) |
| 11:7 | PRIORITY | DMA priority (lower = faster) |
| 4:0 | THRESHOLD | FIFO level at which DREQ asserts |

For TX: DREQ asserts when FIFO level drops **below** threshold (requesting
refill). For RX: DREQ asserts when FIFO level reaches or **exceeds** threshold
(requesting drain).

### 2.3 The Kernel's Default Configuration

The kernel (post-PR #7190) sets DMACTRL to `0xC0000108`:
- DREQ_EN = 1
- PRIORITY = 2
- THRESHOLD = 8

This matches the DMA burst size of 8 for heavy channels, which is correct for
PIO programs that produce/consume data at or above the DMA refill rate.

---

## 3. Two Operating Regimes

We discovered that PIO programs fall into two fundamentally different DMA
regimes depending on how fast they consume/produce FIFO words:

### 3.1 "Fast" Regime: PIO Word Rate >> DMA Refill Rate

**Example:** The internal loopback benchmark (`benchmark/`)

```asm
.wrap_target
    out x, 32        ; autopull: TX FIFO → OSR → X
    mov y, ~x        ; bitwise NOT
    in y, 32         ; autopush: ISR → RX FIFO
.wrap
```

- 3 PIO cycles per 32-bit word at 200 MHz = **66.7 MW/s**
- Each word is a full 32-bit data word
- DMA can deliver ~27 MB/s ≈ 6.75 MW/s (32-bit words)
- **PIO is starved by DMA**, spending most of its time stalled on autopull

In this regime, the FIFO acts as a simple buffer. The PIO empties/fills it
faster than DMA can refill/drain, so the FIFO oscillates between empty and
being refilled in bursts. DMACTRL threshold=8 works perfectly because:

- TX: DREQ fires when level drops below 8 (i.e., immediately upon any
  consumption), DMA refills 8 words in one burst, PIO consumes them quickly,
  repeat
- RX: DREQ fires when level reaches 8, DMA drains 8 words, repeat

**Measured:** 42 MB/s aggregate, 0 data errors. The default `0xC0000108`
DMACTRL works as designed.

### 3.2 "Slow" Regime: PIO Word Rate ≈ DMA Burst Rate

**Example:** The GPIO loopback benchmark (`gpio-loopback/`)

```asm
.wrap_target
top:
    out x, 1          ; [0] autopull every bit → 1 DMA word consumed
    jmp !x low        ; [1]
    set pins, 1       ; [2] drive HIGH
    nop               ; [3]
    nop               ; [4]
    nop               ; [5]
    in pins, 1        ; [6] autopush every bit → 1 DMA word produced
    jmp top           ; [7]
low:
    set pins, 0       ; [2] drive LOW
    nop               ; [3]
    nop               ; [4]
    nop               ; [5]
    in pins, 1        ; [6]
    nop               ; [7]
.wrap
```

- 8 PIO cycles per bit, each bit consumes/produces one 32-bit DMA word
  (1-bit autopull/autopush with threshold=1)
- Word rate: 200 MHz / 8 = **25 MW/s**
- DMA word delivery rate for heavy channel burst=8: each burst delivers 8
  words and takes ~700 ns → ~11.4 MW/s sustained

**In this regime, PIO and DMA operate at comparable rates.** The FIFO doesn't
simply empty and refill — it reaches a dynamic equilibrium where the fill
level matters critically.

---

## 4. The TX Threshold Problem (Empirically Discovered)

### 4.1 Observation: TX Threshold=4 Causes FIFO Boundary Corruption

With TX DMACTRL threshold=4 (`0x80000104`), we observed consistent data
corruption at positions corresponding to FIFO refill boundaries. The pattern:

```
Position  Expected      Got
────────  ────────      ────────
0-7       (correct)     (correct)
8         0xFFFFFFFF    0xFEFFFFFF   ← single bit wrong
9         0x12345678    0xFE123456   ← shifted/garbled
```

The corruption occurs exactly at the boundary where the first DMA burst ends
and the second begins (word 8, since the 8-deep FIFO was initially filled
with 8 words by the first burst).

### 4.2 Observation: TX Threshold ≤ 2 Works Perfectly

With TX threshold=1 (`0x80000101`) or threshold=2 (`0x80000102`), all data
is transferred correctly:

| TX Threshold | RX Threshold | Result (20 iterations × 7 patterns) |
|:---:|:---:|---|
| 1 | 4 | **140/140 PERFECT** |
| 2 | 4 | **140/140 PERFECT** |
| 4 | 4 | DATA_ERR on all patterns |
| 8 | 4 | DMA_FAIL (ETIMEDOUT) |

### 4.3 Theory: Autopull/DMA FIFO Collision

**Status: THEORY — explains all observations but not proven at register level**

We believe the corruption occurs because of a race condition between PIO
autopull and DMA FIFO refill when the threshold is too high:

1. PIO executes `out x, 1` with autopull threshold=1. Each `out` triggers
   an autopull that reads one word from the TX FIFO.

2. Meanwhile, the DMA controller monitors the TX FIFO level. When it drops
   below the DMACTRL threshold, DMA begins a burst transfer of up to 8 words
   into the FIFO.

3. **The collision:** If the DMA threshold is set high (e.g., 4), the DREQ
   deasserts when the FIFO level reaches 4. But PIO is continuously consuming
   words at 25 MW/s. If PIO consumes a word during the DMA burst — between
   the time the DMA controller checks the level and the time it completes
   the burst — the FIFO state becomes inconsistent.

   Specifically, we hypothesize that the RP1 DMA controller's burst logic
   assumes the FIFO level won't change during a burst. When autopull removes
   a word mid-burst, the DMA may write to a slot that's being simultaneously
   read, causing data corruption.

4. With threshold=1, the DREQ fires when the FIFO drops below 1 (i.e., is
   empty). At this point, PIO is stalled waiting for autopull — it *cannot*
   consume a word during the DMA burst because there are no words to consume.
   This eliminates the race.

5. With threshold=2, the window is small enough that the race doesn't manifest
   at 25 MW/s. At higher PIO word rates, threshold=2 might also fail (untested).

### 4.4 Alternative Theory: DREQ Timing Mismatch

An alternative explanation is simpler: the DREQ threshold doesn't account for
in-flight burst words. With threshold=4:

1. FIFO has 3 words, DREQ asserts
2. DMA begins 8-word burst
3. After 4 words arrive, FIFO level = 7, DREQ deasserts
4. DMA completes remaining 4 words, FIFO level = 8 (minus PIO consumption)
5. No new DREQ until level drops below 4 again

If PIO consumed 4+ words during the burst, the FIFO might underflow before
the next DREQ, causing autopull to stall (data loss, not corruption). But we
observe corruption (wrong bits), not stalls, which points more toward the
concurrent-access theory in §4.3.

### 4.5 Why the "Fast" Regime Doesn't Have This Problem

In the internal loopback benchmark (66.7 MW/s PIO, ~11.4 MW/s DMA), PIO
empties the FIFO almost instantly after each DMA burst. The FIFO spends most
of its time at level 0 (empty), with brief bursts to level 8 when DMA refills
it. There is effectively no overlap between DMA writing and PIO reading
because PIO finishes reading the entire burst before DMA can start the next
one.

The "slow" regime is uniquely problematic because PIO and DMA operate at
similar speeds, creating sustained periods where both are accessing the FIFO
concurrently.

---

## 5. The RX Threshold Problem (Empirically Discovered)

### 5.1 Observation: RX Threshold < 4 Causes Massive Corruption

| TX Threshold | RX Threshold | Result |
|:---:|:---:|---|
| 1 | 4 | **PERFECT** |
| 1 | 2 | Massive corruption |
| 1 | 1 | Massive corruption |
| 2 | 2 | Massive corruption |
| 4 | 1 | Massive corruption |

"Massive corruption" means nearly every word is wrong — not subtle bit errors
but completely garbled data.

### 5.2 Theory: RX FIFO Overflow from Insufficient Drain Rate

**Status: THEORY — explains observations, consistent with DMA architecture**

With RX threshold=4, the DMA controller drains the RX FIFO when it
accumulates 4 words. With the PIO producing at 25 MW/s, the FIFO fills
one word every 40 ns. Four words accumulate in 160 ns. The DMA burst to
drain them takes ~700 ns. During those 700 ns, PIO produces another
~17 words — but the 8-deep FIFO can only hold 8. If DMA doesn't return
fast enough, the FIFO overflows and words are lost.

Wait — that arithmetic suggests even threshold=4 should overflow (700 ns
burst time while PIO produces at 25 MW/s). So why does threshold=4 work?

**Refined theory:** The DMA controller likely pipelines bursts. With
threshold=4, the DREQ fires at level 4, and by the time the DMA burst
completes, the FIFO has accumulated more words. The DMA controller can
immediately start another burst without waiting for a new DREQ assertion
(the level is still above threshold). This pipelining keeps the FIFO
draining continuously.

With threshold=1 or 2, the DREQ fires too frequently, potentially
overwhelming the DMA request arbitration or causing the DMA controller to
issue many small bursts instead of efficiently pipelining. The DesignWare
AXI DMAC has a minimum overhead per handshake (the 70 bus cycles), and
triggering too many handshakes may cause the DMA controller to fall behind.

### 5.3 The Asymmetry Between TX and RX Requirements

This is a key empirical finding:

- **TX needs threshold ≤ 2** (lower is safer)
- **RX needs threshold ≥ 4** (higher is better for drain efficiency)

The requirements are **opposite** because:

- TX DREQ means "FIFO needs refill" — lower threshold means less concurrent
  access between DMA writes and PIO reads
- RX DREQ means "FIFO needs drain" — higher threshold means fewer DMA
  handshakes per unit of data, better amortisation of the 70-cycle overhead

---

## 6. The 25 MW/s Word Rate: Requirement or Coincidence?

### 6.1 Claim in the Codebase

The PIO program comments state:
```
; Word rate = 200 MHz / 8 cycles = 25 MW/s — at the DMA threshold.
```

And `gpio_loopback.c` notes:
```c
/* Uses 1-bit autopull/autopush: each `out x,1` consumes a full DMA word
 * (only MSB used) and each `in pins,1` produces a full DMA word (bit in
 * LSB). This gives a DMA word rate of 25 MW/s (8 cycles per bit at
 * 200 MHz), above the RP1 DMA rate minimum of ~25 MW/s. */
```

### 6.2 What This Actually Means

The "25 MW/s" figure is **not a hard minimum imposed by the DMA controller**.
Rather, it's the rate at which the 1-bit-per-word encoding keeps the DMA word
rate high enough that:

1. The DMA controller has enough work to stay busy and pipeline efficiently
2. The FIFO doesn't overflow between DMA drain cycles (RX side)
3. The FIFO doesn't underflow between DMA refill cycles (TX side)

The actual DMA controller can handle much lower word rates — the internal
loopback benchmark uses 32-bit words at 66.7 MW/s and the same controller
handles ~6.75 MW/s actual throughput. The constraint is not "PIO must produce
at least 25 MW/s" but rather "the DMACTRL thresholds must be chosen to match
the PIO word rate."

### 6.3 What Happens at Lower Word Rates

We did not systematically test this, but:

- At `clkdiv=2.0` (100 MHz PIO clock → 12.5 MW/s word rate), DMA transfers
  consistently failed with ETIMEDOUT (errno=110). This may be because the
  DMA transfer takes twice as long and hits a kernel timeout, not because of
  a fundamental word rate minimum.

- The internal loopback benchmark works fine at any clock speed because DMA is
  always the bottleneck — PIO is never the slow side.

### 6.4 A More Accurate Statement

The relationship is:
```
PIO word rate must be high enough that:
  (FIFO depth) / (PIO word rate) > (DMA burst overhead)

For RX with threshold=4, FIFO depth=8:
  Headroom = (8 - 4) words / PIO_word_rate
  Must be > DMA burst time (~700 ns)

  At 25 MW/s: headroom = 4 / 25M = 160 ns
  This is LESS than 700 ns → works only because DMA pipelines bursts

  At 12.5 MW/s: headroom = 4 / 12.5M = 320 ns
  Still less than 700 ns → should also work with pipelining

  At 1 MW/s: headroom = 4 / 1M = 4000 ns
  Comfortable margin → should definitely work
```

The ETIMEDOUT at `clkdiv=2.0` is likely a kernel-level timeout issue
(the total transfer takes too long for the DMA completion callback), not
a fundamental DMA word rate requirement.

---

## 7. The 1-Bit Autopull/Autopush Trick

### 7.1 Why It's Needed

On RP1, `out pins` does NOT drive physical GPIO pads — only `set pins`
toggles GPIO. To serialise arbitrary data through a GPIO pin, the PIO program
must:

1. Extract each bit from the data (`out x, 1`)
2. Branch on the bit value (`jmp !x low`)
3. Drive the pin HIGH or LOW (`set pins, 1` / `set pins, 0`)
4. Wait for the GPIO synchroniser (3 nop cycles on RP1)
5. Sample the input pin (`in pins, 1`)

This takes 8 PIO cycles per bit. With standard 32-bit autopull (threshold=32),
the PIO program would consume one 32-bit word every 8 × 32 = 256 cycles, or
781.25 KW/s at 200 MHz. This is well below the DMA rate, and we found it
caused problems (likely the ETIMEDOUT timeouts, though not definitively tested
with this exact configuration).

### 7.2 How 1-Bit Autopull Works

Setting autopull threshold to 1 (`sm_config_set_out_shift(&c, false, true, 1)`)
makes the PIO hardware trigger autopull after every single `out x, 1`
instruction. This means:

- Each `out x, 1` consumes **one full 32-bit DMA word** from the TX FIFO
  (the PIO reads the entire 32-bit word into the OSR, but only the MSB is
  used by `out x, 1` with left shift)
- Each `in pins, 1` produces **one full 32-bit DMA word** into the RX FIFO
  (the PIO writes the ISR with just bit 0 set, then autopush sends the
  entire 32-bit word)
- DMA word rate = PIO bit rate = 200 MHz / 8 = 25 MW/s

### 7.3 The 32× Data Expansion

The cost is a 32× increase in DMA bandwidth:
- 8 KB of source data → 256 KB of DMA data (each source bit becomes a 32-bit
  DMA word)
- Source data throughput: 25 MW/s × 4 bytes / 32 = **3.125 MB/s** ceiling
- Measured actual: **~1.5 MB/s** per-iteration (~49% of ceiling)

The ~49% efficiency is consistent with the DMA bus contention: both TX and RX
are competing for DMA bandwidth at 25 MW/s each (100 MB/s total DMA traffic
for 3.125 MB/s of actual data).

### 7.4 Data Format

**TX expansion** (source → DMA):
```
Source word 0xDEADBEEF (binary: 1101 1110 1010 1101 ...)
  → DMA word 0: 0x80000000  (bit 31 = 1 → MSB set)
  → DMA word 1: 0x80000000  (bit 30 = 1)
  → DMA word 2: 0x00000000  (bit 29 = 0)
  → DMA word 3: 0x80000000  (bit 28 = 1)
  ... (32 DMA words per source word)
```

**RX compression** (DMA → result):
```
  DMA word 0: 0x00000001  (bit 0 = 1, sampled HIGH)
  DMA word 1: 0x00000001  (bit 0 = 1)
  DMA word 2: 0x00000000  (bit 0 = 0)
  DMA word 3: 0x00000001  (bit 0 = 1)
  ... → reconstruct source word 0xDEADBEEF
```

---

## 8. The Skip Value: Garbage from Uninitialized OSR

### 8.1 The Correct Skip Value: 1

At PIO startup, the OSR contains an indeterminate value. The first `out x, 1`
produces a garbage bit before the first DMA word reaches the TX FIFO and
triggers the first autopull. This means the first RX DMA word is garbage and
must be skipped.

**Skip = 1** was definitively proven through careful measurement:

| Configuration | Skip=1 | Skip=9 |
|---|---|---|
| TX thresh=1, RX thresh=4, 1 word | MATCH | MATCH (coincidence) |
| TX thresh=1, RX thresh=4, 4 words | MATCH | 8-bit rotation error |
| TX thresh=1, RX thresh=4, 64 words | MATCH | 8-bit rotation error |
| TX thresh=4, RX thresh=4, 1 word | — | appeared correct |

### 8.2 The Skip=9 Red Herring

Early experiments with TX threshold=4 showed 9 leading zeros in the RX output
before real data appeared, leading to a hypothesis that skip=9 was needed.

This was **wrong** — the 9 leading zeros were caused by TX threshold=4
corrupting positions 1-8 (FIFO boundary corruption described in §4.1). With
TX threshold=1 (no corruption), only position 0 is garbage.

The skip=9 hypothesis was disproven when multi-word patterns showed an 8-bit
rotation (e.g., `0xDEADBEEF → 0xADBEEFDE`), which is the expected result of
skipping 9 instead of 1 — the 8 extra skipped positions shift the bit
alignment by 8 bits.

---

## 9. Summary of Proven Configuration

```c
/* PIO: 1-bit autopull/autopush */
sm_config_set_out_shift(&c, false, true, 1);   /* left shift, autopull, 1-bit */
sm_config_set_in_shift(&c, false, true, 1);    /* left shift, autopush, 1-bit */

/* DMACTRL */
#define TX_DMACTRL  0x80000101u  /* DREQ_EN | threshold=1 */
#define RX_DMACTRL  0x80000104u  /* DREQ_EN | threshold=4 */
pio_sm_set_dmactrl(pio, sm, true,  TX_DMACTRL);
pio_sm_set_dmactrl(pio, sm, false, RX_DMACTRL);

/* Skip 1 RX word (garbage from uninitialized OSR) */
#define RX_SKIP  1

/* DMA-before-SM sequencing */
pthread_create(&rx_tid, NULL, xfer_thread, &rx_args);
pthread_create(&tx_tid, NULL, xfer_thread, &tx_args);
usleep(2000);  /* ensure DMA requests reach kernel */
pio_sm_set_enabled(pio, sm, true);
```

**Hardware validation:**
- 2100/2100 diagnostic tests PERFECT (7 patterns × 6 sizes × 50 iterations)
- 200/200 benchmark iterations PERFECT (4 patterns × 50 iterations)
- 0 data errors across all configurations

---

## 10. Open Questions

1. **What is the exact mechanism of the TX threshold > 2 corruption?**
   Is it a concurrent FIFO access issue, a DREQ timing issue, or something
   else? Register-level tracing of the DMA controller would be needed.

2. **Why does threshold=8 cause DMA_FAIL (ETIMEDOUT) for TX?**
   Is it a kernel timeout, a DMA controller hang, or a FIFO deadlock?

3. **Would TX threshold=1 work at all PIO word rates?**
   We only tested at 25 MW/s. It would be interesting to test at 1 MW/s
   (160 PIO cycles per bit) to see if the configuration is universally safe.

4. **Is the RX threshold=4 requirement specific to 25 MW/s?**
   At lower PIO rates, threshold=1 might work fine for RX since the FIFO
   fills more slowly. At higher rates, even threshold=4 might overflow.

5. **What causes ETIMEDOUT at clkdiv=2.0?**
   Is it a kernel DMA completion timeout, or does the DMA controller itself
   stall? The kernel `rp1-pio.c` has a timeout on `wait_for_completion()`.

6. **Does the "fast" regime (internal loopback) actually avoid the TX
   threshold bug, or does it just not manifest?**
   The internal loopback uses threshold=8 successfully. This could be because
   PIO empties the FIFO before DMA refills it (no concurrent access), or it
   could be because the 32-bit autopull threshold=32 creates different
   timing. Testing the internal loopback with 1-bit autopull and threshold=1
   vs threshold=4 would isolate the variable.

---

## 11. Comparison: Internal Loopback vs GPIO Loopback

| Property | Internal Loopback | GPIO Loopback |
|---|---|---|
| PIO program | `out x,32 / mov y,~x / in y,32` | `out x,1 / jmp / set / 3×nop / in pins,1 / jmp` |
| Cycles per word | 3 | 256 (8 per bit × 32 bits per source word) |
| PIO word rate | 66.7 MW/s | 25 MW/s (DMA words); 781 KW/s (source words) |
| Autopull threshold | 32 | 1 |
| DMA expansion | 1× | 32× |
| TX DMACTRL | `0xC0000108` (threshold=8) | `0x80000101` (threshold=1) |
| RX DMACTRL | `0xC0000108` (threshold=8) | `0x80000104` (threshold=4) |
| Measured throughput | ~42 MB/s aggregate | ~1.5 MB/s per-iteration (source data) |
| DMA traffic | ~42 MB/s | ~100 MB/s (for ~1.5 MB/s source data) |
| Data verification | bitwise NOT | identity (TX = RX) |

The internal loopback is DMA-limited (PIO is 15× faster than DMA can feed it).
The GPIO loopback is PIO-limited (DMA moves 32× more data than the source
requires, consuming available DMA bandwidth).

---

## Appendix A: DMACTRL Values Used

| Value | Meaning | Used By |
|---|---|---|
| `0xC0000108` | DREQ_EN + STATUS, priority=2, threshold=8 | Internal loopback (default kernel config) |
| `0x80000101` | DREQ_EN, priority=2, threshold=1 | GPIO loopback TX |
| `0x80000104` | DREQ_EN, priority=2, threshold=4 | GPIO loopback RX |

Note: bit 30 (DREQ_STATUS) differs between configurations. The internal
loopback uses `0xC...` (bit 30 set) while GPIO loopback uses `0x8...` (bit 30
clear). Bit 30 is documented as read-only status, so setting it should have no
effect — but this hasn't been explicitly verified.

## Appendix B: Failure Mode Catalogue

| TX Thresh | RX Thresh | Failure Mode | Interpretation |
|:---:|:---:|---|---|
| 1 | 4 | None (PERFECT) | Optimal configuration |
| 2 | 4 | None (PERFECT) | Also works |
| 4 | 4 | Bit errors at FIFO boundary | Autopull/DMA collision |
| 8 | 4 | ETIMEDOUT | DMA hangs or kernel timeout |
| 1 | 1 | Massive corruption | RX FIFO overflow |
| 1 | 2 | Massive corruption | RX FIFO overflow |
| 2 | 2 | Massive corruption | RX FIFO overflow |
| 4 | 1 | Massive corruption | Both TX collision + RX overflow |
