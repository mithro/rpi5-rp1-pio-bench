// SPDX-License-Identifier: GPL-2.0
/*
 * rp1_pio_sram.c — RP1 PIO Cyclic DMA Benchmark
 *
 * Out-of-tree kernel module that sets up cyclic DMA between host DRAM
 * ring buffers and PIO FIFOs, using the RP1's dw-axi-dmac controller.
 *
 * Architecture:
 *   Host CPU ──PCIe──→ Host DRAM ring buffer
 *                          ↓ (RP1 DMA reads via PCIe)
 *                     RP1 DMA (cyclic) ──APB──→ PIO FIFO
 *
 * Note: SRAM-local DMA was attempted but RP1 SRAM (M3 TCM) is not
 * reachable from the DMA controller's AXI port. SRAM is only accessible
 * via M3 core (TCM bus) and PCIe BAR2. See DESIGN.md for details.
 *
 * The module:
 *   1. Allocates coherent DMA buffers in host DRAM
 *   2. Requests DMA channels from the PIO device tree node
 *   3. Sets up cyclic DMA: TX ring → PIO TXF and PIO RXF → RX ring
 *   4. Exposes /dev/rp1_pio_sram for userspace mmap + ioctl
 *
 * PIO state machine setup is done from userspace via piolib before
 * starting DMA through this module's ioctl interface.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/pio_rp1.h>

/* DMA ring buffer layout (in a single 16 KB coherent allocation) */
#define DMA_BUF_SIZE       16384          /* 16 KB = 1 ARM64 page */
#define TX_RING_OFFSET     0             /* TX ring starts at offset 0 */
#define TX_RING_SIZE       8192          /* 8 KB = 8 periods × 1 KB */
#define RX_RING_OFFSET     8192          /* RX ring at offset 8 KB */
#define RX_RING_SIZE       8192          /* 8 KB = 8 periods × 1 KB */
#define DMA_PERIOD_SIZE    1024          /* 1 KB per DMA period */

/* PIO FIFO physical addresses (BCM2712 / CPU perspective) */
#define PIO_PHYS_BASE      0x1F00178000ULL
#define PIO_FIFO_TX0       (PIO_PHYS_BASE + 0x000)
#define PIO_FIFO_RX0       (PIO_PHYS_BASE + 0x010)

/* ioctl interface */
#define SRAM_IOC_MAGIC     'S'
#define SRAM_IOC_START_DMA _IO(SRAM_IOC_MAGIC, 1)
#define SRAM_IOC_STOP_DMA  _IO(SRAM_IOC_MAGIC, 2)
#define SRAM_IOC_DMA_STATUS _IOR(SRAM_IOC_MAGIC, 3, struct sram_dma_status)
#define SRAM_IOC_DMA_DIAG  _IO(SRAM_IOC_MAGIC, 4)

struct sram_dma_status {
	__u32 tx_ring_offset;
	__u32 tx_ring_size;
	__u32 rx_ring_offset;
	__u32 rx_ring_size;
	__u32 period_size;
	__u32 dma_running;
	__u64 tx_bytes;
	__u64 rx_bytes;
};

/* RP1 PIO DMACTRL default value (from rp1-pio driver) */
#define RP1_PIO_DMACTRL_DEFAULT		0x80000104

/* Module state */
static struct device *pio_dev;
static struct rp1_pio_client *pio_client;
static struct dma_chan *tx_chan;
static struct dma_chan *rx_chan;
static struct dma_async_tx_descriptor *tx_desc;
static struct dma_async_tx_descriptor *rx_desc;
static dma_cookie_t tx_cookie;
static dma_cookie_t rx_cookie;
static bool dma_running;
static u64 tx_period_count;
static u64 rx_period_count;

/* DMA buffer (coherent host DRAM) */
static void *dma_buf;
static dma_addr_t dma_buf_addr;

/* ─── DMA callbacks ─────────────────────────────────────────── */

static void tx_dma_callback(void *data)
{
	tx_period_count++;
}

static void rx_dma_callback(void *data)
{
	rx_period_count++;
}

/* ─── DMA channel management ───────────────────────────────── */

