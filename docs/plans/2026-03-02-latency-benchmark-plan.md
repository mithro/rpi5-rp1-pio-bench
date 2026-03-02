# PIO Latency Benchmark Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a layered latency measurement suite (L0-L3) that characterises PIO round-trip latency on RPi5, using RPi4 as an external measurement device.

**Architecture:** Two C programs (RPi4 stimulus/measurement, RPi5 PIO echo) coordinated by a Python orchestrator running on the local machine. Each test layer adds one more software component to the signal path, producing a latency breakdown. Reuses `benchmark_stats.c/h` for statistics.

**Tech Stack:** C11 (RPi4 GPIO mmap, RPi5 piolib), PIO assembly (RP1), Python 3 (orchestrator), Make (build system)

---

## Task 1: Create Directory Structure and Build Skeleton

**Files:**
- Create: `latency/Makefile`
- Create: `latency/.gitignore`

**Step 1: Create the latency directory**

```bash
mkdir -p latency
```

**Step 2: Create .gitignore**

Create `latency/.gitignore`:
```
latency_rpi4
latency_rpi5
*.o
```

**Step 3: Create skeleton Makefile**

Create `latency/Makefile`:
```makefile
# latency/Makefile — Build system for PIO latency benchmark
#
# Targets:
#   rpi4        Build the RPi4 measurement binary (no PIO deps)
#   rpi5        Build the RPi5 PIO latency binary (requires libpio-dev)
#   pioasm      Regenerate .pio.h headers from .pio sources
#   clean       Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared stats module from the benchmark directory
STATS_SRC := ../benchmark/benchmark_stats.c
STATS_INC := -I../benchmark

# ─── Platform detection ─────────────────────────────────
IS_RPI5   := $(shell test -e /dev/pio0 && echo yes || echo no)
IS_RPI4   := $(shell test -e /dev/gpiomem && ! test -e /dev/pio0 && echo yes || echo no)
IS_AARCH64 := $(shell uname -m | grep -q aarch64 && echo yes || echo no)

# ─── Target: rpi4 (RPi4 measurement binary) ─────────────
RPi4_SRCS    := latency_rpi4.c $(STATS_SRC)
RPi4_CFLAGS  := $(CFLAGS) $(STATS_INC) -DPICO_NO_HARDWARE=1
RPi4_LDFLAGS := -lm

.PHONY: rpi4
rpi4: latency_rpi4

latency_rpi4: $(RPi4_SRCS) latency_common.h ../benchmark/benchmark_stats.h
ifeq ($(IS_RPI4),yes)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else
	@echo "ERROR: rpi4 target requires RPi4 with /dev/gpiomem"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: rpi5 (RPi5 PIO latency binary) ─────────────
RPi5_SRCS    := latency_rpi5.c $(STATS_SRC)
RPi5_CFLAGS  := $(CFLAGS) $(STATS_INC) -I/usr/include/piolib
RPi5_LDFLAGS := -lpio -lpthread -lm

.PHONY: rpi5
rpi5: latency_rpi5

latency_rpi5: $(RPi5_SRCS) latency_common.h gpio_echo.pio.h edge_detector.pio.h output_driver.pio.h ../benchmark/benchmark_stats.h
ifeq ($(IS_RPI5),yes)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else
	@echo "ERROR: rpi5 target requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: pioasm (regenerate headers) ─────────────────
PIO_SRCS := gpio_echo.pio edge_detector.pio output_driver.pio

.PHONY: pioasm
pioasm: $(PIO_SRCS)
	pioasm gpio_echo.pio gpio_echo.pio.h
	pioasm edge_detector.pio edge_detector.pio.h
	pioasm output_driver.pio output_driver.pio.h

# ─── Target: clean ───────────────────────────────────────
.PHONY: clean
clean:
	rm -f latency_rpi4 latency_rpi5 *.o
```

**Step 4: Commit**

```bash
git add latency/.gitignore latency/Makefile
git commit -m "Add latency benchmark directory with build skeleton"
```

---

## Task 2: Write PIO Programs

**Files:**
- Create: `latency/gpio_echo.pio`
- Create: `latency/edge_detector.pio`
- Create: `latency/output_driver.pio`

**Step 1: Write gpio_echo.pio (L0: PIO-only echo)**

Create `latency/gpio_echo.pio`:
```asm
; gpio_echo.pio — PIO-only GPIO echo for latency measurement (L0)
;
; Monitors input pin (relative to in_base) and echoes its state to
; output pin (relative to set_base) with minimum latency.
;
; Signal path: input pin → WAIT instruction → SET instruction → output pin
;
; Latency analysis at 200 MHz (5 ns per cycle):
;   Input synchroniser: 2 cycles (10 ns) when enabled
;   WAIT PIN extra stage: 1 cycle (5 ns) vs WAIT GPIO
;   WAIT completes: 1 cycle (5 ns) when condition met
;   SET executes: 1 cycle (5 ns)
;   Output pad delay: ~2-5 ns
;   Total (sync enabled): ~25-30 ns per edge
;   Total (sync bypassed): ~15-20 ns per edge

.program gpio_echo
.wrap_target
    wait 1 pin 0        ; stall until input pin goes high
    set pins, 1          ; drive output pin high
    wait 0 pin 0        ; stall until input pin goes low
    set pins, 0          ; drive output pin low
.wrap
```

**Step 2: Write edge_detector.pio (L1-L3: input watcher)**

Create `latency/edge_detector.pio`:
```asm
; edge_detector.pio — Edge detector for CPU-in-the-loop latency tests (L1-L3)
;
; Watches input pin for rising/falling edges and pushes the new pin state
; (1 or 0) to the RX FIFO via autopush. The CPU reads this value, processes
; it, and writes the response to SM1's TX FIFO.
;
; Requires: autopush enabled, threshold = 32 bits
; Pin mapping: in_base = input GPIO pin

.program edge_detector
.wrap_target
    wait 1 pin 0        ; stall until input goes high
    set x, 1             ; x = 1 (pin is now high)
    in x, 32             ; autopush: ISR ← x, then push to RX FIFO
    wait 0 pin 0        ; stall until input goes low
    set x, 0             ; x = 0 (pin is now low)
    in x, 32             ; autopush: ISR ← x, then push to RX FIFO
.wrap
```

