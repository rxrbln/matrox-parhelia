// SPDX-License-Identifier: GPL-2.0-only
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/overflow.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_ioctl.h>

#include "parhelia_drv.h"

#define PARHELIA_HOST_FORMAT_XRGB8888	0x30

static const struct drm_gem_object_funcs parhelia_host_object_funcs;
static const struct drm_gem_object_funcs parhelia_raw_object_funcs;

struct drm_gem_object *parhelia_gem_create_object(struct drm_device *drm, size_t size)
{
	struct parhelia_bo *bo;

	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->layout = PARHELIA_BO_LAYOUT_LINEAR;
	bo->gbo.bo.base.funcs = &parhelia_raw_object_funcs;
	return &bo->gbo.bo.base;
}

void parhelia_bo_set_layout(struct drm_gem_vram_object *gbo, u8 layout,
			    u32 width, u32 height, u32 pitch)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);

	bo->layout = layout;
	bo->width = width;
	bo->height = height;
	bo->pitch = pitch;
}

bool parhelia_bo_is_native(const struct parhelia_bo *bo)
{
	return bo->layout == PARHELIA_BO_LAYOUT_NATIVE_64X32 ||
	       bo->layout == PARHELIA_BO_LAYOUT_NATIVE_64X16;
}

bool parhelia_gem_is_owned(const struct drm_gem_object *gem)
{
	return gem->funcs == &parhelia_raw_object_funcs ||
	       gem->funcs == &parhelia_host_object_funcs;
}

static void parhelia_host_descriptor_disable(struct parhelia_bo *bo)
{
	struct parhelia_device *pdev = to_parhelia(bo->gbo.bo.base.dev);

	mutex_lock(&pdev->aperture_lock);
	if (bo->host_aperture) {
		parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(bo->aperture_slot), 0);
		parhelia_read(pdev,
			      PARHELIA_HOST_APERTURE_SLOT(bo->aperture_slot));
		__clear_bit(bo->aperture_slot, pdev->aperture_slots);
		bo->host_aperture = false;
	}
	mutex_unlock(&pdev->aperture_lock);
}

static void parhelia_host_object_free(struct drm_gem_object *gem)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);

	/* A VMA owns a GEM reference, so this cannot run while the translated BAR
	 * range remains accessible.  Revoke the descriptor before either address
	 * range can return to TTM.
	 */
	parhelia_host_descriptor_disable(bo);
	parhelia_vram_unpin(&bo->gbo);
	if (bo->aperture_reservation) {
		parhelia_vram_unpin(bo->aperture_reservation);
		drm_gem_vram_put(bo->aperture_reservation);
		bo->aperture_reservation = NULL;
	}
	drm_gem_vram_put(&bo->gbo);
}

static void parhelia_raw_object_free(struct drm_gem_object *gem)
{
	drm_gem_vram_put(drm_gem_vram_of_gem(gem));
}

static const struct drm_gem_object_funcs parhelia_raw_object_funcs = {
	.free = parhelia_raw_object_free,
	.vmap = drm_gem_ttm_vmap,
	.vunmap = drm_gem_ttm_vunmap,
	.mmap = drm_gem_ttm_mmap,
	.print_info = drm_gem_ttm_print_info,
};

static int parhelia_host_object_vmap(struct drm_gem_object *gem,
				     struct iosys_map *map)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);

	if (!bo->host_aperture)
		return -EINVAL;
	iosys_map_set_vaddr_iomem(map, (u8 __iomem *)pdev->vram + bo->aperture_offset);
	return 0;
}

static void parhelia_host_object_vunmap(struct drm_gem_object *gem,
					struct iosys_map *map)
{
	iosys_map_clear(map);
}

static void parhelia_host_vm_open(struct vm_area_struct *vma)
{
	drm_gem_vm_open(vma);
}

static void parhelia_host_vm_close(struct vm_area_struct *vma)
{
	/* Publish outstanding WC stores before the mapping can release its final
	 * GEM reference and invalidate the descriptor.
	 */
	wmb();
	drm_gem_vm_close(vma);
}

static const struct vm_operations_struct parhelia_host_vm_ops = {
	.open = parhelia_host_vm_open,
	.close = parhelia_host_vm_close,
};

static int parhelia_host_object_mmap(struct drm_gem_object *gem,
				     struct vm_area_struct *vma)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	resource_size_t base;

	if (!bo->host_aperture || vma->vm_end - vma->vm_start > gem->size)
		return -EINVAL;

	base = pci_resource_start(pdev->pdev, 0) + bo->aperture_offset;
	/* drm_gem_mmap() has already consumed the fake GEM offset.  The physical
	 * mapping begins at the descriptor's BAR0 aperture range.
	 */
	vma->vm_pgoff = 0;
	vma->vm_ops = &parhelia_host_vm_ops;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return vm_iomap_memory(vma, base, gem->size);
}

static const struct drm_gem_object_funcs parhelia_host_object_funcs = {
	.free = parhelia_host_object_free,
	.vmap = parhelia_host_object_vmap,
	.vunmap = parhelia_host_object_vunmap,
	.mmap = parhelia_host_object_mmap,
	.print_info = drm_gem_ttm_print_info,
};

static int parhelia_host_descriptor_enable(struct parhelia_device *pdev,
					   struct parhelia_bo *bo,
					   u32 surface_offset)
{
	u32 reg = PARHELIA_HOST_APERTURE_SLOT(bo->aperture_slot);
	u32 ctrl = (PARHELIA_HOST_FORMAT_XRGB8888 << 24) |
		   (3u << 16) | bo->width;
	u32 dims = BIT(16) | bo->height;

	/* Descriptor words 1..3 are private until VALID is written last. */
	parhelia_write(pdev, reg, 0);
	parhelia_write(pdev, reg + 4, surface_offset);
	parhelia_write(pdev, reg + 8, ctrl);
	parhelia_write(pdev, reg + 12, dims);
	/* Publish the surface description before making it valid. */
	wmb();
	parhelia_write(pdev, reg,
		       PARHELIA_HOST_APERTURE_VALID | bo->aperture_offset);
	parhelia_read(pdev, reg);
	return 0;
}

