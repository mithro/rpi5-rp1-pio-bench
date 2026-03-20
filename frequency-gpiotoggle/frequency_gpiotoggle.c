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
#include "common.h"

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

    /* Install SIGINT handler for clean shutdown. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* Open PIO instance (must happen before clock_get_hz which uses
     * pio_get_current() internally). */
    PIO pio = pio0;
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr,
            "ERROR: failed to open PIO device.\n"
            "  Is /dev/pio0 present? Is libpio-dev installed?\n"
            "  This program requires RPi5 with RP1 PIO support.\n");
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

    /* Claim a state machine. */
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machines\n");
        return 1;
    }

    /* Build toggle program with desired delay. */
    uint16_t patched_insns[2];
    build_toggle_program(patched_insns, delay);

    pio_program_t prog = {
        .instructions = patched_insns,
        .length = 2,
        .origin = -1,
        .pio_version = 0,
    };

    uint offset = pio_add_program(pio, &prog);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load toggle program\n");
        pio_sm_unclaim(pio, (uint)sm);
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
