/* sram_corruption_test.c — Systematic SRAM DMA firmware corruption diagnostic
 *
 * Tests firmware health at each step of SRAM DMA operation to isolate
 * the exact operation that corrupts RP1 firmware state.
 *
 * Test sequence:
 *   1. Verify firmware healthy (baseline)
 *   2. Open PIO, claim SM, load program
 *   3. Verify firmware still healthy
 *   4. Set DMACTRL (enable DREQ)
 *   5. Verify firmware still healthy
 *   6. Run SRAM DMA for N seconds
 *   7. Terminate DMA (no firmware RPCs)
 *   8. Verify firmware still healthy
 *   9. Call pio_sm_set_dmactrl(0) to disable DREQ
 *  10. Verify firmware still healthy
 *  11. Call pio_sm_clear_fifos()
 *  12. Verify firmware still healthy
 *  13. Unclaim SM, close PIO
 *  14. Verify firmware still healthy
 *
 * Each "verify" step opens /dev/pio0 and does a harmless ioctl.
 * If any step fails, we know exactly where corruption occurred.
 *
 * Also dumps the firmware dynamic SRAM region (0x9F48-0xA14F) before
 * and after to check if DMA overwrites it.
 *
 * Build: gcc -Wall -Wextra -O2 -o sram_corruption_test sram_corruption_test.c -lpio -lm
 * Run:   sudo ./sram_corruption_test
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <misc/rp1_pio_if.h>
#include "piolib.h"
#include "../lib/loopback.pio.h"

/* ioctl definitions (must match kmod) */
#define SRAM_IOC_MAGIC     'S'
#define SRAM_IOC_START_SRAM_DMA _IO(SRAM_IOC_MAGIC, 6)
#define SRAM_IOC_STOP_DMA  _IO(SRAM_IOC_MAGIC, 2)
#define SRAM_IOC_DMA_STATUS _IOR(SRAM_IOC_MAGIC, 3, struct sram_dma_status)

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

/* SRAM BAR2 for direct inspection */
#define SRAM_BAR2_PHYS  0x1F00400000ULL
#define SRAM_BAR2_SIZE  0x10000

/* Firmware dynamic region */
#define FW_DYN_START    0x9F48
#define FW_DYN_END      0xA150
#define FW_DYN_WORDS    ((FW_DYN_END - FW_DYN_START) / 4)

/* SRAM ring offsets (must match kmod) */
#define SRAM_TX_RING_OFF  0xA200
#define SRAM_TX_RING_SIZE 8192
#define SRAM_RX_RING_OFF  0xC200

static volatile uint32_t *sram_map;

static int map_sram(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return -1;
	}
	sram_map = mmap(NULL, SRAM_BAR2_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, SRAM_BAR2_PHYS);
	close(fd);
	if (sram_map == MAP_FAILED) {
		perror("mmap SRAM BAR2");
		return -1;
	}
	return 0;
}

/* Test firmware health by opening /dev/pio0 and doing a harmless ioctl */
static int test_firmware_health(const char *label)
{
	int fd = open("/dev/pio0", O_RDWR);
	if (fd < 0) {
		printf("  [%s] FAIL: cannot open /dev/pio0: %s\n", label, strerror(errno));
		return -1;
	}

	/* SM_IS_CLAIMED is a harmless read-only query */
	struct rp1_pio_sm_claim_args args = { .mask = 1 };
	int ret = ioctl(fd, PIO_IOC_SM_IS_CLAIMED, &args);
	close(fd);

	if (ret < 0) {
		printf("  [%s] FAIL: SM_IS_CLAIMED ioctl failed: %s\n", label, strerror(errno));
		return -1;
	}

	printf("  [%s] OK: firmware responsive\n", label);
	return 0;
}

/* Dump firmware dynamic region */
static void dump_fw_dynamic(const char *label)
{
	printf("  [%s] Firmware dynamic region (0x%X-0x%X):\n", label, FW_DYN_START, FW_DYN_END);
	for (unsigned off = FW_DYN_START; off < FW_DYN_END; off += 32) {
		printf("    0x%04X:", off);
		for (unsigned i = 0; i < 8 && (off + i * 4) < FW_DYN_END; i++)
			printf(" %08x", sram_map[(off + i * 4) / 4]);
		printf("\n");
	}
}

