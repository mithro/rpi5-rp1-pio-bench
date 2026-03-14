/* gpio_loopback.c — RP1 PIO GPIO loopback benchmark
 *
 * Measures bit-serial throughput over a physical GPIO link.
 * TX SM serialises 32-bit words out a GPIO pin (set pins, 5 cycles/bit),
 * RX SM samples the looped-back pin and assembles words (5 cycles/bit).
 * DMA shuttles bulk data between host memory and the PIO FIFOs.
 *
 * Hardware: GPIO5 (output) → loopback wire → GPIO4 (input)
 * Theoretical throughput: 200 MHz / 5 cycles / 8 bits = 5.0 MB/s
 *
 * Build: gcc -O2 -I/usr/include/piolib -I../benchmark -o gpio_loopback \
 *        gpio_loopback.c ../benchmark/benchmark_stats.c \
 *        ../benchmark/benchmark_verify.c ../benchmark/benchmark_format.c \
 *        -lpio -lpthread -lm
 *
 * Run:   sudo ./gpio_loopback [options]
 */

#define _GNU_SOURCE  /* for clock_gettime, CLOCK_MONOTONIC */

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "piolib.h"
#include "gpio_loopback_tx.pio.h"
#include "gpio_loopback_rx.pio.h"

#include "benchmark_format.h"
#include "benchmark_stats.h"
#include "benchmark_verify.h"

#define DEFAULT_TRANSFER_SIZE  (256 * 1024)  /* 256 KB per iteration */
#define DEFAULT_ITERATIONS     100
#define DEFAULT_WARMUP         3
#define DEFAULT_PATTERN        BENCH_PATTERN_SEQUENTIAL
#define DEFAULT_THRESHOLD_MBPS 4.0   /* ~80% of 5.0 MB/s theoretical */
#define DEFAULT_OUTPUT_PIN     5
#define DEFAULT_INPUT_PIN      4

#define DEFAULT_DMA_THRESHOLD  8
#define DEFAULT_DMA_PRIORITY   2

/* Theoretical throughput: 200 MHz / 5 cycles per bit / 8 bits per byte */
#define THROUGHPUT_CEILING_MBPS 5.0

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

/* Two-SM DMA transfer: TX and RX run on separate SMs concurrently. */
static int dma_transfer_gpio(PIO pio, uint sm_tx, uint sm_rx,
                              void *tx_buf, void *rx_buf, size_t size)
{
    pthread_t tx_tid, rx_tid;
    xfer_args_t tx_args = {pio, sm_tx, PIO_DIR_TO_SM, size, tx_buf, 0};
    xfer_args_t rx_args = {pio, sm_rx, PIO_DIR_FROM_SM, size, rx_buf, 0};

    pthread_create(&tx_tid, NULL, xfer_thread, &tx_args);
    pthread_create(&rx_tid, NULL, xfer_thread, &rx_args);
    pthread_join(tx_tid, NULL);
    pthread_join(rx_tid, NULL);

    if (tx_args.ret < 0 || rx_args.ret < 0)
        return -1;
    return 0;
}

