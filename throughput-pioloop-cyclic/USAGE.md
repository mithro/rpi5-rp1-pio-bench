# Cyclic DMA Throughput -- Usage

## Prerequisites

- Raspberry Pi 5
- `libpio-dev` installed (provides piolib headers and library)
- Linux kernel headers (for building the kernel module)
- Root access (DMA and `/dev/mem` operations require sudo)

## Build

Build the userspace benchmark tool:

```sh
make throughput_pioloop_cyclic
```

Build the kernel module:

```sh
cd kmod && make
```

Build all probe/diagnostic tools:

```sh
make all
```

## Load Kernel Module

```sh
sudo insmod kmod/rp1_pio_sram.ko
```

This creates `/dev/rp1_pio_sram` for userspace mmap and ioctl access.

## Run Benchmark

```sh
# SRAM ring buffers (RP1-internal, highest throughput)
sudo ./throughput_pioloop_cyclic --sram

# DRAM ring buffers (host memory via PCIe)
sudo ./throughput_pioloop_cyclic --dram

# Unidirectional modes
sudo ./throughput_pioloop_cyclic --rx-only
sudo ./throughput_pioloop_cyclic --tx-only

# piolib ioctl baseline
sudo ./throughput_pioloop_cyclic --piolib

# JSON output
sudo ./throughput_pioloop_cyclic --dram --json

# Custom duration (seconds)
sudo ./throughput_pioloop_cyclic --dram --duration=5
```

## Automated Testing

Run multiple iterations with summary statistics:

```sh
sudo ./run_tests.sh [--iterations=N] [--duration=S] [--json]
```

Default: 10 iterations, 1 second each.

## Diagnostic Tools

| Tool | Purpose |
|------|---------|
| `sram_probe` | SRAM access verification and bandwidth measurement |
| `fifo_probe` | Direct PIO FIFO access via `/dev/mem` |
| `sram_corruption_test` | SRAM DMA firmware health diagnostic |
| `pio_unclaim` | Release all PIO state machine claims |
| `sram_monitor` | Monitor SRAM for dynamic firmware changes |
| `sram_region_test` | Detect which SRAM regions are safe to write |
| `sram_addr_probe` | Discover DMA-accessible SRAM addresses |

All diagnostic tools require root access and an RPi5 with `/dev/pio0`.

## Unload Kernel Module

```sh
sudo rmmod rp1_pio_sram
```