static int acquire_dma_channels(void)
{
	struct device_node *pio_np;
	struct platform_device *pio_pdev;

	pio_np = of_find_compatible_node(NULL, NULL, "raspberrypi,rp1-pio");
	if (!pio_np) {
		pr_err("rp1_pio_sram: cannot find PIO device tree node\n");
		return -ENODEV;
	}

	pio_pdev = of_find_device_by_node(pio_np);
	of_node_put(pio_np);
	if (!pio_pdev) {
		pr_err("rp1_pio_sram: cannot find PIO platform device\n");
		return -ENODEV;
	}

	pio_dev = &pio_pdev->dev;

	tx_chan = dma_request_chan(pio_dev, "tx0");
	if (IS_ERR(tx_chan)) {
		int err = PTR_ERR(tx_chan);
		pr_err("rp1_pio_sram: failed to request TX DMA channel: %d\n", err);
		tx_chan = NULL;
		put_device(pio_dev);
		return err;
	}

	rx_chan = dma_request_chan(pio_dev, "rx0");
	if (IS_ERR(rx_chan)) {
		int err = PTR_ERR(rx_chan);
		pr_err("rp1_pio_sram: failed to request RX DMA channel: %d\n", err);
		dma_release_channel(tx_chan);
		tx_chan = NULL;
		rx_chan = NULL;
		put_device(pio_dev);
		return err;
	}

	pr_info("rp1_pio_sram: DMA channels acquired (TX: %s, RX: %s)\n",
		dma_chan_name(tx_chan), dma_chan_name(rx_chan));

	return 0;
}

static void release_dma_channels(void)
{
	if (tx_chan) {
		dma_release_channel(tx_chan);
		tx_chan = NULL;
	}
	if (rx_chan) {
		dma_release_channel(rx_chan);
		rx_chan = NULL;
	}
	if (pio_dev) {
		put_device(pio_dev);
		pio_dev = NULL;
	}
}

/* ─── DMA buffer management ──────────────────────────────── */

static int alloc_dma_buffer(void)
{
	if (!tx_chan)
		return -ENODEV;

	dma_buf = dma_alloc_coherent(tx_chan->device->dev, DMA_BUF_SIZE,
				      &dma_buf_addr, GFP_KERNEL);
	if (!dma_buf) {
		pr_err("rp1_pio_sram: failed to allocate DMA buffer\n");
		return -ENOMEM;
	}

	pr_info("rp1_pio_sram: DMA buffer: virt=%px dma=0x%llx size=%u\n",
		dma_buf, (u64)dma_buf_addr, DMA_BUF_SIZE);

	return 0;
}

static void free_dma_buffer(void)
{
	if (dma_buf && tx_chan) {
		dma_free_coherent(tx_chan->device->dev, DMA_BUF_SIZE,
				   dma_buf, dma_buf_addr);
		dma_buf = NULL;
		dma_buf_addr = 0;
	}
}

/* ─── Cyclic DMA control ──────────────────────────────────── */

