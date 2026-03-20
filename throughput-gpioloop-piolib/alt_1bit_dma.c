/* test_1bit_dma.c — Large-scale verification of 1-bit DMA GPIO loopback
 *
 * Proven configuration:
 * - skip=1 (1 garbage word from uninitialized OSR at startup)
 * - TX DMACTRL threshold=1 (avoids autopull/DMA FIFO collision)
 * - RX DMACTRL threshold=4 (ensures DMA drains RX FIFO fast enough)
 * - DMA-before-SM sequencing with usleep(5000)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "piolib.h"
#include "alt_1bit_dma.pio.h"

#define SKIP 1  /* 1 garbage word from uninitialized OSR */
#define TX_DMACTRL 0x80000101u  /* threshold=1 */
#define RX_DMACTRL 0x80000104u  /* threshold=4 */

typedef struct {
    PIO pio; uint sm; enum pio_xfer_dir dir; size_t size; void *buf; int ret;
} xfer_args_t;

static void *xfer_thread(void *arg) {
    xfer_args_t *a = (xfer_args_t *)arg;
    a->ret = pio_sm_xfer_data(a->pio, a->sm, a->dir, a->size, a->buf);
    if (a->ret < 0)
        fprintf(stderr, "xfer(%s) ret=%d errno=%d\n",
                a->dir == PIO_DIR_TO_SM ? "TX" : "RX", a->ret, errno);
    return NULL;
}

static void expand_tx(const uint32_t *src, uint32_t *dst, size_t src_words)
{
    for (size_t i = 0; i < src_words; i++) {
        uint32_t word = src[i];
        for (int bit = 31; bit >= 0; bit--)
            *dst++ = ((word >> bit) & 1) ? 0x80000000u : 0;
    }
}

static uint32_t compress_rx_word(const uint32_t *rx, size_t start)
{
    uint32_t word = 0;
    for (int bit = 31; bit >= 0; bit--) {
        if (rx[start + (size_t)(31 - bit)] & 1)
            word |= (1u << bit);
    }
    return word;
}

/* Run one test: returns error count, or -1 for DMA fail */
static int run_test(PIO pio, uint sm, uint offset, const pio_sm_config *cfg,
                    const uint32_t *src, size_t src_words, int verbose)
{
    size_t real_dma = src_words * 32;
    size_t dma_words = SKIP + real_dma + 1;
    size_t dma_size = dma_words * 4;
    size_t alloc_size = ((dma_size + 63) / 64) * 64;

    uint32_t *tx_dma = aligned_alloc(64, alloc_size);
    uint32_t *rx_dma = aligned_alloc(64, alloc_size);

    expand_tx(src, tx_dma, src_words);
    for (size_t i = real_dma; i < dma_words; i++)
        tx_dma[i] = 0;
    memset(rx_dma, 0xCC, alloc_size);

    /* Full SM reset */
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_init(pio, sm, offset, cfg);
    pio_sm_set_pins_with_mask(pio, sm, 0, 1u << 5);

    /* Configure DMA */
    pio_sm_config_xfer(pio, sm, PIO_DIR_TO_SM, dma_size, 1);
    pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, dma_size, 1);
    pio_sm_set_dmactrl(pio, sm, true, TX_DMACTRL);
    pio_sm_set_dmactrl(pio, sm, false, RX_DMACTRL);

    /* Start DMA before SM */
    pthread_t tx_tid, rx_tid;
    xfer_args_t ta = {pio, sm, PIO_DIR_TO_SM, dma_size, tx_dma, 0};
    xfer_args_t ra = {pio, sm, PIO_DIR_FROM_SM, dma_size, rx_dma, 0};
    pthread_create(&rx_tid, NULL, xfer_thread, &ra);
    pthread_create(&tx_tid, NULL, xfer_thread, &ta);
    usleep(5000);
    pio_sm_set_enabled(pio, sm, true);
    pthread_join(tx_tid, NULL);
    pthread_join(rx_tid, NULL);
    pio_sm_set_enabled(pio, sm, false);

    if (ta.ret < 0 || ra.ret < 0) {
        free(tx_dma); free(rx_dma);
        return -1;
    }

    /* Compress and verify */
    int errors = 0;
    for (size_t w = 0; w < src_words; w++) {
        uint32_t got = compress_rx_word(rx_dma, SKIP + w * 32);
        if (got != src[w]) {
            errors++;
            if (verbose && errors <= 5) {
                printf("    word[%zu] TX=0x%08X RX=0x%08X\n", w, src[w], got);
            }
        }
    }

    free(tx_dma); free(rx_dma);
    return errors;
}

