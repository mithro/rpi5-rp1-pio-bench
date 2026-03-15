# RP1 PIO Firmware Communication Analysis

Analysis of how the Linux kernel PIO driver communicates with the RP1 M3 firmware,
based on `rpi-6.12.y` kernel sources.

## Architecture Overview

```
Userspace (ioctl)
    |
rp1-pio.c (driver)         -- /dev/pio0
    |
rp1-fw.c (firmware client) -- rp1_firmware_message()
    |
rp1-mailbox.c (mailbox)    -- SYSCFG PROC_EVENTS doorbell
    |
[RP1 SRAM shared memory]   -- 0x4040FF00..0x4040FFFF (256 bytes)
    |
M3 firmware (Core 0)       -- IRQ from PROC_EVENTS bit
```

## The Doorbell Mechanism

### Register Addresses (SYSCFG block)

The mailbox controller maps the **SYSCFG** register block at PCIe BAR address
`0xc0_40008000` (size `0x4000`). Within it:

| Offset   | Register               | Direction          | Purpose                          |
|----------|------------------------|--------------------|----------------------------------|
| `0x0008` | `SYSCFG_PROC_EVENTS`   | Host -> M3         | Write bit to interrupt M3        |
| `0x000C` | `SYSCFG_HOST_EVENTS`   | M3 -> Host         | M3 writes bit to interrupt host  |
| `0x0010` | `HOST_EVENT_IRQ_EN`    | Host config        | Enable specific event IRQs       |
| `0x0014` | `HOST_EVENT_IRQ`       | Host read          | Pending host event interrupts    |

Atomic set/clear via hardware set/clear aliases:
- `+0x2000` = `HW_SET_BITS` (atomic OR)
- `+0x3000` = `HW_CLR_BITS` (atomic AND-NOT)

### How `rp1_send_data()` Triggers an M3 Interrupt

```c
// rp1-mailbox.c:rp1_send_data()
static int rp1_send_data(struct mbox_chan *chan, void *data)
{
    struct rp1_mbox *mbox = rp1_chan_mbox(chan);
    unsigned int event = rp1_chan_event(chan);  // = (1 << doorbell_number)

    // Atomic set of the event bit in PROC_EVENTS
    writel(event, mbox->regs + SYSCFG_PROC_EVENTS + HW_SET_BITS);
    //     ^^^^^ = SYSCFG_BASE + 0x0008 + 0x2000 = SYSCFG_BASE + 0x2008

    return 0;
}
```

The firmware channel uses **doorbell 0** (`mboxes = <&rp1_mbox 0>`), so:
- `event = (1 << 0) = 0x1`
- Writing `0x1` to `SYSCFG_BASE + 0x2008` sets bit 0 of `PROC_EVENTS`
- This generates an interrupt on the M3 (IRQ depends on M3's NVIC configuration)

**The M3 interrupt is NOT IRQ59.** IRQ59 (`RP1_INT_SYSCFG`) is the *host-side* IRQ
for `HOST_EVENTS` (M3-to-host direction). The PROC_EVENTS interrupt number on the
M3 side is determined by the M3 firmware's NVIC configuration (not visible in the
Linux kernel sources).

### Absolute Doorbell Address

From the device tree:
- SYSCFG base: `0xc0_40008000` (PCIe BAR space)
- From M3's perspective (RP1 internal bus): `0x40008000`

**To trigger M3 from the RP1 address space:**
```
Write 0x1 to 0x4000A008   (= 0x40008000 + 0x0008 + 0x2000)
```

This is `SYSCFG_PROC_EVENTS + HW_SET_BITS` = the doorbell that wakes the M3 firmware.

## Shared Memory Protocol

### Location

SRAM region `shmem@ff00` within `sram@400000`:
- RP1 internal address: `0x40400000 + 0xFF00 = 0x4040FF00`
- Size: `0x100` (256 bytes)
- PCIe BAR address: `0xc0_4040FF00`

### Message Format

**Request (host writes before ringing doorbell):**
```
Offset 0x00: [31:16] = opcode, [15:0] = data_length
Offset 0x04: request data (up to buf_size - 4 bytes)
```

**Response (M3 writes, then rings HOST_EVENTS doorbell):**
```
Offset 0x00: [31] = error flag, [30:0] = response_length (or negative error code)
Offset 0x04: response data
```

