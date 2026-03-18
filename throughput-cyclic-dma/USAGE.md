# Cyclic DMA Throughput -- Usage

## Prerequisites

- Raspberry Pi 5
- `libpio-dev` installed (provides piolib headers and library)
- Linux kernel headers (for building the kernel module)
- Root access (DMA and `/dev/mem` operations require sudo)

## Build

Build the userspace benchmark tool:

```sh
make sram_dma_bench
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
sudo ./sram_dma_bench --sram

# DRAM ring buffers (host memory via PCIe)
sudo ./sram_dma_bench --dram

# Unidirectional modes
sudo ./sram_dma_bench --rx-only
sudo ./sram_dma_bench --tx-only

# piolib ioctl baseline
sudo ./sram_dma_bench --piolib

# JSON output
sudo ./sram_dma_bench --dram --json

# Custom duration (seconds)
sudo ./sram_dma_bench --dram --duration=5
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
