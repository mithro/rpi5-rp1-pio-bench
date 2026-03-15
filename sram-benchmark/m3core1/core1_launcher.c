/* core1_launcher.c — Load and launch RP1 M3 Core 1 firmware
 *
 * Based on MichaelBell/rp1-hacking/launch_core1/core1_test.c
 *
 * Steps:
 *   1. Map /dev/mem for BAR1 (peripherals) and BAR2 (SRAM)
 *   2. Load Core 1 firmware binary to SRAM
 *   3. Write SYSCFG scratch registers (entry point + stack pointer)
 *   4. Reset + release Core 1 (enters WFE in boot ROM)
 *   5. Patch IRQ vector table to make Core 0 issue SEV
 *   6. Monitor SRAM status word to verify Core 1 is running
 *
 * The SEV stub is placed at 0x7000 (within firmware code region,
 * known to be executable). It replaces an IRQ handler temporarily:
 * issues SEV, restores the original vector entry, and tail-calls
 * the real handler. No push/pop needed since IRQ entry hardware
 * automatically saves r0-r3, r12, lr, pc, xPSR.
 *
 * Memory layout:
 *   0x7000-0x701F: SEV launch stub (Core 0 IRQ hook, ~24 bytes)
 *   0x8A00-0x8AFF: PIO descriptors — DO NOT TOUCH
 *   0x8B00+:       Core 1 firmware (loaded from .bin file)
 *   0x8D00-0x8D3F: Core 1 status (magic + counter + PIO diag)
 *
 * Requires: RPi5, sudo, /dev/mem access
 *
 * Build: gcc -Wall -Wextra -O2 -o core1_launcher core1_launcher.c -lm
 * Run:   sudo ./core1_launcher -f sram_write_test.bin
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <time.h>
#include <getopt.h>
#include <misc/rp1_pio_if.h>

/* RP1 PCIe BAR addresses (BCM2712 physical) */
#define BAR1_PHYS   0x1f00000000ULL  /* Peripherals at 0x40000000 */
#define BAR1_SIZE   0x00400000       /* 4 MB */
#define BAR2_PHYS   0x1f00400000ULL  /* SRAM at 0x20000000 */
#define BAR2_SIZE   0x00010000       /* 64 KB */

/* RP1 internal register addresses (peripheral space, via BAR1) */
#define PROC_CTRL_SET   0x40016000   /* SET page of processor control */
#define PROC_CTRL_CLR   0x40017000   /* CLR page of processor control */
#define SYSCFG_MAGIC    0x4015400C   /* Boot magic */
#define SYSCFG_C1_PC    0x40154014   /* Core 1 PC (XOR'd) */
#define SYSCFG_C1_SP    0x4015401C   /* Core 1 SP */

/* Core 1 XOR magic for PC scratch register */
#define CORE1_PC_XOR    0x4FF83F2D
/* Boot magic value */
#define BOOT_MAGIC      0xb007c0de
/* Core 1 reset bit in PROC_CTRL */
#define CORE1_RESET_BIT (1U << 31)

/* SRAM layout */
#define STUB_OFFSET     0x7000   /* SEV stub (in firmware code region) */
#define FW_LOAD_OFFSET  0x8B00   /* Core 1 firmware load address */
#define STATUS_OFFSET   0x8D00   /* Core 1 status (must be beyond firmware end) */
#define STATUS_MAGIC    0xC0DE1234

/* SRAM base in M3 address space */
#define SRAM_M3_BASE    0x20000000

/* Vector table: IRQ59 (entry 75) has a real firmware handler.
 * This is likely the mailbox/IPC interrupt from the host.
 */
#define VECTTBL_IRQ59_OFF  0x012C  /* byte offset in vector table */

/* Mapped pointers */
static volatile uint32_t *bar1;  /* BAR1 base (peripherals) */
static volatile uint32_t *bar2;  /* BAR2 base (SRAM) */

/* Access BAR1 peripheral register (RP1 internal addr 0x4000_0000+) */
#define PERIPH(addr) (bar1[((addr) - 0x40000000) / 4])

/* Access SRAM word at byte offset (must be 4-byte aligned) */
#define SRAM(offset) (bar2[(offset) / 4])

