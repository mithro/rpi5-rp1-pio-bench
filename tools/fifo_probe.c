/* fifo_probe.c — Direct PIO FIFO access probe via /dev/mem
 *
 * Loads the internal loopback PIO program (TX → NOT → RX) via piolib,
 * then accesses PIO FIFOs directly through /dev/mem mmap of BAR1.
 * Measures polled throughput and validates SRAM↔FIFO data paths.
 *
 * Requires: RPi5, sudo, libpio-dev
 *
 * Build: gcc -O2 -I../lib -I/usr/include/piolib -o fifo_probe \
 *        fifo_probe.c ../lib/benchmark_stats.c \
 *        ../lib/benchmark_verify.c ../lib/benchmark_format.c \
 *        -lpio -lpthread -lm
 * Run:   sudo ./fifo_probe
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#include "piolib.h"
#include "../lib/pio_loopback.pio.h"

#include "benchmark_format.h"
#include "benchmark_stats.h"
#include "benchmark_verify.h"

/* ─── RP1 Physical addresses ─────────────────────────────────── */

/* PIO block base (BAR1) */
#define PIO_PHYS_ADDR    0x1F00178000ULL
#define PIO_MAP_SIZE     0x1000  /* 4 KB is sufficient for FIFO registers */

/* SRAM base (BAR2) */
#define SRAM_PHYS_ADDR   0x1F00400000ULL
#define SRAM_SIZE        0x10000  /* 64 KB */
#define SRAM_SAFE_SIZE   0xFF00   /* 65,280 bytes usable */
#define SRAM_SAFE_WORDS  (SRAM_SAFE_SIZE / 4)

/* ─── PIO FIFO register offsets (from PIO base) ──────────────── */
/* NOTE: RP1 PIO register layout differs from RP2040/RP2350!
 * These offsets come from the rp1-pio kernel driver (rp1-pio.c).
 * FSTAT and other control registers are NOT directly accessible
 * from the host — they're behind the RP1 firmware RPC layer. */

#define PIO_TXF0_OFF     0x000   /* TX FIFO for SM0 (write-only) */
#define PIO_TXF1_OFF     0x004
#define PIO_TXF2_OFF     0x008
#define PIO_TXF3_OFF     0x00C
#define PIO_RXF0_OFF     0x010   /* RX FIFO for SM0 (read-only) */
#define PIO_RXF1_OFF     0x014
#define PIO_RXF2_OFF     0x018
#define PIO_RXF3_OFF     0x01C

/* ─── Timing ──────────────────────────────────────────────────── */

static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ─── Watchdog (prevent hanging in D state) ───────────────────── */

static volatile sig_atomic_t watchdog_fired = 0;

static void watchdog_handler(int sig)
{
    (void)sig;
    watchdog_fired = 1;
    /* Write directly to stderr to be async-signal-safe */
    const char msg[] = "\nWATCHDOG: operation timed out, exiting\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(2);
}

static void watchdog_start(unsigned int seconds)
{
    watchdog_fired = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watchdog_handler;
    sigaction(SIGALRM, &sa, NULL);
    alarm(seconds);
}

static void watchdog_stop(void)
{
    alarm(0);
}

/* ─── Global state ────────────────────────────────────────────── */

static volatile uint32_t *pio_regs;   /* mmap'd PIO register base */
static volatile uint32_t *sram_base;  /* mmap'd SRAM */
static int mem_fd = -1;

static PIO pio;
static int sm = -1;
static uint offset;

/* ─── /dev/mem mmap helpers ───────────────────────────────────── */

static volatile uint32_t *mmap_phys(unsigned long long phys_addr, size_t size)
{
    volatile uint32_t *ptr = (volatile uint32_t *)mmap(
        NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
        mem_fd, (off_t)phys_addr);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap 0x%llx failed: %s\n",
                phys_addr, strerror(errno));
        return NULL;
    }
    return ptr;
}

static int devmem_init(void)
{
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "ERROR: cannot open /dev/mem: %s\n", strerror(errno));
        return -1;
    }

    pio_regs = mmap_phys(PIO_PHYS_ADDR, PIO_MAP_SIZE);
    if (!pio_regs)
        return -1;

    sram_base = mmap_phys(SRAM_PHYS_ADDR, SRAM_SIZE);
    if (!sram_base)
        return -1;

    return 0;
}

static void devmem_cleanup(void)
{
    if (pio_regs)
        munmap((void *)pio_regs, PIO_MAP_SIZE);
    if (sram_base)
        munmap((void *)sram_base, SRAM_SIZE);
    if (mem_fd >= 0)
        close(mem_fd);
}

