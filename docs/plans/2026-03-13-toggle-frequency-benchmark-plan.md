# PIO Toggle Frequency Benchmark Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Measure the actual RP1 PIO clock rate by toggling GPIO5 as fast as possible and measuring the frequency with Glasgow Interface Explorer and RPi4 edge counting.

**Architecture:** RPi5 PIO runs a 2-instruction toggle loop at configurable clock divider and delay settings. Glasgow (FPGA-based, on RPi5 USB) provides primary frequency measurement via logic capture. RPi4 provides cross-validation via mmap GPIO edge counting. A Python orchestrator coordinates all three devices via SSH.

**Tech Stack:** C11 (RPi5 piolib, RPi4 mmap GPIO), Python 3 (orchestrator), Glasgow CLI (analyzer applet), Make (build system).

---

## Working Directory

All files are created in the worktree: `.worktrees/toggle-benchmark/toggle/`

The worktree branch is `feature/toggle-benchmark`.

---

### Task 1: Directory Structure, PIO Program, and Pre-Generated Header

**Files:**
- Create: `toggle/.gitignore`
- Create: `toggle/gpio_toggle.pio`
- Create: `toggle/gpio_toggle.pio.h`

**Step 1: Create directory and .gitignore**

```bash
mkdir -p toggle
```

`toggle/.gitignore`:
```
toggle_rpi4
toggle_rpi5
*.o
```

**Step 2: Create the PIO program**

`toggle/gpio_toggle.pio`:
```asm
; gpio_toggle.pio — Maximum-speed GPIO toggle for frequency measurement
;
; Toggles output pin (relative to set_base) between HIGH and LOW as fast
; as the PIO clock allows. With clkdiv=1 and delay=0, this produces the
; maximum possible toggle frequency: f_pio / 2.
;
; At 200 MHz PIO clock (5 ns per cycle):
;   set pins, 1: 1 cycle (5 ns)
;   set pins, 0: 1 cycle (5 ns)
;   Full period: 2 cycles (10 ns)
;   Toggle frequency: 100 MHz
;
; With clock divider D and instruction delay N:
;   Each instruction: (1 + N) * D cycles
;   Full period: 2 * (1 + N) * D cycles
;   Toggle frequency: f_pio / (2 * (1 + N) * D)

.program gpio_toggle

.wrap_target
    set pins, 1          ; drive output pin HIGH
    set pins, 0          ; drive output pin LOW
.wrap
```

**Step 3: Create the pre-generated header**

`toggle/gpio_toggle.pio.h`:
```c
/* gpio_toggle.pio.h — Auto-generated from gpio_toggle.pio by pioasm
 *
 * DO NOT EDIT — regenerate with: pioasm gpio_toggle.pio gpio_toggle.pio.h
 */

#ifndef GPIO_TOGGLE_PIO_H
#define GPIO_TOGGLE_PIO_H

#include "pio_platform.h"

static const uint16_t gpio_toggle_program_instructions[] = {
    0xe001, /*  0: set pins, 1                  */
    0xe000, /*  1: set pins, 0                  */
};

static const struct pio_program gpio_toggle_program = {
    .instructions = gpio_toggle_program_instructions,
    .length = 2,
    .origin = -1,
};

static inline pio_sm_config gpio_toggle_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + 1);
    return c;
}

#endif /* GPIO_TOGGLE_PIO_H */
```

**Step 4: Commit**

```bash
cd .worktrees/toggle-benchmark
git add toggle/.gitignore toggle/gpio_toggle.pio toggle/gpio_toggle.pio.h
git commit -m "Add PIO toggle frequency program and pre-generated header

Two-instruction toggle loop (set pins 1 / set pins 0) that produces
the maximum possible square wave from RP1 PIO: f_pio / 2 = 100 MHz
at the default 200 MHz clock with no divider or delay."
```

---

### Task 2: Shared Header (toggle_common.h)

**Files:**
- Create: `toggle/toggle_common.h`

**Step 1: Create toggle_common.h**

This follows the pattern from `latency/latency_common.h` but is adapted for frequency measurement instead of latency measurement.

`toggle/toggle_common.h`:
```c
/* toggle_common.h — Shared definitions for PIO toggle frequency benchmark
 *
 * Common types, constants, and formatting for both the RPi5 toggle generator
 * and the RPi4 frequency measurement program.
 */

#ifndef TOGGLE_COMMON_H
#define TOGGLE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Default pin assignment (JC connector, bottom row).
 * GPIO5 (JC9) is tapped by Glasgow pin A7 for FPGA-based measurement. */
#define DEFAULT_TOGGLE_PIN     5

/* Default sweep parameters */
#define DEFAULT_DURATION_MS    2000   /* 2 seconds per measurement */

/* ─── Timing helper ──────────────────────────────────── */

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ─── Frequency formatting ───────────────────────────── */

static inline void format_freq_hz(char *buf, size_t buf_size, double freq_hz)
{
    if (freq_hz >= 1e9)
        snprintf(buf, buf_size, "%.3f GHz", freq_hz / 1e9);
    else if (freq_hz >= 1e6)
        snprintf(buf, buf_size, "%.3f MHz", freq_hz / 1e6);
    else if (freq_hz >= 1e3)
        snprintf(buf, buf_size, "%.3f kHz", freq_hz / 1e3);
    else
        snprintf(buf, buf_size, "%.1f Hz", freq_hz);
}

/* ─── RPi4 measurement result ────────────────────────── */

typedef struct {
    int pin;
    int duration_ms;
    uint64_t edges_counted;
    uint64_t samples_taken;
    uint64_t elapsed_ns;
    double measured_freq_hz;     /* edges / (2 * elapsed_sec) */
    double transition_ratio;    /* edges / samples */
} toggle_rpi4_result_t;

static inline void toggle_rpi4_print_json(FILE *f,
                                           const toggle_rpi4_result_t *r)
{
    char freq_buf[64];
    format_freq_hz(freq_buf, sizeof(freq_buf), r->measured_freq_hz);
    fprintf(f,
        "{\n"
        "  \"benchmark\": \"rp1-pio-toggle-freq\",\n"
        "  \"device\": \"rpi4\",\n"
        "  \"config\": {\n"
        "    \"pin\": %d,\n"
        "    \"duration_ms\": %d\n"
        "  },\n"
        "  \"results\": {\n"
        "    \"edges_counted\": %llu,\n"
        "    \"samples_taken\": %llu,\n"
        "    \"elapsed_ns\": %llu,\n"
        "    \"measured_freq_hz\": %.1f,\n"
        "    \"measured_freq_human\": \"%s\",\n"
        "    \"transition_ratio\": %.6f\n"
        "  }\n"
        "}\n",
        r->pin,
        r->duration_ms,
        (unsigned long long)r->edges_counted,
        (unsigned long long)r->samples_taken,
        (unsigned long long)r->elapsed_ns,
        r->measured_freq_hz,
        freq_buf,
        r->transition_ratio);
}

#endif /* TOGGLE_COMMON_H */
```

