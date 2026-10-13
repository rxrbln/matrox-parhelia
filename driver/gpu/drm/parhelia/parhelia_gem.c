// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/dma-resv.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_vma_manager.h>
#include <drm/ttm/ttm_tt.h>

#include "parhelia_drv.h"

/* Each uncached aperture read is one PCIe round trip: 8-byte loads move
 * 19 MB/s where memcpy_fromio's 4-byte loads move 9.8 (MEASURED, P690). */
static void parhelia_vram_read(void *dst, const void __iomem *src, size_t len)
{
#ifdef CONFIG_64BIT
	u8 *out = dst;

	if (len >= 4 && ((uintptr_t)src & 7) == 4) {
		put_unaligned(readl(src), (u32 *)out);
		src += 4;
		out += 4;
		len -= 4;
	}
	for (; len >= 8; len -= 8, src += 8, out += 8)
		put_unaligned(readq(src), (u64 *)out);
	if (len)
		memcpy_fromio(out, src, len);
#else
	memcpy_fromio(dst, src, len);
#endif
}

#define PARHELIA_HOST_FORMAT_XRGB8888	0x30
/* The vendor's "2-D view" code for every 16 bpp surface (mtxpserd.dll format
 * record table, usage 0).  PARHELIA_HOST_FORMAT_RGB565=code overrides it. */
#define PARHELIA_HOST_FORMAT_RGB565	0x20
static unsigned int host_format_rgb565 = PARHELIA_HOST_FORMAT_RGB565;
module_param(host_format_rgb565, uint, 0644);
MODULE_PARM_DESC(host_format_rgb565,
		 "host-aperture format code for 16 bpp surfaces");

static const struct drm_gem_object_funcs parhelia_view_object_funcs;
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

static bool engine_transfer = true;
module_param(engine_transfer, bool, 0644);
MODULE_PARM_DESC(engine_transfer,
	"P690: move UPLOAD/DOWNLOAD_RENDER data with the 2-D engine (default true)");

/* Below these sizes one submit round trip costs more than the CPU copy. */
static unsigned int engine_download_min = SZ_1K;
module_param(engine_download_min, uint, 0644);
MODULE_PARM_DESC(engine_download_min, "smallest engine download in bytes");
/* Queued engine uploads return sooner than the aperture copy from 64 KiB
 * (80 vs 87 us) but not at 4 KiB (23 vs 11 us); end to end they tie (MEASURED). */
static unsigned int engine_upload_min = SZ_64K;
module_param(engine_upload_min, uint, 0644);
MODULE_PARM_DESC(engine_upload_min, "smallest engine upload in bytes");
/* Queued uploads overlap the caller's CPU work and earlier rendering
 * instead of draining the engine; false waits for each blit (validated). */
static bool engine_upload_async = true;
module_param(engine_upload_async, bool, 0644);
MODULE_PARM_DESC(engine_upload_async,
	"P690: queue engine uploads under a fence without waiting (default true)");

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

static void parhelia_host_view_reap(struct work_struct *work);

void parhelia_host_aperture_init(struct parhelia_device *pdev)
{
	/* Slot zero owns [0, 256 KiB), followed by the vendor's 256 KiB guard.
	 * P690 drops class-0 address bit 24, so no mapping may reach 16 MiB.
	 */
	drm_mm_init(&pdev->aperture_mm, PARHELIA_HOST_APERTURE_FIRST,
		    PARHELIA_HOST_APERTURE_LIMIT - PARHELIA_HOST_APERTURE_FIRST);
	INIT_LIST_HEAD(&pdev->aperture_mappings);
	/* EmulMmSurfaceMapper keeps BAR0 bits 27:25 and ORs them into every
	 * coordinate it publishes (mtx_drv.so .text+0x74bdd, +0x74cc9).
	 * Without them the top descriptor shadowed all the others: the bank
	 * appears to decode the largest coordinate not above the access. */
	pdev->aperture_coord_base = lower_32_bits(pci_bus_address(pdev->pdev, 0)) &
				    GENMASK(27, 25);
	INIT_LIST_HEAD(&pdev->host_view_lru);
	INIT_LIST_HEAD(&pdev->host_view_dead);
	INIT_WORK(&pdev->host_view_work, parhelia_host_view_reap);
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

	/* Drain write-combined stores through the old translation first. */
	wmb();
	/* Dwords 1..3 are private until VALID is published in dword 0. */
	parhelia_write(pdev, reg, 0);
	parhelia_write(pdev, reg + 4, mapping->surface);
	parhelia_write(pdev, reg + 8, mapping->ctrl);
	parhelia_write(pdev, reg + 12, mapping->dims);
	wmb();
	parhelia_write(pdev, reg, PARHELIA_HOST_APERTURE_VALID |
		       pdev->aperture_coord_base |
		       lower_32_bits(mapping->node.start));
	parhelia_read(pdev, reg);
	parhelia_aperture_set_slot(mapping, slot);
}

static bool parhelia_host_view_evict_one_locked(struct parhelia_device *pdev);

