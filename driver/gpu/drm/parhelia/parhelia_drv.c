// SPDX-License-Identifier: GPL-2.0-only
#include <linux/aperture.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/drm_drv.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_vblank.h>

#include "parhelia_drv.h"

#define DRIVER_NAME "parhelia"
#define DRIVER_DESC "Matrox Parhelia"

static bool reset_on_probe;
module_param(reset_on_probe, bool, 0400);
MODULE_PARM_DESC(reset_on_probe, "Reset PCI function before initialization (bring-up only)");

DEFINE_DRM_GEM_FOPS(parhelia_fops);

static void parhelia_postclose(struct drm_device *drm, struct drm_file *file)
{
	parhelia_submit_postclose(to_parhelia(drm), file);
}

static const struct drm_ioctl_desc parhelia_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PARHELIA_GET_PARAM, parhelia_get_param_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_SUBMIT, parhelia_submit_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_CREATE_RENDER, parhelia_create_render_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_CREATE_VERTEX, parhelia_create_vertex_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_WAIT_FENCE, parhelia_wait_fence_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_GET_BO_INFO, parhelia_get_bo_info_ioctl,
			  DRM_RENDER_ALLOW),
};

static const struct drm_driver parhelia_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_RENDER | DRIVER_ATOMIC,
	.fops = &parhelia_fops,
	.postclose = parhelia_postclose,
	.ioctls = parhelia_ioctls,
	.num_ioctls = ARRAY_SIZE(parhelia_ioctls),
	.gem_create_object = parhelia_gem_create_object,
	.debugfs_init = drm_vram_mm_debugfs_init,
	.dumb_create = parhelia_dumb_create,
	.dumb_map_offset = drm_gem_ttm_dumb_map_offset,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = 1,
	.minor = 0,
};

static const struct parhelia_chip_info parhelia_512_info = {
	.device = PARHELIA_512, .name = "Parhelia-512",
	.tile_layout = PARHELIA_TILE_64X32_8K, .texture_units = 4,
	.texture_pipe_mode = PARHELIA_TEXTURE_PIPES_PAIRED,
	.mc_family = PARHELIA_MC_HSLPAR, .clock_family = PARHELIA_CLOCK_HSLPAR,
	.output_family = PARHELIA_OUTPUT_ORIGINAL,
	.reset_family = PARHELIA_RESET_HSLPAR,
	.max_pixel_clock_khz = 300000,
};

static const struct parhelia_chip_info parhelia_8x_info = {
	.device = PARHELIA_8X, .name = "Parhelia-8X",
	.tile_layout = PARHELIA_TILE_64X32_8K, .texture_units = 4,
	.texture_pipe_mode = PARHELIA_TEXTURE_PIPES_PAIRED,
	.mc_family = PARHELIA_MC_HSLPAR, .clock_family = PARHELIA_CLOCK_HSLPAR,
	.output_family = PARHELIA_OUTPUT_ORIGINAL,
	.reset_family = PARHELIA_RESET_HSLPAR,
	.max_pixel_clock_khz = 300000,
};

static const struct parhelia_chip_info parhelia_lx_info = {
	.device = PARHELIA_LX, .name = "Parhelia-LX/Sundog",
	.tile_layout = PARHELIA_TILE_64X16_4K, .texture_units = 2,
	.texture_pipe_mode = PARHELIA_TEXTURE_PIPES_SINGLE,
	.mc_family = PARHELIA_MC_HSLPAR, .clock_family = PARHELIA_CLOCK_HSLPAR,
	.output_family = PARHELIA_OUTPUT_LX,
	.reset_family = PARHELIA_RESET_HSLPAR,
	.max_pixel_clock_khz = 300000,
};