**Step 3: Write output_driver.pio (L1-L3: output from FIFO)**

Create `latency/output_driver.pio`:
```asm
; output_driver.pio — FIFO-driven output for CPU-in-the-loop tests (L1-L3)
;
; Reads 1-bit values from the TX FIFO (via autopull) and drives the
; output pin accordingly. Stalls when the TX FIFO is empty, waiting
; for the CPU to write the next value.
;
; Requires: autopull enabled, threshold = 32 bits
; Pin mapping: out_base = output GPIO pin, out_count = 1

.program output_driver
.wrap_target
    out pins, 1          ; autopull: TX FIFO → OSR, then shift 1 bit → output pin
.wrap
```

**Step 4: Generate .pio.h headers on RPi5**

This step must be run on RPi5 where `pioasm` is installed:
```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com "cd /path/to/latency && make pioasm"
```

Alternatively, if pioasm is not available locally, we'll pre-generate the headers
and commit them (like the existing `loopback.pio.h`). The headers can be generated
by examining the PIO instruction encoding manually or by running pioasm on any
machine that has it installed.

**Step 5: Commit PIO programs**

```bash
git add latency/gpio_echo.pio latency/edge_detector.pio latency/output_driver.pio
git commit -m "Add PIO programs for latency measurement

gpio_echo.pio: L0 PIO-only echo (4 instructions, ~25 ns/edge)
edge_detector.pio: L1-L3 input watcher (6 instructions, autopush)
output_driver.pio: L1-L3 output driver (1 instruction, autopull)"
```

---

## Task 3: Write Shared Header and Latency Report Types

**Files:**
- Create: `latency/latency_common.h`

**Step 1: Create latency_common.h**

Create `latency/latency_common.h`:
```c
/* latency_common.h — Shared definitions for PIO latency benchmark
 *
 * Common types, constants, and formatting for both the RPi4 measurement
 * program and the RPi5 PIO latency program.
 */

#ifndef LATENCY_COMMON_H
#define LATENCY_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "benchmark_stats.h"

/* Default pin assignments (JC connector, bottom row) */
#define DEFAULT_STIMULUS_PIN   4   /* GPIO4 = JC7 (GPCLK0) */
#define DEFAULT_RESPONSE_PIN   5   /* GPIO5 = JC9 (GPCLK1) */

/* Default iteration counts */
#define DEFAULT_ITERATIONS     1000
#define DEFAULT_WARMUP         10

/* Test layer identifiers */
#define TEST_L0  0   /* PIO-only echo (hardware baseline) */
#define TEST_L1  1   /* PIO → ioctl → PIO */
#define TEST_L2  2   /* PIO → DMA → CPU poll → DMA → PIO */
#define TEST_L3  3   /* PIO → mmap FIFO → PIO (experimental) */

/* Latency report for a single test run. */
typedef struct {
    bench_summary_stats_t latency_ns;  /* round-trip latency in nanoseconds */
    int test_layer;                     /* TEST_L0 .. TEST_L3 */
    int stimulus_pin;
    int response_pin;
    size_t num_iterations;
    size_t num_warmup;
    int rt_priority;                    /* 0 = no RT, 1-99 = SCHED_FIFO */
    int cpu_affinity;                   /* -1 = no affinity, 0-3 = core */
} latency_report_t;

/* ─── Timing helper ──────────────────────────────────── */

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ─── Test layer name ────────────────────────────────── */

static inline const char *test_layer_name(int layer)
{
    switch (layer) {
    case TEST_L0: return "L0 (PIO-only echo)";
    case TEST_L1: return "L1 (PIO -> ioctl -> PIO)";
    case TEST_L2: return "L2 (PIO -> DMA -> poll -> PIO)";
    case TEST_L3: return "L3 (PIO -> mmap FIFO -> PIO)";
    default:      return "unknown";
    }
}

/* ─── Report printing ────────────────────────────────── */

static inline void latency_print_report(FILE *f, const latency_report_t *r)
{
    fprintf(f,
        "================================================================\n"
        "PIO Latency Benchmark\n"
        "================================================================\n"
        "\n"
        "Configuration:\n"
        "  Test:              %s\n"
        "  Stimulus pin:      GPIO%d\n"
        "  Response pin:      GPIO%d\n"
        "  Iterations:        %zu (warmup: %zu)\n",
        test_layer_name(r->test_layer),
        r->stimulus_pin,
        r->response_pin,
        r->num_iterations,
        r->num_warmup);

    if (r->rt_priority > 0)
        fprintf(f, "  RT priority:       SCHED_FIFO %d\n", r->rt_priority);
    if (r->cpu_affinity >= 0)
        fprintf(f, "  CPU affinity:      core %d\n", r->cpu_affinity);

    fprintf(f,
        "\n"
        "Round-trip latency (nanoseconds):\n"
        "  ----------------------------------------------------------------\n"
        "  Min:               %.0f\n"
        "  Max:               %.0f\n"
        "  Mean:              %.1f\n"
        "  Median:            %.0f\n"
        "  Std Dev:           %.1f\n"
        "  P5:                %.0f\n"
        "  P95:               %.0f\n"
        "  P99:               %.0f\n"
        "  ----------------------------------------------------------------\n"
        "================================================================\n",
        r->latency_ns.min,
        r->latency_ns.max,
        r->latency_ns.mean,
        r->latency_ns.median,
        r->latency_ns.stddev,
        r->latency_ns.p5,
        r->latency_ns.p95,
        r->latency_ns.p99);
}

static inline void latency_print_json(FILE *f, const latency_report_t *r)
{
    fprintf(f,
        "{\n"
        "  \"benchmark\": \"rp1-pio-latency\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"config\": {\n"
        "    \"test\": \"%s\",\n"
        "    \"test_layer\": %d,\n"
        "    \"stimulus_pin\": %d,\n"
        "    \"response_pin\": %d,\n"
        "    \"iterations\": %zu,\n"
        "    \"warmup\": %zu,\n"
        "    \"rt_priority\": %d,\n"
        "    \"cpu_affinity\": %d\n"
        "  },\n"
        "  \"results\": {\n"
        "    \"round_trip_ns\": {\n"
        "      \"min\": %.1f,\n"
        "      \"max\": %.1f,\n"
        "      \"mean\": %.1f,\n"
        "      \"median\": %.1f,\n"
        "      \"stddev\": %.1f,\n"
        "      \"p5\": %.1f,\n"
        "      \"p95\": %.1f,\n"
        "      \"p99\": %.1f\n"
        "    }\n"
        "  }\n"
        "}\n",
        test_layer_name(r->test_layer),
        r->test_layer,
        r->stimulus_pin,
        r->response_pin,
        r->num_iterations,
        r->num_warmup,
        r->rt_priority,
        r->cpu_affinity,
        r->latency_ns.min,
        r->latency_ns.max,
        r->latency_ns.mean,
        r->latency_ns.median,
        r->latency_ns.stddev,
        r->latency_ns.p5,
        r->latency_ns.p95,
        r->latency_ns.p99);
}

#endif /* LATENCY_COMMON_H */
```

