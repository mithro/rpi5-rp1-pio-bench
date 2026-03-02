/* latency_rpi4.c — RPi4 GPIO latency measurement program
 *
 * Generates stimulus pulses on one GPIO pin and measures round-trip time
 * by busy-polling a response pin. Uses memory-mapped BCM2711 GPIO registers
 * via /dev/gpiomem (no root required for GPIO access).
 *
 * This program runs on the RPi4 as the "external observer" in the latency
 * benchmark. It drives a stimulus pin HIGH, waits for the RPi5 PIO program
 * to echo the signal back on the response pin, and records the round-trip
 * latency in nanoseconds.
 *
 * Build:  make rpi4   (or see Makefile for manual gcc invocation)
 * Run:    ./latency_rpi4 [options]
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

/* ─── BCM2711 GPIO register offsets ──────────────────────── */

#define GPIO_MEM_SIZE         0x100    /* Enough for all registers we need */

#define GPFSEL0               0x00     /* Function select (3 bits/pin, 10 pins/reg) */
#define GPSET0                0x1C     /* Output set: write 1 = HIGH */
#define GPCLR0                0x28     /* Output clear: write 1 = LOW */
#define GPLEV0                0x34     /* Pin level: read current state */
#define GPIO_PUP_PDN_CNTRL0  0xE4     /* BCM2711 pull-up/down control */

/* GPIO function select values (3-bit fields in GPFSEL registers) */
#define FSEL_INPUT   0
#define FSEL_OUTPUT  1

/* Pull-up/down control values (2-bit fields) */
#define PUD_OFF      0
#define PUD_DOWN     1
#define PUD_UP       2

/* Timeout for warmup busy-poll (100 ms in nanoseconds) */
#define POLL_TIMEOUT_NS       100000000ULL

/* ─── Globals ────────────────────────────────────────────── */

static volatile uint32_t *gpio_base;
static int gpio_fd = -1;
static int stimulus_pin = DEFAULT_STIMULUS_PIN;
static int response_pin = DEFAULT_RESPONSE_PIN;
static volatile sig_atomic_t running = 1;

/* ─── Signal handler ─────────────────────────────────────── */

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ─── GPIO register helpers ──────────────────────────────── */

static inline uint32_t gpio_read(unsigned offset)
{
    return gpio_base[offset / 4];
}

static inline void gpio_write(unsigned offset, uint32_t val)
{
    gpio_base[offset / 4] = val;
}

/* Set the function select for a GPIO pin.
 * GPFSEL registers: 10 pins per register, 3 bits per pin.
 * Register n covers pins (n*10) to (n*10 + 9). */
static void gpio_set_fsel(int pin, int fsel)
{
    unsigned reg = GPFSEL0 + (unsigned)(pin / 10) * 4;
    unsigned shift = (unsigned)(pin % 10) * 3;

    uint32_t val = gpio_read(reg);
    val &= ~(7u << shift);           /* Clear the 3-bit field */
    val |= ((unsigned)fsel & 7u) << shift;
    gpio_write(reg, val);
}

/* Set pull-up/down for a GPIO pin (BCM2711 style).
 * GPIO_PUP_PDN_CNTRL registers: 16 pins per register, 2 bits per pin. */
static void gpio_set_pull(int pin, int pull)
{
    unsigned reg = GPIO_PUP_PDN_CNTRL0 + (unsigned)(pin / 16) * 4;
    unsigned shift = (unsigned)(pin % 16) * 2;

    uint32_t val = gpio_read(reg);
    val &= ~(3u << shift);           /* Clear the 2-bit field */
    val |= ((unsigned)pull & 3u) << shift;
    gpio_write(reg, val);
}

static inline void gpio_set_high(int pin)
{
    gpio_write(GPSET0, 1u << (unsigned)pin);
}

static inline void gpio_set_low(int pin)
{
    gpio_write(GPCLR0, 1u << (unsigned)pin);
}

static inline int gpio_read_level(int pin)
{
    return (int)((gpio_read(GPLEV0) >> (unsigned)pin) & 1u);
}

/* ─── GPIO mmap setup ────────────────────────────────────── */

static int gpio_mmap_init(void)
{
    gpio_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (gpio_fd < 0) {
        perror("ERROR: cannot open /dev/gpiomem");
        fprintf(stderr, "Hint: ensure you are on an RPi4 and your user is "
                        "in the 'gpio' group.\n");
        return -1;
    }

    void *map = mmap(NULL, GPIO_MEM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, gpio_fd, 0);
    if (map == MAP_FAILED) {
        perror("ERROR: mmap failed");
        close(gpio_fd);
        gpio_fd = -1;
        return -1;
    }

    gpio_base = (volatile uint32_t *)map;
    return 0;
}

