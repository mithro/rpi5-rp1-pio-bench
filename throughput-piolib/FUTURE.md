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

This tests real-world signal integrity over the Pmod jumper cables and exercises the complete external IO path.

**Available connections:** 21 unique GPIO connections between RPi5 and RPi4 across Pmod connectors JA, JB, JC (see `hw.md` for complete pin mapping).

## 3. Async DMA API

The current benchmark uses pthreads to work around `pio_sm_xfer_data()` being a blocking call. If piolib adds a non-blocking variant (e.g., `pio_sm_xfer_data_async()`), the benchmark should be updated to use it. This would:

- Remove thread creation/join overhead from timing
- Enable more precise throughput measurement
- Simplify the code

## 4. Transfer Size Sweep

Automated sweep from 256 bytes to 4 MB to characterise:

- Per-transfer setup overhead
- Optimal transfer size for maximum throughput
- Point of diminishing returns

This would produce a transfer-size vs throughput curve useful for tuning real applications.

## Completed Investigations

The following were previously listed as future work and have been completed:

- **Joined FIFO single-direction benchmarks:** FIFO joining is not applicable for bidirectional loopback (both TX and RX FIFOs needed simultaneously). Unidirectional mode (`--tx-only`/`--rx-only`) achieves higher throughput without joining — RX-only reaches 55.97 MB/s. See `throughput-cyclic-dma/DESIGN.md`.

- **M3 Core bounce buffer:** Implemented and benchmarked (`m3core1/pio_bridge.s`). PIO FIFO access from M3 Core 1 is NOT single-cycle — it takes ~54 cycles (~270 ns) via the APB bus bridge, limiting CPU-polled throughput to 6.89 MB/s. This is 6× slower than cyclic DMA (40-54 MB/s). cleverca22's ~66 MB/s was achieved with host-side DMA ([source](https://github.com/cleverca22/libsigrok/commit/e3783bac8176e7454863b37723ab6d8a3f99731a)), not M3 Core 1 polling. See `throughput-cyclic-dma/DESIGN.md` for full results.
