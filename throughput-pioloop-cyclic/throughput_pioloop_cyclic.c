/* throughput_pioloop_cyclic.c — Cyclic DMA PIO Loopback Benchmark
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
 * Build: gcc -Wall -Wextra -O2 -o throughput_pioloop_cyclic throughput_pioloop_cyclic.c \
 *        -I/usr/include/piolib -lpio -lm
 * Run:   sudo ./throughput_pioloop_cyclic
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
#include <pthread.h>
#include <unistd.h>

#include "piolib.h"
#include "../lib/pio_loopback.pio.h"

/* ─── Unidirectional PIO programs ──────────────────────────────── */

/* RX-only: generates data by shifting 32 null bits into ISR with autopush.
 * Each PIO cycle pushes one 32-bit zero word to the RX FIFO.
 * PIO instruction: in null, 32 = 0x4060 */
static const uint16_t rx_gen_insns[] = { 0x4060 };
static const pio_program_t rx_gen_program = {
	.instructions = rx_gen_insns,
	.length = 1,
	.origin = -1,
	.pio_version = 0,
};

/* TX-only: consumes data by shifting 32 bits out of OSR to null with autopull.
 * Each PIO cycle pulls one 32-bit word from the TX FIFO and discards it.
 * PIO instruction: out null, 32 = 0x6060 */
static const uint16_t tx_con_insns[] = { 0x6060 };
static const pio_program_t tx_con_program = {
	.instructions = tx_con_insns,
	.length = 1,
	.origin = -1,
	.pio_version = 0,
};

/* ─── ioctl definitions (must match kernel module) ────────────── */

#define SRAM_IOC_MAGIC     'S'
#define SRAM_IOC_START_DMA _IO(SRAM_IOC_MAGIC, 1)
#define SRAM_IOC_STOP_DMA  _IO(SRAM_IOC_MAGIC, 2)
#define SRAM_IOC_DMA_STATUS _IOR(SRAM_IOC_MAGIC, 3, struct sram_dma_status)
#define SRAM_IOC_DMA_DIAG  _IO(SRAM_IOC_MAGIC, 4)
#define SRAM_IOC_START_SRAM_DMA _IO(SRAM_IOC_MAGIC, 6)
#define SRAM_IOC_SET_DMA_DIR   _IOW(SRAM_IOC_MAGIC, 7, int)

/* DMA direction modes (must match kernel module) */
#define DMA_DIR_BOTH    0
#define DMA_DIR_TX_ONLY 1
#define DMA_DIR_RX_ONLY 2

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
#define DMA_MMAP_SIZE      0x10000  /* 65,536 bytes (64 KB) */

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
static int dma_dir = DMA_DIR_BOTH;  /* 0=both, 1=tx-only, 2=rx-only */

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
static const pio_program_t *loaded_program;
static int dev_fd = -1;
static volatile uint32_t *dma_buf;

/* ─── PIO setup ───────────────────────────────────────────────── */

static int pio_setup(void)
{
	const pio_program_t *prog;
	const char *prog_name;

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

	/* Select PIO program based on DMA direction */
	if (dma_dir == DMA_DIR_RX_ONLY) {
		prog = &rx_gen_program;
		prog_name = "rx-generator (in null, 32)";
	} else if (dma_dir == DMA_DIR_TX_ONLY) {
		prog = &tx_con_program;
		prog_name = "tx-consumer (out null, 32)";
	} else {
		prog = &loopback_program;
		prog_name = "loopback (pull->NOT->push)";
	}

	pio_offset = pio_add_program(pio, prog);
	if (pio_offset == PIO_ORIGIN_INVALID) {
		fprintf(stderr, "ERROR: failed to load PIO program\n");
		pio_sm_unclaim(pio, (uint)sm);
		pio_close(pio);
		return -1;
	}
	loaded_program = prog;

	/* Configure SM: 32-bit shifts, 200 MHz (clkdiv=1) */
	pio_sm_config c = pio_get_default_sm_config_for_pio(pio);
	sm_config_set_wrap(&c, pio_offset, pio_offset + prog->length - 1);
	sm_config_set_clkdiv(&c, 1.0f);

	if (dma_dir == DMA_DIR_RX_ONLY) {
		sm_config_set_in_shift(&c, false, true, 32);  /* autopush */
	} else if (dma_dir == DMA_DIR_TX_ONLY) {
		sm_config_set_out_shift(&c, false, true, 32);  /* autopull */
	} else {
		sm_config_set_out_shift(&c, false, true, 32);
		sm_config_set_in_shift(&c, false, true, 32);
	}

	pio_sm_init(pio, (uint)sm, pio_offset, &c);
	pio_sm_set_enabled(pio, (uint)sm, true);

	printf("  PIO SM%d: %s loaded at offset %u\n", sm, prog_name, pio_offset);
	return 0;
}

