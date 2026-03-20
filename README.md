> **Note:** This repository was generated with the assistance of AI tools
> (Claude). Content should be independently verified before relying on it
> for hardware decisions or production use.

# RP1 PIO Performance Exploration on Raspberry Pi 5

The RP1 is the southbridge chip on the Raspberry Pi 5, connected to the BCM2712 ARM host via PCIe 2.0 x4. It contains a PIO (Programmable I/O) block compatible with the RP2040 instruction set: 4 state machines, 200 MHz clock, and 8-deep FIFOs.

This repository measures DMA throughput and GPIO latency between the ARM host and the RP1's PIO block using several approaches: standard kernel DMA via `piolib`, cyclic DMA through a custom kernel module with SRAM-backed ring buffers, and direct PIO access from the RP1's M3 Core 1. It also documents RP1 hardware quirks and the kernel patches that affect performance.

Pre-built binaries for all benchmarks are available from [GitHub Actions](https://github.com/mithro/rpi5-rp1-pio-bench/actions).

## Throughput Results

All throughput values are in **MB/s (megabytes per second)**, not Mbit/s. Measured on 2026-03-17 (fresh boot, kernel 6.12+, both [PR #6994](https://github.com/raspberrypi/linux/pull/6994) and [PR #7190](https://github.com/raspberrypi/linux/pull/7190) applied).

| Approach | TX (MB/s) | RX (MB/s) | Benchmark | Notes |
|----------|----------:|----------:|-----------|-------|
| Cyclic DMA, SRAM bidirectional | 54 | 45 | [throughput-pioloop-cyclic](throughput-pioloop-cyclic/) `--sram` | SRAM ring buffers at 0xA200+ |
| RX-only DMA, DRAM | -- | 56 | [throughput-pioloop-cyclic](throughput-pioloop-cyclic/) `--rx-only` | Single-direction, kernel module |
| TX-only DMA, DRAM | 41 | -- | [throughput-pioloop-cyclic](throughput-pioloop-cyclic/) `--tx-only` | Single-direction, kernel module |
| Cyclic DMA, DRAM bidirectional | 40 | 36 | [throughput-pioloop-cyclic](throughput-pioloop-cyclic/) `--dram` | Standard DRAM ring buffers |
| Standard kernel DMA | ~42 | ~42 | [throughput-pioloop-piolib](throughput-pioloop-piolib/) | piolib ioctl, concurrent TX+RX via pthreads |
| piolib ioctl (sequential) | 18 | 18 | [throughput-pioloop-cyclic](throughput-pioloop-cyclic/) `--piolib` | Sequential ioctl calls |
| GPIO serial loopback | ~2 | ~2 | [throughput-gpioloop-piolib](throughput-gpioloop-piolib/) | 1-bit serial, 32x DMA expansion |
| M3 Core 1 CPU-polled | 7 | 7 | [throughput-pioloop-m3poll](throughput-pioloop-m3poll/) | APB bus bottleneck |
| cleverca22 custom driver (ref) | -- | ~66 | -- | Host-side DMA, [custom driver](https://github.com/cleverca22/libsigrok/commit/e3783bac8176e7454863b37723ab6d8a3f99731a) |

See individual benchmark [Results](throughput-pioloop-cyclic/RESULTS.md) for full details.

## Latency Results

Measured over 1000 iterations, 50 warmup, GPIO4/GPIO5 (JC connector), kernel 6.12+. RPi4 generates stimulus pulses; RPi5 echoes through PIO at progressively higher abstraction layers.

| Layer | Description | Median | Ratio to L0 |
|-------|-------------|-------:|-----------:|
| L0 | PIO-only echo (no CPU) | 388 ns | 1x |
| L1 | PIO + ioctl round-trip | 44 us | 113x |
| L2 | PIO + single-word DMA | 52 us | 134x |
| L3 | Batched 4 KB DMA read | 89 us | 229x |

See [latency-gpioloop/RESULTS.md](latency-gpioloop/RESULTS.md) for full statistics (min, P95, P99, max, stddev).

## Benchmarks

### [throughput-pioloop-cyclic](throughput-pioloop-cyclic/) -- Cyclic DMA Throughput

Uses a custom kernel module (`rp1_pio_sram.ko`) to set up cyclic DMA transfers with configurable ring buffer locations (SRAM or DRAM), burst sizes, and period sizes. Supports unidirectional TX-only and RX-only modes.

```bash
cd throughput-pioloop-cyclic && make
sudo insmod kmod/rp1_pio_sram.ko
sudo ./throughput_pioloop_cyclic --sram
```

[Results](throughput-pioloop-cyclic/RESULTS.md) | [Design](throughput-pioloop-cyclic/DESIGN.md) | [Usage](throughput-pioloop-cyclic/USAGE.md)

### [throughput-pioloop-piolib](throughput-pioloop-piolib/) -- Standard DMA Throughput

Measures round-trip DMA throughput using `pio_sm_xfer_data()` (the standard piolib interface). A 3-instruction PIO program performs bitwise NOT in a loop; TX and RX run concurrently in separate pthreads.

```bash
cd throughput-pioloop-piolib && make
sudo ./throughput_pioloop_piolib --iterations=20
```

[Results](throughput-pioloop-piolib/RESULTS.md) | [Design](throughput-pioloop-piolib/DESIGN.md) | [Usage](throughput-pioloop-piolib/USAGE.md)

### [throughput-gpioloop-piolib](throughput-gpioloop-piolib/) -- GPIO Loopback Throughput

Measures DMA throughput through a 1-bit GPIO serial loopback. PIO serialises each 32-bit word to a single GPIO pin and deserialises on input, creating 32x DMA expansion.

```bash
cd throughput-gpioloop-piolib && make
sudo ./throughput_gpioloop_piolib
```

[Results](throughput-gpioloop-piolib/RESULTS.md) | [Design](throughput-gpioloop-piolib/DESIGN.md) | [Usage](throughput-gpioloop-piolib/USAGE.md)

### [throughput-pioloop-m3poll](throughput-pioloop-m3poll/) -- M3 Core 1 PIO Access

Bootstraps the RP1's second ARM Cortex-M3 core via SEV and measures PIO FIFO throughput with direct CPU polling from the M3 side.

```bash
cd throughput-pioloop-m3poll && make
sudo ./throughput_pioloop_m3poll
```

[Results](throughput-pioloop-m3poll/RESULTS.md) | [Design](throughput-pioloop-m3poll/DESIGN.md) | [Usage](throughput-pioloop-m3poll/USAGE.md)

### [latency-gpioloop](latency-gpioloop/) -- GPIO Latency

Coordinates RPi4 (stimulus) and RPi5 (echo) over SSH to measure round-trip GPIO latency at each abstraction layer. Requires two devices connected via GPIO4/GPIO5 (Pmod JC connector).

```bash
uv run python latency-gpioloop/run.py --tests L0 L1 L2 L3
```

[Results](latency-gpioloop/RESULTS.md) | [Design](latency-gpioloop/DESIGN.md) | [Usage](latency-gpioloop/USAGE.md)

### [frequency-gpiotoggle](frequency-gpiotoggle/) -- Toggle Frequency

Measures GPIO toggle frequency at various PIO clock dividers, using a Glasgow Interface Explorer as an independent frequency counter.

```bash
cd frequency-gpiotoggle && make
uv run python run.py
```

[Results](frequency-gpiotoggle/RESULTS.md) | [Design](frequency-gpiotoggle/DESIGN.md) | [Usage](frequency-gpiotoggle/USAGE.md)

## Hardware Setup

- **RPi5**: RP1 PIO target (runs benchmarks, kernel 6.12+ with PR #6994 and PR #7190)
- **RPi4**: GPIO stimulus/measurement for latency tests (mmap `/dev/gpiomem`, BCM2711)
- **Connection**: Digilent Pmod HAT Adapters on both devices, jumper cables across ports JA, JB, JC (21 GPIO lines)
- **Required kernel patches**:
  - [PR #6994](https://github.com/raspberrypi/linux/pull/6994) (2025-08): Heavy DMA channel reservation
  - [PR #7190](https://github.com/raspberrypi/linux/pull/7190) (2026-01): FIFO threshold fix (prevents data corruption)

See [`hw.md`](hw.md) for the full pin mapping. Additional RP1 documentation is in [`docs/`](docs/).

## References

- [RP1 Peripherals datasheet (PDF)](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf) -- PIO, DMA, and peripheral register maps
- [raspberrypi/linux PR #6994](https://github.com/raspberrypi/linux/pull/6994) -- Heavy DMA channel reservation
- [raspberrypi/linux PR #7190](https://github.com/raspberrypi/linux/pull/7190) -- FIFO threshold fix
- [raspberrypi/utils rp1-pio](https://github.com/raspberrypi/utils/tree/master/rp1-pio) -- piolib userspace library
- [cleverca22's RP1 PIO sigrok driver](https://github.com/cleverca22/libsigrok/commit/e3783bac8176e7454863b37723ab6d8a3f99731a) -- Custom host-side DMA achieving ~66 MB/s
- [cleverca22/rp1-kernel-example](https://github.com/cleverca22/rp1-kernel-example) -- RP1 PIO kernel module example
- [MichaelBell/rp1-hacking](https://github.com/MichaelBell/rp1-hacking) -- PIO register map, Core 1 bootstrap
- [librerpi/rp1-lk](https://github.com/librerpi/rp1-lk) -- Bare-metal Little Kernel OS on RP1 M3 cores
- [G33KatWork/RP1-Reverse-Engineering](https://github.com/G33KatWork/RP1-Reverse-Engineering) -- Firmware extraction and analysis

## License

This project is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