static int start_cyclic_dma(void)
{
	struct dma_slave_config tx_cfg = {};
	struct dma_slave_config rx_cfg = {};
	dma_addr_t tx_dma, rx_dma;
	int ret;

	if (dma_running)
		return -EBUSY;
	if (!tx_chan || !rx_chan || !dma_buf)
		return -ENODEV;

	tx_dma = dma_buf_addr + TX_RING_OFFSET;
	rx_dma = dma_buf_addr + RX_RING_OFFSET;

	/* Configure TX: ring buffer → PIO TX FIFO */
	tx_cfg.direction = DMA_MEM_TO_DEV;
	tx_cfg.dst_addr = PIO_FIFO_TX0;
	tx_cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	tx_cfg.dst_maxburst = 4;
	tx_cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;

	ret = dmaengine_slave_config(tx_chan, &tx_cfg);
	if (ret) {
		pr_err("rp1_pio_sram: TX slave config failed: %d\n", ret);
		return ret;
	}

	/* Configure RX: PIO RX FIFO → ring buffer */
	rx_cfg.direction = DMA_DEV_TO_MEM;
	rx_cfg.src_addr = PIO_FIFO_RX0;
	rx_cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	rx_cfg.src_maxburst = 1;
	rx_cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;

	ret = dmaengine_slave_config(rx_chan, &rx_cfg);
	if (ret) {
		pr_err("rp1_pio_sram: RX slave config failed: %d\n", ret);
		return ret;
	}

	pr_info("rp1_pio_sram: TX: DRAM 0x%llx (%u B) -> PIO TXF0 0x%llx\n",
		(u64)tx_dma, TX_RING_SIZE, (u64)PIO_FIFO_TX0);
	pr_info("rp1_pio_sram: RX: PIO RXF0 0x%llx -> DRAM 0x%llx (%u B)\n",
		(u64)PIO_FIFO_RX0, (u64)rx_dma, RX_RING_SIZE);

	/* Enable PIO DREQ signals via rp1-pio firmware */
	if (!pio_client) {
		pio_client = rp1_pio_open();
		if (IS_ERR(pio_client)) {
			pr_err("rp1_pio_sram: rp1_pio_open failed: %ld\n",
			       PTR_ERR(pio_client));
			pio_client = NULL;
			return -ENODEV;
		}
	}

	ret = pio_sm_set_dmactrl(pio_client, 0, true, RP1_PIO_DMACTRL_DEFAULT);
	if (ret) {
		pr_err("rp1_pio_sram: TX dmactrl failed: %d\n", ret);
		return ret;
	}

	ret = pio_sm_set_dmactrl(pio_client, 0, false,
				 (RP1_PIO_DMACTRL_DEFAULT & ~0x1f) | 1);
	if (ret) {
		pr_err("rp1_pio_sram: RX dmactrl failed: %d\n", ret);
		return ret;
	}

	/* Prepare cyclic TX DMA */
	tx_desc = dmaengine_prep_dma_cyclic(tx_chan, tx_dma,
					     TX_RING_SIZE, DMA_PERIOD_SIZE,
					     DMA_MEM_TO_DEV,
					     DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx_desc) {
		pr_err("rp1_pio_sram: failed to prepare TX cyclic DMA\n");
		return -EIO;
	}
	tx_desc->callback = tx_dma_callback;

	/* Prepare cyclic RX DMA */
	rx_desc = dmaengine_prep_dma_cyclic(rx_chan, rx_dma,
					     RX_RING_SIZE, DMA_PERIOD_SIZE,
					     DMA_DEV_TO_MEM,
					     DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!rx_desc) {
		pr_err("rp1_pio_sram: failed to prepare RX cyclic DMA\n");
		return -EIO;
	}
	rx_desc->callback = rx_dma_callback;

	/* Reset counters and start */
	tx_period_count = 0;
	rx_period_count = 0;

	tx_cookie = dmaengine_submit(tx_desc);
	if (dma_submit_error(tx_cookie)) {
		pr_err("rp1_pio_sram: TX DMA submit failed\n");
		return -EIO;
	}

	rx_cookie = dmaengine_submit(rx_desc);
	if (dma_submit_error(rx_cookie)) {
		pr_err("rp1_pio_sram: RX DMA submit failed\n");
		dmaengine_terminate_sync(tx_chan);
		return -EIO;
	}

	dma_async_issue_pending(tx_chan);
	dma_async_issue_pending(rx_chan);

	dma_running = true;
	pr_info("rp1_pio_sram: cyclic DMA started\n");

	return 0;
}

static void stop_cyclic_dma(void)
{
	if (!dma_running)
		return;

	if (tx_chan)
		dmaengine_terminate_sync(tx_chan);
	if (rx_chan)
		dmaengine_terminate_sync(rx_chan);

	dma_running = false;
	pr_info("rp1_pio_sram: DMA stopped (TX: %llu periods, RX: %llu periods)\n",
		tx_period_count, rx_period_count);

	/* Debug: show buffer contents */
	if (dma_buf) {
		u32 *tx = (u32 *)(dma_buf + TX_RING_OFFSET);
		u32 *rx = (u32 *)(dma_buf + RX_RING_OFFSET);
		pr_info("rp1_pio_sram: TX[0..1]: 0x%08x 0x%08x\n", tx[0], tx[1]);
		pr_info("rp1_pio_sram: RX[0..1]: 0x%08x 0x%08x\n", rx[0], rx[1]);
	}
}