static void gpio_mmap_cleanup(void)
{
    if (gpio_base) {
        munmap((void *)gpio_base, GPIO_MEM_SIZE);
        gpio_base = NULL;
    }
    if (gpio_fd >= 0) {
        close(gpio_fd);
        gpio_fd = -1;
    }
}

/* ─── GPIO pin setup and teardown ────────────────────────── */

static void gpio_setup_pins(void)
{
    /* Stimulus pin: output, initially LOW */
    gpio_set_fsel(stimulus_pin, FSEL_OUTPUT);
    gpio_set_low(stimulus_pin);

    /* Response pin: input with pull-down */
    gpio_set_fsel(response_pin, FSEL_INPUT);
    gpio_set_pull(response_pin, PUD_DOWN);
}

static void gpio_restore_pins(void)
{
    /* Drive stimulus LOW before restoring to input */
    gpio_set_low(stimulus_pin);

    /* Restore both pins to input mode (safe default) */
    gpio_set_fsel(stimulus_pin, FSEL_INPUT);
    gpio_set_fsel(response_pin, FSEL_INPUT);

    /* Remove pulls */
    gpio_set_pull(stimulus_pin, PUD_OFF);
    gpio_set_pull(response_pin, PUD_OFF);
}

/* ─── RT optimisations ───────────────────────────────────── */

static int apply_rt_priority(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;

    if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
        perror("WARNING: sched_setscheduler(SCHED_FIFO) failed");
        fprintf(stderr, "Hint: run with sudo or set CAP_SYS_NICE\n");
        return -1;
    }
    return 0;
}

static int apply_cpu_affinity(int cpu)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
        perror("WARNING: sched_setaffinity failed");
        return -1;
    }
    return 0;
}

