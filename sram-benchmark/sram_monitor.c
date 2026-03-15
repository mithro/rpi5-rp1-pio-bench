/* sram_monitor.c — Monitor RP1 SRAM for dynamic changes
 *
 * Takes two read-only snapshots of SRAM separated by a delay,
 * then reports which regions changed (actively used by firmware)
 * vs which stayed the same (static data, potentially safe).
 *
 * Build: gcc -Wall -Wextra -Werror -O2 -std=c11 -o sram_monitor sram_monitor.c
 * Run:   sudo ./sram_monitor [delay_ms]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SRAM_PHYS_ADDR   0x1F00400000ULL
#define SRAM_SIZE        0x10000       /* 64 KB */
#define SRAM_WORDS       (SRAM_SIZE / 4)

static void msleep(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    unsigned delay_ms = 1000;  /* default 1 second */
    if (argc > 1)
        delay_ms = (unsigned)atoi(argv[1]);

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open /dev/mem: %s\n", strerror(errno));
        return 1;
    }

    volatile uint32_t *sram = (volatile uint32_t *)mmap(NULL, SRAM_SIZE,
        PROT_READ, MAP_SHARED, fd, (off_t)SRAM_PHYS_ADDR);
    if (sram == MAP_FAILED) {
        fprintf(stderr, "ERROR: mmap failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    /* Snapshot 1 */
    uint32_t snap1[SRAM_WORDS];
    for (unsigned i = 0; i < SRAM_WORDS; i++)
        snap1[i] = sram[i];

    printf("Snapshot 1 taken. Waiting %u ms...\n", delay_ms);
    msleep(delay_ms);

    /* Snapshot 2 */
    uint32_t snap2[SRAM_WORDS];
    for (unsigned i = 0; i < SRAM_WORDS; i++)
        snap2[i] = sram[i];

    printf("Snapshot 2 taken.\n\n");

    /* Compare */
    printf("=== SRAM Dynamic Analysis (delay=%u ms) ===\n\n", delay_ms);

    unsigned changed_words = 0;
    int in_changed = 0;
    unsigned region_start = 0;

    printf("--- Changed regions (firmware actively writing) ---\n");
    for (unsigned i = 0; i < SRAM_WORDS; i++) {
        int changed = (snap1[i] != snap2[i]);
        if (changed) {
            changed_words++;
            if (!in_changed) {
                region_start = i;
                in_changed = 1;
            }
        } else {
            if (in_changed) {
                printf("  0x%04x - 0x%04x (%u words changed)\n",
                       region_start * 4, i * 4 - 1, i - region_start);
                /* Show first few changes */
                for (unsigned j = region_start; j < i && j < region_start + 4; j++) {
                    printf("    [0x%04x] 0x%08x -> 0x%08x\n",
                           j * 4, snap1[j], snap2[j]);
                }
                in_changed = 0;
            }
        }
    }
    if (in_changed) {
        printf("  0x%04x - 0x%04x (%u words changed)\n",
               region_start * 4, SRAM_SIZE - 1, SRAM_WORDS - region_start);
    }

    printf("\nTotal changed: %u / %u words (%.1f%%)\n\n",
           changed_words, (unsigned)SRAM_WORDS,
           100.0 * changed_words / SRAM_WORDS);

    /* Now take a 3rd snapshot after another delay to check consistency */
    printf("Taking snapshot 3 after another %u ms...\n", delay_ms);
    msleep(delay_ms);

    uint32_t snap3[SRAM_WORDS];
    for (unsigned i = 0; i < SRAM_WORDS; i++)
        snap3[i] = sram[i];

    unsigned changed_23 = 0;
    for (unsigned i = 0; i < SRAM_WORDS; i++) {
        if (snap2[i] != snap3[i])
            changed_23++;
    }

    printf("Snap2 vs Snap3: %u words changed\n\n", changed_23);

    /* Categorize regions */
    printf("--- Region Summary ---\n");
    printf("  Static non-zero: firmware code/data loaded at boot\n");
    printf("  Dynamic: firmware actively reading/writing\n");
    printf("  Always zero: potentially free (but may still be reserved)\n\n");

    unsigned always_zero = 0;
    unsigned static_nonzero = 0;
    unsigned dynamic = 0;

    for (unsigned i = 0; i < SRAM_WORDS; i++) {
        if (snap1[i] == 0 && snap2[i] == 0 && snap3[i] == 0) {
            always_zero++;
        } else if (snap1[i] == snap2[i] && snap2[i] == snap3[i]) {
            static_nonzero++;
        } else {
            dynamic++;
        }
    }

    printf("  Always zero:     %5u words (%5u bytes, %4.1f%%)\n",
           always_zero, always_zero * 4, 100.0 * always_zero / SRAM_WORDS);
    printf("  Static non-zero: %5u words (%5u bytes, %4.1f%%)\n",
           static_nonzero, static_nonzero * 4, 100.0 * static_nonzero / SRAM_WORDS);
    printf("  Dynamic:         %5u words (%5u bytes, %4.1f%%)\n",
           dynamic, dynamic * 4, 100.0 * dynamic / SRAM_WORDS);

    munmap((void *)sram, SRAM_SIZE);
    close(fd);
    return 0;
}