static int parhelia_aperture_try_alloc_locked(struct parhelia_device *pdev,
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

static bool parhelia_engine_parked(struct parhelia_device *pdev)
{
	return pdev->submit_owner == current && !pdev->queued_slots;
}

/* Out of slots or window space: revoke least recently used host views.
 * Revocation reprograms the bank, so only a caller holding the engine
 * parked may do it. */
static int parhelia_aperture_alloc_locked(struct parhelia_device *pdev,
					  struct parhelia_aperture_mapping *mapping,
					  u32 map_size)
{
	int ret;

	do {
		ret = parhelia_aperture_try_alloc_locked(pdev, mapping, map_size);
	} while (ret == -ENOSPC && parhelia_engine_parked(pdev) &&
		 parhelia_host_view_evict_one_locked(pdev));
	return ret;
}

/* For callers that do not park the engine themselves: park it and retry
 * when only revoking host views can make room.  *parked tells the caller
 * to parhelia_submit_unlock() once its window is released. */
static int parhelia_aperture_alloc_window(struct parhelia_device *pdev,
					  struct parhelia_aperture_mapping *mapping,
					  u32 map_size, bool *parked)
{
	int ret = parhelia_aperture_alloc_locked(pdev, mapping, map_size);

	if (ret != -ENOSPC || parhelia_engine_parked(pdev) ||
	    pdev->submit_owner == current || list_empty(&pdev->host_view_lru))
		return ret;
	mutex_unlock(&pdev->aperture_lock);
	ret = parhelia_submit_idle_lock(pdev);
	mutex_lock(&pdev->aperture_lock);
	if (ret)
		return ret;
	*parked = true;
	return parhelia_aperture_alloc_locked(pdev, mapping, map_size);
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
		wmb();
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
	INIT_LIST_HEAD(&bo->host_views);
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
	       gem->funcs == &parhelia_view_object_funcs;
}

/* Byte pitch of the object's CPU mapping and of its framebuffers. */
u32 parhelia_bo_cpu_pitch(const struct parhelia_bo *bo)
{
	return bo->host_view ? bo->host_pitch : bo->gpu_pitch;
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

/* Engine address of @offset inside an explicit VRAM resource.  A TTM move
 * cannot use parhelia_vram_gpu_addr(): the destination resource is not
 * assigned to the object yet, and the source one is about to be replaced.
 * The bank encoding is the same as there. */
static int parhelia_resource_gpu_addr(struct parhelia_device *pdev,
				      struct ttm_resource *res, u64 offset,
				      u64 length, u32 *gpu_addr)
{
	u64 bank_size = pdev->vram_size >> 1;
	u64 physical, gpu;

	if (!bank_size || !res || res->mem_type != TTM_PL_VRAM || !length)
		return -EINVAL;
	physical = ((u64)res->start << PAGE_SHIFT) + offset;
	if (physical > U32_MAX || physical >= pdev->vram_size ||
	    length > pdev->vram_size - physical)
		return -ERANGE;
	/* One linear engine range cannot cross the gap between the banks. */
	if (physical < bank_size && physical + length > bank_size)
		return -ERANGE;
	gpu = physical >= bank_size ?
		PARHELIA_LOCAL_BANK_B_BASE + physical - bank_size : physical;
	if (gpu >= PARHELIA_HOST_APERTURE_BASE ||
	    length > PARHELIA_HOST_APERTURE_BASE - gpu)
		return -ERANGE;
	*gpu_addr = gpu;
	return 0;
}

/* Move an object from one VRAM resource to another through the aperture, one
 * window at a time via a bounce buffer: the window shows a single translated
 * range, so source and destination cannot be mapped at once.  This is what a
 * placement change between the colour, scanout and depth bands needs. */
int parhelia_vram_copy_resources(struct parhelia_device *pdev,
				 struct ttm_buffer_object *bo,
				 struct ttm_resource *from,
				 struct ttm_resource *to)
{
	struct parhelia_aperture_mapping mapping = { };
	u64 size = bo->base.size, done;
	void *bounce;
	bool parked = false;
	int ret;

	bounce = kmalloc(SZ_8K, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;
	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_window(pdev, &mapping, SZ_8K, &parked);
	for (done = 0; !ret && done < size; done += SZ_8K) {
		u32 bytes = min_t(u64, size - done, SZ_8K);
		u32 src, dst;

		ret = parhelia_resource_gpu_addr(pdev, from, done, bytes, &src);
		if (!ret)
			ret = parhelia_resource_gpu_addr(pdev, to, done, bytes,
							 &dst);
		if (ret)
			break;
		if (!mapping.active)
			parhelia_aperture_publish_locked(pdev, &mapping, src,
				0x10000020, 0x00010001, NULL);
		else
			parhelia_aperture_retarget_locked(pdev, &mapping, src,
				0x10000020, 0x00010001);
		parhelia_vram_read(bounce,
			      (u8 __iomem *)pdev->vram + mapping.node.start,
			      bytes);
		parhelia_aperture_retarget_locked(pdev, &mapping, dst,
			0x10000020, 0x00010001);
		memcpy_toio((u8 __iomem *)pdev->vram + mapping.node.start,
			    bounce, bytes);
		wmb();
		parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(mapping.slot));
	}
	if (mapping.active)
		parhelia_aperture_release_locked(pdev, &mapping);
	else
		parhelia_aperture_free_unpublished_locked(&mapping);
	mutex_unlock(&pdev->aperture_lock);
	kfree(bounce);
	return ret;
}

/* Copy a whole object between a VRAM resource and its system pages through
 * the translated host aperture, so TTM can evict and restore it.
 *
 * TTM's own move is a CPU copy over BAR0, which on this chip is a translated
 * window rather than a linear view of VRAM: a moved object kept only the
 * bytes its first window happened to cover.  This takes aperture_lock alone,
 * never submit_lock: a move runs inside a validate that may already hold it,
 * and an object TTM is moving is by definition unpinned, so no queued submit
 * can have it bound.
 */
int parhelia_vram_move_pages(struct parhelia_device *pdev,
			     struct ttm_buffer_object *bo,
			     struct ttm_resource *vram, bool to_vram)
{
	struct parhelia_aperture_mapping mapping = { };
	struct ttm_tt *tt = bo->ttm;
	u64 size = bo->base.size, done;
	bool parked = false;
	int ret;

	if (!tt || !ttm_tt_is_populated(tt) || !tt->pages)
		return -EINVAL;

	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_window(pdev, &mapping, SZ_8K, &parked);
	for (done = 0; !ret && done < size; ) {
		u32 bytes = min_t(u64, size - done, SZ_8K);
		u32 surface_address, in_window = 0;

		ret = parhelia_resource_gpu_addr(pdev, vram, done, bytes,
						 &surface_address);
		if (ret)
			break;
		if (!mapping.active)
			parhelia_aperture_publish_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001, NULL);
		else
			parhelia_aperture_retarget_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001);

		/* The window spans two pages at most, but say it in general. */
		while (in_window < bytes) {
			u64 at = done + in_window;
			u32 page_offset = offset_in_page(at);
			u32 chunk = min_t(u32, bytes - in_window,
					  PAGE_SIZE - page_offset);
			void __iomem *window = (u8 __iomem *)pdev->vram +
					       mapping.node.start + in_window;
			void *page;

			if (!tt->pages[at >> PAGE_SHIFT]) {
				ret = -EFAULT;
				break;
			}
			page = kmap_local_page(tt->pages[at >> PAGE_SHIFT]);
			if (to_vram)
				memcpy_toio(window, page + page_offset, chunk);
			else
				parhelia_vram_read(page + page_offset, window, chunk);
			kunmap_local(page);
			in_window += chunk;
		}
		if (!ret && to_vram) {
			wmb();
			parhelia_read(pdev,
				      PARHELIA_HOST_APERTURE_SLOT(mapping.slot));
		}
		done += bytes;
	}
	if (mapping.active)
		parhelia_aperture_release_locked(pdev, &mapping);
	else
		parhelia_aperture_free_unpublished_locked(&mapping);
	mutex_unlock(&pdev->aperture_lock);
	return ret;
}

