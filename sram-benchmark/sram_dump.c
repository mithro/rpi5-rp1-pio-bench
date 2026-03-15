/* sram_dump.c — Read-only dump of RP1 Shared SRAM
 *
 * Reads and displays all 64 KB of SRAM without writing anything.
 * Used to understand what the RP1 firmware stores in SRAM.
 *
 * Build: gcc -Wall -Wextra -Werror -O2 -std=c11 -o sram_dump sram_dump.c
 * Run:   sudo ./sram_dump
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

#define SRAM_PHYS_ADDR   0x1F00400000ULL
#define SRAM_SIZE        0x10000       /* 64 KB */
#define SRAM_WORDS       (SRAM_SIZE / 4)

int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

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

    printf("=== RP1 Shared SRAM Read-Only Dump ===\n\n");

    /* Scan for non-zero regions */
    printf("--- Non-zero regions ---\n");
    int in_nonzero = 0;
    unsigned region_start = 0;
    unsigned nonzero_count = 0;

    for (unsigned i = 0; i < SRAM_WORDS; i++) {
        uint32_t val = sram[i];
        if (val != 0) {
            if (!in_nonzero) {
                region_start = i;
                in_nonzero = 1;
            }
            nonzero_count++;
        } else {
            if (in_nonzero) {
                printf("  0x%04x - 0x%04x (%u words non-zero)\n",
                       region_start * 4, i * 4 - 1, i - region_start);
                in_nonzero = 0;
            }
        }
    }
    if (in_nonzero) {
        printf("  0x%04x - 0x%04x (%u words non-zero)\n",
               region_start * 4, SRAM_SIZE - 1, SRAM_WORDS - region_start);
    }

    printf("\nTotal non-zero words: %u / %u (%.1f%%)\n\n",
           nonzero_count, (unsigned)SRAM_WORDS,
           100.0 * nonzero_count / SRAM_WORDS);

    /* Hex dump of non-zero regions and their surroundings */
    printf("--- Hex dump (non-zero areas, 16 words per line) ---\n");
    for (unsigned i = 0; i < SRAM_WORDS; i += 16) {
        /* Check if any word in this 64-byte line is non-zero */
        int any_nonzero = 0;
        for (unsigned j = i; j < i + 16 && j < SRAM_WORDS; j++) {
            if (sram[j] != 0) { any_nonzero = 1; break; }
        }
        if (!any_nonzero) continue;

        printf("0x%04x: ", i * 4);
        for (unsigned j = i; j < i + 16 && j < SRAM_WORDS; j++)
            printf("%08x ", sram[j]);
        printf("\n");
    }

    /* Summary of firmware mailbox region */
    printf("\n--- Firmware mailbox (0xFF00-0xFFFF) ---\n");
    for (unsigned i = 0xFF00 / 4; i < SRAM_WORDS; i += 8) {
        printf("0x%04x: ", i * 4);
        for (unsigned j = i; j < i + 8 && j < SRAM_WORDS; j++)
            printf("%08x ", sram[j]);
        printf("\n");
    }

    munmap((void *)sram, SRAM_SIZE);
    close(fd);
    return 0;
}
