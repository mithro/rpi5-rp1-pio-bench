/* sram_probe.c — RP1 Shared SRAM probe and bandwidth measurement
 *
 * Validates SRAM access via /dev/mem mmap of BAR2, runs write/readback
 * verification with multiple patterns, measures PCIe bandwidth to SRAM,
 * and verifies the firmware region (0xFF00-0xFFFF) is untouched.
 *
 * Requires: RPi5, sudo (for /dev/mem access)
 *
 * Build: gcc -Wall -Wextra -Werror -O2 -std=c11 -o sram_probe sram_probe.c -lm
 * Run:   sudo ./sram_probe
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
#include <unistd.h>

/* ─── RP1 SRAM constants ──────────────────────────────────────── */

/* BCM2712 physical address of RP1 shared SRAM (BAR2) */
#define SRAM_PHYS_ADDR   0x1F00400000ULL
#define SRAM_SIZE        0x10000       /* 64 KB */

/* Safe region: 0x0000–0xFEFF (firmware uses 0xFF00–0xFFFF) */
#define SRAM_SAFE_OFFSET 0x0000
#define SRAM_SAFE_SIZE   0xFF00        /* 65,280 bytes */
#define SRAM_SAFE_WORDS  (SRAM_SAFE_SIZE / 4)  /* 16,320 words */

/* Firmware mailbox: 0xFF00–0xFFFF (256 bytes, DO NOT WRITE) */
#define FW_REGION_OFFSET 0xFF00
#define FW_REGION_SIZE   0x100         /* 256 bytes */
#define FW_REGION_WORDS  (FW_REGION_SIZE / 4)  /* 64 words */

/* ─── Timing ──────────────────────────────────────────────────── */

static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ─── Test patterns ───────────────────────────────────────────── */

typedef struct {
    const char *name;
    uint32_t (*generate)(size_t index, uint32_t seed);
} pattern_t;

static uint32_t gen_zeros(size_t index, uint32_t seed)
{
    (void)index; (void)seed;
    return 0x00000000u;
}

static uint32_t gen_ones(size_t index, uint32_t seed)
{
    (void)index; (void)seed;
    return 0xFFFFFFFFu;
}

static uint32_t gen_walking_one(size_t index, uint32_t seed)
{
    (void)seed;
    return 1u << (index % 32);
}

static uint32_t gen_sequential(size_t index, uint32_t seed)
{
    return (uint32_t)index + seed;
}

static uint32_t gen_random(size_t index, uint32_t seed)
{
    /* Simple xorshift32 PRNG, seeded by index + seed */
    uint32_t x = (uint32_t)index ^ seed ^ 0xDEADBEEFu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static const pattern_t patterns[] = {
    {"all-zeros",    gen_zeros},
    {"all-ones",     gen_ones},
    {"walking-one",  gen_walking_one},
    {"sequential",   gen_sequential},
    {"random",       gen_random},
};

#define NUM_PATTERNS (sizeof(patterns) / sizeof(patterns[0]))

/* ─── SRAM mmap ───────────────────────────────────────────────── */

static volatile uint32_t *sram_base;
static int mem_fd = -1;

static int sram_mmap_init(void)
{
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "ERROR: cannot open /dev/mem: %s\n", strerror(errno));
        fprintf(stderr, "  (run with sudo)\n");
        return -1;
    }

    sram_base = (volatile uint32_t *)mmap(NULL, SRAM_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, mem_fd,
                                           (off_t)SRAM_PHYS_ADDR);
    if (sram_base == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap SRAM at 0x%llx failed: %s\n",
                (unsigned long long)SRAM_PHYS_ADDR, strerror(errno));
        close(mem_fd);
        mem_fd = -1;
        return -1;
    }

    return 0;
}

static void sram_mmap_cleanup(void)
{
    if (sram_base && sram_base != MAP_FAILED)
        munmap((void *)sram_base, SRAM_SIZE);
    if (mem_fd >= 0)
        close(mem_fd);
}

/* ─── Test: Write/Readback verification ───────────────────────── */