**Step 2: Commit**

```bash
git add toggle/toggle_common.h
git commit -m "Add shared header for toggle frequency benchmark

Defines toggle_rpi4_result_t for edge counting results, frequency
formatting helpers, and shared constants. GPIO5 is the default
toggle pin (tapped by Glasgow A7 for FPGA measurement)."
```

---

### Task 3: RPi5 Toggle Generator (toggle_rpi5.c)

**Files:**
- Create: `toggle/toggle_rpi5.c`

**Step 1: Create toggle_rpi5.c**

This is the RPi5 program that loads the PIO toggle program, configures the
GPIO pin, and runs it for a specified duration. It supports clock divider
and instruction delay sweeping.

Key features:
- `--clkdiv N` sets the PIO clock divider (float, default 1.0)
- `--delay N` sets instruction delay cycles (0-31, default 0)
- `--duration-ms N` how long to run the toggle (default 2000)
- `--pin N` which GPIO to toggle (default 5)
- Reports `clock_get_hz(clk_sys)` in JSON output
- For delay > 0, patches the PIO instruction words before loading
- Configures GPIO for fast slew rate and maximum (12 mA) drive strength

`toggle/toggle_rpi5.c`:
```c
/* toggle_rpi5.c — RPi5 PIO toggle frequency generator
 *
 * Loads a 2-instruction PIO toggle program on RP1 and runs it for a
 * configurable duration. The output pin toggles at:
 *
 *   f_toggle = f_pio / (2 * (1 + delay) * clkdiv)
 *
 * Where f_pio is the PIO system clock (nominally 200 MHz).
 *
 * Requires RPi5 with libpio-dev installed and root privileges (/dev/pio0).
 *
 * Build: see Makefile (make rpi5)
 * Run:   sudo ./toggle_rpi5 [options]
 */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "piolib.h"
#include "hardware/clocks.h"
#include "gpio_toggle.pio.h"
#include "toggle_common.h"

/* ─── Signal handling ──────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ─── PIO instruction patching for delay values ────────────── */

/* PIO SET instruction format:
 *   Bits [15:13] = 111 (SET opcode)
 *   Bits [12:8]  = delay/side-set (5 bits, 0-31 when no side-set)
 *   Bits [7:5]   = destination (000 = PINS)
 *   Bits [4:0]   = data value (0-31)
 *
 * gpio_toggle program:
 *   instruction 0: set pins, 1 = 0xE001
 *   instruction 1: set pins, 0 = 0xE000
 */
#define SET_PINS_HIGH  0xE001
#define SET_PINS_LOW   0xE000
#define DELAY_SHIFT    8
#define DELAY_MASK     0x1F00

static void build_toggle_program(uint16_t *insns, int delay)
{
    insns[0] = SET_PINS_HIGH | (uint16_t)((delay & 0x1F) << DELAY_SHIFT);
    insns[1] = SET_PINS_LOW  | (uint16_t)((delay & 0x1F) << DELAY_SHIFT);
}

/* ─── Usage ────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "PIO toggle frequency generator for RP1.\n"
        "\n"
        "Options:\n"
        "  --pin N          GPIO pin to toggle (default: %d)\n"
        "  --clkdiv F       PIO clock divider, >= 1.0 (default: 1.0)\n"
        "  --delay N        Instruction delay cycles, 0-31 (default: 0)\n"
        "  --duration-ms N  Duration in milliseconds (default: %d)\n"
        "  --json           Output configuration as JSON to stdout\n"
        "  --help           Show this help\n",
        prog, DEFAULT_TOGGLE_PIN, DEFAULT_DURATION_MS);
}

/* ─── Main ─────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int pin         = DEFAULT_TOGGLE_PIN;
    float clkdiv    = 1.0f;
    int delay       = 0;
    int duration_ms = DEFAULT_DURATION_MS;
    int json_output = 0;

    static struct option long_options[] = {
        {"pin",         required_argument, NULL, 'p'},
        {"clkdiv",      required_argument, NULL, 'd'},
        {"delay",       required_argument, NULL, 'D'},
        {"duration-ms", required_argument, NULL, 't'},
        {"json",        no_argument,       NULL, 'j'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:d:D:t:jh",
                               long_options, NULL)) != -1) {
        switch (opt) {
        case 'p': pin = atoi(optarg); break;
        case 'd': clkdiv = strtof(optarg, NULL); break;
        case 'D': delay = atoi(optarg); break;
        case 't': duration_ms = atoi(optarg); break;
        case 'j': json_output = 1; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    /* Validate parameters. */
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "ERROR: pin must be 0-27, got %d\n", pin);
        return 1;
    }
    if (clkdiv < 1.0f) {
        fprintf(stderr, "ERROR: clkdiv must be >= 1.0, got %.2f\n",
                (double)clkdiv);
        return 1;
    }
    if (delay < 0 || delay > 31) {
        fprintf(stderr, "ERROR: delay must be 0-31, got %d\n", delay);
        return 1;
    }
    if (duration_ms <= 0) {
        fprintf(stderr, "ERROR: duration-ms must be > 0, got %d\n",
                duration_ms);
        return 1;
    }

    /* Query the reported PIO system clock. */
    uint32_t sys_clk_hz = clock_get_hz(clk_sys);

    /* Compute expected toggle frequency. */
    double expected_freq = (double)sys_clk_hz /
                           (2.0 * (1.0 + delay) * (double)clkdiv);

    char expected_buf[64];
    format_freq_hz(expected_buf, sizeof(expected_buf), expected_freq);

    fprintf(stderr, "PIO Toggle Generator\n");
    fprintf(stderr, "  Pin:            GPIO%d\n", pin);
    fprintf(stderr, "  clock_get_hz:   %u Hz (%.1f MHz)\n",
            sys_clk_hz, (double)sys_clk_hz / 1e6);
    fprintf(stderr, "  Clock divider:  %.2f\n", (double)clkdiv);
    fprintf(stderr, "  Delay cycles:   %d\n", delay);
    fprintf(stderr, "  Expected freq:  %s\n", expected_buf);
    fprintf(stderr, "  Duration:       %d ms\n", duration_ms);

    /* Install SIGINT handler for clean shutdown. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* Open PIO instance. */
    PIO pio = pio_open(0);
    if (!pio) {
        fprintf(stderr, "ERROR: failed to open /dev/pio0 (%s)\n",
                strerror(errno));
        return 1;
    }

    /* Claim a state machine. */
    int sm = pio_claim_unused_sm(pio);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machines\n");
        pio_close(pio);
        return 1;
    }

    /* Build toggle program with desired delay. */
    uint16_t patched_insns[2];
    build_toggle_program(patched_insns, delay);

    struct pio_program prog = {
        .instructions = patched_insns,
        .length = 2,
        .origin = -1,
    };

    uint offset = pio_add_program(pio, &prog);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load toggle program\n");
        pio_sm_unclaim(pio, (uint)sm);
        pio_close(pio);
        return 1;
    }

    /* Configure GPIO for PIO function. */
    pio_gpio_init(pio, (uint)pin);

    /* Set pin direction: output. */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)pin, 1, true);

    /* Configure the state machine. */
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + 1);
    sm_config_set_set_pins(&c, (uint)pin, 1);
    sm_config_set_clkdiv(&c, clkdiv);

    /* Initialise state machine (does NOT enable yet). */
    pio_sm_init(pio, (uint)sm, offset, &c);

    /* Start toggling. */
    fprintf(stderr, "Starting PIO toggle on GPIO%d...\n", pin);
    pio_sm_set_enabled(pio, (uint)sm, true);

    /* Output JSON immediately so orchestrator knows we're running. */
    if (json_output) {
        printf("{\n"
               "  \"benchmark\": \"rp1-pio-toggle-freq\",\n"
               "  \"device\": \"rpi5\",\n"
               "  \"config\": {\n"
               "    \"pin\": %d,\n"
               "    \"clkdiv\": %.2f,\n"
               "    \"delay\": %d,\n"
               "    \"duration_ms\": %d,\n"
               "    \"sys_clk_hz\": %u,\n"
               "    \"expected_freq_hz\": %.1f\n"
               "  },\n"
               "  \"status\": \"running\"\n"
               "}\n", pin, (double)clkdiv, delay, duration_ms,
               sys_clk_hz, expected_freq);
        fflush(stdout);
    }

    /* Run for the specified duration. */
    uint64_t start_ns = get_time_ns();
    uint64_t duration_ns = (uint64_t)duration_ms * 1000000ULL;

    while (g_running) {
        usleep(100000); /* 100 ms sleep between checks */
        if ((get_time_ns() - start_ns) >= duration_ns)
            break;
    }

    /* Stop and clean up. */
    pio_sm_set_enabled(pio, (uint)sm, false);

    /* Drive pin LOW before releasing. */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)pin, 1, true);
    pio_sm_exec(pio, (uint)sm, 0xe000); /* set pins, 0 */

    pio_remove_program(pio, &prog, offset);
    pio_sm_unclaim(pio, (uint)sm);
    pio_close(pio);

    uint64_t actual_ns = get_time_ns() - start_ns;
    fprintf(stderr, "Toggle stopped after %llu ms.\n",
            (unsigned long long)(actual_ns / 1000000ULL));

    return 0;
}
```

