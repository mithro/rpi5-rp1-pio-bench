/* latency_rpi5.c — RPi5 PIO latency benchmark
 *
 * Configures RP1 PIO state machines for latency testing across multiple
 * abstraction layers:
 *
 *   L0: PIO-only echo (hardware baseline, no CPU involvement)
 *   L1: PIO -> ioctl -> PIO (CPU reads RX FIFO, writes TX FIFO)
 *   L2: PIO -> DMA -> CPU poll -> DMA -> PIO (stub)
 *   L3: PIO -> mmap FIFO -> PIO (stub)
 *
 * Requires RPi5 with libpio-dev installed and root privileges (/dev/pio0).
 *
 * Build: see Makefile (make rpi5)
 * Run:   sudo ./latency_rpi5 [options]
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

/* ─── Signal handling for L0 mode ──────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ─── RT optimisations ─────────────────────────────────────── */

static void apply_rt_priority(int priority)
{
    if (priority <= 0)
        return;

    struct sched_param param;
    param.sched_priority = priority;
    if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
        perror("WARNING: sched_setscheduler(SCHED_FIFO)");
    } else {
        fprintf(stderr, "INFO: RT priority set to SCHED_FIFO %d\n", priority);
    }
}

static void apply_cpu_affinity(int cpu)
{
    if (cpu < 0)
        return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
        perror("WARNING: sched_setaffinity");
    } else {
        fprintf(stderr, "INFO: CPU affinity set to core %d\n", cpu);
    }
}

static void lock_memory(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
        perror("WARNING: mlockall");
    }
}

/* ─── L0: PIO-only echo ───────────────────────────────────── */

static int run_l0(PIO pio, int input_pin, int output_pin)
{
    /* Claim a state machine. */
    int sm = pio_claim_unused_sm(pio, true);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machines\n");
        return 1;
    }

    /* Load the gpio_echo program. */
    uint offset = pio_add_program(pio, &gpio_echo_program);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load gpio_echo program\n");
        pio_sm_unclaim(pio, (uint)sm);
        return 1;
    }

    /* Configure GPIOs for PIO function. */
    pio_gpio_init(pio, (uint)input_pin);
    pio_gpio_init(pio, (uint)output_pin);

    /* Set pin directions: input pin as input, output pin as output. */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)input_pin, 1, false);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)output_pin, 1, true);

    /* Configure the state machine. */
    pio_sm_config c = gpio_echo_program_get_default_config(offset);
    sm_config_set_in_pins(&c, (uint)input_pin);
    sm_config_set_set_pins(&c, (uint)output_pin, 1);
    sm_config_set_clkdiv(&c, 1.0f);

    /* Initialise and enable. */
    pio_sm_init(pio, (uint)sm, offset, &c);
    pio_sm_set_enabled(pio, (uint)sm, true);

    fprintf(stderr,
        "L0: PIO-only echo active (input=GPIO%d, output=GPIO%d)\n"
        "    Press Ctrl-C to stop.\n",
        input_pin, output_pin);

    /* Install signal handler and wait. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    while (g_running)
        pause();

    fprintf(stderr, "\nL0: Shutting down...\n");

    /* Clean up. */
    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_remove_program(pio, &gpio_echo_program, offset);
    pio_sm_unclaim(pio, (uint)sm);

    return 0;
}

/* ─── L1: PIO -> ioctl -> PIO ──────────────────────────────── */

