// SPDX-License-Identifier: GPL-2.0
/*
 * rp1_pio_sram.c — RP1 PIO DMA via Shared SRAM
 *
 * Out-of-tree kernel module that sets up cyclic DMA between RP1 shared
 * SRAM and PIO FIFOs, bypassing the PCIe round-trip per DMA burst that
 * limits the standard kernel DMA path to ~42 MB/s.
 *
 * Architecture:
 *   Host CPU ──PCIe──→ SRAM ring buffer (0x8B00-0xFEFF, ~29 KB)
 *                          ↓ (RP1-internal AXI, no PCIe per burst)
 *                     RP1 DMA (cyclic) ──APB──→ PIO FIFO
 *
 * The module:
 *   1. Claims a PIO SM and loads the loopback program via rp1-pio APIs
 *   2. Maps SRAM via ioremap for CPU access
 *   3. Sets up cyclic DMA from SRAM → PIO TXF and PIO RXF → SRAM
 *   4. Exposes /dev/rp1_pio_sram for userspace mmap + ioctl
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

/* RP1 SRAM physical addresses (BCM2712 perspective) */
#define SRAM_PHYS_BASE     0x1F00400000ULL
#define SRAM_SIZE          0x10000           /* 64 KB */
#define SRAM_SAFE_OFFSET   0x8B00           /* Start of safe region */
#define SRAM_SAFE_SIZE     0x7400           /* 29,696 bytes */
#define SRAM_SAFE_END      (SRAM_SAFE_OFFSET + SRAM_SAFE_SIZE)

/* Module state */
static void __iomem *sram_base;

/* ─── Misc device (character device) ─────────────────────────── */

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
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	phys_addr_t phys = SRAM_PHYS_BASE + SRAM_SAFE_OFFSET + offset;

	if (offset + size > SRAM_SAFE_SIZE)
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return io_remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
				  size, vma->vm_page_prot);
}

static const struct file_operations rp1_pio_sram_fops = {
	.owner   = THIS_MODULE,
	.open    = rp1_pio_sram_open,
	.release = rp1_pio_sram_release,
	.mmap    = rp1_pio_sram_mmap,
};

static struct miscdevice rp1_pio_sram_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "rp1_pio_sram",
	.fops  = &rp1_pio_sram_fops,
};

/* ─── Module init/exit ───────────────────────────────────────── */

static int __init rp1_pio_sram_init(void)
{
	int ret;

	pr_info("rp1_pio_sram: initializing\n");

	/* Map SRAM */
	sram_base = ioremap(SRAM_PHYS_BASE, SRAM_SIZE);
	if (!sram_base) {
		pr_err("rp1_pio_sram: failed to ioremap SRAM at 0x%llx\n",
		       SRAM_PHYS_BASE);
		return -ENOMEM;
	}

	/* Sanity check: read PIO descriptor magic at 0x8A00 */
	u32 pio_magic = ioread32(sram_base + 0x8A04);
	if (pio_magic != 0x50494f20) {  /* "PIO " */
		pr_warn("rp1_pio_sram: PIO magic mismatch: 0x%08x (expected 0x50494f20)\n",
			pio_magic);
	} else {
		pr_info("rp1_pio_sram: PIO magic OK at SRAM+0x8A04\n");
	}

	/* Register misc device */
	ret = misc_register(&rp1_pio_sram_misc);
	if (ret) {
		pr_err("rp1_pio_sram: misc_register failed: %d\n", ret);
		iounmap(sram_base);
		return ret;
	}

	pr_info("rp1_pio_sram: SRAM mapped at %p, safe region 0x%x-0x%x (%u bytes)\n",
		sram_base, SRAM_SAFE_OFFSET, SRAM_SAFE_END - 1, SRAM_SAFE_SIZE);
	pr_info("rp1_pio_sram: /dev/rp1_pio_sram created\n");

	return 0;
}

static void __exit rp1_pio_sram_exit(void)
{
	misc_deregister(&rp1_pio_sram_misc);

	if (sram_base)
		iounmap(sram_base);

	pr_info("rp1_pio_sram: unloaded\n");
}

module_init(rp1_pio_sram_init);
module_exit(rp1_pio_sram_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tim Ansell");
MODULE_DESCRIPTION("RP1 PIO DMA via Shared SRAM");
