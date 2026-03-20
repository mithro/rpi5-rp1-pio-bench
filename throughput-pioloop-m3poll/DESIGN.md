# M3 Core 1 PIO Bridge -- Design

## M3 Core 1 Bootstrap

Core 1 cannot be woken directly from the host. The bootstrap sequence is:

1. Load firmware binary to SRAM via BAR2.
2. Write entry point and stack pointer to SYSCFG scratch registers.
3. Reset and release Core 1 (enters WFE in boot ROM).
4. Patch an IRQ vector table entry to point to a small SEV stub at 0x7000.
5. When Core 0 takes that IRQ, it executes the stub: issues SEV, restores the
   original vector entry, and tail-calls the real handler.
6. Core 1 wakes from WFE, reads the entry point, and begins executing firmware.

The SEV stub needs no push/pop because ARM IRQ entry hardware automatically
saves r0-r3, r12, lr, pc, xPSR.

## Firmware Architecture (pio_bridge.s)

The firmware runs a continuous loop moving data through PIO SM3:

```
SRAM TX buffer (0x20009000) --> TXF3 write --> PIO SM3 (pull->NOT->push) --> RXF3 read --> SRAM RX buffer (0x2000A000)
```

Per-word data path:
1. Load word from TX buffer in SRAM.
2. Write to TXF3 (SM3 auto-pulls from TX FIFO).
3. DSB + small delay (wait for PIO to process).
4. Read from RXF3 (SM3 auto-pushes to RX FIFO).
5. Store to RX buffer in SRAM.

Status area at 0x20008D00 reports magic, heartbeat counter, pass/fail result,
words processed, error count, and a host command word (0=stop, 1=run,
2=run+verify).

## PIO Setup Ordering

PIO program loading and SM3 configuration MUST happen AFTER Core 1 launch. The
`trigger_pio_mailbox` call during bootstrap causes Core 0 firmware to refresh
PIO instruction memory. If PIO is configured before this refresh, the loaded
program is overwritten.

## PIO Address Mappings

The RP1 exposes PIO registers at two addresses from the M3:

| Address      | Access Rate     | FSTAT     | Notes                              |
|--------------|-----------------|-----------|------------------------------------|
| 0xF0000000   | 6.7M reads/sec  | Correct   | Vendor-specific alias, 1.41x faster|
| 0x40178000   | 4.76M reads/sec | Incorrect | Standard peripheral bus, slower    |

Both traverse the APB bridge. The firmware uses 0xF0000000 for all FIFO access.

## FSTAT Limitation

FSTAT at 0xF0000000 returns the correct initial value (0x0F000F00, all FIFOs
empty) but does NOT dynamically update when FIFO state changes. Polling RXEMPTY
or TXFULL from Core 1 hangs indefinitely. The firmware uses fixed delays instead
of FSTAT polling.

## APB Bus Path

The measured ~54 cycles per PIO register access is explained by the bus path:

```
M3 Core --> AHB-Lite System Bus --> 32-to-40-bit Converter --> AXI Fabric --> AHB-to-APB Bridge --> PIO
```

This contrasts with the RP2040 where PIO sits directly on a zero-wait-state
AHB-Lite crossbar (~2-7 cycles).

## Core 0 Interference

Core 0 firmware periodically accesses shared bus resources, causing data
corruption at approximately buffer index 62. This occurs roughly once per 8-10
complete passes through the 1024-word buffer. The interference is visible as
single-word errors in verify mode.

## Source Files

| File               | Description                                            |
|--------------------|--------------------------------------------------------|
| pio_bridge.s       | Core 1 firmware: SRAM TX buf -> TXF3 -> RXF3 -> SRAM RX buf |
| throughput_pioloop_m3poll.c | Host benchmark: sets up PIO SM3, launches firmware, measures throughput |
| core1_launcher.c   | Host tool: loads firmware to SRAM, bootstraps Core 1 via SEV |
| test_pio_fifo.s    | Core 1 firmware: basic PIO FIFO loopback test (write TXF3, read RXF3) |
| test_pio_addr.s    | Core 1 firmware: compares PIO access at 0xF0000000 vs 0x40178000 |
| test_clock.s       | Core 1 firmware: tight SRAM loop to measure M3 clock speed |
| test_sram_write.s  | Core 1 firmware: basic SRAM write test to verify Core 1 bootstrap |
| memmap_core1.ld    | Linker script for Core 1 firmware binaries              |
| Makefile           | Builds all firmware .bin files and host tools            |