static int run_l1(PIO pio, int input_pin, int output_pin,
                  size_t iterations, size_t warmup,
                  int rt_priority, int cpu_affinity, int json_output)
{
    int ret = 0;

    /* Claim two state machines: one for RX (edge detector), one for TX (output driver). */
    int sm_rx = pio_claim_unused_sm(pio, true);
    if (sm_rx < 0) {
        fprintf(stderr, "ERROR: no free state machine for edge_detector\n");
        return 1;
    }

    int sm_tx = pio_claim_unused_sm(pio, true);
    if (sm_tx < 0) {
        fprintf(stderr, "ERROR: no free state machine for output_driver\n");
        pio_sm_unclaim(pio, (uint)sm_rx);
        return 1;
    }

    /* Load edge_detector program. */
    uint offset_rx = pio_add_program(pio, &edge_detector_program);
    if (offset_rx == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load edge_detector program\n");
        pio_sm_unclaim(pio, (uint)sm_tx);
        pio_sm_unclaim(pio, (uint)sm_rx);
        return 1;
    }

    /* Load output_driver program. */
    uint offset_tx = pio_add_program(pio, &output_driver_program);
    if (offset_tx == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load output_driver program\n");
        pio_remove_program(pio, &edge_detector_program, offset_rx);
        pio_sm_unclaim(pio, (uint)sm_tx);
        pio_sm_unclaim(pio, (uint)sm_rx);
        return 1;
    }

    /* Configure GPIOs for PIO function. */
    pio_gpio_init(pio, (uint)input_pin);
    pio_gpio_init(pio, (uint)output_pin);

    /* Set pin directions. */
    pio_sm_set_consecutive_pindirs(pio, (uint)sm_rx, (uint)input_pin, 1, false);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm_tx, (uint)output_pin, 1, true);

    /* Configure SM_RX (edge_detector). */
    pio_sm_config c_rx = edge_detector_program_get_default_config(offset_rx);
    sm_config_set_in_pins(&c_rx, (uint)input_pin);
    sm_config_set_in_shift(&c_rx, false, true, 32);  /* left shift, autopush, 32-bit */
    sm_config_set_clkdiv(&c_rx, 1.0f);

    /* Configure SM_TX (output_driver). */
    pio_sm_config c_tx = output_driver_program_get_default_config(offset_tx);
    sm_config_set_out_pins(&c_tx, (uint)output_pin, 1);
    sm_config_set_out_shift(&c_tx, true, true, 32);  /* right shift, autopull, 32-bit */
    sm_config_set_clkdiv(&c_tx, 1.0f);

    /* Initialise both state machines. */
    pio_sm_init(pio, (uint)sm_rx, offset_rx, &c_rx);
    pio_sm_init(pio, (uint)sm_tx, offset_tx, &c_tx);

    /* Enable both state machines. */
    pio_sm_set_enabled(pio, (uint)sm_rx, true);
    pio_sm_set_enabled(pio, (uint)sm_tx, true);

    fprintf(stderr,
        "L1: PIO->ioctl->PIO (input=GPIO%d, output=GPIO%d)\n"
        "    Iterations: %zu (warmup: %zu)\n",
        input_pin, output_pin, iterations, warmup);

    /* Allocate measurement buffers. */
    double *latencies = (double *)malloc(iterations * sizeof(double));
    double *scratch = (double *)malloc(iterations * sizeof(double));
    if (!latencies || !scratch) {
        fprintf(stderr, "ERROR: failed to allocate measurement buffers\n");
        ret = 1;
        goto cleanup;
    }

    /* Apply RT optimisations before the measurement loop. */
    apply_rt_priority(rt_priority);
    apply_cpu_affinity(cpu_affinity);
    lock_memory();

    /* Warmup: read edges and echo, discarding timing. */
    for (size_t i = 0; i < warmup; i++) {
        uint32_t edge_val = pio_sm_get_blocking(pio, (uint)sm_rx);
        pio_sm_put_blocking(pio, (uint)sm_tx, edge_val);
    }

    fprintf(stderr, "    Warmup complete, measuring...\n");

    /* Measured loop: time each get+put pair. */
    for (size_t i = 0; i < iterations; i++) {
        uint64_t t0 = get_time_ns();
        uint32_t edge_val = pio_sm_get_blocking(pio, (uint)sm_rx);
        pio_sm_put_blocking(pio, (uint)sm_tx, edge_val);
        uint64_t t1 = get_time_ns();
        latencies[i] = (double)(t1 - t0);
    }

    fprintf(stderr, "    Measurement complete.\n");

    /* Compute statistics and print report. */
    latency_report_t report;
    memset(&report, 0, sizeof(report));
    report.test_layer = TEST_L1;
    report.stimulus_pin = input_pin;
    report.response_pin = output_pin;
    report.num_iterations = iterations;
    report.num_warmup = warmup;
    report.rt_priority = rt_priority;
    report.cpu_affinity = cpu_affinity;

    bench_compute_stats(latencies, iterations, scratch, &report.latency_ns);

    if (json_output)
        latency_print_json(stdout, &report);
    else
        latency_print_report(stdout, &report);

cleanup:
    free(latencies);
    free(scratch);

    /* Disable and clean up state machines. */
    pio_sm_set_enabled(pio, (uint)sm_tx, false);
    pio_sm_set_enabled(pio, (uint)sm_rx, false);
    pio_remove_program(pio, &output_driver_program, offset_tx);
    pio_remove_program(pio, &edge_detector_program, offset_rx);
    pio_sm_unclaim(pio, (uint)sm_tx);
    pio_sm_unclaim(pio, (uint)sm_rx);

    return ret;
}

