/* gpio_loopback.c — RP1 PIO GPIO loopback benchmark
 *
 * Measures bit-serial throughput over a physical GPIO link.
 * A single PIO SM serialises each TX word bit-by-bit out a GPIO pin
 * (using set pins — RP1 'out pins' does NOT drive physical pads),
 * reads the pin back via the GPIO input synchroniser, and assembles
 * the received bits into RX words.
 *
 * Uses 1-bit autopull/autopush: each `out x,1` consumes a full DMA word
 * (only MSB used) and each `in pins,1` produces a full DMA word (bit in
 * LSB). This gives a DMA word rate of 25 MW/s (8 cycles per bit at
 * 200 MHz), above the RP1 DMA rate minimum of ~25 MW/s.
 *
 * Each source word is expanded to 32 DMA words for TX, and 32 RX DMA
 * words are compressed back to 1 source word for verification.
 *
 * DMACTRL: TX threshold=1 (avoids autopull/DMA FIFO collision),
 *          RX threshold=4 (ensures DMA drains RX FIFO fast enough).
 *
 * Hardware: GPIO5 (output) looped back to GPIO5 or GPIO4 (input)
 * Theoretical throughput: 200 MHz / 8 / 32 = 3.125 MB/s source data
 *
 * Build: make  (on RPi5)
 * Run:   sudo ./throughput_gpioloop_piolib [options]
 */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "piolib.h"
#include "gpio_loopback.pio.h"

#include "benchmark_cli.h"
#include "benchmark_format.h"
#include "benchmark_stats.h"
#include "benchmark_verify.h"

/* ─── Constants ────────────────────────────────────────────── */

#define DEFAULT_SOURCE_SIZE    8192      /* 8 KB source data per iteration */
#define DEFAULT_ITERATIONS     100
#define DEFAULT_WARMUP         3
#define DEFAULT_PATTERN        BENCH_PATTERN_SEQUENTIAL
#define DEFAULT_THRESHOLD_MBPS 1.0       /* ~50% of actual ~1.5 MB/s */
#define DEFAULT_OUTPUT_PIN     5
#define DEFAULT_INPUT_PIN      5         /* same pin = single-pin loopback */

/* PIO timing: 8 cycles per bit, 32 bits per source word */
#define CYCLES_PER_BIT         8
#define PIO_CLOCK_MHZ          200
#define THROUGHPUT_CEILING_MBPS 3.125    /* 200 / 8 / 32 * 4 */

/* Skip 1 RX DMA word (garbage from uninitialized OSR at startup) */
#define RX_SKIP                1

/* DMACTRL values: TX threshold=1 avoids autopull/FIFO collision,
 * RX threshold=4 ensures DMA drains fast enough.
 * Bit 31 = DREQ_EN, bits 11:7 = default TREQ, bits 4:0 = threshold. */
#define TX_DMACTRL             0x80000101u   /* threshold=1 */
#define RX_DMACTRL             0x80000104u   /* threshold=4 */

/* ─── Timing helper ────────────────────────────────────────── */

static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ─── Data expansion / compression ─────────────────────────── */

/* Expand: each source bit → MSB (bit 31) of a 32-bit DMA word.
 * MSB-first ordering. */
static void expand_tx(const uint32_t *src, uint32_t *dst, size_t src_words)
{
    for (size_t i = 0; i < src_words; i++) {
        uint32_t word = src[i];
        for (int bit = 31; bit >= 0; bit--)
            *dst++ = ((word >> bit) & 1) ? 0x80000000u : 0;
    }
}

/* Compress: extract LSB (bit 0) from each of 32 consecutive DMA words
 * to reconstruct one source word. MSB-first ordering. */
static void compress_rx(const uint32_t *src, uint32_t *dst, size_t dst_words)
{
    for (size_t i = 0; i < dst_words; i++) {
        uint32_t word = 0;
        for (int bit = 31; bit >= 0; bit--) {
            if (*src++ & 1)
                word |= (1u << bit);
        }
        dst[i] = word;
    }
}

/* ─── Pthread DMA transfer ─────────────────────────────────── */

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
    if (a->ret < 0) {
        fprintf(stderr, "ERROR: xfer_data(%s, size=%zu) returned %d, errno=%d\n",
                a->dir == PIO_DIR_TO_SM ? "TX" : "RX", a->size, a->ret, errno);
    }
    return NULL;
}