/* ─── Direct FIFO access helpers ──────────────────────────────── */

/* FSTAT is not directly accessible from host on RP1 — use piolib
 * ioctl functions for FIFO status, but direct register access for
 * FIFO data reads/writes. */

static inline int tx_full(void)
{
    return pio_sm_is_tx_fifo_full(pio, (uint)sm);
}

static inline int rx_empty(void)
{
    return pio_sm_is_rx_fifo_empty(pio, (uint)sm);
}

static inline void fifo_tx_write(uint32_t val)
{
    pio_regs[PIO_TXF0_OFF / 4] = val;
}

static inline uint32_t fifo_rx_read(void)
{
    return pio_regs[PIO_RXF0_OFF / 4];
}

/* ─── PIO setup via piolib ────────────────────────────────────── */

static int pio_setup(void)
{
    pio = pio0;
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr, "ERROR: failed to open PIO\n");
        return -1;
    }

    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machines (all claimed by other processes?)\n");
        fprintf(stderr, "  Try: sudo reboot, or kill other PIO users\n");
        pio_close(pio);
        return -1;
    }

    offset = pio_add_program(pio, &loopback_program);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load PIO program\n");
        pio_sm_unclaim(pio, (uint)sm);
        pio_close(pio);
        return -1;
    }

    /* Configure: 32-bit autopull/autopush, 200 MHz */
    pio_sm_config c = loopback_program_get_default_config(offset);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, (uint)sm, offset, &c);
    pio_sm_set_enabled(pio, (uint)sm, true);

    printf("  PIO SM%d configured with loopback program at offset %u\n", sm, offset);
    return 0;
}

static void pio_teardown(void)
{
    if (sm >= 0) {
        pio_sm_set_enabled(pio, (uint)sm, false);
        pio_remove_program(pio, &loopback_program, offset);
        pio_sm_unclaim(pio, (uint)sm);
    }
    pio_close(pio);
}

/* ─── Test 1: Single-word direct FIFO loopback ────────────────── */

static int test_single_word(void)
{
    printf("\n=== Test 1: Single-Word Direct FIFO Loopback ===\n\n");

    /* First verify piolib blocking path works (as baseline).
     * Use watchdog to prevent hanging in uninterruptible kernel ioctl. */
    printf("  Baseline: piolib blocking path...\n");
    uint32_t tx_val = 0xDEADBEEFu;
    watchdog_start(5);
    pio_sm_put_blocking(pio, (uint)sm, tx_val);
    uint32_t rx_val = pio_sm_get_blocking(pio, (uint)sm);
    watchdog_stop();
    if (rx_val != ~tx_val) {
        printf("  FAIL: piolib baseline broken! sent 0x%08x, expected 0x%08x, got 0x%08x\n",
               tx_val, ~tx_val, rx_val);
        return 1;
    }
    printf("  Baseline PASS: piolib 0x%08x → 0x%08x (expected 0x%08x)\n",
           tx_val, rx_val, ~tx_val);

    /* Now test direct register access */
    printf("  Testing direct register access...\n");

    uint32_t test_words[] = {
        0x00000000u, 0xFFFFFFFFu, 0xAAAAAAAAu, 0x55555555u,
        0x12345678u, 0xDEADBEEFu, 0x80000001u, 0x7FFFFFFEu,
    };
    int num_tests = (int)(sizeof(test_words) / sizeof(test_words[0]));
    int errors = 0;

    for (int i = 0; i < num_tests; i++) {
        tx_val = test_words[i];
        uint32_t expected = ~tx_val;

        /* Write via direct register, read via piolib (to isolate TX path) */
        fifo_tx_write(tx_val);
        rx_val = pio_sm_get_blocking(pio, (uint)sm);

        if (rx_val != expected) {
            printf("  FAIL: word %d: sent 0x%08x, expected ~= 0x%08x, got 0x%08x\n",
                   i, tx_val, expected, rx_val);
            errors++;
        }
    }

    if (errors == 0)
        printf("  Direct TX PASS: %d words sent via direct register, received via piolib\n",
               num_tests);
    else
        printf("  Direct TX FAIL: %d / %d words mismatched\n", errors, num_tests);

    /* Now test direct RX read: write via piolib, read via direct register */
    printf("  Testing direct RX read...\n");
    int rx_errors = 0;

    for (int i = 0; i < num_tests; i++) {
        tx_val = test_words[i];
        uint32_t expected = ~tx_val;

        pio_sm_put_blocking(pio, (uint)sm, tx_val);

        /* Small delay to ensure PIO has processed the word and
         * the RX FIFO has data before we read via direct register */
        usleep(1);

        /* Check RX FIFO has data before reading */
        if (rx_empty()) {
            printf("  WARNING: RX FIFO empty after piolib put + 1us delay (word %d)\n", i);
            /* Try waiting a bit more */
            for (int w = 0; w < 1000 && rx_empty(); w++)
                usleep(1);
            if (rx_empty()) {
                printf("  FAIL: RX FIFO still empty after 1ms\n");
                rx_errors++;
                continue;
            }
        }

        rx_val = fifo_rx_read();

        if (rx_val != expected) {
            printf("  FAIL: word %d: sent 0x%08x, expected ~= 0x%08x, got 0x%08x\n",
                   i, tx_val, expected, rx_val);
            rx_errors++;
        }
    }

    if (rx_errors == 0)
        printf("  Direct RX PASS: %d words sent via piolib, received via direct register\n",
               num_tests);
    else
        printf("  Direct RX FAIL: %d / %d words mismatched\n", rx_errors, num_tests);

    /* Full direct path: TX and RX both via direct registers */
    printf("  Testing full direct TX+RX...\n");
    int full_errors = 0;

    for (int i = 0; i < num_tests; i++) {
        tx_val = test_words[i];
        uint32_t expected = ~tx_val;

        fifo_tx_write(tx_val);
        usleep(1);  /* allow PIO to process */

        if (rx_empty()) {
            for (int w = 0; w < 1000 && rx_empty(); w++)
                usleep(1);
            if (rx_empty()) {
                printf("  FAIL: RX FIFO empty for full direct word %d\n", i);
                full_errors++;
                continue;
            }
        }

        rx_val = fifo_rx_read();

        if (rx_val != expected) {
            printf("  FAIL: word %d: sent 0x%08x, expected ~= 0x%08x, got 0x%08x\n",
                   i, tx_val, expected, rx_val);
            full_errors++;
        }
    }

    if (full_errors == 0)
        printf("  Full direct PASS: %d words via direct TX + direct RX\n", num_tests);
    else
        printf("  Full direct FAIL: %d / %d words mismatched\n", full_errors, num_tests);

    return (errors > 0 || rx_errors > 0 || full_errors > 0) ? 1 : 0;
}

