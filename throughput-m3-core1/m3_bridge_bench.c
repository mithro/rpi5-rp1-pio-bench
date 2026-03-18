/* m3_bridge_bench.c — M3 Core 1 SRAM↔FIFO bridge benchmark
 *
 * Measures throughput of Core 1 moving data through PIO FIFOs:
 *   Host sets up PIO SM3 with autonomous pull→NOT→push program via BAR1.
 *   Host fills TX buffer in SRAM → Core 1 writes TXF3 (SM3 auto-processes),
 *   reads RXF3, writes to RX buffer → Host verifies RX = ~TX.
 *
 * Build: gcc -Wall -Wextra -O2 -o m3_bridge_bench m3_bridge_bench.c -lm
 * Run:   sudo ./m3_bridge_bench [options]
 *
 * Options:
 *   -t SECS   Benchmark duration (default 5)
 *   -v        Enable verify mode (cmd=2)
 *   -p PAT    Pattern: seq, walk, random, fixed (default seq)
 *   -j        JSON output
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
#include <math.h>
#include <misc/rp1_pio_if.h>
#include <piolib/piolib.h>

/* RP1 PCIe BAR addresses */
#define BAR1_PHYS   0x1f00000000ULL
#define BAR1_SIZE   0x00400000
#define BAR2_PHYS   0x1f00400000ULL
#define BAR2_SIZE   0x00010000

/* SRAM layout — buffers must avoid firmware dynamic region (0x9F48-0xA150).
 * See throughput-cyclic-dma/DESIGN.md for full SRAM map. */
#define STUB_OFFSET     0x7000
#define FW_LOAD_OFFSET  0x8B00
#define STATUS_OFFSET   0x8D00
#define TX_BUF_OFFSET   0xA200    /* Past firmware dynamic region */
#define RX_BUF_OFFSET   0xB200    /* TX + 4KB */
#define BUF_WORDS       1024
#define BUF_BYTES       (BUF_WORDS * 4)

#define STATUS_MAGIC    0xC0DE1234
#define SRAM_M3_BASE    0x20000000

/* Core 1 bootstrap */
#define PROC_CTRL_SET   0x40016000
#define PROC_CTRL_CLR   0x40017000
#define SYSCFG_MAGIC    0x4015400C
#define SYSCFG_C1_PC    0x40154014
#define SYSCFG_C1_SP    0x4015401C
#define CORE1_PC_XOR    0x4FF83F2D
#define BOOT_MAGIC      0xb007c0de
#define CORE1_RESET_BIT (1U << 31)

/* Status field offsets */
#define OFF_MAGIC       0x00
#define OFF_COUNTER     0x04
#define OFF_RESULT      0x08
#define OFF_WORDS_DONE  0x0C
#define OFF_ERR_COUNT   0x10
#define OFF_ERR_IDX     0x14
#define OFF_ERR_GOT     0x18
#define OFF_ERR_EXP     0x1C
#define OFF_PASSES      0x20
#define OFF_CMD         0x24

/* PIO registers (RP1 internal addresses, via BAR1) */
#define PIO_BASE        0x40178000
#define PIO_CTRL        (PIO_BASE + 0x00)
#define PIO_FSTAT       (PIO_BASE + 0x04)
#define PIO_INSTR_MEM0  (PIO_BASE + 0x4C)  /* INSTR_MEM[n] = +0x4C + n*4 */
#define PIO_SM3_CLKDIV    (PIO_BASE + 0x12C)
#define PIO_SM3_EXECCTRL  (PIO_BASE + 0x130)
#define PIO_SM3_SHIFTCTRL (PIO_BASE + 0x134)
#define PIO_SM3_PINCTRL   (PIO_BASE + 0x140)

/* PIO instruction encodings */
#define PIO_PULL_BLOCK      0x80A0
#define PIO_MOV_ISR_NOT_OSR 0xA0CF
#define PIO_PUSH_BLOCK      0x8020

/* PIO program slot allocation (high slots to avoid kernel driver conflicts) */
#define PIO_PROG_SLOT   29  /* Instructions at slots 29, 30, 31 */

/* SM3 control bits in PIO_CTRL */
#define SM3_ENABLE      (1U << 3)
#define SM3_RESTART     (1U << 7)
#define SM3_CLKDIV_RST  (1U << 11)

static volatile uint32_t *bar1;
static volatile uint32_t *bar2;

#define PERIPH(addr) (bar1[((addr) - 0x40000000) / 4])
#define SRAM(offset) (bar2[(offset) / 4])