static const struct parhelia_chip_info parhelia_lx2_info = {
	.device = PARHELIA_LX2, .name = "Parhelia-LX/Sunex",
	.tile_layout = PARHELIA_TILE_64X16_4K, .texture_units = 2,
	.texture_pipe_mode = PARHELIA_TEXTURE_PIPES_SINGLE,
	.mc_family = PARHELIA_MC_HSLPAR, .clock_family = PARHELIA_CLOCK_HSLPAR,
	.output_family = PARHELIA_OUTPUT_LX,
	.reset_family = PARHELIA_RESET_SUNEX,
	.max_pixel_clock_khz = 300000,
};

static const struct parhelia_chip_info parhelia_p690_info = {
	.device = PARHELIA_P690, .name = "P690/SUN90",
	.tile_layout = PARHELIA_TILE_64X16_4K, .texture_units = 2,
	.texture_pipe_mode = PARHELIA_TEXTURE_PIPES_SINGLE,
	.mc_family = PARHELIA_MC_SUN90, .clock_family = PARHELIA_CLOCK_SUN90,
	.output_family = PARHELIA_OUTPUT_P690,
	.reset_family = PARHELIA_RESET_SUNEX,
	.max_pixel_clock_khz = 300000,
};

static int parhelia_validate_chip_info(struct pci_dev *pci,
				       const struct parhelia_chip_info *chip)
{
	if (!chip || chip->device != pci->device || !chip->name ||
	    chip->tile_layout > PARHELIA_TILE_64X16_4K ||
	    chip->texture_pipe_mode > PARHELIA_TEXTURE_PIPES_SINGLE ||
	    chip->mc_family > PARHELIA_MC_SUN90 ||
	    chip->clock_family > PARHELIA_CLOCK_SUN90 ||
	    chip->output_family > PARHELIA_OUTPUT_P690 ||
	    chip->reset_family > PARHELIA_RESET_SUNEX ||
	    !chip->texture_units || !chip->max_pixel_clock_khz)
		return -EINVAL;

	/* These are architectural pairings, not optional feature guesses.  Fail
	 * closed rather than letting a corrupt/new table entry select a foreign
	 * memory-controller or clock register block. */
	if (chip->device == PARHELIA_P690) {
		if (chip->mc_family != PARHELIA_MC_SUN90 ||
		    chip->clock_family != PARHELIA_CLOCK_SUN90 ||
		    chip->output_family != PARHELIA_OUTPUT_P690 ||
		    chip->reset_family != PARHELIA_RESET_SUNEX)
			return -EINVAL;
	} else if (chip->mc_family != PARHELIA_MC_HSLPAR ||
		   chip->clock_family != PARHELIA_CLOCK_HSLPAR) {
		return -EINVAL;
	}

	if ((chip->device == PARHELIA_512 || chip->device == PARHELIA_8X) !=
	    (chip->output_family == PARHELIA_OUTPUT_ORIGINAL))
		return -EINVAL;
	if ((chip->device == PARHELIA_512 || chip->device == PARHELIA_8X) !=
	    (chip->texture_pipe_mode == PARHELIA_TEXTURE_PIPES_PAIRED))
		return -EINVAL;

	return 0;
}

#define PARHELIA_PCI_INFO(_device, _info) \
	{ PCI_DEVICE(PARHELIA_VENDOR, (_device)), \
	  .driver_data = (kernel_ulong_t)&(_info) }

static const struct pci_device_id parhelia_pci_table[] = {
	PARHELIA_PCI_INFO(PARHELIA_512, parhelia_512_info),
	PARHELIA_PCI_INFO(PARHELIA_8X, parhelia_8x_info),
	PARHELIA_PCI_INFO(PARHELIA_LX, parhelia_lx_info),
	PARHELIA_PCI_INFO(PARHELIA_LX2, parhelia_lx2_info),
	PARHELIA_PCI_INFO(PARHELIA_P690, parhelia_p690_info),
	{ }
};
MODULE_DEVICE_TABLE(pci, parhelia_pci_table);