/* ─── Usage ─────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "GPIO loopback benchmark: serialise data out one pin, read back from another.\n"
        "Requires external loopback wire between output and input pins.\n"
        "\n"
        "Options:\n"
        "  --size=BYTES       Transfer size per iteration (default %d)\n"
        "  --iterations=N     Number of measured iterations (default %d)\n"
        "  --warmup=N         Warmup iterations (default %d)\n"
        "  --pattern=ID       Test pattern: 0=seq, 1=ones, 2=alt, 3=random (default %d)\n"
        "  --threshold=MB/S   Pass/fail threshold (default %.1f)\n"
        "  --json             Output JSON instead of human-readable table\n"
        "  --no-verify        Skip data verification (measure raw throughput)\n"
        "  --dma-threshold=N  FIFO threshold 1-8 (default %d)\n"
        "  --dma-priority=N   DMA priority 0-31 (default %d)\n"
        "  --output-pin=N     GPIO pin for TX output (default %d)\n"
        "  --input-pin=N      GPIO pin for RX input (default %d)\n"
        "  --help             Show this help\n",
        prog,
        DEFAULT_TRANSFER_SIZE,
        DEFAULT_ITERATIONS,
        DEFAULT_WARMUP,
        DEFAULT_PATTERN,
        DEFAULT_THRESHOLD_MBPS,
        DEFAULT_DMA_THRESHOLD,
        DEFAULT_DMA_PRIORITY,
        DEFAULT_OUTPUT_PIN,
        DEFAULT_INPUT_PIN);
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
    int dma_threshold = DEFAULT_DMA_THRESHOLD;
    int dma_priority = DEFAULT_DMA_PRIORITY;
    int output_pin = DEFAULT_OUTPUT_PIN;
    int input_pin = DEFAULT_INPUT_PIN;

    /* Parse command-line options. */
    static struct option long_options[] = {
        {"size",          required_argument, NULL, 's'},
        {"iterations",    required_argument, NULL, 'i'},
        {"warmup",        required_argument, NULL, 'w'},
        {"pattern",       required_argument, NULL, 'p'},
        {"threshold",     required_argument, NULL, 't'},
        {"json",          no_argument,       NULL, 'j'},
        {"no-verify",     no_argument,       NULL, 'n'},
        {"dma-threshold", required_argument, NULL, 'T'},
        {"dma-priority",  required_argument, NULL, 'P'},
        {"output-pin",    required_argument, NULL, 'o'},
        {"input-pin",     required_argument, NULL, 'I'},
        {"help",          no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:i:w:p:t:jnT:P:o:I:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 's': transfer_size = (size_t)atol(optarg); break;
        case 'i': iterations = atoi(optarg); break;
        case 'w': warmup = atoi(optarg); break;
        case 'p': pattern = atoi(optarg); break;
        case 't': threshold = atof(optarg); break;
        case 'j': json_output = 1; break;
        case 'n': no_verify = 1; break;
        case 'T': dma_threshold = atoi(optarg); break;
        case 'P': dma_priority = atoi(optarg); break;
        case 'o': output_pin = atoi(optarg); break;
        case 'I': input_pin = atoi(optarg); break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* Validate parameters. */
    if (transfer_size < 4 || transfer_size % 4 != 0) {
        fprintf(stderr, "ERROR: transfer size must be >= 4 and a multiple of 4\n");
        return 1;
    }
    if (iterations < 1) {
        fprintf(stderr, "ERROR: iterations must be >= 1\n");
        return 1;
    }
    if (dma_threshold < 1 || dma_threshold > 8) {
        fprintf(stderr, "ERROR: --dma-threshold must be 1-8 (FIFO depth)\n");
        return 1;
    }
    if (dma_priority < 0 || dma_priority > 31) {
        fprintf(stderr, "ERROR: --dma-priority must be 0-31 (5-bit field)\n");
        return 1;
    }
    if (output_pin < 0 || output_pin > 27) {
        fprintf(stderr, "ERROR: --output-pin must be 0-27\n");
        return 1;
    }
    if (input_pin < 0 || input_pin > 27) {
        fprintf(stderr, "ERROR: --input-pin must be 0-27\n");
        return 1;
    }
    if (output_pin == input_pin) {
        fprintf(stderr, "ERROR: --output-pin and --input-pin must differ\n");
        return 1;
    }

    size_t word_count = transfer_size / 4;

    /* ─── PIO setup ─────────────────────────────────────────── */

    PIO pio = pio0;
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr, "ERROR: failed to open PIO (is /dev/pio0 present?)\n");
        return 1;
    }

    /* Claim two state machines. */
    int sm_tx = pio_claim_unused_sm(pio, true);
    if (sm_tx < 0) {
        fprintf(stderr, "ERROR: no free state machine for TX\n");
        pio_close(pio);
        return 1;
    }
    int sm_rx = pio_claim_unused_sm(pio, true);
    if (sm_rx < 0) {
        fprintf(stderr, "ERROR: no free state machine for RX\n");
        pio_sm_unclaim(pio, (uint)sm_tx);
        pio_close(pio);
        return 1;
    }

    /* Load both PIO programs.
     * Initialize offsets to sentinel so cleanup_prog can skip unloaded ones. */
    uint tx_offset = PIO_ORIGIN_INVALID;
    uint rx_offset = PIO_ORIGIN_INVALID;

    tx_offset = pio_add_program(pio, &gpio_loopback_tx_program);
    if (tx_offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load TX PIO program\n");
        ret = 1;
        goto cleanup_sm;
    }

    rx_offset = pio_add_program(pio, &gpio_loopback_rx_program);
    if (rx_offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load RX PIO program\n");
        ret = 1;
        goto cleanup_prog;
    }

    /* Configure TX state machine.
     * Pin routing uses set_pins (not out_pins) — on RP1, 'out pins' does NOT
     * drive physical GPIO pads. out_shift controls the OSR for 'out x, 1'. */
    pio_sm_config tx_c = gpio_loopback_tx_program_get_default_config(tx_offset);
    sm_config_set_set_pins(&tx_c, (uint)output_pin, 1);
    sm_config_set_out_shift(&tx_c, false, true, 32);  /* left shift, autopull, 32-bit */
    sm_config_set_clkdiv(&tx_c, 1.0f);                /* full 200 MHz */
    pio_gpio_init(pio, (uint)output_pin);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm_tx, (uint)output_pin, 1, true);
    pio_sm_init(pio, (uint)sm_tx, tx_offset, &tx_c);

    /* Configure RX state machine. */
    pio_sm_config rx_c = gpio_loopback_rx_program_get_default_config(rx_offset);
    sm_config_set_in_pins(&rx_c, (uint)input_pin);
    sm_config_set_in_shift(&rx_c, false, true, 32);   /* left shift, autopush, 32-bit */
    sm_config_set_clkdiv(&rx_c, 1.0f);                /* full 200 MHz */
    pio_sm_init(pio, (uint)sm_rx, rx_offset, &rx_c);

    /* Force output pin LOW before enabling. */
    pio_sm_set_pins_with_mask(pio, (uint)sm_tx, 0, 1u << output_pin);

    /* Configure DMA for both SMs. */
    ret = pio_sm_config_xfer(pio, (uint)sm_tx, PIO_DIR_TO_SM, transfer_size, 1);
    if (ret < 0) {
        fprintf(stderr, "ERROR: failed to configure TX DMA (%d)\n", ret);
        goto cleanup_prog;
    }
    ret = pio_sm_config_xfer(pio, (uint)sm_rx, PIO_DIR_FROM_SM, transfer_size, 1);
    if (ret < 0) {
        fprintf(stderr, "ERROR: failed to configure RX DMA (%d)\n", ret);
        goto cleanup_prog;
    }

    uint32_t dmactrl = build_dmactrl(dma_threshold, dma_priority);
    pio_sm_set_dmactrl(pio, (uint)sm_tx, true,  dmactrl);  /* TX direction */
    pio_sm_set_dmactrl(pio, (uint)sm_rx, false, dmactrl);  /* RX direction */

    /* SM mask for simultaneous enable/disable. */
    uint32_t sm_mask = (1u << sm_tx) | (1u << sm_rx);

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

        /* Disable, clear FIFOs, restart both SMs. */
        pio_set_sm_mask_enabled(pio, sm_mask, false);
        pio_sm_clear_fifos(pio, (uint)sm_tx);
        pio_sm_clear_fifos(pio, (uint)sm_rx);
        pio_sm_restart(pio, (uint)sm_tx);
        pio_sm_restart(pio, (uint)sm_rx);

        /* Force output LOW before re-enable. */
        pio_sm_set_pins_with_mask(pio, (uint)sm_tx, 0, 1u << output_pin);

        /* Enable both SMs simultaneously — required for sampling alignment.
         * TX drives at cycle [2], RX samples at cycle [4]. If SMs start
         * out of phase, the 2-cycle synchroniser margin is violated. */
        pio_set_sm_mask_enabled(pio, sm_mask, true);

        if (dma_transfer_gpio(pio, (uint)sm_tx, (uint)sm_rx,
                               tx_buf, rx_buf, transfer_size) < 0) {
            fprintf(stderr, "ERROR: DMA transfer failed during warmup %d\n", i);
            ret = 1;
            goto cleanup_bufs;
        }
    }

    /* ─── Measured iterations ───────────────────────────────── */

    uint32_t total_errors = 0;
    double total_start = get_time_sec();

    for (int i = 0; i < iterations; i++) {
        bench_fill_pattern(tx_buf, word_count, pattern, (uint32_t)(warmup + i + 1));
        memset(rx_buf, 0, transfer_size);

        /* Disable, clear FIFOs, restart both SMs. */
        pio_set_sm_mask_enabled(pio, sm_mask, false);
        pio_sm_clear_fifos(pio, (uint)sm_tx);
        pio_sm_clear_fifos(pio, (uint)sm_rx);
        pio_sm_restart(pio, (uint)sm_tx);
        pio_sm_restart(pio, (uint)sm_rx);

        /* Force output LOW before re-enable. */
        pio_sm_set_pins_with_mask(pio, (uint)sm_tx, 0, 1u << output_pin);

        /* Enable both SMs simultaneously — required for sampling alignment.
         * TX drives at cycle [2], RX samples at cycle [4]. If SMs start
         * out of phase, the 2-cycle synchroniser margin is violated. */
        pio_set_sm_mask_enabled(pio, sm_mask, true);

        double t0 = get_time_sec();
        int xfer_ret = dma_transfer_gpio(pio, (uint)sm_tx, (uint)sm_rx,
                                          tx_buf, rx_buf, transfer_size);
        double t1 = get_time_sec();

        if (xfer_ret < 0) {
            fprintf(stderr, "ERROR: DMA transfer failed at iteration %d\n", i);
            ret = 1;
            goto cleanup_bufs;
        }

        throughputs[i] = ((double)transfer_size / (1024.0 * 1024.0)) / (t1 - t0);

        if (!no_verify) {
            size_t mismatch_idx;
            uint32_t mismatch_exp, mismatch_act;
            uint32_t errors = bench_verify_identity(tx_buf, rx_buf, word_count,
                                                     &mismatch_idx,
                                                     &mismatch_exp,
                                                     &mismatch_act);
            if (errors > 0 && total_errors == 0) {
                fprintf(stderr,
                    "ERROR: data mismatch at iteration %d, word %zu: "
                    "expected 0x%08X, got 0x%08X (XOR 0x%08X)\n",
                    i, mismatch_idx, mismatch_exp, mismatch_act,
                    mismatch_exp ^ mismatch_act);
            }
            total_errors += errors;
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
    snprintf(mode_label, sizeof(mode_label),
             "DMA (threshold=%d, priority=%d)", dma_threshold, dma_priority);
    report.transfer_mode = mode_label;
    report.transfer_mode_id = "dma";
    report.dma_threshold = dma_threshold;
    report.dma_priority = dma_priority;
    report.throughput_ceiling_mbps = THROUGHPUT_CEILING_MBPS;

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

cleanup_prog:
    pio_set_sm_mask_enabled(pio, sm_mask, false);
    if (rx_offset != PIO_ORIGIN_INVALID)
        pio_remove_program(pio, &gpio_loopback_rx_program, rx_offset);
    if (tx_offset != PIO_ORIGIN_INVALID)
        pio_remove_program(pio, &gpio_loopback_tx_program, tx_offset);

cleanup_sm:
    pio_sm_unclaim(pio, (uint)sm_rx);
    pio_sm_unclaim(pio, (uint)sm_tx);
    pio_close(pio);

    return ret;
}
