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

struct sram_dma_status {
	uint32_t tx_ring_offset;
	uint32_t tx_ring_size;
	uint32_t rx_ring_offset;
	uint32_t rx_ring_size;
	uint32_t period_size;
	uint32_t dma_running;
	uint64_t tx_bytes;
	uint64_t rx_bytes;
};

/* ─── Constants ───────────────────────────────────────────────── */

/* mmap the DMA buffer at offset 0 */
#define DMA_MMAP_OFFSET    0
#define DMA_MMAP_SIZE      0x4000  /* 16,384 bytes (one 16 KB page) */

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

	/* Verify RX data: should be bitwise NOT of TX pattern */
	printf("\n  Verifying RX data...\n");
	__sync_synchronize();
	unsigned errors = 0;
	unsigned first_err_idx = 0;
	uint32_t first_expected = 0, first_actual = 0;

	/* Check the first min(tx_words, rx_words) entries */
	unsigned check_words = tx_words < rx_words ? tx_words : rx_words;
	for (unsigned i = 0; i < check_words; i++) {
		uint32_t expected = ~(0xA0000000u | (i % tx_words));
		uint32_t actual = rx_ring[i];
		if (actual != expected) {
			if (errors == 0) {
				first_err_idx = i;
				first_expected = expected;
				first_actual = actual;
			}
			errors++;
		}
	}

	if (errors == 0) {
		printf("    PASS: %u words verified (bitwise NOT matches)\n", check_words);
	} else {
		printf("    FAIL: %u/%u words mismatched\n", errors, check_words);
		printf("    First mismatch at word %u: expected 0x%08x, got 0x%08x\n",
		       first_err_idx, first_expected, first_actual);
		/* Show first few RX words for debugging */
		printf("    First 8 RX words:\n");
		for (unsigned i = 0; i < 8 && i < rx_words; i++)
			printf("      [%u] = 0x%08x (expected 0x%08x)\n",
			       i, rx_ring[i], ~(0xA0000000u | i));
	}

	printf("\n");
	return (errors == 0 && st.tx_bytes > 0) ? 0 : 1;
}

/* ─── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	double duration = 2.0;

	if (argc >= 2)
		duration = atof(argv[1]);

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	printf("sram_dma_bench — Cyclic DMA PIO Loopback Benchmark\n");
	printf("===================================================\n\n");

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

	/* Step 3: Run DMA diagnostic (quick self-test) */
	printf("Step 3: Running DMA diagnostic...\n");
	{
		int diag_ret = ioctl(dev_fd, SRAM_IOC_DMA_DIAG);
		if (diag_ret < 0)
			printf("  DIAG: FAILED (%s) — check dmesg\n", strerror(errno));
		else
			printf("  DIAG: PASSED — cyclic DMA loopback verified\n");
	}

	/* Step 4: Run DMA loopback throughput test */
	int ret = test_dma_loopback(duration);

	/* Cleanup */
	device_cleanup();
	pio_teardown();

	printf("===================================================\n");
	if (ret == 0)
		printf("RESULT: PASS\n");
	else
		printf("RESULT: FAIL\n");

	return ret;
}