static void pio_teardown(void)
{
	if (sm >= 0) {
		pio_sm_set_enabled(pio, (uint)sm, false);
		/* Reset DMACTRL to defaults (disable DREQ) so subsequent
		 * piolib users don't inherit our threshold/priority settings */
		pio_sm_set_dmactrl(pio, (uint)sm, true, 0);
		pio_sm_set_dmactrl(pio, (uint)sm, false, 0);
		pio_sm_clear_fifos(pio, (uint)sm);
		pio_remove_program(pio, loaded_program, pio_offset);
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

	if (dma_dir != DMA_DIR_RX_ONLY) {
		printf("  Filling TX ring with sequential pattern...\n");
		for (unsigned i = 0; i < tx_words; i++)
			tx_ring[i] = 0xA0000000u | i;
		__sync_synchronize();
	}

	if (dma_dir != DMA_DIR_TX_ONLY) {
		/* Clear RX ring */
		for (unsigned i = 0; i < rx_words; i++)
			rx_ring[i] = 0xDEADDEADu;
		__sync_synchronize();
	}

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

	/* For unidirectional modes, skip detailed verification */
	if (dma_dir != DMA_DIR_BOTH) {
		int verify_pass = 1;
		if (dma_dir == DMA_DIR_RX_ONLY) {
			/* RX-only: check that RX ring was written (not all 0xDEADDEAD) */
			__sync_synchronize();
			unsigned rx_written = 0;
			for (unsigned i = 0; i < rx_words; i++) {
				if (rx_ring[i] != 0xDEADDEADu)
					rx_written++;
			}
			printf("\n  RX-only: %u/%u words written by DMA\n", rx_written, rx_words);
			/* RX generator produces 0x00000000 — spot check */
			if (rx_written > 0) {
				printf("    RX[0..3]: 0x%08x 0x%08x 0x%08x 0x%08x\n",
				       rx_ring[0], rx_ring[1], rx_ring[2], rx_ring[3]);
			}
			verify_pass = (rx_written > 0);
		}
		printf("\n");
		results.mode = "dram";
		results.duration = elapsed;
		results.tx_bytes = st.tx_bytes;
		results.rx_bytes = st.rx_bytes;
		results.tx_mbps = st.tx_bytes > 0 ? (double)st.tx_bytes / elapsed / (1024.0 * 1024.0) : 0;
		results.rx_mbps = st.rx_bytes > 0 ? (double)st.rx_bytes / elapsed / (1024.0 * 1024.0) : 0;
		results.verify_pass = verify_pass;
		return (verify_pass && (st.tx_bytes > 0 || st.rx_bytes > 0)) ? 0 : 1;
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

/* ─── Test: piolib DMA baseline ──────────────────────────────── */

struct xfer_args {
	PIO pio;
	uint sm;
	enum pio_xfer_dir dir;
	void *buf;
	size_t size;
	int ret;
};

static void *xfer_thread(void *arg)
{
	struct xfer_args *a = (struct xfer_args *)arg;
	a->ret = pio_sm_xfer_data(a->pio, a->sm, a->dir, a->size, a->buf);
	return NULL;
}

static int test_piolib_baseline(double duration_sec)
{
	printf("\n=== piolib DMA Baseline Benchmark ===\n\n");
	printf("  Data path: Host DRAM → ioctl → kernel DMA → PIO FIFO (standard path)\n\n");

	/* Use 4 KB aligned buffers (piolib requires 64-byte alignment) */
	size_t xfer_size = 4096;
	void *tx_buf = aligned_alloc(64, xfer_size);
	void *rx_buf = aligned_alloc(64, xfer_size);
	if (!tx_buf || !rx_buf) {
		fprintf(stderr, "ERROR: aligned_alloc failed\n");
		free(tx_buf);
		free(rx_buf);
		return -1;
	}

	/* Fill TX with pattern */
	uint32_t *tx32 = (uint32_t *)tx_buf;
	for (size_t i = 0; i < xfer_size / 4; i++)
		tx32[i] = 0xA0000000u | (uint32_t)i;
	memset(rx_buf, 0xDD, xfer_size);

	/* Set DMACTRL: enable DREQ, threshold=1, priority=2 */
	uint32_t dmactrl = 0x80000000u | (2 << 7) | 1;
	pio_sm_set_dmactrl(pio, (uint)sm, true, dmactrl);
	pio_sm_set_dmactrl(pio, (uint)sm, false, dmactrl);

	/* Configure DMA channels */
	int ret = pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_TO_SM, xfer_size, 1);
	if (ret < 0) {
		fprintf(stderr, "ERROR: pio_sm_config_xfer TX failed: %d (errno=%s)\n",
			ret, strerror(errno));
		free(tx_buf);
		free(rx_buf);
		return -1;
	}
	ret = pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_FROM_SM, xfer_size, 1);
	if (ret < 0) {
		fprintf(stderr, "ERROR: pio_sm_config_xfer RX failed: %d (errno=%s)\n",
			ret, strerror(errno));
		free(tx_buf);
		free(rx_buf);
		return -1;
	}

	/* Run transfers in a loop for the specified duration */
	uint64_t total_bytes = 0;
	unsigned iterations = 0;
	unsigned errors = 0;

	double t0 = get_time_sec();
	double deadline = t0 + duration_sec;

	printf("  Running for %.1f seconds...\n", duration_sec);

	while (get_time_sec() < deadline) {
		struct xfer_args tx_args = {
			.pio = pio, .sm = (uint)sm,
			.dir = PIO_DIR_TO_SM, .buf = tx_buf, .size = xfer_size
		};
		struct xfer_args rx_args = {
			.pio = pio, .sm = (uint)sm,
			.dir = PIO_DIR_FROM_SM, .buf = rx_buf, .size = xfer_size
		};

		pthread_t tx_tid, rx_tid;
		pthread_create(&tx_tid, NULL, xfer_thread, &tx_args);
		pthread_create(&rx_tid, NULL, xfer_thread, &rx_args);
		pthread_join(tx_tid, NULL);
		pthread_join(rx_tid, NULL);

		if (tx_args.ret < 0 || rx_args.ret < 0) {
			fprintf(stderr, "ERROR: xfer failed at iter %u (TX=%d, RX=%d)\n",
				iterations, tx_args.ret, rx_args.ret);
			errors++;
			/* Re-configure for next attempt */
			pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_TO_SM, xfer_size, 1);
			pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_FROM_SM, xfer_size, 1);
			continue;
		}

		/* Verify: RX should be bitwise NOT of TX */
		uint32_t *rx32 = (uint32_t *)rx_buf;
		for (size_t i = 0; i < xfer_size / 4; i++) {
			if (rx32[i] != ~tx32[i]) {
				errors++;
				break;
			}
		}

		total_bytes += xfer_size;
		iterations++;
	}

	double elapsed = get_time_sec() - t0;
	double mbps = (double)total_bytes / elapsed / (1024.0 * 1024.0);

	printf("\n  Results:\n");
	printf("    Duration:    %.3f seconds\n", elapsed);
	printf("    Iterations:  %u (each %zu bytes)\n", iterations, xfer_size);
	printf("    Total bytes: %lu\n", (unsigned long)total_bytes);
	printf("    Throughput:  %.2f MB/s\n", mbps);
	printf("    Errors:      %u\n", errors);
	printf("\n");

	/* Populate results struct */
	results.mode = "piolib";
	results.duration = elapsed;
	results.tx_bytes = total_bytes;
	results.rx_bytes = total_bytes;
	results.tx_mbps = mbps;
	results.rx_mbps = mbps;
	results.verify_pass = (errors == 0);
	results.verify_errors = errors;
	results.verify_total = iterations;

	free(tx_buf);
	free(rx_buf);

	return errors == 0 ? 0 : 1;
}

