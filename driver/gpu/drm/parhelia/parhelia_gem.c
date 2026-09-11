// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>

#include "parhelia_drv.h"

#define PARHELIA_HOST_FORMAT_XRGB8888	0x30

static const struct drm_gem_object_funcs parhelia_host_object_funcs;
static const struct drm_gem_object_funcs parhelia_raw_object_funcs;
static const struct drm_gem_object_funcs parhelia_unmappable_object_funcs;

static bool texture_upload_probe = true;
module_param(texture_upload_probe, bool, 0400);
MODULE_PARM_DESC(texture_upload_probe,
	"Enable hardware-tested P690 type-0 transient-aperture texture uploads (default true)");

static bool texture_upload_trace;
module_param(texture_upload_trace, bool, 0400);
MODULE_PARM_DESC(texture_upload_trace,
	"Log up to 4096 transient texture-upload aperture chunks");

static atomic_t texture_upload_trace_count = ATOMIC_INIT(0);

static bool root_transition_trace;
module_param(root_transition_trace, bool, 0400);
MODULE_PARM_DESC(root_transition_trace,
	"Trace root sample transitions at every Parhelia ioctl/mmap boundary");

static_assert(SZ_8K <= GENMASK(13, 0));

static bool parhelia_read_root_samples(struct parhelia_device *pdev, u32 value[3])
{
	static const struct {
		u16 x;
		u16 y;
	} probe[] = { { 0, 402 }, { 64, 402 }, { 0, 418 } };
	u64 bank_size = pdev->vram_size >> 1;
	u32 surface, ctrl, dims, pitch, height;
	u64 physical_base;
	unsigned int i;

	surface = parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1) + 4);
	ctrl = parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1) + 8);
	dims = parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1) + 12);
	pitch = ctrl & GENMASK(13, 0);
	height = dims & GENMASK(13, 0);
	if (!(parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1)) &
	      PARHELIA_HOST_APERTURE_VALID) || !bank_size ||
	    pdev->chip->tile_layout != PARHELIA_TILE_64X16_4K ||
	    ((ctrl >> 24) & 0x3f) != PARHELIA_HOST_FORMAT_XRGB8888 ||
	    ((ctrl >> 16) & 7) != 3 || pitch <= 64 || (pitch & 63) ||
	    height <= 418)
		return false;
	if (surface < bank_size)
		physical_base = surface;
	else if (surface >= PARHELIA_LOCAL_BANK_B_BASE &&
		 surface - PARHELIA_LOCAL_BANK_B_BASE < bank_size)
		physical_base = bank_size + surface - PARHELIA_LOCAL_BANK_B_BASE;
	else
		return false;
	for (i = 0; i < ARRAY_SIZE(probe); i++) {
		u32 x = probe[i].x, y = probe[i].y;
		u32 tpr = pitch / 64, tcol = x / 64, trow = y / 16;
		u32 ix = x & 63, iy = y & 15;
		u32 x5 = ((ix >> 5) & 1) ^ (trow & 1);
		u32 in_tile;
		u64 offset;

		in_tile = (ix & 1) | (((ix >> 1) & 1) << 1) |
			  (((ix >> 2) & 1) << 2) | ((iy & 1) << 3) |
			  (((ix >> 3) & 1) << 4) | (((ix >> 4) & 1) << 5) |
			  (((iy >> 1) & 1) << 6) | (x5 << 7) |
			  (((iy >> 2) & 1) << 8) | (((iy >> 3) & 1) << 9);
		offset = physical_base +
			 ((u64)trow * tpr + tcol) * SZ_4K +
			 in_tile * sizeof(u32);

		if (offset > pdev->vram_size - sizeof(u32))
			return false;
		value[i] = readl((u8 __iomem *)pdev->vram + offset);
	}
	return true;
}

