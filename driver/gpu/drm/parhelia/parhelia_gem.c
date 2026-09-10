// SPDX-License-Identifier: GPL-2.0-only
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_ioctl.h>

#include "parhelia_drv.h"

#define PARHELIA_HOST_FORMAT_XRGB8888	0x30

static const struct drm_gem_object_funcs parhelia_host_object_funcs;
static const struct drm_gem_object_funcs parhelia_raw_object_funcs;
static const struct drm_gem_object_funcs parhelia_unmappable_object_funcs;

static bool texture_upload_probe;
module_param(texture_upload_probe, bool, 0400);
MODULE_PARM_DESC(texture_upload_probe,
	"Enable guarded type-0 transient-aperture texture uploads");

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
			    u32 width, u32 height, u32 gpu_pitch)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);

	bo->layout = layout;
	bo->width = width;
	bo->height = height;
	bo->gpu_pitch = gpu_pitch;
}

void parhelia_bo_make_unmappable(struct drm_gem_vram_object *gbo)
{
	if (texture_upload_probe)
		gbo->bo.base.funcs = &parhelia_unmappable_object_funcs;
}

bool parhelia_texture_upload_enabled(void)
{
	return texture_upload_probe;
}

bool parhelia_bo_is_native(const struct parhelia_bo *bo)
{
	return bo->layout == PARHELIA_BO_LAYOUT_NATIVE_64X32 ||
	       bo->layout == PARHELIA_BO_LAYOUT_NATIVE_64X16;
}

bool parhelia_gem_is_owned(const struct drm_gem_object *gem)
{
	return gem->funcs == &parhelia_raw_object_funcs ||
	       gem->funcs == &parhelia_unmappable_object_funcs ||
	       gem->funcs == &parhelia_host_object_funcs;
}

/* Validate and return the CPU/display-controller view of a TTM placement. */
int parhelia_vram_bar_addr(struct parhelia_device *pdev,
			   struct drm_gem_vram_object *bo, u64 offset,
			   u64 length, u32 *bar_addr)
{
	u64 bo_size = bo->bo.base.size;
	u64 physical;
	s64 placement;

	if (!length || offset > bo_size || length > bo_size - offset)
		return -EINVAL;
	placement = drm_gem_vram_offset(bo);
	if (placement < 0 || (u64)placement > pdev->vram_size ||
	    bo_size > pdev->vram_size - (u64)placement)
		return -ERANGE;
	if (check_add_overflow((u64)placement, offset, &physical) ||
	    physical > U32_MAX || length > pdev->vram_size - physical)
		return -EOVERFLOW;

	*bar_addr = physical;
	return 0;
}

/*
 * TTM and CPU mappings use BAR0 byte offsets.  The graphics engine instead
 * sees two local-memory banks in a fixed 256 MiB address
 * window: bank A starts at 0 and bank B at 128 MiB.  On a 128 MiB board this
 * leaves the engine address range 64..128 MiB unmapped, although those bytes
 * are contiguous in BAR0.
 *
 * Validate in BAR coordinates first.  A translated bank-B address can be
 * larger than pdev->vram_size and is nevertheless valid.  Conversely, one
 * linear engine range cannot cross the discontinuity between the two banks.
 */
int parhelia_vram_gpu_addr(struct parhelia_device *pdev,
			   struct drm_gem_vram_object *bo, u64 offset,
			   u64 length, u32 *gpu_addr)
{
	u64 bank_size = pdev->vram_size >> 1;
	u64 physical, physical_end, gpu;
	u32 bar_addr;
	int ret;

	if (!bank_size)
		return -EINVAL;
	ret = parhelia_vram_bar_addr(pdev, bo, offset, length, &bar_addr);
	if (ret)
		return ret;
	physical = bar_addr;
	physical_end = physical + length;
	if (physical < bank_size && physical_end > bank_size)
		return -ERANGE;

	if (physical >= bank_size)
		gpu = PARHELIA_LOCAL_BANK_B_BASE + physical - bank_size;
	else
		gpu = physical;
	if (gpu >= PARHELIA_HOST_APERTURE_BASE ||
	    length > PARHELIA_HOST_APERTURE_BASE - gpu)
		return -ERANGE;

	*gpu_addr = gpu;
	return 0;
}

