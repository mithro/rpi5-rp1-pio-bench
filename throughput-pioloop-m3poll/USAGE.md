# M3 Core 1 PIO Bridge -- Usage

## Prerequisites

- Raspberry Pi 5
- `arm-none-eabi-gcc` toolchain (for firmware assembly)
- `libpio-dev` (for PIO setup in throughput_pioloop_m3poll)
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
- Firmware: `test_sram_write.bin`, `test_pio_fifo.bin`, `pio_bridge.bin`,
  `test_pio_addr.bin`, `test_clock.bin`
- Host tools: `core1_launcher`, `throughput_pioloop_m3poll`

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
sudo ./core1_launcher -f test_sram_write.bin -m 5
```

### throughput_pioloop_m3poll

Runs the full PIO FIFO bridge benchmark. Sets up PIO SM3 with a loopback
program, launches `pio_bridge.bin` on Core 1, and measures throughput.

```
sudo ./throughput_pioloop_m3poll [-t secs] [-v] [-p pattern] [-j]
```

Options:
- `-t SECS` -- Benchmark duration in seconds (default: 5)
- `-v` -- Enable verify mode (checks RX data = bitwise NOT of TX data)
- `-p PAT` -- TX data pattern: `seq`, `walk`, `random`, `fixed` (default: `seq`)
- `-j` -- Output results as JSON

Example:
```
sudo ./throughput_pioloop_m3poll -t 10 -v -p seq
```

### Available Firmware Binaries

| Binary               | Purpose                                       |
|----------------------|-----------------------------------------------|
| test_sram_write.bin  | Basic SRAM write test to verify Core 1 bootstrap |
| test_pio_fifo.bin    | PIO FIFO loopback test (TXF3 write, RXF3 read)  |
| pio_bridge.bin       | Full SRAM-to-FIFO bridge (used by throughput_pioloop_m3poll) |
| test_pio_addr.bin    | Compare PIO access at 0xF0000000 vs 0x40178000   |
| test_clock.bin       | Tight SRAM loop to measure M3 clock speed         |

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