### Transaction Sequence

```c
// rp1-fw.c:rp1_firmware_message()
1. mutex_lock(&transaction_lock)          // serialize access
2. memcpy_toio(&fw->buf[1], data, len)    // write request data to SRAM+4
3. writel((op<<16)|len, fw->buf)          // write opcode|length to SRAM+0
4. reinit_completion(&fw->c)              // prepare to wait
5. mbox_send_message(fw->chan, NULL)       // -> rp1_send_data() -> doorbell!
6. wait_for_completion_timeout(&fw->c, HZ) // wait for M3 response (1 sec)
7. rc = readl(fw->buf)                    // read response header from SRAM+0
8. memcpy_fromio(resp, &fw->buf[1], rc)   // read response data from SRAM+4
9. mutex_unlock(&transaction_lock)
```

The M3 firmware processes the request and rings `HOST_EVENTS` bit 0 to wake the
host. The host's IRQ handler (`rp1_mbox_irq`) calls `mbox_chan_received_data()`,
which calls `response_callback()`, which calls `complete(&fw->c)`.

## PIO Ioctl Classification

### ALL ioctls go through firmware

Every PIO ioctl calls `rp1_pio_message()` or `rp1_pio_message_resp()`, which call
`rp1_firmware_message()`, which rings the doorbell. **There are no kernel-side-only
PIO operations** (some have kernel-side caching/optimization before the firmware call).

The opcode sent is `fw_pio_base + op`, where `fw_pio_base` is obtained at probe time
via `rp1_firmware_get_feature(fw, FOURCC_PIO, &op_base, &op_count)`.

### Ioctls That DEFINITELY Trigger Firmware (all of them)

| Ioctl                  | Firmware Op            | Notes                              |
|------------------------|------------------------|------------------------------------|
| `SM_INIT`              | `PIO_SM_INIT`          | Configures SM                      |
| `SM_SET_CONFIG`        | `PIO_SM_SET_CONFIG`    | Sets SM config                     |
| `SM_EXEC`              | `PIO_SM_EXEC`          | **Executes instruction on SM**     |
| `SM_SET_ENABLED`       | `PIO_SM_SET_ENABLED`   | Enables/disables SM                |
| `SM_PUT`               | `PIO_SM_PUT`           | **Writes FIFO via firmware**       |
| `SM_GET`               | `PIO_SM_GET`           | **Reads FIFO via firmware**        |
| `SM_SET_PINS`          | `PIO_SM_SET_PINS`      | Sets pin values                    |
| `SM_SET_PINDIRS`       | `PIO_SM_SET_PINDIRS`   | Sets pin directions                |
| `SM_CLAIM`             | `PIO_SM_CLAIM`         | Claims SM (kernel tracks too)      |
| `SM_UNCLAIM`           | `PIO_SM_UNCLAIM`       | Releases SM                        |
| `ADD_PROGRAM`          | `PIO_ADD_PROGRAM`      | Kernel caches, then firmware call  |
| `REMOVE_PROGRAM`       | `PIO_REMOVE_PROGRAM`   | Ref-counted, firmware on last ref  |
| `CLEAR_INSTR_MEM`      | `PIO_CLEAR_INSTR_MEM`  | Clears all programs                |
| `SM_CLEAR_FIFOS`       | `PIO_SM_CLEAR_FIFOS`   | Clears SM FIFOs                    |
| `SM_SET_CLKDIV`        | `PIO_SM_SET_CLKDIV`    | Sets clock divider                 |
| `SM_RESTART`           | `PIO_SM_RESTART`       | Restarts SM                        |
| `SM_SET_DMACTRL`       | `PIO_SM_SET_DMACTRL`   | Configures DMA control             |
| `SM_FIFO_STATE`        | `PIO_SM_FIFO_STATE`    | Queries FIFO level/empty/full      |
| `SM_DRAIN_TX`          | `PIO_SM_DRAIN_TX`      | Drains TX FIFO                     |
| `GPIO_INIT`            | `GPIO_INIT`            | Initializes GPIO for PIO           |
| `GPIO_SET_FUNCTION`    | `GPIO_SET_FUNCTION`    | Sets GPIO function                 |
| `GPIO_SET_PULLS`       | `GPIO_SET_PULLS`       | Configures pull-ups/downs          |
| `READ_HW`             | `READ_HW`              | **Reads arbitrary HW register**    |
| `WRITE_HW`            | `WRITE_HW`             | **Writes arbitrary HW register**   |