static double gettime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void setup_mmap(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        fprintf(stderr, "Run as root (sudo)?\n");
        exit(1);
    }
    bar1 = mmap(NULL, BAR1_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BAR1_PHYS);
    bar2 = mmap(NULL, BAR2_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BAR2_PHYS);
    if (bar1 == MAP_FAILED || bar2 == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    close(fd);
}

static int trigger_pio_mailbox(void)
{
    int fd = open("/dev/pio0", O_RDWR);
    if (fd < 0) return -1;
    struct rp1_pio_sm_claim_args args = { .mask = 1 };
    int ret = ioctl(fd, PIO_IOC_SM_IS_CLAIMED, &args);
    close(fd);
    return ret;
}

static void write_sev_stub_at(uint32_t sram_offset, uint32_t orig_handler)
{
    uint32_t stub_words[] = {
        0x4801BF40,   /* sev; ldr r0, [pc, #4] */
        0xBF004700,   /* bx r0; nop */
        orig_handler,
    };
    for (size_t i = 0; i < 3; i++)
        SRAM(sram_offset + i * 4) = stub_words[i];
}

static int find_active_irq_handlers(uint32_t *vec_offsets, uint32_t *handlers, int max)
{
    int count = 0;
    for (uint32_t off = 0x40; off < 0x140 && count < max; off += 4) {
        uint32_t handler = SRAM(off);
        if (handler >= 0x20000180 && handler <= 0x20000213) continue;
        if (handler == 0) continue;
        vec_offsets[count] = off;
        handlers[count] = handler;
        count++;
    }
    return count;
}

static int load_firmware(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    struct stat st;
    fstat(fileno(f), &st);
    size_t size = (size_t)st.st_size;
    if (size > 0x500) {
        fprintf(stderr, "ERROR: firmware too large (%zu bytes)\n", size);
        fclose(f);
        return -1;
    }
    uint8_t buf[0x500];
    memset(buf, 0, sizeof(buf));
    if (fread(buf, 1, size, f) != size) { perror("fread"); fclose(f); return -1; }
    fclose(f);
    for (size_t i = 0; i < (size + 3) / 4; i++) {
        uint32_t word;
        memcpy(&word, &buf[i * 4], 4);
        SRAM(FW_LOAD_OFFSET + i * 4) = word;
    }
    printf("  Loaded %s (%zu bytes)\n", path, size);
    return 0;
}

static int check_core1_alive(void)
{
    if (SRAM(STATUS_OFFSET + OFF_MAGIC) != STATUS_MAGIC) return 0;
    uint32_t c1 = SRAM(STATUS_OFFSET + OFF_COUNTER);
    usleep(1000);
    uint32_t c2 = SRAM(STATUS_OFFSET + OFF_COUNTER);
    return (c2 != c1);
}

/* Fill TX buffer with test pattern */
static void fill_tx_buffer(const char *pattern, uint32_t seed)
{
    for (uint32_t i = 0; i < BUF_WORDS; i++) {
        uint32_t val;
        if (strcmp(pattern, "seq") == 0) {
            val = seed + i;
        } else if (strcmp(pattern, "walk") == 0) {
            val = 1U << (i % 32);
        } else if (strcmp(pattern, "random") == 0) {
            /* Simple xorshift32 */
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            val = seed;
        } else {
            /* fixed pattern */
            val = 0xDEADBEEF;
        }
        SRAM(TX_BUF_OFFSET + i * 4) = val;
    }
}

/* Verify RX buffer = ~TX buffer (host-side verification) */
static int verify_rx_buffer(uint32_t *first_err_idx, uint32_t *got, uint32_t *exp)
{
    int errors = 0;
    for (uint32_t i = 0; i < BUF_WORDS; i++) {
        uint32_t tx = SRAM(TX_BUF_OFFSET + i * 4);
        uint32_t rx = SRAM(RX_BUF_OFFSET + i * 4);
        uint32_t expected = ~tx;
        if (rx != expected) {
            if (errors == 0) {
                *first_err_idx = i;
                *got = rx;
                *exp = expected;
            }
            errors++;
        }
    }
    return errors;
}

/* Global PIO handle — must stay open during benchmark */
static PIO g_pio;
static uint g_sm;
static uint g_prog_offset;

/* Set up PIO SM3 with autonomous pull→NOT→push program via piolib.
 * Uses the kernel driver (which communicates with M3 Core 0 firmware)
 * to properly load the program into instruction memory.
 */