/* Check if our TX ring overlaps the firmware dynamic region */
static void check_ring_overlap(void)
{
	unsigned tx_start = SRAM_TX_RING_OFF;
	unsigned tx_end = SRAM_TX_RING_OFF + SRAM_TX_RING_SIZE;

	printf("\n  SRAM layout analysis:\n");
	printf("    TX ring:          0x%04X - 0x%04X (%u bytes)\n",
	       tx_start, tx_end, SRAM_TX_RING_SIZE);
	printf("    FW dynamic:       0x%04X - 0x%04X (%u bytes)\n",
	       FW_DYN_START, FW_DYN_END, FW_DYN_END - FW_DYN_START);

	if (tx_end > FW_DYN_START && tx_start < FW_DYN_END) {
		unsigned overlap_start = tx_start > FW_DYN_START ? tx_start : FW_DYN_START;
		unsigned overlap_end = tx_end < FW_DYN_END ? tx_end : FW_DYN_END;
		printf("    *** OVERLAP: 0x%04X - 0x%04X (%u bytes) ***\n",
		       overlap_start, overlap_end, overlap_end - overlap_start);
	} else {
		printf("    No overlap\n");
	}

	/* Also check RX ring */
	/* Read actual kmod RX ring offset from SRAM layout */
	unsigned rx_start = tx_end;  /* RX follows TX */
	unsigned rx_end = rx_start + SRAM_TX_RING_SIZE;  /* same size */
	printf("    RX ring (est):    0x%04X - 0x%04X\n", rx_start, rx_end);

	/* Check PIO descriptor region */
	printf("    PIO descriptors:  0x8A00 - 0x8AFF (256 bytes)\n");
	if (tx_start <= 0x8AFF && tx_end > 0x8A00)
		printf("    *** TX RING OVERLAPS PIO DESCRIPTORS! ***\n");

	printf("    FW mailbox:       0xFF00 - 0xFFFF (256 bytes)\n");
	if (rx_end > 0xFF00)
		printf("    *** RX RING OVERLAPS FIRMWARE MAILBOX! ***\n");
}

/* Save firmware dynamic region for comparison */
static void save_fw_dynamic(uint32_t *buf)
{
	for (unsigned i = 0; i < FW_DYN_WORDS; i++)
		buf[i] = sram_map[(FW_DYN_START + i * 4) / 4];
}

/* Compare firmware dynamic region */
static int compare_fw_dynamic(const uint32_t *before, const char *label)
{
	int changed = 0;
	for (unsigned i = 0; i < FW_DYN_WORDS; i++) {
		uint32_t now = sram_map[(FW_DYN_START + i * 4) / 4];
		if (now != before[i]) {
			if (changed == 0)
				printf("  [%s] FW dynamic region CHANGED:\n", label);
			printf("    0x%04X: 0x%08x → 0x%08x\n",
			       FW_DYN_START + i * 4, before[i], now);
			changed++;
		}
	}
	if (changed == 0)
		printf("  [%s] FW dynamic region unchanged\n", label);
	return changed;
}