/* Record the whole descriptor bank before engine init replaces it. */
int parhelia_host_aperture_snapshot(struct parhelia_device *pdev)
{
	unsigned int i, n = PARHELIA_HOST_APERTURE_SLOTS * 4;

	pdev->fw_aperture = devm_kcalloc(&pdev->pdev->dev, n,
					 sizeof(*pdev->fw_aperture), GFP_KERNEL);
	if (!pdev->fw_aperture)
		return -ENOMEM;
	for (i = 0; i < n; i++)
		pdev->fw_aperture[i] = parhelia_read(pdev, i * 4);
	return 0;
}

/* Hand the bank back as firmware left it, the way the vendor's WARP
 * self-test restores it: every slot invalid, then payloads, then dword 0. */
static void parhelia_host_aperture_restore(struct parhelia_device *pdev)
{
	unsigned int slot, i;

	for (slot = 0; slot < PARHELIA_HOST_APERTURE_SLOTS; slot++)
		parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(slot), 0);
	for (slot = 0; slot < PARHELIA_HOST_APERTURE_SLOTS; slot++)
		for (i = 1; i < 4; i++)
			parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(slot) + i * 4,
				       pdev->fw_aperture[slot * 4 + i]);
	for (slot = 0; slot < PARHELIA_HOST_APERTURE_SLOTS; slot++)
		parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(slot),
			       pdev->fw_aperture[slot * 4]);
	parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(0));
}

void parhelia_host_aperture_fini(struct parhelia_device *pdev)
{
	unsigned int slot;

	flush_work(&pdev->host_view_work);
	mutex_lock(&pdev->aperture_lock);
	while (parhelia_host_view_evict_one_locked(pdev))
		;
	if (pdev->aperture_live) {
		for (slot = 1; slot < PARHELIA_HOST_APERTURE_SLOTS; slot++)
			parhelia_write(pdev, PARHELIA_HOST_APERTURE_SLOT(slot), 0);
		parhelia_read(pdev,
			      PARHELIA_HOST_APERTURE_SLOT(PARHELIA_HOST_APERTURE_SLOTS - 1));
		pdev->aperture_live = false;
	}
	if (pdev->raw_aperture_owned && pdev->fw_aperture) {
		parhelia_host_aperture_restore(pdev);
		pdev->fw_aperture = NULL;
	}
	mutex_unlock(&pdev->aperture_lock);
}