**Step 2: Commit**

```bash
git add latency/latency_common.h
git commit -m "Add shared latency benchmark header with report types and formatting"
```

---

## Task 4: Write RPi4 Measurement Program

**Files:**
- Create: `latency/latency_rpi4.c`

This is the stimulus generator and external round-trip timer. Uses direct
memory-mapped GPIO access via `/dev/gpiomem` on BCM2711 for minimum overhead.

**Step 1: Write latency_rpi4.c**

Create `latency/latency_rpi4.c`:
```c
/* latency_rpi4.c — RPi4 GPIO latency measurement program
 *
 * Generates stimulus pulses on one GPIO pin and measures the round-trip
 * time until a response appears on another GPIO pin. Uses direct
 * memory-mapped BCM2711 GPIO registers via /dev/gpiomem for minimum
 * measurement overhead.
 *
 * Measurement overhead: ~68 ns per GPIO read (BCM2711 bus round-trip)
 * plus ~20-50 ns for clock_gettime(). This is the measurement floor.
 *
 * Build: gcc -O2 -o latency_rpi4 latency_rpi4.c ../benchmark/benchmark_stats.c
 *        -I../benchmark -DPICO_NO_HARDWARE=1 -lm
 * Run:   ./latency_rpi4 [options]
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "latency_common.h"

/* ─── BCM2711 GPIO register offsets ──────────────────── */

#define GPFSEL0   0x00   /* Function Select (3 bits/pin, 10 pins/reg) */
#define GPSET0    0x1C   /* Output Set (write 1 = HIGH, write 0 = no effect) */
#define GPCLR0    0x28   /* Output Clear (write 1 = LOW, write 0 = no effect) */
#define GPLEV0    0x34   /* Pin Level (read-only, 1 bit per pin) */

/* Pull-up/Pull-down control (BCM2711-specific, 2 bits per pin) */
#define GPIO_PUP_PDN_CNTRL_REG0  0xE4

/* Pull-up/down values for BCM2711 */
#define PUD_OFF   0
#define PUD_UP    1
#define PUD_DOWN  2

/* Register access */
#define GPIO_REG(base, off)  (*(volatile uint32_t *)((char *)(base) + (off)))

/* ─── GPIO helpers ───────────────────────────────────── */

static volatile void *gpio_base;

static volatile void *map_gpio(void)
{
    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/gpiomem");
        fprintf(stderr, "Ensure user is in 'gpio' group: sudo usermod -aG gpio $USER\n");
        return NULL;
    }
    volatile void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap /dev/gpiomem");
        return NULL;
    }
    return p;
}

static void gpio_set_function(int pin, int func)
{
    int reg = GPFSEL0 + (pin / 10) * 4;
    int shift = (pin % 10) * 3;
    uint32_t val = GPIO_REG(gpio_base, reg);
    val &= ~(7u << shift);
    val |= ((uint32_t)func & 7u) << shift;
    GPIO_REG(gpio_base, reg) = val;
}

static void gpio_set_pull(int pin, int pull)
{
    int reg_offset = GPIO_PUP_PDN_CNTRL_REG0 + (pin / 16) * 4;
    int shift = (pin % 16) * 2;
    uint32_t val = GPIO_REG(gpio_base, reg_offset);
    val &= ~(3u << shift);
    val |= ((uint32_t)pull & 3u) << shift;
    GPIO_REG(gpio_base, reg_offset) = val;
}

static inline void gpio_set_output(int pin)
{
    gpio_set_function(pin, 1);  /* 001 = output */
}

static inline void gpio_set_input(int pin)
{
    gpio_set_function(pin, 0);  /* 000 = input */
}

static inline void gpio_set_high(int pin)
{
    GPIO_REG(gpio_base, GPSET0 + (pin / 32) * 4) = 1u << (pin % 32);
}

static inline void gpio_set_low(int pin)
{
    GPIO_REG(gpio_base, GPCLR0 + (pin / 32) * 4) = 1u << (pin % 32);
}

static inline int gpio_read(int pin)
{
    return (GPIO_REG(gpio_base, GPLEV0 + (pin / 32) * 4) >> (pin % 32)) & 1;
}

/* ─── Signal handling ────────────────────────────────── */

static volatile sig_atomic_t running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ─── Timeout for busy-poll ──────────────────────────── */

#define POLL_TIMEOUT_NS  (100ULL * 1000000ULL)  /* 100 ms */

/* ─── Usage ──────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Measures GPIO round-trip latency from RPi4 to RPi5 and back.\n"
        "Drives stimulus pin HIGH/LOW and measures time until response pin changes.\n"
        "\n"
        "Options:\n"
        "  --stimulus-pin=N   GPIO pin for stimulus output (default %d)\n"
        "  --response-pin=N   GPIO pin for response input (default %d)\n"
        "  --iterations=N     Number of measured iterations (default %d)\n"
        "  --warmup=N         Warmup iterations (default %d)\n"
        "  --rt-priority=N    Set SCHED_FIFO RT priority 1-99 (default: off)\n"
        "  --cpu=N            Pin to CPU core N (default: no affinity)\n"
        "  --json             Output JSON instead of human-readable\n"
        "  --help             Show this help\n",
        prog,
        DEFAULT_STIMULUS_PIN,
        DEFAULT_RESPONSE_PIN,
        DEFAULT_ITERATIONS,
        DEFAULT_WARMUP);
}

/* ─── Main ───────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int stimulus_pin = DEFAULT_STIMULUS_PIN;
    int response_pin = DEFAULT_RESPONSE_PIN;
    int iterations = DEFAULT_ITERATIONS;
    int warmup = DEFAULT_WARMUP;
    int rt_priority = 0;
    int cpu_affinity = -1;
    int json_output = 0;

    static struct option long_options[] = {
        {"stimulus-pin", required_argument, NULL, 's'},
        {"response-pin", required_argument, NULL, 'r'},
        {"iterations",   required_argument, NULL, 'i'},
        {"warmup",       required_argument, NULL, 'w'},
        {"rt-priority",  required_argument, NULL, 'p'},
        {"cpu",          required_argument, NULL, 'c'},
        {"json",         no_argument,       NULL, 'j'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:r:i:w:p:c:jh",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 's': stimulus_pin = atoi(optarg); break;
        case 'r': response_pin = atoi(optarg); break;
        case 'i': iterations = atoi(optarg); break;
        case 'w': warmup = atoi(optarg); break;
        case 'p': rt_priority = atoi(optarg); break;
        case 'c': cpu_affinity = atoi(optarg); break;
        case 'j': json_output = 1; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* Validate pins */
    if (stimulus_pin < 0 || stimulus_pin > 27 ||
        response_pin < 0 || response_pin > 27) {
        fprintf(stderr, "ERROR: GPIO pins must be 0-27\n");
        return 1;
    }
    if (stimulus_pin == response_pin) {
        fprintf(stderr, "ERROR: stimulus and response pins must be different\n");
        return 1;
    }
    if (iterations < 1) {
        fprintf(stderr, "ERROR: iterations must be >= 1\n");
        return 1;
    }

    /* Map GPIO registers */
    gpio_base = map_gpio();
    if (!gpio_base)
        return 1;

    /* Set up signal handler for clean shutdown */
    signal(SIGINT, sigint_handler);

    /* Apply RT priority if requested */
    if (rt_priority > 0) {
        struct sched_param sp = { .sched_priority = rt_priority };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
            perror("WARNING: sched_setscheduler(SCHED_FIFO) failed");
            fprintf(stderr, "  (run with sudo for RT priority)\n");
        }
    }

    /* Apply CPU affinity if requested */
    if (cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_affinity, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
            perror("WARNING: sched_setaffinity failed");
        }
    }

    /* Lock memory to prevent page faults during measurement */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
        perror("WARNING: mlockall failed (non-fatal)");
    }

    /* Configure GPIO pins */
    gpio_set_output(stimulus_pin);
    gpio_set_input(response_pin);
    gpio_set_pull(response_pin, PUD_DOWN);  /* pull-down to avoid floating */
    gpio_set_low(stimulus_pin);             /* start low */

    /* Wait briefly for pins to settle */
    usleep(1000);

    /* Verify initial state: response should be low */
    if (gpio_read(response_pin) != 0) {
        fprintf(stderr, "WARNING: response pin GPIO%d reads HIGH before test\n",
                response_pin);
        fprintf(stderr, "  (expected LOW — check wiring and RPi5 program)\n");
    }

    if (!json_output) {
        fprintf(stderr, "RPi4 Latency Measurement\n");
        fprintf(stderr, "  Stimulus: GPIO%d (output)\n", stimulus_pin);
        fprintf(stderr, "  Response: GPIO%d (input)\n", response_pin);
        fprintf(stderr, "  Iterations: %d (warmup: %d)\n", iterations, warmup);
        fprintf(stderr, "  Waiting for RPi5 to be ready...\n");
    }

    /* Allocate latency array */
    double *latencies = (double *)malloc((size_t)iterations * sizeof(double));
    if (!latencies) {
        fprintf(stderr, "ERROR: malloc failed\n");
        return 1;
    }

    /* ─── Warmup iterations ──────────────────────────── */

    for (int i = 0; i < warmup && running; i++) {
        /* Rising edge: drive stimulus HIGH, wait for response HIGH */
        gpio_set_high(stimulus_pin);

        uint64_t deadline = get_time_ns() + POLL_TIMEOUT_NS;
        while (gpio_read(response_pin) == 0 && running) {
            if (get_time_ns() > deadline) {
                fprintf(stderr, "ERROR: timeout waiting for response HIGH "
                        "(warmup %d)\n", i);
                fprintf(stderr, "  Is the RPi5 program running?\n");
                free(latencies);
                gpio_set_input(stimulus_pin);
                return 1;
            }
        }

        /* Falling edge: drive stimulus LOW, wait for response LOW */
        gpio_set_low(stimulus_pin);

        deadline = get_time_ns() + POLL_TIMEOUT_NS;
        while (gpio_read(response_pin) != 0 && running) {
            if (get_time_ns() > deadline) {
                fprintf(stderr, "ERROR: timeout waiting for response LOW "
                        "(warmup %d)\n", i);
                free(latencies);
                gpio_set_input(stimulus_pin);
                return 1;
            }
        }
    }

    if (!json_output)
        fprintf(stderr, "  Warmup complete, measuring...\n");

    /* ─── Measured iterations ────────────────────────── */

    for (int i = 0; i < iterations && running; i++) {
        /* Rising edge measurement */
        uint64_t t0 = get_time_ns();
        gpio_set_high(stimulus_pin);

        /* Busy-poll for response HIGH */
        while (gpio_read(response_pin) == 0) {
            /* tight poll — no timeout check in hot loop for accuracy */
        }

        uint64_t t1 = get_time_ns();

        /* Record rising-edge round-trip latency */
        latencies[i] = (double)(t1 - t0);

        /* Falling edge: drive stimulus LOW, wait for response LOW */
        gpio_set_low(stimulus_pin);
        while (gpio_read(response_pin) != 0) {
            /* tight poll */
        }

        /* Small gap between iterations to let things settle */
        /* (The PIO program handles both edges, so no explicit delay needed) */
    }

    if (!running) {
        fprintf(stderr, "\nInterrupted after %d iterations\n", iterations);
    }

    /* ─── Compute statistics and print report ────────── */

    double *scratch = (double *)malloc((size_t)iterations * sizeof(double));
    if (!scratch) {
        fprintf(stderr, "ERROR: malloc scratch failed\n");
        free(latencies);
        gpio_set_input(stimulus_pin);
        return 1;
    }

    latency_report_t report;
    memset(&report, 0, sizeof(report));
    bench_compute_stats(latencies, (size_t)iterations, scratch,
                        &report.latency_ns);
    report.test_layer = -1;  /* RPi4 doesn't know the RPi5 test layer */
    report.stimulus_pin = stimulus_pin;
    report.response_pin = response_pin;
    report.num_iterations = (size_t)iterations;
    report.num_warmup = (size_t)warmup;
    report.rt_priority = rt_priority;
    report.cpu_affinity = cpu_affinity;

    if (json_output)
        latency_print_json(stdout, &report);
    else
        latency_print_report(stdout, &report);

    /* ─── Cleanup ────────────────────────────────────── */

    free(scratch);
    free(latencies);
    gpio_set_input(stimulus_pin);  /* restore to safe input mode */
    gpio_set_pull(response_pin, PUD_OFF);
    munmap((void *)gpio_base, 4096);

    return 0;
}
```

