# PIO Toggle Frequency Benchmark -- Design

## PIO Toggle Program

The `gpio_toggle` program is a 2-instruction loop that toggles a GPIO pin at the maximum rate allowed by the PIO clock and divider settings:

```
.wrap_target
    set pins, 1          ; drive output HIGH
    set pins, 0          ; drive output LOW
.wrap
```

Toggle frequency formula:

```
f_toggle = f_pio / (2 * (1 + delay) * clkdiv)
```

Where:
- `f_pio` = PIO system clock (200 MHz on RP1)
- `delay` = per-instruction delay cycles (0 for maximum speed)
- `clkdiv` = clock divider (1.0 to 65535.0)

At clkdiv=1, delay=0: each instruction takes 1 cycle (5 ns), full period = 2 cycles (10 ns), toggle frequency = 100 MHz.

## Glasgow Frequency Counter (Primary Instrument)

Custom FPGA applet running on the Glasgow Interface Explorer (iCE40 FPGA):

- **PLL configuration**: iCE40 SB_PLL40_CORE at 48 MHz x 12 / 2 = 288 MHz (VCO = 576 MHz)
- **DDR sampling**: SB_IO DDR mode samples on both PLL clock edges, giving 576 MHz effective sample rate
- **Nyquist limit**: 288 MHz (can measure up to 100 MHz toggle without aliasing)
- **Edge detection**: Pipelined XOR -> registered edge flags -> registered sum
- **Counter**: 3-stage segmented counter (8+12+12 bits) with registered carries
- **Gate control**: Free-running counters with snapshot subtraction; host sends gate time, FPGA returns edge count and gate cycle count
- **Placement**: Manual nextpnr `--pre-pack` script constrains fast-domain cells near the DDR I/O pin to minimise routing delays

The 288 MHz PLL exceeds the iCE40 timing model (~194 MHz max) but works reliably with manual placement constraints. 336 MHz showed counter corruption.

## Clock Divider Sweep Methodology

`run_glasgow_freq_sweep.py` automates the measurement:

1. For each clkdiv value (1, 2, 4, 8, 16, 32, 64, 128, 256):
   - Start `toggle_rpi5` on RPi5 via SSH with the given clkdiv and GPIO5
   - Run Glasgow frequency counter applet with 1-second gate time
   - Take 3 measurements per setting
   - Record measured frequency and compute accuracy against 200 MHz prediction
   - Stop toggle program
2. Output results as JSON or table

## RPi4 Edge Counter (Cross-Validation)

`toggle_rpi4.c` provides an independent measurement using mmap GPIO polling:

- Memory-maps BCM2711 GPIO registers via `/dev/gpiomem`
- Counts edges on the input pin by polling `GPLEV0` in a tight loop
- Polling rate: ~14.7 MHz (~68 ns per read)
- Accurate for frequencies up to ~3.1 MHz (Nyquist ~7.35 MHz)
- Above ~7.35 MHz, edge counting aliases and underreports

`run_toggle_benchmark.py` coordinates RPi5 (toggle generator) and RPi4 (edge counter) via SSH for automated sweeps.

## Instrument Comparison

| Instrument | Sample Rate | Accurate Range | Nyquist |
|------------|-------------|----------------|---------|
| Glasgow PLL+DDR freq-counter | 576 MHz effective | DC -- 100 MHz | 288 MHz |
| Glasgow PLL-only (SDR) | 144 MHz | DC -- 50 MHz | 72 MHz |
| Glasgow standard analyzer | 48 MHz | DC -- 12.5 MHz | 24 MHz |
| RPi4 GPIO edge counter | ~14.7 MHz | DC -- 3.1 MHz | ~7.35 MHz |
