# GPIO Loopback Throughput Benchmark

Measures DMA throughput through a physical GPIO loopback path on the RPi5 RP1 PIO block.

A single PIO state machine serialises each 32-bit source word to a 1-bit GPIO output pin, then deserialises the signal back from the GPIO input pin. Because 1-bit autopull/autopush is used, each source bit consumes and produces a full 32-bit DMA word, creating a 32x DMA expansion factor.

## Key Result

- Source data throughput: ~1.5 MB/s per iteration (theoretical ceiling 3.125 MB/s)
- DMA traffic: ~100 MB/s (32x expansion of source data)
- DMA word rate: 25 MW/s (200 MHz / 8 cycles per bit)

## How It Works

1. Each source word (32 bits) is expanded into 32 DMA words (one bit per word, MSB only)
2. PIO shifts out each bit via `set pins` (RP1 `out pins` does not drive physical GPIO pads)
3. After 4 cycles of GPIO synchroniser delay, PIO samples the input pin via `in pins,1`
4. 32 received DMA words (LSB only) are compressed back to one result word for verification

## Files

| File | Description |
|------|-------------|
| `throughput_gpioloop_piolib.c` | Benchmark program (RPi5, requires libpio-dev) |
| `gpio_loopback.pio` | PIO program: 8-cycle bit-serial loopback |
| `Makefile` | Build system |
| `dma-word-rate-theory.md` | DMA/PIO interaction analysis and threshold tuning |
| `dma-improvements.md` | Survey of DMA optimisation approaches for RP1 PIO |

## See Also

- [DMA Word Rate Theory](dma-word-rate-theory.md) -- autopull/FIFO collision analysis, threshold requirements
- [DMA Improvements](dma-improvements.md) -- SRAM rings, cyclic DMA, zero-copy, M3 Core 1 approaches
- [Design](DESIGN.md) -- PIO program design and 32x DMA expansion mechanism
- [Results](RESULTS.md) -- measured throughput and comparison with internal loopback
- [Usage](USAGE.md) -- build, wiring, and run instructions