/* ─── Test 2: Polled throughput (direct vs piolib) ────────────── */

#define POLLED_WORDS  256
#define POLLED_ITERS  5

static int test_polled_throughput(void)
{
    printf("\n=== Test 2: Polled FIFO Throughput ===\n\n");

    uint32_t *tx_buf = (uint32_t *)malloc(POLLED_WORDS * 4);
    uint32_t *rx_buf = (uint32_t *)malloc(POLLED_WORDS * 4);
    if (!tx_buf || !rx_buf) {
        fprintf(stderr, "ERROR: malloc failed\n");
        free(tx_buf);
        free(rx_buf);
        return 1;
    }

    /* Fill TX buffer */
    bench_fill_pattern(tx_buf, POLLED_WORDS, BENCH_PATTERN_SEQUENTIAL, 42);

    /* ── Direct register with piolib FSTAT polling ── */
    /* Use piolib ioctl for FIFO status, direct registers for data.
     * This measures: data via direct reg, status via ioctl. */
    double direct_times[POLLED_ITERS];
    for (int iter = 0; iter < POLLED_ITERS; iter++) {
        memset(rx_buf, 0, POLLED_WORDS * 4);

        /* Drain any stale RX data */
        while (!rx_empty())
            (void)fifo_rx_read();

        double t0 = get_time_sec();
        for (size_t i = 0; i < POLLED_WORDS; i++) {
            while (tx_full())
                ;
            fifo_tx_write(tx_buf[i]);
            while (rx_empty())
                ;
            rx_buf[i] = fifo_rx_read();
        }
        double t1 = get_time_sec();
        direct_times[iter] = t1 - t0;
    }

    /* Verify last iteration */
    uint32_t direct_errors = bench_verify_not(tx_buf, rx_buf, POLLED_WORDS,
                                               NULL, NULL, NULL);

    /* ── piolib blocking throughput ── */
    double blocking_times[POLLED_ITERS];
    watchdog_start(60);  /* generous timeout for blocking transfers */
    for (int iter = 0; iter < POLLED_ITERS; iter++) {
        memset(rx_buf, 0, POLLED_WORDS * 4);

        double t0 = get_time_sec();
        for (size_t i = 0; i < POLLED_WORDS; i++) {
            pio_sm_put_blocking(pio, (uint)sm, tx_buf[i]);
            rx_buf[i] = pio_sm_get_blocking(pio, (uint)sm);
        }
        double t1 = get_time_sec();
        blocking_times[iter] = t1 - t0;
    }
    watchdog_stop();

    /* Verify last iteration */
    uint32_t blocking_errors = bench_verify_not(tx_buf, rx_buf, POLLED_WORDS,
                                                 NULL, NULL, NULL);

    /* Compute stats */
    double direct_min = 1e9, direct_sum = 0;
    double blocking_min = 1e9, blocking_sum = 0;

    for (int i = 0; i < POLLED_ITERS; i++) {
        double d_mbps = ((double)(POLLED_WORDS * 4) / (1024.0 * 1024.0))
                        / direct_times[i];
        double b_mbps = ((double)(POLLED_WORDS * 4) / (1024.0 * 1024.0))
                        / blocking_times[i];

        if (d_mbps < direct_min) direct_min = d_mbps;
        direct_sum += d_mbps;
        if (b_mbps < blocking_min) blocking_min = b_mbps;
        blocking_sum += b_mbps;
    }

    double direct_mean = direct_sum / POLLED_ITERS;
    double blocking_mean = blocking_sum / POLLED_ITERS;

    printf("  Direct reg + piolib FSTAT (%d words x %d iters):\n",
           POLLED_WORDS, POLLED_ITERS);
    printf("    Throughput: min %.3f MB/s, mean %.3f MB/s\n",
           direct_min, direct_mean);
    printf("    Data integrity: %s (%u errors)\n",
           direct_errors == 0 ? "PASS" : "FAIL", direct_errors);

    printf("  piolib blocking put/get (%d words x %d iters):\n",
           POLLED_WORDS, POLLED_ITERS);
    printf("    Throughput: min %.3f MB/s, mean %.3f MB/s\n",
           blocking_min, blocking_mean);
    printf("    Data integrity: %s (%u errors)\n",
           blocking_errors == 0 ? "PASS" : "FAIL", blocking_errors);

    printf("  Speedup: direct/blocking = %.2fx\n",
           direct_mean / blocking_mean);

    free(tx_buf);
    free(rx_buf);

    return (direct_errors > 0 || blocking_errors > 0) ? 1 : 0;
}