static void fill_pattern(uint32_t *buf, size_t words, int pattern)
{
    for (size_t i = 0; i < words; i++) {
        switch (pattern) {
        case 0: buf[i] = (uint32_t)(i + 1); break;
        case 1: buf[i] = 0xDEADBEEF; break;
        case 2: buf[i] = 0xAAAAAAAA; break;
        case 3: buf[i] = (uint32_t)((i * 2654435761u) ^ 0xB5); break;
        case 4: buf[i] = 0x55555555; break;
        case 5: buf[i] = 0xFFFFFFFF; break;
        case 6: buf[i] = (i & 1) ? 0xFFFFFFFF : 0x00000000; break;
        default: buf[i] = (uint32_t)i; break;
        }
    }
}

static const char *pattern_names[] = {
    "counting", "DEADBEEF", "AAAAAAAA", "hash",
    "55555555", "FFFFFFFF", "toggle"
};

int main(void)
{
    PIO pio = pio0;
    uint sm = (uint)pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &test_1bit_dma_program);

    pio_sm_config c = test_1bit_dma_program_get_default_config(offset);
    sm_config_set_set_pins(&c, 5, 1);
    sm_config_set_in_pins(&c, 5);
    sm_config_set_out_shift(&c, false, true, 1);
    sm_config_set_in_shift(&c, false, true, 1);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_gpio_init(pio, 5);
    pio_sm_set_consecutive_pindirs(pio, sm, 5, 1, true);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_pins_with_mask(pio, sm, 0, 1u << 5);

    printf("=== 1-bit DMA GPIO loopback: large-scale verification ===\n");
    printf("skip=%d, TX=0x%08X, RX=0x%08X\n\n", SKIP, TX_DMACTRL, RX_DMACTRL);

    size_t sizes[] = {1, 4, 16, 64, 256, 1024};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    int npatterns = 7;
    int iterations = 20;

    int total_pass = 0, total_fail = 0, total_dma_fail = 0;

    for (int si = 0; si < nsizes; si++) {
        size_t src_words = sizes[si];
        size_t dma_words = SKIP + src_words * 32 + 1;
        printf("--- %zu words (%zu bytes src, %zu DMA words) ---\n",
               src_words, src_words * 4, dma_words);

        uint32_t *src = malloc(src_words * 4);

        for (int pi = 0; pi < npatterns; pi++) {
            fill_pattern(src, src_words, pi);
            int pass = 0, fail = 0, dma_fail = 0;

            for (int iter = 0; iter < iterations; iter++) {
                int ret = run_test(pio, sm, offset, &c, src, src_words,
                                   iter == 0);
                if (ret == 0) pass++;
                else if (ret < 0) dma_fail++;
                else fail++;
            }

            printf("  %-10s %d/%d", pattern_names[pi], pass, iterations);
            if (fail > 0) printf(" FAIL(%d)", fail);
            if (dma_fail > 0) printf(" DMA_FAIL(%d)", dma_fail);
            if (pass == iterations) printf(" PERFECT");
            printf("\n");

            total_pass += pass;
            total_fail += fail;
            total_dma_fail += dma_fail;
        }

        free(src);
        printf("\n");
    }

    printf("=== SUMMARY ===\n");
    int total = total_pass + total_fail + total_dma_fail;
    printf("Total: %d/%d pass (%d fail, %d dma_fail)\n",
           total_pass, total, total_fail, total_dma_fail);
    printf("Result: %s\n",
           (total_fail == 0 && total_dma_fail == 0) ? "ALL PASS" : "SOME FAILED");

    pio_remove_program(pio, &test_1bit_dma_program, offset);
    pio_sm_unclaim(pio, sm);
    pio_close(pio);
    return 0;
}
