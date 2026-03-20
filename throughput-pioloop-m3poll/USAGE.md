# M3 Core 1 PIO Bridge -- Usage

## Prerequisites

- Raspberry Pi 5
- `arm-none-eabi-gcc` toolchain (for firmware assembly)
- `libpio-dev` (for PIO setup in m3_bridge_bench)
- Root access (`sudo`) for `/dev/mem` mapping

Install on Debian/Ubuntu:
```
sudo apt install gcc-arm-none-eabi libpio-dev
```

## Build

```
make
```

This builds all firmware binaries and host tools:
- Firmware: `sram_write_test.bin`, `pio_fifo_test.bin`, `pio_bridge.bin`,
  `pio_addr_test.bin`, `clock_test.bin`
- Host tools: `core1_launcher`, `m3_bridge_bench`

## Running

### core1_launcher

Loads firmware to SRAM and bootstraps M3 Core 1.

```
sudo ./core1_launcher -f <firmware.bin> [-m seconds]
```

Options:
- `-f FILE` -- Firmware binary to load (required)
- `-m SECS` -- Monitor duration in seconds

Example:
```
sudo ./core1_launcher -f sram_write_test.bin -m 5
```

### m3_bridge_bench

Runs the full PIO FIFO bridge benchmark. Sets up PIO SM3 with a loopback
program, launches `pio_bridge.bin` on Core 1, and measures throughput.

```
sudo ./m3_bridge_bench [-t secs] [-v] [-p pattern] [-j]
```

Options:
- `-t SECS` -- Benchmark duration in seconds (default: 5)
- `-v` -- Enable verify mode (checks RX data = bitwise NOT of TX data)
- `-p PAT` -- TX data pattern: `seq`, `walk`, `random`, `fixed` (default: `seq`)
- `-j` -- Output results as JSON

Example:
```
sudo ./m3_bridge_bench -t 10 -v -p seq
```

### Available Firmware Binaries

| Binary               | Purpose                                       |
|----------------------|-----------------------------------------------|
| sram_write_test.bin  | Basic SRAM write test to verify Core 1 bootstrap |
| pio_fifo_test.bin    | PIO FIFO loopback test (TXF3 write, RXF3 read)  |
| pio_bridge.bin       | Full SRAM-to-FIFO bridge (used by m3_bridge_bench) |
| pio_addr_test.bin    | Compare PIO access at 0xF0000000 vs 0x40178000   |
| clock_test.bin       | Tight SRAM loop to measure M3 clock speed         |

## Recovery

If firmware hangs or the RP1 enters a bad state, a Linux reboot is NOT
sufficient to reset the RP1 -- the M3 cores and PIO hardware retain state across
host reboots.

A full PoE power cycle is required. For the test rig (Netgear GSM7252PS at
10.1.5.23, port 1/0/1):

```
# Power off
snmpset -v2c -c private 10.1.5.23 1.3.6.1.2.1.105.1.1.1.3.1.1 i 2
# Wait ~5 seconds, then power on
snmpset -v2c -c private 10.1.5.23 1.3.6.1.2.1.105.1.1.1.3.1.1 i 1
```

Requires the `snmp` package (`sudo apt install snmp`).