static int parhelia_pci_probe(struct pci_dev *pci,
			      const struct pci_device_id *ent)
{
	struct parhelia_device *pdev;
	struct drm_device *drm;
	unsigned int mmio_bar;
	int ret;

	ret = aperture_remove_conflicting_pci_devices(pci, DRIVER_NAME);
	if (ret)
		return ret;

	ret = pcim_enable_device(pci);
	if (ret)
		return ret;
	pci_clear_master(pci);

	ret = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pci->dev, ret, "32-bit DMA is unavailable\n");

	/* On the measured 0528, PMCSR is at 0xe0: the recovered vendor
	 * 3 -> 0 strobe is the standard D3hot -> D0 reset. Let PCI core choose
	 * the supported method and preserve configuration. No live DRM clients
	 * or driver-owned DMA allocations exist at this point.
	 */
	if (reset_on_probe) {
		/* Driver core already holds the device lock during probe. */
		ret = pci_reset_function_locked(pci);
		if (ret)
			return dev_err_probe(&pci->dev, ret, "PCI function reset failed\n");
		dev_info(&pci->dev, "PCI function reset completed\n");
	}

	pdev = devm_drm_dev_alloc(&pci->dev, &parhelia_driver,
				 struct parhelia_device, drm);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	drm = &pdev->drm;
	pdev->pdev = pci;
	pdev->chip = (const struct parhelia_chip_info *)ent->driver_data;
	ret = parhelia_validate_chip_info(pci, pdev->chip);
	if (ret)
		return dev_err_probe(&pci->dev, -ENODEV,
				     "missing or inconsistent silicon profile\n");
	pdev->vram_size = pci_resource_len(pci, 0);
	/* BAR0 is a 64-bit VRAM resource on P690, so BAR1 is its consumed upper
	 * half and the register aperture moves to BAR2.  Earlier Parhelia parts
	 * expose the register aperture directly as BAR1. */
	mmio_bar = pci_resource_len(pci, 1) ? 1 : 2;
	if (!(pci_resource_flags(pci, 0) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pci, mmio_bar) & IORESOURCE_MEM) ||
	    !pdev->vram_size || !pci_resource_len(pci, mmio_bar))
		return dev_err_probe(&pci->dev, -ENODEV,
				     "invalid VRAM/MMIO BAR layout\n");
	/* Read the option ROM before reserving/mapping the VRAM and MMIO BARs;
	 * pci_map_rom() may temporarily enable the legacy shadow aperture and the
	 * PCI resource checker otherwise reports overlapping active mappings. */
	parhelia_bios_init(pdev);
	pdev->vram = pcim_iomap(pci, 0, 0);
	pdev->mmio = pcim_iomap(pci, mmio_bar, 0);
	if (!pdev->vram || !pdev->mmio)
		return dev_err_probe(&pci->dev, -ENOMEM, "cannot map BARs\n");
	dev_info(&pci->dev,
		 "%s: mapped %pa-byte VRAM BAR0 and MMIO BAR%u, tile layout %u, "
		 "MC %s, clock %s, output %u, reset %s\n",
		 pdev->chip->name, &pdev->vram_size, mmio_bar,
		 pdev->chip->tile_layout,
		 pdev->chip->mc_family == PARHELIA_MC_SUN90 ? "SUN90" : "HSLPAR",
		 pdev->chip->clock_family == PARHELIA_CLOCK_SUN90 ? "SUN90" : "HSLPAR",
		 pdev->chip->output_family,
		 pdev->chip->reset_family == PARHELIA_RESET_SUNEX ? "SUNEX" : "HSLPAR");
	/* POST already established DRAM timings.  HSLPAR and SUN90 use unrelated
	 * memory-controller blocks; touching either without decoded PInS timing data
	 * is destructive.  Keep both families firmware-owned and only consume the
	 * BAR size and the chip-specific surface layout. */
	dev_info(&pci->dev, "preserving firmware-programmed %s memory controller\n",
		 pdev->chip->mc_family == PARHELIA_MC_SUN90 ? "SUN90" : "HSLPAR");

	/* Parhelia has no GPU VM: every scanout object stays pinned in BAR0. */
	ret = drmm_vram_helper_init(drm, pci_resource_start(pci, 0),
				   pdev->vram_size);
	if (ret)
		return ret;

	ret = drmm_mutex_init(drm, &pdev->submit_lock);
	if (ret)
		return ret;
	ret = drmm_mutex_init(drm, &pdev->aperture_lock);
	if (ret)
		return ret;
	bitmap_zero(pdev->aperture_slots, PARHELIA_HOST_APERTURE_SLOTS);
	__set_bit(0, pdev->aperture_slots);
	INIT_LIST_HEAD(&pdev->vertex_bos);
	pdev->primary_cpu = dmam_alloc_coherent(&pci->dev,
			PARHELIA_PRIMARY_DWORDS * sizeof(u32),
			&pdev->primary_bus, GFP_KERNEL);
	if (!pdev->primary_cpu)
		return -ENOMEM;
	pdev->vertex_cpu = dmam_alloc_coherent(&pci->dev,
			PARHELIA_VERTEX_DWORDS * sizeof(u32), &pdev->vertex_bus,
			GFP_KERNEL);
	if (!pdev->vertex_cpu)
		return -ENOMEM;
	pdev->writeback_cpu = dmam_alloc_coherent(&pci->dev, PAGE_SIZE,
			&pdev->writeback_bus, GFP_KERNEL);
	if (!pdev->writeback_cpu)
		return -ENOMEM;
	ret = parhelia_submit_init(pdev);
	if (ret)
		return ret;

	ret = parhelia_modeset_init(pdev);
	if (ret)
		return ret;
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	/* The vendor IRQ block is W1C. Keep every source masked until DRM asks
	 * for head0 vblank; shared ownership is required on legacy PCI. */
	parhelia_write(pdev, PARHELIA_MMIO_IRQ_MASK, 0);
	parhelia_write(pdev, PARHELIA_MMIO_IRQ_ACK, ~0u);
	ret = request_irq(pci->irq, parhelia_irq_handler, IRQF_SHARED,
			  DRIVER_NAME, pdev);
	if (ret)
		return dev_err_probe(&pci->dev, ret, "cannot request IRQ %u\n", pci->irq);
	pdev->irq_installed = true;

	/* Initialize and populate the WARP store before exposing either DRM node.
	 * Userspace only selects the fixed entry tokens advertised by GET_PARAM. */
	ret = parhelia_engine_init(pdev);
	if (ret) {
		free_irq(pci->irq, pdev);
		pdev->irq_installed = false;
		return dev_err_probe(&pci->dev, ret, "cannot initialize WARP engine\n");
	}
	mutex_lock(&pdev->aperture_lock);
	pdev->aperture_live = pdev->raw_aperture_owned;
	mutex_unlock(&pdev->aperture_lock);

	/* Engine initialization enabled mastering only after installing our span. */
	ret = drm_dev_register(drm, 0);
	if (ret) {
		free_irq(pci->irq, pdev);
		pci_clear_master(pci);
		return ret;
	}

	pci_set_drvdata(pci, drm);
	return 0;
}

static void parhelia_pci_remove(struct pci_dev *pci)
{
	struct drm_device *drm = pci_get_drvdata(pci);

	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
	if (to_parhelia(drm)->irq_installed) {
		parhelia_write(to_parhelia(drm), PARHELIA_MMIO_IRQ_MASK, 0);
		free_irq(pci->irq, to_parhelia(drm));
	}
	parhelia_submit_fini(to_parhelia(drm));
	parhelia_host_aperture_fini(to_parhelia(drm));
}

static struct pci_driver parhelia_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = parhelia_pci_table,
	.probe = parhelia_pci_probe,
	.remove = parhelia_pci_remove,
};
module_pci_driver(parhelia_pci_driver);

MODULE_AUTHOR("René Rebe <rene@exactco.de>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
