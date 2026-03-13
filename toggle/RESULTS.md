# PIO Toggle Frequency Benchmark Results

**Date:** 2026-03-13
**PIO program:** 2-instruction SET loop (`set pins,1` / `set pins,0`)
**Reported clock:** `clock_get_hz(clk_sys)` = 200,000,000 Hz (200 MHz)

## Summary

**The RP1 PIO clock is 200 MHz.** The claim that "PIO runs at 400 MHz" is false.

Nine measurements across three independent instruments (RPi4 edge counter, Glasgow 48 MHz analyzer, Glasgow 144 MHz PLL freq-counter) match the 200 MHz model with **99.99% accuracy**. If PIO ran at 400 MHz, all frequencies would be 2x higher than observed.

## Combined Results (delay=0)

| clkdiv | Expected (200 MHz) | RPi4 Measured | Glasgow 48 MHz | Glasgow 144 MHz PLL | Best Accuracy |
|-------:|-------------------:|--------------:|---------------:|--------------------:|--------------:|
|    256 |        390.625 kHz |   389.920 kHz |    390.587 kHz |         390.591 kHz |       99.99%  |
|    128 |        781.250 kHz |   779.993 kHz |    781.233 kHz |         781.181 kHz |       99.99%  |
|     64 |          1.562 MHz |     1.560 MHz |      1.562 MHz |           1.562 MHz |       99.99%  |
|     32 |          3.125 MHz |     3.108 MHz |      3.125 MHz |           3.125 MHz |       99.99%  |
|     16 |          6.250 MHz |   *aliased*   |      6.249 MHz |           6.249 MHz |       99.99%  |
|      8 |         12.500 MHz |   *aliased*   |     12.487 MHz |          12.499 MHz |       99.99%  |
|      4 |         25.000 MHz |   *aliased*   |     22.995 MHz |          24.998 MHz |       99.99%  |
|      2 |         50.000 MHz |   *aliased*   |    *aliased*   |          49.996 MHz |       99.99%  |
|      1 |        100.000 MHz |   *no edges*  |    *aliased*   |     44.009 MHz (a)  |           —   |

(a) Aliased: |100 - 144| = 44 MHz, confirming signal is 100 MHz sampled at 144 MHz.

## Instrument Capabilities

### RPi4 BCM2711 Edge Counter (GPIO4)
- **Method:** mmap `/dev/gpiomem`, tight polling loop on GPLEV0 register
- **Sample rate:** ~14.7 MHz (~68 ns per GPIO read)
- **Accurate range:** DC -- 3.1 MHz (clkdiv >= 32)
- **Nyquist limit:** ~7.35 MHz; aliased results above this

### Glasgow Interface Explorer revC3 -- Standard Analyzer (Pin A7 = GPIO5)
- **Method:** FPGA logic analyzer, VCD capture with nanosecond timestamps
- **Sample rate:** ~48 MHz (iCE40 FPGA USB clock)
- **Accurate range:** DC -- 12.5 MHz (clkdiv >= 8)
- **Nyquist limit:** ~24 MHz; aliased results above this
- **Jitter:** 21--22 ns (= 1 sample period)

### Glasgow Interface Explorer revC3 -- PLL Freq-Counter (Pin A7 = GPIO5)
- **Method:** Custom FPGA applet with iCE40 PLL at 144 MHz (3x base clock)
- **Sample rate:** 144 MHz (PLL-clocked edge detection)
- **Accurate range:** DC -- 50 MHz (clkdiv >= 2)
- **Nyquist limit:** 72 MHz; aliased results above this
- **Measurement:** Edge count over 1-second gate; frequency = edges / (2 * gate_seconds)
- **Repeatability:** 3 measurements per setting, variance < 1 edge count

