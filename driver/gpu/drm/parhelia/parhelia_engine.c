// SPDX-License-Identifier: GPL-2.0-only
/* Parhelia 3D engine bring-up.
 *
 * Recovered from xf86-video-parhelia/src/parhelia3d/p3d_init.c. This resets
 * both display timing blocks and must run before exposing KMS, not while
 * scanout is active. The PInS-driven memory-clock reset is not implemented;
 * host-aperture initialization is opt-in until its KMS mapping contract is
 * verified. Cold-boot independence still needs hardware verification.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/string.h>

#include "parhelia_drv.h"

#define P3D_R_EOBTAG		0x12b8
#define P3D_R_WATERMARK		0x12b0

/* The recovered aperture baseline has been verified on the P690 with KMS
 * scanout and independent colour, depth and texture render targets.  It must
 * be established at probe time: otherwise CREATE_RENDER cannot safely expose
 * tiled GPU surfaces after a normal reboot. */
static bool init_aperture = true;
module_param(init_aperture, bool, 0400);
MODULE_PARM_DESC(init_aperture, "Initialize owned host apertures before DRM registration");

static void parhelia_rmw(struct parhelia_device *pdev, u32 reg,
			  u32 and_mask, u32 or_mask)
{
	parhelia_write(pdev, reg, (parhelia_read(pdev, reg) & and_mask) | or_mask);
}

static void parhelia_apply_vid_reset(struct parhelia_device *pdev, unsigned int head)
{
	u32 clk = head ? 0x1c04 : 0x1c00;
	u32 dct = 0x1900 + head * 0x200;
	u32 value;

	value = parhelia_read(pdev, 0x1c00);
	if (value & 0x20)
		parhelia_write(pdev, 0x1c00, value & ~0x20u);
	value = parhelia_read(pdev, 0x1c04);
	if (value & 0x20)
		parhelia_write(pdev, 0x1c04, value & ~0x20u);

	parhelia_rmw(pdev, dct, ~0u, 0x00030000u);
	parhelia_rmw(pdev, clk, ~0u, 0x10u);
	value = parhelia_read(pdev, 0x1c00);
	parhelia_write(pdev, 0x1c00, value & ~BIT(24));
	udelay(10);
	parhelia_write(pdev, 0x1c00, value | BIT(24));
	parhelia_write(pdev, 0x1c00, value & ~BIT(24));
	parhelia_rmw(pdev, clk, ~0x10u, 0);
	parhelia_rmw(pdev, dct, ~0x00030000u, 0);
}

int parhelia_engine_init(struct parhelia_device *pdev)
{
	int ret;

	parhelia_root_trace_check(pdev, "engine_init:enter",
				  pdev->engine_initialized, init_aperture, 0);
	if (pdev->engine_initialized)
		return 0;
	/* Reset and WARP initialization write engine state outside normal submit. */
	parhelia_state_shadow_invalidate(pdev, "engine initialization");

	parhelia_apply_vid_reset(pdev, 0);
	parhelia_apply_vid_reset(pdev, 1);
	parhelia_root_trace_check(pdev, "engine_init:video_reset", 0, 0, 0);
	parhelia_rmw(pdev, 0x1c00, ~0u, 0x00808020u);
	parhelia_rmw(pdev, 0x1c04, ~0u, 0x00000020u);
	udelay(10);
	parhelia_write(pdev, 0x1c08, 2);
	parhelia_write(pdev, 0x1c24, 7);
	parhelia_write(pdev, 0x08fc, 0);
	if (pdev->pdev->device == PARHELIA_512 || pdev->pdev->device == PARHELIA_8X)
		parhelia_write(pdev, 0x1c58, 0x40);
	parhelia_rmw(pdev, 0x0920, 0x7fffffffu, 0);
	parhelia_write(pdev, 0x10b0, 0x40);
	parhelia_rmw(pdev, 0x1c0c, ~0u, 0x0808u);
	parhelia_rmw(pdev, 0x1c10, ~0u, 0x0808u);
	parhelia_write(pdev, P3D_R_EOBTAG, 0x00070601u);
	parhelia_write(pdev, P3D_R_WATERMARK, 0);
	if (init_aperture) {
		unsigned int i;

		/* Exact P3D_INIT_APERTURE sequence from the original C core.
		 * This is a CPU BAR-view table, not GPU surface tiling state.
		 * No clients or KMS planes have been exposed at this point.
		 */
		for (i = 1; i <= 0x13; i++)
			parhelia_rmw(pdev, i << 4, 0x0fffffffu, 0);
		parhelia_write(pdev, 0x0000, 0x10000000);
		parhelia_write(pdev, 0x0004, 0x00000000);
		parhelia_write(pdev, 0x0008, 0x10000020);
		parhelia_write(pdev, 0x000c, 0x00010001);
		if (parhelia_read(pdev, 0) != 0x10000000 || parhelia_read(pdev, 4) ||
		    parhelia_read(pdev, 8) != 0x10000020 ||
		    parhelia_read(pdev, 12) != 0x00010001)
			return -EIO;
		pdev->raw_aperture_owned = true;
		dev_info(&pdev->pdev->dev, "Owned host-aperture baseline initialized: %08x %08x %08x %08x\n",
			 parhelia_read(pdev, 0), parhelia_read(pdev, 4),
			 parhelia_read(pdev, 8), parhelia_read(pdev, 12));
	}
	parhelia_root_trace_check(pdev, "engine_init:aperture",
				  pdev->raw_aperture_owned, 0, 0);
	parhelia_rmw(pdev, 0x0300, 0xe0e0ff3fu, 0x100u);
	parhelia_write(pdev, 0x0308,
		       pdev->pdev->device == PARHELIA_8X ? 0x83u : 0x01u);

	/* A page-aligned DMA32 status area is mandatory for DWGSYNC fences. */
	memset(pdev->writeback_cpu, 0, PAGE_SIZE);
	dma_wmb();
	parhelia_write(pdev, PARHELIA_MMIO_WRITEBACK,
		       lower_32_bits(pdev->writeback_bus) & PAGE_MASK);
	/* The open-time trace arms an EMPTY primary span before WARP setup.
	 * Install only our own DMA addresses before enabling PCI bus mastering.
	 * A zero-length span requests no command fetch, but initializes the
	 * primary completion condition. Still require the full idle gate below.
	 */
	parhelia_write(pdev, PARHELIA_MMIO_RING_START,
		       lower_32_bits(pdev->primary_bus));
	parhelia_write(pdev, PARHELIA_MMIO_RING_END,
		       lower_32_bits(pdev->primary_bus) | 3);
	pci_set_master(pdev->pdev);
	parhelia_root_trace_check(pdev, "engine_init:ring_armed",
				  lower_32_bits(pdev->primary_bus),
				  lower_32_bits(pdev->writeback_bus), 0);
	ret = parhelia_warp_init(pdev);
	parhelia_root_trace_check(pdev, "engine_init:warp_exit",
				  (u64)(s64)ret, 0, 0);
	if (ret) {
		pci_clear_master(pdev->pdev);
		return ret;
	}
	pdev->engine_initialized = true;
	parhelia_root_trace_check(pdev, "engine_init:exit", 1, 0, 0);
	return 0;
}