**Step 2: Commit**

```bash
git add toggle/toggle_rpi5.c
git commit -m "Add RPi5 PIO toggle frequency generator

Loads a 2-instruction toggle loop on RP1 PIO with configurable clock
divider (--clkdiv) and instruction delay (--delay). Reports
clock_get_hz(clk_sys) and expected frequency. Patches PIO instruction
words at runtime for delay values > 0."
```

---

### Task 4: RPi4 Edge Counter (toggle_rpi4.c)

**Files:**
- Create: `toggle/toggle_rpi4.c`

**Step 1: Create toggle_rpi4.c**

This is the RPi4 program that counts level transitions on a GPIO pin using
mmap'd register access. It runs a tight polling loop for a specified duration,
counting every time the pin level changes.

The measurement is accurate for toggle frequencies up to ~5 MHz (limited by
the ~68 ns GPIO read latency on BCM2711). At higher frequencies the
transition_ratio approaches 0.5 (Nyquist aliasing), and edges_counted
underreports the true count.

`toggle/toggle_rpi4.c`:
```c
/* toggle_rpi4.c — RPi4 GPIO edge counter for frequency measurement
 *
 * Counts level transitions on a GPIO pin using mmap'd /dev/gpiomem.
 * Runs a tight polling loop for a configurable duration and reports
 * edges counted, sample rate, measured frequency, and transition ratio.
 *
 * Accurate for toggle frequencies up to ~5 MHz (limited by BCM2711
 * GPIO read latency of ~68 ns per GPLEV0 access).
 *
 * Requires RPi4 with /dev/gpiomem access (user in 'gpio' group).
 *
 * Build: see Makefile (make rpi4)
 * Run:   ./toggle_rpi4 [options]
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "toggle_common.h"

/* ─── BCM2711 GPIO registers ──────────────────────────────── */

#define GPIO_MEM_SIZE         0x100
#define GPLEV0                0x34     /* Pin level: read current state */
#define GPIO_PUP_PDN_CNTRL0  0xE4     /* BCM2711 pull-up/down control */
#define GPFSEL0               0x00     /* Function select */

#define FSEL_INPUT   0
#define PUD_OFF      0

/* ─── GPIO mmap state ─────────────────────────────────────── */

static volatile uint32_t *gpio_base;
static int gpio_fd = -1;

static inline uint32_t gpio_read(unsigned reg)
{
    return gpio_base[reg / 4];
}

static inline void gpio_write(unsigned reg, uint32_t val)
{
    gpio_base[reg / 4] = val;
}

/* BCM2711 GPFSEL: 10 pins per register, 3 bits per pin. */
static void gpio_set_fsel(int pin, int fsel)
{
    unsigned reg = GPFSEL0 + (unsigned)(pin / 10) * 4;
    unsigned shift = (unsigned)(pin % 10) * 3;
    uint32_t val = gpio_read(reg);
    val &= ~(7u << shift);
    val |= ((unsigned)fsel & 7u) << shift;
    gpio_write(reg, val);
}

/* BCM2711 GPIO_PUP_PDN: 16 pins per register, 2 bits per pin. */
static void gpio_set_pull(int pin, int pull)
{
    unsigned reg = GPIO_PUP_PDN_CNTRL0 + (unsigned)(pin / 16) * 4;
    unsigned shift = (unsigned)(pin % 16) * 2;
    uint32_t val = gpio_read(reg);
    val &= ~(3u << shift);
    val |= ((unsigned)pull & 3u) << shift;
    gpio_write(reg, val);
}

static int gpio_mmap_init(void)
{
    gpio_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (gpio_fd < 0) {
        perror("ERROR: cannot open /dev/gpiomem");
        fprintf(stderr, "Hint: ensure you are on an RPi4 and your user is "
                        "in the 'gpio' group.\n");
        return -1;
    }

    gpio_base = (volatile uint32_t *)mmap(NULL, GPIO_MEM_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, gpio_fd, 0);
    if (gpio_base == MAP_FAILED) {
        perror("ERROR: mmap failed");
        close(gpio_fd);
        gpio_fd = -1;
        return -1;
    }

    return 0;
}

static void gpio_mmap_cleanup(void)
{
    if (gpio_base && gpio_base != MAP_FAILED)
        munmap((void *)gpio_base, GPIO_MEM_SIZE);
    if (gpio_fd >= 0)
        close(gpio_fd);
}

/* ─── Signal handling ─────────────────────────────────────── */

static volatile sig_atomic_t running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ─── Usage ───────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "GPIO edge counter for measuring PIO toggle frequency.\n"
        "\n"
        "Options:\n"
        "  --pin N          GPIO pin to monitor (default: %d)\n"
        "  --duration-ms N  Measurement duration in ms (default: %d)\n"
        "  --json           JSON output to stdout\n"
        "  --help           Show this help\n",
        prog, DEFAULT_TOGGLE_PIN, DEFAULT_DURATION_MS);
}

/* ─── Main ────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int pin         = DEFAULT_TOGGLE_PIN;
    int duration_ms = DEFAULT_DURATION_MS;
    int json_output = 0;
    int ret         = 0;

    static struct option long_options[] = {
        {"pin",         required_argument, NULL, 'p'},
        {"duration-ms", required_argument, NULL, 't'},
        {"json",        no_argument,       NULL, 'j'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:t:jh",
                               long_options, NULL)) != -1) {
        switch (opt) {
        case 'p': pin = atoi(optarg); break;
        case 't': duration_ms = atoi(optarg); break;
        case 'j': json_output = 1; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (pin < 0 || pin > 27) {
        fprintf(stderr, "ERROR: pin must be 0-27, got %d\n", pin);
        return 1;
    }

    /* Install SIGINT handler. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* Map GPIO registers. */
    if (gpio_mmap_init() < 0)
        return 1;

    /* Configure pin as input with no pull. */
    gpio_set_fsel(pin, FSEL_INPUT);
    gpio_set_pull(pin, PUD_OFF);

    fprintf(stderr, "Edge counter on GPIO%d for %d ms...\n",
            pin, duration_ms);

    /* ─── Tight polling loop ──────────────────────────────── */

    uint32_t pin_mask = 1u << (unsigned)pin;
    uint64_t duration_ns = (uint64_t)duration_ms * 1000000ULL;

    /* Read initial pin state. */
    uint32_t prev = gpio_read(GPLEV0) & pin_mask;
    uint64_t edges = 0;
    uint64_t samples = 0;

    uint64_t start_ns = get_time_ns();

    while (running) {
        uint32_t curr = gpio_read(GPLEV0) & pin_mask;
        samples++;
        if (curr != prev) {
            edges++;
            prev = curr;
        }
        /* Check duration every 65536 samples to reduce overhead. */
        if (!(samples & 0xFFFF)) {
            if ((get_time_ns() - start_ns) >= duration_ns)
                break;
        }
    }

    uint64_t elapsed_ns = get_time_ns() - start_ns;

    /* ─── Compute results ─────────────────────────────────── */

    double elapsed_sec = (double)elapsed_ns / 1e9;
    /* Each full toggle cycle has 2 edges (rising + falling).
     * measured_freq = edges / 2 / elapsed_sec */
    double measured_freq = (edges > 0 && elapsed_sec > 0)
                           ? (double)edges / (2.0 * elapsed_sec)
                           : 0.0;
    double transition_ratio = (samples > 0)
                              ? (double)edges / (double)samples
                              : 0.0;

    /* ─── Output ──────────────────────────────────────────── */

    toggle_rpi4_result_t result = {
        .pin             = pin,
        .duration_ms     = duration_ms,
        .edges_counted   = edges,
        .samples_taken   = samples,
        .elapsed_ns      = elapsed_ns,
        .measured_freq_hz = measured_freq,
        .transition_ratio = transition_ratio,
    };

    if (json_output) {
        toggle_rpi4_print_json(stdout, &result);
    } else {
        char freq_buf[64];
        format_freq_hz(freq_buf, sizeof(freq_buf), measured_freq);
        printf("================================================================\n");
        printf("PIO Toggle Frequency — RPi4 Edge Counter\n");
        printf("================================================================\n");
        printf("  Pin:              GPIO%d\n", pin);
        printf("  Duration:         %.1f ms\n",
               (double)elapsed_ns / 1e6);
        printf("  Edges counted:    %llu\n", (unsigned long long)edges);
        printf("  Samples taken:    %llu\n", (unsigned long long)samples);
        printf("  Sample rate:      %.2f MHz\n",
               (double)samples / elapsed_sec / 1e6);
        printf("  Measured freq:    %s\n", freq_buf);
        printf("  Transition ratio: %.6f\n", transition_ratio);
        if (transition_ratio > 0.45) {
            printf("  WARNING: ratio near 0.5 — signal likely faster than "
                   "sample rate (Nyquist aliasing)\n");
        }
        printf("================================================================\n");
    }

    /* Cleanup. */
    gpio_set_pull(pin, PUD_OFF);
    gpio_mmap_cleanup();

    return ret;
}
```