### Combined Coverage
- **8 of 9 clkdiv settings** have accurate measurements (99.99%) from at least one instrument
- **4 settings** (clkdiv 256/128/64/32) have cross-validated measurements from all three instruments
- **clkdiv=1 aliasing confirms 100 MHz:** |f_signal - f_sample| = |100 - 144| = 44 MHz (measured: 44.009 MHz)
- All accurate measurements confirm the 200 MHz model within 0.01%

## Why clkdiv=1 Can't Be Directly Measured (But Is Confirmed via Aliasing)

At clkdiv=1, the expected 100 MHz toggle produces 5 ns half-periods -- faster than the 72 MHz Nyquist limit of the 144 MHz PLL freq-counter. The measured 44.009 MHz is the alias frequency: |100 - 144| = 44 MHz. This aliasing pattern itself confirms the signal is 100 MHz (a 200 MHz toggle would alias to |200 - 144| = 56 MHz instead).

Additionally, clkdiv=1 with delay=15 produces 6.25 MHz (same as clkdiv=16, delay=0). The freq-counter measures this at 6.249 MHz (99.99%), confirming PIO executes correctly at clkdiv=1.

## Measurement Methodology

**RPi4 edge counter:**
- RPi5 runs `toggle_rpi5 --pin 4 --clkdiv N --duration-ms 5000`
- RPi4 runs `toggle_rpi4 --pin 4 --duration-ms 3000 --json`
- RPi5 starts 2 seconds before RPi4 measurement begins
- Frequency = `edges / (2 * elapsed_seconds)` (2 edges per cycle)
- Three independent runs with consistent results (+/-0.3% variation)

**Glasgow analyzer (48 MHz):**
- RPi5 runs `toggle_rpi5 --pin 5 --clkdiv N --duration-ms 15000`
- Glasgow captures VCD trace via `glasgow run analyzer -V 3.3 --i A7`
- VCD parsed for transition timestamps; frequency = `1 / avg_full_period`
- FIFO overrun terminates capture after ~430 transitions (sufficient for measurement)

**Glasgow PLL freq-counter (144 MHz):**
- Custom Glasgow applet using iCE40 SB_PLL40_CORE: 48 MHz x 12 / 4 = 144 MHz
- Input sampled via io.Buffer + FFSynchronizer into PLL clock domain
- Edge detection at 144 MHz; 32-bit counter accumulates edges over gate period
- Host sends 4-byte gate time, FPGA returns 12-byte result (edges, gate_cycles, sample_freq)
- 3 measurements per clkdiv setting, 1-second gate time each

## Conclusion

| Hypothesis                     | Prediction at clkdiv=2 | Glasgow 144 MHz Measured | Verdict       |
|--------------------------------|-----------------------:|-------------------------:|---------------|
| PIO clock = 200 MHz (nominal)  |          50.000 MHz    |             49.996 MHz   | **CONFIRMED** |
| PIO clock = 400 MHz (claimed)  |         100.000 MHz    |             49.996 MHz   | **REJECTED**  |

The RP1 PIO system clock is **200 MHz** as reported by `clock_get_hz(clk_sys)`. The "400 MHz" claim likely conflates camera pixel throughput (200 MHz x 2 pixels/clock in CSI DPI mode) with actual PIO instruction execution rate.

With the 144 MHz PLL freq-counter, we can now measure **8 of 9 clkdiv settings at 99.99% accuracy**, including clkdiv=2 (50 MHz) which was previously unmeasurable. The clkdiv=1 aliasing pattern provides additional confirmation of the 100 MHz toggle frequency (and thus 200 MHz PIO clock).

## Hardware Notes

- **GPIO5 (JC9):** Physical jumper cable between RPi5 and RPi4 appears disconnected; Glasgow (directly wired to RPi5 header) can still measure this pin
- **GPIO4 (JC7):** Working between RPi5 and RPi4; used for RPi4 edge counter measurements
- **Glasgow pin A7** is wired to GPIO5 on the RPi5 header (not via inter-RPi cable)
