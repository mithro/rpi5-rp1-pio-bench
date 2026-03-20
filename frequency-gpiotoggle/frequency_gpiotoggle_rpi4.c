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

#include "common.h"

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
