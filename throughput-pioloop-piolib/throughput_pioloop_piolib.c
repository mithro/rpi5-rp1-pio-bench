/* benchmark_main.c — RP1 PIO internal loopback benchmark
 *
 * Measures round-trip throughput: Host CPU → TX FIFO → PIO (bitwise NOT)
 * → RX FIFO → Host CPU. Supports DMA and blocking transfer modes.
 * Requires RPi5 with piolib installed.
 *
 * Build: gcc -O2 -I/usr/include/piolib -o pio_loopback benchmark_main.c \
 *        benchmark_stats.c benchmark_verify.c benchmark_format.c \
 *        -lpio -lpthread -lm
 *
 * Run:   sudo ./pio_loopback [options]
 */

#define _GNU_SOURCE  /* for clock_gettime, CLOCK_MONOTONIC */

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "piolib.h"
#include "pio_loopback.pio.h"

#include "benchmark_format.h"
#include "benchmark_stats.h"
#include "benchmark_verify.h"

#define DEFAULT_TRANSFER_SIZE  (256 * 1024)  /* 256 KB per iteration */
#define DEFAULT_ITERATIONS     100
#define DEFAULT_WARMUP         3
#define DEFAULT_PATTERN        BENCH_PATTERN_SEQUENTIAL
#define DEFAULT_THRESHOLD_MBPS 10.0

/* ─── Transfer mode ────────────────────────────────────────── */

typedef enum {
    BENCH_MODE_DMA = 0,
    BENCH_MODE_BLOCKING,
} bench_mode_t;

#define DEFAULT_DMA_THRESHOLD 8
#define DEFAULT_DMA_PRIORITY  2

/* Build the dmactrl register value from threshold and priority.
 *
 * dmactrl register layout (from MichaelBell's reverse engineering):
 *   Bit 31:    DREQ_EN — enable DMA request signal
 *   Bit 30:    DREQ_STATUS — current DREQ status (read-only, set for safety)
 *   Bits 29:12: reserved
 *   Bits 11:7:  PRIORITY — DMA priority (lower = faster)
 *   Bits 6:5:   reserved
 *   Bits 4:0:   THRESHOLD — FIFO threshold (0-31, must match burst size) */
static uint32_t build_dmactrl(int threshold, int priority)
{
    return 0xC0000000u
         | ((uint32_t)(priority & 0x1F) << 7)
         | (uint32_t)(threshold & 0x1F);
}

/* ─── Timing helper ─────────────────────────────────────────── */

static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ─── Pthread DMA transfer wrapper ──────────────────────────── */

typedef struct {
    PIO pio;
    uint sm;
    enum pio_xfer_dir dir;
    size_t size;
    void *buf;
    int ret;
} xfer_args_t;

static void *xfer_thread(void *arg)
{
    xfer_args_t *a = (xfer_args_t *)arg;
    a->ret = pio_sm_xfer_data(a->pio, a->sm, a->dir, a->size, a->buf);
    return NULL;
}

/* ─── Transfer helpers ─────────────────────────────────────── */

static int dma_transfer(PIO pio, uint sm, void *tx_buf, void *rx_buf,
                        size_t size)
{
    pthread_t tx_tid, rx_tid;
    xfer_args_t tx_args = {pio, sm, PIO_DIR_TO_SM, size, tx_buf, 0};
    xfer_args_t rx_args = {pio, sm, PIO_DIR_FROM_SM, size, rx_buf, 0};

    pthread_create(&tx_tid, NULL, xfer_thread, &tx_args);
    pthread_create(&rx_tid, NULL, xfer_thread, &rx_args);
    pthread_join(tx_tid, NULL);
    pthread_join(rx_tid, NULL);

    if (tx_args.ret < 0 || rx_args.ret < 0)
        return -1;
    return 0;
}