/* ─── Usage ──────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "RPi4 GPIO latency measurement for PIO latency benchmark.\n"
        "Drives a stimulus pin and measures round-trip time via response pin.\n"
        "\n"
        "Options:\n"
        "  --stimulus-pin=N   GPIO pin for stimulus output (default %d)\n"
        "  --response-pin=N   GPIO pin for response input  (default %d)\n"
        "  --iterations=N     Number of measured iterations (default %d)\n"
        "  --warmup=N         Warmup iterations with timeout (default %d)\n"
        "  --rt-priority=N    SCHED_FIFO priority 1-99 (default: off)\n"
        "  --cpu=N            CPU affinity (default: no affinity)\n"
        "  --json             Output JSON instead of human-readable\n"
        "  --help             Show this help\n",
        prog,
        DEFAULT_STIMULUS_PIN,
        DEFAULT_RESPONSE_PIN,
        DEFAULT_ITERATIONS,
        DEFAULT_WARMUP);
}

/* ─── Main ───────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Default parameters */
    int iterations = DEFAULT_ITERATIONS;
    int warmup = DEFAULT_WARMUP;
    int rt_priority = 0;
    int cpu_affinity = -1;
    int json_output = 0;

    /* Parse command-line options */
    static struct option long_options[] = {
        {"stimulus-pin", required_argument, NULL, 'S'},
        {"response-pin", required_argument, NULL, 'R'},
        {"iterations",   required_argument, NULL, 'i'},
        {"warmup",       required_argument, NULL, 'w'},
        {"rt-priority",  required_argument, NULL, 'r'},
        {"cpu",          required_argument, NULL, 'c'},
        {"json",         no_argument,       NULL, 'j'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "S:R:i:w:r:c:jh",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'S': stimulus_pin = atoi(optarg); break;
        case 'R': response_pin = atoi(optarg); break;
        case 'i': iterations = atoi(optarg); break;
        case 'w': warmup = atoi(optarg); break;
        case 'r': rt_priority = atoi(optarg); break;
        case 'c': cpu_affinity = atoi(optarg); break;
        case 'j': json_output = 1; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* Validate parameters */
    if (stimulus_pin < 0 || stimulus_pin > 27 ||
        response_pin < 0 || response_pin > 27) {
        fprintf(stderr, "ERROR: GPIO pins must be 0-27\n");
        return 1;
    }
    if (stimulus_pin == response_pin) {
        fprintf(stderr, "ERROR: stimulus and response pins must differ\n");
        return 1;
    }
    if (iterations < 1) {
        fprintf(stderr, "ERROR: iterations must be >= 1\n");
        return 1;
    }
    if (warmup < 0) {
        fprintf(stderr, "ERROR: warmup must be >= 0\n");
        return 1;
    }
    if (rt_priority < 0 || rt_priority > 99) {
        fprintf(stderr, "ERROR: rt-priority must be 0-99\n");
        return 1;
    }

    /* Install signal handler for clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Open and mmap GPIO registers */
    if (gpio_mmap_init() < 0)
        return 1;

    /* Configure GPIO pins */
    gpio_setup_pins();

    /* Apply RT optimisations if requested */
    if (rt_priority > 0)
        apply_rt_priority(rt_priority);
    if (cpu_affinity >= 0)
        apply_cpu_affinity(cpu_affinity);

    /* Lock all current and future memory to prevent page faults */
    if (rt_priority > 0) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
            perror("WARNING: mlockall failed");
    }

    /* Allocate measurement buffers */
    double *latencies = (double *)malloc((size_t)iterations * sizeof(double));
    if (!latencies) {
        fprintf(stderr, "ERROR: failed to allocate latency buffer\n");
        gpio_restore_pins();
        gpio_mmap_cleanup();
        return 1;
    }

    int ret = 0;

    /* ─── Pre-test sanity check ─────────────────────────────── */

    /* Verify response pin is LOW before starting. If it's HIGH already,
     * the RPi5 PIO output may be in a stale state from a previous run. */
    if (gpio_read_level(response_pin)) {
        fprintf(stderr,
            "WARNING: response pin GPIO%d is already HIGH before test.\n"
            "  This usually means the RPi5 PIO output pin was not cleaned\n"
            "  up from a previous run. Waiting up to 2s for it to go LOW...\n",
            response_pin);
        uint64_t t_wait = get_time_ns();
        while (gpio_read_level(response_pin)) {
            if ((get_time_ns() - t_wait) > 2000000000ULL) {
                fprintf(stderr,
                    "ERROR: response pin GPIO%d still HIGH after 2s.\n"
                    "  Ensure RPi5 PIO program is configured correctly.\n",
                    response_pin);
                ret = 1;
                goto cleanup;
            }
        }
        fprintf(stderr, "  Response pin went LOW, proceeding.\n");
    }

    /* ─── Warmup iterations (with timeout) ─────────────────── */

    fprintf(stderr, "Running %d warmup iterations...\n", warmup);

    for (int i = 0; i < warmup && running; i++) {
        uint64_t t_start, t_now;

        /* Drive stimulus HIGH and wait for response HIGH */
        gpio_set_high(stimulus_pin);

        t_start = get_time_ns();
        while (running) {
            if (gpio_read_level(response_pin))
                break;
            t_now = get_time_ns();
            if ((t_now - t_start) > POLL_TIMEOUT_NS) {
                fprintf(stderr, "ERROR: warmup %d timed out waiting for "
                        "response HIGH (>100 ms)\n", i);
                fprintf(stderr, "Hint: is the RPi5 running the echo program "
                        "and wired correctly?\n");
                ret = 1;
                goto cleanup;
            }
        }

        /* Drive stimulus LOW and wait for response LOW */
        gpio_set_low(stimulus_pin);

        t_start = get_time_ns();
        while (running) {
            if (!gpio_read_level(response_pin))
                break;
            t_now = get_time_ns();
            if ((t_now - t_start) > POLL_TIMEOUT_NS) {
                fprintf(stderr, "ERROR: warmup %d timed out waiting for "
                        "response LOW (>100 ms)\n", i);
                ret = 1;
                goto cleanup;
            }
        }
    }

    if (!running) {
        fprintf(stderr, "\nInterrupted during warmup.\n");
        ret = 1;
        goto cleanup;
    }

    fprintf(stderr, "Warmup complete. Running %d measured iterations...\n",
            iterations);

    /* ─── Measured iterations (tight busy-poll, no timeout) ── */

    for (int i = 0; i < iterations && running; i++) {
        uint64_t t_start, t_end;

        /* Rising edge measurement:
         * Drive stimulus HIGH, busy-poll for response HIGH, record time. */
        t_start = get_time_ns();
        gpio_set_high(stimulus_pin);

        while (!gpio_read_level(response_pin)) {
            /* Tight busy-poll — no timeout check for accuracy */
        }
        t_end = get_time_ns();

        latencies[i] = (double)(t_end - t_start);

        /* Falling edge: drive stimulus LOW, wait for response LOW.
         * We don't time this — only the rising edge matters. */
        gpio_set_low(stimulus_pin);

        while (gpio_read_level(response_pin)) {
            /* Tight busy-poll */
        }
    }

    if (!running) {
        fprintf(stderr, "\nInterrupted during measurement.\n");
        ret = 1;
        goto cleanup;
    }

    /* ─── Compute and print results ──────────────────────── */

    double *scratch = (double *)malloc((size_t)iterations * sizeof(double));
    if (!scratch) {
        fprintf(stderr, "ERROR: failed to allocate scratch buffer\n");
        ret = 1;
        goto cleanup;
    }

    latency_report_t report;
    memset(&report, 0, sizeof(report));

    bench_compute_stats(latencies, (size_t)iterations, scratch,
                        &report.latency_ns);

    report.test_layer = TEST_L0;
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

    free(scratch);

cleanup:
    /* Ensure stimulus is LOW before restoring */
    gpio_set_low(stimulus_pin);

    gpio_restore_pins();
    gpio_mmap_cleanup();
    free(latencies);

    return ret;
}
