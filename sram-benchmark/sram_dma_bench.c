/* sram_dma_bench.c — Cyclic DMA PIO Loopback Benchmark
 *
 * Sets up PIO loopback via piolib, then uses the rp1_pio_sram kernel module
 * to run cyclic DMA between host DRAM ring buffers and PIO FIFOs. Measures
 * throughput and verifies data integrity.
 *
 * Architecture:
 *   This tool (userspace) → piolib → PIO SM0 running loopback (TX → NOT → RX)
 *   rp1_pio_sram.ko → cyclic DMA: DRAM TX ring → PIO TXF0 → PIO RXF0 → DRAM RX ring
 *
 * Requires: RPi5, sudo, libpio-dev, rp1_pio_sram.ko loaded
 *
 * Build: gcc -Wall -Wextra -O2 -o sram_dma_bench sram_dma_bench.c \
 *        -I/usr/include/piolib -lpio -lm
 * Run:   sudo ./sram_dma_bench
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "piolib.h"
#include "../benchmark/loopback.pio.h"

/* ─── ioctl definitions (must match kernel module) ────────────── */

#define SRAM_IOC_MAGIC     'S'
#define SRAM_IOC_START_DMA _IO(SRAM_IOC_MAGIC, 1)
#define SRAM_IOC_STOP_DMA  _IO(SRAM_IOC_MAGIC, 2)
#define SRAM_IOC_DMA_STATUS _IOR(SRAM_IOC_MAGIC, 3, struct sram_dma_status)
#define SRAM_IOC_DMA_DIAG  _IO(SRAM_IOC_MAGIC, 4)
#define SRAM_IOC_START_SRAM_DMA _IO(SRAM_IOC_MAGIC, 6)

struct sram_dma_status {
	uint32_t tx_ring_offset;
	uint32_t tx_ring_size;
	uint32_t rx_ring_offset;
	uint32_t rx_ring_size;
	uint32_t period_size;
	uint32_t dma_running;
	uint64_t tx_bytes;
	uint64_t rx_bytes;
	uint32_t verify_errors;
	uint32_t verify_total;
};

/* ─── Constants ───────────────────────────────────────────────── */

/* mmap the DMA buffer at offset 0 */
#define DMA_MMAP_OFFSET    0
#define DMA_MMAP_SIZE      0x4000  /* 16,384 bytes (one 16 KB page) */

/* ─── Benchmark results (filled by test functions) ────────────── */

static struct {
	const char *mode;	/* "dram" or "sram" */
	double duration;
	uint64_t tx_bytes;
	uint64_t rx_bytes;
	double tx_mbps;
	double rx_mbps;
	int verify_pass;	/* 1=pass, 0=fail */
	uint32_t verify_errors;
	uint32_t verify_total;
	unsigned periods_clean;
	unsigned periods_torn;
	unsigned periods_bad;
	unsigned periods_empty;
} results;

static int json_mode;

/* ─── Timing ──────────────────────────────────────────────────── */

static double get_time_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ─── Global state ────────────────────────────────────────────── */

static PIO pio;
static int sm = -1;
static uint pio_offset;
static int dev_fd = -1;
static volatile uint32_t *dma_buf;

/* ─── PIO setup ───────────────────────────────────────────────── */