int parhelia_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args)
{
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_gem_vram_object *reservation = NULL, *gbo = NULL;
	struct parhelia_bo *bo;
	unsigned long align_pages, native_align_pages, slot;
	u32 pitch_pixels, alloc_height;
	s64 aperture_offset, surface_offset;
	size_t size, reservation_size;
	u32 handle;
	int ret;

	if (!args->width || !args->height || args->width > 2048 ||
	    args->height > 2048 || args->bpp != 32 || !pdev->raw_aperture_owned)
		return -EINVAL;
	pitch_pixels = roundup_pow_of_two(max_t(u32, args->width, 64));
	alloc_height = ALIGN(args->height,
			     pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ?
			     16 : 32);
	if (check_mul_overflow((size_t)pitch_pixels * 4, (size_t)alloc_height,
			       &size))
		return -EOVERFLOW;
	size = PAGE_ALIGN(size);
	reservation_size = ALIGN(size, PARHELIA_HOST_APERTURE_ALIGN) +
			   PARHELIA_HOST_APERTURE_ALIGN;
	align_pages = PARHELIA_HOST_APERTURE_ALIGN / PAGE_SIZE;

	mutex_lock(&pdev->aperture_lock);
	slot = find_next_zero_bit(pdev->aperture_slots,
				  PARHELIA_HOST_APERTURE_SLOTS, 1);
	if (slot == PARHELIA_HOST_APERTURE_SLOTS) {
		ret = -ENOSPC;
		goto out_unlock;
	}
	__set_bit(slot, pdev->aperture_slots);

	/* Reserve the BAR coordinates before allocating the aliased surface.  TTM
	 * then cannot place a raw-mapped BO where translated host accesses land.
	 */
	reservation = drm_gem_vram_create(drm, reservation_size, align_pages);
	if (IS_ERR(reservation)) {
		ret = PTR_ERR(reservation);
		reservation = NULL;
		goto out_slot;
	}
	ret = parhelia_vram_pin(reservation);
	if (ret)
		goto out_reservation;
	aperture_offset = drm_gem_vram_offset(reservation);
	if (aperture_offset < 0 || aperture_offset >= PARHELIA_HOST_APERTURE_VALID ||
	    !IS_ALIGNED(aperture_offset, PARHELIA_HOST_APERTURE_ALIGN)) {
		ret = -ERANGE;
		goto out_unpin_reservation;
	}

	native_align_pages = pdev->chip->tile_layout == PARHELIA_TILE_64X32_8K ? 2 : 1;
	gbo = drm_gem_vram_create(drm, size, native_align_pages);
	if (IS_ERR(gbo)) {
		ret = PTR_ERR(gbo);
		gbo = NULL;
		goto out_unpin_reservation;
	}
	ret = parhelia_vram_pin(gbo);
	if (ret)
		goto out_gbo;
	surface_offset = drm_gem_vram_offset(gbo);
	if (surface_offset < 0 || surface_offset > U32_MAX) {
		ret = -ERANGE;
		goto out_unpin_gbo;
	}

	bo = container_of(gbo, struct parhelia_bo, gbo);
	bo->aperture_reservation = reservation;
	bo->width = args->width;
	bo->height = args->height;
	bo->pitch = pitch_pixels * 4;
	bo->aperture_offset = aperture_offset;
	bo->aperture_slot = slot;
	bo->layout = pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ?
		PARHELIA_BO_LAYOUT_NATIVE_64X16 : PARHELIA_BO_LAYOUT_NATIVE_64X32;
	bo->host_aperture = true;
	gbo->bo.base.funcs = &parhelia_host_object_funcs;
	parhelia_host_descriptor_enable(pdev, bo, surface_offset);
	mutex_unlock(&pdev->aperture_lock);

	ret = drm_gem_handle_create(file, &gbo->bo.base, &handle);
	if (ret) {
		drm_gem_object_put(&gbo->bo.base);
		return ret;
	}
	drm_gem_object_put(&gbo->bo.base);
	args->handle = handle;
	args->pitch = bo->pitch;
	args->size = size;
	return 0;

out_unpin_gbo:
	parhelia_vram_unpin(gbo);
out_gbo:
	drm_gem_object_put(&gbo->bo.base);
out_unpin_reservation:
	parhelia_vram_unpin(reservation);
out_reservation:
	drm_gem_object_put(&reservation->bo.base);
out_slot:
	__clear_bit(slot, pdev->aperture_slots);
out_unlock:
	mutex_unlock(&pdev->aperture_lock);
	return ret;
}

int parhelia_get_bo_info_ioctl(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct drm_parhelia_get_bo_info *args = data;
	struct drm_gem_object *gem;
	struct parhelia_bo *bo;

	if (!args->handle || args->flags || args->layout || args->width ||
	    args->height || args->pitch || args->pad || args->size)
		return -EINVAL;
	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;
	if (!parhelia_gem_is_owned(gem)) {
		drm_gem_object_put(gem);
		return -EINVAL;
	}
	bo = to_parhelia_bo(gem);
	args->flags = bo->host_aperture ? PARHELIA_BO_INFO_HOST_APERTURE : 0;
	args->layout = bo->layout;
	args->width = bo->width;
	args->height = bo->height;
	args->pitch = bo->pitch;
	args->size = gem->size;
	drm_gem_object_put(gem);
	return 0;
}