static int test_write_readback(void)
{
    printf("=== SRAM Write/Readback Verification ===\n\n");

    volatile uint32_t *safe = sram_base + (SRAM_SAFE_OFFSET / 4);
    int all_pass = 1;

    for (size_t p = 0; p < NUM_PATTERNS; p++) {
        const pattern_t *pat = &patterns[p];

        /* Write pattern */
        for (size_t i = 0; i < SRAM_SAFE_WORDS; i++)
            safe[i] = pat->generate(i, 0x12345678u);

        /* Memory barrier to ensure all writes complete */
        __sync_synchronize();

        /* Readback and verify */
        uint32_t errors = 0;
        size_t first_mismatch = 0;
        uint32_t first_expected = 0, first_actual = 0;

        for (size_t i = 0; i < SRAM_SAFE_WORDS; i++) {
            uint32_t expected = pat->generate(i, 0x12345678u);
            uint32_t actual = safe[i];
            if (actual != expected) {
                if (errors == 0) {
                    first_mismatch = i;
                    first_expected = expected;
                    first_actual = actual;
                }
                errors++;
            }
        }

        if (errors == 0) {
            printf("  %-14s  PASS  (%zu words verified)\n",
                   pat->name, SRAM_SAFE_WORDS);
        } else {
            printf("  %-14s  FAIL  %u errors / %zu words\n",
                   pat->name, errors, SRAM_SAFE_WORDS);
            printf("    first mismatch at word %zu: expected 0x%08x, got 0x%08x\n",
                   first_mismatch, first_expected, first_actual);
            all_pass = 0;
        }
    }

    /* Clean up: zero the safe region */
    for (size_t i = 0; i < SRAM_SAFE_WORDS; i++)
        safe[i] = 0;
    __sync_synchronize();

    printf("\n");
    return all_pass ? 0 : 1;
}

/* ─── Test: Bandwidth measurement ─────────────────────────────── */

#define BW_ITERATIONS 100

static int test_bandwidth(void)
{
    printf("=== SRAM Bandwidth Measurement ===\n\n");

    volatile uint32_t *safe = sram_base + (SRAM_SAFE_OFFSET / 4);

    /* ── Write bandwidth ── */
    double write_times[BW_ITERATIONS];
    for (int iter = 0; iter < BW_ITERATIONS; iter++) {
        double t0 = get_time_sec();
        for (size_t i = 0; i < SRAM_SAFE_WORDS; i++)
            safe[i] = (uint32_t)i;
        __sync_synchronize();
        double t1 = get_time_sec();
        write_times[iter] = t1 - t0;
    }

    /* ── Read bandwidth ── */
    double read_times[BW_ITERATIONS];
    volatile uint32_t sink = 0;
    for (int iter = 0; iter < BW_ITERATIONS; iter++) {
        double t0 = get_time_sec();
        for (size_t i = 0; i < SRAM_SAFE_WORDS; i++)
            sink += safe[i];
        __sync_synchronize();
        double t1 = get_time_sec();
        read_times[iter] = t1 - t0;
    }
    (void)sink;  /* prevent optimization */

    /* ── Single-word latency ── */
    #define LATENCY_ITERS 100000
    double t0 = get_time_sec();
    for (int i = 0; i < LATENCY_ITERS; i++) {
        safe[0] = (uint32_t)i;
        __sync_synchronize();
    }
    double t1 = get_time_sec();
    double write_latency_ns = (t1 - t0) / LATENCY_ITERS * 1e9;

    t0 = get_time_sec();
    for (int i = 0; i < LATENCY_ITERS; i++) {
        sink += safe[0];
        __sync_synchronize();
    }
    t1 = get_time_sec();
    double read_latency_ns = (t1 - t0) / LATENCY_ITERS * 1e9;
    (void)sink;

    /* ── Compute stats ── */
    double write_min = 1e9, write_max = 0, write_sum = 0;
    double read_min = 1e9, read_max = 0, read_sum = 0;

    for (int i = 0; i < BW_ITERATIONS; i++) {
        double wbw = (double)SRAM_SAFE_SIZE / write_times[i] / (1024.0 * 1024.0);
        double rbw = (double)SRAM_SAFE_SIZE / read_times[i] / (1024.0 * 1024.0);

        if (wbw < write_min) write_min = wbw;
        if (wbw > write_max) write_max = wbw;
        write_sum += wbw;

        if (rbw < read_min) read_min = rbw;
        if (rbw > read_max) read_max = rbw;
        read_sum += rbw;
    }

    double write_mean = write_sum / BW_ITERATIONS;
    double read_mean = read_sum / BW_ITERATIONS;

    printf("  Host → SRAM write:\n");
    printf("    min: %8.2f MB/s  mean: %8.2f MB/s  max: %8.2f MB/s\n",
           write_min, write_mean, write_max);
    printf("  SRAM → Host read:\n");
    printf("    min: %8.2f MB/s  mean: %8.2f MB/s  max: %8.2f MB/s\n",
           read_min, read_mean, read_max);
    printf("  Single-word latency:\n");
    printf("    write: %.0f ns    read: %.0f ns\n",
           write_latency_ns, read_latency_ns);

    /* Clean up */
    for (size_t i = 0; i < SRAM_SAFE_WORDS; i++)
        safe[i] = 0;
    __sync_synchronize();

    printf("\n");
    return 0;
}

