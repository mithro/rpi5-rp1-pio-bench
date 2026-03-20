/* latency_gpioloop.c — RPi5 PIO latency benchmark
 *
 * Configures RP1 PIO state machines for latency testing across multiple
 * abstraction layers:
 *
 *   L0: PIO-only echo (hardware baseline, no CPU involvement)
 *   L1: PIO -> ioctl -> PIO (CPU reads RX FIFO, writes TX FIFO)
 *   L2: PIO -> DMA -> CPU poll -> DMA -> PIO (single-word DMA per iteration)
 *   L3: Batched DMA throughput (standalone, no external GPIO)
 *
 * Requires RPi5 with libpio-dev installed and root privileges (/dev/pio0).
 *
 * Build: see Makefile (make)
 * Run:   sudo ./latency_gpioloop [options]
 */

#define _GNU_SOURCE

#include <errno.h>
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
#include "common.h"

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

    /* Initialise state machine (does NOT enable yet). */
    pio_sm_init(pio, (uint)sm, offset, &c);

    /* Force output pin LOW before enabling, to prevent a stale HIGH
     * from being visible to the external observer before the PIO program
     * has executed its first SET instruction. */
    pio_sm_set_pins_with_mask(pio, (uint)sm, 0, 1u << (uint)output_pin);

    /* Now enable. */
    pio_sm_set_enabled(pio, (uint)sm, true);

    fprintf(stderr,
        "L0: PIO-only echo active (input=GPIO%d, output=GPIO%d)\n"
        "    Press Ctrl-C to stop.\n",
        input_pin, output_pin);

    while (g_running)
        pause();

    fprintf(stderr, "\nL0: Shutting down...\n");

    /* Clean up: disable SM, drive output LOW, set as input. */
    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_sm_set_pins_with_mask(pio, (uint)sm, 0, 1u << (uint)output_pin);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)output_pin, 1, false);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)input_pin, 1, false);
    pio_remove_program(pio, &gpio_echo_program, offset);
    pio_sm_unclaim(pio, (uint)sm);

    return 0;
}

/* ─── Shared setup for L1/L2 edge-echo state machines ────── */

/* Holds the two SMs and program offsets used by both L1 and L2.
 * Callers must enable the SMs themselves (L2 needs DMA config first). */
typedef struct {
    int sm_rx;          /* edge_detector state machine */
    int sm_tx;          /* output_driver state machine */
    uint offset_rx;     /* instruction memory offset for edge_detector */
    uint offset_tx;     /* instruction memory offset for output_driver */
} edge_echo_sms_t;

/* Claim two SMs, load edge_detector + output_driver programs, configure
 * pin mappings and shift registers, init both SMs, set pin directions,
 * and force output LOW.  Does NOT enable SMs (caller does that).
 * Returns 0 on success, -1 on failure (partial resources cleaned up). */
static int setup_edge_echo_sms(PIO pio, int input_pin, int output_pin,
                                edge_echo_sms_t *out)
{
    out->sm_rx = pio_claim_unused_sm(pio, true);
    if (out->sm_rx < 0) {
        fprintf(stderr, "ERROR: no free state machine for edge_detector\n");
        return -1;
    }

    out->sm_tx = pio_claim_unused_sm(pio, true);
    if (out->sm_tx < 0) {
        fprintf(stderr, "ERROR: no free state machine for output_driver\n");
        pio_sm_unclaim(pio, (uint)out->sm_rx);
        return -1;
    }

    out->offset_rx = pio_add_program(pio, &edge_detector_program);
    if (out->offset_rx == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load edge_detector program\n");
        pio_sm_unclaim(pio, (uint)out->sm_tx);
        pio_sm_unclaim(pio, (uint)out->sm_rx);
        return -1;
    }

    out->offset_tx = pio_add_program(pio, &output_driver_program);
    if (out->offset_tx == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load output_driver program\n");
        pio_remove_program(pio, &edge_detector_program, out->offset_rx);
        pio_sm_unclaim(pio, (uint)out->sm_tx);
        pio_sm_unclaim(pio, (uint)out->sm_rx);
        return -1;
    }

    /* Configure GPIOs for PIO function. */
    pio_gpio_init(pio, (uint)input_pin);
    pio_gpio_init(pio, (uint)output_pin);

    /* Configure SM_RX (edge_detector): autopush at 32 bits. */
    pio_sm_config c_rx = edge_detector_program_get_default_config(out->offset_rx);
    sm_config_set_in_pins(&c_rx, (uint)input_pin);
    sm_config_set_in_shift(&c_rx, false, true, 32);
    sm_config_set_clkdiv(&c_rx, 1.0f);

    /* Configure SM_TX (output_driver).
     * Uses 'set pins' (not 'out pins') because RP1 PIO 'out pins' does
     * not drive physical GPIO pads.  Autopull DISABLED — the PIO program
     * uses explicit 'pull block'. */
    pio_sm_config c_tx = output_driver_program_get_default_config(out->offset_tx);
    sm_config_set_out_pins(&c_tx, (uint)output_pin, 1);
    sm_config_set_set_pins(&c_tx, (uint)output_pin, 1);
    sm_config_set_out_shift(&c_tx, true, false, 32);
    sm_config_set_clkdiv(&c_tx, 1.0f);

    /* Initialise both state machines. */
    pio_sm_init(pio, (uint)out->sm_rx, out->offset_rx, &c_rx);
    pio_sm_init(pio, (uint)out->sm_tx, out->offset_tx, &c_tx);

    /* Set pin directions AFTER init. */
    pio_sm_set_consecutive_pindirs(pio, (uint)out->sm_rx, (uint)input_pin, 1, false);
    pio_sm_set_consecutive_pindirs(pio, (uint)out->sm_tx, (uint)output_pin, 1, true);

    /* Force output pin LOW before enabling. */
    pio_sm_set_pins_with_mask(pio, (uint)out->sm_tx, 0, 1u << (uint)output_pin);

    return 0;
}