**Step 2: Verify it compiles (must be done on RPi4 or with FORCE_BUILD)**

```bash
ssh tim@rpi4-pmod.iot.welland.mithis.com "cd /path/to/latency && make rpi4"
```

**Step 3: Commit**

```bash
git add latency/latency_rpi4.c
git commit -m "Add RPi4 GPIO latency measurement program

Uses memory-mapped /dev/gpiomem for direct BCM2711 GPIO register access.
Drives stimulus pin, busy-polls response pin, measures round-trip time
with clock_gettime(CLOCK_MONOTONIC). Supports RT priority and CPU affinity."
```

---

## Task 5: Write RPi5 Latency Program (L0 Only)

**Files:**
- Create: `latency/latency_rpi5.c`

Start with L0 (PIO-only echo) support only. L1-L3 will be added in later tasks.

**Step 1: Write latency_rpi5.c with L0 support**

Create `latency/latency_rpi5.c`:
```c
/* latency_rpi5.c — RPi5 PIO latency benchmark program
 *
 * Configures RP1 PIO state machine(s) for various latency test modes:
 *   L0: PIO-only echo — PIO watches input pin, echoes to output pin
 *   L1: PIO → ioctl → PIO — CPU reads/writes via pio_sm_get/put
 *   L2: PIO → DMA → poll → PIO — CPU polls DMA buffer
 *   L3: PIO → mmap FIFO → PIO — direct FIFO register access
 *
 * Build: gcc -O2 -I/usr/include/piolib -I../benchmark -o latency_rpi5 \
 *        latency_rpi5.c ../benchmark/benchmark_stats.c -lpio -lpthread -lm
 * Run:   sudo ./latency_rpi5 --test=L0 --input-pin=4 --output-pin=5
 */

#define _GNU_SOURCE

#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "piolib.h"
#include "gpio_echo.pio.h"
#include "edge_detector.pio.h"
#include "output_driver.pio.h"

#include "latency_common.h"

/* ─── Signal handling ────────────────────────────────── */

static volatile sig_atomic_t running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ─── L0: PIO-only echo ─────────────────────────────── */

static int run_l0(PIO pio, uint input_pin, uint output_pin)
{
    /* Claim a state machine */
    int sm = pio_claim_unused_sm(pio, true);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machines\n");
        return 1;
    }

    /* Load PIO program */
    uint offset = pio_add_program(pio, &gpio_echo_program);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load gpio_echo program\n");
        pio_sm_unclaim(pio, (uint)sm);
        return 1;
    }

    /* Configure GPIO pins for PIO */
    pio_gpio_init(pio, input_pin);
    pio_gpio_init(pio, output_pin);

    /* Set pin directions */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, input_pin, 1, false);   /* input */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, output_pin, 1, true);   /* output */

    /* Configure state machine */
    pio_sm_config c = gpio_echo_program_get_default_config(offset);
    sm_config_set_in_pins(&c, input_pin);       /* WAIT PIN relative to this */
    sm_config_set_set_pins(&c, output_pin, 1);  /* SET PINS target */
    sm_config_set_clkdiv(&c, 1.0f);             /* full 200 MHz */

    /* Initialise and start */
    pio_sm_init(pio, (uint)sm, offset, &c);
    pio_sm_set_enabled(pio, (uint)sm, true);

    fprintf(stderr, "L0: PIO-only echo running (GPIO%u → GPIO%u)\n",
            input_pin, output_pin);
    fprintf(stderr, "  PIO echoes input pin state to output pin autonomously.\n");
    fprintf(stderr, "  Press Ctrl+C to stop.\n");

    /* PIO runs autonomously — just wait for SIGINT */
    while (running)
        pause();  /* sleep until signal */

    fprintf(stderr, "\nStopping PIO...\n");

    /* Cleanup */
    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_remove_program(pio, &gpio_echo_program, offset);
    pio_sm_unclaim(pio, (uint)sm);

    return 0;
}

/* ─── L1: PIO → ioctl → PIO ─────────────────────────── */

static int run_l1(PIO pio, uint input_pin, uint output_pin,
                  int iterations, int warmup, int json_output,
                  int rt_priority, int cpu_affinity)
{
    /* Claim two state machines */
    int sm_rx = pio_claim_unused_sm(pio, true);
    if (sm_rx < 0) {
        fprintf(stderr, "ERROR: no free state machine for RX\n");
        return 1;
    }
    int sm_tx = pio_claim_unused_sm(pio, true);
    if (sm_tx < 0) {
        fprintf(stderr, "ERROR: no free state machine for TX\n");
        pio_sm_unclaim(pio, (uint)sm_rx);
        return 1;
    }

    /* Load PIO programs */
    uint offset_ed = pio_add_program(pio, &edge_detector_program);
    if (offset_ed == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load edge_detector program\n");
        goto cleanup_sm;
    }
    uint offset_od = pio_add_program(pio, &output_driver_program);
    if (offset_od == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load output_driver program\n");
        pio_remove_program(pio, &edge_detector_program, offset_ed);
        goto cleanup_sm;
    }

    /* Configure GPIO pins */
    pio_gpio_init(pio, input_pin);
    pio_gpio_init(pio, output_pin);

    /* SM_RX: edge detector (input watcher) */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm_rx, input_pin, 1, false);
    pio_sm_config c_rx = edge_detector_program_get_default_config(offset_ed);
    sm_config_set_in_pins(&c_rx, input_pin);
    sm_config_set_in_shift(&c_rx, false, true, 32);  /* autopush, 32-bit */
    sm_config_set_clkdiv(&c_rx, 1.0f);

    /* SM_TX: output driver */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm_tx, output_pin, 1, true);
    pio_sm_config c_tx = output_driver_program_get_default_config(offset_od);
    sm_config_set_out_pins(&c_tx, output_pin, 1);
    sm_config_set_out_shift(&c_tx, true, true, 32);  /* autopull, 32-bit */
    sm_config_set_clkdiv(&c_tx, 1.0f);

    /* Initialise both SMs */
    pio_sm_init(pio, (uint)sm_rx, offset_ed, &c_rx);
    pio_sm_init(pio, (uint)sm_tx, offset_od, &c_tx);

    /* Enable both SMs */
    pio_sm_set_enabled(pio, (uint)sm_rx, true);
    pio_sm_set_enabled(pio, (uint)sm_tx, true);

    fprintf(stderr, "L1: PIO → ioctl → PIO running (GPIO%u → GPIO%u)\n",
            input_pin, output_pin);

    /* Allocate latency array */
    double *latencies = (double *)malloc((size_t)iterations * sizeof(double));
    if (!latencies) {
        fprintf(stderr, "ERROR: malloc failed\n");
        goto cleanup_pio;
    }

    /* Warmup: read edge from RX FIFO, write to TX FIFO */
    for (int i = 0; i < warmup && running; i++) {
        uint32_t state = pio_sm_get_blocking(pio, (uint)sm_rx);
        pio_sm_put_blocking(pio, (uint)sm_tx, state);
    }

    /* Measured iterations */
    for (int i = 0; i < iterations && running; i++) {
        /* Time the CPU processing: get from RX FIFO + put to TX FIFO */
        uint64_t t0 = get_time_ns();
        uint32_t state = pio_sm_get_blocking(pio, (uint)sm_rx);
        pio_sm_put_blocking(pio, (uint)sm_tx, state);
        uint64_t t1 = get_time_ns();
        latencies[i] = (double)(t1 - t0);
    }

    /* Compute and print report */
    double *scratch = (double *)malloc((size_t)iterations * sizeof(double));
    if (!scratch) {
        fprintf(stderr, "ERROR: malloc scratch failed\n");
        free(latencies);
        goto cleanup_pio;
    }

    latency_report_t report;
    memset(&report, 0, sizeof(report));
    bench_compute_stats(latencies, (size_t)iterations, scratch,
                        &report.latency_ns);
    report.test_layer = TEST_L1;
    report.stimulus_pin = (int)input_pin;
    report.response_pin = (int)output_pin;
    report.num_iterations = (size_t)iterations;
    report.num_warmup = (size_t)warmup;
    report.rt_priority = rt_priority;
    report.cpu_affinity = cpu_affinity;

    if (json_output)
        latency_print_json(stdout, &report);
    else
        latency_print_report(stdout, &report);

    free(scratch);
    free(latencies);

cleanup_pio:
    pio_sm_set_enabled(pio, (uint)sm_rx, false);
    pio_sm_set_enabled(pio, (uint)sm_tx, false);
    pio_remove_program(pio, &output_driver_program, offset_od);
    pio_remove_program(pio, &edge_detector_program, offset_ed);

cleanup_sm:
    pio_sm_unclaim(pio, (uint)sm_tx);
    pio_sm_unclaim(pio, (uint)sm_rx);
    return 0;
}

/* ─── Usage ──────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "RP1 PIO latency benchmark for Raspberry Pi 5.\n"
        "\n"
        "Options:\n"
        "  --test=MODE        Test mode: L0, L1, L2, L3 (default L0)\n"
        "  --input-pin=N      Input GPIO pin (default %d)\n"
        "  --output-pin=N     Output GPIO pin (default %d)\n"
        "  --iterations=N     Measured iterations for L1-L3 (default %d)\n"
        "  --warmup=N         Warmup iterations (default %d)\n"
        "  --rt-priority=N    Set SCHED_FIFO RT priority 1-99\n"
        "  --cpu=N            Pin to CPU core N\n"
        "  --json             Output JSON\n"
        "  --help             Show this help\n"
        "\n"
        "Test modes:\n"
        "  L0  PIO-only echo (runs until Ctrl+C, no CPU loop)\n"
        "  L1  PIO → pio_sm_get/put → PIO (ioctl path)\n"
        "  L2  PIO → DMA → CPU poll → PIO (not yet implemented)\n"
        "  L3  PIO → mmap FIFO → PIO (not yet implemented)\n",
        prog,
        DEFAULT_STIMULUS_PIN,
        DEFAULT_RESPONSE_PIN,
        DEFAULT_ITERATIONS,
        DEFAULT_WARMUP);
}

/* ─── Main ───────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int test_layer = TEST_L0;
    uint input_pin = DEFAULT_STIMULUS_PIN;
    uint output_pin = DEFAULT_RESPONSE_PIN;
    int iterations = DEFAULT_ITERATIONS;
    int warmup = DEFAULT_WARMUP;
    int rt_priority = 0;
    int cpu_affinity = -1;
    int json_output = 0;

    static struct option long_options[] = {
        {"test",         required_argument, NULL, 't'},
        {"input-pin",    required_argument, NULL, 'I'},
        {"output-pin",   required_argument, NULL, 'O'},
        {"iterations",   required_argument, NULL, 'i'},
        {"warmup",       required_argument, NULL, 'w'},
        {"rt-priority",  required_argument, NULL, 'p'},
        {"cpu",          required_argument, NULL, 'c'},
        {"json",         no_argument,       NULL, 'j'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:I:O:i:w:p:c:jh",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 't':
            if (strcmp(optarg, "L0") == 0 || strcmp(optarg, "l0") == 0)
                test_layer = TEST_L0;
            else if (strcmp(optarg, "L1") == 0 || strcmp(optarg, "l1") == 0)
                test_layer = TEST_L1;
            else if (strcmp(optarg, "L2") == 0 || strcmp(optarg, "l2") == 0)
                test_layer = TEST_L2;
            else if (strcmp(optarg, "L3") == 0 || strcmp(optarg, "l3") == 0)
                test_layer = TEST_L3;
            else {
                fprintf(stderr, "ERROR: unknown test mode '%s'\n", optarg);
                return 1;
            }
            break;
        case 'I': input_pin = (uint)atoi(optarg); break;
        case 'O': output_pin = (uint)atoi(optarg); break;
        case 'i': iterations = atoi(optarg); break;
        case 'w': warmup = atoi(optarg); break;
        case 'p': rt_priority = atoi(optarg); break;
        case 'c': cpu_affinity = atoi(optarg); break;
        case 'j': json_output = 1; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* Validate pins */
    if (input_pin > 27 || output_pin > 27) {
        fprintf(stderr, "ERROR: GPIO pins must be 0-27\n");
        return 1;
    }
    if (input_pin == output_pin) {
        fprintf(stderr, "ERROR: input and output pins must be different\n");
        return 1;
    }

    /* Set up signal handler */
    signal(SIGINT, sigint_handler);

    /* Apply RT priority if requested */
    if (rt_priority > 0) {
        struct sched_param sp = { .sched_priority = rt_priority };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
            perror("WARNING: sched_setscheduler(SCHED_FIFO) failed");
    }

    /* Apply CPU affinity if requested */
    if (cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_affinity, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
            perror("WARNING: sched_setaffinity failed");
    }

    /* Lock memory */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        perror("WARNING: mlockall failed (non-fatal)");

    /* Open PIO device */
    PIO pio = pio0;
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr, "ERROR: failed to open PIO (is /dev/pio0 present?)\n");
        return 1;
    }

    int ret;
    switch (test_layer) {
    case TEST_L0:
        ret = run_l0(pio, input_pin, output_pin);
        break;
    case TEST_L1:
        ret = run_l1(pio, input_pin, output_pin, iterations, warmup,
                     json_output, rt_priority, cpu_affinity);
        break;
    case TEST_L2:
        fprintf(stderr, "ERROR: L2 (DMA path) not yet implemented\n");
        ret = 1;
        break;
    case TEST_L3:
        fprintf(stderr, "ERROR: L3 (mmap FIFO) not yet implemented\n");
        ret = 1;
        break;
    default:
        fprintf(stderr, "ERROR: unknown test layer %d\n", test_layer);
        ret = 1;
    }

    pio_close(pio);
    return ret;
}
```