/* ─── Test: Firmware region safety ────────────────────────────── */

static int test_firmware_safety(void)
{
    printf("=== Firmware Region Safety Check ===\n\n");

    volatile uint32_t *fw = sram_base + (FW_REGION_OFFSET / 4);

    /* Save firmware region contents */
    uint32_t fw_before[FW_REGION_WORDS];
    for (size_t i = 0; i < FW_REGION_WORDS; i++)
        fw_before[i] = fw[i];

    printf("  Firmware region (0x%04X-0x%04X): %u words saved\n",
           FW_REGION_OFFSET, FW_REGION_OFFSET + FW_REGION_SIZE - 1,
           FW_REGION_WORDS);

    /* Print first few words for reference */
    printf("  First 8 words: ");
    for (size_t i = 0; i < 8 && i < FW_REGION_WORDS; i++)
        printf("%08x ", fw_before[i]);
    printf("\n");

    /* Run the write/readback test (which writes to safe region only) */
    /* Already done above, but let's verify firmware region is unchanged */

    /* Re-read and compare */
    uint32_t errors = 0;
    for (size_t i = 0; i < FW_REGION_WORDS; i++) {
        uint32_t actual = fw[i];
        if (actual != fw_before[i]) {
            if (errors == 0) {
                printf("  CORRUPTION at word %zu: was 0x%08x, now 0x%08x\n",
                       i, fw_before[i], actual);
            }
            errors++;
        }
    }

    if (errors == 0) {
        printf("  Status: PASS (firmware region unchanged)\n");
    } else {
        printf("  Status: FAIL (%u words corrupted!)\n", errors);
    }

    printf("\n");
    return errors > 0 ? 1 : 0;
}

/* ─── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("sram_probe — RP1 Shared SRAM Verification Tool\n");
    printf("================================================\n\n");

    /* Step 1: mmap SRAM */
    printf("Mapping SRAM at physical address 0x%llx (%u bytes)...\n",
           (unsigned long long)SRAM_PHYS_ADDR, SRAM_SIZE);

    if (sram_mmap_init() < 0)
        return 1;

    printf("  SRAM mapped successfully at %p\n\n", (void *)sram_base);

    /* Step 2: Save firmware region before any tests */
    volatile uint32_t *fw = sram_base + (FW_REGION_OFFSET / 4);
    uint32_t fw_snapshot[FW_REGION_WORDS];
    for (size_t i = 0; i < FW_REGION_WORDS; i++)
        fw_snapshot[i] = fw[i];

    /* Step 3: Run tests */
    int ret = 0;

    ret |= test_write_readback();
    ret |= test_bandwidth();

    /* Step 4: Verify firmware region is still intact after all tests */
    uint32_t fw_errors = 0;
    for (size_t i = 0; i < FW_REGION_WORDS; i++) {
        if (fw[i] != fw_snapshot[i])
            fw_errors++;
    }

    printf("=== Firmware Region Safety Check ===\n\n");
    printf("  Region: 0x%04X-0x%04X (%u bytes)\n",
           FW_REGION_OFFSET, FW_REGION_OFFSET + FW_REGION_SIZE - 1,
           FW_REGION_SIZE);
    printf("  First 8 words: ");
    for (size_t i = 0; i < 8 && i < FW_REGION_WORDS; i++)
        printf("%08x ", fw_snapshot[i]);
    printf("\n");

    if (fw_errors == 0) {
        printf("  Status: PASS (firmware region unchanged after all tests)\n");
    } else {
        printf("  Status: FAIL (%u words corrupted!)\n", fw_errors);
        ret = 1;
    }

    printf("\n");

    /* Final verdict */
    printf("================================================\n");
    if (ret == 0)
        printf("RESULT: ALL TESTS PASSED\n");
    else
        printf("RESULT: SOME TESTS FAILED\n");

    sram_mmap_cleanup();
    return ret;
}
