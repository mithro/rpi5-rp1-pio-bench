# PIO Toggle Frequency Benchmark Design

## Purpose

Determine the true RP1 PIO instruction execution rate by measuring the GPIO
toggle frequency of a minimal PIO program across multiple clock divider
settings. Verify or dismiss the claim that "PIO executes at 400 MHz (2x the
reported 200 MHz)."

## Background

Another Claude Code session claimed: "The RP1 PIO clock is 2x faster than
reported -- clock_get_hz(clk_sys) says 200 MHz but SM executes at effectively
400 MHz."

Research found three independent source code locations all confirming 200 MHz:
- `piolib/pio_rp1.c`: `clock_get_hz(clk_sys)` returns hardcoded `200 * MHZ`
- Linux kernel `drivers/mfd/rp1.c`: `#define RP1_SYSCLK_RATE 200000000`
- Linux kernel DPI PIO driver uses `clock_get_hz(clk_sys)` for timing math

The "400" number likely originates from camera pixel throughput (200 MHz x 2
pixels/clock = 400 Mpx/s), not PIO clock speed.

This benchmark provides empirical measurement to settle the question
definitively.

## Test Setup

### Physical Signal Path

```
RPi5 PIO (GPIO5) --+-- Pmod JC9 ---- jumper ---- RPi4 GPIO5 (edge counting)
                    |
                    +-- Glasgow A7 (frequency measurement)
```

### Measurement Devices

1. **Glasgow revC3** (on RPi5 USB, pin A7): Primary measurement -- FPGA-based
   capture, high bandwidth. iCE40HX8K can do ~100-133 MHz LVCMOS33 I/O.
2. **RPi4** (mmap polling on GPIO5): Secondary cross-validation -- edge
   counting accurate up to ~5 MHz.
3. **RPi5 `clock_get_hz(clk_sys)`**: Software-reported clock rate.

### Key Prediction

- If PIO = 200 MHz: 2-instruction toggle = **100 MHz**
- If PIO = 400 MHz: 2-instruction toggle = **200 MHz**

## PIO Program

Minimal 2-instruction toggle loop using `set pins` (confirmed working on RP1):

```asm
.program gpio_toggle
.wrap_target
    set pins, 1    ; drive HIGH -- 1 cycle
    set pins, 0    ; drive LOW  -- 1 cycle
.wrap
```

### Toggle Frequency Formula

`f_toggle = f_pio / (2 x (1 + delay) x clkdiv)`

### Sweep Parameters

| Parameter         | Values                                       | Purpose                                      |
|-------------------|----------------------------------------------|----------------------------------------------|
| Clock divider     | 256, 128, 64, 32, 16, 8, 4, 2, 1            | Slow toggle to measurable range, then max    |
| Instruction delay | [0] primary; [1], [3], [7], [15], [31] secondary | Verify delay model independent of divider |

### Expected Frequencies (delay=0)

| clkdiv | Expected (200 MHz) | Expected (400 MHz) |
|--------|--------------------|--------------------|
| 256    | 390.625 kHz        | 781.250 kHz        |
| 64     | 1.5625 MHz         | 3.125 MHz          |
| 16     | 6.25 MHz           | 12.5 MHz           |
| 4      | 25 MHz             | 50 MHz             |
| 1      | 100 MHz            | 200 MHz            |

### GPIO Configuration

For maximum output signal quality:
- Slew rate: **SLEWFAST=1** (fast)
- Drive strength: **12 mA** (maximum, DRIVE=3)
- No pull-up/pull-down

## Measurement Strategy

### Glasgow (Primary)

- SSH to RPi5, run `glasgow run analyzer` on pin A7
- Capture short VCD trace (~10ms, enough for thousands of cycles)
- Post-process VCD to count edges and compute frequency
- Explore whether Glasgow has a `freq` or `sensor-freq` applet
- At clkdiv=1 (100 MHz toggle), test Glasgow's actual maximum sample rate

### RPi4 (Cross-Validation)

- Tight mmap polling loop on GPIO5 counting level transitions
- Accurate for toggle frequencies below ~5 MHz (clkdiv >= 20)
- At higher frequencies, reports "saturated" (transition ratio -> 0.5)
- Provides independent verification of Glasgow readings

### RPi5 Self-Report

