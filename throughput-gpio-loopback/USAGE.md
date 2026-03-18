# GPIO Loopback Throughput -- Usage

## Prerequisites

- Raspberry Pi 5 with kernel 6.12+ (PR #6994 and PR #7190 applied)
- `libpio-dev` installed (`sudo apt install libpio-dev`)
- Root privileges (required for `/dev/pio0` access)
- `pioasm` available for regenerating PIO headers (optional)

## GPIO Wiring

Connect GPIO5 output to GPIO5 input for single-pin loopback (default configuration). Alternatively, connect GPIO5 (output) to GPIO4 (input) for two-pin loopback.

The benchmark uses `set pins` on the output pin and `in pins` on the input pin. Both pins must be accessible via the Pmod HAT or header.

| Pin | Function | Direction |
|-----|----------|-----------|
| GPIO5 | Output (set pins) | Out |
| GPIO5 or GPIO4 | Input (in pins) | In |

For single-pin loopback, no external wiring is needed -- the pin drives and reads itself.

## Build

On the RPi5:

```sh
cd throughput-gpio-loopback
make benchmark
```

To force build on a non-RPi5 host (cross-compilation):

```sh
make benchmark FORCE_BUILD=1
```

To regenerate PIO headers from `.pio` source:

```sh
make pioasm
```

## Run

```sh
sudo ./gpio_loopback [options]
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--size=BYTES` | 8192 | Source data size per iteration |
| `--iterations=N` | 100 | Number of measured iterations |
| `--warmup=N` | 3 | Warmup iterations (not measured) |
| `--pattern=ID` | 0 | Test pattern: 0=sequential, 1=ones, 2=alternating, 3=random |
| `--threshold=MB/S` | 1.0 | Pass/fail throughput threshold |
| `--json` | off | Output results as JSON |
| `--no-verify` | off | Skip data verification |
| `--output-pin=N` | 5 | GPIO pin for output |
| `--input-pin=N` | 5 | GPIO pin for input |

### Example

```sh
# Default: 8 KB, 100 iterations, sequential pattern, single-pin loopback on GPIO5
sudo ./gpio_loopback

# Larger transfer with random data, JSON output
sudo ./gpio_loopback --size=32768 --pattern=3 --iterations=200 --json

# Two-pin loopback (GPIO5 out, GPIO4 in)
sudo ./gpio_loopback --output-pin=5 --input-pin=4
```