static int blocking_transfer(PIO pio, uint sm,
                              const uint32_t *tx_buf, uint32_t *rx_buf,
                              size_t word_count)
{
    for (size_t i = 0; i < word_count; i++) {
        pio_sm_put_blocking(pio, sm, tx_buf[i]);
        rx_buf[i] = pio_sm_get_blocking(pio, sm);
    }
    return 0;
}

/* ─── Usage ─────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --size=BYTES       Transfer size per iteration (default %d)\n"
        "  --iterations=N     Number of measured iterations (default %d)\n"
        "  --warmup=N         Warmup iterations (default %d)\n"
        "  --pattern=ID       Test pattern: 0=seq, 1=ones, 2=alt, 3=random (default %d)\n"
        "  --threshold=MB/S   Pass/fail threshold (default %.1f)\n"
        "  --json             Output JSON instead of human-readable table\n"
        "  --no-verify        Skip data verification (measure raw throughput)\n"
        "  --mode=MODE        Transfer mode: dma or blocking (default dma)\n"
        "  --dma-threshold=N  FIFO threshold 1-8, DMA mode only (default %d)\n"
        "  --dma-priority=N   DMA priority 0-31, DMA mode only (default %d)\n"
        "  --help             Show this help\n",
        prog,
        DEFAULT_TRANSFER_SIZE,
        DEFAULT_ITERATIONS,
        DEFAULT_WARMUP,
        DEFAULT_PATTERN,
        DEFAULT_THRESHOLD_MBPS,
        DEFAULT_DMA_THRESHOLD,
        DEFAULT_DMA_PRIORITY);
}

/* ─── Main ──────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int ret = 0;

    /* Default parameters. */
    size_t transfer_size = DEFAULT_TRANSFER_SIZE;
    int iterations = DEFAULT_ITERATIONS;
    int warmup = DEFAULT_WARMUP;
    int pattern = DEFAULT_PATTERN;
    double threshold = DEFAULT_THRESHOLD_MBPS;
    int json_output = 0;
    int no_verify = 0;
    bench_mode_t mode = BENCH_MODE_DMA;
    int dma_threshold = DEFAULT_DMA_THRESHOLD;
    int dma_priority = DEFAULT_DMA_PRIORITY;

    /* Parse command-line options. */
    static struct option long_options[] = {
        {"size",          required_argument, NULL, 's'},
        {"iterations",    required_argument, NULL, 'i'},
        {"warmup",        required_argument, NULL, 'w'},
        {"pattern",       required_argument, NULL, 'p'},
        {"threshold",     required_argument, NULL, 't'},
        {"json",          no_argument,       NULL, 'j'},
        {"no-verify",     no_argument,       NULL, 'n'},
        {"mode",          required_argument, NULL, 'm'},
        {"dma-threshold", required_argument, NULL, 'T'},
        {"dma-priority",  required_argument, NULL, 'P'},
        {"help",          no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:i:w:p:t:jnm:T:P:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 's': transfer_size = (size_t)atol(optarg); break;
        case 'i': iterations = atoi(optarg); break;
        case 'w': warmup = atoi(optarg); break;
        case 'p': pattern = atoi(optarg); break;
        case 't': threshold = atof(optarg); break;
        case 'j': json_output = 1; break;
        case 'n': no_verify = 1; break;
        case 'm':
            if (strcmp(optarg, "dma") == 0)
                mode = BENCH_MODE_DMA;
            else if (strcmp(optarg, "blocking") == 0)
                mode = BENCH_MODE_BLOCKING;
            else {
                fprintf(stderr, "ERROR: unknown mode '%s' (use 'dma' or 'blocking')\n",
                        optarg);
                return 1;
            }
            break;
        case 'T': dma_threshold = atoi(optarg); break;
        case 'P': dma_priority = atoi(optarg); break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* Transfer size must be a multiple of 4 bytes (32-bit words). */
    if (transfer_size < 4 || transfer_size % 4 != 0) {
        fprintf(stderr, "ERROR: transfer size must be >= 4 and a multiple of 4\n");
        return 1;
    }
    if (iterations < 1) {
        fprintf(stderr, "ERROR: iterations must be >= 1\n");
        return 1;
    }

    /* Validate DMA parameters. */
    if (dma_threshold < 1 || dma_threshold > 8) {
        fprintf(stderr, "ERROR: --dma-threshold must be 1-8 (FIFO depth)\n");
        return 1;
    }
    if (dma_priority < 0 || dma_priority > 31) {
        fprintf(stderr, "ERROR: --dma-priority must be 0-31 (5-bit field)\n");
        return 1;
    }

    /* Warn if DMA options used with blocking mode. */
    if (mode == BENCH_MODE_BLOCKING) {
        if (dma_threshold != DEFAULT_DMA_THRESHOLD)
            fprintf(stderr, "WARNING: --dma-threshold ignored in blocking mode\n");
        if (dma_priority != DEFAULT_DMA_PRIORITY)
            fprintf(stderr, "WARNING: --dma-priority ignored in blocking mode\n");
    }

    size_t word_count = transfer_size / 4;

    /* Blocking mode time estimate. */
    if (mode == BENCH_MODE_BLOCKING) {
        /* ~0.2 MB/s = 200000 bytes/s, 2 mailbox round-trips per word. */
        double est_sec = (double)(transfer_size * (size_t)(warmup + iterations))
                         / 200000.0;
        if (est_sec > 10.0)
            fprintf(stderr, "NOTE: blocking mode estimated time: %.0f s "
                    "(use --size to reduce)\n", est_sec);
    }

    /* ─── PIO setup ─────────────────────────────────────────── */

    PIO pio = pio0;
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr, "ERROR: failed to open PIO (is /dev/pio0 present?)\n");
        return 1;
    }

    int sm = pio_claim_unused_sm(pio, true);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machines\n");
        pio_close(pio);
        return 1;
    }

    uint offset = pio_add_program(pio, &loopback_program);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load PIO program\n");
        pio_sm_unclaim(pio, (uint)sm);
        pio_close(pio);
        return 1;
    }

    /* Configure state machine. */
    pio_sm_config c = loopback_program_get_default_config(offset);
    sm_config_set_out_shift(&c, false, true, 32);   /* left shift, autopull, 32-bit */
    sm_config_set_in_shift(&c, false, true, 32);    /* left shift, autopush, 32-bit */
    sm_config_set_clkdiv(&c, 1.0f);                 /* full 200 MHz */
    pio_sm_init(pio, (uint)sm, offset, &c);

    /* Configure DMA (only in DMA mode). */
    if (mode == BENCH_MODE_DMA) {
        ret = pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_TO_SM, transfer_size, 1);
        if (ret < 0) {
            fprintf(stderr, "ERROR: failed to configure TX DMA (%d)\n", ret);
            goto cleanup;
        }
        ret = pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_FROM_SM, transfer_size, 1);
        if (ret < 0) {
            fprintf(stderr, "ERROR: failed to configure RX DMA (%d)\n", ret);
            goto cleanup;
        }

        /* Set FIFO threshold and DMA priority via dmactrl register.
         * Default 0xC0000108: DREQ_EN | threshold=8 | priority=2.
         * Threshold must match burst size to avoid corruption (PR #7190). */
        uint32_t dmactrl = build_dmactrl(dma_threshold, dma_priority);
        pio_sm_set_dmactrl(pio, (uint)sm, true,  dmactrl);  /* TX */
        pio_sm_set_dmactrl(pio, (uint)sm, false, dmactrl);  /* RX */
    }

    /* Enable state machine. */
    pio_sm_set_enabled(pio, (uint)sm, true);

    /* ─── Allocate buffers ──────────────────────────────────── */

    uint32_t *tx_buf = (uint32_t *)aligned_alloc(64, transfer_size);
    uint32_t *rx_buf = (uint32_t *)aligned_alloc(64, transfer_size);
    double *throughputs = (double *)malloc((size_t)iterations * sizeof(double));

    if (!tx_buf || !rx_buf || !throughputs) {
        fprintf(stderr, "ERROR: failed to allocate buffers\n");
        ret = 1;
        goto cleanup_bufs;
    }

    /* ─── Warmup iterations ─────────────────────────────────── */

    for (int i = 0; i < warmup; i++) {
        bench_fill_pattern(tx_buf, word_count, pattern, (uint32_t)(i + 1));
        memset(rx_buf, 0, transfer_size);

        if (mode == BENCH_MODE_BLOCKING) {
            blocking_transfer(pio, (uint)sm, tx_buf, rx_buf, word_count);
        } else {
            if (dma_transfer(pio, (uint)sm, tx_buf, rx_buf, transfer_size) < 0) {
                fprintf(stderr, "ERROR: DMA transfer failed during warmup %d\n", i);
                ret = 1;
                goto cleanup_bufs;
            }
        }
    }

    /* ─── Measured iterations ───────────────────────────────── */

    uint32_t total_errors = 0;
    double total_start = get_time_sec();

    for (int i = 0; i < iterations; i++) {
        bench_fill_pattern(tx_buf, word_count, pattern, (uint32_t)(warmup + i + 1));
        memset(rx_buf, 0, transfer_size);

        double t0 = get_time_sec();

        if (mode == BENCH_MODE_BLOCKING) {
            blocking_transfer(pio, (uint)sm, tx_buf, rx_buf, word_count);
        } else {
            if (dma_transfer(pio, (uint)sm, tx_buf, rx_buf, transfer_size) < 0) {
                fprintf(stderr, "ERROR: DMA transfer failed at iteration %d\n", i);
                ret = 1;
                goto cleanup_bufs;
            }
        }

        double t1 = get_time_sec();
        double elapsed = t1 - t0;

        throughputs[i] = ((double)transfer_size / (1024.0 * 1024.0)) / elapsed;

        if (!no_verify) {
            total_errors += bench_verify_not(tx_buf, rx_buf, word_count,
                                             NULL, NULL, NULL);
        }
    }

    double total_end = get_time_sec();
    double total_elapsed = total_end - total_start;

    /* ─── Compute and print report ──────────────────────────── */

    double *scratch = (double *)malloc((size_t)iterations * sizeof(double));
    if (!scratch) {
        fprintf(stderr, "ERROR: failed to allocate scratch buffer\n");
        ret = 1;
        goto cleanup_bufs;
    }

    bench_report_t report;
    bench_build_report(throughputs, (size_t)iterations, scratch,
                       transfer_size, total_elapsed, total_errors, &report);

    /* Set transfer mode fields. */
    static char mode_label[128];
    if (mode == BENCH_MODE_BLOCKING) {
        report.transfer_mode = "blocking put/get (no DMA)";
        report.transfer_mode_id = "blocking";
        report.dma_threshold = -1;
        report.dma_priority = -1;
        report.throughput_ceiling_mbps = 0.4;
    } else {
        snprintf(mode_label, sizeof(mode_label),
                 "DMA (threshold=%d, priority=%d)", dma_threshold, dma_priority);
        report.transfer_mode = mode_label;
        report.transfer_mode_id = "dma";
        report.dma_threshold = dma_threshold;
        report.dma_priority = dma_priority;
        report.throughput_ceiling_mbps = 27.0;
    }

    if (json_output)
        bench_print_json(stdout, &report);
    else
        bench_print_report(stdout, &report);

    ret = bench_print_verdict(stdout, &report, threshold);

    free(scratch);

cleanup_bufs:
    free(tx_buf);
    free(rx_buf);
    free(throughputs);

cleanup:
    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_remove_program(pio, &loopback_program, offset);
    pio_sm_unclaim(pio, (uint)sm);
    pio_close(pio);

    return ret;
}