static void parhelia_raw_object_free(struct drm_gem_object *gem)
{
	struct parhelia_device *pdev = to_parhelia(gem->dev);

	parhelia_root_trace_check(pdev, "raw_free:enter",
				  to_parhelia_bo(gem)->layout, gem->size, 0);
	if (drm_gem_vram_of_gem(gem)->bo.pin_count)
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

static void parhelia_view_object_free(struct drm_gem_object *gem);
static int parhelia_view_object_mmap(struct drm_gem_object *gem,
				     struct vm_area_struct *vma);
static const struct vm_operations_struct parhelia_view_vm_ops;

static const struct drm_gem_object_funcs parhelia_raw_object_funcs = {
	.free = parhelia_view_object_free,
	.vmap = parhelia_raw_object_vmap,
	.vunmap = parhelia_raw_object_vunmap,
	.mmap = parhelia_view_object_mmap,
	.vm_ops = &parhelia_view_vm_ops,
	.print_info = drm_gem_ttm_print_info,
};

/* Native sampler storage has no meaningful raw CPU address on P690.  It is
 * populated through UPLOAD_RENDER's transient translated aperture instead. */
static const struct drm_gem_object_funcs parhelia_unmappable_object_funcs = {
	.free = parhelia_raw_object_free,
	.print_info = drm_gem_ttm_print_info,
};

static void parhelia_host_vm_close(struct vm_area_struct *vma)
{
	/* Publish outstanding WC stores before the mapping can release its final
	 * GEM reference and invalidate the descriptor.
	 */
	wmb();
	drm_gem_vm_close(vma);
}


/*
 * Native class targets have no permanent descriptor.  Like a GTT mmap, a CPU
 * fault binds a type-3 view of the object (or of a row band of a large one)
 * into a free slot, inserts its BAR0 pages, and least recently used views are
 * revoked - user PTEs first - when slots or window space run out.  A view
 * stays valid until the object moves, which drops it under the engine park.
 * Raw objects (linear storage, legacy targets) get untranslated type-0 views
 * of their bytes in the same way: a raw BAR0 mapping above 16 MiB folds onto
 * the descriptor window instead of reaching the object.
 */
struct parhelia_host_view {
	struct parhelia_aperture_mapping mapping;
	struct parhelia_bo *bo;
	struct list_head bo_link;
	struct list_head lru_link;
	/* the CPU mapping's bytes this view serves */
	u64 offset;
	u32 size;
	u32 vmap_count;
};

/* Objects whose CPU view fits this window are bound whole, larger ones in
 * row bands of about a mebibyte. */
#define PARHELIA_HOST_VIEW_WHOLE	SZ_4M
#define PARHELIA_HOST_VIEW_BAND		SZ_1M
/* The type-0 window shape the transfer paths use: 8 KiB of raw bytes. */
#define PARHELIA_RAW_VIEW		SZ_8K
#define PARHELIA_RAW_VIEW_CTRL		0x10000020
#define PARHELIA_RAW_VIEW_DIMS		0x00010001

static bool parhelia_gem_has_views(const struct drm_gem_object *gem)
{
	return gem->funcs == &parhelia_view_object_funcs ||
	       gem->funcs == &parhelia_raw_object_funcs;
}

static u64 parhelia_host_view_extent(const struct parhelia_bo *bo)
{
	return bo->host_view ? bo->host_size : bo->gbo.bo.base.size;
}

static void parhelia_host_view_destroy_locked(struct parhelia_device *pdev,
					      struct parhelia_host_view *view)
{
	struct parhelia_bo *bo = view->bo;

	lockdep_assert_held(&pdev->aperture_lock);
	if (bo) {
		unmap_mapping_range(pdev->drm.anon_inode->i_mapping,
			drm_vma_node_offset_addr(&bo->gbo.bo.base.vma_node) +
			view->offset, view->size, 1);
		list_del(&view->bo_link);
	}
	list_del(&view->lru_link);
	parhelia_aperture_release_locked(pdev, &view->mapping);
	kfree(view);
}

static bool parhelia_host_view_evict_one_locked(struct parhelia_device *pdev)
{
	lockdep_assert_held(&pdev->aperture_lock);
	if (list_empty(&pdev->host_view_lru))
		return false;
	parhelia_host_view_destroy_locked(pdev,
		list_first_entry(&pdev->host_view_lru,
				 struct parhelia_host_view, lru_link));
	return true;
}

/* Called by TTM moves with the engine parked: the views name the old place. */
void parhelia_host_views_drop(struct parhelia_device *pdev,
			      struct ttm_buffer_object *tbo)
{
	struct parhelia_host_view *view, *next;
	struct parhelia_bo *bo;

	if (!parhelia_gem_has_views(&tbo->base))
		return;
	bo = to_parhelia_bo(&tbo->base);
	mutex_lock(&pdev->aperture_lock);
	list_for_each_entry_safe(view, next, &bo->host_views, bo_link)
		parhelia_host_view_destroy_locked(pdev, view);
	mutex_unlock(&pdev->aperture_lock);
}

/* New GPU work on the object: the next CPU access must fault and wait. */
void parhelia_host_views_revoke_cpu(struct parhelia_device *pdev,
				    struct drm_gem_object *gem)
{
	if (!parhelia_gem_has_views(gem))
		return;
	mutex_lock(&pdev->aperture_lock);
	if (!list_empty(&to_parhelia_bo(gem)->host_views))
		drm_vma_node_unmap(&gem->vma_node,
				   pdev->drm.anon_inode->i_mapping);
	mutex_unlock(&pdev->aperture_lock);
}

static u32 parhelia_host_view_rows(const struct parhelia_bo *bo)
{
	u32 granule = bo->layout == PARHELIA_BO_LAYOUT_NATIVE_64X16 ? 32 : 64;

	/* Bands start on even tile rows: the in-tile order alternates. */
	if (bo->host_size <= PARHELIA_HOST_VIEW_WHOLE)
		return bo->alloc_height;
	return max(granule, rounddown(PARHELIA_HOST_VIEW_BAND / bo->host_pitch,
				      granule));
}

static struct parhelia_host_view *
parhelia_host_view_find_locked(struct parhelia_bo *bo, u64 offset)
{
	struct parhelia_host_view *view;

	list_for_each_entry(view, &bo->host_views, bo_link)
		if (offset >= view->offset && offset - view->offset < view->size)
			return view;
	return NULL;
}

/* A view of the whole CPU mapping, as kernel vmap needs it. */
static struct parhelia_host_view *
parhelia_host_view_find_whole_locked(struct parhelia_bo *bo)
{
	struct parhelia_host_view *view;

	list_for_each_entry(view, &bo->host_views, bo_link)
		if (!view->offset && view->size >= parhelia_host_view_extent(bo))
			return view;
	return NULL;
}

static struct parhelia_host_view *
parhelia_host_view_create_locked(struct parhelia_device *pdev,
				 struct parhelia_bo *bo, u64 offset, bool whole)
{
	struct parhelia_host_view *view;
	u32 surface, ctrl, dims, first_row = 0, rows = 0;
	u64 gpu_offset, gpu_size;
	int ret;

	lockdep_assert_held(&pdev->aperture_lock);
	view = kzalloc_obj(*view);
	if (!view)
		return ERR_PTR(-ENOMEM);
	if (bo->host_view) {
		u32 band = whole ? bo->alloc_height : parhelia_host_view_rows(bo);
		u32 format;

		first_row = rounddown((u32)(offset / bo->host_pitch), band);
		rows = min(band, bo->alloc_height - first_row);
		view->offset = (u64)first_row * bo->host_pitch;
		view->size = rows * bo->host_pitch;
		gpu_offset = (u64)first_row * bo->gpu_pitch;
		gpu_size = (u64)rows * bo->gpu_pitch;
		format = bo->cpp == 2 ? host_format_rgb565 :
					PARHELIA_HOST_FORMAT_XRGB8888;
		ctrl = (format << 24) | (3u << 16) | (bo->gpu_pitch / bo->cpp);
		dims = BIT(16) | rows;
	} else {
		view->offset = rounddown(offset, PARHELIA_RAW_VIEW);
		view->size = min_t(u64, PARHELIA_RAW_VIEW,
				   bo->gbo.bo.base.size - view->offset);
		gpu_offset = view->offset;
		gpu_size = view->size;
		ctrl = PARHELIA_RAW_VIEW_CTRL;
		dims = PARHELIA_RAW_VIEW_DIMS;
	}
	ret = parhelia_vram_gpu_addr(pdev, &bo->gbo, gpu_offset, gpu_size,
				     &surface);
	if (!ret)
		ret = parhelia_aperture_alloc_locked(pdev, &view->mapping,
						     view->size);
	if (ret) {
		kfree(view);
		return ERR_PTR(ret);
	}
	parhelia_aperture_publish_locked(pdev, &view->mapping, surface, ctrl,
					 dims, NULL);
	view->bo = bo;
	list_add(&view->bo_link, &bo->host_views);
	list_add_tail(&view->lru_link, &pdev->host_view_lru);
	return view;
}

/* CPU physical address of BAR0 for user mappings. */
static phys_addr_t parhelia_vram_cpu_phys(struct parhelia_device *pdev)
{
#ifdef CONFIG_ALPHA
	struct pci_controller *hose = pdev->pdev->sysdata;

	/* Alpha CPUs reach PCI memory through the hose's dense space. */
	return pci_bus_address(pdev->pdev, 0) + hose->dense_mem_base;
#else
	return pci_resource_start(pdev->pdev, 0);
#endif
}

/* Insert the view's pages from the faulting one to the end of view or VMA. */
static vm_fault_t parhelia_host_view_insert(struct vm_fault *vmf,
					    struct parhelia_host_view *view,
					    pgoff_t page)
{
	struct vm_area_struct *vma = vmf->vma;
	struct parhelia_device *pdev = to_parhelia(view->bo->gbo.bo.base.dev);
	u64 view_offset = view->offset;
	pgoff_t view_end = (view_offset + view->size) >> PAGE_SHIFT;
	unsigned long address = vmf->address;
	unsigned long pfn;
	vm_fault_t ret = VM_FAULT_NOPAGE;

	pfn = (parhelia_vram_cpu_phys(pdev) + view->mapping.node.start +
	       ((u64)page << PAGE_SHIFT) - view_offset) >> PAGE_SHIFT;
	/* a vmapped view stays off the eviction list */
	if (!view->vmap_count)
		list_move_tail(&view->lru_link, &pdev->host_view_lru);
	for (; page < view_end && address < vma->vm_end;
	     page++, pfn++, address += PAGE_SIZE) {
		ret = vmf_insert_pfn_prot(vma, address, pfn, vma->vm_page_prot);
		if (ret & VM_FAULT_ERROR)
			return address == vmf->address ? ret : VM_FAULT_NOPAGE;
	}
	return VM_FAULT_NOPAGE;
}

static vm_fault_t parhelia_host_view_errno(int err)
{
	if (err == -EINTR || err == -ERESTARTSYS || err == -EAGAIN)
		return VM_FAULT_NOPAGE;
	return err == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS;
}

static vm_fault_t parhelia_host_view_fault(struct vm_fault *vmf)
{
	struct drm_gem_object *gem = vmf->vma->vm_private_data;
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	struct parhelia_host_view *view;
	pgoff_t page;
	vm_fault_t ret;
	long wait;
	u64 offset;
	int err;

	page = vmf->pgoff - drm_vma_node_start(&gem->vma_node);
	offset = (u64)page << PAGE_SHIFT;
	if (vmf->pgoff < drm_vma_node_start(&gem->vma_node) ||
	    offset >= parhelia_host_view_extent(bo))
		return VM_FAULT_SIGBUS;

	/* A bound view is current while the reservation holds the placement. */
	if (dma_resv_lock_interruptible(gem->resv, NULL))
		return VM_FAULT_NOPAGE;
	wait = dma_resv_wait_timeout(gem->resv, DMA_RESV_USAGE_BOOKKEEP, true,
				     MAX_SCHEDULE_TIMEOUT);
	if (wait < 0) {
		dma_resv_unlock(gem->resv);
		return parhelia_host_view_errno(wait);
	}
	mutex_lock(&pdev->aperture_lock);
	view = parhelia_host_view_find_locked(bo, offset);
	ret = view ? parhelia_host_view_insert(vmf, view, page) : 0;
	mutex_unlock(&pdev->aperture_lock);
	dma_resv_unlock(gem->resv);
	if (view)
		return ret;

	/* Binding programs the shared bank, which needs the engine parked;
	 * the pin keeps the object in place without holding its reservation,
	 * which a queued submit takes under submit_lock. */
	err = parhelia_vram_pin(&bo->gbo);
	if (err)
		return parhelia_host_view_errno(err);
	err = parhelia_submit_idle_lock(pdev);
	if (err) {
		parhelia_vram_unpin(&bo->gbo);
		return parhelia_host_view_errno(err);
	}
	mutex_lock(&pdev->aperture_lock);
	view = parhelia_host_view_find_locked(bo, offset);
	if (!view)
		view = parhelia_host_view_create_locked(pdev, bo, offset, false);
	ret = IS_ERR(view) ? parhelia_host_view_errno(PTR_ERR(view)) :
			     parhelia_host_view_insert(vmf, view, page);
	mutex_unlock(&pdev->aperture_lock);
	parhelia_submit_unlock(pdev);
	parhelia_vram_unpin(&bo->gbo);
	return ret;
}

static const struct vm_operations_struct parhelia_view_vm_ops = {
	.fault = parhelia_host_view_fault,
	.open = drm_gem_vm_open,
	.close = parhelia_host_vm_close,
};

static int parhelia_view_object_mmap(struct drm_gem_object *gem,
				     struct vm_area_struct *vma)
{
	/* Revocation zaps the DRM device's address space; a dma-buf VMA lives
	 * in another one and could keep a revoked view mapped. */
	if (!vma->vm_file ||
	    vma->vm_file->f_mapping != gem->dev->anon_inode->i_mapping)
		return -EOPNOTSUPP;
	vma->vm_ops = &parhelia_view_vm_ops;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	return 0;
}

/* Revoke the views of freed objects with the engine parked, and only then
 * return their storage: a live descriptor naming reused VRAM corrupted the
 * transfer windows of the next object placed there. */
static void parhelia_host_view_reap(struct work_struct *work)
{
	struct parhelia_device *pdev = container_of(work, struct parhelia_device,
						     host_view_work);
	struct parhelia_host_view *view, *next;
	struct parhelia_bo *bo, *tmp;
	LIST_HEAD(dead);
	int parked;

	/* A wedged engine runs nothing, so revoking without the park is safe. */
	parked = parhelia_submit_idle_lock(pdev);
	mutex_lock(&pdev->aperture_lock);
	list_splice_init(&pdev->host_view_dead, &dead);
	list_for_each_entry(bo, &dead, dead_link)
		list_for_each_entry_safe(view, next, &bo->host_views, bo_link)
			parhelia_host_view_destroy_locked(pdev, view);
	mutex_unlock(&pdev->aperture_lock);
	if (!parked)
		parhelia_submit_unlock(pdev);
	list_for_each_entry_safe(bo, tmp, &dead, dead_link)
		parhelia_raw_object_free(&bo->gbo.bo.base);
}

static void parhelia_view_object_free(struct drm_gem_object *gem)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	bool views;

	/* The final reference can drop under submit_lock, which cannot park. */
	mutex_lock(&pdev->aperture_lock);
	views = !list_empty(&bo->host_views);
	if (views)
		list_add_tail(&bo->dead_link, &pdev->host_view_dead);
	mutex_unlock(&pdev->aperture_lock);
	if (views)
		schedule_work(&pdev->host_view_work);
	else
		parhelia_raw_object_free(gem);
}

