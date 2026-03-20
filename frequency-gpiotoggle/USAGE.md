# PIO Toggle Frequency Benchmark -- Usage

## Prerequisites

### Hardware

- Raspberry Pi 5 (PIO toggle generator)
- Glasgow Interface Explorer (FPGA frequency counter) -- for primary measurements
- RPi4 (optional, for cross-validation via GPIO edge counting)
- GPIO5 on RPi5 connected to Glasgow pin A7 (or RPi4 GPIO4 for edge counter)

### Software

- RPi5: kernel 6.12+ with `libpio-dev` installed, root access
- Glasgow: `uv` tool installation (`glasgow` CLI)
- RPi4 (optional): standard Raspberry Pi OS with `/dev/gpiomem`
- Development machine: Python 3.10+, SSH key-based access to RPi5

## Build

### On RPi5

```sh
cd toggle-frequency
make rpi5
```

### On RPi4 (optional, for edge counter cross-validation)

```sh
cd toggle-frequency
make rpi4
```

### Other Targets

```sh
make pioasm        # Regenerate gpio_toggle.pio.h
make clean         # Remove build artifacts
FORCE_BUILD=1 make rpi5   # Force build on non-RPi5 host
```

## Running the Toggle Generator

On the RPi5:

```sh
sudo ./toggle_rpi5 --pin=5 --clkdiv=1 --duration-ms=5000
```

This toggles GPIO5 at 100 MHz (clkdiv=1) for 5 seconds.

## Glasgow Frequency Counter Sweep

From the development machine:

```sh
uv run toggle-frequency/run_glasgow_freq_sweep.py
```

This sweeps clkdiv values 1 through 256, measuring the toggle frequency at each setting with the Glasgow PLL+DDR frequency counter. Each measurement uses a 1-second gate time with 3 repeats per setting.

Requirements:
- `toggle_rpi5` built on RPi5
- Glasgow connected to GPIO5 on RPi5 header (pin A7)
- SSH access to RPi5

## RPi4 Edge Counter Sweep

From the development machine:

```sh
uv run toggle-frequency/run_toggle_benchmark.py
```

This coordinates RPi5 (toggle generator) and RPi4 (edge counter) via SSH. Accurate only for clkdiv >= 32 (frequencies up to ~3.1 MHz) due to RPi4 GPIO polling rate limits.

Requirements:
- `toggle_rpi5` built on RPi5
- `toggle_rpi4` built on RPi4
- GPIO5 on RPi5 connected to GPIO4 on RPi4
- SSH access to both RPis
