> **Note:** This repository was generated with the assistance of AI tools
> (Claude). Content should be independently verified before relying on it
> for hardware decisions or production use.

# RP1 PIO Performance Exploration on Raspberry Pi 5

The RP1 is the southbridge chip on the Raspberry Pi 5, connected to the BCM2712 ARM host via PCIe 2.0 x4. It contains a PIO (Programmable I/O) block compatible with the RP2040 instruction set: 4 state machines, 200 MHz clock, and 8-deep FIFOs.

This repository measures DMA throughput and GPIO latency between the ARM host and the RP1's PIO block using several approaches: standard kernel DMA via `piolib`, cyclic DMA through a custom kernel module with SRAM-backed ring buffers, and direct PIO access from the RP1's M3 Core 1. It also documents RP1 hardware quirks and the kernel patches that affect performance.

## Throughput Results

All throughput values are in **MB/s (megabytes per second)**, not Mbit/s. The RP1 datasheet uses Mbit/s in some places; those figures have been divided by 8 where referenced. Measured on 2026-03-17 (fresh boot, kernel 6.12+, both [PR #6994](https://github.com/raspberrypi/linux/pull/6994) and [PR #7190](https://github.com/raspberrypi/linux/pull/7190) applied).

| Approach | TX (MB/s) | RX (MB/s) | Tool | Notes |
|----------|----------:|----------:|------|-------|
| RX-only DMA, DRAM | -- | 56 | `sram_dma_bench --rx-only` | Single-direction, kernel module |
| Cyclic DMA, SRAM bidirectional | 54 | 45 | `sram_dma_bench --sram` | SRAM ring buffers at 0xA200+ |
| TX-only DMA, DRAM | 41 | -- | `sram_dma_bench --tx-only` | Single-direction, kernel module |
| Cyclic DMA, DRAM bidirectional | 40 | 36 | `sram_dma_bench --dram` | Standard DRAM ring buffers |
| Standard kernel DMA (piolib ioctl) | ~42 | ~42 | `throughput-piolib/pio_loopback` | Concurrent TX+RX via pthreads |
| piolib ioctl (benchmark tool) | 18 | 18 | `sram_dma_bench --piolib` | Sequential ioctl calls |
| M3 Core 1 CPU-polled | 7 | 7 | `m3_bridge_bench` | APB bus bottleneck (see below) |
| cleverca22 custom driver (reference) | -- | ~66 | -- | Host-side DMA with custom driver, not M3 Core 1 |

For detailed analysis including hardware findings, SRAM memory map, firmware reverse-engineering, and DMA configuration, see [`throughput-cyclic-dma/DESIGN.md`](throughput-cyclic-dma/DESIGN.md).

## Latency Results

Measured over 1000 iterations, 50 warmup, GPIO4/GPIO5 (JC connector), kernel 6.12+. RPi4 generates stimulus pulses; RPi5 echoes through PIO at progressively higher abstraction layers.

| Layer | Description | Median | Ratio to L0 |
|-------|-------------|-------:|-----------:|
| L0 | PIO-only echo (no CPU) | 388 ns | 1x |
| L1 | PIO + ioctl round-trip | 44 us | 113x |
| L2 | PIO + single-word DMA | 52 us | 134x |
| L3 | Batched 4 KB DMA read | 89 us | 229x |

L1 is 113x L0 because each `pio_sm_get/put` requires an ioctl through the kernel into the RP1 firmware mailbox. See [latency-gpio/RESULTS.md](latency-gpio/RESULTS.md) for full statistics (min, P95, P99, max, stddev).

## Benchmark Tools

### `throughput-piolib/pio_loopback` -- Standard DMA Throughput

Measures round-trip DMA throughput using `pio_sm_xfer_data()` (the standard piolib interface). A 3-instruction PIO program performs bitwise NOT in a loop; TX and RX run concurrently in separate pthreads.

```bash
cd throughput-piolib && make benchmark
sudo ./pio_loopback --iterations=20
```

### `throughput-cyclic-dma/sram_dma_bench` -- Cyclic DMA Throughput

Uses a custom kernel module (`throughput-cyclic-dma/kmod/rp1_pio_sram.ko`) to set up cyclic DMA transfers with configurable ring buffer locations (SRAM or DRAM), burst sizes, and period sizes.

```bash
cd throughput-cyclic-dma && make
sudo insmod kmod/rp1_pio_sram.ko
sudo ./sram_dma_bench --sram    # SRAM ring buffers
sudo ./sram_dma_bench --dram    # DRAM ring buffers
sudo ./sram_dma_bench --rx-only # Single-direction RX
```

### `throughput-m3-core1/m3_bridge_bench` -- M3 Core 1 Access

Bootstraps the RP1's second ARM Cortex-M3 core via SEV and measures PIO FIFO throughput with direct CPU polling from the M3 side.

```bash
cd throughput-m3-core1 && make
sudo ./m3_bridge_bench
```

### `latency-gpio/run_latency_benchmark.py` -- GPIO Latency

Coordinates RPi4 (stimulus) and RPi5 (echo) over SSH to measure round-trip GPIO latency at each abstraction layer.

```bash
uv run python latency-gpio/run_latency_benchmark.py --tests L0 L1 L2 L3
```

Requires two devices connected via GPIO4/GPIO5 (Pmod JC connector). See [`hw.md`](hw.md) for wiring.

### `throughput-gpio-loopback/gpio_loopback` -- GPIO Loopback Throughput

Measures DMA throughput through a 1-bit GPIO serial loopback. PIO serialises each 32-bit word to a single GPIO pin and deserialises on input, creating 32x DMA expansion.

```bash
cd throughput-gpio-loopback && make
sudo ./gpio_loopback
```

### `toggle-frequency/toggle_rpi5` -- Toggle Frequency

Measures GPIO toggle frequency at various PIO clock dividers, using a Glasgow Interface Explorer as an independent frequency counter.

```bash
cd toggle-frequency && make
uv run python run_toggle_benchmark.py
```

## Hardware Setup

- **RPi5**: RP1 PIO target (runs benchmarks, kernel 6.12+ with PR #6994 and PR #7190)
- **RPi4**: GPIO stimulus/measurement for latency tests (mmap `/dev/gpiomem`, BCM2711)
- **Connection**: Digilent Pmod HAT Adapters on both devices, jumper cables across ports JA, JB, JC (21 GPIO lines)
- **Required kernel patches**:
  - [PR #6994](https://github.com/raspberrypi/linux/pull/6994) (2025-08): Heavy DMA channel reservation
  - [PR #7190](https://github.com/raspberrypi/linux/pull/7190) (2026-01): FIFO threshold fix (prevents data corruption)

See [`hw.md`](hw.md) for the full pin mapping.

## Repository Structure

**Benchmarks:**

| Path | What it measures |
|------|------------------|
| [`throughput-piolib/`](throughput-piolib/) | DMA throughput via standard piolib ioctl API |
| [`throughput-cyclic-dma/`](throughput-cyclic-dma/) | DMA throughput via cyclic DMA with SRAM/DRAM ring buffers |
| [`throughput-m3-core1/`](throughput-m3-core1/) | PIO FIFO throughput via M3 Core 1 CPU polling |
| [`throughput-gpio-loopback/`](throughput-gpio-loopback/) | DMA throughput via 1-bit GPIO serial loopback |
| [`latency-gpio/`](latency-gpio/) | Round-trip GPIO latency (L0-L3 abstraction layers) |
| [`toggle-frequency/`](toggle-frequency/) | GPIO toggle frequency at various PIO clock dividers |

**Documentation and tools:**

| Path | Description |
|------|-------------|
| [`hw.md`](hw.md) | Hardware setup, Pmod HAT pin mapping, jumper connections |
| [`docs/`](docs/) | RP1 DMA architecture, PIO register maps, firmware protocol, resources |
| [`verify_pmod_connections.py`](verify_pmod_connections.py) | GPIO connection test script for Pmod wiring |

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
