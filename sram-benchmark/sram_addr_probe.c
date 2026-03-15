/* sram_addr_probe.c — Try different DMA addresses for SRAM
 *
 * This tool probes the kernel module to find which DMA address
 * allows the RP1 DMA controller to access shared SRAM.
 *
 * Sets up PIO loopback via piolib (same as sram_dma_bench), then
 * calls the SRAM_IOC_PROBE_ADDR ioctl for each candidate address.
 *
 * Requires: RPi5, sudo, libpio-dev, rp1_pio_sram.ko loaded
 *
 * Build: gcc -Wall -Wextra -O2 -o sram_addr_probe sram_addr_probe.c \
 *        -I/usr/include/piolib -I../benchmark -lpio -lm
 * Run:   sudo ./sram_addr_probe
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

#include "piolib.h"
#include "../benchmark/loopback.pio.h"

#define SRAM_IOC_MAGIC     'S'
#define SRAM_IOC_PROBE_ADDR _IOW(SRAM_IOC_MAGIC, 5, uint64_t)

/* Candidate DMA addresses for SRAM */
static const struct {
    uint64_t addr;
    const char *desc;
} candidates[] = {
    { 0x0000000000ULL, "DRAM CONTROL TEST (addr=0 → use host DRAM)" },
    { 0xc0401c0000ULL, "RP1_RAM_BASE (G33KatWork: 0x401c0000)" },
    { 0xc040400000ULL, "DT sram@400000 (BAR2 offset)" },
    { 0x00401c0000ULL, "RP1_RAM_BASE no 0xc0 prefix" },
    { 0x0040400000ULL, "BAR2 offset no 0xc0 prefix" },
    { 0x0020000000ULL, "M3 SRAM addr (0x20000000)" },
    { 0xc020000000ULL, "M3 SRAM addr with 0xc0 prefix" },
    { 0x001f00400000ULL, "CPU phys BAR2 (0x1F00400000)" },
};
#define N_CANDIDATES (sizeof(candidates) / sizeof(candidates[0]))

/* Global PIO state for cleanup */
static PIO pio;
static int sm = -1;
static uint pio_offset;

static int setup_pio_loopback(void)
{
    pio = pio_open(0);
    if (!pio) {
        fprintf(stderr, "ERROR: pio_open(0) failed\n");
        return -1;
    }

    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        fprintf(stderr, "ERROR: no free state machines\n");
        pio_close(pio);
        pio = NULL;
        return -1;
    }

    pio_offset = pio_add_program(pio, &loopback_program);
    if (pio_offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "ERROR: failed to load PIO program\n");
        pio_sm_unclaim(pio, (uint)sm);
        pio_close(pio);
        pio = NULL;
        sm = -1;
        return -1;
    }

    /* Configure: 32-bit autopull/autopush, 200 MHz (clkdiv=1) */
    pio_sm_config c = loopback_program_get_default_config(pio_offset);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, (uint)sm, pio_offset, &c);
    pio_sm_set_enabled(pio, (uint)sm, true);

    printf("  PIO SM%d: loopback program loaded at offset %u\n", sm, pio_offset);
    return 0;
}

static void pio_teardown(void)
{
    if (sm >= 0) {
        pio_sm_set_enabled(pio, (uint)sm, false);
        pio_remove_program(pio, &loopback_program, pio_offset);
        pio_sm_unclaim(pio, (uint)sm);
        sm = -1;
    }
    if (pio) {
        pio_close(pio);
        pio = NULL;
    }
}

int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("sram_addr_probe — Find correct DMA address for RP1 SRAM\n");
    printf("=========================================================\n\n");

    /* Set up PIO loopback */
    printf("Setting up PIO loopback...\n");
    if (setup_pio_loopback() < 0)
        return 1;

    /* Open kernel module */
    int fd = open("/dev/rp1_pio_sram", O_RDWR);
    if (fd < 0) {
        perror("open /dev/rp1_pio_sram");
        pio_teardown();
        return 1;
    }

    printf("\nProbing %zu candidate addresses...\n\n", N_CANDIDATES);

    for (size_t i = 0; i < N_CANDIDATES; i++) {
        uint64_t addr = candidates[i].addr;
        printf("[%zu] addr=0x%012llx  %s\n",
               i, (unsigned long long)addr, candidates[i].desc);

        int ret = ioctl(fd, SRAM_IOC_PROBE_ADDR, &addr);
        if (ret == 0) {
            printf("    >>> SUCCESS — RX data changed!\n");
        } else {
            printf("    --- failed (ret=%d)\n", ret);
        }
        printf("\n");

        /* Small delay between probes */
        usleep(100000);
    }

    close(fd);
    pio_teardown();
    printf("Done. Check dmesg for detailed results.\n");
    return 0;
}