/* ─── Test 3: SRAM-mediated FIFO loopback ─────────────────────── */

#define SRAM_TEST_WORDS  256
#define SRAM_TEST_ITERS  5

static int test_sram_mediated(void)
{
    printf("\n=== Test 3: SRAM-Mediated FIFO Loopback ===\n\n");

    /* Use SRAM as both source and destination:
     *   SRAM[0..N-1] = TX data (host writes)
     *   → CPU reads SRAM, writes TXF0
     *   → PIO does bitwise NOT
     *   → CPU reads RXF0, writes SRAM[N..2N-1]
     *   Verify SRAM[N..2N-1] == ~SRAM[0..N-1]
     */

    volatile uint32_t *sram_tx = sram_base;
    volatile uint32_t *sram_rx = sram_base + SRAM_TEST_WORDS;

    /* Temp buffers for verification (can't pass volatile to bench_verify) */
    uint32_t *tx_copy = (uint32_t *)malloc(SRAM_TEST_WORDS * 4);
    uint32_t *rx_copy = (uint32_t *)malloc(SRAM_TEST_WORDS * 4);
    if (!tx_copy || !rx_copy) {
        fprintf(stderr, "ERROR: malloc failed\n");
        free(tx_copy);
        free(rx_copy);
        return 1;
    }

    int total_errors = 0;

    for (int iter = 0; iter < SRAM_TEST_ITERS; iter++) {
        /* Fill TX region in SRAM */
        for (size_t i = 0; i < SRAM_TEST_WORDS; i++)
            sram_tx[i] = (uint32_t)(i + (uint32_t)iter * 0x10000);
        __sync_synchronize();

        /* Clear RX region */
        for (size_t i = 0; i < SRAM_TEST_WORDS; i++)
            sram_rx[i] = 0;
        __sync_synchronize();

        /* Pump data: SRAM → TXF0 → PIO NOT → RXF0 → SRAM
         *
         * Pure direct register access — NO piolib ioctl calls.
         * The PIO loopback program processes each word in 3 cycles
         * (15 ns at 200 MHz). A PCIe write to TX takes ~100 ns
         * (posted write), and a PCIe read from RX takes ~930 ns.
         * By the time the RX read completes, PIO has long finished.
         *
         * We process one word at a time (lockstep):
         *   1. Read word from SRAM (PCIe BAR2 read, ~930 ns)
         *   2. Write word to TXF0 (PCIe BAR1 write, posted ~100 ns)
         *   3. Read word from RXF0 (PCIe BAR1 read, ~930 ns)
         *   4. Write word to SRAM (PCIe BAR2 write, posted ~100 ns)
         *
         * PCIe ordering within the same device ensures steps 2→3
         * see processed data. Cross-BAR ordering is guaranteed by
         * the PCIe spec for ordered completions. */
        double t0 = get_time_sec();
        int stall = 0;
        for (size_t i = 0; i < SRAM_TEST_WORDS; i++) {
            uint32_t tx_val = sram_tx[i];     /* SRAM read (BAR2) */
            fifo_tx_write(tx_val);             /* FIFO write (BAR1) */
            uint32_t rx_val = fifo_rx_read();  /* FIFO read (BAR1) */
            sram_rx[i] = rx_val;               /* SRAM write (BAR2) */
        }
        __sync_synchronize();
        double t1 = get_time_sec();

        (void)stall;

        /* Copy for verification */
        for (size_t i = 0; i < SRAM_TEST_WORDS; i++) {
            tx_copy[i] = sram_tx[i];
            rx_copy[i] = sram_rx[i];
        }

        uint32_t errors = bench_verify_not(tx_copy, rx_copy, SRAM_TEST_WORDS,
                                            NULL, NULL, NULL);

        double mbps = ((double)(SRAM_TEST_WORDS * 4) / (1024.0 * 1024.0))
                      / (t1 - t0);

        if (errors > 0) {
            printf("  Iter %2d: FAIL (%u errors), %.3f MB/s\n",
                   iter, errors, mbps);
            total_errors += (int)errors;
        } else if (iter == 0 || iter == SRAM_TEST_ITERS - 1) {
            printf("  Iter %2d: PASS, %.3f MB/s\n", iter, mbps);
        }
    }

    if (total_errors == 0)
        printf("  All %d iterations PASSED\n", SRAM_TEST_ITERS);
    else
        printf("  FAILED: %d total errors across %d iterations\n",
               total_errors, SRAM_TEST_ITERS);

    /* Clean up SRAM regions */
    for (size_t i = 0; i < SRAM_TEST_WORDS * 2; i++)
        sram_base[i] = 0;
    __sync_synchronize();

    free(tx_copy);
    free(rx_copy);

    return total_errors > 0 ? 1 : 0;
}