/* Trigger a PIO mailbox transaction to fire IRQ59 on the M3.
 * Opens /dev/pio0 and does a harmless ioctl that requires firmware
 * communication — this causes the mailbox interrupt to fire.
 * Returns 0 on success, -1 on error.
 */
static int trigger_pio_mailbox(int verbose)
{
    int fd = open("/dev/pio0", O_RDWR);
    if (fd < 0) {
        if (verbose) perror("  open /dev/pio0");
        return -1;
    }

    /* Try SM_IS_CLAIMED — a harmless read-only query */
    struct rp1_pio_sm_claim_args args = { .mask = 1 };
    int ret = ioctl(fd, PIO_IOC_SM_IS_CLAIMED, &args);
    if (verbose) {
        if (ret < 0)
            perror("  ioctl SM_IS_CLAIMED");
        else
            printf("  ioctl SM_IS_CLAIMED OK (mask=%u)\n", args.mask);
    }

    close(fd);
    return ret;
}

static void setup_mmap(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        fprintf(stderr, "Run as root (sudo)?\n");
        exit(1);
    }

    bar1 = mmap(NULL, BAR1_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED, fd, BAR1_PHYS);
    if (bar1 == MAP_FAILED) {
        perror("mmap BAR1");
        exit(1);
    }

    bar2 = mmap(NULL, BAR2_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED, fd, BAR2_PHYS);
    if (bar2 == MAP_FAILED) {
        perror("mmap BAR2");
        exit(1);
    }

    close(fd);
}

/* Write a SEV stub for one IRQ handler at a given SRAM offset.
 *
 * Each stub is 12 bytes (3 words). Runs in IRQ context on Core 0.
 * Hardware saves r0-r3, lr on IRQ entry — we can freely use r0.
 *
 * Layout (12 bytes):
 *   0x00: sev                  BF40
 *   0x02: ldr r0, [pc, #4]    4801  → loads orig_handler
 *   0x04: bx  r0              4700  → tail-call original handler
 *   0x06: nop                 BF00  → alignment padding
 *   0x08: orig_handler        (4 bytes, literal pool)
 *
 * PC-relative: ldr at +0x02, PC = stub+0x06, aligned = stub+0x04
 *   target = stub+0x04 + 1*4 = stub+0x08 ✓
 */
static void write_sev_stub_at(uint32_t sram_offset, uint32_t orig_handler)
{
    uint32_t stub_words[] = {
        0x4801BF40,   /* sev; ldr r0, [pc, #4] */
        0xBF004700,   /* bx r0; nop */
        orig_handler, /* literal: original handler address */
    };

    for (size_t i = 0; i < sizeof(stub_words) / sizeof(stub_words[0]); i++) {
        SRAM(sram_offset + i * 4) = stub_words[i];
    }
}

/* Load firmware binary file to SRAM at FW_LOAD_OFFSET */
static int load_firmware(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }

    struct stat st;
    if (fstat(fileno(f), &st) < 0) {
        perror("fstat");
        fclose(f);
        return -1;
    }

    size_t size = (size_t)st.st_size;
    if (size > 0x500) {
        fprintf(stderr, "ERROR: firmware too large (%zu bytes, max 1280)\n", size);
        fclose(f);
        return -1;
    }

    printf("  Loading %s (%zu bytes) to SRAM 0x%08X\n",
           path, size, SRAM_M3_BASE + FW_LOAD_OFFSET);

    uint8_t buf[0x500];
    memset(buf, 0, sizeof(buf));
    if (fread(buf, 1, size, f) != size) {
        perror("fread");
        fclose(f);
        return -1;
    }
    fclose(f);

    for (size_t i = 0; i < (size + 3) / 4; i++) {
        uint32_t word;
        memcpy(&word, &buf[i * 4], 4);
        SRAM(FW_LOAD_OFFSET + i * 4) = word;
    }

    return 0;
}

/* Clear Core 1 status area */
static void clear_status(void)
{
    SRAM(STATUS_OFFSET + 0) = 0;
    SRAM(STATUS_OFFSET + 4) = 0;
    SRAM(STATUS_OFFSET + 8) = 0;
}