void parhelia_root_trace_check(struct parhelia_device *pdev,
			       const char *event, u64 arg0, u64 arg1, u64 arg2)
{
	u32 value[3];
	u64 seq;

	if (!root_transition_trace ||
	    !parhelia_read_root_samples(pdev, value))
		return;
	mutex_lock(&pdev->root_trace_lock);
	seq = ++pdev->root_trace_seq;
	if (!pdev->root_trace_valid) {
		memcpy(pdev->root_trace_value, value, sizeof(value));
		pdev->root_trace_valid = true;
		dev_info(&pdev->pdev->dev,
			 "root transition baseline: seq=%llu event=%s pid=%d comm=%s values=%08x/%08x/%08x args=%#llx/%#llx/%#llx\n",
			 seq, event, task_pid_nr(current), current->comm,
			 value[0], value[1], value[2], arg0, arg1, arg2);
	} else if (memcmp(pdev->root_trace_value, value, sizeof(value))) {
		dev_err(&pdev->pdev->dev,
			"root transition: seq=%llu event=%s pid=%d comm=%s values=%08x/%08x/%08x->%08x/%08x/%08x args=%#llx/%#llx/%#llx\n",
			seq, event, task_pid_nr(current), current->comm,
			pdev->root_trace_value[0], pdev->root_trace_value[1],
			pdev->root_trace_value[2], value[0], value[1], value[2],
			arg0, arg1, arg2);
		memcpy(pdev->root_trace_value, value, sizeof(value));
	}
	mutex_unlock(&pdev->root_trace_lock);
}

static void parhelia_bootstrap_reservation_release(struct drm_device *drm,
						   void *data)
{
	struct drm_gem_vram_object *reservation = data;

	parhelia_vram_unpin(reservation);
	drm_gem_vram_put(reservation);
}

int parhelia_host_aperture_reserve_bootstrap(struct parhelia_device *pdev)
{
	struct drm_gem_vram_object *reservation;
	s64 offset;
	int ret;

	/* P690 class-0 engine and CPU addresses fold at 16 MiB.  Reserve that
	 * complete physical range before creating any real surface: drm_mm then
	 * suballocates independent logical dword-0 coordinates inside the
	 * carveout, while every descriptor's dword 1 names storage above it.
	 */
	reservation = drm_gem_vram_create(&pdev->drm,
					  PARHELIA_HOST_APERTURE_LIMIT,
					  PARHELIA_HOST_APERTURE_ALIGN / PAGE_SIZE);
	if (IS_ERR(reservation))
		return PTR_ERR(reservation);
	ret = parhelia_vram_pin(reservation);
	if (ret)
		goto out_put;
	offset = drm_gem_vram_offset(reservation);
	if (offset != 0) {
		ret = -ERANGE;
		goto out_unpin;
	}
	return drmm_add_action_or_reset(&pdev->drm,
					parhelia_bootstrap_reservation_release,
					reservation);

out_unpin:
	parhelia_vram_unpin(reservation);
out_put:
	drm_gem_vram_put(reservation);
	return ret;
}

void parhelia_host_aperture_init(struct parhelia_device *pdev)
{
	/* Slot zero owns [0, 256 KiB), followed by the vendor's 256 KiB guard.
	 * P690 drops class-0 address bit 24, so no mapping may reach 16 MiB.
	 */
	drm_mm_init(&pdev->aperture_mm, PARHELIA_HOST_APERTURE_FIRST,
		    PARHELIA_HOST_APERTURE_LIMIT - PARHELIA_HOST_APERTURE_FIRST);
	INIT_LIST_HEAD(&pdev->aperture_mappings);
	pdev->aperture_mapping_count = 0;
}

static void parhelia_aperture_set_slot(struct parhelia_aperture_mapping *mapping,
				       u8 slot)
{
	mapping->slot = slot;
	if (mapping->owner)
		mapping->owner->aperture_slot = slot;
}

static void parhelia_aperture_program(struct parhelia_device *pdev, u8 slot,
				      struct parhelia_aperture_mapping *mapping)
{
	u32 reg = PARHELIA_HOST_APERTURE_SLOT(slot);

	/* Dwords 1..3 are private until VALID is published in dword 0. */
	parhelia_write(pdev, reg, 0);
	parhelia_write(pdev, reg + 4, mapping->surface);
	parhelia_write(pdev, reg + 8, mapping->ctrl);
	parhelia_write(pdev, reg + 12, mapping->dims);
	wmb();
	parhelia_write(pdev, reg, PARHELIA_HOST_APERTURE_VALID |
		       lower_32_bits(mapping->node.start));
	parhelia_read(pdev, reg);
	parhelia_aperture_set_slot(mapping, slot);
}