static int setup_pio_sm3(int verbose)
{
    /* Open PIO device */
    g_pio = pio_open(0);
    if (PIO_IS_ERR(g_pio)) {
        fprintf(stderr, "ERROR: pio_open(0) failed: %d\n", PIO_ERR_VAL(g_pio));
        return -1;
    }
    if (verbose) printf("  Opened /dev/pio0\n");

    /* Claim SM3 specifically (Core 1 firmware uses TXF3/RXF3) */
    g_sm = 3;
    pio_sm_claim(g_pio, g_sm);
    if (verbose) printf("  Claimed SM%u\n", g_sm);

    /* Load PIO program: pull block → mov isr, ~osr → push block */
    static const uint16_t prog_insns[] = {
        0x80A0,  /* pull block */
        0xA0CF,  /* mov isr, ~osr */
        0x8020,  /* push block */
    };
    static const pio_program_t prog = {
        .instructions = prog_insns,
        .length = 3,
        .origin = -1,  /* let piolib choose the slot */
        .pio_version = 0,
    };
    g_prog_offset = pio_add_program(g_pio, &prog);
    if (verbose)
        printf("  Program loaded at INSTR_MEM offset %u (slots %u-%u)\n",
               g_prog_offset, g_prog_offset, g_prog_offset + 2);

    /* Configure SM3 */
    pio_sm_config c = pio_get_default_sm_config_for_pio(g_pio);
    sm_config_set_wrap(&c, g_prog_offset, g_prog_offset + 2);

    /* Initialize SM3 (sets PC to initial_pc, applies config) */
    pio_sm_init(g_pio, g_sm, g_prog_offset, &c);
    if (verbose) printf("  SM%u initialized (PC=%u, wrap %u-%u)\n",
                        g_sm, g_prog_offset, g_prog_offset, g_prog_offset + 2);

    /* Enable SM3 */
    pio_sm_set_enabled(g_pio, g_sm, true);
    if (verbose) printf("  SM%u enabled and running\n", g_sm);

    return 0;
}

static int launch_core1(int verbose)
{
    /* Clear status (but not CMD — set that separately) */
    for (int i = 0; i < 10; i++)
        SRAM(STATUS_OFFSET + i * 4) = 0;

    /* Reset and configure Core 1 */
    uint32_t initial_sp = SRAM(FW_LOAD_OFFSET + 0);
    uint32_t initial_pc = SRAM(FW_LOAD_OFFSET + 4);
    if (verbose)
        printf("  SP=0x%08X PC=0x%08X\n", initial_sp, initial_pc);

    PERIPH(PROC_CTRL_SET) = CORE1_RESET_BIT;
    usleep(100000);
    PERIPH(SYSCFG_C1_SP) = initial_sp;
    PERIPH(SYSCFG_C1_PC) = initial_pc ^ CORE1_PC_XOR;
    PERIPH(SYSCFG_MAGIC) = BOOT_MAGIC;
    PERIPH(PROC_CTRL_CLR) = CORE1_RESET_BIT;
    usleep(100000);

    /* Check if started without SEV */
    usleep(500000);
    if (check_core1_alive()) {
        if (verbose) printf("  Core 1 started (no SEV needed)\n");
        return 0;
    }

    /* Hook IRQ vectors for SEV */
    uint32_t vec_offs[16], orig_handlers[16];
    int n_irqs = find_active_irq_handlers(vec_offs, orig_handlers, 16);
    if (n_irqs == 0) {
        fprintf(stderr, "ERROR: no active IRQ handlers\n");
        return -1;
    }
    if (verbose) printf("  Patching %d IRQ vectors for SEV...\n", n_irqs);

    for (int i = 0; i < n_irqs; i++) {
        uint32_t stub_off = STUB_OFFSET + (uint32_t)i * 12;
        write_sev_stub_at(stub_off, orig_handlers[i]);
        SRAM(vec_offs[i]) = (SRAM_M3_BASE + stub_off) | 1;
    }

    /* Trigger SEV via PIO mailbox */
    for (int attempt = 0; attempt < 10; attempt++) {
        trigger_pio_mailbox();
        usleep(100000);
        if (check_core1_alive()) {
            if (verbose) printf("  Core 1 started after %d trigger(s)\n", attempt + 1);
            for (int i = 0; i < n_irqs; i++)
                SRAM(vec_offs[i]) = orig_handlers[i];
            return 0;
        }
    }

    /* Restore vectors on failure */
    for (int i = 0; i < n_irqs; i++)
        SRAM(vec_offs[i]) = orig_handlers[i];
    fprintf(stderr, "ERROR: Core 1 failed to start\n");
    return -1;
}

