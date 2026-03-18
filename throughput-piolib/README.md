# throughput-piolib -- RP1 PIO DMA Throughput Benchmark

Measures DMA throughput through the RPi5 RP1 PIO block using the standard
piolib ioctl API (`pio_sm_xfer_data`). A 3-instruction PIO program loops
data from the TX FIFO through a bitwise NOT transform back to the RX FIFO,
exercising the full round-trip path:

    ARM DRAM --> PCIe --> RP1 DMA --> PIO FIFO --> PIO SM --> PIO FIFO --> RP1 DMA --> PCIe --> ARM DRAM

TX and RX transfers run concurrently in separate pthreads to avoid FIFO
deadlock (the piolib `pio_sm_xfer_data` call is blocking). Data integrity
is verified after each iteration by checking the bitwise NOT relationship
between sent and received buffers.

## Key result

~42 MB/s aggregate throughput with default settings (256 KB transfers,
100 iterations, DMA threshold=8, priority=2). Zero data errors.

## Documentation

- [DESIGN.md](DESIGN.md) -- how the benchmark works
- [RESULTS.md](RESULTS.md) -- measured throughput numbers
- [USAGE.md](USAGE.md) -- build and run instructions, CLI reference
- [FUTURE.md](FUTURE.md) -- planned improvements
