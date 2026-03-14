# PIO Toggle Frequency Benchmark Results

**Date:** 2026-03-14
**PIO program:** 2-instruction SET loop (`set pins,1` / `set pins,0`)
**Reported clock:** `clock_get_hz(clk_sys)` = 200,000,000 Hz (200 MHz)

## Summary

**The RP1 PIO clock is 200 MHz.** The claim that "PIO runs at 400 MHz" is false.

All 9 clkdiv settings from 1 to 256 measured at **99.99% accuracy** using a custom Glasgow FPGA frequency counter with 288 MHz PLL + DDR I/O (576 MHz effective sample rate) and manual placement constraints. If PIO ran at 400 MHz, all frequencies would be 2x higher than observed.

## Results (delay=0)

| clkdiv | Expected (200 MHz) | Glasgow PLL+DDR Measured | Accuracy |
|-------:|-------------------:|-------------------------:|---------:|
|    256 |        390.625 kHz |              390.591 kHz |   99.99% |
|    128 |        781.250 kHz |              781.182 kHz |   99.99% |
|     64 |          1.562 MHz |                1.562 MHz |   99.99% |
|     32 |          3.125 MHz |                3.125 MHz |   99.99% |
|     16 |          6.250 MHz |                6.249 MHz |   99.99% |
|      8 |         12.500 MHz |               12.499 MHz |   99.99% |
|      4 |         25.000 MHz |               24.998 MHz |   99.99% |
|      2 |         50.000 MHz |               49.996 MHz |   99.99% |
|      1 |        100.000 MHz |               99.991 MHz |   99.99% |

Additional cross-validation: clkdiv=1 with delay=15 (expected 6.250 MHz) measured 6.249 MHz (99.99%).

## Earlier Instrument Results (for cross-validation)

| clkdiv | RPi4 Edge Counter | Glasgow 48 MHz Analyzer | Glasgow 144 MHz SDR |
|-------:|------------------:|------------------------:|--------------------:|
|    256 |       389.920 kHz |             390.587 kHz |         390.591 kHz |
|    128 |       779.993 kHz |             781.233 kHz |         781.181 kHz |
|     64 |         1.560 MHz |               1.562 MHz |           1.562 MHz |
|     32 |         3.108 MHz |               3.125 MHz |           3.125 MHz |
|     16 |       *aliased*   |               6.249 MHz |           6.249 MHz |
|      8 |       *aliased*   |              12.487 MHz |          12.499 MHz |
|      4 |       *aliased*   |              22.995 MHz |          24.998 MHz |
|      2 |       *aliased*   |             *aliased*   |          49.996 MHz |
|      1 |       *no edges*  |             *aliased*   |      44.009 MHz (a) |

(a) Aliased: |100 - 144| = 44 MHz, consistent with 100 MHz signal sampled at 144 MHz SDR.

## Instrument Capabilities

### Glasgow PLL+DDR Freq-Counter (primary instrument)
- **Method:** Custom FPGA applet: iCE40 PLL at 288 MHz + DDR I/O (SB_IO DDR mode)
- **Effective sample rate:** 576 MHz (samples on both PLL clock edges)
- **Accurate range:** DC -- 100 MHz (all 9 clkdiv settings confirmed)
- **Nyquist limit:** 288 MHz
- **Architecture:** Free-running counters with snapshot subtraction. Pipelined edge detection (XOR -> registered -> combined sum -> registered). 3-stage segmented counter (8+12+12 bits) with registered carries. Zero conditional logic in counter paths.
- **Manual placement:** nextpnr --pre-pack script constrains all fast-domain cells to a rectangular region near the DDR I/O pin, minimizing routing delays.
- **Measurement:** Single combined edge counter, gate counter tracks PLL cycles. Sync domain computes (end - start) for both.
- **Repeatability:** 3 measurements per setting, variance < 2 edge counts in ~200M
- **Overclocking:** nextpnr reports ~194 MHz max achievable, but the design works reliably at 288 MHz with manual placement constraints (iCE40 timing models are conservative). 336 MHz shows counter corruption.