### Kernel-Side DMA (does NOT go through firmware)

These ioctls are handled entirely in the kernel via Linux DMA engine:
- `SM_CONFIG_XFER` / `SM_CONFIG_XFER32` -- configures DMA channel and bounce buffers
- `SM_XFER_DATA` / `SM_XFER_DATA32` -- performs DMA transfer to/from PIO FIFO

DMA transfers write directly to PIO FIFO registers at `phys_addr + 0x00..0x1C`
(mapped via PCIe BAR), bypassing the M3 firmware entirely. However,
`SM_CONFIG_XFER` does call `SM_SET_DMACTRL` which goes through firmware.

## Key Question: Triggering M3 Interrupt for SEV

### Option 1: Direct PROC_EVENTS Register Write (Recommended)

From code running on RP1 M3 Core 1, write to the PROC_EVENTS register to interrupt
Core 0:

```c
// RP1 internal address for SYSCFG PROC_EVENTS
#define SYSCFG_BASE        0x40008000
#define PROC_EVENTS        0x08
#define HW_SET_BITS        0x2000

// Ring doorbell bit 0 (same as what the Linux host does)
*(volatile uint32_t *)(SYSCFG_BASE + PROC_EVENTS + HW_SET_BITS) = (1 << 0);
```

**However**, this interrupts Core 0's firmware, not wakes Core 1. The PROC_EVENTS
mechanism is for Host-to-M3-Core0 communication.

### Option 2: Use WRITE_HW Ioctl from Host

Any `WRITE_HW` ioctl triggers a firmware message, which:
1. Writes to shared SRAM at `0x4040FF00`
2. Rings PROC_EVENTS bit 0 (doorbell)
3. M3 Core 0 firmware wakes, processes the message
4. M3 Core 0 could theoretically issue SEV to wake Core 1

But this requires the firmware to support Core 1 wake-up, which it does not.

### Option 3: Direct Doorbell for Custom Firmware

If running custom firmware on M3 Core 0, you can:
1. Configure the NVIC to route a PROC_EVENTS bit to an interrupt handler
2. In that handler, issue `SEV` to wake Core 1 from `WFE`
3. From the host, write the chosen bit to `PROC_EVENTS + HW_SET_BITS`

**The doorbell register to write from the host (via PCIe BAR):**
```
PCIe BAR + 0x40008000 + 0x0008 + 0x2000 = BAR + 0x4000A008
```

Write `(1 << N)` where N is the doorbell bit (0-31) your custom firmware listens on.

### Option 4: Inter-Core Communication via SRAM Polling

For Core 0 to wake Core 1 without PROC_EVENTS:
- Core 1 polls a flag in SRAM (e.g., at `0x20000000 + offset`)
- Core 0 writes the flag after receiving a PIO ioctl
- No hardware interrupt needed, but burns CPU cycles

## Summary

| What | Address (RP1 internal) | Address (PCIe BAR-relative) |
|------|------------------------|-----------------------------|
| SYSCFG base | `0x40008000` | `0xc0_40008000` |
| PROC_EVENTS (host->M3 doorbell) | `0x40008008` | `0xc0_40008008` |
| PROC_EVENTS + HW_SET (atomic set) | `0x4000A008` | `0xc0_4000A008` |
| HOST_EVENTS (M3->host doorbell) | `0x4000800C` | `0xc0_4000800C` |
| Shared SRAM (mailbox buffer) | `0x4040FF00` | `0xc0_4040FF00` |
| Shared SRAM size | 256 bytes | 256 bytes |
| PIO FIFO base | `0x40178000` | `0xc0_40178000` |

**Bottom line:** Every PIO ioctl triggers `rp1_firmware_message()` which writes to
shared SRAM at `0x4040FF00` then atomically sets bit 0 of `PROC_EVENTS` at
`0x4000A008`. This generates an interrupt on the M3 Core 0. The specific M3 NVIC
IRQ number for PROC_EVENTS is not in the kernel sources -- it's configured in the
M3 firmware itself.