/* ─── Usage ────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "RPi5 PIO latency benchmark — measures round-trip GPIO latency\n"
        "through various RP1 PIO abstraction layers.\n"
        "\n"
        "Options:\n"
        "  --test=MODE        Test mode: L0, L1, L2, L3 (default: L0)\n"
        "  --input-pin=N      Input GPIO pin number (default: %d)\n"
        "  --output-pin=N     Output GPIO pin number (default: %d)\n"
        "  --iterations=N     Number of measured iterations (default: %d)\n"
        "  --warmup=N         Warmup iterations before measurement (default: %d)\n"
        "  --rt-priority=N    Set SCHED_FIFO real-time priority (1-99)\n"
        "  --cpu=N            Pin process to CPU core N\n"
        "  --json             Output results as JSON\n"
        "  --help             Show this help message\n"
        "\n"
        "Test modes:\n"
        "  L0  PIO-only echo (hardware baseline, runs until Ctrl-C)\n"
        "  L1  PIO -> ioctl -> PIO (CPU reads RX FIFO, writes TX FIFO)\n"
        "  L2  PIO -> DMA -> CPU poll -> DMA -> PIO (not yet implemented)\n"
        "  L3  PIO -> mmap FIFO -> PIO (not yet implemented)\n",
        prog,
        DEFAULT_STIMULUS_PIN,
        DEFAULT_RESPONSE_PIN,
        DEFAULT_ITERATIONS,
        DEFAULT_WARMUP);
}

/* ─── Command-line option parsing ──────────────────────────── */

static int parse_test_mode(const char *arg)
{
    if (strcasecmp(arg, "L0") == 0) return TEST_L0;
    if (strcasecmp(arg, "L1") == 0) return TEST_L1;
    if (strcasecmp(arg, "L2") == 0) return TEST_L2;
    if (strcasecmp(arg, "L3") == 0) return TEST_L3;
    return -1;
}

/* ─── Main ─────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Default parameters. */
    int test_mode    = TEST_L0;
    int input_pin    = DEFAULT_STIMULUS_PIN;
    int output_pin   = DEFAULT_RESPONSE_PIN;
    int iterations   = DEFAULT_ITERATIONS;
    int warmup       = DEFAULT_WARMUP;
    int rt_priority  = 0;
    int cpu_affinity = -1;
    int json_output  = 0;

    /* Parse command-line options. */
    static struct option long_options[] = {
        {"test",        required_argument, NULL, 't'},
        {"input-pin",   required_argument, NULL, 'I'},
        {"output-pin",  required_argument, NULL, 'O'},
        {"iterations",  required_argument, NULL, 'i'},
        {"warmup",      required_argument, NULL, 'w'},
        {"rt-priority", required_argument, NULL, 'r'},
        {"cpu",         required_argument, NULL, 'c'},
        {"json",        no_argument,       NULL, 'j'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:I:O:i:w:r:c:jh",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 't': {
            int mode = parse_test_mode(optarg);
            if (mode < 0) {
                fprintf(stderr, "ERROR: unknown test mode '%s' "
                        "(use L0, L1, L2, or L3)\n", optarg);
                return 1;
            }
            test_mode = mode;
            break;
        }
        case 'I': input_pin = atoi(optarg); break;
        case 'O': output_pin = atoi(optarg); break;
        case 'i': iterations = atoi(optarg); break;
        case 'w': warmup = atoi(optarg); break;
        case 'r': rt_priority = atoi(optarg); break;
        case 'c': cpu_affinity = atoi(optarg); break;
        case 'j': json_output = 1; break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Validate parameters. */
    if (iterations < 1) {
        fprintf(stderr, "ERROR: iterations must be >= 1\n");
        return 1;
    }
    if (warmup < 0) {
        fprintf(stderr, "ERROR: warmup must be >= 0\n");
        return 1;
    }
    if (input_pin == output_pin) {
        fprintf(stderr, "ERROR: input and output pins must be different\n");
        return 1;
    }
    if (rt_priority < 0 || rt_priority > 99) {
        fprintf(stderr, "ERROR: rt-priority must be 0-99\n");
        return 1;
    }

    /* ─── Open PIO device ──────────────────────────────────── */

    PIO pio = pio0;
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr,
            "ERROR: failed to open PIO device.\n"
            "  Is /dev/pio0 present? Is libpio-dev installed?\n"
            "  This program requires RPi5 with RP1 PIO support.\n");
        return 1;
    }

    /* ─── Dispatch to test mode ────────────────────────────── */

    int ret;

    switch (test_mode) {
    case TEST_L0:
        ret = run_l0(pio, input_pin, output_pin);
        break;

    case TEST_L1:
        ret = run_l1(pio, input_pin, output_pin,
                     (size_t)iterations, (size_t)warmup,
                     rt_priority, cpu_affinity, json_output);
        break;

    case TEST_L2:
        fprintf(stderr,
            "ERROR: L2 (PIO -> DMA -> poll -> PIO) is not yet implemented.\n");
        ret = 1;
        break;

    case TEST_L3:
        fprintf(stderr,
            "ERROR: L3 (PIO -> mmap FIFO -> PIO) is not yet implemented.\n");
        ret = 1;
        break;

    default:
        fprintf(stderr, "ERROR: unknown test mode %d\n", test_mode);
        ret = 1;
        break;
    }

    /* ─── Close PIO device ─────────────────────────────────── */

    pio_close(pio);

    return ret;
}