/* Kernel access (cursor upload, fbdev damage blit) through one view of the
 * whole object, beside any mmap bands; it stays bound and off the eviction
 * list until vunmap.  Objects beyond the aperture window get -E2BIG. */
static int parhelia_view_object_vmap(struct drm_gem_object *gem,
				     struct iosys_map *map)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	struct parhelia_host_view *view;
	int ret;

	if (pdev->submit_owner == current)
		return -EBUSY;
	ret = parhelia_vram_pin_locked(&bo->gbo);
	if (ret)
		return ret;
	mutex_lock(&pdev->aperture_lock);
	view = parhelia_host_view_find_whole_locked(bo);
	if (view && !view->vmap_count++)
		list_del_init(&view->lru_link);
	mutex_unlock(&pdev->aperture_lock);
	if (!view) {
		/* Binding parks the engine, and submits take reservations under
		 * submit_lock: drop the caller's, the pin keeps the placement. */
		dma_resv_unlock(gem->resv);
		ret = parhelia_submit_idle_lock(pdev);
		if (!ret) {
			mutex_lock(&pdev->aperture_lock);
			view = parhelia_host_view_find_whole_locked(bo);
			if (!view)
				view = parhelia_host_view_create_locked(pdev, bo,
									0, true);
			if (IS_ERR(view))
				ret = PTR_ERR(view);
			else if (!view->vmap_count++)
				list_del_init(&view->lru_link);
			mutex_unlock(&pdev->aperture_lock);
			parhelia_submit_unlock(pdev);
		}
		dma_resv_lock(gem->resv, NULL);
	}
	if (ret) {
		ttm_bo_unpin(&bo->gbo.bo);
		return ret;
	}
	iosys_map_set_vaddr_iomem(map, (u8 __iomem *)pdev->vram +
				  view->mapping.node.start);
	return 0;
}