/* Disable SMs, drive output LOW, restore pins as inputs, remove programs,
 * and unclaim state machines.  Safe to call even if SMs were never enabled. */
static void cleanup_edge_echo_sms(PIO pio, int input_pin, int output_pin,
                                   const edge_echo_sms_t *sms)
{
    pio_sm_set_enabled(pio, (uint)sms->sm_tx, false);
    pio_sm_set_enabled(pio, (uint)sms->sm_rx, false);
    pio_sm_set_pins_with_mask(pio, (uint)sms->sm_tx, 0, 1u << (uint)output_pin);
    pio_sm_set_consecutive_pindirs(pio, (uint)sms->sm_tx, (uint)output_pin, 1, false);
    pio_sm_set_consecutive_pindirs(pio, (uint)sms->sm_rx, (uint)input_pin, 1, false);
    pio_remove_program(pio, &output_driver_program, sms->offset_tx);
    pio_remove_program(pio, &edge_detector_program, sms->offset_rx);
    pio_sm_unclaim(pio, (uint)sms->sm_tx);
    pio_sm_unclaim(pio, (uint)sms->sm_rx);
}

/* ─── L1: PIO -> ioctl -> PIO ──────────────────────────────── */

static int run_l1(PIO pio, int input_pin, int output_pin,
                  size_t iterations, size_t warmup,
                  int rt_priority, int cpu_affinity, int json_output)
{
    int ret = 0;

    /* Set up edge-detector (RX) and output-driver (TX) state machines. */
    edge_echo_sms_t sms;
    if (setup_edge_echo_sms(pio, input_pin, output_pin, &sms) < 0)
        return 1;

    int sm_rx = sms.sm_rx;
    int sm_tx = sms.sm_tx;

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

    /* Warmup: read edges and echo, discarding timing.
     * Use non-blocking reads with a poll loop so that SIGINT/SIGTERM
     * can interrupt us (blocking ioctl is not signal-interruptible).
     *
     * After each TX FIFO write, read back the FIFO level.  This PCIe
     * read acts as a barrier, ensuring the posted write has reached the
     * RP1 PIO block before we poll the RX FIFO again.  Without this,
     * a tight spin on pio_sm_is_rx_fifo_empty() can outrace the posted
     * write, preventing the output_driver from updating the pin. */
    for (size_t i = 0; i < warmup && g_running; i++) {
        while (pio_sm_is_rx_fifo_empty(pio, (uint)sm_rx)) {
            if (!g_running) goto cleanup;
        }
        uint32_t edge_val = pio_sm_get(pio, (uint)sm_rx);
        pio_sm_put_blocking(pio, (uint)sm_tx, edge_val);
        (void)pio_sm_get_tx_fifo_level(pio, (uint)sm_tx);
    }

    if (!g_running) { ret = 1; goto cleanup; }

    fprintf(stderr, "    Warmup complete, measuring...\n");

    /* Measured loop: time each get+put pair.
     * For measurement accuracy, we use a tight poll loop without
     * signal checks — the measurement should be fast enough. */
    for (size_t i = 0; i < iterations && g_running; i++) {
        while (pio_sm_is_rx_fifo_empty(pio, (uint)sm_rx)) {
            if (!g_running) goto cleanup;
        }
        uint64_t t0 = get_time_ns();
        uint32_t edge_val = pio_sm_get(pio, (uint)sm_rx);
        pio_sm_put_blocking(pio, (uint)sm_tx, edge_val);
        uint64_t t1 = get_time_ns();
        latencies[i] = (double)(t1 - t0);
    }

    if (!g_running) {
        fprintf(stderr, "    Interrupted, %zu iterations completed.\n", iterations);
        ret = 1;
        goto cleanup;
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
    report.timing_label = "Ioctl processing time";

    bench_compute_stats(latencies, iterations, scratch, &report.latency_ns);

    if (json_output)
        latency_print_json(stdout, &report);
    else
        latency_print_report(stdout, &report);

cleanup:
    free(latencies);
    free(scratch);

    cleanup_edge_echo_sms(pio, input_pin, output_pin, &sms);

    return ret;
}

/* ─── L2: PIO -> DMA -> PIO ────────────────────────────────── */

static int run_l2(PIO pio, int input_pin, int output_pin,
                  size_t iterations, size_t warmup,
                  int rt_priority, int cpu_affinity, int json_output)
{
    int ret = 0;
    double *latencies = NULL;
    double *scratch = NULL;

    /* Set up edge-detector (RX) and output-driver (TX) state machines. */
    edge_echo_sms_t sms;
    if (setup_edge_echo_sms(pio, input_pin, output_pin, &sms) < 0)
        return 1;

    int sm_rx = sms.sm_rx;
    int sm_tx = sms.sm_tx;

    /* ── Configure DMA for single-word transfers ──────────── */

    int rc;
    rc = pio_sm_config_xfer(pio, (uint)sm_rx, PIO_DIR_FROM_SM, 4, 1);
    if (rc < 0) {
        fprintf(stderr, "ERROR: pio_sm_config_xfer(RX) failed: %d (%s)\n",
                rc, strerror(errno));
        ret = 1;
        goto cleanup;
    }
    rc = pio_sm_config_xfer(pio, (uint)sm_tx, PIO_DIR_TO_SM, 4, 1);
    if (rc < 0) {
        fprintf(stderr, "ERROR: pio_sm_config_xfer(TX) failed: %d (%s)\n",
                rc, strerror(errno));
        ret = 1;
        goto cleanup;
    }

    /* Enable SMs after DMA config. */
    pio_sm_set_enabled(pio, (uint)sm_rx, true);
    pio_sm_set_enabled(pio, (uint)sm_tx, true);

    fprintf(stderr,
        "L2: PIO->DMA->PIO (input=GPIO%d, output=GPIO%d)\n"
        "    Iterations: %zu (warmup: %zu)\n",
        input_pin, output_pin, iterations, warmup);

    /* Allocate measurement buffers. */
    latencies = (double *)malloc(iterations * sizeof(double));
    scratch = (double *)malloc(iterations * sizeof(double));
    if (!latencies || !scratch) {
        fprintf(stderr, "ERROR: failed to allocate measurement buffers\n");
        ret = 1;
        goto cleanup;
    }

    /* Apply RT optimisations. */
    apply_rt_priority(rt_priority);
    apply_cpu_affinity(cpu_affinity);
    lock_memory();

    /* ── Wait for first stimulus edge from RPi4 ─────────────
     * pio_sm_xfer_data() is a blocking DMA call with a kernel-imposed
     * timeout.  If we call it before the RPi4 has started sending
     * stimulus edges, the DMA will timeout (ETIMEDOUT).  Use non-blocking
     * ioctl polling (like L1) to wait for the first edge, then switch
     * to DMA for the actual data transfers. */

    fprintf(stderr, "    Waiting for first stimulus edge...\n");
    while (pio_sm_is_rx_fifo_empty(pio, (uint)sm_rx)) {
        if (!g_running) goto cleanup;
    }

    /* ── Warmup loop (blocking DMA per word) ──────────────── */

    for (size_t i = 0; i < warmup && g_running; i++) {
        uint32_t rx_word;
        rc = pio_sm_xfer_data(pio, (uint)sm_rx, PIO_DIR_FROM_SM, 4, &rx_word);
        if (rc < 0) {
            fprintf(stderr, "ERROR: xfer_data(RX) failed at warmup[%zu]: %d (%s)\n",
                    i, rc, strerror(errno));
            ret = 1;
            goto cleanup;
        }
        rc = pio_sm_xfer_data(pio, (uint)sm_tx, PIO_DIR_TO_SM, 4, &rx_word);
        if (rc < 0) {
            fprintf(stderr, "ERROR: xfer_data(TX) failed at warmup[%zu]: %d (%s)\n",
                    i, rc, strerror(errno));
            ret = 1;
            goto cleanup;
        }
    }

    if (!g_running) { ret = 1; goto cleanup; }

    fprintf(stderr, "    Warmup complete, measuring...\n");

    /* ── Measurement loop ─────────────────────────────────── *
     * Unlike L1/L3 which pre-poll until data is available then timestamp,
     * L2 timestamps before the blocking DMA receive call.  This means L2
     * latency includes the DMA-internal wait for FIFO data (DREQ-paced)
     * plus the full DMA setup/teardown/ioctl overhead for both directions.
     * This is the correct quantity for L2: total CPU cost of one DMA-based
     * echo round-trip, including the blocking wait. */

    for (size_t i = 0; i < iterations && g_running; i++) {
        uint32_t rx_word;
        uint64_t t0 = get_time_ns();
        rc = pio_sm_xfer_data(pio, (uint)sm_rx, PIO_DIR_FROM_SM, 4, &rx_word);
        if (rc < 0) {
            fprintf(stderr, "ERROR: xfer_data(RX) failed at iter[%zu]: %d (%s)\n",
                    i, rc, strerror(errno));
            ret = 1;
            goto cleanup;
        }
        rc = pio_sm_xfer_data(pio, (uint)sm_tx, PIO_DIR_TO_SM, 4, &rx_word);
        if (rc < 0) {
            fprintf(stderr, "ERROR: xfer_data(TX) failed at iter[%zu]: %d (%s)\n",
                    i, rc, strerror(errno));
            ret = 1;
            goto cleanup;
        }
        uint64_t t1 = get_time_ns();
        latencies[i] = (double)(t1 - t0);
    }

    if (!g_running) {
        fprintf(stderr, "    Interrupted, %zu iterations completed.\n", iterations);
        ret = 1;
        goto cleanup;
    }

    fprintf(stderr, "    Measurement complete.\n");

    /* ── Report ───────────────────────────────────────────── */

    latency_report_t report;
    memset(&report, 0, sizeof(report));
    report.test_layer = TEST_L2;
    report.stimulus_pin = input_pin;
    report.response_pin = output_pin;
    report.num_iterations = iterations;
    report.num_warmup = warmup;
    report.rt_priority = rt_priority;
    report.cpu_affinity = cpu_affinity;
    report.timing_label = "DMA processing time";

    bench_compute_stats(latencies, iterations, scratch, &report.latency_ns);

    if (json_output)
        latency_print_json(stdout, &report);
    else
        latency_print_report(stdout, &report);

cleanup:
    free(latencies);
    free(scratch);

    cleanup_edge_echo_sms(pio, input_pin, output_pin, &sms);

    return ret;
}

/* ─── L3: Batched DMA throughput (RPi5 standalone) ─────────── */

/* DMA batch size in bytes.  Each measured iteration reads this many bytes
 * from the RX FIFO via a single pio_sm_xfer_data() call.  1024 words
 * (4096 bytes) is large enough to amortise DMA setup overhead while
 * keeping per-iteration time reasonable (~150 µs at 27 MB/s). */
#define L3_BATCH_SIZE  4096

/* Self-generating PIO program: pushes zeros to RX FIFO via autopush.
 * Single instruction: `in null, 32` — shifts 32 zero bits into ISR,
 * autopush transfers ISR to RX FIFO every cycle.
 * At 200 MHz with one instruction per word, this can generate up to
 * 800 MB/s of data (far exceeding DMA throughput). */
static const uint16_t datagen_insn[] = { 0x4060 };  /* in null, 32 */
static const struct pio_program datagen_program = {
    .instructions = datagen_insn,
    .length = 1,
    .origin = -1,
};

static int run_l3(PIO pio, int input_pin, int output_pin,
                  size_t iterations, size_t warmup,
                  int rt_priority, int cpu_affinity, int json_output)
{
    (void)input_pin;
    (void)output_pin;

    int ret = 0;

    /* Claim a state machine. */
    int sm = pio_claim_unused_sm(pio, true);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machine for datagen\n");
        return 1;
    }

    /* Load the data generator program. */
    uint offset = pio_add_program(pio, &datagen_program);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load datagen program\n");
        pio_sm_unclaim(pio, (uint)sm);
        return 1;
    }

    /* Configure SM: autopush at 32 bits, no pins needed, full speed. */
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + datagen_program.length - 1);
    sm_config_set_in_shift(&c, false, true, 32);  /* left shift, autopush, 32-bit */
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio, (uint)sm, offset, &c);

    /* Configure DMA for batched RX transfers. */
    int rc3 = pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_FROM_SM, L3_BATCH_SIZE, 1);
    if (rc3 < 0) {
        fprintf(stderr, "ERROR: pio_sm_config_xfer(RX) failed: %d\n", rc3);
        pio_remove_program(pio, &datagen_program, offset);
        pio_sm_unclaim(pio, (uint)sm);
        return 1;
    }

    /* Enable SM. */
    pio_sm_set_enabled(pio, (uint)sm, true);

    fprintf(stderr,
        "L3: Batched DMA throughput (SM=%d, batch=%d bytes)\n"
        "    Iterations: %zu (warmup: %zu)\n",
        sm, L3_BATCH_SIZE, iterations, warmup);

    /* Allocate measurement and data buffers. */
    double *latencies = (double *)malloc(iterations * sizeof(double));
    double *scratch = (double *)malloc(iterations * sizeof(double));
    uint8_t *rx_buf = (uint8_t *)malloc(L3_BATCH_SIZE);
    if (!latencies || !scratch || !rx_buf) {
        fprintf(stderr, "ERROR: failed to allocate buffers\n");
        ret = 1;
        goto cleanup;
    }

    /* Apply RT optimisations. */
    apply_rt_priority(rt_priority);
    apply_cpu_affinity(cpu_affinity);
    lock_memory();

    /* ── Warmup: run DMA reads to prime the path ──────────── */

    for (size_t i = 0; i < warmup && g_running; i++) {
        int rc = pio_sm_xfer_data(pio, (uint)sm, PIO_DIR_FROM_SM,
                                   L3_BATCH_SIZE, rx_buf);
        if (rc < 0) {
            fprintf(stderr, "ERROR: xfer_data(RX) failed at warmup[%zu]: %d (%s)\n",
                    i, rc, strerror(errno));
            ret = 1;
            goto cleanup;
        }
    }

    if (!g_running) { ret = 1; goto cleanup; }

    fprintf(stderr, "    Warmup complete, measuring...\n");

    /* ── Measurement loop ─────────────────────────────────── *
     * Each iteration performs one blocking DMA read of L3_BATCH_SIZE
     * bytes from the PIO RX FIFO.  The PIO data generator fills the
     * FIFO continuously, so DMA throughput is the only bottleneck. */

    for (size_t i = 0; i < iterations && g_running; i++) {
        uint64_t t0 = get_time_ns();
        int rc = pio_sm_xfer_data(pio, (uint)sm, PIO_DIR_FROM_SM,
                                   L3_BATCH_SIZE, rx_buf);
        uint64_t t1 = get_time_ns();
        if (rc < 0) {
            fprintf(stderr, "ERROR: xfer_data(RX) failed at iter[%zu]: %d (%s)\n",
                    i, rc, strerror(errno));
            ret = 1;
            goto cleanup;
        }
        latencies[i] = (double)(t1 - t0);
    }

    if (!g_running) {
        fprintf(stderr, "    Interrupted, %zu iterations completed.\n", iterations);
        ret = 1;
        goto cleanup;
    }

    fprintf(stderr, "    Measurement complete.\n");

    /* ── Report ───────────────────────────────────────────── */

    latency_report_t report;
    memset(&report, 0, sizeof(report));
    report.test_layer = TEST_L3;
    report.stimulus_pin = -1;   /* no GPIO pins used */
    report.response_pin = -1;
    report.num_iterations = iterations;
    report.num_warmup = warmup;
    report.rt_priority = rt_priority;
    report.cpu_affinity = cpu_affinity;
    report.timing_label = "DMA batch read time";

    bench_compute_stats(latencies, iterations, scratch, &report.latency_ns);

    if (json_output)
        latency_print_json(stdout, &report);
    else
        latency_print_report(stdout, &report);

cleanup:
    free(latencies);
    free(scratch);
    free(rx_buf);

    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_remove_program(pio, &datagen_program, offset);
    pio_sm_unclaim(pio, (uint)sm);

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
        "  L2  PIO -> DMA -> CPU poll -> DMA -> PIO (single-word DMA)\n"
        "  L3  Batched DMA throughput (4KB reads, standalone)\n",
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

    /* ─── Install signal handler for clean shutdown ─────────── */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

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
        ret = run_l2(pio, input_pin, output_pin,
                     (size_t)iterations, (size_t)warmup,
                     rt_priority, cpu_affinity, json_output);
        break;

    case TEST_L3:
        ret = run_l3(pio, input_pin, output_pin,
                     (size_t)iterations, (size_t)warmup,
                     rt_priority, cpu_affinity, json_output);
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
