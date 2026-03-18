/* sram_probe.c — RP1 Shared SRAM probe and bandwidth measurement
 *
 * Validates SRAM access via /dev/mem mmap of BAR2, runs write/readback
 * verification with multiple patterns, measures PCIe bandwidth to SRAM.
 * Only writes to the safe region (0x8B00-0xFEFF) — avoids firmware code
 * (0x0000-0x7FFF), PIO descriptors (0x8A00-0x8AFF), and mailbox (0xFF00+).
 * Saves and restores original SRAM contents to be non-destructive.
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

/*
 * Safe writable region: 0x8B00–0xFEFF (29,952 bytes)
 *
 * SRAM layout (measured via sram_region_test):
 *   0x0000-0x7FFF  Firmware code/data — DO NOT WRITE
 *   0x8000-0x89FF  Safe (2,560 B) — but small, skip for simplicity
 *   0x8A00-0x8AFF  PIO descriptors — DO NOT WRITE
 *   0x8B00-0xFEFF  Safe (29,952 B) — primary test region
 *   0xFF00-0xFFFF  Firmware mailbox — DO NOT WRITE
 */
#define SAFE_OFFSET      0x8B00
#define SAFE_SIZE        0x7400        /* 29,696 bytes (rounded down to 4-align) */
#define SAFE_WORDS       (SAFE_SIZE / 4)  /* 7,424 words */

/* Firmware regions to verify are untouched */
#define FW_CODE_OFFSET   0x0000
#define FW_CODE_SIZE     0x0100        /* First 256 bytes of vector table */
#define FW_CODE_WORDS    (FW_CODE_SIZE / 4)

#define PIO_DESC_OFFSET  0x8A00
#define PIO_DESC_SIZE    0x0100        /* 256 bytes */
#define PIO_DESC_WORDS   (PIO_DESC_SIZE / 4)

#define FW_MBOX_OFFSET   0xFF00
#define FW_MBOX_SIZE     0x0100        /* 256 bytes */
#define FW_MBOX_WORDS    (FW_MBOX_SIZE / 4)

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
    uint32_t (*generate)(unsigned index, uint32_t seed);
} pattern_t;

static uint32_t gen_zeros(unsigned index, uint32_t seed)
{
    (void)index; (void)seed;
    return 0x00000000u;
}

static uint32_t gen_ones(unsigned index, uint32_t seed)
{
    (void)index; (void)seed;
    return 0xFFFFFFFFu;
}

static uint32_t gen_walking_one(unsigned index, uint32_t seed)
{
    (void)seed;
    return 1u << (index % 32);
}

static uint32_t gen_sequential(unsigned index, uint32_t seed)
{
    return index + seed;
}

static uint32_t gen_random(unsigned index, uint32_t seed)
{
    uint32_t x = index ^ seed ^ 0xDEADBEEFu;
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

/* Backup of the safe region's original contents */
static uint32_t *safe_backup;

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

    /* Save original contents of the safe region */
    safe_backup = malloc(SAFE_SIZE);
    if (!safe_backup) {
        fprintf(stderr, "ERROR: malloc failed\n");
        munmap((void *)sram_base, SRAM_SIZE);
        close(mem_fd);
        return -1;
    }

    volatile uint32_t *safe = sram_base + (SAFE_OFFSET / 4);
    for (unsigned i = 0; i < SAFE_WORDS; i++)
        safe_backup[i] = safe[i];

    return 0;
}

static void sram_restore_and_cleanup(void)
{
    /* Restore original contents */
    if (safe_backup && sram_base && sram_base != MAP_FAILED) {
        volatile uint32_t *safe = sram_base + (SAFE_OFFSET / 4);
        for (unsigned i = 0; i < SAFE_WORDS; i++)
            safe[i] = safe_backup[i];
        __sync_synchronize();
    }

    free(safe_backup);
    safe_backup = NULL;

    if (sram_base && sram_base != MAP_FAILED)
        munmap((void *)sram_base, SRAM_SIZE);
    if (mem_fd >= 0)
        close(mem_fd);
}

/* ─── Test: Write/Readback verification ───────────────────────── */

static int test_write_readback(void)
{
    printf("=== SRAM Write/Readback Verification ===\n");
    printf("  Region: 0x%04X-0x%04X (%u bytes, %u words)\n\n",
           SAFE_OFFSET, SAFE_OFFSET + SAFE_SIZE - 1, SAFE_SIZE, SAFE_WORDS);

    volatile uint32_t *safe = sram_base + (SAFE_OFFSET / 4);
    int all_pass = 1;

    for (unsigned p = 0; p < NUM_PATTERNS; p++) {
        const pattern_t *pat = &patterns[p];

        /* Write pattern */
        for (unsigned i = 0; i < SAFE_WORDS; i++)
            safe[i] = pat->generate(i, 0x12345678u);
        __sync_synchronize();

        /* Readback and verify */
        uint32_t errors = 0;
        unsigned first_mismatch = 0;
        uint32_t first_expected = 0, first_actual = 0;

        for (unsigned i = 0; i < SAFE_WORDS; i++) {
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
            printf("  %-14s  PASS  (%u words verified)\n",
                   pat->name, SAFE_WORDS);
        } else {
            printf("  %-14s  FAIL  %u errors / %u words\n",
                   pat->name, errors, SAFE_WORDS);
            printf("    first mismatch at word %u (0x%04X): expected 0x%08x, got 0x%08x\n",
                   first_mismatch, SAFE_OFFSET + first_mismatch * 4,
                   first_expected, first_actual);
            all_pass = 0;
        }
    }

    printf("\n");
    return all_pass ? 0 : 1;
}

