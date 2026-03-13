# PIO Toggle Frequency Benchmark Results

**Date:** 2026-03-13
**Hardware:** RPi5 (RP1 PIO toggle) → GPIO4 → RPi4 (BCM2711 edge counter)
**PIO program:** 2-instruction SET loop (`set pins,1` / `set pins,0`)
**Reported clock:** `clock_get_hz(clk_sys)` = 200,000,000 Hz (200 MHz)

## Summary

**The RP1 PIO clock is 200 MHz.** The claim that "PIO runs at 400 MHz" is false.

Four measurements at clkdiv=256/128/64/32 match the 200 MHz model with **99.5–99.9% accuracy** across three independent runs. If PIO ran at 400 MHz, these frequencies would all be 2× higher than observed.

## Clock Divider Sweep (delay=0)

| clkdiv | Expected (200 MHz) | Measured      | Accuracy | Status          |
|-------:|-------------------:|--------------:|---------:|-----------------|
|    256 |        390.625 kHz |   389.920 kHz |   99.8%  | Accurate        |
|    128 |        781.250 kHz |   779.993 kHz |   99.8%  | Accurate        |
|     64 |          1.562 MHz |     1.560 MHz |   99.9%  | Accurate        |
|     32 |          3.125 MHz |     3.108 MHz |   99.5%  | Accurate        |
|     16 |          6.250 MHz |     6.002 MHz |   96.0%  | Nyquist limited |
|      8 |         12.500 MHz |     2.537 MHz |   20.3%  | Aliased         |
|      4 |         25.000 MHz |     3.390 MHz |   13.6%  | Aliased         |
|      2 |         50.000 MHz |     2.253 MHz |    4.5%  | Aliased         |
|      1 |        100.000 MHz |       ~0 Hz   |    0.0%  | Unmeasurable    |

### Why clkdiv ≤ 16 shows wrong values

The RPi4 BCM2711 GPIO read (GPLEV0 register via mmap) takes ~68 ns per access, giving a maximum polling rate of ~14.7 MHz. By Nyquist, signals above ~7.35 MHz cannot be accurately measured. At clkdiv=16 (6.25 MHz expected) we're near this limit; at clkdiv=8 and below, results are aliased garbage.

At clkdiv=1 (100 MHz), each half-period is only 5 ns — the GPIO read takes 13× longer than a full toggle cycle, so the pin appears static.

### Why clkdiv=1 still proves PIO is working

Testing clkdiv=1 with delay=15 produces an expected 6.25 MHz (same as clkdiv=16, delay=0). The RPi4 measures 5.93 MHz — matching the clkdiv=16 result. This confirms PIO executes correctly at clkdiv=1; only the measurement is limited.

## Measurement methodology

- RPi5 runs `toggle_rpi5 --pin 4 --clkdiv N --duration-ms 5000`
- RPi4 runs `toggle_rpi4 --pin 4 --duration-ms 3000 --json`
- RPi5 starts 2 seconds before RPi4 measurement begins
- Edge counting: tight polling loop on GPLEV0, transition = pin level change
- Frequency: `edges / (2 * elapsed_seconds)` (2 edges per full cycle)
- Three independent runs with consistent results (±0.3% variation)

## Conclusion

| Hypothesis                    | Prediction at clkdiv=256 | Observed     | Verdict |
|-------------------------------|-------------------------:|-------------:|---------|
| PIO clock = 200 MHz (nominal) |             390.625 kHz  | 389.920 kHz  | **CONFIRMED** |
| PIO clock = 400 MHz (claimed) |             781.250 kHz  | 389.920 kHz  | **REJECTED**  |

The RP1 PIO system clock is 200 MHz as reported by `clock_get_hz(clk_sys)`. The "400 MHz" claim likely conflates camera pixel throughput (200 MHz × 2 pixels/clock in CSI DPI mode) with actual PIO instruction execution rate.

## Future work

- **Glasgow measurement**: Use Glasgow Interface Explorer (FPGA, 48+ MHz sample rate) to accurately measure frequencies above the RPi4's ~7 MHz Nyquist limit
- **GPIO5 connection**: Physical jumper cable on JC9 (GPIO5) appears disconnected; GPIO4 (JC7) used as fallback