**Step 2: Commit**

```bash
git add latency/latency_rpi5.c
git commit -m "Add RPi5 PIO latency program with L0 and L1 support

L0: PIO-only echo - loads gpio_echo.pio, runs until Ctrl+C
L1: PIO -> ioctl -> PIO - uses pio_sm_get/put_blocking() in a tight loop,
    measures internal CPU processing time per edge
L2/L3: stubs for future implementation"
```

---

## Task 6: Generate PIO Headers on RPi5

**Files:**
- Create: `latency/gpio_echo.pio.h` (generated)
- Create: `latency/edge_detector.pio.h` (generated)
- Create: `latency/output_driver.pio.h` (generated)

**Step 1: Copy PIO source files to RPi5 and generate headers**

The `.pio.h` headers must be generated by `pioasm` which is only available on RPi5.

```bash
# From local machine: copy PIO source files to RPi5
scp latency/*.pio tim@rpi5-pmod.iot.welland.mithis.com:/tmp/pio-gen/

# On RPi5: generate headers
ssh tim@rpi5-pmod.iot.welland.mithis.com "
    cd /tmp/pio-gen &&
    pioasm gpio_echo.pio gpio_echo.pio.h &&
    pioasm edge_detector.pio edge_detector.pio.h &&
    pioasm output_driver.pio output_driver.pio.h
"

# Copy back to local
scp tim@rpi5-pmod.iot.welland.mithis.com:/tmp/pio-gen/*.pio.h latency/
```