/* Check if Core 1 is running (magic word present + counter changing) */
static int check_core1_alive(void)
{
    uint32_t magic = SRAM(STATUS_OFFSET + 0);
    if (magic != STATUS_MAGIC)
        return 0;

    uint32_t c1 = SRAM(STATUS_OFFSET + 4);
    usleep(1000);
    uint32_t c2 = SRAM(STATUS_OFFSET + 4);
    return (c2 != c1);
}

/* Reset Core 1 and set up scratch registers for boot */
static void reset_and_launch_core1(void)
{
    uint32_t initial_sp = SRAM(FW_LOAD_OFFSET + 0);
    uint32_t initial_pc = SRAM(FW_LOAD_OFFSET + 4);

    printf("  Firmware SP=0x%08X PC=0x%08X\n", initial_sp, initial_pc);

    printf("  Asserting Core 1 reset...\n");
    PERIPH(PROC_CTRL_SET) = CORE1_RESET_BIT;
    usleep(100000);

    PERIPH(SYSCFG_C1_SP) = initial_sp;
    PERIPH(SYSCFG_C1_PC) = initial_pc ^ CORE1_PC_XOR;
    PERIPH(SYSCFG_MAGIC) = BOOT_MAGIC;

    printf("  SYSCFG: magic=0x%08X pc_xor=0x%08X sp=0x%08X\n",
           BOOT_MAGIC, initial_pc ^ CORE1_PC_XOR, initial_sp);

    printf("  Releasing Core 1 from reset...\n");
    PERIPH(PROC_CTRL_CLR) = CORE1_RESET_BIT;
    usleep(100000);
}

/* Scan vector table for non-BKPT IRQ handlers (both ROM and SRAM).
 * The doorbell interrupt (for host→M3 mailbox) uses a ROM handler,
 * so we MUST include ROM handlers in the scan.
 * Returns handler addresses and their vector table offsets.
 * Skips entries pointing to BKPT stubs (0x20000180-0x20000213).
 */
static int find_active_irq_handlers(uint32_t *vec_offsets, uint32_t *handlers,
                                    int max_entries)
{
    int count = 0;

    /* Scan IRQ entries (vector table offset 0x40 onwards, entries 16-79) */
    for (uint32_t off = 0x40; off < 0x140 && count < max_entries; off += 4) {
        uint32_t handler = SRAM(off);

        /* Skip default BKPT stubs (0x20000180-0x20000213 range) */
        if (handler >= 0x20000180 && handler <= 0x20000213)
            continue;

        /* Skip zero entries */
        if (handler == 0)
            continue;

        vec_offsets[count] = off;
        handlers[count] = handler;
        count++;
    }

    return count;
}

/* (Old strategy functions removed — using vector table multi-patch approach) */