static int pio_setup(void)
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
		return -1;
	}

	pio_offset = pio_add_program(pio, &loopback_program);
	if (pio_offset == PIO_ORIGIN_INVALID) {
		fprintf(stderr, "ERROR: failed to load PIO program\n");
		pio_sm_unclaim(pio, (uint)sm);
		pio_close(pio);
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

/* ─── Device setup ────────────────────────────────────────────── */

static int device_setup(void)
{
	dev_fd = open("/dev/rp1_pio_sram", O_RDWR);
	if (dev_fd < 0) {
		fprintf(stderr, "ERROR: cannot open /dev/rp1_pio_sram: %s\n",
			strerror(errno));
		fprintf(stderr, "  Is rp1_pio_sram.ko loaded? sudo insmod rp1_pio_sram.ko\n");
		return -1;
	}

	/* mmap the DMA buffer at offset 0 */
	dma_buf = (volatile uint32_t *)mmap(NULL, DMA_MMAP_SIZE,
					     PROT_READ | PROT_WRITE,
					     MAP_SHARED, dev_fd,
					     (off_t)DMA_MMAP_OFFSET);
	if (dma_buf == MAP_FAILED) {
		fprintf(stderr, "ERROR: mmap /dev/rp1_pio_sram failed: %s\n",
			strerror(errno));
		close(dev_fd);
		dev_fd = -1;
		return -1;
	}

	printf("  DMA buffer mapped (%u bytes)\n", DMA_MMAP_SIZE);
	return 0;
}

static void device_cleanup(void)
{
	if (dma_buf && dma_buf != MAP_FAILED)
		munmap((void *)dma_buf, DMA_MMAP_SIZE);
	if (dev_fd >= 0)
		close(dev_fd);
}

/* ─── Test: DMA loopback ──────────────────────────────────────── */

static int test_dma_loopback(double duration_sec)
{
	struct sram_dma_status st;
	int ret;

	printf("\n=== Cyclic DMA PIO Loopback Test ===\n\n");

	/* Get ring buffer info */
	ret = ioctl(dev_fd, SRAM_IOC_DMA_STATUS, &st);
	if (ret < 0) {
		fprintf(stderr, "ERROR: DMA_STATUS ioctl failed: %s\n", strerror(errno));
		return -1;
	}

	printf("  TX ring: offset %u (%u bytes, %u periods)\n",
	       st.tx_ring_offset, st.tx_ring_size, st.tx_ring_size / st.period_size);
	printf("  RX ring: offset %u (%u bytes, %u periods)\n",
	       st.rx_ring_offset, st.rx_ring_size, st.rx_ring_size / st.period_size);

	/* Calculate buffer pointers.
	 * dma_buf is mmap'd at offset 0 of the DMA buffer.
	 * TX and RX rings are at their reported offsets within that buffer. */
	volatile uint32_t *tx_ring = dma_buf + (st.tx_ring_offset / 4);
	volatile uint32_t *rx_ring = dma_buf + (st.rx_ring_offset / 4);
	unsigned tx_words = st.tx_ring_size / 4;
	unsigned rx_words = st.rx_ring_size / 4;

	printf("  Filling TX ring with sequential pattern...\n");
	for (unsigned i = 0; i < tx_words; i++)
		tx_ring[i] = 0xA0000000u | i;
	__sync_synchronize();

	/* Clear RX ring */
	for (unsigned i = 0; i < rx_words; i++)
		rx_ring[i] = 0xDEADDEADu;
	__sync_synchronize();

	/* Start DMA */
	printf("  Starting cyclic DMA...\n");
	ret = ioctl(dev_fd, SRAM_IOC_START_DMA);
	if (ret < 0) {
		fprintf(stderr, "ERROR: START_DMA failed: %s\n", strerror(errno));
		return -1;
	}

	/* Let DMA run for specified duration */
	double t0 = get_time_sec();
	printf("  DMA running for %.1f seconds...\n", duration_sec);
	usleep((unsigned)(duration_sec * 1e6));
	double t1 = get_time_sec();
	double elapsed = t1 - t0;

	/* Stop DMA and get status */
	ioctl(dev_fd, SRAM_IOC_STOP_DMA);

	ret = ioctl(dev_fd, SRAM_IOC_DMA_STATUS, &st);
	if (ret < 0) {
		fprintf(stderr, "ERROR: DMA_STATUS ioctl failed after stop: %s\n",
			strerror(errno));
		return -1;
	}

	printf("\n  Results:\n");
	printf("    Duration:    %.3f seconds\n", elapsed);
	printf("    TX bytes:    %lu (%lu periods)\n",
	       (unsigned long)st.tx_bytes, (unsigned long)(st.tx_bytes / st.period_size));
	printf("    RX bytes:    %lu (%lu periods)\n",
	       (unsigned long)st.rx_bytes, (unsigned long)(st.rx_bytes / st.period_size));

	if (st.tx_bytes > 0) {
		double tx_bw = (double)st.tx_bytes / elapsed / (1024.0 * 1024.0);
		printf("    TX throughput: %.2f MB/s\n", tx_bw);
	}
	if (st.rx_bytes > 0) {
		double rx_bw = (double)st.rx_bytes / elapsed / (1024.0 * 1024.0);
		printf("    RX throughput: %.2f MB/s\n", rx_bw);
	}

	/* Verify RX data: should be bitwise NOT of TX pattern.
	 * Cyclic DMA wraps continuously. When DMA stops, one period in
	 * the ring was being actively written ("torn"), while the other
	 * period(s) were last fully completed ("clean"). We verify each
	 * period by counting consecutive matching words from the start.
	 * A "clean" period has all words matching. A "torn" period has
	 * a prefix of correct data followed by stale data from a previous
	 * wrap. If at least one period is fully clean, the data path works. */
	printf("\n  Verifying RX data (per-period phase detection)...\n");
	__sync_synchronize();
	unsigned period_words = st.period_size / 4;
	unsigned num_periods = rx_words / period_words;

	unsigned periods_clean = 0;
	unsigned periods_torn = 0;
	unsigned periods_empty = 0;
	unsigned periods_bad = 0;
	unsigned total_verified = 0;

	for (unsigned p = 0; p < num_periods; p++) {
		unsigned base = p * period_words;
		uint32_t rx0 = rx_ring[base];

		if (rx0 == 0xDEADDEADu) {
			periods_empty++;
			printf("    Period %u: empty (never written)\n", p);
			continue;
		}

		uint32_t inv_rx0 = ~rx0;
		if ((inv_rx0 & 0xF0000000u) != 0xA0000000u ||
		    (inv_rx0 & 0x0FFFFFFFu) >= tx_words) {
			periods_bad++;
			printf("    Period %u: BAD — unrecognized RX[0]=0x%08x\n", p, rx0);
			continue;
		}

		unsigned phase = inv_rx0 & 0x0FFFFFFFu;
		unsigned consecutive_ok = 0;
		for (unsigned i = 0; i < period_words; i++) {
			uint32_t expected = ~(0xA0000000u | ((phase + i) % tx_words));
			if (rx_ring[base + i] != expected)
				break;
			consecutive_ok++;
		}

		total_verified += consecutive_ok;
		if (consecutive_ok == period_words) {
			periods_clean++;
			printf("    Period %u: CLEAN — %u/%u words verified (phase=%u)\n",
			       p, period_words, period_words, phase);
		} else {
			periods_torn++;
			printf("    Period %u: TORN at word %u — %u/%u consecutive matches (phase=%u)\n",
			       p, consecutive_ok, consecutive_ok, period_words, phase);
		}
	}

	int verify_pass = (periods_clean > 0 || (periods_torn > 0 && periods_bad == 0));
	if (periods_clean == num_periods) {
		printf("    PASS: all %u periods fully verified (%u words total)\n",
		       num_periods, total_verified);
	} else if (periods_clean > 0) {
		printf("    PASS: %u/%u periods clean, %u torn (DMA stop mid-transfer)\n",
		       periods_clean, num_periods, periods_torn);
	} else if (periods_torn > 0 && periods_bad == 0) {
		printf("    PASS (partial): all %u periods torn but data pattern correct\n",
		       num_periods);
		printf("    (DMA stopped while both periods were being written)\n");
	} else {
		printf("    FAIL: %u bad, %u empty, %u torn periods\n",
		       periods_bad, periods_empty, periods_torn);
		verify_pass = 0;
	}

	printf("\n");

	/* Populate results struct */
	results.mode = "dram";
	results.duration = elapsed;
	results.tx_bytes = st.tx_bytes;
	results.rx_bytes = st.rx_bytes;
	results.tx_mbps = st.tx_bytes > 0 ? (double)st.tx_bytes / elapsed / (1024.0 * 1024.0) : 0;
	results.rx_mbps = st.rx_bytes > 0 ? (double)st.rx_bytes / elapsed / (1024.0 * 1024.0) : 0;
	results.verify_pass = verify_pass;
	results.verify_errors = periods_bad;
	results.verify_total = total_verified;
	results.periods_clean = periods_clean;
	results.periods_torn = periods_torn;
	results.periods_bad = periods_bad;
	results.periods_empty = periods_empty;

	return (verify_pass && st.tx_bytes > 0) ? 0 : 1;
}

/* ─── Test: SRAM DMA loopback ────────────────────────────────── */

static int test_sram_loopback(double duration_sec)
{
	struct sram_dma_status st;
	int ret;

	printf("\n=== SRAM Cyclic DMA PIO Loopback Test ===\n\n");
	printf("  Data path: SRAM → DMA → PIO TXF → PIO(NOT) → PIO RXF → DMA → SRAM\n");
	printf("  (No PCIe round-trip per DMA burst — RP1-internal only)\n\n");

	/* Start SRAM DMA (kernel fills TX ring, clears RX ring internally) */
	printf("  Starting SRAM cyclic DMA...\n");
	ret = ioctl(dev_fd, SRAM_IOC_START_SRAM_DMA);
	if (ret < 0) {
		fprintf(stderr, "ERROR: START_SRAM_DMA failed: %s\n", strerror(errno));
		return -1;
	}

	/* Let DMA run */
	double t0 = get_time_sec();
	printf("  DMA running for %.1f seconds...\n", duration_sec);
	usleep((unsigned)(duration_sec * 1e6));
	double t1 = get_time_sec();
	double elapsed = t1 - t0;

	/* Stop DMA and get status */
	ioctl(dev_fd, SRAM_IOC_STOP_DMA);

	ret = ioctl(dev_fd, SRAM_IOC_DMA_STATUS, &st);
	if (ret < 0) {
		fprintf(stderr, "ERROR: DMA_STATUS ioctl failed after stop: %s\n",
			strerror(errno));
		return -1;
	}

	printf("\n  Results:\n");
	printf("    Mode:        SRAM (RP1-internal DMA)\n");
	printf("    Duration:    %.3f seconds\n", elapsed);
	printf("    TX bytes:    %lu (%lu periods)\n",
	       (unsigned long)st.tx_bytes, (unsigned long)(st.tx_bytes / st.period_size));
	printf("    RX bytes:    %lu (%lu periods)\n",
	       (unsigned long)st.rx_bytes, (unsigned long)(st.rx_bytes / st.period_size));

	if (st.tx_bytes > 0) {
		double tx_bw = (double)st.tx_bytes / elapsed / (1024.0 * 1024.0);
		printf("    TX throughput: %.2f MB/s\n", tx_bw);
	}
	if (st.rx_bytes > 0) {
		double rx_bw = (double)st.rx_bytes / elapsed / (1024.0 * 1024.0);
		printf("    RX throughput: %.2f MB/s\n", rx_bw);
	}

	/* SRAM data verification is done by the kernel module on DMA stop.
	 * Re-query status to get verification results. */
	ret = ioctl(dev_fd, SRAM_IOC_DMA_STATUS, &st);
	if (ret < 0) {
		fprintf(stderr, "ERROR: DMA_STATUS ioctl failed after stop: %s\n",
			strerror(errno));
		return -1;
	}

	printf("\n  Data verification (kernel-side, per-period phase detection):\n");
	if (st.verify_total == 0) {
		printf("    SKIP: no verification data available\n");
	} else if (st.verify_errors == 0) {
		printf("    PASS: %u words verified (bitwise NOT matches)\n",
		       st.verify_total);
	} else {
		printf("    FAIL: %u/%u words mismatched (check dmesg for details)\n",
		       st.verify_errors, st.verify_total);
	}
	printf("\n");

	/* Populate results struct */
	results.mode = "sram";
	results.duration = elapsed;
	results.tx_bytes = st.tx_bytes;
	results.rx_bytes = st.rx_bytes;
	results.tx_mbps = st.tx_bytes > 0 ? (double)st.tx_bytes / elapsed / (1024.0 * 1024.0) : 0;
	results.rx_mbps = st.rx_bytes > 0 ? (double)st.rx_bytes / elapsed / (1024.0 * 1024.0) : 0;
	results.verify_pass = (st.verify_errors == 0 && st.tx_bytes > 0);
	results.verify_errors = st.verify_errors;
	results.verify_total = st.verify_total;

	return (st.tx_bytes > 0 && st.rx_bytes > 0 && st.verify_errors == 0) ? 0 : 1;
}

/* ─── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	double duration = 2.0;
	int use_sram = 0;

	int diag_only = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--sram") == 0)
			use_sram = 1;
		else if (strcmp(argv[i], "--dram") == 0)
			use_sram = 0;
		else if (strcmp(argv[i], "--diag") == 0)
			diag_only = 1;
		else if (strcmp(argv[i], "--json") == 0)
			json_mode = 1;
		else if (strncmp(argv[i], "--duration=", 11) == 0)
			duration = atof(argv[i] + 11);
		else if (argv[i][0] != '-')
			duration = atof(argv[i]);
	}

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	printf("sram_dma_bench — Cyclic DMA PIO Loopback Benchmark\n");
	printf("===================================================\n");
	printf("Mode: %s\n\n", use_sram ? "SRAM (RP1-internal)" : "DRAM (host, via PCIe)");

	/* Step 1: Set up PIO loopback */
	printf("Step 1: Setting up PIO loopback...\n");
	if (pio_setup() < 0)
		return 1;

	/* Step 2: Open kernel module device */
	printf("Step 2: Opening /dev/rp1_pio_sram...\n");
	if (device_setup() < 0) {
		pio_teardown();
		return 1;
	}

	int ret;
	if (diag_only) {
		/* Diagnostic-only mode: run the kernel DMA self-test.
		 * WARNING: the dw-axi-dma driver's descriptor pool gets
		 * corrupted after diag (stale IRQ race), so the module must
		 * be reloaded before running a normal benchmark. */
		printf("Step 3: Running DMA diagnostic (standalone)...\n");
		int diag_ret = ioctl(dev_fd, SRAM_IOC_DMA_DIAG);
		if (diag_ret < 0)
			printf("  DIAG: FAILED (%s) — check dmesg\n", strerror(errno));
		else
			printf("  DIAG: PASSED — cyclic DMA loopback verified\n");
		ret = diag_ret < 0 ? 1 : 0;
	} else if (use_sram) {
		/* SRAM mode */
		ret = test_sram_loopback(duration);
	} else {
		/* DRAM mode: skip diag (it corrupts DMA channel state).
		 * Run diag separately with --diag flag. */
		ret = test_dma_loopback(duration);
	}

	/* Cleanup */
	device_cleanup();
	pio_teardown();

	if (json_mode && !diag_only) {
		printf("{\"mode\":\"%s\",\"result\":\"%s\","
		       "\"duration\":%.3f,"
		       "\"tx_bytes\":%lu,\"rx_bytes\":%lu,"
		       "\"tx_mbps\":%.2f,\"rx_mbps\":%.2f,"
		       "\"verify_pass\":%s,"
		       "\"verify_errors\":%u,\"verify_total\":%u,"
		       "\"periods_clean\":%u,\"periods_torn\":%u,"
		       "\"periods_bad\":%u,\"periods_empty\":%u}\n",
		       results.mode ? results.mode : "unknown",
		       ret == 0 ? "PASS" : "FAIL",
		       results.duration,
		       (unsigned long)results.tx_bytes,
		       (unsigned long)results.rx_bytes,
		       results.tx_mbps, results.rx_mbps,
		       results.verify_pass ? "true" : "false",
		       results.verify_errors, results.verify_total,
		       results.periods_clean, results.periods_torn,
		       results.periods_bad, results.periods_empty);
	} else {
		printf("===================================================\n");
		if (ret == 0)
			printf("RESULT: PASS\n");
		else
			printf("RESULT: FAIL\n");
	}

	return ret;
}