static int parhelia_aperture_alloc_locked(struct parhelia_device *pdev,
					  struct parhelia_aperture_mapping *mapping,
					  u32 map_size)
{
	u64 span;

	lockdep_assert_held(&pdev->aperture_lock);
	if (!pdev->aperture_live)
		return -ENODEV;
	if (!map_size || pdev->aperture_mapping_count >=
			PARHELIA_HOST_APERTURE_SLOTS - 1)
		return map_size ? -ENOSPC : -EINVAL;
	if (check_add_overflow(ALIGN((u64)map_size,
				     PARHELIA_HOST_APERTURE_ALIGN),
			       (u64)PARHELIA_HOST_APERTURE_ALIGN, &span) ||
	    span >= PARHELIA_HOST_APERTURE_LIMIT)
		return -E2BIG;

	memset(mapping, 0, sizeof(*mapping));
	INIT_LIST_HEAD(&mapping->link);
	mapping->map_size = map_size;
	return drm_mm_insert_node_generic(&pdev->aperture_mm, &mapping->node,
					  span, PARHELIA_HOST_APERTURE_ALIGN,
					  0, DRM_MM_INSERT_LOW);
}

static void parhelia_aperture_free_unpublished_locked(
		struct parhelia_aperture_mapping *mapping)
{
	if (drm_mm_node_allocated(&mapping->node))
		drm_mm_remove_node(&mapping->node);
}

static void parhelia_aperture_publish_locked(struct parhelia_device *pdev,
					     struct parhelia_aperture_mapping *mapping,
					     u32 surface, u32 ctrl, u32 dims,
					     struct parhelia_bo *owner)
{
	struct parhelia_aperture_mapping *pos;
	u8 slot = 1;

	lockdep_assert_held(&pdev->aperture_lock);
	mapping->surface = surface;
	mapping->ctrl = ctrl;
	mapping->dims = dims;
	mapping->owner = owner;

	list_for_each_entry(pos, &pdev->aperture_mappings, link) {
		if (mapping->node.start < pos->node.start)
			break;
		slot++;
	}
	/* Shift the suffix from high to low.  Duplicate entries during the move
	 * describe identical translations, so existing mappings remain valid.
	 */
	list_for_each_entry_reverse(pos, &pdev->aperture_mappings, link) {
		if (pos->slot < slot)
			break;
		parhelia_aperture_program(pdev, pos->slot + 1, pos);
	}
	if (slot == pdev->aperture_mapping_count + 1)
		list_add_tail(&mapping->link, &pdev->aperture_mappings);
	else {
		pos = list_first_entry(&pdev->aperture_mappings,
				       struct parhelia_aperture_mapping, link);
		while (pos->slot < slot)
			pos = list_next_entry(pos, link);
		list_add_tail(&mapping->link, &pos->link);
	}
	parhelia_aperture_program(pdev, slot, mapping);
	mapping->active = true;
	pdev->aperture_mapping_count++;
}

int parhelia_host_aperture_acquire(struct parhelia_device *pdev,
				   struct parhelia_aperture_mapping *mapping,
				   u32 map_size, u32 surface, u32 ctrl,
				   u32 dims, struct parhelia_bo *owner)
{
	int ret;

	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_locked(pdev, mapping, map_size);
	if (!ret)
		parhelia_aperture_publish_locked(pdev, mapping, surface, ctrl,
						 dims, owner);
	mutex_unlock(&pdev->aperture_lock);
	return ret;
}

static void parhelia_aperture_retarget_locked(struct parhelia_device *pdev,
					      struct parhelia_aperture_mapping *mapping,
					      u32 surface, u32 ctrl, u32 dims)
{
	lockdep_assert_held(&pdev->aperture_lock);
	mapping->surface = surface;
	mapping->ctrl = ctrl;
	mapping->dims = dims;
	parhelia_aperture_program(pdev, mapping->slot, mapping);
}