static double gettime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char *argv[])
{
    const char *firmware_path = NULL;
    int monitor_secs = 5;

    int opt;
    while ((opt = getopt(argc, argv, "f:m:h")) != -1) {
        switch (opt) {
        case 'f':
            firmware_path = optarg;
            break;
        case 'm':
            monitor_secs = atoi(optarg);
            break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s -f firmware.bin [-m monitor_secs]\n"
                "  -f FILE   Core 1 firmware binary\n"
                "  -m SECS   Monitor duration (default 5)\n",
                argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!firmware_path) {
        fprintf(stderr, "ERROR: -f firmware.bin required\n");
        return 1;
    }

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("core1_launcher — RP1 M3 Core 1 bootstrap\n");
    printf("==========================================\n\n");

    /* Step 1: Map /dev/mem */
    printf("[1] Mapping /dev/mem...\n");
    setup_mmap();
    printf("  BAR1 (peripherals) mapped OK\n");
    printf("  BAR2 (SRAM) mapped OK\n\n");

    /* Step 2: Verify M3 firmware is responsive */
    printf("[2] Testing M3 firmware responsiveness...\n");
    {
        int ret = trigger_pio_mailbox(1);
        if (ret < 0) {
            fprintf(stderr, "ERROR: M3 firmware not responding (reboot needed?)\n");
            return 1;
        }
        printf("  M3 firmware is alive\n\n");
    }

    /* Step 3: Load firmware */
    printf("[3] Loading firmware...\n");
    if (load_firmware(firmware_path) < 0)
        return 1;
    printf("\n");

    /* Step 4: Clear status and set up Core 1 */
    printf("[4] Preparing Core 1 launch...\n");
    clear_status();
    reset_and_launch_core1();

    /* Verify firmware still alive after Core 1 reset */
    printf("  Checking M3 firmware after Core 1 reset...\n");
    {
        int ret = trigger_pio_mailbox(1);
        if (ret < 0) {
            fprintf(stderr, "  WARNING: M3 firmware not responding after Core 1 reset!\n");
            fprintf(stderr, "  Core 1 reset may have disrupted firmware. Reboot needed.\n");
            return 1;
        }
        printf("  M3 firmware still alive after Core 1 reset\n");
    }
    printf("\n");

    /* Step 5: Check if Core 1 started without SEV (unlikely) */
    printf("[5] Checking if Core 1 started...\n");
    usleep(500000);
    if (check_core1_alive()) {
        printf("  Core 1 is RUNNING (no SEV hook needed)!\n");
        goto monitor;
    }
    printf("  Core 1 not running yet (boot ROM WFE, need SEV)\n\n");

    /* Step 6: Issue SEV via firmware hook */
    printf("[6] Hooking Core 0 firmware to issue SEV...\n");

    /* Patch ALL active IRQ vectors (ROM + SRAM handlers) with SEV stubs.
     *
     * The M3 firmware mailbox uses a doorbell interrupt that's handled
     * by a boot ROM handler (not an SRAM handler). We need to patch
     * ALL non-BKPT vector entries to ensure we catch the doorbell IRQ.
     *
     * Each stub is 12 bytes: SEV, then tail-call the original handler.
     * Stubs are placed at STUB_OFFSET (0x7000) sequentially.
     */
    printf("\n  Scanning vector table for active IRQ handlers...\n");
    uint32_t vec_offs[16];
    uint32_t orig_handlers[16];
    int n_irqs = find_active_irq_handlers(vec_offs, orig_handlers, 16);
    printf("  Found %d active IRQ handlers:\n", n_irqs);

    for (int i = 0; i < n_irqs; i++) {
        int irq_num = (int)(vec_offs[i] - 0x40) / 4;
        const char *type = (orig_handlers[i] & 0xF0000000) == 0x10000000
                           ? "ROM" : "SRAM";
        printf("    IRQ%-2d (0x%04X) → 0x%08X [%s]\n",
               irq_num, vec_offs[i], orig_handlers[i], type);
    }

    if (n_irqs == 0) {
        fprintf(stderr, "ERROR: no active IRQ handlers found\n");
        return 1;
    }

    /* Write SEV stubs and patch vectors */
    printf("\n  Writing SEV stubs at 0x%08X...\n", SRAM_M3_BASE + STUB_OFFSET);
    for (int i = 0; i < n_irqs; i++) {
        uint32_t stub_off = STUB_OFFSET + (uint32_t)i * 12;
        uint32_t stub_m3 = SRAM_M3_BASE + stub_off;

        write_sev_stub_at(stub_off, orig_handlers[i]);

        /* Patch vector table entry (with Thumb bit) */
        SRAM(vec_offs[i]) = stub_m3 | 1;
    }
    printf("  Patched %d vector entries\n", n_irqs);

    /* Trigger PIO mailbox to fire the doorbell interrupt */
    printf("\n  Triggering PIO mailbox (doorbell → IRQ → SEV)...\n");
    for (int attempt = 0; attempt < 10; attempt++) {
        int ret = trigger_pio_mailbox(attempt < 3);
        if (ret < 0 && attempt == 0) {
            printf("  WARNING: ioctl failed, firmware may not respond\n");
        }
        usleep(100000);

        if (check_core1_alive()) {
            printf("  Core 1 started after %d PIO trigger(s)!\n", attempt + 1);

            /* Restore all vector table entries */
            for (int i = 0; i < n_irqs; i++)
                SRAM(vec_offs[i]) = orig_handlers[i];
            printf("  Restored %d vector entries\n", n_irqs);
            goto verify;
        }
    }

    /* Restore vectors on failure */
    for (int i = 0; i < n_irqs; i++)
        SRAM(vec_offs[i]) = orig_handlers[i];
    printf("  Restored %d vector entries\n", n_irqs);

    /* Diagnostic: dump status area to distinguish "never started" from "crashed" */
    fprintf(stderr, "\nERROR: SEV hook failed — Core 1 did not start\n");
    fprintf(stderr, "  Status area dump (0x%08X):\n", SRAM_M3_BASE + STATUS_OFFSET);
    for (int i = 0; i < 10; i++) {
        fprintf(stderr, "    +0x%02X: 0x%08X\n", i * 4, SRAM(STATUS_OFFSET + i * 4));
    }
    /* Also check if magic was written (Core 1 started but crashed) */
    uint32_t diag_magic = SRAM(STATUS_OFFSET + 0);
    if (diag_magic == STATUS_MAGIC)
        fprintf(stderr, "  → Magic IS present — Core 1 started but crashed!\n");
    else
        fprintf(stderr, "  → Magic NOT present (0x%08X) — Core 1 never reached _entry\n",
                diag_magic);
    return 1;

verify:
    printf("\n[7] Verifying Core 1...\n");
    usleep(200000);
    if (!check_core1_alive()) {
        printf("  magic=0x%08X counter=%u\n",
               SRAM(STATUS_OFFSET + 0), SRAM(STATUS_OFFSET + 4));
        fprintf(stderr, "ERROR: Core 1 not running\n");
        return 1;
    }
    printf("  Core 1 is RUNNING!\n");
    printf("  magic=0x%08X counter=%u\n",
           SRAM(STATUS_OFFSET + 0), SRAM(STATUS_OFFSET + 4));
    /* Diagnostic readbacks from PIO firmware */
    printf("  PIO diagnostics (step=%u):\n", SRAM(STATUS_OFFSET + 0x0C));
    printf("    tx_val=0x%08X rx_val=0x%08X exp_val=0x%08X\n",
           SRAM(STATUS_OFFSET + 0x10), SRAM(STATUS_OFFSET + 0x14),
           SRAM(STATUS_OFFSET + 0x18));
    printf("    fstat_init=0x%08X  fstat_post_tx=0x%08X\n",
           SRAM(STATUS_OFFSET + 0x1C), SRAM(STATUS_OFFSET + 0x20));
    printf("    addr_pre_pull=0x%08X  fstat_post_pull=0x%08X  addr_post_pull=0x%08X\n",
           SRAM(STATUS_OFFSET + 0x24), SRAM(STATUS_OFFSET + 0x28),
           SRAM(STATUS_OFFSET + 0x2C));
    printf("    fstat_post_push=0x%08X  addr_post_push=0x%08X\n",
           SRAM(STATUS_OFFSET + 0x30), SRAM(STATUS_OFFSET + 0x34));
    printf("    fstat_pre_rx=0x%08X\n", SRAM(STATUS_OFFSET + 0x38));
    printf("\n");

monitor:
    printf("[8] Monitoring Core 1 for %d seconds...\n", monitor_secs);
    double t_start = gettime();
    uint32_t c_start = SRAM(STATUS_OFFSET + 4);
    uint32_t c_prev = c_start;

    for (int s = 0; s < monitor_secs; s++) {
        sleep(1);
        uint32_t c_now  = SRAM(STATUS_OFFSET + 0x04);
        double elapsed  = gettime() - t_start;
        double rate     = (c_now - c_start) / elapsed;

        /* Extended status fields (PIO FIFO test firmware) */
        uint32_t result   = SRAM(STATUS_OFFSET + 0x08);

        printf("  t=%ds: counter=%u (+%u) rate=%.1f loops/sec",
               s + 1, c_now, c_now - c_prev, rate);

        if (result != 0) {
            const char *res_str = (result == 1) ? "PASS" :
                                  (result == 2) ? "FAIL" : "???";
            printf(" result=%s", res_str);
        }
        printf("\n");

        c_prev = c_now;
    }

    uint32_t c_end = SRAM(STATUS_OFFSET + 4);
    double total_time = gettime() - t_start;
    printf("\n  Total: %u loops in %.1f sec = %.1f Mloops/sec\n",
           c_end - c_start, total_time,
           (c_end - c_start) / total_time / 1e6);

    /* Final PIO status summary */
    uint32_t final_result = SRAM(STATUS_OFFSET + 0x08);
    if (final_result != 0) {
        printf("\n  PIO FIFO test: %s\n",
               (final_result == 1) ? "PASS" : "FAIL");
    }

    printf("\nDone. Core 1 continues running.\n");
    return (final_result == 2) ? 1 : 0;
}
