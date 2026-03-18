/* sram_region_test.c — Test which SRAM regions are safe to write
 *
 * Writes a pattern to a specific SRAM region, then verifies PIO still works
 * by claiming/releasing a state machine. Restores original SRAM contents
 * after each test.
 *
 * Usage: sudo ./sram_region_test [start_offset] [size]
 *   Default: tests upper half (0x8000-0xFEFF)
 *
 * Build: gcc -Wall -Wextra -Werror -O2 -std=c11 -o sram_region_test \
 *        sram_region_test.c -I/usr/include/piolib -lpio -lm
 * Run:   sudo ./sram_region_test
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pio_platform.h>
#include <piolib.h>

#define SRAM_PHYS_ADDR   0x1F00400000ULL
#define SRAM_SIZE        0x10000       /* 64 KB */
#define SRAM_WORDS       (SRAM_SIZE / 4)

/* Default: test upper half, avoiding firmware mailbox */
#define DEFAULT_START    0x8000
#define DEFAULT_SIZE     0x7F00   /* 0x8000 to 0xFEFF = 32,512 bytes */

static volatile uint32_t *sram_base;
static int mem_fd = -1;

static int sram_mmap_init(void)
{
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "ERROR: cannot open /dev/mem: %s\n", strerror(errno));
        return -1;
    }
    sram_base = (volatile uint32_t *)mmap(NULL, SRAM_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, (off_t)SRAM_PHYS_ADDR);
    if (sram_base == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap failed: %s\n", strerror(errno));
        close(mem_fd);
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

/* Test PIO by claiming and immediately releasing a state machine */
static int test_pio_works(void)
{
    PIO pio = pio_open(0);
    if (!pio) {
        fprintf(stderr, "  PIO: pio_open failed\n");
        return -1;
    }

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        fprintf(stderr, "  PIO: pio_claim_unused_sm failed (errno=%d: %s)\n",
                errno, strerror(errno));
        pio_close(pio);
        return -1;
    }

    pio_sm_unclaim(pio, (uint)sm);
    pio_close(pio);
    return 0;
}

/* Test a specific SRAM region: write pattern, test PIO, restore */
static int test_region(unsigned start, unsigned size)
{
    if (start + size > SRAM_SIZE) {
        fprintf(stderr, "ERROR: region 0x%04x+0x%04x exceeds SRAM\n", start, size);
        return -1;
    }
    if (start >= 0xFF00) {
        fprintf(stderr, "ERROR: cannot write to firmware mailbox (0xFF00+)\n");
        return -1;
    }
    if (start + size > 0xFF00)
        size = 0xFF00 - start;

    unsigned words = size / 4;
    volatile uint32_t *region = sram_base + (start / 4);

    /* Save original contents */
    uint32_t *backup = malloc(size);
    if (!backup) {
        fprintf(stderr, "ERROR: malloc failed\n");
        return -1;
    }
    for (unsigned i = 0; i < words; i++)
        backup[i] = region[i];

    /* Write test pattern */
    printf("  Writing pattern to 0x%04x-0x%04x (%u bytes)...\n",
           start, start + size - 1, size);
    for (unsigned i = 0; i < words; i++)
        region[i] = 0xDEAD0000u | i;
    __sync_synchronize();

    /* Verify pattern was written correctly */
    unsigned write_errors = 0;
    for (unsigned i = 0; i < words; i++) {
        uint32_t expected = 0xDEAD0000u | i;
        if (region[i] != expected)
            write_errors++;
    }

    if (write_errors > 0) {
        printf("  WARNING: %u/%u words failed write verification\n",
               write_errors, words);
    }

    /* Test PIO */
    printf("  Testing PIO after write...\n");
    int pio_ok = test_pio_works();

    /* Restore original contents IMMEDIATELY */
    for (unsigned i = 0; i < words; i++)
        region[i] = backup[i];
    __sync_synchronize();

    /* Verify restore */
    unsigned restore_errors = 0;
    for (unsigned i = 0; i < words; i++) {
        if (region[i] != backup[i])
            restore_errors++;
    }

    free(backup);

    if (pio_ok == 0) {
        printf("  RESULT: SAFE (PIO works after writing to 0x%04x-0x%04x)\n",
               start, start + size - 1);
        if (restore_errors > 0)
            printf("  WARNING: %u restore errors\n", restore_errors);
        return 0;
    } else {
        printf("  RESULT: UNSAFE (PIO broken after writing to 0x%04x-0x%04x)\n",
               start, start + size - 1);
        return 1;
    }
}

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    unsigned start = DEFAULT_START;
    unsigned size = DEFAULT_SIZE;

    if (argc >= 2)
        start = (unsigned)strtoul(argv[1], NULL, 0);
    if (argc >= 3)
        size = (unsigned)strtoul(argv[2], NULL, 0);

    /* Align to 4-byte boundary */
    start &= ~3u;
    size &= ~3u;

    printf("sram_region_test — SRAM Region Safety Tester\n");
    printf("=============================================\n\n");

    /* Step 1: Verify PIO works BEFORE any writes */
    printf("Step 1: Verify PIO works before test...\n");
    if (sram_mmap_init() < 0)
        return 1;

    if (test_pio_works() != 0) {
        printf("ERROR: PIO already broken! Need power cycle.\n");
        sram_mmap_cleanup();
        return 1;
    }
    printf("  PIO: OK\n\n");

    /* Step 2: Test the requested region */
    printf("Step 2: Test region 0x%04x-0x%04x (%u bytes)...\n",
           start, start + size - 1, size);
    int ret = test_region(start, size);
    printf("\n");

    /* Step 3: Verify PIO still works after restore */
    printf("Step 3: Verify PIO works after restore...\n");
    if (test_pio_works() != 0) {
        printf("  PIO: BROKEN (even after restore!)\n");
        ret = 1;
    } else {
        printf("  PIO: OK\n");
    }

    printf("\n=============================================\n");
    if (ret == 0)
        printf("RESULT: Region 0x%04x-0x%04x is SAFE to use\n", start, start + size - 1);
    else
        printf("RESULT: Region 0x%04x-0x%04x is NOT SAFE\n", start, start + size - 1);

    sram_mmap_cleanup();
    return ret;
}