int parhelia_host_aperture_retarget(struct parhelia_device *pdev,
				    struct parhelia_aperture_mapping *mapping,
				    u32 surface, u32 ctrl, u32 dims)
{
	int ret = 0;

	/* The caller must have drained any work which can still use the previous
	 * dword-1 target.  The mapping's logical address and sorted position do
	 * not change, so future engine packets may keep using node.start.
	 */
	mutex_lock(&pdev->aperture_lock);
	if (!pdev->aperture_live || !mapping->active)
		ret = -ENODEV;
	else
		parhelia_aperture_retarget_locked(pdev, mapping, surface, ctrl,
						  dims);
	mutex_unlock(&pdev->aperture_lock);
	return ret;
}

static void parhelia_aperture_release_locked(struct parhelia_device *pdev,
					     struct parhelia_aperture_mapping *mapping)
{
	struct parhelia_aperture_mapping *pos;
	u8 old_tail;

	lockdep_assert_held(&pdev->aperture_lock);
	if (!mapping->active)
		return;
	old_tail = pdev->aperture_mapping_count;
	if (pdev->aperture_live) {
		parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(mapping->slot), 0);
		list_for_each_entry(pos, &pdev->aperture_mappings, link) {
			if (pos->slot > mapping->slot)
				parhelia_aperture_program(pdev, pos->slot - 1, pos);
		}
		parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(old_tail), 0);
		parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(old_tail));
	}
	list_del_init(&mapping->link);
	drm_mm_remove_node(&mapping->node);
	mapping->active = false;
	mapping->owner = NULL;
	pdev->aperture_mapping_count--;
}

void parhelia_host_aperture_release(struct parhelia_device *pdev,
				    struct parhelia_aperture_mapping *mapping)
{
	mutex_lock(&pdev->aperture_lock);
	parhelia_aperture_release_locked(pdev, mapping);
	mutex_unlock(&pdev->aperture_lock);
}

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

void parhelia_bo_set_placement_class(struct drm_gem_vram_object *gbo, u8 class)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);

	bo->placement_class = class;
}

void parhelia_bo_make_unmappable(struct drm_gem_vram_object *gbo)
{
	if (texture_upload_probe)
		gbo->bo.base.funcs = &parhelia_unmappable_object_funcs;
}

void parhelia_bo_force_unmappable(struct drm_gem_vram_object *gbo)
{
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

	/* Host-aperture dword 0 is only a CPU/BAR alias.  Engine SURF and TEX
	 * operands consume the bank-encoded physical allocation address stored in
	 * descriptor dword 1, including for dumb/DRI3 objects.
	 */
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

	if (bo->host_aperture) {
		parhelia_host_aperture_release(pdev, &bo->aperture);
		bo->host_aperture = false;
	}
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
		pdev->aperture_live = false;
	}
	mutex_unlock(&pdev->aperture_lock);
}

static void parhelia_host_object_free(struct drm_gem_object *gem)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);

	parhelia_root_trace_check(pdev, "host_free:enter", bo->layout,
				  gem->size, bo->aperture_slot);
	/* A VMA owns a GEM reference, so this cannot run while the translated BAR
	 * range remains accessible.  Revoke the descriptor before either address
	 * range can return to TTM.
	 */
	parhelia_host_descriptor_disable(bo);
	parhelia_vram_unpin(&bo->gbo);
	parhelia_root_trace_check(pdev, "host_free:exit", bo->layout,
				  gem->size, bo->aperture_slot);
	drm_gem_vram_put(&bo->gbo);
}

static void parhelia_raw_object_free(struct drm_gem_object *gem)
{
	struct parhelia_device *pdev = to_parhelia(gem->dev);

	parhelia_root_trace_check(pdev, "raw_free:enter",
				  to_parhelia_bo(gem)->layout, gem->size, 0);
	if (to_parhelia_bo(gem)->resident)
		parhelia_vram_unpin(drm_gem_vram_of_gem(gem));
	drm_gem_vram_put(drm_gem_vram_of_gem(gem));
}

static int parhelia_raw_object_vmap(struct drm_gem_object *gem,
				    struct iosys_map *map)
{
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	int ret;

	parhelia_root_trace_check(pdev, "raw_vmap:enter",
				  to_parhelia_bo(gem)->layout, gem->size, 0);
	ret = drm_gem_ttm_vmap(gem, map);
	parhelia_root_trace_check(pdev, "raw_vmap:exit", (u64)(s64)ret,
				  to_parhelia_bo(gem)->layout, gem->size);
	return ret;
}