### RPi4 BCM2711 Edge Counter (GPIO4)
- **Method:** mmap `/dev/gpiomem`, tight polling loop on GPLEV0 register
- **Sample rate:** ~14.7 MHz (~68 ns per GPIO read)
- **Accurate range:** DC -- 3.1 MHz (clkdiv >= 32)
- **Nyquist limit:** ~7.35 MHz

### Glasgow Standard Analyzer (48 MHz)
- **Method:** FPGA logic analyzer, VCD capture
- **Sample rate:** ~48 MHz (iCE40 USB clock)
- **Accurate range:** DC -- 12.5 MHz (clkdiv >= 8)
- **Nyquist limit:** ~24 MHz

### Glasgow PLL-only Freq-Counter (144 MHz SDR)
- **Method:** Custom FPGA applet: iCE40 PLL at 144 MHz, single-edge sampling
- **Sample rate:** 144 MHz
- **Accurate range:** DC -- 50 MHz (clkdiv >= 2)
- **Nyquist limit:** 72 MHz

## Measurement Methodology

**Glasgow PLL+DDR freq-counter (primary):**
- Custom Glasgow applet: `toggle/glasgow_freq_counter/__init__.py`
- iCE40 SB_PLL40_CORE: 48 MHz x 12 / 2 = 288 MHz (VCO = 576 MHz)
- DDR via io.DDRBuffer with `i_domain="fast"`: SB_IO samples at both PLL clock edges
- Pipelined edge detection: DDR -> prev register -> XOR -> registered edge flags -> registered sum
- Free-running 3-stage segmented counter (8+12+12 bits) with registered carries between stages
- Single combined edge counter (no conditional logic in datapath)
- Gate control via snapshot subtraction: start/end values captured, difference computed in sync domain
- Manual placement via --pre-pack constrains fast-domain cells near I/O pin (requires native nextpnr-ice40, GLASGOW_TOOLCHAIN=system)
- Built with --timing-allow-fail (288 MHz exceeds nextpnr's ~194 MHz model but works empirically with placement constraints)
- Host sends 4-byte gate time (48 MHz ticks), FPGA returns 12-byte result
- 3 measurements per clkdiv setting, 1-second gate time each

**RPi4 edge counter:**
- RPi5 runs `toggle_rpi5 --pin 4 --clkdiv N --duration-ms 5000`
- RPi4 runs `toggle_rpi4 --pin 4 --duration-ms 3000 --json`
- Frequency = `edges / (2 * elapsed_seconds)` (2 edges per cycle)

**Glasgow analyzer (48 MHz):**
- Glasgow captures VCD trace via `glasgow run analyzer -V 3.3 --i A7`
- VCD parsed for transition timestamps; frequency = `1 / avg_full_period`

## Conclusion

| Hypothesis                     | Prediction at clkdiv=1 | Glasgow DDR Measured | Verdict       |
|--------------------------------|-----------------------:|---------------------:|---------------|
| PIO clock = 200 MHz (nominal)  |          100.000 MHz   |         99.991 MHz   | **CONFIRMED** |
| PIO clock = 400 MHz (claimed)  |          200.000 MHz   |         99.991 MHz   | **REJECTED**  |

The RP1 PIO system clock is **200 MHz** as reported by `clock_get_hz(clk_sys)`.

With the 288 MHz PLL + DDR freq-counter (576 MHz effective, Nyquist 288 MHz), we directly measured the maximum toggle frequency at clkdiv=1: **99.991 MHz** -- matching the 200 MHz / 2 instructions = 100 MHz prediction to 99.99% accuracy. The signal is well within Nyquist -- no aliasing possible.

If PIO ran at 400 MHz, clkdiv=1 would produce 200 MHz, also within our 288 MHz Nyquist -- and we would measure 200 MHz, not 99.991 MHz. The "400 MHz" claim likely conflates camera pixel throughput (200 MHz x 2 pixels/clock in CSI DPI mode) with actual PIO instruction execution rate.

## Hardware Notes

- **GPIO5 (JC9):** Physical jumper cable between RPi5 and RPi4 appears disconnected; Glasgow (directly wired to RPi5 header) can still measure this pin
- **GPIO4 (JC7):** Working between RPi5 and RPi4; used for RPi4 edge counter measurements
- **Glasgow pin A7** is wired to GPIO5 on the RPi5 header (not via inter-RPi cable)