/* ─── DMA diagnostic: quick self-test ─────────────────────── */

static int dma_diag_test(void)
{
	struct dma_slave_config tx_cfg = {}, rx_cfg = {};
	struct dma_async_tx_descriptor *diag_tx_desc, *diag_rx_desc;
	void *tx_buf, *rx_buf;
	dma_addr_t tx_dma, rx_dma;
	u32 *tx_words, *rx_words;
	int ret, i, errors;
	u64 diag_tx_periods, diag_rx_periods;

	#define DIAG_BUF_SIZE  4096
	#define DIAG_PERIOD    1024

	if (!tx_chan || !rx_chan)
		return -ENODEV;

	tx_buf = dma_alloc_coherent(tx_chan->device->dev, DIAG_BUF_SIZE,
				     &tx_dma, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	rx_buf = dma_alloc_coherent(rx_chan->device->dev, DIAG_BUF_SIZE,
				     &rx_dma, GFP_KERNEL);
	if (!rx_buf) {
		dma_free_coherent(tx_chan->device->dev, DIAG_BUF_SIZE,
				   tx_buf, tx_dma);
		return -ENOMEM;
	}

	/* Fill TX with pattern, clear RX */
	tx_words = (u32 *)tx_buf;
	rx_words = (u32 *)rx_buf;
	for (i = 0; i < DIAG_BUF_SIZE / 4; i++) {
		tx_words[i] = 0xA0000000u | i;
		rx_words[i] = 0xDEADDEADu;
	}

	/* Configure slave */
	tx_cfg.direction = DMA_MEM_TO_DEV;
	tx_cfg.dst_addr = PIO_FIFO_TX0;
	tx_cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	tx_cfg.dst_maxburst = 4;
	tx_cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dmaengine_slave_config(tx_chan, &tx_cfg);

	rx_cfg.direction = DMA_DEV_TO_MEM;
	rx_cfg.src_addr = PIO_FIFO_RX0;
	rx_cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	rx_cfg.src_maxburst = 1;
	rx_cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dmaengine_slave_config(rx_chan, &rx_cfg);

	/* Enable DREQ */
	if (!pio_client) {
		pio_client = rp1_pio_open();
		if (IS_ERR(pio_client)) {
			pio_client = NULL;
			ret = -ENODEV;
			goto out;
		}
	}
	pio_sm_set_dmactrl(pio_client, 0, true, RP1_PIO_DMACTRL_DEFAULT);
	pio_sm_set_dmactrl(pio_client, 0, false,
			   (RP1_PIO_DMACTRL_DEFAULT & ~0x1f) | 1);

	/* Prepare cyclic DMA */
	diag_tx_desc = dmaengine_prep_dma_cyclic(tx_chan, tx_dma,
		DIAG_BUF_SIZE, DIAG_PERIOD, DMA_MEM_TO_DEV,
		DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	diag_rx_desc = dmaengine_prep_dma_cyclic(rx_chan, rx_dma,
		DIAG_BUF_SIZE, DIAG_PERIOD, DMA_DEV_TO_MEM,
		DMA_PREP_INTERRUPT | DMA_CTRL_ACK);

	if (!diag_tx_desc || !diag_rx_desc) {
		pr_err("rp1_pio_sram: DIAG: prep cyclic failed\n");
		ret = -EIO;
		goto out;
	}

	diag_tx_desc->callback = tx_dma_callback;
	diag_rx_desc->callback = rx_dma_callback;
	tx_period_count = 0;
	rx_period_count = 0;

	dmaengine_submit(diag_tx_desc);
	dmaengine_submit(diag_rx_desc);
	dma_async_issue_pending(tx_chan);
	dma_async_issue_pending(rx_chan);

	msleep(100);

	diag_tx_periods = tx_period_count;
	diag_rx_periods = rx_period_count;

	dmaengine_terminate_sync(tx_chan);
	dmaengine_terminate_sync(rx_chan);

	/* Check results */
	errors = 0;
	for (i = 0; i < DIAG_BUF_SIZE / 4; i++) {
		u32 expected = ~(0xA0000000u | (i % (DIAG_BUF_SIZE / 4)));
		if (rx_words[i] != expected)
			errors++;
	}

	pr_info("rp1_pio_sram: DIAG: TX=%llu RX=%llu periods, %d/%d errors\n",
		diag_tx_periods, diag_rx_periods, errors, DIAG_BUF_SIZE / 4);

	ret = (errors == 0 && diag_tx_periods > 0) ? 0 : -EIO;

out:
	dma_free_coherent(tx_chan->device->dev, DIAG_BUF_SIZE, tx_buf, tx_dma);
	dma_free_coherent(rx_chan->device->dev, DIAG_BUF_SIZE, rx_buf, rx_dma);
	return ret;
}

/* ─── Misc device ─────────────────────────────────────────── */

static int rp1_pio_sram_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int rp1_pio_sram_release(struct inode *inode, struct file *file)
{
	return 0;
}

static int rp1_pio_sram_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!dma_buf || !tx_chan)
		return -ENODEV;

	/* Only allow mapping the entire DMA buffer (16 KB) at offset 0 */
	if (vma->vm_pgoff != 0 || size > DMA_BUF_SIZE)
		return -EINVAL;

	return dma_mmap_coherent(tx_chan->device->dev, vma,
				  dma_buf, dma_buf_addr, size);
}