static void parhelia_raw_object_vunmap(struct drm_gem_object *gem,
				       struct iosys_map *map)
{
	struct parhelia_device *pdev = to_parhelia(gem->dev);

	parhelia_root_trace_check(pdev, "raw_vunmap:enter",
				  to_parhelia_bo(gem)->layout, gem->size, 0);
	drm_gem_ttm_vunmap(gem, map);
	parhelia_root_trace_check(pdev, "raw_vunmap:exit",
				  to_parhelia_bo(gem)->layout, gem->size, 0);
}

static int parhelia_raw_object_mmap(struct drm_gem_object *gem,
				    struct vm_area_struct *vma)
{
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	int ret;

	parhelia_root_trace_check(pdev, "raw_mmap:enter",
				  to_parhelia_bo(gem)->layout, gem->size,
				  vma->vm_end - vma->vm_start);
	ret = drm_gem_ttm_mmap(gem, vma);
	parhelia_root_trace_check(pdev, "raw_mmap:exit", (u64)(s64)ret,
				  to_parhelia_bo(gem)->layout, gem->size);
	return ret;
}

static const struct drm_gem_object_funcs parhelia_raw_object_funcs = {
	.free = parhelia_raw_object_free,
	.vmap = parhelia_raw_object_vmap,
	.vunmap = parhelia_raw_object_vunmap,
	.mmap = parhelia_raw_object_mmap,
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

	parhelia_root_trace_check(pdev, "host_vmap:enter", bo->layout,
				  gem->size, bo->aperture_slot);
	mutex_lock(&pdev->aperture_lock);
	if (!pdev->aperture_live || !bo->host_aperture) {
		ret = -ENODEV;
	} else {
		addr = (u8 __iomem *)pdev->vram + bo->aperture_offset;
		iosys_map_set_vaddr_iomem(map, addr);
	}
	mutex_unlock(&pdev->aperture_lock);
	parhelia_root_trace_check(pdev, "host_vmap:exit", (u64)(s64)ret,
				  bo->layout, gem->size);
	return ret;
}

static void parhelia_host_object_vunmap(struct drm_gem_object *gem,
					struct iosys_map *map)
{
	struct parhelia_device *pdev = to_parhelia(gem->dev);

	parhelia_root_trace_check(pdev, "host_vunmap:enter",
				  to_parhelia_bo(gem)->layout, gem->size, 0);
	iosys_map_clear(map);
	parhelia_root_trace_check(pdev, "host_vunmap:exit",
				  to_parhelia_bo(gem)->layout, gem->size, 0);
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

	parhelia_root_trace_check(pdev, "host_mmap:enter", bo->layout,
				  gem->size, vma->vm_end - vma->vm_start);
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
	parhelia_root_trace_check(pdev, "host_mmap:exit", (u64)(s64)ret,
				  bo->layout, gem->size);
	return ret;
}

static const struct drm_gem_object_funcs parhelia_host_object_funcs = {
	.free = parhelia_host_object_free,
	.vmap = parhelia_host_object_vmap,
	.vunmap = parhelia_host_object_vunmap,
	.mmap = parhelia_host_object_mmap,
	.print_info = drm_gem_ttm_print_info,
};

/* Zero a newly placed, pinned object through a kernel aperture window. */
int parhelia_vram_clear(struct parhelia_device *pdev,
			struct drm_gem_vram_object *gbo)
{
	struct parhelia_aperture_mapping mapping = { };
	u64 size = gbo->bo.base.size, done;
	int ret;

	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_locked(pdev, &mapping, SZ_8K);
	for (done = 0; !ret && done < size; done += SZ_8K) {
		u32 bytes = min_t(u64, size - done, SZ_8K);
		u32 surface_address;

		ret = parhelia_vram_gpu_addr(pdev, gbo, done, bytes,
					     &surface_address);
		if (ret)
			break;
		if (!mapping.active)
			parhelia_aperture_publish_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001, NULL);
		else
			parhelia_aperture_retarget_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001);
		memset_io((u8 __iomem *)pdev->vram + mapping.node.start, 0,
			  bytes);
		wmb();
		parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(mapping.slot));
	}
	if (mapping.active)
		parhelia_aperture_release_locked(pdev, &mapping);
	else
		parhelia_aperture_free_unpublished_locked(&mapping);
	mutex_unlock(&pdev->aperture_lock);
	return ret;
}