/* ─── Test: Bandwidth measurement ─────────────────────────────── */

#define BW_ITERATIONS 100

static int test_bandwidth(void)
{
    printf("=== SRAM Bandwidth Measurement ===\n\n");

    volatile uint32_t *safe = sram_base + (SAFE_OFFSET / 4);

    /* ── Write bandwidth ── */
    double write_times[BW_ITERATIONS];
    for (int iter = 0; iter < BW_ITERATIONS; iter++) {
        double t0 = get_time_sec();
        for (unsigned i = 0; i < SAFE_WORDS; i++)
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
        for (unsigned i = 0; i < SAFE_WORDS; i++)
            sink += safe[i];
        __sync_synchronize();
        double t1 = get_time_sec();
        read_times[iter] = t1 - t0;
    }
    (void)sink;

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
        double wbw = (double)SAFE_SIZE / write_times[i] / (1024.0 * 1024.0);
        double rbw = (double)SAFE_SIZE / read_times[i] / (1024.0 * 1024.0);

        if (wbw < write_min) write_min = wbw;
        if (wbw > write_max) write_max = wbw;
        write_sum += wbw;

        if (rbw < read_min) read_min = rbw;
        if (rbw > read_max) read_max = rbw;
        read_sum += rbw;
    }

    double write_mean = write_sum / BW_ITERATIONS;
    double read_mean = read_sum / BW_ITERATIONS;

    printf("  Host -> SRAM write:\n");
    printf("    min: %8.2f MB/s  mean: %8.2f MB/s  max: %8.2f MB/s\n",
           write_min, write_mean, write_max);
    printf("  SRAM -> Host read:\n");
    printf("    min: %8.2f MB/s  mean: %8.2f MB/s  max: %8.2f MB/s\n",
           read_min, read_mean, read_max);
    printf("  Single-word latency:\n");
    printf("    write: %.0f ns    read: %.0f ns\n",
           write_latency_ns, read_latency_ns);

    printf("\n");
    return 0;
}

/* ─── Test: Firmware safety check ─────────────────────────────── */

static int test_firmware_safety(void)
{
    printf("=== Firmware Region Safety Check ===\n\n");

    int ret = 0;

    /* Check vector table (first 256 bytes) */
    volatile uint32_t *vt = sram_base + (FW_CODE_OFFSET / 4);
    uint32_t isp = vt[0];  /* Initial stack pointer */
    uint32_t reset = vt[1]; /* Reset vector */
    printf("  Vector table:  ISP=0x%08x  Reset=0x%08x", isp, reset);
    if ((reset & 0xFFF00000) == 0x20000000)
        printf("  (valid SRAM)\n");
    else {
        printf("  (UNEXPECTED!)\n");
        ret = 1;
    }

    /* Check PIO descriptors */
    volatile uint32_t *pio = sram_base + (PIO_DESC_OFFSET / 4);
    uint32_t pio_magic = pio[1];  /* Should be "PIO " = 0x50494f20 */
    printf("  PIO desc:      magic=0x%08x", pio_magic);
    if (pio_magic == 0x50494f20)
        printf("  (\"PIO \" OK)\n");
    else {
        printf("  (UNEXPECTED! Expected 0x50494f20)\n");
        ret = 1;
    }

    /* Check mailbox */
    volatile uint32_t *mb = sram_base + (FW_MBOX_OFFSET / 4);
    printf("  Mailbox:       ");
    for (int i = 0; i < 4; i++)
        printf("%08x ", mb[i]);
    printf("\n");

    /* Check magic sentinels at end of SRAM */
    uint32_t magic1 = sram_base[0xFFF8 / 4];
    uint32_t magic2 = sram_base[0xFFFC / 4];
    printf("  Footer magic:  0x%08x 0x%08x", magic1, magic2);
    if (magic1 == 0x5A55AA51 && magic2 == 0x5A55AA55)
        printf("  (OK)\n");
    else {
        printf("  (UNEXPECTED!)\n");
        ret = 1;
    }

    printf("\n");
    return ret;
}

/* ─── Main ────────────────────────────────────────────────────── */

int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("sram_probe — RP1 Shared SRAM Verification Tool\n");
    printf("================================================\n\n");

    /* Step 1: mmap SRAM */
    printf("Mapping SRAM at physical address 0x%llx (%u bytes)...\n",
           (unsigned long long)SRAM_PHYS_ADDR, SRAM_SIZE);

    if (sram_mmap_init() < 0)
        return 1;

    printf("  SRAM mapped successfully at %p\n\n", (void *)sram_base);

    /* Step 2: Run tests */
    int ret = 0;

    ret |= test_firmware_safety();
    ret |= test_write_readback();
    ret |= test_bandwidth();

    /* Step 3: Re-check firmware safety after write tests */
    printf("=== Post-Test Firmware Check ===\n\n");
    volatile uint32_t *pio = sram_base + (PIO_DESC_OFFSET / 4);
    uint32_t pio_magic = pio[1];
    volatile uint32_t *vt = sram_base;
    uint32_t reset = vt[1];
    printf("  Vector table Reset: 0x%08x  PIO magic: 0x%08x\n",
           reset, pio_magic);
    if ((reset & 0xFFF00000) != 0x20000000 || pio_magic != 0x50494f20) {
        printf("  ERROR: Firmware regions corrupted!\n");
        ret = 1;
    } else {
        printf("  OK: Firmware regions intact\n");
    }
    printf("\n");

    /* Final verdict */
    printf("================================================\n");
    if (ret == 0)
        printf("RESULT: ALL TESTS PASSED\n");
    else
        printf("RESULT: SOME TESTS FAILED\n");

    sram_restore_and_cleanup();
    return ret;
}