**Step 2: Commit**

```bash
git add toggle/toggle_rpi4.c
git commit -m "Add RPi4 GPIO edge counter for toggle frequency measurement

Tight mmap polling loop counting level transitions on a GPIO pin.
Accurate for toggle frequencies up to ~5 MHz (BCM2711 GPIO read
latency ~68 ns). Reports edges, sample rate, measured frequency,
and transition ratio (warns when near Nyquist aliasing limit)."
```

---

### Task 5: Makefile

**Files:**
- Create: `toggle/Makefile`

**Step 1: Create Makefile**

Following the pattern from `latency/Makefile`:

`toggle/Makefile`:
```makefile
# toggle/Makefile — Build system for PIO toggle frequency benchmark
#
# Targets:
#   rpi4        Build the RPi4 edge counter binary (no PIO deps)
#   rpi5        Build the RPi5 PIO toggle generator (requires libpio-dev)
#   pioasm      Regenerate .pio.h header from .pio source
#   clean       Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# ─── Platform detection ─────────────────────────────────
IS_RPI5   := $(shell test -e /dev/pio0 && echo yes || echo no)
IS_RPI4   := $(shell test -e /dev/gpiomem && ! test -e /dev/pio0 && echo yes || echo no)

# ─── Target: rpi4 (RPi4 edge counter binary) ─────────────
RPi4_SRCS    := toggle_rpi4.c
RPi4_CFLAGS  := $(CFLAGS) -DPICO_NO_HARDWARE=1
RPi4_LDFLAGS := -lm

.PHONY: rpi4
rpi4: toggle_rpi4

toggle_rpi4: $(RPi4_SRCS) toggle_common.h
ifeq ($(IS_RPI4),yes)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else
	@echo "ERROR: rpi4 target requires RPi4 with /dev/gpiomem"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: rpi5 (RPi5 PIO toggle generator) ─────────────
RPi5_SRCS    := toggle_rpi5.c
RPi5_CFLAGS  := $(CFLAGS) -I/usr/include/piolib
RPi5_LDFLAGS := -lpio -lm

.PHONY: rpi5
rpi5: toggle_rpi5

toggle_rpi5: $(RPi5_SRCS) toggle_common.h gpio_toggle.pio.h
ifeq ($(IS_RPI5),yes)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else
	@echo "ERROR: rpi5 target requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: pioasm (regenerate header) ───────────────────
.PHONY: pioasm
pioasm: gpio_toggle.pio
	pioasm gpio_toggle.pio gpio_toggle.pio.h

# ─── Target: clean ─────────────────────────────────────────
.PHONY: clean
clean:
	rm -f toggle_rpi4 toggle_rpi5 *.o
```