static void parhelia_host_descriptor_disable(struct parhelia_bo *bo)
{
	struct parhelia_device *pdev = to_parhelia(bo->gbo.bo.base.dev);

	mutex_lock(&pdev->aperture_lock);
	if (bo->host_aperture) {
		if (pdev->aperture_live) {
			parhelia_write(pdev,
				       PARHELIA_HOST_APERTURE_SLOT(bo->aperture_slot), 0);
			parhelia_read(pdev,
				      PARHELIA_HOST_APERTURE_SLOT(bo->aperture_slot));
			__clear_bit(bo->aperture_slot, pdev->aperture_slots);
		}
		bo->host_aperture = false;
	}
	mutex_unlock(&pdev->aperture_lock);
}

void parhelia_host_aperture_fini(struct parhelia_device *pdev)
{
	unsigned int slot;

	mutex_lock(&pdev->aperture_lock);
	if (pdev->aperture_live) {
		for (slot = 1; slot < PARHELIA_HOST_APERTURE_SLOTS; slot++)
			parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(slot), 0);
		parhelia_read(pdev,
			      PARHELIA_HOST_APERTURE_SLOT(PARHELIA_HOST_APERTURE_SLOTS - 1));
		bitmap_clear(pdev->aperture_slots, 1,
			     PARHELIA_HOST_APERTURE_SLOTS - 1);
		pdev->aperture_live = false;
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

/* Native sampler storage has no meaningful raw CPU address on P690.  It is
 * populated through UPLOAD_RENDER's transient translated aperture instead. */
static const struct drm_gem_object_funcs parhelia_unmappable_object_funcs = {
	.free = parhelia_raw_object_free,
	.print_info = drm_gem_ttm_print_info,
};

static int parhelia_host_object_vmap(struct drm_gem_object *gem,
				     struct iosys_map *map)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	void __iomem *addr;
	int ret = 0;

	mutex_lock(&pdev->aperture_lock);
	if (!pdev->aperture_live || !bo->host_aperture) {
		ret = -ENODEV;
	} else {
		addr = (u8 __iomem *)pdev->vram + bo->aperture_offset;
		iosys_map_set_vaddr_iomem(map, addr);
	}
	mutex_unlock(&pdev->aperture_lock);
	return ret;
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
	u64 map_offset, node_start;
	int ret;

	mutex_lock(&pdev->aperture_lock);
	if (!pdev->aperture_live || !bo->host_aperture) {
		ret = -ENODEV;
		goto out_unlock;
	}
	node_start = drm_vma_node_start(&gem->vma_node);
	if (vma->vm_pgoff < node_start) {
		ret = -EINVAL;
		goto out_unlock;
	}
	map_offset = (u64)(vma->vm_pgoff - node_start) << PAGE_SHIFT;
	if (map_offset >= bo->host_size ||
	    vma->vm_end - vma->vm_start > bo->host_size - map_offset) {
		ret = -EINVAL;
		goto out_unlock;
	}

	base = pci_resource_start(pdev->pdev, 0) + bo->aperture_offset + map_offset;
	/* drm_gem_mmap() has already consumed the fake GEM offset.  The physical
	 * mapping begins at the descriptor's BAR0 aperture range.
	 */
	vma->vm_pgoff = 0;
	vma->vm_ops = &parhelia_host_vm_ops;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	ret = vm_iomap_memory(vma, base, bo->host_size - map_offset);
out_unlock:
	mutex_unlock(&pdev->aperture_lock);
	return ret;
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
		   (3u << 16) | (bo->gpu_pitch / 4);
	u32 dims = BIT(16) | bo->alloc_height;

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
	u32 gpu_pitch_pixels, host_pitch_pixels, alloc_height;
	s64 aperture_offset;
	u32 surface_address;
	size_t host_size, native_size, size, reservation_size;
	u32 handle;
	int ret;

	if (!args->width || !args->height || args->width > 2048 ||
	    args->height > 2048 || args->bpp != 32 || !pdev->raw_aperture_owned)
		return -EINVAL;
	gpu_pitch_pixels = ALIGN(args->width, 64);
	host_pitch_pixels = roundup_pow_of_two(max_t(u32, args->width, 64));
	alloc_height = ALIGN(args->height,
			     pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ?
			     16 : 32);
	if (check_mul_overflow((size_t)host_pitch_pixels * 4,
			       (size_t)alloc_height, &host_size) ||
	    check_mul_overflow((size_t)gpu_pitch_pixels * 4,
			       (size_t)alloc_height, &native_size))
		return -EOVERFLOW;
	host_size = PAGE_ALIGN(host_size);
	native_size = PAGE_ALIGN(native_size);
	size = max(host_size, native_size);
	reservation_size = ALIGN(host_size, PARHELIA_HOST_APERTURE_ALIGN) +
			   PARHELIA_HOST_APERTURE_ALIGN;
	align_pages = PARHELIA_HOST_APERTURE_ALIGN / PAGE_SIZE;

	mutex_lock(&pdev->aperture_lock);
	if (!pdev->aperture_live) {
		ret = -ENODEV;
		goto out_unlock;
	}
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
	ret = parhelia_vram_gpu_addr(pdev, gbo, 0, gbo->bo.base.size,
				     &surface_address);
	if (ret) {
		goto out_unpin_gbo;
	}

	bo = container_of(gbo, struct parhelia_bo, gbo);
	bo->aperture_reservation = reservation;
	bo->width = args->width;
	bo->height = args->height;
	bo->gpu_pitch = gpu_pitch_pixels * 4;
	bo->host_pitch = host_pitch_pixels * 4;
	bo->alloc_height = alloc_height;
	bo->aperture_offset = aperture_offset;
	bo->host_size = host_size;
	bo->aperture_slot = slot;
	bo->layout = pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ?
		PARHELIA_BO_LAYOUT_NATIVE_64X16 : PARHELIA_BO_LAYOUT_NATIVE_64X32;
	bo->host_aperture = true;
	gbo->bo.base.funcs = &parhelia_host_object_funcs;
	parhelia_host_descriptor_enable(pdev, bo, surface_address);
	mutex_unlock(&pdev->aperture_lock);

	ret = drm_gem_handle_create(file, &gbo->bo.base, &handle);
	if (ret) {
		drm_gem_object_put(&gbo->bo.base);
		return ret;
	}
	args->handle = handle;
	args->pitch = host_pitch_pixels * 4;
	args->size = host_size;
	drm_gem_object_put(&gbo->bo.base);
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
	args->pitch = bo->gpu_pitch;
	args->size = gem->size;
	drm_gem_object_put(gem);
	return 0;
}