- Call `clock_get_hz(clk_sys)` and report the value
- Expected: hardcoded 200000000 (200 MHz)

## Software Architecture

Following existing project patterns (latency benchmark):

```
toggle/
+-- gpio_toggle.pio              # PIO program
+-- gpio_toggle.pio.h            # Pre-generated header
+-- toggle_rpi5.c                # RPi5: load PIO, sweep dividers, report clock_get_hz
+-- toggle_rpi4.c                # RPi4: mmap edge counting
+-- toggle_common.h              # Shared types, frequency formatting
+-- Makefile                     # Platform-detecting build (reuses benchmark_stats)
+-- run_toggle_benchmark.py      # Orchestrator: SSH coordination, Glasgow, analysis
```

### RPi5 Program (toggle_rpi5.c)

- Accepts `--clkdiv N`, `--delay N`, `--duration-ms N` args
- Loads PIO program, configures GPIO5 with fast slew + 12 mA drive
- Sets clock divider and starts state machine
- Runs for specified duration, then stops SM and cleans up
- Prints `clock_get_hz(clk_sys)` to stdout as JSON
- Signal handler for clean SIGINT shutdown

### RPi4 Program (toggle_rpi4.c)

- Accepts `--pin N`, `--duration-ms N` args
- mmap `/dev/gpiomem`, tight polling loop counting transitions
- Reports: `edges_counted`, `duration_ns`, `measured_freq_hz`, `samples`,
  `transition_ratio`
- JSON output

### Orchestrator (run_toggle_benchmark.py)

- Syncs source, builds on both hosts
- For each (clkdiv, delay) pair:
  1. SSH RPi5: start `toggle_rpi5` in background
  2. Wait settle time (1s)
  3. SSH RPi5: run Glasgow capture on A7 (short burst)
  4. SSH RPi4: run `toggle_rpi4` edge counting (1s window)
  5. SSH RPi5: stop `toggle_rpi5`
  6. Parse Glasgow VCD -> compute frequency
  7. Parse RPi4 JSON -> measured frequency
- Output: comparison table + verdict

## Output Format

```
================================================================
PIO Toggle Frequency Benchmark
================================================================
Reported clock_get_hz(clk_sys): 200000000 Hz (200.000 MHz)

clkdiv | delay | expected_hz  | glasgow_hz   | rpi4_hz      | ratio
-------+-------+--------------+--------------+--------------+------
   256 |     0 |    390625.0  |    390631.2  |    390618.4  | 1.000
   128 |     0 |    781250.0  |    781242.8  |    781255.1  | 1.000
    64 |     0 |   1562500.0  |   1562488.3  |   1562512.7  | 1.000
    32 |     0 |   3125000.0  |   3124998.1  |   3125006.2  | 1.000
    16 |     0 |   6250000.0  |   6249987.5  |  (saturated) | 1.000
     8 |     0 |  12500000.0  |  12500012.8  |  (saturated) | 1.000
     4 |     0 |  25000000.0  |  25000001.3  |  (saturated) | 1.000
     2 |     0 |  50000000.0  |  49999997.4  |  (saturated) | 1.000
     1 |     0 | 100000000.0  | 100000003.7  |  (saturated) | 1.000

Verdict: PIO clock is 200 MHz (ratio = 1.000x, claim of 400 MHz DISMISSED)
================================================================
```

The `ratio` column = `glasgow_hz / expected_hz` (where expected assumes
200 MHz). If PIO were 400 MHz, all ratios would be ~2.0.

## Success Criteria

1. Glasgow measures frequency at clkdiv=256 within 0.1% of expected 390.625 kHz
2. All measured frequencies form a linear relationship with 1/clkdiv
3. Extrapolation to clkdiv=1 confirms 100 MHz toggle (200 MHz PIO clock)
4. RPi4 cross-validation agrees with Glasgow at overlapping frequencies
5. Clear verdict on the 200 MHz vs 400 MHz question

## Constraints

- BCM2711 GPIO pads limited to ~75-80 MHz input bandwidth (RPi4 can't
  directly measure 100 MHz)
- Glasgow iCE40HX8K LVCMOS33 I/O rated ~100-133 MHz (marginal at clkdiv=1)
- `clock_get_hz(clk_sys)` is hardcoded, not a hardware register read
- USB 2.0 bandwidth limits Glasgow streaming to ~48 MB/s