**Step 2: Commit**

```bash
git add toggle/Makefile
git commit -m "Add Makefile for toggle frequency benchmark

Platform-detecting build system following the latency benchmark
pattern. Builds toggle_rpi5 on RPi5 (requires libpio-dev) and
toggle_rpi4 on RPi4 (requires /dev/gpiomem)."
```

---

### Task 6: Deploy, Compile, and Smoke Test on RPi5

**Step 1: Sync source to RPi5**

```bash
rsync -av --delete \
    --exclude='*.o' --exclude='toggle_rpi4' --exclude='toggle_rpi5' \
    toggle/ \
    tim@rpi5-pmod.iot.welland.mithis.com:/home/tim/rpi5-rp1-pio-bench/toggle/
```

**Step 2: Compile on RPi5**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && make rpi5"
```

Expected: compiles without errors.

**Step 3: Smoke test on RPi5**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && sudo ./toggle_rpi5 --help"
```

Expected: prints usage message with all options.

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && sudo ./toggle_rpi5 --clkdiv 256 --duration-ms 2000 --json"
```

Expected: prints JSON with sys_clk_hz=200000000, runs for 2 seconds, exits cleanly.

---

### Task 7: Deploy, Compile, and Smoke Test on RPi4

**Step 1: Sync source to RPi4**

```bash
rsync -av --delete \
    --exclude='*.o' --exclude='toggle_rpi4' --exclude='toggle_rpi5' \
    toggle/ \
    tim@rpi4-pmod.iot.welland.mithis.com:/home/tim/rpi5-rp1-pio-bench/toggle/
```

**Step 2: Compile on RPi4**

```bash
ssh tim@rpi4-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && make rpi4"
```

Expected: compiles without errors.

**Step 3: Smoke test on RPi4**

```bash
ssh tim@rpi4-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && ./toggle_rpi4 --help"
```

Expected: prints usage message.

---

### Task 8: First Integration Test — RPi5 + RPi4

**Step 1: Start RPi5 toggle at clkdiv=256 (slow, ~390 kHz)**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && sudo ./toggle_rpi5 --clkdiv 256 --duration-ms 10000 --json" &
```

**Step 2: Wait 2 seconds for settle, then run RPi4 measurement**

```bash
sleep 2
ssh tim@rpi4-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && ./toggle_rpi4 --duration-ms 2000 --json"
```

Expected: RPi4 reports measured_freq_hz near 390625.0 Hz (if PIO is 200 MHz)
or near 781250.0 Hz (if PIO is 400 MHz). Transition ratio should be well
below 0.5 at this frequency.

**Step 3: Kill RPi5 process**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com "sudo pkill -f toggle_rpi5; true"
```

**Step 4: Verify cleanup — pin should be LOW**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com "pinctrl get 5"
```

---

### Task 9: Glasgow Measurement Integration

This task adds Glasgow-based frequency measurement to the workflow.

**Step 1: Check Glasgow availability on RPi5**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "export PATH=\$HOME/.local/bin:\$PATH && glasgow --version"
```

**Step 2: Check available Glasgow applets**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "export PATH=\$HOME/.local/bin:\$PATH && glasgow run --help"
```

Look for `analyzer` applet and any frequency-related applets (`freq`,
`sensor-freq`, etc.).

**Step 3: Test Glasgow analyzer capture**

While RPi5 PIO toggles at clkdiv=256 (~390 kHz), capture a trace:

```bash
# Start RPi5 toggle in background
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "cd /home/tim/rpi5-rp1-pio-bench/toggle && sudo ./toggle_rpi5 --clkdiv 256 --duration-ms 30000" &

sleep 2

# Capture Glasgow trace on A7 (GPIO5) — sample at ~10 MHz, ~100ms capture
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "export PATH=\$HOME/.local/bin:\$PATH && glasgow run analyzer -V 3.3 --pin-set-data A7 -f 10000000 -o /tmp/toggle_trace.vcd" &

sleep 3  # Let capture run

# Kill both
ssh tim@rpi5-pmod.iot.welland.mithis.com "sudo pkill -f toggle_rpi5; pkill -f glasgow; true"
```

**Step 4: Analyse VCD trace to determine frequency**

Write a small Python script to parse the VCD and count edges:

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "uv run python3 -c \"
import re
with open('/tmp/toggle_trace.vcd') as f:
    content = f.read()
