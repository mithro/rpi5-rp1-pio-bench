# PIO Toggle Frequency Benchmark

Measures the GPIO toggle frequency produced by a 2-instruction PIO program across a range of clock divider settings, using a Glasgow Interface Explorer FPGA frequency counter as the primary instrument.

## Purpose

Determines the actual RP1 PIO system clock frequency by comparing measured toggle frequencies against predictions for 200 MHz and 400 MHz clock hypotheses.

## Key Result

The RP1 PIO clock is **200 MHz**. All 9 clock divider settings (1 to 256) measured at 99.99% accuracy against the 200 MHz prediction. The "400 MHz PIO clock" claim is false.

| clkdiv | Expected (200 MHz) | Measured | Accuracy |
|-------:|-------------------:|---------:|---------:|
| 1 | 100.000 MHz | 99.991 MHz | 99.99% |
| 2 | 50.000 MHz | 49.996 MHz | 99.99% |
| 4 | 25.000 MHz | 24.998 MHz | 99.99% |
| 256 | 390.625 kHz | 390.591 kHz | 99.99% |

## Files

| File | Description |
|------|-------------|
| `toggle_rpi5.c` | RPi5 PIO toggle generator |
| `toggle_rpi4.c` | RPi4 GPIO edge counter (cross-validation) |
| `gpio_toggle.pio` | 2-instruction PIO toggle program |
| `run_toggle_benchmark.py` | SSH orchestrator (RPi5 + RPi4) |
| `run_glasgow_freq_sweep.py` | Glasgow frequency counter sweep |
| `glasgow_freq_counter/` | Custom Glasgow FPGA applet (288 MHz PLL + DDR) |
| `Makefile` | Build system |

## See Also

- [Design](DESIGN.md) -- PIO toggle program, Glasgow applet, sweep methodology
- [Results](RESULTS.md) -- full measurement tables, instrument comparison, methodology
- [Usage](USAGE.md) -- prerequisites, build, running sweeps