static void parhelia_view_object_vunmap(struct drm_gem_object *gem,
					struct iosys_map *map)
{
	struct parhelia_bo *bo = to_parhelia_bo(gem);
	struct parhelia_device *pdev = to_parhelia(gem->dev);
	struct parhelia_host_view *view;

	mutex_lock(&pdev->aperture_lock);
	view = parhelia_host_view_find_whole_locked(bo);
	if (view && view->vmap_count && !--view->vmap_count)
		list_add_tail(&view->lru_link, &pdev->host_view_lru);
	mutex_unlock(&pdev->aperture_lock);
	iosys_map_clear(map);
	ttm_bo_unpin(&bo->gbo.bo);
}

static const struct drm_gem_object_funcs parhelia_view_object_funcs = {
	.free = parhelia_view_object_free,
	.vmap = parhelia_view_object_vmap,
	.vunmap = parhelia_view_object_vunmap,
	.mmap = parhelia_view_object_mmap,
	.vm_ops = &parhelia_view_vm_ops,
	.print_info = drm_gem_ttm_print_info,
};

/* Give a native class target its demand-bound CPU-linear view.  The view's
 * pitch is the vendor host pitch, so the mmap can outgrow the object. */
int parhelia_bo_init_host_view(struct drm_gem_vram_object *gbo, u32 cpp,
			       u32 alloc_height)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);
	struct drm_gem_object *gem = &gbo->bo.base;
	struct drm_vma_offset_manager *mgr = gem->dev->vma_offset_manager;
	u64 host_size;
	int ret;

	if (!parhelia_bo_is_native(bo) || (cpp != 2 && cpp != 4) ||
	    bo->gpu_pitch % cpp || !alloc_height)
		return -EINVAL;
	bo->cpp = cpp;
	bo->alloc_height = alloc_height;
	bo->host_pitch = roundup_pow_of_two(bo->gpu_pitch / cpp) * cpp;
	host_size = (u64)bo->host_pitch * alloc_height;
	if (!IS_ALIGNED(host_size, PAGE_SIZE) || host_size > SZ_64M)
		return -EINVAL;
	bo->host_size = host_size;
	if (drm_vma_node_size(&gem->vma_node) < host_size >> PAGE_SHIFT) {
		drm_vma_offset_remove(mgr, &gem->vma_node);
		ret = drm_vma_offset_add(mgr, &gem->vma_node,
					 host_size >> PAGE_SHIFT);
		if (ret)
			return ret;
	}
	bo->host_view = true;
	gem->funcs = &parhelia_view_object_funcs;
	return 0;
}

/* Zero [@offset, @offset + @len) of a pinned object through a kernel
 * aperture window. */