**Step 2: Verify the generated headers look correct**

Each header should contain a `pio_program` struct with the correct instruction
count (gpio_echo: 4, edge_detector: 6, output_driver: 1).

**Step 3: Commit generated headers**

```bash
git add latency/gpio_echo.pio.h latency/edge_detector.pio.h latency/output_driver.pio.h
git commit -m "Add pre-generated PIO headers for latency programs

Generated by pioasm on RPi5. These are committed so the project can be
built without requiring pioasm on the build machine."
```

---

## Task 7: Build and Test on Real Hardware (L0)

**Step 1: Copy source to RPi5 and build**

```bash
# Copy entire project to RPi5
rsync -av --exclude='.git' . tim@rpi5-pmod.iot.welland.mithis.com:~/rpi5-rp1-pio-bench/

# Build on RPi5
ssh tim@rpi5-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && make rpi5"
```

**Step 2: Copy source to RPi4 and build**

```bash
rsync -av --exclude='.git' . tim@rpi4-pmod.iot.welland.mithis.com:~/rpi5-rp1-pio-bench/

ssh tim@rpi4-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && make rpi4"
```

**Step 3: Run L0 test**

Terminal 1 (RPi5): Start PIO echo
```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && sudo ./latency_rpi5 --test=L0 --input-pin=4 --output-pin=5"
```

