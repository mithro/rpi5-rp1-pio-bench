> **Note:** This document was generated with the assistance of AI tools
> (Claude). Links and descriptions should be independently verified.

# RP1 PIO Resources

Curated collection of documentation, source code, and community projects related to RP1 PIO on the Raspberry Pi 5.

## Official Documentation

- [RP1 Peripherals Datasheet](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf) — covers GPIO, UART, SPI, I2C, DMA, clocks, and address maps (~90 pages). PIO section is notably absent; only listed in the address table at `0x40178000`.
- [PIOLib Announcement Blog Post](https://www.raspberrypi.com/news/piolib-a-userspace-library-for-pio-control/) — canonical description of the mailbox architecture, userspace API, and performance expectations.

## Source Code

- [PIOLib (userspace SDK)](https://github.com/raspberrypi/utils/tree/master/piolib) — Pico SDK-compatible API wrapping ioctl calls to `/dev/pio0`. Includes WS2812, PWM, quadrature encoder, and DPI sync examples.
- [rp1-pio kernel driver](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/misc/rp1-pio.c) — handles DMA allocation, firmware mailbox proxying, and ioctl interface. Creates the `/dev/pio0` character device.
- [In-kernel PIO API header](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/include/linux/pio_rp1.h) — enables kernel modules to use PIO directly. Reference implementations in `pwm-pio-rp1.c` and `rp1_dpi_pio.c`.

## Performance-Related Kernel PRs

- [PR #6994](https://github.com/raspberrypi/linux/pull/6994) (August 2025) — reserves heavy DMA channels 0/1 for PIO, increases burst size to 8 beats. Throughput jumped from ~10 MB/s to ~27 MB/s.
- [PR #7190](https://github.com/raspberrypi/linux/pull/7190) (January 2026) — fixes FIFO DMA request threshold to match burst size, resolving data corruption after ~16 words when PIO produces data slower than one word per burst interval.

## Community Projects

- [MichaelBell/rp1-hacking](https://github.com/MichaelBell/rp1-hacking) — primary source for reverse-engineered PIO register documentation (`PIO.md`), including DMACTRL register format and address mappings.
- [librerpi/rp1-lk](https://github.com/librerpi/rp1-lk) — bare-metal RP1 code (Little Kernel) with direct PIO register manipulation running on the M3 cores. Demonstrated ~66 MB/s throughput bypassing the kernel stack.
- [Marian-Vittek/raspberry-pi-dshot-pio](https://github.com/Marian-Vittek/raspberry-pi-dshot-pio) — DSHOT motor control driver using RP1 PIO with DMA, supports up to 26 motors.
- cleverca22's sigrok fork — logic analyser using PIO DMA for signal capture (referenced in community discussions).

## Discussion and Issue Tracking

- [raspberrypi/utils Issue #116](https://github.com/raspberrypi/utils/issues/116) — tracks DMA throughput limitations. Contains Jeff Epler's systematic benchmarks demonstrating the 10 MB/s wall.
- [Raspberry Pi Forum: PIO DMA Performance](https://forums.raspberrypi.com/viewtopic.php?t=390556) — most active thread on PIO DMA performance, featuring direct responses from Raspberry Pi engineers (jdb, pelwell).

## Hardware

- [Digilent Pmod HAT Adapter Reference Manual](https://digilent.com/reference/add-ons/pmod-hat/reference-manual) — pinout and specifications for the Pmod HAT used in this project's hardware setup.
- [Pmod Interface Specification v1.3.1](https://digilent.com/reference/_media/reference/pmod/pmod-interface-specification-1_3_1.pdf) — defines the standard Pmod connector types (GPIO, SPI, UART, I2C, etc.) and signal assignments.