int parhelia_vram_clear_range(struct parhelia_device *pdev,
			      struct drm_gem_vram_object *gbo, u64 offset,
			      u64 len)
{
	struct parhelia_aperture_mapping mapping = { };
	u64 done;
	u32 bytes = 0;
	bool parked = false;
	int ret;

	if (offset > gbo->bo.base.size || len > gbo->bo.base.size - offset)
		return -EINVAL;
	if (!len)
		return 0;
	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_window(pdev, &mapping, SZ_8K, &parked);
	for (done = 0; !ret && done < len; done += bytes) {
		u32 skew = (offset + done) & (parhelia_aperture_addr_unit(pdev) - 1);
		u32 surface_address;

		bytes = min_t(u64, len - done, SZ_8K - skew);
		ret = parhelia_vram_gpu_addr(pdev, gbo, offset + done - skew,
					     bytes + skew, &surface_address);
		if (ret)
			break;
		if (!mapping.active)
			parhelia_aperture_publish_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001, NULL);
		else
			parhelia_aperture_retarget_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001);
		memset_io((u8 __iomem *)pdev->vram + mapping.node.start + skew,
			  0, bytes);
		wmb();
		parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(mapping.slot));
	}
	if (mapping.active)
		parhelia_aperture_release_locked(pdev, &mapping);
	else
		parhelia_aperture_free_unpublished_locked(&mapping);
	mutex_unlock(&pdev->aperture_lock);
	if (parked)
		parhelia_submit_unlock(pdev);
	return ret;
}

int parhelia_vram_clear(struct parhelia_device *pdev,
			struct drm_gem_vram_object *gbo)
{
	return parhelia_vram_clear_range(pdev, gbo, 0, gbo->bo.base.size);
}

/* Copy @len bytes into a VRAM object through the driver's own aperture window.
 * BAR0 is translated by the host-aperture descriptors, so a plain vmap of the
 * object does not reliably reach its storage: on the original family a cursor
 * image written that way never arrived in VRAM. */
int parhelia_vram_write(struct parhelia_device *pdev,
			struct drm_gem_vram_object *gbo, u64 offset,
			const void *src, size_t len)
{
	struct parhelia_aperture_mapping mapping = { };
	u64 done;
	u32 bytes = 0;
	bool parked = false;
	int ret;

	if (offset > gbo->bo.base.size || len > gbo->bo.base.size - offset)
		return -EINVAL;
	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_window(pdev, &mapping, SZ_8K, &parked);
	for (done = 0; !ret && done < len; done += bytes) {
		u32 skew = (offset + done) & (parhelia_aperture_addr_unit(pdev) - 1);
		u32 surface_address;

		bytes = min_t(u64, len - done, SZ_8K - skew);
		ret = parhelia_vram_gpu_addr(pdev, gbo, offset + done - skew,
					     bytes + skew, &surface_address);
		if (ret)
			break;
		if (!mapping.active)
			parhelia_aperture_publish_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001, NULL);
		else
			parhelia_aperture_retarget_locked(pdev, &mapping,
				surface_address, 0x10000020, 0x00010001);
		memcpy_toio((u8 __iomem *)pdev->vram + mapping.node.start + skew,
			    (const u8 *)src + done, bytes);
		wmb();
		parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(mapping.slot));
	}
	if (mapping.active)
		parhelia_aperture_release_locked(pdev, &mapping);
	else
		parhelia_aperture_free_unpublished_locked(&mapping);
	mutex_unlock(&pdev->aperture_lock);
	if (parked)
		parhelia_submit_unlock(pdev);
	return ret;
}

int parhelia_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args)
{
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_gem_vram_object *gbo;
	unsigned long native_align_pages;
	u32 gpu_pitch_pixels, host_pitch_pixels, alloc_height, cpp, tile_width;
	size_t host_size, native_size, size;
	u32 handle;
	int ret;

	if (!args->width || !args->height || args->width > 2048 ||
	    args->height > 2048 || !pdev->raw_aperture_owned)
		return -EINVAL;
	/* 16 bpp keeps the same tile bytes and rows over twice the width. */
	if (args->bpp != 32 &&
	    (args->bpp != 16 ||
	     pdev->chip->tile_layout != PARHELIA_TILE_64X16_4K))
		return -EINVAL;
	cpp = args->bpp / 8;
	tile_width = 64 * 4 / cpp;
	gpu_pitch_pixels = ALIGN(args->width, tile_width);
	host_pitch_pixels = roundup_pow_of_two(max_t(u32, args->width,
						     tile_width));
	alloc_height = ALIGN(args->height,
			     pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ?
			     16 : 32);
	if (check_mul_overflow((size_t)host_pitch_pixels * cpp,
			       (size_t)alloc_height, &host_size) ||
	    check_mul_overflow((size_t)gpu_pitch_pixels * cpp,
			       (size_t)alloc_height, &native_size))
		return -EOVERFLOW;
	host_size = PAGE_ALIGN(host_size);
	native_size = PAGE_ALIGN(native_size);
	size = max(host_size, native_size);

	native_align_pages = pdev->chip->tile_layout == PARHELIA_TILE_64X32_8K ? 2 : 1;
	gbo = drm_gem_vram_create(drm, size, native_align_pages);
	if (IS_ERR(gbo))
		return PTR_ERR(gbo);
	parhelia_bo_set_layout(gbo, pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ?
			       PARHELIA_BO_LAYOUT_NATIVE_64X16 :
			       PARHELIA_BO_LAYOUT_NATIVE_64X32,
			       args->width, args->height, gpu_pitch_pixels * cpp);
	/* The CPU view is demand-bound like a native class target's; the
	 * allocation still covers the whole view for generic dumb users. */
	ret = parhelia_bo_init_host_view(gbo, cpp, alloc_height);
	if (!ret && to_parhelia_bo(&gbo->bo.base)->host_pitch !=
		    host_pitch_pixels * cpp)
		ret = -EINVAL;
	/* Dumb objects are KMS framebuffers: refresh them from the P690 display
	 * band so it does not compete with bank-A colour and texture traffic. */
	if (!ret && pdev->pdev->device == PARHELIA_P690) {
		parhelia_bo_set_placement_class(gbo,
			PARHELIA_PLACEMENT_NATIVE_SCANOUT);
		ret = parhelia_vram_pin_new(pdev, gbo);
		if (ret == -ENOSPC || ret == -ENOMEM || ret == -E2BIG) {
			parhelia_bo_set_placement_class(gbo,
				PARHELIA_PLACEMENT_LEGACY);
			ret = parhelia_vram_pin_new(pdev, gbo);
		}
	} else if (!ret)
		ret = parhelia_vram_pin_new(pdev, gbo);
	if (ret) {
		drm_gem_object_put(&gbo->bo.base);
		return ret;
	}
	/* TTM does not clear new P690 objects. */
	ret = parhelia_bo_clear(pdev, file, gbo);
	parhelia_vram_unpin(gbo);
	if (!ret)
		ret = drm_gem_handle_create(file, &gbo->bo.base, &handle);
	if (ret) {
		drm_gem_object_put(&gbo->bo.base);
		return ret;
	}
	args->handle = handle;
	args->pitch = host_pitch_pixels * cpp;
	args->size = host_size;
	drm_gem_object_put(&gbo->bo.base);
	return 0;
}