/* ─── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	double duration = 2.0;
	int use_sram = 0;
	int use_piolib = 0;

	int diag_only = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--sram") == 0)
			use_sram = 1;
		else if (strcmp(argv[i], "--dram") == 0)
			use_sram = 0;
		else if (strcmp(argv[i], "--piolib") == 0)
			use_piolib = 1;
		else if (strcmp(argv[i], "--diag") == 0)
			diag_only = 1;
		else if (strcmp(argv[i], "--tx-only") == 0)
			dma_dir = DMA_DIR_TX_ONLY;
		else if (strcmp(argv[i], "--rx-only") == 0)
			dma_dir = DMA_DIR_RX_ONLY;
		else if (strcmp(argv[i], "--json") == 0)
			json_mode = 1;
		else if (strncmp(argv[i], "--duration=", 11) == 0)
			duration = atof(argv[i] + 11);
		else if (argv[i][0] != '-')
			duration = atof(argv[i]);
	}

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	const char *dir_str = dma_dir == DMA_DIR_TX_ONLY ? " TX-only" :
			      dma_dir == DMA_DIR_RX_ONLY ? " RX-only" : "";
	const char *mode_str = use_piolib ? "piolib (standard ioctl DMA)" :
			       use_sram  ? "SRAM (RP1-internal)" :
					   "DRAM (host, via PCIe)";
	printf("throughput_pioloop_cyclic — Cyclic DMA PIO Loopback Benchmark\n");
	printf("===================================================\n");
	printf("Mode: %s%s\n\n", mode_str, dir_str);

	/* Step 1: Set up PIO loopback */
	printf("Step 1: Setting up PIO loopback...\n");
	if (pio_setup() < 0)
		return 1;

	int ret;
	if (use_piolib) {
		/* piolib baseline: no kernel module needed */
		ret = test_piolib_baseline(duration);
		pio_teardown();
	} else {
		/* Step 2: Open kernel module device */
		printf("Step 2: Opening /dev/rp1_pio_sram...\n");
		if (device_setup() < 0) {
			pio_teardown();
			return 1;
		}

		/* Set DMA direction before starting test */
		if (dma_dir != DMA_DIR_BOTH) {
			if (ioctl(dev_fd, SRAM_IOC_SET_DMA_DIR, &dma_dir) < 0) {
				fprintf(stderr, "ERROR: SET_DMA_DIR failed: %s\n",
					strerror(errno));
				device_cleanup();
				pio_teardown();
				return 1;
			}
			printf("  DMA direction: %s\n",
			       dma_dir == DMA_DIR_TX_ONLY ? "TX-only" : "RX-only");
		}

		if (diag_only) {
			printf("Step 3: Running DMA diagnostic (standalone)...\n");
			int diag_ret = ioctl(dev_fd, SRAM_IOC_DMA_DIAG);
			if (diag_ret < 0)
				printf("  DIAG: FAILED (%s) — check dmesg\n",
				       strerror(errno));
			else
				printf("  DIAG: PASSED — cyclic DMA loopback verified\n");
			ret = diag_ret < 0 ? 1 : 0;
		} else if (use_sram) {
			ret = test_sram_loopback(duration);
		} else {
			ret = test_dma_loopback(duration);
		}

		/* Reset DMA direction to bidirectional for next run */
		if (dma_dir != DMA_DIR_BOTH) {
			int both = DMA_DIR_BOTH;
			ioctl(dev_fd, SRAM_IOC_SET_DMA_DIR, &both);
		}
		device_cleanup();
		pio_teardown();
	}

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
