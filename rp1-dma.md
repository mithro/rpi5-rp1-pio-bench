# High-performance DMA transfers between ARM and RP1 PIO on Raspberry Pi 5

**Achieving >10 MB/s full-duplex DMA transfers between the BCM2712 host CPU and RP1's PIO block is now practical**, with recent kernel patches pushing sustained throughput from a ~10 MB/s ceiling to **~27 MB/s per direction**. The primary bottleneck was never the PCIe link or PIO clock speed — it was suboptimal DMA burst configuration in the kernel driver. This report covers the complete architecture, DMA mechanics, optimization techniques, kernel integration, and working code examples for anyone building high-throughput PIO applications on the Pi 5.

The RP1 chip acts as the Pi 5's southbridge, connected via **PCIe 2.0 x4** to the BCM2712 SoC. Its PIO block is architecturally similar to the RP2040's but sits behind a firmware mailbox, making direct register access from Linux impossible. All PIO data transfer must flow through DMA from host memory, across PCIe, through RP1's internal DMA controller, into the PIO FIFOs. Understanding this path — and where it constrains bandwidth — is essential to achieving high performance.

---

## RP1 PIO is half an RP2040 behind a PCIe wall

The RP1 contains a **single PIO block with 4 state machines**, 32 words of shared instruction memory, and **8-entry FIFOs per state machine** (double the RP2040's 4-entry FIFOs). The PIO clock runs at **200 MHz** from the `pll_sys` clock tree. Internally, RP1 houses dual ARM Cortex-M3 cores with 16 KB private SRAM each and 64 KB shared SRAM — a significant upgrade from the RP2040's Cortex-M0+ cores.

The critical architectural constraint is that **PIO configuration registers live at address `0xF000_0000`, accessible only to the M3 cores** — they cannot be reached over PCIe from the host ARM CPU. Only the PIO TX/RX FIFOs (mapped at `0x40178000` in RP1's address space) are exposed through PCIe BAR0. This means every PIO setup operation — loading programs, configuring state machines, setting clock dividers — must travel through a mailbox RPC to the RP1 firmware running on core0, incurring **≥10 μs latency per operation**. The PIO instruction set uses the same 9 opcodes as RP2040 (JMP, WAIT, IN, OUT, PUSH, PULL, MOV, IRQ, SET), but register-level differences mean **there is no binary compatibility** with RP2040 PIO configurations. As Raspberry Pi engineer Luke Wren stated: "In terms of PIO functionality it is half an RP2040."

The comparison with RP2040 reveals important trade-offs:

| Feature | RP2040 | RP1 (Pi 5) |
|---------|--------|-------------|
| PIO blocks / state machines | 2 / 8 total | 1 / 4 total |
| FIFO depth per SM | 4 entries | **8 entries** |
| PIO clock | 125–200 MHz | **200 MHz** |
| Host access to PIO registers | Direct | **Firmware RPC only** |
| FIFO access from host | Direct memory-mapped | **Over PCIe (~320 ns latency)** |
| DMA channels | 12 | 8 |
| Local CPU | 2× Cortex-M0+ | 2× Cortex-M3 |

The doubled FIFO depth partially compensates for PCIe latency by providing more buffering before overflow occurs. When using `PIO_FIFO_JOIN_TX` or `PIO_FIFO_JOIN_RX`, the effective depth reaches **16 entries** (64 bytes), which is critical for sustaining throughput during DMA burst gaps.

---

## The DMA data path traverses two bus domains

Data flowing between host SDRAM and PIO FIFOs passes through a multi-stage pipeline. **Only RP1's internal 8-channel Synopsys DesignWare DMA controller handles PIO transfers** — the BCM2712's own DMA engine is not involved. The complete path is:

```
Host SDRAM ←→ BCM2712 PCIe Root Complex ←→ PCIe 2.0 x4 Link (2 GB/s)
    ←→ RP1 PCIe Endpoint ←→ RP1 AXI Fabric (128-bit, 100 MHz)
        ←→ RP1 DMA Controller ←→ PIO FIFO (32-bit interface)
```

The PCIe 2.0 x4 link provides **~16 Gbps (gigabits/s) effective bandwidth** (~2 GB/s (gigabytes/s)) with **~320 ns to ~1 μs latency** per transaction. This is emphatically not the bottleneck — it has over 100× headroom for PIO transfers. The real constraints emerge inside RP1 itself.

RP1's DMA controller connects to an **internal 128-bit AXI bus clocked at 100 MHz**, yielding 1.6 GB/s raw internal bandwidth. However, the PIO FIFO interface is only **32-bit wide**, meaning **75% of the DMA bus bandwidth is structurally wasted** on PIO transfers. The RP1 datasheet specifies typical per-channel read bandwidth of **500–600 Mbps (megabits/s)** and write bandwidth of **2 Gbps (gigabits/s)**. For the 32-bit PIO path, this translates to a theoretical ceiling of roughly **62–75 MB/s (megabytes/s)** per direction.

The DMA controller has **heterogeneous channels**: channels 0 and 1 are "heavy" channels supporting **MSIZE=8** (8-beat bursts of 32 bits = 32 bytes per burst), while the remaining channels support only MSIZE=4 or less. This distinction proved to be the key performance differentiator, as detailed in the optimization section.

In the kernel driver (`drivers/misc/rp1-pio.c`), DMA is configured using a ring of **4 bounce buffers, each 4 KB by default**:

```c
#define DMA_BOUNCE_BUFFER_SIZE 0x1000   // 4 KB per bounce buffer
#define DMA_BOUNCE_BUFFER_COUNT 4       // 4 buffers in ring

struct dma_info {
    struct dma_chan *chan;
    size_t buf_size;
    unsigned int head_idx, tail_idx;
    struct dma_buf_info bufs[DMA_BOUNCE_BUFFER_COUNT];
};
```

Each transfer uses the standard Linux DMA engine API: `dmaengine_prep_slave_sg()` prepares scatter-gather descriptors, a completion callback advances the ring buffer head, and `copy_from_user`/`copy_to_user` moves data between userspace and bounce buffers. The PIO's DMA request (DREQ) signals pace the transfer — DMA only reads/writes when the FIFO has space or data available.

---

## Breaking the 10 MB/s wall requires kernel patches and correct burst configuration

Before August 2025, every benchmark hit the same ceiling: **~10.75 MB/s regardless of PIO clock speed**. Adafruit engineer Jeff Epler's systematic benchmarks on GitHub Issue #116 demonstrated this dramatically:

```
PIO at   1 MHz → 3.99 MB/s  (FIFO-limited, expected)
PIO at   2 MHz → 7.97 MB/s  (FIFO-limited, expected)  
PIO at   5 MHz → 10.75 MB/s ← hits wall
PIO at  10 MHz → 10.75 MB/s ← same
PIO at 200 MHz → 10.75 MB/s ← same
```

This proved the bottleneck was not the PIO clock (which can theoretically sustain **800 MB/s** at 200 MHz with 32-bit autopush) but the DMA software configuration. Two kernel patches eliminated this limitation:

**PR #6994** (merged August 19, 2025) delivered the largest improvement. It reserves DMA channels 0 and 1 — the heavy channels with MSIZE=8 support — specifically for PIO, increases the DMA burst size to 8 beats, and adds per-channel burst limit configuration in the device tree. **Result: throughput jumped from ~10 MB/s to ~27 MB/s**, a 2.5× improvement.

**PR #7190** (January 2026) fixed a subtle data corruption bug where the FIFO DMA request threshold didn't match the burst size. When a PIO program produces data slower than one word per DMA burst interval, the DMA controller would attempt to read more words than available, causing **underflow corruption** — manifesting as corrupted data after approximately 16 words. The fix ensures the FIFO threshold always equals the DMA burst size. The workaround before this patch was manually setting the DMA control register: `pio_sm_set_dmactrl(pio, sm, false, 0xc0000108)`.

For achieving >10 MB/s full duplex with current software, the recommended configuration is:

- **Update kernel** to include both PR #6994 and PR #7190 (`sudo rpi-update` on recent builds)
- **Set PIO clock divider to 1.0** for maximum 200 MHz operation
- **Join FIFOs** using `sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX)` or `PIO_FIFO_JOIN_RX` to double buffer depth to 16 entries
- **Use 32-bit autopush/autopull** with `sm_config_set_out_shift(&c, false, true, 32)` to maximize data per FIFO entry
- **Use large DMA buffers** — at least 64 KB per transfer to amortize per-transfer overhead; 1 MB transfers work fine
- **Minimize PIO instructions per data word** — ideally 1 instruction per push/pull cycle

With these settings, **~27 MB/s per direction is achievable**, giving roughly **~50 MB/s aggregate** for full-duplex operation. Full duplex requires two DMA channels running simultaneously (one TX, one RX), preferably both using heavy channels 0 and 1. The theoretical per-channel limit of **62–75 MB/s** suggests further driver improvements could unlock additional headroom. Before the official driver, community member cleverca22 demonstrated **~66 MB/s** through direct register hacking (bypassing the kernel stack entirely), confirming the hardware can sustain much higher rates.

---

## Working code examples from piolib and community projects

The official userspace library **piolib** lives in the `raspberrypi/utils` repository and mirrors the Pico SDK API. The critical DMA functions are `pio_sm_config_xfer()` for setup and `pio_sm_xfer_data()` for execution. Here is the core pattern for DMA-based PIO transfers:

```c
#include "piolib.h"

// Configure DMA: direction, buffer size, buffer count
pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, 65536, 1);   // TX: 64KB buffer
pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, 65536, 1);  // RX: 64KB buffer

// Execute blocking DMA transfer
pio_sm_xfer_data(pio, sm, PIO_DIR_TO_SM, sizeof(data), data);    // Send
pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, sizeof(data), data);  // Receive
```

The benchmark program from GitHub Issue #116 demonstrates throughput measurement with a minimal PIO program that simply consumes FIFO data:

```c
// PIO program: single instruction consuming one 32-bit word per cycle
static const uint16_t bench_program_instructions[] = { 0x6020 }; // out x, 32
static const struct pio_program bench_program = {
    .instructions = bench_program_instructions, .length = 1, .origin = -1,
};

long databuf[1048576];
int main(int argc, const char **argv) {
    PIO pio = pio0;
    int sm = pio_claim_unused_sm(pio, true);
    pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, 65536, 1);
    uint offset = pio_add_program(pio, &bench_program);
    // ... init with autopull, TX join, clock config ...
    double t0 = monotonic();
    size_t xfer = 0;
    do {
        pio_sm_xfer_data(pio, sm, PIO_DIR_TO_SM, sizeof(databuf), databuf);
        xfer += sizeof(databuf);
    } while (monotonic() - t0 < 1);
    printf("Rate: %g MB/s\n", xfer / (monotonic() - t0) / 1e6);
}
```

For DMA **receive** (reading from PIO), the self-generating counter pattern serves as an effective loopback test without external hardware:

```c
// PIO program: generates known sequence into RX FIFO
// .program gencounter
//     set y, 10
// loop:
//     mov isr, y
//     push block
//     jmp y--, loop
//     jmp loop

pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, 1048576, 1);
uint8_t data_block[1048576];
while (1) {
    pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, sizeof(data_block), data_block);
    write(STDOUT_FILENO, data_block, sizeof(data_block));  // pipe to validator
}
```

For **true GPIO loopback** (bidirectional), the PIO SPI loopback pattern from pico-examples is directly adaptable to RP1: map MOSI and MISO to the same GPIO pin so serialized output feeds directly back into the input. This validates the complete TX → GPIO → RX data path including DMA in both directions.

Notable community projects using RP1 PIO with DMA include the **DSHOT motor control driver** (`Marian-Vittek/raspberry-pi-dshot-pio`) supporting up to 26 motors, cleverca22's **sigrok logic analyzer** fork using PIO DMA for signal capture, and the official **WS2812 NeoPixel example** (`piolib/examples/piotest.c`) which was the first published DMA transfer demonstration.

---

## The kernel stack spans five modules from PCIe to userspace

The Linux kernel integration follows a layered architecture with five key modules:

**`rp1` (drivers/mfd/rp1.c)** — The MFD (Multi-Function Device) PCIe driver that enumerates RP1 and creates platform sub-devices for all peripherals. It maps BAR0 (peripheral registers at `0x40000000`) and BAR1 (64 KB shared SRAM at `0x20000000`).

**`rp1-mailbox` (drivers/mailbox/rp1-mailbox.c)** — Implements the doorbell-based mailbox using `SYSCFG_PROC_EVENTS` (host→RP1) and `SYSCFG_HOST_EVENTS` (RP1→host) registers. Provides 4 mailbox channels over the standard Linux mailbox framework.

**`rp1-firmware` (drivers/firmware/rp1.c)** — Firmware communication layer using mailbox + shared memory for synchronous request/response messages. The function `rp1_firmware_message(fw, op, data, len, resp, resp_size)` is the fundamental RPC primitive.

**`rp1-pio` (drivers/misc/rp1-pio.c)** — The main PIO driver, authored by Phil Elwell. Creates the **`/dev/pio0` character device** and exposes both an ioctl interface for userspace and exported kernel functions for in-kernel consumers. It manages program loading, state machine lifecycle, GPIO configuration, and all DMA operations. The ioctl interface includes over 30 operations spanning `PIO_IOC_SM_CONFIG_XFER` and `PIO_IOC_SM_XFER_DATA` for DMA, plus operations for every PIO configuration primitive.

**In-kernel PIO consumers** — `pwm-pio-rp1` (PWM on any GPIO), `ws2812-pio-rp1` (NeoPixel driver), and `rp1_dpi_pio.c` (DPI composite sync) demonstrate the kernel-side PIO API. These use the same `pio_rp1.h` header that defines `PIO` as an opaque client handle and wraps all firmware operations.

The complete call chain for a userspace DMA transfer is:

```
piolib: pio_sm_xfer_data(pio, sm, dir, len, buf)
  → ioctl(fd, PIO_IOC_SM_XFER_DATA, &args)
    → rp1_pio_sm_xfer_data() in kernel
      → dmaengine_prep_slave_sg() → dmaengine_submit() → dma_async_issue_pending()
        → RP1 DMA controller reads/writes PIO FIFO via AXI bus
          → PIO state machine processes data
      → wait_for_completion (callback fires semaphore)
    → copy_to_user() / copy_from_user() for bounce buffer
  → returns to userspace
```

Current limitations include: `pio_sm_xfer_data()` is **blocking with no async variant**, no interrupt/callback support for PIO IRQs from userspace, and `pio_sm_get_blocking()` busy-waits at 100% CPU. The `data_bytes` field was originally limited to 16 bits (65,532 byte maximum) but has been expanded in recent releases.

---

## Key documentation and where the remaining gaps are

The **RP1 Peripherals Datasheet** (`datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf`) covers GPIO, UART, SPI, I2C, DMA, clocks, and address maps across ~90 pages — but **the PIO section is conspicuously absent**, with only a single reference in the peripheral address map at offset `0x40178000`. The most complete PIO register documentation comes from **MichaelBell's reverse-engineering work** (`github.com/MichaelBell/rp1-hacking/blob/main/PIO.md`), which maps the register layout as "generally very similar to the RP2040."

The official **PIOLib announcement blog post** (`raspberrypi.com/news/piolib-a-userspace-library-for-pio-control/`) provides the canonical description of the mailbox architecture and performance expectations. The **piolib source code** (`github.com/raspberrypi/utils/tree/master/piolib`) and its examples directory contain the best working code references. For kernel internals, the **rp1-pio pull request #6470** and **performance PR #6994** contain detailed engineering discussion about DMA configuration trade-offs.

The data path from ARM CPU to PIO FIFO traverses these address domains:

```
ARM virtual address (Linux) 
  → BCM2712 physical → PCIe TLP → RP1 BAR0 decode
    → RP1 internal 0x40178000 (PIO FIFO registers)
      → TX FIFO: offsets 0x00/0x04/0x08/0x0C (SM 0-3)
      → RX FIFO: offsets 0x10/0x14/0x18/0x1C (SM 0-3)
```

For DMA, RP1 sees host system RAM at address range `0x10_00000000` through the PCIe address translation, allowing it to DMA directly to/from host SDRAM without CPU involvement during the transfer itself.

---

## Conclusion

The RP1 PIO subsystem on Pi 5 is architecturally capable of far higher throughput than early software limitations suggested. The **~10 MB/s wall** that frustrated early adopters was a DMA burst configuration issue, now resolved by kernel PR #6994 which unlocked **~27 MB/s** — well above the >10 MB/s full-duplex target. The theoretical hardware ceiling sits at **62–75 MB/s per direction**, leaving substantial room for future driver improvements.

Three insights stand out from this investigation. First, the PIO clock speed is essentially irrelevant for throughput — even at 2.5 MHz, PIO can sustain 10 MB/s with 32-bit transfers; the bottleneck is always the DMA software path. Second, the heterogeneous DMA channel architecture (heavy channels 0–1 vs. lighter channels 2–7) means channel selection has outsized impact on performance — getting this wrong costs 2.5× throughput. Third, the firmware mailbox architecture introduces an inescapable **≥10 μs latency floor** per PIO operation, making DMA the only viable path for bulk data transfer — single-word `pio_sm_put_blocking()` calls max out at a mere ~250 KB/s.

For developers targeting high-throughput applications: ensure your kernel includes the August 2025 and January 2026 DMA patches, use large transfer sizes (≥64 KB), join FIFOs for maximum depth, configure 32-bit autopush/autopull, and let DMA handle all bulk data movement. The piolib API, while still maturing (no async transfers, no cyclic DMA), provides a functional and improving foundation for production use.