int parhelia_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args)
{
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_gem_vram_object *gbo;
	struct parhelia_bo *bo;
	unsigned long native_align_pages;
	u32 gpu_pitch_pixels, host_pitch_pixels, alloc_height;
	u32 surface_address;
	size_t host_size, native_size, size;
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

	/* The probe-time carveout keeps this real surface above the folded class-0
	 * VA window.  Allocate its independent low logical address before pinning
	 * and recording the physical dword-1 target.
	 */
	native_align_pages = pdev->chip->tile_layout == PARHELIA_TILE_64X32_8K ? 2 : 1;
	gbo = drm_gem_vram_create(drm, size, native_align_pages);
	if (IS_ERR(gbo))
		return PTR_ERR(gbo);
	bo = container_of(gbo, struct parhelia_bo, gbo);

	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_locked(pdev, &bo->aperture, host_size);
	if (ret)
		goto out_gbo_unlock;
	ret = parhelia_vram_pin(gbo);
	if (ret)
		goto out_aperture;
	ret = parhelia_vram_gpu_addr(pdev, gbo, 0, gbo->bo.base.size,
				     &surface_address);
	if (ret)
		goto out_unpin_gbo;

	bo->width = args->width;
	bo->height = args->height;
	bo->gpu_pitch = gpu_pitch_pixels * 4;
	bo->host_pitch = host_pitch_pixels * 4;
	bo->alloc_height = alloc_height;
	bo->aperture_offset = bo->aperture.node.start;
	bo->host_size = host_size;
	bo->layout = pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ?
		PARHELIA_BO_LAYOUT_NATIVE_64X16 : PARHELIA_BO_LAYOUT_NATIVE_64X32;
	bo->host_aperture = true;
	gbo->bo.base.funcs = &parhelia_host_object_funcs;
	parhelia_aperture_publish_locked(pdev, &bo->aperture, surface_address,
		(PARHELIA_HOST_FORMAT_XRGB8888 << 24) | (3u << 16) |
		(bo->gpu_pitch / 4), BIT(16) | bo->alloc_height, bo);
	/* TTM no longer clears new P690 objects; use the object's own view. */
	memset_io((u8 __iomem *)pdev->vram + bo->aperture.node.start, 0,
		  host_size);
	wmb();
	parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(bo->aperture.slot));
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
out_aperture:
	parhelia_aperture_free_unpublished_locked(&bo->aperture);
out_gbo_unlock:
	mutex_unlock(&pdev->aperture_lock);
	drm_gem_object_put(&gbo->bo.base);
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