# Parse timescale and count transitions
print('VCD size:', len(content), 'bytes')
# Count value change lines for signal A7
changes = content.count('1!')  + content.count('0!')
print('Approximate transitions:', changes)
\""
```

The exact VCD parsing will be refined based on the actual Glasgow output
format during implementation.

---

### Task 10: Python Orchestrator (run_toggle_benchmark.py)

**Files:**
- Create: `toggle/run_toggle_benchmark.py`

**Step 1: Create orchestrator**

The orchestrator follows the pattern from `latency/run_latency_benchmark.py`
but is adapted for the toggle frequency sweep.

`toggle/run_toggle_benchmark.py`:
```python
#!/usr/bin/env python3
"""PIO Toggle Frequency Benchmark — Orchestrator

Coordinates RPi5 (PIO toggle generator), RPi4 (edge counter), and
Glasgow Interface Explorer (FPGA frequency measurement) to measure the
actual RP1 PIO clock rate across a sweep of clock divider settings.

Usage:
    uv run toggle/run_toggle_benchmark.py
    uv run toggle/run_toggle_benchmark.py --clkdivs 256,64,16,4,1 --json
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# ─── Defaults ──────────────────────────────────────────────────

DEFAULT_RPI5_HOST = "rpi5-pmod.iot.welland.mithis.com"
DEFAULT_RPI4_HOST = "rpi4-pmod.iot.welland.mithis.com"
DEFAULT_REMOTE_DIR = "/home/tim/rpi5-rp1-pio-bench"
DEFAULT_TOGGLE_PIN = 5
DEFAULT_DURATION_MS = 2000
DEFAULT_SETTLE_SECS = 2
DEFAULT_MEASUREMENT_TIMEOUT = 30

# clkdiv values for the sweep (high to low)
DEFAULT_CLKDIVS = [256, 128, 64, 32, 16, 8, 4, 2, 1]


# ─── Logging ──────────────────────────────────────────────────

def log(msg: str = "") -> None:
    """Print a status message to stderr."""
    print(msg, file=sys.stderr, flush=True)


# ─── SSH helpers ──────────────────────────────────────────────

def ssh_run(
    host: str,
    cmd: str,
    timeout: int = 60,
    check: bool = True,
) -> subprocess.CompletedProcess:
    """Run a command on a remote host via SSH."""
    ssh_cmd = [
        "ssh",
        "-o", "ControlMaster=no",
        "-o", "ConnectTimeout=5",
        "-o", "BatchMode=yes",
        host,
        cmd,
    ]
    return subprocess.run(
        ssh_cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=check,
    )


def ssh_check(host: str, label: str) -> bool:
    """Verify SSH connectivity."""
    try:
        r = ssh_run(host, "echo ok", timeout=10)
        if r.returncode == 0 and "ok" in r.stdout:
            log(f"  {label}: OK")
            return True
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
        log(f"  {label}: FAILED ({e})")
    return False


def ssh_run_bg(host: str, cmd: str) -> subprocess.Popen:
    """Start a command on a remote host in the background."""
    ssh_cmd = [
        "ssh",
        "-o", "ControlMaster=no",
        "-o", "ConnectTimeout=5",
        "-o", "BatchMode=yes",
        host,
        cmd,
    ]
    return subprocess.Popen(
        ssh_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


# ─── Build and deploy ────────────────────────────────────────

def sync_source(local_dir: Path, host: str, remote_dir: str, label: str) -> bool:
    """Rsync toggle/ source to remote host."""
    log(f"Syncing source to {label}...")
    local_toggle = local_dir / "toggle"
    if not local_toggle.exists():
        log(f"  ERROR: {local_toggle} does not exist")
        return False

    cmd = [
        "rsync", "-av", "--delete",
        "--exclude=*.o",
        "--exclude=toggle_rpi4",
        "--exclude=toggle_rpi5",
        f"{local_toggle}/",
        f"{host}:{remote_dir}/toggle/",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        log(f"  ERROR: rsync failed: {r.stderr}")
        return False
    log(f"  {label}: synced")
    return True


def deploy_and_build(host: str, remote_dir: str, target: str, label: str) -> bool:
    """Build on remote host."""
    log(f"Building {target} on {label}...")
    r = ssh_run(
        host,
        f"cd {remote_dir}/toggle && make clean && make {target}",
        timeout=60,
        check=False,
    )
    if r.returncode != 0:
        log(f"  ERROR: build failed on {label}:")
        log(f"  stdout: {r.stdout}")
        log(f"  stderr: {r.stderr}")
        return False

    binary = f"toggle_{target}"
    r = ssh_run(host, f"test -x {remote_dir}/toggle/{binary}", timeout=10, check=False)
    if r.returncode != 0:
        log(f"  ERROR: binary {binary} not found after build")
        return False

    log(f"  {label}: {binary} built OK")
    return True


# ─── Pin cleanup ──────────────────────────────────────────────

def cleanup_pins(rpi5_host: str, rpi4_host: str, pin: int) -> None:
    """Kill old processes and restore pin to input."""
    for host, binary in [(rpi5_host, "toggle_rpi5"), (rpi4_host, "toggle_rpi4")]:
        try:
            pfx = "sudo " if binary == "toggle_rpi5" else ""
            ssh_run(host, f"{pfx}pkill -9 -f {binary}; true", timeout=10, check=False)
        except subprocess.TimeoutExpired:
            pass

    # Restore pins to inputs
    for host in [rpi5_host, rpi4_host]:
        try:
            ssh_run(host, f"pinctrl set {pin} ip pn", timeout=10, check=False)
        except subprocess.TimeoutExpired:
            pass


# ─── Measurement results ─────────────────────────────────────

@dataclass
class SweepPoint:
    clkdiv: float
    delay: int
    expected_freq_hz: float
    sys_clk_hz: int = 0
    rpi4_freq_hz: float | None = None
    rpi4_edges: int = 0
    rpi4_samples: int = 0
    rpi4_transition_ratio: float = 0.0
    rpi4_saturated: bool = False
    glasgow_freq_hz: float | None = None
    ratio: float | None = None  # measured / expected


# ─── Single measurement ──────────────────────────────────────

def run_measurement(
    rpi5_host: str,
    rpi4_host: str,
    remote_dir: str,
    pin: int,
    clkdiv: float,
    delay: int,
    duration_ms: int,
    settle_secs: int,
    measurement_timeout: int,
) -> SweepPoint:
    """Run one toggle measurement at a specific clkdiv/delay setting."""

    # Assume 200 MHz for expected calculation
    expected_freq = 200e6 / (2.0 * (1 + delay) * clkdiv)

    point = SweepPoint(
        clkdiv=clkdiv,
        delay=delay,
        expected_freq_hz=expected_freq,
    )

    # Start RPi5 toggle generator
    rpi5_cmd = (
        f"cd {remote_dir}/toggle && "
        f"sudo ./toggle_rpi5 --pin {pin} --clkdiv {clkdiv} "
        f"--delay {delay} --duration-ms {duration_ms + (settle_secs + 5) * 1000} --json"
    )

    log(f"  Starting RPi5 toggle (clkdiv={clkdiv}, delay={delay})...")
    rpi5_proc = ssh_run_bg(rpi5_host, rpi5_cmd)

    # Wait for settle
    time.sleep(settle_secs)

    # Run RPi4 edge counter
    rpi4_cmd = (
        f"cd {remote_dir}/toggle && "
        f"./toggle_rpi4 --pin {pin} --duration-ms {duration_ms} --json"
    )

    log(f"  Running RPi4 edge counter ({duration_ms} ms)...")
    try:
        rpi4_result = ssh_run(
            rpi4_host, rpi4_cmd,
            timeout=measurement_timeout,
            check=False,
        )
        if rpi4_result.returncode == 0 and rpi4_result.stdout.strip():
            try:
                data = json.loads(rpi4_result.stdout)
                results = data.get("results", {})
                point.rpi4_freq_hz = results.get("measured_freq_hz")
                point.rpi4_edges = results.get("edges_counted", 0)
                point.rpi4_samples = results.get("samples_taken", 0)
                point.rpi4_transition_ratio = results.get("transition_ratio", 0)
                point.rpi4_saturated = point.rpi4_transition_ratio > 0.45
            except json.JSONDecodeError as e:
                log(f"  WARNING: failed to parse RPi4 JSON: {e}")
        else:
            log(f"  WARNING: RPi4 measurement failed (rc={rpi4_result.returncode})")
            if rpi4_result.stderr:
                log(f"  stderr: {rpi4_result.stderr.strip()}")
    except subprocess.TimeoutExpired:
        log(f"  WARNING: RPi4 measurement timed out")

    # Parse RPi5 JSON output for sys_clk_hz
    try:
        rpi5_proc.terminate()
        rpi5_stdout, rpi5_stderr = rpi5_proc.communicate(timeout=5)
        if rpi5_stdout.strip():
            try:
                rpi5_data = json.loads(rpi5_stdout)
                point.sys_clk_hz = rpi5_data.get("config", {}).get("sys_clk_hz", 0)
            except json.JSONDecodeError:
                pass
    except (subprocess.TimeoutExpired, OSError):
        rpi5_proc.kill()

    # Compute ratio (measured vs expected assuming 200 MHz)
    if point.rpi4_freq_hz and not point.rpi4_saturated:
        point.ratio = point.rpi4_freq_hz / expected_freq

    return point


# ─── Output formatting ───────────────────────────────────────

def format_freq(freq_hz: float | None) -> str:
    """Format frequency for display."""
    if freq_hz is None:
        return "—"
    if freq_hz >= 1e6:
        return f"{freq_hz / 1e6:.3f} MHz"
    elif freq_hz >= 1e3:
        return f"{freq_hz / 1e3:.3f} kHz"
    else:
        return f"{freq_hz:.1f} Hz"


def print_results_table(points: list[SweepPoint], sys_clk_hz: int) -> None:
    """Print human-readable results table."""
    print("=" * 78)
    print("PIO Toggle Frequency Benchmark")
    print("=" * 78)
    print(f"Reported clock_get_hz(clk_sys): {sys_clk_hz} Hz "
          f"({sys_clk_hz / 1e6:.1f} MHz)")
    print()
    print(f"{'clkdiv':>8} | {'delay':>5} | {'expected':>14} | "
          f"{'rpi4_measured':>14} | {'ratio':>7} | {'notes':>12}")
    print("-" * 78)

    for p in points:
        expected_str = format_freq(p.expected_freq_hz)
        if p.rpi4_saturated:
            rpi4_str = "(saturated)"
            ratio_str = "—"
            notes = f"tr={p.rpi4_transition_ratio:.3f}"
        elif p.rpi4_freq_hz is not None:
            rpi4_str = format_freq(p.rpi4_freq_hz)
            ratio_str = f"{p.ratio:.4f}" if p.ratio else "—"
            notes = ""
        else:
            rpi4_str = "(no data)"
            ratio_str = "—"
            notes = ""

        print(f"{p.clkdiv:>8.0f} | {p.delay:>5} | {expected_str:>14} | "
              f"{rpi4_str:>14} | {ratio_str:>7} | {notes:>12}")

    print("=" * 78)

    # Verdict
    ratios = [p.ratio for p in points if p.ratio is not None]
    if ratios:
        avg_ratio = sum(ratios) / len(ratios)
        if abs(avg_ratio - 1.0) < 0.05:
            print(f"\nVerdict: PIO clock is 200 MHz (avg ratio = {avg_ratio:.4f}x)")
            print("The claim of 400 MHz execution is DISMISSED.")
        elif abs(avg_ratio - 2.0) < 0.1:
            print(f"\nVerdict: PIO clock is 400 MHz! (avg ratio = {avg_ratio:.4f}x)")
            print("The claim of 400 MHz execution is CONFIRMED.")
        else:
            print(f"\nVerdict: Inconclusive (avg ratio = {avg_ratio:.4f}x)")
            print("This does not match either 200 MHz or 400 MHz.")
    else:
        print("\nVerdict: No unsaturated measurements — cannot determine clock rate.")
        print("All measured frequencies exceeded RPi4 polling capability.")


def print_results_json(points: list[SweepPoint], sys_clk_hz: int) -> None:
    """Print JSON results."""
    data = {
        "benchmark": "rp1-pio-toggle-freq",
        "sys_clk_hz": sys_clk_hz,
        "sweep": [],
    }
    for p in points:
        entry = {
            "clkdiv": p.clkdiv,
            "delay": p.delay,
            "expected_freq_hz": p.expected_freq_hz,
            "rpi4": {
                "measured_freq_hz": p.rpi4_freq_hz,
                "edges_counted": p.rpi4_edges,
                "samples_taken": p.rpi4_samples,
                "transition_ratio": p.rpi4_transition_ratio,
                "saturated": p.rpi4_saturated,
            },
            "ratio": p.ratio,
        }
        data["sweep"].append(entry)

    ratios = [p.ratio for p in points if p.ratio is not None]
    if ratios:
        avg_ratio = sum(ratios) / len(ratios)
        data["avg_ratio"] = avg_ratio
        if abs(avg_ratio - 1.0) < 0.05:
            data["verdict"] = "200_mhz"
        elif abs(avg_ratio - 2.0) < 0.1:
            data["verdict"] = "400_mhz"
        else:
            data["verdict"] = "inconclusive"
    else:
        data["verdict"] = "no_data"

    print(json.dumps(data, indent=2))


# ─── Argument parsing ─────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="PIO Toggle Frequency Benchmark",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--clkdivs",
        default=",".join(str(d) for d in DEFAULT_CLKDIVS),
        help=f"Comma-separated clock dividers (default: {','.join(str(d) for d in DEFAULT_CLKDIVS)})",
    )
    parser.add_argument(
        "--delay", type=int, default=0,
        help="Instruction delay cycles, 0-31 (default: 0)",
    )
    parser.add_argument(
        "--pin", type=int, default=DEFAULT_TOGGLE_PIN,
        help=f"GPIO pin to toggle (default: {DEFAULT_TOGGLE_PIN})",
    )
    parser.add_argument(
        "--duration-ms", type=int, default=DEFAULT_DURATION_MS,
        help=f"Measurement duration per point in ms (default: {DEFAULT_DURATION_MS})",
    )
    parser.add_argument(
        "--rpi5-host", default=DEFAULT_RPI5_HOST,
        help=f"RPi5 hostname (default: {DEFAULT_RPI5_HOST})",
    )
    parser.add_argument(
        "--rpi4-host", default=DEFAULT_RPI4_HOST,
        help=f"RPi4 hostname (default: {DEFAULT_RPI4_HOST})",
    )
    parser.add_argument(
        "--remote-dir", default=DEFAULT_REMOTE_DIR,
        help=f"Remote project directory (default: {DEFAULT_REMOTE_DIR})",
    )
    parser.add_argument("--json", action="store_true", help="JSON output")
    parser.add_argument("--no-build", action="store_true", help="Skip building")
    parser.add_argument("--no-sync", action="store_true", help="Skip source sync")
    parser.add_argument(
        "--settle-secs", type=int, default=DEFAULT_SETTLE_SECS,
        help=f"Settle time after starting toggle (default: {DEFAULT_SETTLE_SECS})",
    )
    parser.add_argument(
        "--measurement-timeout", type=int, default=DEFAULT_MEASUREMENT_TIMEOUT,
        help=f"Timeout for RPi4 measurement (default: {DEFAULT_MEASUREMENT_TIMEOUT})",
    )
    return parser.parse_args()


# ─── Main ─────────────────────────────────────────────────────

def main() -> int:
    args = parse_args()

    # Parse clkdiv list
    try:
        clkdivs = [float(x.strip()) for x in args.clkdivs.split(",")]
    except ValueError:
        log(f"ERROR: invalid clkdivs: {args.clkdivs}")
        return 1

    log("=" * 60)
    log("PIO Toggle Frequency Benchmark")
    log("=" * 60)
    log()

    # Check SSH connectivity
    log("Checking SSH connectivity...")
    if not ssh_check(args.rpi5_host, "RPi5"):
        return 1
    if not ssh_check(args.rpi4_host, "RPi4"):
        return 1
    log()

    # Determine local directory (find toggle/ relative to this script)
    script_dir = Path(__file__).resolve().parent
    local_dir = script_dir.parent  # Parent of toggle/

    # Sync and build
    if not args.no_sync:
        if not sync_source(local_dir, args.rpi5_host, args.remote_dir, "RPi5"):
            return 1
        if not sync_source(local_dir, args.rpi4_host, args.remote_dir, "RPi4"):
            return 1
        log()

    if not args.no_build:
        if not deploy_and_build(args.rpi5_host, args.remote_dir, "rpi5", "RPi5"):
            return 1
        if not deploy_and_build(args.rpi4_host, args.remote_dir, "rpi4", "RPi4"):
            return 1
        log()

    # Initial cleanup
    cleanup_pins(args.rpi5_host, args.rpi4_host, args.pin)

    # Run sweep
    log(f"Running frequency sweep: clkdivs={clkdivs}, delay={args.delay}")
    log()

    points: list[SweepPoint] = []
    sys_clk_hz = 0

    for clkdiv in clkdivs:
        log(f"--- clkdiv={clkdiv}, delay={args.delay} ---")

        point = run_measurement(
            rpi5_host=args.rpi5_host,
            rpi4_host=args.rpi4_host,
            remote_dir=args.remote_dir,
            pin=args.pin,
            clkdiv=clkdiv,
            delay=args.delay,
            duration_ms=args.duration_ms,
            settle_secs=args.settle_secs,
            measurement_timeout=args.measurement_timeout,
        )

        if point.sys_clk_hz:
            sys_clk_hz = point.sys_clk_hz

        # Log immediate result
        if point.rpi4_saturated:
            log(f"  RPi4: SATURATED (tr={point.rpi4_transition_ratio:.3f})")
        elif point.rpi4_freq_hz is not None:
            log(f"  RPi4: {format_freq(point.rpi4_freq_hz)} "
                f"(ratio={point.ratio:.4f})" if point.ratio else "")
        else:
            log(f"  RPi4: no data")

        points.append(point)

        # Cleanup between runs
        cleanup_pins(args.rpi5_host, args.rpi4_host, args.pin)
        time.sleep(1)

        log()

    # Print final results
    if args.json:
        print_results_json(points, sys_clk_hz)
    else:
        print_results_table(points, sys_clk_hz)

    return 0


if __name__ == "__main__":
    sys.exit(main())
```

**Step 2: Commit**

```bash
git add toggle/run_toggle_benchmark.py
git commit -m "Add orchestrator for toggle frequency benchmark

Python script that coordinates RPi5 (PIO toggle), RPi4 (edge
counting), via SSH. Sweeps clock dividers from 256 down to 1,
measures frequency at each point, and produces a comparison table
with a verdict on the 200 MHz vs 400 MHz PIO clock question."
```

---

### Task 11: Full Integration Test

This is the real hardware test. Run the complete sweep.

**Step 1: Run the full benchmark**

```bash
cd .worktrees/toggle-benchmark
uv run toggle/run_toggle_benchmark.py --duration-ms 2000
```

Expected output: a table showing measured vs expected frequencies at each
clkdiv, with ratios near 1.0 (confirming 200 MHz) or near 2.0 (confirming
400 MHz).

**Step 2: Run with JSON output for archiving**

```bash
uv run toggle/run_toggle_benchmark.py --duration-ms 2000 --json > results.json
```

**Step 3: Run at clkdiv=1 only (max frequency test)**

```bash
uv run toggle/run_toggle_benchmark.py --clkdivs 1 --duration-ms 3000
```

At clkdiv=1, the RPi4 will likely report "saturated" (transition ratio ~0.5),
confirming the toggle frequency is above its ~5 MHz measurement limit.

**Step 4: Commit results**

If the benchmark runs successfully, commit a summary of the results.

---

### Task 12: Glasgow Frequency Measurement (if analyzer applet works)

This task is exploratory — depends on Glasgow analyzer capabilities discovered
in Task 9.

**Step 1: Determine Glasgow's max sample rate for a single pin**

```bash
ssh tim@rpi5-pmod.iot.welland.mithis.com \
    "export PATH=\$HOME/.local/bin:\$PATH && glasgow run analyzer --help"
```

**Step 2: Capture at various toggle frequencies**

Start with clkdiv=256 (~390 kHz) and work down to test Glasgow's limits.

**Step 3: Add Glasgow measurement to orchestrator**

Extend `run_toggle_benchmark.py` with Glasgow capture and VCD parsing.

**Step 4: Commit Glasgow integration**

---

### Task 13: Final Verification and Documentation

**Step 1: Run the full benchmark one final time**

```bash
uv run toggle/run_toggle_benchmark.py --duration-ms 5000
```

**Step 2: Capture results output**

Include the actual output in the commit message or a results file.

**Step 3: Final commit with results**

```bash
git add -A
git commit -m "Complete PIO toggle frequency benchmark with results

[Include key findings: measured PIO clock rate, verdict on 400 MHz claim]"
```