/* DMA transfer with DMA-before-SM sequencing.
 * Start DMA threads, wait for them to be submitted to kernel,
 * then enable SM. This prevents alignment jitter.
 * Returns 0 on success, -1 on DMA failure.
 * If elapsed_out is non-NULL, stores the PIO transfer time (SM enable
 * to DMA completion) — excludes setup/reset overhead. */
static int dma_transfer(PIO pio, uint sm, uint offset, const pio_sm_config *cfg,
                        int output_pin,
                        void *tx_buf, void *rx_buf, size_t dma_size,
                        double *elapsed_out)
{
    /* Reset SM: disable, clear FIFOs, restart, re-init with full config */
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_init(pio, sm, offset, cfg);
    pio_sm_set_pins_with_mask(pio, sm, 0, 1u << output_pin);

    /* Configure DMA channels */
    pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, dma_size, 1);
    pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, dma_size, 1);
    pio_sm_set_dmactrl(pio, sm, true, TX_DMACTRL);
    pio_sm_set_dmactrl(pio, sm, false, RX_DMACTRL);

    /* Start DMA before enabling SM — critical for deterministic alignment */
    pthread_t tx_tid, rx_tid;
    xfer_args_t tx_args = {pio, sm, PIO_DIR_TO_SM, dma_size, tx_buf, 0};
    xfer_args_t rx_args = {pio, sm, PIO_DIR_FROM_SM, dma_size, rx_buf, 0};

    pthread_create(&rx_tid, NULL, xfer_thread, &rx_args);
    pthread_create(&tx_tid, NULL, xfer_thread, &tx_args);
    usleep(2000);  /* ensure both DMA requests reach kernel */

    /* Time the actual PIO transfer (SM enable → DMA complete) */
    double t0 = get_time_sec();
    pio_sm_set_enabled(pio, sm, true);

    pthread_join(tx_tid, NULL);
    pthread_join(rx_tid, NULL);
    double t1 = get_time_sec();

    pio_sm_set_enabled(pio, sm, false);

    if (elapsed_out)
        *elapsed_out = t1 - t0;

    if (tx_args.ret < 0 || rx_args.ret < 0)
        return -1;
    return 0;
}

