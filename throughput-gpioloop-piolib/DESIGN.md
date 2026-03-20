# GPIO Loopback Throughput -- Design

## PIO Program

The `throughput_gpioloop_piolib` PIO program uses a single state machine to serialise and deserialise data through a physical GPIO pin. It runs at 8 PIO cycles per bit at 200 MHz (5 ns per cycle).

```
Cycle  Instruction         Purpose
─────  ──────────────────  ────────────────────────────────────
[0]    out x, 1            Shift 1 bit from OSR into X (autopull every bit)
[1]    jmp !x low          Branch on bit value
[2]    set pins, 1 / 0     Drive GPIO HIGH (bit=1) or LOW (bit=0)
[3]    nop                 GPIO synchroniser delay
[4]    nop                 GPIO synchroniser delay
[5]    nop                 GPIO synchroniser delay (RP1 needs 4 cycles: [2]->[6])
[6]    in pins, 1          Sample GPIO input -> ISR (autopush every bit)
[7]    jmp top / nop       Next bit (jmp on HIGH path, nop on LOW path)
```

RP1 `out pins` does not drive physical GPIO pads. The program uses `set pins` with conditional branching (`jmp !x`) to drive the output, adding one cycle of overhead compared to a direct `out pins` approach.

## 1-Bit Autopull/Autopush and 32x DMA Expansion

Setting autopull and autopush thresholds to 1 causes PIO to consume/produce one full 32-bit DMA word per bit:

- `out x, 1` triggers autopull after each bit, loading a new 32-bit word from the TX FIFO (only MSB is used)
- `in pins, 1` triggers autopush after each bit, writing a 32-bit word to the RX FIFO (sampled value in LSB)

This means each 32-bit source word requires 32 DMA words for TX and produces 32 DMA words on RX -- a 32x data expansion.

### Data Format

**TX expansion** (host -> DMA -> PIO):
```
Source word 0xDEADBEEF (bit 31 = 1, bit 30 = 1, bit 29 = 0, bit 28 = 1, ...)
  -> DMA word 0: 0x80000000  (bit 31 = 1, MSB set)
  -> DMA word 1: 0x80000000  (bit 30 = 1)
  -> DMA word 2: 0x00000000  (bit 29 = 0)
  -> DMA word 3: 0x80000000  (bit 28 = 1)
  ... (32 DMA words total)
```

**RX compression** (PIO -> DMA -> host):
```
  DMA word 0: 0x00000001  (bit 0 = 1, pin sampled HIGH)
  DMA word 1: 0x00000001  (bit 0 = 1)
  DMA word 2: 0x00000000  (bit 0 = 0)
  DMA word 3: 0x00000001  (bit 0 = 1)
  ... -> reconstruct 0xDEADBEEF
```

## DMA Word Rate

At 200 MHz with 8 PIO cycles per bit:

- DMA word rate: 200 MHz / 8 = **25 MW/s** (each bit consumes/produces one DMA word)
- Source word rate: 25 MW/s / 32 = 781 KW/s
- Theoretical source throughput: 781 KW/s x 4 bytes = **3.125 MB/s**
- Total DMA traffic: 25 MW/s x 4 bytes x 2 directions = **200 MB/s**

The 25 MW/s word rate keeps the DMA controller busy enough to pipeline bursts. At lower PIO clock rates (e.g. clkdiv=2.0, 12.5 MW/s), DMA transfers fail with ETIMEDOUT.

## DMACTRL Threshold Configuration

TX and RX require asymmetric DMACTRL thresholds due to the interaction between autopull/autopush and DMA burst timing at 25 MW/s:

| Direction | DMACTRL | Threshold | Rationale |
|-----------|---------|-----------|-----------|
| TX | `0x80000101` | 1 | Avoids autopull/DMA FIFO collision. Threshold=1 means DREQ fires only when FIFO is empty, so PIO cannot consume words during DMA burst. |
| RX | `0x80000104` | 4 | Ensures DMA drains fast enough. Higher threshold reduces DMA handshake frequency, allowing burst pipelining. |

Threshold=4 on TX causes bit errors at FIFO refill boundaries. Threshold < 4 on RX causes massive corruption from FIFO overflow. See [dma-word-rate-theory.md](dma-word-rate-theory.md) for the full analysis.

## Startup Skip

At PIO startup, the OSR contains an indeterminate value. The first `out x, 1` produces a garbage bit before the first DMA word arrives via autopull. The software sends `src_words * 32 + 1` TX words and discards the first RX word (skip = 1).

## DMA Sequencing

DMA transfers must be started before enabling the PIO state machine:

1. Start RX DMA thread
2. Start TX DMA thread
3. Wait 2 ms for DMA requests to reach the kernel
4. Enable PIO state machine

This ensures the DMA controller is ready to service FIFO requests when PIO begins executing.