int main(int argc, char *argv[])
{
	double dma_duration = 2.0;
	if (argc > 1)
		dma_duration = atof(argv[1]);

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	printf("SRAM DMA Firmware Corruption Diagnostic\n");
	printf("========================================\n\n");

	/* Map SRAM for direct inspection */
	if (map_sram() < 0)
		return 1;

	/* Check ring/firmware overlap */
	check_ring_overlap();
	printf("\n");

	/* Step 1: Baseline firmware health */
	printf("Step 1: Baseline firmware health\n");
	if (test_firmware_health("baseline") < 0)
		return 1;

	uint32_t fw_before[FW_DYN_WORDS];
	save_fw_dynamic(fw_before);
	dump_fw_dynamic("before DMA");
	printf("\n");

	/* Step 2: Open PIO, claim SM, load program */
	printf("Step 2: PIO setup\n");
	PIO pio = pio_open(0);
	if (PIO_IS_ERR(pio)) {
		printf("  FAIL: pio_open: %d\n", PIO_ERR_VAL(pio));
		return 1;
	}
	int sm = pio_claim_unused_sm(pio, false);
	if (sm < 0) {
		printf("  FAIL: no free SMs\n");
		pio_close(pio);
		return 1;
	}
	uint pio_offset = pio_add_program(pio, &loopback_program);
	pio_sm_config c = loopback_program_get_default_config(pio_offset);
	sm_config_set_out_shift(&c, false, true, 32);
	sm_config_set_in_shift(&c, false, true, 32);
	sm_config_set_clkdiv(&c, 1.0f);
	pio_sm_init(pio, (uint)sm, pio_offset, &c);
	pio_sm_set_enabled(pio, (uint)sm, true);
	printf("  PIO SM%d loaded, offset %u\n", sm, pio_offset);

	printf("Step 3: Post-PIO-setup firmware health\n");
	if (test_firmware_health("post-pio-setup") < 0) {
		printf("  *** CORRUPTION DURING PIO SETUP ***\n");
		return 1;
	}
	printf("\n");

	/* Step 4: Set DMACTRL */
	printf("Step 4: Set DMACTRL (enable DREQ)\n");
	uint32_t dmactrl = 0x80000108;  /* DREQ_EN + threshold=8 */
	pio_sm_set_dmactrl(pio, (uint)sm, true, dmactrl);
	pio_sm_set_dmactrl(pio, (uint)sm, false, dmactrl);
	printf("  DMACTRL set to 0x%08x\n", dmactrl);

	printf("Step 5: Post-DMACTRL firmware health\n");
	if (test_firmware_health("post-dmactrl") < 0) {
		printf("  *** CORRUPTION DURING DMACTRL ***\n");
		return 1;
	}
	printf("\n");

	/* Step 6: Open kmod and run SRAM DMA */
	printf("Step 6: Starting SRAM DMA (%.1f seconds)\n", dma_duration);
	int dev_fd = open("/dev/rp1_pio_sram", O_RDWR);
	if (dev_fd < 0) {
		printf("  FAIL: cannot open /dev/rp1_pio_sram: %s\n", strerror(errno));
		printf("  Load kmod: sudo insmod kmod/rp1_pio_sram.ko\n");
		return 1;
	}

	save_fw_dynamic(fw_before);  /* snapshot just before DMA */

	int ret = ioctl(dev_fd, SRAM_IOC_START_SRAM_DMA);
	if (ret < 0) {
		printf("  FAIL: START_SRAM_DMA: %s\n", strerror(errno));
		close(dev_fd);
		return 1;
	}
	printf("  SRAM DMA running...\n");

	/* Check firmware during DMA */
	usleep(500000);  /* wait 500ms */
	printf("Step 6a: Firmware health DURING SRAM DMA\n");
	(void)test_firmware_health("during-dma");
	compare_fw_dynamic(fw_before, "during-dma");

	/* Let DMA run for remaining duration */
	usleep((unsigned)((dma_duration - 0.5) * 1e6));

	/* Step 7: Stop DMA (kmod skips RPCs for SRAM mode) */
	printf("\nStep 7: Stopping SRAM DMA (no firmware RPCs)\n");
	ioctl(dev_fd, SRAM_IOC_STOP_DMA);

	struct sram_dma_status st;
	ioctl(dev_fd, SRAM_IOC_DMA_STATUS, &st);
	printf("  TX: %lu bytes, RX: %lu bytes\n",
	       (unsigned long)st.tx_bytes, (unsigned long)st.rx_bytes);
	close(dev_fd);

	printf("Step 8: Post-DMA-stop firmware health\n");
	int post_stop_health = test_firmware_health("post-dma-stop");
	compare_fw_dynamic(fw_before, "post-dma-stop");
	dump_fw_dynamic("after DMA");
	printf("\n");

	if (post_stop_health < 0) {
		printf("  *** CORRUPTION DURING OR AFTER SRAM DMA ***\n");
		printf("  Skipping RPC tests (firmware already broken)\n");
		goto done;
	}

	/* Step 9: Try disabling DREQ (this was suspected to cause corruption) */
	printf("Step 9: Disable TX DREQ via pio_sm_set_dmactrl(0)\n");
	pio_sm_set_dmactrl(pio, (uint)sm, true, 0);
	printf("  pio_sm_set_dmactrl(sm, TX, 0) called\n");

	printf("Step 9a: Firmware health after TX DREQ disable\n");
	int post_tx_dreq = test_firmware_health("post-tx-dreq-disable");
	if (post_tx_dreq < 0) {
		printf("  *** CORRUPTION ON TX DREQ DISABLE ***\n");
		goto done;
	}
	printf("\n");

	printf("Step 10: Disable RX DREQ via pio_sm_set_dmactrl(0)\n");
	pio_sm_set_dmactrl(pio, (uint)sm, false, 0);
	printf("  pio_sm_set_dmactrl(sm, RX, 0) called\n");

	printf("Step 10a: Firmware health after RX DREQ disable\n");
	int post_rx_dreq = test_firmware_health("post-rx-dreq-disable");
	if (post_rx_dreq < 0) {
		printf("  *** CORRUPTION ON RX DREQ DISABLE ***\n");
		goto done;
	}
	printf("\n");

	printf("Step 11: Clear FIFOs\n");
	pio_sm_clear_fifos(pio, (uint)sm);
	printf("  pio_sm_clear_fifos called\n");

	printf("Step 11a: Firmware health after FIFO clear\n");
	int post_fifo = test_firmware_health("post-fifo-clear");
	if (post_fifo < 0) {
		printf("  *** CORRUPTION ON FIFO CLEAR ***\n");
		goto done;
	}
	printf("\n");

	/* Step 12: Clean up PIO */
	printf("Step 12: PIO cleanup (disable SM, unclaim)\n");
	pio_sm_set_enabled(pio, (uint)sm, false);
	pio_remove_program(pio, &loopback_program, pio_offset);
	pio_sm_unclaim(pio, (uint)sm);
	pio_close(pio);
	pio = NULL;

	printf("Step 13: Final firmware health\n");
	if (test_firmware_health("final") < 0) {
		printf("  *** CORRUPTION DURING PIO CLEANUP ***\n");
		goto done;
	}

	printf("\n========================================\n");
	printf("ALL STEPS PASSED — no firmware corruption detected\n");
	printf("========================================\n");

	munmap((void *)sram_map, SRAM_BAR2_SIZE);
	return 0;

done:
	printf("\n========================================\n");
	printf("FIRMWARE CORRUPTION DETECTED — see steps above\n");
	printf("Recovery: PoE power cycle required\n");
	printf("========================================\n");

	if (pio) {
		pio_sm_set_enabled(pio, (uint)sm, false);
		pio_remove_program(pio, &loopback_program, pio_offset);
		pio_sm_unclaim(pio, (uint)sm);
		pio_close(pio);
	}
	munmap((void *)sram_map, SRAM_BAR2_SIZE);
	return 1;
}
