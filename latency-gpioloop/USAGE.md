# GPIO Latency Benchmark -- Usage

## Prerequisites

### Hardware

- Raspberry Pi 5 (device under test, runs PIO echo programs)
- Raspberry Pi 4 (external stimulus/measurement device)
- Pmod HAT on both RPis, or direct GPIO header connection
- Jumper wire connecting GPIO4 on RPi4 to GPIO4 on RPi5 (stimulus)
- Jumper wire connecting GPIO5 on RPi5 to GPIO5 on RPi4 (response)

### Software

- RPi5: kernel 6.12+ with `libpio-dev` installed, root access
- RPi4: kernel with `/dev/gpiomem` support (standard Raspberry Pi OS)
- Development machine: Python 3.10+, `uv`, SSH key-based access to both RPis
- Both RPis reachable via SSH (default: `rpi5-pmod.iot.welland.mithis.com`, `rpi4-pmod.iot.welland.mithis.com`)

## Running via Python Orchestrator (Recommended)

The orchestrator handles source sync, building, test coordination, and result collection:

```sh
# Run all test layers (L0, L1, L2, L3)
uv run latency-gpioloop/run.py

# Run specific layers
uv run latency-gpioloop/run.py --tests L0
uv run latency-gpioloop/run.py --tests L0 L1 L2 L3

# Custom iteration count with JSON output
uv run latency-gpioloop/run.py --iterations 5000 --json

# Skip build (use existing binaries)
uv run latency-gpioloop/run.py --no-build --no-sync
```

### Orchestrator Options

| Option | Default | Description |
|--------|---------|-------------|
| `--tests` | L0 L1 | Test layers to run (L0, L1, L2, L3) |
| `--iterations` | 1000 | Measurement iterations per layer |
| `--warmup` | 50 | Warmup iterations (not measured) |
| `--input-pin` | 4 | GPIO pin for stimulus (RPi4 output, RPi5 input) |
| `--output-pin` | 5 | GPIO pin for response (RPi5 output, RPi4 input) |
| `--rpi5-host` | rpi5-pmod.iot.welland.mithis.com | RPi5 SSH hostname |
| `--rpi4-host` | rpi4-pmod.iot.welland.mithis.com | RPi4 SSH hostname |
| `--remote-dir` | /home/tim/rpi5-rp1-pio-bench | Remote source directory |
| `--json` | off | Output combined JSON results |
| `--no-build` | off | Skip remote build step |
| `--no-sync` | off | Skip rsync of source code |
| `--settle-secs` | 3 | Seconds to wait after starting RPi5 PIO |
| `--measurement-timeout` | 120 | Timeout in seconds for RPi4 measurement |
| `--rt-priority` | 0 | SCHED_FIFO priority (0 = disabled) |
| `--cpu` | -1 | CPU affinity (-1 = no affinity) |

## Manual Build and Run

### On RPi5

```sh
cd latency-gpioloop
make
sudo ./latency_gpioloop --test=L0 --input-pin=4 --output-pin=5
```

### On RPi4

```sh
cd latency-gpioloop
make rpi4
./latency_gpioloop_rpi4 --stimulus-pin=4 --response-pin=5 --iterations=1000 --warmup=50 --json
```

### Build Options

```sh
make rpi4          # Build RPi4 measurement binary
make               # Build RPi5 PIO latency binary (default target)
make pioasm        # Regenerate .pio.h headers
make clean         # Remove build artifacts
FORCE_BUILD=1 make # Force build on non-RPi5 host
```
