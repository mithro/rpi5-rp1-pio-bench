# GPIO Latency Benchmark

Measures round-trip GPIO latency through the RPi5 RP1 PIO block at four abstraction layers, quantifying the overhead of each software layer in the PIO data path.

## Architecture

Two Raspberry Pis connected via Pmod HAT:

- **RPi4** (stimulus/measurement): drives GPIO4 HIGH/LOW via mmap `/dev/gpiomem`, measures round-trip time with `CLOCK_MONOTONIC`
- **RPi5** (echo): runs PIO programs that echo the input signal back to the output pin at different abstraction layers

A Python orchestrator on the development machine coordinates both devices via SSH.

## Test Layers

| Layer | Path | Median Latency |
|-------|------|----------------|
| L0 | PIO-only echo (no CPU) | 388 ns |
| L1 | PIO -> ioctl -> CPU -> ioctl -> PIO | 44 us |
| L2 | PIO -> DMA -> CPU poll -> DMA -> PIO | 52 us |
| L3 | Batched DMA (4 KB reads, standalone) | 89 us |

L0 represents the hardware baseline. L1 is 113x slower due to ioctl round-trip overhead. L2 adds DMA setup overhead. L3 measures bulk DMA throughput latency.

## Files

| File | Description |
|------|-------------|
| `latency_rpi5.c` | RPi5 PIO echo program (all layers) |
| `latency_rpi4.c` | RPi4 stimulus/measurement program |
| `run_latency_benchmark.py` | Python SSH orchestrator |
| `gpio_echo.pio` | L0 PIO program: 4-instruction echo |
| `edge_detector.pio` | L1/L2 PIO program: edge detection with FIFO |
| `output_driver.pio` | L1/L2 PIO program: output from FIFO |
| `latency_common.h` | Shared constants and structures |
| `Makefile` | Build system (rpi4 and rpi5 targets) |

## See Also

- [Design](DESIGN.md) -- layer descriptions, measurement method, PIO programs
- [Results](RESULTS.md) -- full latency statistics per layer
- [Usage](USAGE.md) -- prerequisites, wiring, orchestrator CLI