static int parhelia_transfer_render_ioctl(struct drm_device *drm, void *data,
					   struct drm_file *file,
					   bool download)
{
	struct drm_parhelia_upload_render *args = data;
	struct parhelia_device *pdev = to_parhelia(drm);
	struct parhelia_aperture_mapping mapping = { };
	struct drm_gem_object *gem;
	struct parhelia_bo *bo;
	u32 surface_address;
	u64 copied;
	void *buffer;
	int ret;

	if (pdev->pdev->device != 0x2539 || !pdev->raw_aperture_owned)
		return -EOPNOTSUPP;
	if (!args->data || !args->handle || !args->size || args->flags ||
	    args->pad || !IS_ALIGNED(args->offset, 4) ||
	    !IS_ALIGNED(args->size, 4) || args->size > SZ_16M ||
	    args->size > SIZE_MAX)
		return -EINVAL;

	if (download) {
		buffer = kvmalloc(args->size, GFP_KERNEL);
		if (!buffer)
			return -ENOMEM;
	} else {
		buffer = vmemdup_user(u64_to_user_ptr(args->data), args->size);
		if (IS_ERR(buffer))
			return PTR_ERR(buffer);
	}

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem) {
		ret = -ENOENT;
		goto out_buffer;
	}
	bo = to_parhelia_bo(gem);
	if (!parhelia_gem_is_owned(gem) ||
	    !((bo->layout == PARHELIA_BO_LAYOUT_TEXTURE && texture_upload_probe) ||
	      bo->placement_class == PARHELIA_PLACEMENT_NATIVE_COLOR ||
	      bo->placement_class == PARHELIA_PLACEMENT_NATIVE_DEPTH) ||
	    args->offset >= gem->size || args->size > gem->size - args->offset) {
		ret = -EINVAL;
		goto out_gem;
	}

	/* No submit may bind the target while its temporary predecessor mapping is
	 * installed.  aperture_lock also keeps the mapping's compacted slot stable
	 * for the duration of the copy.
	 */
	ret = parhelia_submit_idle_lock(pdev);
	if (ret)
		goto out_gem;
	ret = parhelia_vram_pin(&bo->gbo);
	if (ret)
		goto out_submit_unlock;
	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_locked(pdev, &mapping, SZ_8K);
	if (ret)
		goto out_aperture_unlock;

	for (copied = 0; copied < args->size;) {
		u32 bytes = min_t(u64, args->size - copied, SZ_8K);
		u32 root_before[3], root_after[3];
		bool watch_root;

		if (!IS_ALIGNED(bytes, sizeof(u32))) {
			ret = -EINVAL;
			goto out_mapping;
		}
		ret = parhelia_vram_gpu_addr(pdev, &bo->gbo,
				args->offset + copied, bytes, &surface_address);
		if (ret)
			goto out_mapping;

		/* The vendor keeps a raw type-0 extent in software and publishes the
		 * canonical minimum-width hardware shape.  Retargeting dword 1 lets one
		 * low 8 KiB VA window service an arbitrary-size upload in chunks.
		 */
		if (!mapping.active)
			parhelia_aperture_publish_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001, NULL);
		else
			parhelia_aperture_retarget_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001);
		if (texture_upload_trace &&
		    atomic_inc_return(&texture_upload_trace_count) <= 4096)
			dev_info(&pdev->pdev->dev,
				 "render %s slot=%u aperture=%#llx surface=%#x copied=%#llx total=%#llx bo_offset=%#llx bytes=%#x ctrl=%#x\n",
				 download ? "download" : "upload",
				 mapping.slot,
				 (unsigned long long)mapping.node.start,
				 surface_address, (unsigned long long)copied,
				 (unsigned long long)args->size,
				 (unsigned long long)args->offset, bytes,
				 mapping.ctrl);
		watch_root = !download && texture_upload_trace &&
			parhelia_read_root_samples(pdev, root_before);
		if (download) {
			memcpy_fromio((u8 *)buffer + copied,
				      (u8 __iomem *)pdev->vram + mapping.node.start,
				      bytes);
		} else {
			memcpy_toio((u8 __iomem *)pdev->vram + mapping.node.start,
				    (u8 *)buffer + copied, bytes);
			wmb();
			parhelia_read(pdev,
				      PARHELIA_HOST_APERTURE_SLOT(mapping.slot));
		}
		if (watch_root && parhelia_read_root_samples(pdev, root_after) &&
		    memcmp(root_before, root_after, sizeof(root_before)))
			dev_err(&pdev->pdev->dev,
				"render upload changed root probes: handle=%u surface=%#x copied=%#llx bytes=%#x values=%08x/%08x/%08x -> %08x/%08x/%08x\n",
				args->handle, surface_address,
				(unsigned long long)copied, bytes,
				root_before[0], root_before[1], root_before[2],
				root_after[0], root_after[1], root_after[2]);
		copied += bytes;
	}
	ret = 0;

out_mapping:
	if (mapping.active)
		parhelia_aperture_release_locked(pdev, &mapping);
	else
		parhelia_aperture_free_unpublished_locked(&mapping);
out_aperture_unlock:
	mutex_unlock(&pdev->aperture_lock);
	parhelia_vram_unpin(&bo->gbo);
out_submit_unlock:
	mutex_unlock(&pdev->submit_lock);
out_gem:
	drm_gem_object_put(gem);
out_buffer:
	if (!ret && download && copy_to_user(u64_to_user_ptr(args->data),
						 buffer, args->size))
		ret = -EFAULT;
	kvfree(buffer);
	return ret;
}

int parhelia_upload_render_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	return parhelia_transfer_render_ioctl(drm, data, file, false);
}

int parhelia_download_render_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	return parhelia_transfer_render_ioctl(drm, data, file, true);
}