int parhelia_upload_render_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_parhelia_upload_render *args = data;
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_gem_vram_object *reservation = NULL;
	struct drm_gem_object *gem;
	struct parhelia_bo *bo;
	unsigned long align_pages, slot = PARHELIA_HOST_APERTURE_SLOTS;
	size_t reservation_size;
	s64 aperture_offset;
	u32 surface_address, reg, ctrl, dims;
	u64 copied;
	void *source;
	int ret;

	if (!texture_upload_probe || pdev->pdev->device != 0x2539 ||
	    !pdev->raw_aperture_owned)
		return -EOPNOTSUPP;
	if (!args->data || !args->handle || !args->size || args->flags ||
	    args->pad || !IS_ALIGNED(args->offset, 4) ||
	    !IS_ALIGNED(args->size, 4) || args->size > SIZE_MAX)
		return -EINVAL;

	source = vmemdup_user(u64_to_user_ptr(args->data), args->size);
	if (IS_ERR(source))
		return PTR_ERR(source);

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem) {
		ret = -ENOENT;
		goto out_source;
	}
	bo = to_parhelia_bo(gem);
	if (gem->funcs != &parhelia_unmappable_object_funcs ||
	    bo->layout != PARHELIA_BO_LAYOUT_TEXTURE ||
	    args->offset >= gem->size || args->size > gem->size - args->offset) {
		ret = -EINVAL;
		goto out_gem;
	}
	reservation_size = 2 * PARHELIA_HOST_APERTURE_ALIGN;

	/* This mirrors the vendor upload record: drain conflicting work, map the
	 * destination through a short-lived aperture, LocMemCpy, then unmap. */
	ret = parhelia_submit_idle_lock(pdev);
	if (ret)
		goto out_gem;
	ret = parhelia_vram_pin(&bo->gbo);
	if (ret)
		goto out_submit_unlock;
	mutex_lock(&pdev->aperture_lock);
	if (!pdev->aperture_live) {
		ret = -ENODEV;
		goto out_aperture_unlock;
	}
	slot = find_next_zero_bit(pdev->aperture_slots,
				  PARHELIA_HOST_APERTURE_SLOTS, 1);
	if (slot == PARHELIA_HOST_APERTURE_SLOTS) {
		ret = -ENOSPC;
		goto out_aperture_unlock;
	}
	__set_bit(slot, pdev->aperture_slots);
	align_pages = PARHELIA_HOST_APERTURE_ALIGN / PAGE_SIZE;
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
	if (aperture_offset < 0 ||
	    aperture_offset >= PARHELIA_HOST_APERTURE_VALID ||
	    !IS_ALIGNED(aperture_offset, PARHELIA_HOST_APERTURE_ALIGN)) {
		ret = -ERANGE;
		goto out_unpin_reservation;
	}

	reg = PARHELIA_HOST_APERTURE_SLOT(slot);
	dims = BIT(16) | 1;
	for (copied = 0; copied < args->size;) {
		u32 bytes = min_t(u64, args->size - copied, SZ_8K);

		ret = parhelia_vram_gpu_addr(pdev, &bo->gbo,
				args->offset + copied, bytes, &surface_address);
		if (ret)
			goto out_disable;
		/* 0x10/type 0 is the measured byte-linear bootstrap format.  Mesa
		 * supplies the already-swizzled native bytes, so this is an opaque
		 * view and cannot apply the 32x16 texture transform a second time. */
		ctrl = (0x10u << 24) | bytes;
		parhelia_write(pdev, reg, 0);
		parhelia_write(pdev, reg + 4, surface_address);
		parhelia_write(pdev, reg + 8, ctrl);
		parhelia_write(pdev, reg + 12, dims);
		wmb();
		parhelia_write(pdev, reg,
			       PARHELIA_HOST_APERTURE_VALID | (u32)aperture_offset);
		parhelia_read(pdev, reg);
		memcpy_toio((u8 __iomem *)pdev->vram + aperture_offset,
			    (u8 *)source + copied, bytes);
		wmb();
		parhelia_read(pdev, reg);
		copied += bytes;
	}
	/* Make every posted host write visible before the surface may be bound by
	 * the next submit, then revoke the translation before recycling its BAR
	 * coordinates. */
	ret = 0;
out_disable:
	parhelia_write(pdev, reg, 0);
	parhelia_read(pdev, reg);

out_unpin_reservation:
	parhelia_vram_unpin(reservation);
out_reservation:
	drm_gem_object_put(&reservation->bo.base);
out_slot:
	__clear_bit(slot, pdev->aperture_slots);
out_aperture_unlock:
	mutex_unlock(&pdev->aperture_lock);
	parhelia_vram_unpin(&bo->gbo);
out_submit_unlock:
	mutex_unlock(&pdev->submit_lock);
out_gem:
	drm_gem_object_put(gem);
out_source:
	kvfree(source);
	return ret;
}
