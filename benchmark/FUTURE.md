# Future Improvements

Planned extensions to the RP1 PIO benchmark suite.

## 1. GPIO Loopback (Single Wire)

Connect one RPi5 GPIO as PIO output to another GPIO as PIO input on the same board. This tests the complete GPIO data path including pad drivers, output enable, and input synchronisers — components not exercised by the internal loopback.

**Approach:** Use `sm_config_set_out_pins()` and `sm_config_set_in_pins()` with two different GPIOs connected by a jumper wire. The PIO program serialises data out through one pin and deserialises through the other.

**Expected result:** Similar throughput to internal loopback since the DMA path is identical — GPIO adds negligible latency compared to the DMA handshake overhead.

## 2. GPIO Loopback via RPi4

Use the existing Pmod jumper cable connections between RPi5 and RPi4 (documented in `hw.md`) to test inter-device data transfer:

- RPi5 PIO outputs data on GPIO pins through the Pmod HAT connector
- RPi4 reads the data (via GPIO or SPI peripheral) and responds
- RPi5 PIO reads the response back

This tests real-world signal integrity over the Pmod jumper cables and exercises the complete external IO path. The RPi4 side would use standard GPIO or SPI since it has no PIO block.

**Available connections:** 21 unique GPIO connections between RPi5 and RPi4 across Pmod connectors JA, JB, JC (see `hw.md` for complete pin mapping).

## 3. Async DMA API

The current benchmark uses pthreads to work around `pio_sm_xfer_data()` being a blocking call. If piolib adds a non-blocking variant (e.g., `pio_sm_xfer_data_async()`), the benchmark should be updated to use it. This would:

- Remove thread creation/join overhead from timing
- Enable more precise throughput measurement
- Simplify the code

## 4. Joined FIFO Single-Direction Benchmarks

Separate TX-only and RX-only benchmarks using `PIO_FIFO_JOIN_TX` or `PIO_FIFO_JOIN_RX` to double the FIFO depth to 16 entries. This would measure maximum single-direction throughput, which should be higher than the bidirectional loopback since joined FIFOs provide more buffering for burst absorption.

## 5. Transfer Size Sweep

Automated sweep from 256 bytes to 4 MB to characterise:

- Per-transfer setup overhead
- Optimal transfer size for maximum throughput
- Point of diminishing returns

This would produce a transfer-size vs throughput curve useful for tuning real applications.

## 6. M3 Core Bounce Buffer

If official support for user code on RP1's Cortex-M3 cores becomes available, implement a bounce buffer strategy:

- M3 core reads PIO FIFO with single-cycle latency into shared SRAM
- DMA transfers from shared SRAM (wider bus) to host memory

This bypasses the 70-cycle DMA handshake bottleneck entirely. Community experiments suggest this path can reach ~66 MB/s. See `rp1-dma-2.md` for architecture details.