int main(int argc, char *argv[])
{
    int bench_secs = 5;
    int verify = 1;
    int json = 0;
    const char *pattern = "seq";

    int opt;
    while ((opt = getopt(argc, argv, "t:vp:jh")) != -1) {
        switch (opt) {
        case 't': bench_secs = atoi(optarg); break;
        case 'v': verify = 1; break;
        case 'p': pattern = optarg; break;
        case 'j': json = 1; break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s [-t secs] [-v] [-p pattern] [-j]\n"
                "  -t SECS   Duration (default 5)\n"
                "  -v        Verify mode (Core 1 checks data)\n"
                "  -p PAT    Pattern: seq, walk, random, fixed\n"
                "  -j        JSON output\n",
                argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (!json) {
        printf("M3 Core 1 SRAM<->FIFO Bridge Benchmark\n");
        printf("=======================================\n\n");
    }

    /* Step 1: Map memory */
    if (!json) printf("[1] Mapping /dev/mem...\n");
    setup_mmap();

    /* Step 2: Check M3 firmware */
    if (!json) printf("[2] Testing M3 firmware...\n");
    if (trigger_pio_mailbox() < 0) {
        fprintf(stderr, "ERROR: M3 firmware not responding\n");
        return 1;
    }

    /* Step 3: Load bridge firmware */
    if (!json) printf("[3] Loading bridge firmware...\n");
    if (load_firmware("pio_bridge.bin") < 0)
        return 1;

    /* Step 4: Fill TX buffer with test pattern */
    if (!json) printf("[4] Filling TX buffer (pattern=%s, %d words)...\n",
                      pattern, BUF_WORDS);
    fill_tx_buffer(pattern, 0x12345678);

    /* Clear RX buffer */
    for (uint32_t i = 0; i < BUF_WORDS; i++)
        SRAM(RX_BUF_OFFSET + i * 4) = 0;

    /* Step 5: Launch Core 1 (involves trigger_pio_mailbox for SEV) */
    if (!json) printf("[5] Launching Core 1...\n");
    if (launch_core1(!json) < 0)
        return 1;

    /* Step 6: Set up PIO SM3 AFTER Core 1 launch.
     * Must be after launch_core1() because trigger_pio_mailbox()
     * causes the M3 Core 0 firmware to refresh PIO instruction memory,
     * overwriting any prior INSTR_MEM writes. */
    if (!json) printf("[6] Setting up PIO SM3 via piolib (pull->NOT->push)...\n");
    if (setup_pio_sm3(!json) < 0)
        return 1;

    /* Step 7: Send run command */
    uint32_t cmd = verify ? 2 : 1;
    if (!json) printf("[7] Starting bridge (cmd=%u, verify=%s)...\n",
                      cmd, verify ? "yes" : "no");
    SRAM(STATUS_OFFSET + OFF_CMD) = cmd;

    /* Step 8: Monitor throughput */
    if (!json) printf("[8] Benchmarking for %d seconds...\n", bench_secs);

    double t_start = gettime();
    uint32_t w_start = SRAM(STATUS_OFFSET + OFF_WORDS_DONE);

    /* Collect samples every 500ms */
    int n_samples = bench_secs * 2;
    double *rates = malloc((size_t)n_samples * sizeof(double));
    int sample_idx = 0;
    uint32_t w_prev = w_start;

    for (int i = 0; i < n_samples; i++) {
        usleep(500000);
        uint32_t w_now = SRAM(STATUS_OFFSET + OFF_WORDS_DONE);
        double t_now = gettime();
        double interval_rate = (double)(w_now - w_prev) * 4.0 / 0.5;  /* bytes/sec */
        rates[sample_idx++] = interval_rate;

        if (!json) {
            double overall_rate = (double)(w_now - w_start) * 4.0 / (t_now - t_start);
            uint32_t passes = SRAM(STATUS_OFFSET + OFF_PASSES);
            uint32_t errors = SRAM(STATUS_OFFSET + OFF_ERR_COUNT);
            printf("  t=%.1fs: %u words (%u passes) %.2f MB/s (interval %.2f MB/s) errors=%u\n",
                   t_now - t_start, w_now, passes,
                   overall_rate / 1e6, interval_rate / 1e6, errors);
        }
        w_prev = w_now;
    }

    /* Step 9: Stop Core 1 */
    SRAM(STATUS_OFFSET + OFF_CMD) = 0;
    usleep(100000);  /* Let Core 1 finish current pass */

    double t_end = gettime();
    double duration = t_end - t_start;
    uint32_t w_end = SRAM(STATUS_OFFSET + OFF_WORDS_DONE);
    uint32_t total_words = w_end - w_start;
    double total_bytes = (double)total_words * 4.0;
    double throughput = total_bytes / duration;

    /* Core 1 error count */
    uint32_t c1_errors = SRAM(STATUS_OFFSET + OFF_ERR_COUNT);
    uint32_t c1_passes = SRAM(STATUS_OFFSET + OFF_PASSES);
    (void)SRAM(STATUS_OFFSET + OFF_RESULT);

    /* Host-side verification */
    uint32_t host_err_idx = 0, host_err_got = 0, host_err_exp = 0;
    int host_errors = verify_rx_buffer(&host_err_idx, &host_err_got, &host_err_exp);

    /* Compute statistics */
    double sum = 0, sum2 = 0;
    double min_rate = 1e18, max_rate = 0;
    for (int i = 0; i < sample_idx; i++) {
        sum += rates[i];
        sum2 += rates[i] * rates[i];
        if (rates[i] < min_rate) min_rate = rates[i];
        if (rates[i] > max_rate) max_rate = rates[i];
    }
    double mean_rate = sum / sample_idx;
    double stddev = sqrt((sum2 / sample_idx) - (mean_rate * mean_rate));

    int pass = (c1_errors == 0 && host_errors == 0);

    if (json) {
        printf("{\n");
        printf("  \"test\": \"m3_bridge\",\n");
        printf("  \"result\": \"%s\",\n", pass ? "PASS" : "FAIL");
        printf("  \"pattern\": \"%s\",\n", pattern);
        printf("  \"verify\": %s,\n", verify ? "true" : "false");
        printf("  \"duration_sec\": %.3f,\n", duration);
        printf("  \"total_words\": %u,\n", total_words);
        printf("  \"total_bytes\": %.0f,\n", total_bytes);
        printf("  \"passes\": %u,\n", c1_passes);
        printf("  \"throughput_MBps\": %.3f,\n", throughput / 1e6);
        printf("  \"rate_min_MBps\": %.3f,\n", min_rate / 1e6);
        printf("  \"rate_max_MBps\": %.3f,\n", max_rate / 1e6);
        printf("  \"rate_mean_MBps\": %.3f,\n", mean_rate / 1e6);
        printf("  \"rate_stddev_MBps\": %.3f,\n", stddev / 1e6);
        printf("  \"c1_errors\": %u,\n", c1_errors);
        printf("  \"host_errors\": %d\n", host_errors);
        printf("}\n");
    } else {
        printf("\n");
        printf("Results:\n");
        printf("  Pattern:        %s\n", pattern);
        printf("  Duration:       %.3f sec\n", duration);
        printf("  Total words:    %u (%u passes of %d)\n",
               total_words, c1_passes, BUF_WORDS);
        printf("  Total bytes:    %.0f (%.1f KB)\n", total_bytes, total_bytes / 1024);
        printf("  Throughput:     %.3f MB/s\n", throughput / 1e6);
        printf("  Rate min/mean/max: %.3f / %.3f / %.3f MB/s\n",
               min_rate / 1e6, mean_rate / 1e6, max_rate / 1e6);
        printf("  Rate stddev:    %.3f MB/s\n", stddev / 1e6);
        printf("  Core 1 errors:  %u\n", c1_errors);
        printf("  Host errors:    %d\n", host_errors);
        if (host_errors > 0) {
            printf("  First host err: idx=%u got=0x%08X exp=0x%08X\n",
                   host_err_idx, host_err_got, host_err_exp);
        }
        if (c1_errors > 0) {
            printf("  First C1 err:   idx=%u got=0x%08X exp=0x%08X\n",
                   SRAM(STATUS_OFFSET + OFF_ERR_IDX),
                   SRAM(STATUS_OFFSET + OFF_ERR_GOT),
                   SRAM(STATUS_OFFSET + OFF_ERR_EXP));
        }
        printf("  Result:         %s\n", pass ? "PASS" : "FAIL");

        if (throughput / 1e6 < 42.0) {
            printf("\n  WARNING: Throughput < 42 MB/s baseline — investigate!\n");
        }
    }

    free(rates);
    return pass ? 0 : 1;
}