Terminal 2 (RPi4): Run measurement
```bash
ssh tim@rpi4-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && ./latency_rpi4 --stimulus-pin=4 --response-pin=5 --iterations=1000"
```

**Expected results for L0:**
- Round-trip latency: ~100-500 ns
  - ~25 ns PIO processing
  - ~68 ns RPi4 GPIO read overhead
  - ~30-50 ns clock_gettime overhead
  - Remaining: wire propagation and measurement jitter

**Step 4: Run L1 test**

Terminal 1 (RPi5): Start PIO with ioctl loop
```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && sudo ./latency_rpi5 --test=L1 --input-pin=4 --output-pin=5 --iterations=100"
```

Terminal 2 (RPi4): Run measurement (with longer timeout since L1 is slower)
```bash
ssh tim@rpi4-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && ./latency_rpi4 --stimulus-pin=4 --response-pin=5 --iterations=100"
```

**Expected results for L1:**
- RPi4 round-trip: ~20-50 us
- RPi5 internal CPU time: ~15-30 us (two ioctl calls)

**Step 5: Test with RT optimisations**

```bash
# RPi5 with RT priority and CPU affinity
ssh tim@rpi5-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && sudo ./latency_rpi5 --test=L1 --input-pin=4 --output-pin=5 --iterations=100 --rt-priority=80 --cpu=3"

# RPi4 with RT priority
ssh tim@rpi4-pmod.iot.welland.mithis.com "cd ~/rpi5-rp1-pio-bench/latency && sudo ./latency_rpi4 --stimulus-pin=4 --response-pin=5 --iterations=100 --rt-priority=80 --cpu=3"
```

