> **Note:** This repository was generated with the assistance of AI tools
> (Claude). Content should be independently verified before relying on it
> for hardware decisions or production use.

# RP1 PIO Performance Exploration on Raspberry Pi 5

Investigating DMA throughput between the BCM2712 ARM host and the RP1 southbridge's PIO block on the Raspberry Pi 5. The RP1 connects via PCIe 2.0 x4 and contains a PIO block similar to the RP2040 — same instruction set, 4 state machines, but with 8-deep FIFOs and a 200 MHz clock.

## Repository Structure

| Path | Description |
|------|-------------|
| [`hw.md`](hw.md) | Hardware setup — RPi5/RPi4 specs, Digilent Pmod HAT pin mapping, inter-device jumper connections |
| [`rp1-dma.md`](rp1-dma.md) | Deep dive into RP1 DMA architecture, the 10 MB/s wall, and kernel optimisations |
| [`rp1-dma-2.md`](rp1-dma-2.md) | RP1 PIO register map, DMA data path details, performance measurements, and code examples |
| [`resources.md`](resources.md) | Curated collection of datasheets, source repos, PRs, and community projects |
| [`benchmark/`](benchmark/) | PIO internal loopback benchmark — measures round-trip DMA throughput (~42 MB/s measured) |
| [`verify_pmod_connections.py`](verify_pmod_connections.py) | Tests GPIO jumper connections between RPi5 and RPi4 via Pmod HAT |

## Benchmark

The [benchmark](benchmark/) uses a 3-instruction PIO program (bitwise NOT loopback) to measure full-duplex DMA throughput. Data flows from host memory through DMA to the PIO TX FIFO, gets inverted by the state machine, and returns through the RX FIFO back to host memory.

**Measured result: ~42 MB/s aggregate throughput** with zero data errors, significantly exceeding the previously documented ~27 MB/s community results.

See [`benchmark/README.md`](benchmark/README.md) for build instructions, architecture details, and full results.

## Hardware

Two Raspberry Pi devices (RPi5 + RPi4) with Digilent Pmod HAT Adapters, connected via jumper cables across all three Pmod ports (JA, JB, JC — 21 unique GPIO connections).

See [`hw.md`](hw.md) for complete pin mapping and connection details.

## License

This project is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