/* ─── Usage ────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "GPIO loopback benchmark: serialise data bit-by-bit out a pin,\n"
        "read back via the GPIO input synchroniser, verify integrity.\n"
        "Default: single-pin loopback (same pin for output and input).\n"
        "\n"
        "Benchmark-specific options:\n"
        "  --size=BYTES       Source data size per iteration (default %d)\n"
        "  --pattern=ID       Test pattern: 0=seq, 1=ones, 2=alt, 3=random (default %d)\n"
        "  --threshold=MB/S   Pass/fail threshold (default %.1f)\n"
        "  --output-pin=N     GPIO pin for output (default %d)\n"
        "  --input-pin=N      GPIO pin for input (default %d)\n",
        prog,
        DEFAULT_SOURCE_SIZE,
        DEFAULT_PATTERN,
        DEFAULT_THRESHOLD_MBPS,
        DEFAULT_OUTPUT_PIN,
        DEFAULT_INPUT_PIN);
    benchmark_cli_print_common_help();
}

/* ─── Main ─────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int ret = 0;

    /* Parse common CLI flags (--iterations, --warmup, --json, etc.) */
    benchmark_config_t cfg = benchmark_cli_parse(argc, argv);

    /* Map common flags to local variables */
    int iterations = cfg.iterations;
    int warmup = cfg.warmup;
    int json_output = cfg.json_output;
    int no_verify = cfg.no_verify;

    /* Default benchmark-specific parameters */
    size_t source_size = DEFAULT_SOURCE_SIZE;
    int pattern = DEFAULT_PATTERN;
    double threshold = DEFAULT_THRESHOLD_MBPS;
    int output_pin = DEFAULT_OUTPUT_PIN;
    int input_pin = DEFAULT_INPUT_PIN;

    /* Parse benchmark-specific options from remaining args */
    static struct option long_options[] = {
        {"size",       required_argument, NULL, 's'},
        {"pattern",    required_argument, NULL, 'p'},
        {"threshold",  required_argument, NULL, 't'},
        {"output-pin", required_argument, NULL, 'o'},
        {"input-pin",  required_argument, NULL, 'I'},
        {NULL, 0, NULL, 0}
    };

    optind = 1;
    int opt;
    while ((opt = getopt_long(cfg.argc_remaining, cfg.argv_remaining,
                              "s:p:t:o:I:", long_options, NULL)) != -1) {
        switch (opt) {
        case 's': source_size = (size_t)atol(optarg); break;
        case 'p': pattern = atoi(optarg); break;
        case 't': threshold = atof(optarg); break;
        case 'o': output_pin = atoi(optarg); break;
        case 'I': input_pin = atoi(optarg); break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cfg.help_requested) {
        print_usage(argv[0]);
        return 0;
    }

    /* Validate */
    if (source_size < 4 || source_size % 4 != 0) {
        fprintf(stderr, "ERROR: source size must be >= 4 and a multiple of 4\n");
        return 1;
    }
    if (iterations < 1) {
        fprintf(stderr, "ERROR: iterations must be >= 1\n");
        return 1;
    }
    if (output_pin < 0 || output_pin > 27 || input_pin < 0 || input_pin > 27) {
        fprintf(stderr, "ERROR: pin numbers must be 0-27\n");
        return 1;
    }

    size_t src_words = source_size / 4;

    /* DMA buffer sizing:
     *   TX: src_words * 32 expanded words + 1 trailing zero
     *   RX: RX_SKIP + src_words * 32 words + 1 trailing
     * Use the larger of the two for both (they must be equal for DMA). */
    size_t expanded_words = src_words * 32;
    size_t dma_words = RX_SKIP + expanded_words + 1;
    size_t dma_size = dma_words * 4;
    size_t alloc_size = ((dma_size + 63) / 64) * 64;

    if (!json_output) {
        printf("GPIO Loopback Benchmark\n");
        printf("  Source: %zu bytes (%zu words)\n", source_size, src_words);
        printf("  DMA:    %zu bytes (%zu words, %.1fx expansion)\n",
               dma_size, dma_words, (double)dma_size / (double)source_size);
        printf("  Pins:   output=GPIO%d, input=GPIO%d%s\n",
               output_pin, input_pin,
               output_pin == input_pin ? " (single-pin loopback)" : "");
        printf("  PIO:    %d cyc/bit, %d MHz, %.3f MB/s ceiling\n",
               CYCLES_PER_BIT, PIO_CLOCK_MHZ, THROUGHPUT_CEILING_MBPS);
        printf("  DMACTRL: TX=0x%08X (thresh=1), RX=0x%08X (thresh=4)\n",
               TX_DMACTRL, RX_DMACTRL);
        printf("\n");
    }

    /* ─── PIO setup ────────────────────────────────────────── */

    PIO pio = pio0;
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr, "ERROR: failed to open PIO\n");
        return 1;
    }

    int sm = pio_claim_unused_sm(pio, true);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machine\n");
        pio_close(pio);
        return 1;
    }

    uint offset = pio_add_program(pio, &gpio_loopback_program);

    /* Configure SM: 1-bit autopull/autopush */
    pio_sm_config c = gpio_loopback_program_get_default_config(offset);
    sm_config_set_set_pins(&c, (uint)output_pin, 1);
    sm_config_set_in_pins(&c, (uint)input_pin);
    sm_config_set_out_shift(&c, false, true, 1);   /* left shift, autopull, 1-bit */
    sm_config_set_in_shift(&c, false, true, 1);    /* left shift, autopush, 1-bit */
    sm_config_set_clkdiv(&c, 1.0f);               /* 200 MHz */

    pio_gpio_init(pio, (uint)output_pin);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)output_pin, 1, true);
    if (input_pin != output_pin) {
        pio_gpio_init(pio, (uint)input_pin);
        pio_sm_set_consecutive_pindirs(pio, (uint)sm, (uint)input_pin, 1, false);
    }

    pio_sm_init(pio, (uint)sm, offset, &c);
    pio_sm_set_pins_with_mask(pio, (uint)sm, 0, 1u << output_pin);

    /* ─── Allocate buffers ─────────────────────────────────── */

    uint32_t *src_buf = (uint32_t *)aligned_alloc(64, source_size);
    uint32_t *rx_result = (uint32_t *)aligned_alloc(64, source_size);
    uint32_t *tx_dma = (uint32_t *)aligned_alloc(64, alloc_size);
    uint32_t *rx_dma = (uint32_t *)aligned_alloc(64, alloc_size);
    double *throughputs = (double *)malloc((size_t)iterations * sizeof(double));

    if (!src_buf || !rx_result || !tx_dma || !rx_dma || !throughputs) {
        fprintf(stderr, "ERROR: failed to allocate buffers\n");
        ret = 1;
        goto cleanup;
    }

    /* ─── Warmup iterations ────────────────────────────────── */

    for (int i = 0; i < warmup; i++) {
        bench_fill_pattern(src_buf, src_words, pattern, (uint32_t)(i + 1));
        expand_tx(src_buf, tx_dma, src_words);
        for (size_t j = expanded_words; j < dma_words; j++)
            tx_dma[j] = 0;
        memset(rx_dma, 0xCC, alloc_size);

        if (dma_transfer(pio, (uint)sm, offset, &c, output_pin,
                         tx_dma, rx_dma, dma_size, NULL) < 0) {
            fprintf(stderr, "ERROR: DMA transfer failed during warmup %d\n", i);
            ret = 1;
            goto cleanup;
        }
    }

    /* ─── Measured iterations ──────────────────────────────── */

    uint32_t total_errors = 0;
    double total_start = get_time_sec();

    for (int i = 0; i < iterations; i++) {
        bench_fill_pattern(src_buf, src_words, pattern,
                           (uint32_t)(warmup + i + 1));

        /* Expand source → DMA format */
        expand_tx(src_buf, tx_dma, src_words);
        for (size_t j = expanded_words; j < dma_words; j++)
            tx_dma[j] = 0;
        memset(rx_dma, 0xCC, alloc_size);

        /* DMA transfer — timing is measured inside (SM enable to DMA done) */
        double pio_elapsed;
        int xfer_ret = dma_transfer(pio, (uint)sm, offset, &c, output_pin,
                                     tx_dma, rx_dma, dma_size, &pio_elapsed);

        if (xfer_ret < 0) {
            fprintf(stderr, "ERROR: DMA transfer failed at iteration %d\n", i);
            ret = 1;
            goto cleanup;
        }

        /* Throughput in terms of source data, not expanded DMA data */
        throughputs[i] = ((double)source_size / (1024.0 * 1024.0)) / pio_elapsed;

        /* Compress RX DMA → source words and verify */
        if (!no_verify) {
            compress_rx(rx_dma + RX_SKIP, rx_result, src_words);

            size_t mismatch_idx;
            uint32_t mismatch_exp, mismatch_act;
            uint32_t errors = bench_verify_identity(
                src_buf, rx_result, src_words,
                &mismatch_idx, &mismatch_exp, &mismatch_act);

            if (errors > 0 && total_errors == 0) {
                fprintf(stderr,
                    "ERROR: iter %d, first mismatch at word %zu: "
                    "expected 0x%08X, got 0x%08X (%u total errors)\n",
                    i, mismatch_idx, mismatch_exp, mismatch_act, errors);
            }
            total_errors += errors;
        }
    }

    double total_end = get_time_sec();
    double total_elapsed = total_end - total_start;

    /* ─── Report ───────────────────────────────────────────── */

    double *scratch = (double *)malloc((size_t)iterations * sizeof(double));
    if (!scratch) {
        fprintf(stderr, "ERROR: failed to allocate scratch buffer\n");
        ret = 1;
        goto cleanup;
    }

    bench_report_t report;
    bench_build_report(throughputs, (size_t)iterations, scratch,
                       source_size, total_elapsed, total_errors, &report);

    report.transfer_mode = "DMA (1-bit GPIO loopback)";
    report.transfer_mode_id = "dma";
    report.dma_threshold = 1;  /* TX threshold */
    report.dma_priority = -1;
    report.throughput_ceiling_mbps = THROUGHPUT_CEILING_MBPS;

    if (json_output)
        bench_print_json(stdout, &report);
    else
        bench_print_report(stdout, &report);

    ret = bench_print_verdict(stdout, &report, threshold);

    free(scratch);

cleanup:
    free(src_buf);
    free(rx_result);
    free(tx_dma);
    free(rx_dma);
    free(throughputs);

    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_remove_program(pio, &gpio_loopback_program, offset);
    pio_sm_unclaim(pio, (uint)sm);
    pio_close(pio);

    return ret;
}