/* ─── Signal handler for clean shutdown ───────────────────────── */

static volatile sig_atomic_t g_cleanup_needed = 0;

static void cleanup_handler(int sig)
{
    (void)sig;
    /* Try to clean up PIO state before exit */
    if (g_cleanup_needed && sm >= 0) {
        pio_sm_set_enabled(pio, (uint)sm, false);
        pio_remove_program(pio, &loopback_program, offset);
        pio_sm_unclaim(pio, (uint)sm);
        pio_close(pio);
    }
    _exit(2);
}

/* ─── Main ────────────────────────────────────────────────────── */

int main(void)
{
    /* Force unbuffered stdout for SSH visibility */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    /* Install signal handlers for clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cleanup_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    printf("fifo_probe — Direct PIO FIFO Access Probe\n");
    printf("==========================================\n\n");

    /* Step 1: Open /dev/mem and mmap PIO + SRAM */
    printf("Mapping PIO registers and SRAM via /dev/mem...\n");
    if (devmem_init() < 0)
        return 1;
    printf("  PIO registers mapped at %p\n", (void *)pio_regs);
    printf("  SRAM mapped at %p\n", (void *)sram_base);

    /* Step 2: Set up PIO via piolib */
    printf("\nSetting up PIO loopback program via piolib...\n");
    if (pio_setup() < 0) {
        devmem_cleanup();
        return 1;
    }
    g_cleanup_needed = 1;

    /* Step 3: Run tests */
    int ret = 0;
    ret |= test_single_word();
    ret |= test_polled_throughput();
    ret |= test_sram_mediated();

    /* Final verdict */
    printf("\n==========================================\n");
    if (ret == 0)
        printf("RESULT: ALL TESTS PASSED\n");
    else
        printf("RESULT: SOME TESTS FAILED\n");

    g_cleanup_needed = 0;
    pio_teardown();
    devmem_cleanup();
    return ret;
}
