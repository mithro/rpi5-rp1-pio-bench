# PIO Loopback Throughput -- Usage

## Prerequisites

- Raspberry Pi 5
- `libpio-dev` package installed (`sudo apt install libpio-dev`)
- Kernel 6.12+ with the following patches applied:
  - PR #6994 -- Heavy DMA channel reservation
  - PR #7190 -- DMA threshold/burst alignment fix
- `/dev/pio0` device node present

## Build

```
cd throughput-pioloop-piolib
make
```

This produces the `throughput_pioloop_piolib` binary. The Makefile will check for
`libpio-dev` and `/dev/pio0` automatically.

Other build targets:

| Target          | Description                                        |
|-----------------|----------------------------------------------------|
| `make test`     | Build portable test binary (no hardware required)  |
| `make run-test` | Build and run the portable test                    |
| `make`          | Build the RPi5 benchmark binary                    |
| `make check-deps`| Verify RPi5 build dependencies are present        |
| `make install-deps`| Install RPi5 dependencies (requires sudo)       |
| `make pioasm`   | Regenerate `pio_loopback.pio.h` from `pio_loopback.pio` |
| `make clean`    | Remove build artifacts                             |

## Run

```
sudo ./throughput_pioloop_piolib
```

Root access is required for `/dev/pio0` access.

## CLI options

| Option              | Default    | Description                                    |
|---------------------|------------|------------------------------------------------|
| `--size=BYTES`      | 262144     | Transfer size per iteration (bytes)            |
| `--iterations=N`    | 100        | Number of measured iterations                  |
| `--warmup=N`        | 3          | Warmup iterations (not measured)               |
| `--pattern=ID`      | 0          | Test pattern: 0=sequential, 1=ones, 2=alternating, 3=random |
| `--threshold=MB/S`  | 10.0       | Pass/fail throughput threshold                 |
| `--json`            | off        | Output JSON instead of human-readable table    |
| `--no-verify`       | off        | Skip data verification                        |
| `--mode=MODE`       | dma        | Transfer mode: `dma` or `blocking`             |
| `--dma-threshold=N` | 8          | FIFO threshold 1-8 (DMA mode only)            |
| `--dma-priority=N`  | 2          | DMA priority 0-31 (DMA mode only)             |
| `--help`            |            | Show usage help                                |

## Examples

Run with defaults (256 KB, 100 iterations, DMA mode):

```
sudo ./throughput_pioloop_piolib
```

Smaller transfer size with JSON output:

```
sudo ./throughput_pioloop_piolib --size=65536 --iterations=50 --json
```

Run without data verification:

```
sudo ./throughput_pioloop_piolib --no-verify
```

Use random test pattern with custom DMA settings:

```
sudo ./throughput_pioloop_piolib --pattern=3 --dma-threshold=4 --dma-priority=0
```

Blocking mode (slow, for comparison):

```
sudo ./throughput_pioloop_piolib --mode=blocking --size=4096 --iterations=10
```

## Interpreting results

The benchmark prints a report with two throughput sections:

- **Results** -- per-iteration statistics (min, max, mean, median, stddev,
  P5/P95/P99) and aggregate throughput (total bytes / total wall time).
  Data integrity status is reported as PASS (zero errors) or FAIL.

- **Theoretical analysis** -- compares measured throughput against the PIO
  internal rate (267 MB/s) and the estimated DMA ceiling. The
  "achieved" percentage shows how close the benchmark gets to the
  ceiling estimate.

The final line is a PASS/FAIL verdict. PASS requires both:
1. Aggregate throughput >= the `--threshold` value (default 10.0 MB/s)
2. Zero data verification errors