int parhelia_get_bo_info_ioctl(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct drm_parhelia_get_bo_info *args = data;
	struct drm_gem_object *gem;
	struct parhelia_bo *bo;

	if (!args->handle || args->flags || args->layout || args->width ||
	    args->height || args->pitch || args->host_pitch || args->size)
		return -EINVAL;
	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;
	if (!parhelia_gem_is_owned(gem)) {
		drm_gem_object_put(gem);
		return -EINVAL;
	}
	bo = to_parhelia_bo(gem);
	args->flags = bo->host_view ? PARHELIA_BO_INFO_HOST_APERTURE : 0;
	args->layout = bo->layout;
	args->width = bo->width;
	args->height = bo->height;
	args->pitch = bo->gpu_pitch;
	if (args->flags & PARHELIA_BO_INFO_HOST_APERTURE)
		args->host_pitch = bo->host_pitch;
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
	void *buffer = NULL;
	bool parked = false, engine, async_up;
	int ret;

	if (!pdev->raw_aperture_owned)
		return -EOPNOTSUPP;
	if (!args->data || !args->handle || !args->size || args->flags ||
	    args->pad || !IS_ALIGNED(args->offset, 4) ||
	    !IS_ALIGNED(args->size, 4) || args->size > SZ_16M ||
	    args->size > SIZE_MAX)
		return -EINVAL;

	engine = pdev->pdev->device == PARHELIA_P690 && engine_transfer &&
		 !texture_upload_trace &&
		 args->size >= (download ? engine_download_min : engine_upload_min);
	/* Queued engine uploads copy straight from the caller into the ring. */
	async_up = engine && !download && engine_upload_async;
	if (download) {
		buffer = kvmalloc(args->size, GFP_KERNEL);
		if (!buffer)
			return -ENOMEM;
	} else if (!async_up) {
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
	      (pdev->pdev->device != PARHELIA_P690 &&
	       (bo->layout == PARHELIA_BO_LAYOUT_NATIVE_64X32 ||
		bo->layout == PARHELIA_BO_LAYOUT_NATIVE_64X16)) ||
	      bo->placement_class == PARHELIA_PLACEMENT_NATIVE_COLOR ||
	      bo->placement_class == PARHELIA_PLACEMENT_NATIVE_DEPTH ||
	      bo->placement_class == PARHELIA_PLACEMENT_NATIVE_SCANOUT) ||
	    args->offset >= gem->size || args->size > gem->size - args->offset) {
		ret = -EINVAL;
		goto out_gem;
	}

	/* P690: the 2-D engine moves the data through class-3 staging. */
	if (async_up) {
		ret = parhelia_engine_upload_async(pdev, file, &bo->gbo,
				args->offset, u64_to_user_ptr(args->data),
				args->size);
		if (!ret || ret == -EINTR || ret == -ERESTARTSYS || ret == -EFAULT)
			goto out_gem;
		if (ret != -EOPNOTSUPP)
			dev_warn_ratelimited(&pdev->pdev->dev,
				"queued engine upload of %llu bytes failed (%d), using the aperture\n",
				(unsigned long long)args->size, ret);
		buffer = vmemdup_user(u64_to_user_ptr(args->data), args->size);
		if (IS_ERR(buffer)) {
			ret = PTR_ERR(buffer);
			buffer = NULL;
			goto out_gem;
		}
	} else if (engine) {
		ret = parhelia_engine_transfer(pdev, file, &bo->gbo, args->offset,
					       buffer, args->size, download);
		if (!ret || ret == -EINTR || ret == -ERESTARTSYS)
			goto out_gem;
		dev_warn_ratelimited(&pdev->pdev->dev,
			"engine %s of %llu bytes failed (%d), using the aperture\n",
			download ? "download" : "upload",
			(unsigned long long)args->size, ret);
	}

	/* No submit may bind the target while its temporary predecessor mapping is
	 * installed.  aperture_lock also keeps the mapping's compacted slot stable
	 * for the duration of the copy.
	 */
	/* Pin before the idle lock: an evicted object moves back in through
	 * parhelia_ttm_move(), which takes that lock itself. */
	ret = parhelia_vram_pin(&bo->gbo);
	if (ret)
		goto out_gem;
	ret = parhelia_submit_idle_lock(pdev);
	if (ret)
		goto out_unpin;
	mutex_lock(&pdev->aperture_lock);
	ret = parhelia_aperture_alloc_window(pdev, &mapping, SZ_8K, &parked);
	if (ret)
		goto out_aperture_unlock;

	for (copied = 0; copied < args->size;) {
		/* The descriptor addresses VRAM in 32/64-byte units: publish the unit
		 * holding the offset and copy from inside the window. */
		u32 skew = (args->offset + copied) &
			   (parhelia_aperture_addr_unit(pdev) - 1);
		u32 bytes = min_t(u64, args->size - copied, SZ_8K - skew);
		u32 root_before[3], root_after[3];
		bool watch_root;

		if (!IS_ALIGNED(bytes, sizeof(u32))) {
			ret = -EINVAL;
			goto out_mapping;
		}
		ret = parhelia_vram_gpu_addr(pdev, &bo->gbo,
				args->offset + copied - skew, bytes + skew,
				&surface_address);
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
			parhelia_vram_read((u8 *)buffer + copied,
				      (u8 __iomem *)pdev->vram + mapping.node.start +
				      skew, bytes);
		} else {
			memcpy_toio((u8 __iomem *)pdev->vram + mapping.node.start + skew,
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
	parhelia_submit_unlock(pdev);
out_unpin:
	parhelia_vram_unpin(&bo->gbo);
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