**Expected:** RT priority should reduce P99/max without significantly changing median.

---

## Task 8: Write Python Orchestrator Script

**Files:**
- Create: `run_latency_benchmark.py`

**Step 1: Write the orchestrator**

Create `run_latency_benchmark.py`:

A Python script that:
1. Verifies SSH connectivity to both devices
2. Copies the source code to both devices
3. Builds on each device
4. Starts the RPi5 program in the background (via SSH)
5. Waits briefly for it to initialise
6. Runs the RPi4 measurement program
7. Stops the RPi5 program
8. Collects and displays results
9. Supports running all test layers in sequence

The script should follow the pattern of `verify_pmod_connections.py` for SSH
access (using `subprocess.run` with `ssh` commands, user `tim`, same hostnames).

Key functions:
- `ssh_cmd(host, command)` — run command on remote host (reuse from verify script)
- `rsync_to(host, local_path, remote_path)` — copy files
- `build_on(host, target)` — run make on remote host
- `run_rpi5_background(host, args)` — start RPi5 program via SSH in background
- `run_rpi4_measure(host, args)` — run RPi4 measurement, capture output
- `stop_rpi5(host)` — send SIGINT to RPi5 program

**Step 2: Commit**

```bash
git add run_latency_benchmark.py
git commit -m "Add Python orchestrator for coordinated latency benchmarks

Orchestrates RPi4 + RPi5 programs via SSH: copies source, builds on
each device, starts RPi5 PIO echo, runs RPi4 measurement, collects
results. Supports all test layers and JSON output."
```

---

## Task 9: Add L2 Support (DMA Path)

**Files:**
- Modify: `latency/latency_rpi5.c`

**Step 1: Implement run_l2()**

Add a `run_l2()` function that:
1. Uses the same edge_detector + output_driver PIO programs as L1
2. Configures DMA via `pio_sm_config_xfer()` for minimal transfer size (4 bytes)
3. Uses `pio_sm_xfer_data()` in a tight loop for single-word transfers
4. Measures internal CPU time per edge

**Note:** This may behave similarly to L1 since `pio_sm_xfer_data()` is blocking
and goes through the same kernel path. The key difference is it uses the DMA engine
rather than firmware mailbox for FIFO access. If the DMA path doesn't provide a
measurable improvement, document this finding.

**Step 2: Test on hardware**

Run the same RPi4 + RPi5 coordinated test as Task 7 but with `--test=L2`.

**Step 3: Commit**

```bash
git add latency/latency_rpi5.c
git commit -m "Add L2 (DMA path) latency test

Uses pio_sm_xfer_data() for single-word DMA transfers instead of
pio_sm_get/put_blocking() ioctl calls."
```

---

## Task 10: Add L3 Support (mmap FIFO, Experimental)

**Files:**
- Modify: `latency/latency_rpi5.c`

**Step 1: Research FIFO register offsets**

Before implementing, verify the PIO FIFO register layout on the actual hardware:
```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com "sudo cat /proc/iomem | grep pio"
```

The RP1 PIO FIFO registers should be at physical address `0x1f00178000`.
From the RP2040 datasheet (RP1 compatible), the FIFO registers are:
- `TXF0` (TX FIFO SM0): offset 0x10
- `TXF1` (TX FIFO SM1): offset 0x14
- `RXF0` (RX FIFO SM0): offset 0x20
- `RXF1` (RX FIFO SM1): offset 0x24
- `FSTAT` (FIFO status): offset 0x04

**IMPORTANT:** These offsets must be verified against the actual RP1 PIO register
map. The RP1 PIO register layout may differ from RP2040. Check MichaelBell's
rp1-hacking repository and the RP1 peripherals datasheet.

**Step 2: Implement run_l3()**

Add a `run_l3()` function that:
1. Opens `/dev/mem` and mmaps the PIO FIFO register page
2. Uses the same edge_detector + output_driver PIO programs
3. In the CPU loop: directly reads RX FIFO register, writes TX FIFO register
4. Checks FSTAT register to avoid reading empty FIFO / writing full FIFO
5. Measures internal CPU time per edge

**Step 3: Test on hardware**

This test MUST be done carefully:
- Run L3 test in isolation (don't run L0-L2 simultaneously)
- Watch for kernel oops or PIO malfunction
- If it doesn't work, document why and mark as "not feasible"

**Step 4: Commit**

```bash
git add latency/latency_rpi5.c
git commit -m "Add L3 (mmap FIFO) experimental latency test

Bypasses kernel driver to directly read/write PIO FIFO registers via
mmap of /dev/mem. Expected to achieve ~320 ns - 1 us latency."
```

---

## Task 11: Final Integration and Documentation

**Files:**
- Modify: `README.md` — add latency benchmark section
- Modify: `docs/plans/2026-03-02-latency-benchmark-design.md` — update with actual results

**Step 1: Run complete benchmark suite**

Execute all test layers (L0, L1, L2, L3) with the orchestrator script and
collect results. Run each test with and without RT optimisations.

**Step 2: Update README with results**

Add a "Latency Benchmark" section to `README.md` with:
- How to run the latency tests
- Table of measured results for each layer
- Latency breakdown diagram
- Comparison with theoretical expectations

**Step 3: Update design doc with actual measurements**

Replace "Expected latency" values in the design doc with actual measured values.

**Step 4: Final commit**

```bash
git add README.md docs/plans/2026-03-02-latency-benchmark-design.md
git commit -m "Document latency benchmark results

Add measured latency values for all test layers (L0-L3) with and
without RT optimisations. Update design doc with actual results."
```