static long rp1_pio_sram_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	switch (cmd) {
	case SRAM_IOC_START_DMA:
		return start_cyclic_dma();

	case SRAM_IOC_STOP_DMA:
		stop_cyclic_dma();
		return 0;

	case SRAM_IOC_DMA_STATUS: {
		struct sram_dma_status st = {
			.tx_ring_offset = TX_RING_OFFSET,
			.tx_ring_size = TX_RING_SIZE,
			.rx_ring_offset = RX_RING_OFFSET,
			.rx_ring_size = RX_RING_SIZE,
			.period_size = DMA_PERIOD_SIZE,
			.dma_running = dma_running ? 1 : 0,
			.tx_bytes = tx_period_count * DMA_PERIOD_SIZE,
			.rx_bytes = rx_period_count * DMA_PERIOD_SIZE,
		};
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}

	case SRAM_IOC_DMA_DIAG:
		return dma_diag_test();

	default:
		return -ENOTTY;
	}
}

static const struct file_operations rp1_pio_sram_fops = {
	.owner          = THIS_MODULE,
	.open           = rp1_pio_sram_open,
	.release        = rp1_pio_sram_release,
	.mmap           = rp1_pio_sram_mmap,
	.unlocked_ioctl = rp1_pio_sram_ioctl,
};

static struct miscdevice rp1_pio_sram_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "rp1_pio_sram",
	.fops  = &rp1_pio_sram_fops,
};

/* ─── Module init/exit ───────────────────────────────────── */

static int __init rp1_pio_sram_init(void)
{
	int ret;

	pr_info("rp1_pio_sram: initializing\n");

	ret = acquire_dma_channels();
	if (ret) {
		pr_err("rp1_pio_sram: DMA channels unavailable: %d\n", ret);
		return ret;
	}

	ret = alloc_dma_buffer();
	if (ret) {
		release_dma_channels();
		return ret;
	}

	ret = misc_register(&rp1_pio_sram_misc);
	if (ret) {
		pr_err("rp1_pio_sram: misc_register failed: %d\n", ret);
		free_dma_buffer();
		release_dma_channels();
		return ret;
	}

	pr_info("rp1_pio_sram: ready (TX %u B + RX %u B, period %u B)\n",
		TX_RING_SIZE, RX_RING_SIZE, DMA_PERIOD_SIZE);

	return 0;
}

static void __exit rp1_pio_sram_exit(void)
{
	stop_cyclic_dma();
	misc_deregister(&rp1_pio_sram_misc);
	free_dma_buffer();
	release_dma_channels();

	if (pio_client) {
		rp1_pio_close(pio_client);
		pio_client = NULL;
	}

	pr_info("rp1_pio_sram: unloaded\n");
}

module_init(rp1_pio_sram_init);
module_exit(rp1_pio_sram_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tim Ansell");
MODULE_DESCRIPTION("RP1 PIO Cyclic DMA Benchmark");
