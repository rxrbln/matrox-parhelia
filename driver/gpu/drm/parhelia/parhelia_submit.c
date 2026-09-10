// SPDX-License-Identifier: GPL-2.0-only
/* Validated submission for the Parhelia WARP DMA engine.
 *
 * The primary ring, section packets and vertex fetch buffer are DMA32 memory
 * owned by the kernel. The UAPI carries no raw device address: VRAM operands
 * are resolved from GEM handles and 0x97 vertex ranges from the copied upload.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>

#include <drm/drm_gem.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/ttm/ttm_bo.h>

#include "parhelia_drv.h"
#include "parhelia_packets.h"
#include "parhelia_firmware.h"
#include "parhelia_warp_state.h"

#define PARHELIA_CALL_HEADER		0x20292820u
#define PARHELIA_CALL_DWORDS		5u
#define PARHELIA_FENCE_TIMEOUT_US	50000u
#define PARHELIA_R_DWGCTL		0x1300

static bool trace_submit;
module_param(trace_submit, bool, 0400);
MODULE_PARM_DESC(trace_submit, "Trace coherent retirement of each pre-draw state packet");

/* The vendor queues all primary CALLs, its final writeback, and then rings the
 * producer doorbell once.  The original bring-up path deliberately published
 * one CALL at a time so every boundary could be observed. The coalesced P690
 * path has passed the direct-render, GLX and Quake target gates; trace/full-sync
 * modes retain their exact diagnostic streams regardless. */
static bool coalesce_primary = true;
module_param(coalesce_primary, bool, 0400);
MODULE_PARM_DESC(coalesce_primary,
		 "Queue a normal submit's primary CALLs behind one producer doorbell (default true)");

/* The documented hardware replay uses --rrfence auto, not the unpatched
 * p3d_state counter literals. Keep that progression on the normal path.
 */
static bool replay_fences = true;
module_param(replay_fences, bool, 0400);
MODULE_PARM_DESC(replay_fences, "Use the working tri_ringreplay --rrfence auto counter progression (default true)");

static bool full_sync;
module_param(full_sync, bool, 0400);
MODULE_PARM_DESC(full_sync, "Compare vendor primary-ring 307/004/707 flush with prior writeback arm");

static bool post_draw_wb;
module_param(post_draw_wb, bool, 0400);
MODULE_PARM_DESC(post_draw_wb, "Emit the captured post-draw writeback before EOB and flush");

static bool null_stages;
module_param(null_stages, bool, 0400);
MODULE_PARM_DESC(null_stages, "Initialize auxiliary setup bindings to the recovered unbound sentinel");

/* A deliberately opt-in gate for the standalone mode-7 pixel test.  The
 * normal Mesa frontend never emits mode 7; leaving this false keeps the
 * experimental ABI unavailable to every render client. */
static bool mode7_screen_test;
module_param(mode7_screen_test, bool, 0400);
MODULE_PARM_DESC(mode7_screen_test,
		 "Enable the isolated P690 mode-7 indexed screen test (default false)");

static bool mode3_screen_test;
module_param(mode3_screen_test, bool, 0400);
MODULE_PARM_DESC(mode3_screen_test,
		 "Enable the isolated P690 mode-3 bound-range screen test (default false)");

static bool mode3_bound_range;
module_param(mode3_bound_range, bool, 0400);
MODULE_PARM_DESC(mode3_bound_range,
		 "Enable P690 one-triangle-per-packet mode-3 rendering (default false)");

/* BAR0 is a programmable host-aperture window, not a linear CPU view of
 * local memory.  A raw TTM mmap can therefore alias a native framebuffer;
 * the observed 0x01364000 vertex placement wrote vertex bytes into root rows
 * 403/404.  Keep class-0 vertex fetch unavailable until CREATE_VERTEX owns a
 * dedicated host-aperture mapping.  The coherent DMA upload ABI remains the
 * normal, hardware-accelerated fallback. */
static bool vram_vertex;
module_param(vram_vertex, bool, 0400);
MODULE_PARM_DESC(vram_vertex,
		 "Enable unsafe raw-BAR P690 class-0 vertex buffers (default false)");

/* Permit one ioctl to carry two complete logical state batches.  The kernel
 * retains each batch's opening/EOB/DWGSYNC bracket and coalesces only the final
 * primary producer update. */
static bool stateful_submit;
module_param(stateful_submit, bool, 0400);
MODULE_PARM_DESC(stateful_submit,
		 "Enable two-section stateful async submits (experimental, default false)");

/*
 * The vendor keeps a device-wide hardware-register shadow and emits dirty
 * groups only.  This first kernel experiment is deliberately more restrictive:
 * it drops a CALL only when every write in that CALL is an equal, relocation-
 * free write to the explicit scalar-state allowlist below.  Validation,
 * relocation lookup, pinning and dma-resv fence attachment still happen for
 * the complete userspace request before this filter runs.
 */
static bool state_shadow_cache;
module_param(state_shadow_cache, bool, 0400);
MODULE_PARM_DESC(state_shadow_cache,
		 "Omit proven repeated scalar 3D state CALLs (experimental, default false)");

static bool trace_state_shadow;
module_param(trace_state_shadow, bool, 0400);
MODULE_PARM_DESC(trace_state_shadow,
		 "Trace state-shadow packet/write omission counters");

struct parhelia_shadow_packet {
	bool all_safe;
	bool all_same;
	u32 writes;
};

struct parhelia_shadow_txn {
	struct parhelia_reg_shadow predicted;
	u32 packets;
	u32 packets_omitted;
	u32 writes_omitted;
	bool active;
};

/*
 * Exact initial allowlist.  These are ordinary raster/depth scalar registers
 * emitted by the recovered p3d state builder and cached by the vendor's HWREG
 * dirty map.  Excluded ranges include vertex fetch (00..3f), WARP bindings and
 * latches (40..57), texture and windowed texture state (58..9f except none),
 * combiner/FIFO state (98..a9), surfaces (88..8f), draw control/geometry
 * (c0..c8 and e2/e3), fences/strobes (9d/ac/ae/af), and all 0x41 constants.
 */
static bool parhelia_shadow_safe_reg(u32 index)
{
	switch (index) {
	case 0xb0: /* 0x12c0 colour/depth mask high */
	case 0xb1: /* 0x12c4 colour/depth mask low */
	case 0xb2: /* 0x12c8 depth enable/function */
	case 0xb3: /* 0x12cc depth mask */
	case 0xb4: /* 0x12d0 depth write */
	case 0xb5: /* 0x12d4 depth range minimum */
	case 0xb6: /* 0x12d8 depth range maximum */
	case 0xb7: /* 0x12dc depth range scale */
	case 0xb9: /* 0x12e4 alpha test */
	case 0xba: /* 0x12e8 blend control */
	case 0xbe: /* 0x12f8 AA threshold */
	case 0xbf: /* 0x12fc raster mode */
	case 0xc9: /* 0x1324 colour write mask */
	case 0xca: /* 0x1328 scissor minimum */
	case 0xcb: /* 0x132c scissor maximum */
		return true;
	default:
		return false;
	}
}

/* Pure parser/state transition: @cmd must already have passed validation. */
static struct parhelia_shadow_packet
parhelia_shadow_apply_packet(struct parhelia_reg_shadow *shadow,
			     const u32 *cmd, u32 length, bool has_reloc)
{
	struct parhelia_shadow_packet result = {
		.all_safe = !has_reloc && !(cmd[0] >> 24),
		.all_same = true,
	};
	u32 pos = 1;

	if (cmd[0] >> 24)
		return result;

	while (pos < length) {
		u32 tag = cmd[pos++];
		u32 slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;
			u32 value;

			if (index == 0x21)
				break;
			value = cmd[pos++];
			result.writes++;
			if (!parhelia_shadow_safe_reg(index)) {
				result.all_safe = false;
				continue;
			}
			if (!test_bit(index, shadow->valid) ||
			    shadow->value[index] != value)
				result.all_same = false;
			shadow->value[index] = value;
			set_bit(index, shadow->valid);
		}
	}

	return result;
}

static int parhelia_shadow_static_selftest(void)
{
	struct parhelia_reg_shadow *shadow;
	const u32 depth[] = { 3, 0x21b4b3b2, 7, 0xff, 0x100 };
	const u32 changed[] = { 3, 0x21b4b3b2, 3, 0xff, 0x100 };
	const u32 unsafe[] = { 3, 0x21c0b3b2, 3, 0xff, 0x307 };
	struct parhelia_shadow_packet result;
	int ret = -EINVAL;

	shadow = kzalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return -ENOMEM;
	result = parhelia_shadow_apply_packet(shadow, depth, ARRAY_SIZE(depth), false);
	if (!result.all_safe || result.all_same || result.writes != 3)
		goto out;
	result = parhelia_shadow_apply_packet(shadow, depth, ARRAY_SIZE(depth), false);
	if (!result.all_safe || !result.all_same || result.writes != 3)
		goto out;
	result = parhelia_shadow_apply_packet(shadow, changed, ARRAY_SIZE(changed), false);
	if (!result.all_safe || result.all_same || shadow->value[0xb2] != 3)
		goto out;
	result = parhelia_shadow_apply_packet(shadow, changed, ARRAY_SIZE(changed), true);
	if (result.all_safe || !result.all_same)
		goto out;
	result = parhelia_shadow_apply_packet(shadow, unsafe, ARRAY_SIZE(unsafe), false);
	if (result.all_safe || !result.all_same || shadow->value[0xb2] != 3)
		goto out;
	bitmap_zero(shadow->valid, PARHELIA_INDEXED_REGS);
	result = parhelia_shadow_apply_packet(shadow, changed, ARRAY_SIZE(changed), false);
	if (result.all_same)
		goto out;
	ret = 0;
out:
	kfree(shadow);
	return ret;
}

void parhelia_state_shadow_invalidate(struct parhelia_device *pdev,
				      const char *reason)
{
	bitmap_zero(pdev->reg_shadow.valid, PARHELIA_INDEXED_REGS);
	pdev->reg_shadow_invalidations++;
	if (trace_state_shadow)
		dev_info(&pdev->pdev->dev, "state shadow invalidated: %s\n", reason);
}

static bool parhelia_packet_has_reloc(const struct drm_parhelia_reloc *relocs,
				      u32 count, u32 packet)
{
	u32 i;

	for (i = 0; i < count; i++)
		if (relocs[i].type != PARHELIA_RELOC_BATCH_BOUNDARY &&
		    relocs[i].packet == packet)
			return true;
	return false;
}

/* Called with submit_lock held, after every relocation and private patch. */
static void parhelia_shadow_filter_plan(const struct parhelia_reg_shadow *shadow_base,
		const u32 *commands, const struct drm_parhelia_packet *packets,
		u32 packet_count, const struct drm_parhelia_reloc *relocs,
		u32 reloc_count, struct parhelia_call_plan *plan,
		struct parhelia_shadow_txn *txn)
{
	DECLARE_BITMAP(omit, PARHELIA_MAX_USER_PACKETS);
	u32 i, out = 0;

	bitmap_zero(omit, PARHELIA_MAX_USER_PACKETS);
	txn->predicted = *shadow_base;
	txn->active = true;
	for (i = 0; i < packet_count; i++) {
		const u32 *cmd = commands + packets[i].offset;
		struct parhelia_shadow_packet result;

		result = parhelia_shadow_apply_packet(&txn->predicted, cmd,
			packets[i].length,
			parhelia_packet_has_reloc(relocs, reloc_count, i));
		if (!result.all_safe)
			continue;
		txn->packets++;
		if (!result.all_same)
			continue;
		set_bit(i, omit);
		txn->packets_omitted++;
		txn->writes_omitted += result.writes;
	}

	for (i = 0; i < plan->count; i++) {
		u32 block = plan->blocks[i];

		if (block < packet_count && test_bit(block, omit))
			continue;
		plan->blocks[out++] = block;
	}
	plan->count = out;
}

static int parhelia_shadow_plan_static_selftest(void)
{
	const u32 commands[] = {
		3, 0x21b4b3b2, 7, 0xff, 0x100,
		3, 0x21b4b3b2, 7, 0xff, 0x100,
	};
	const struct drm_parhelia_packet packets[] = {
		{ .offset = 0, .length = 5 },
		{ .offset = 5, .length = 5 },
	};
	const struct drm_parhelia_reloc reloc = { .packet = 0 };
	struct parhelia_shadow_plan_test {
		struct parhelia_reg_shadow initial;
		struct parhelia_shadow_txn first, second, relocated;
		struct parhelia_call_plan plan;
	} *test;
	int ret = -EINVAL;

	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->plan.blocks[0] = 0;
	test->plan.blocks[1] = 1;
	test->plan.count = 2;

	/* Within one submit the first packet establishes state; its repeat drops. */
	parhelia_shadow_filter_plan(&test->initial, commands, packets, 2, NULL, 0,
		&test->plan, &test->first);
	if (test->plan.count != 1 || test->plan.blocks[0] != 0 ||
	    test->first.packets != 2 || test->first.packets_omitted != 1 ||
	    test->first.writes_omitted != 3)
		goto out;

	/* The committed prediction makes both packets redundant next submit. */
	test->plan.blocks[0] = 0;
	test->plan.blocks[1] = 1;
	test->plan.count = 2;
	parhelia_shadow_filter_plan(&test->first.predicted, commands, packets, 2,
		NULL, 0, &test->plan, &test->second);
	if (test->plan.count || test->second.packets_omitted != 2 ||
	    test->second.writes_omitted != 6)
		goto out;

	/* A relocation keeps its owner CALL even when its values are equal. */
	test->plan.blocks[0] = 0;
	test->plan.blocks[1] = 1;
	test->plan.count = 2;
	parhelia_shadow_filter_plan(&test->first.predicted, commands, packets, 2,
		&reloc, 1, &test->plan, &test->relocated);
	if (test->plan.count != 1 || test->plan.blocks[0] != 0 ||
	    test->relocated.packets_omitted != 1)
		goto out;

	ret = 0;
out:
	kfree(test);
	return ret;
}

/* Commit only after the complete primary span is visible to the producer. */
static void parhelia_shadow_commit(struct parhelia_device *pdev,
				   struct parhelia_shadow_txn *txn)
{
	if (!txn || !txn->active)
		return;
	lockdep_assert_held(&pdev->submit_lock);
	pdev->reg_shadow = txn->predicted;
	pdev->reg_shadow_submits++;
	pdev->reg_shadow_packets += txn->packets;
	pdev->reg_shadow_packets_omitted += txn->packets_omitted;
	pdev->reg_shadow_writes_omitted += txn->writes_omitted;
	if (trace_state_shadow)
		dev_info_ratelimited(&pdev->pdev->dev,
			"state shadow: omit %u/%u packets, %u writes; totals %llu/%llu packets, %llu writes\n",
			txn->packets_omitted, txn->packets, txn->writes_omitted,
			(unsigned long long)pdev->reg_shadow_packets_omitted,
			(unsigned long long)pdev->reg_shadow_packets,
			(unsigned long long)pdev->reg_shadow_writes_omitted);
	txn->active = false;
}

static bool parhelia_vram_register(u32 index)
{
	/* Texture base and the four colour/depth surface base slots. */
	return index == 0x77 || (index >= 0x88 && index <= 0x8e && !(index & 1));
}

static int parhelia_validate_reglist(const u32 *cmd, u32 length)
{
	u32 pos = 1;

	while (pos < length) {
		u32 tag = cmd[pos++];
		unsigned int slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;

			if (index == 0x21)
				break;
			/* 0x138c (index e3) is the documented 2D fill extent/
			 * trigger used by p3d_emit_clear; it lies above the WARP
			 * state window. Keep the remaining high indices rejected.
			 */
			if ((index > 0xdf && index != 0xe3) || pos >= length)
				return -EINVAL;
			/* DMA addresses and the global sync counter are kernel-owned.
			 * Render-node clients only provide state values and relocatable
			 * VRAM operands; primary-ring and writeback addresses remain
			 * entirely kernel-owned.
			 */
			if (index == 0x24 || index == 0x25 || index == 0x28 ||
			    index == 0x29 || index == 0xaf)
				return -EACCES;
			/* This operand must be populated by a GEM relocation. */
			if (parhelia_vram_register(index) && cmd[pos])
				return -EACCES;
			pos++;
		}
	}

	return pos == length ? 0 : -EINVAL;
}

static int parhelia_validate_packet(const u32 *cmd,
				    const struct drm_parhelia_packet *packet)
{
	u32 header, words;

	if (packet->length < 2)
		return -EINVAL;
	header = cmd[packet->offset];
	if ((header >> 24) == 0x41)
		return parhelia_validate_constant_group(cmd + packet->offset, packet->length);
	words = (header & 0xffff) + 2;
	if (words != packet->length)
		return -EINVAL;

	switch (header >> 24) {
	case 0x00:
		return parhelia_validate_reglist(&cmd[packet->offset], words);
	case 0x97:
		return words == 3 ? 0 : -EINVAL;
	case 0x8f:
		/* Exact shapes used by the fixed six-vertex depth-interlock
		 * diagnostic.  These operands are ordinals, not addresses. */
		if (words == 5 && header == 0x8f000003u)
			return !cmd[packet->offset + 1] &&
			       cmd[packet->offset + 2] == 3 &&
			       cmd[packet->offset + 3] == 3 &&
			       cmd[packet->offset + 4] == 6 ? 0 : -EINVAL;
		if (words != 3 || header != 0x8f000001u)
			return -EINVAL;
		return ((cmd[packet->offset + 1] <= U32_MAX - 3 &&
			 cmd[packet->offset + 2] == cmd[packet->offset + 1] + 3) ||
			(!cmd[packet->offset + 1] &&
			 cmd[packet->offset + 2] == 6)) ? 0 : -EINVAL;
	case 0x9f:
		/* Mode 7 is structurally a three-dword, zero-address packet.  The
		 * submit path keeps it unavailable until its index source is copied
		 * to immutable kernel-owned backing. */
		return words == 3 && header == 0x9f000001u && !cmd[packet->offset + 1] &&
		       !cmd[packet->offset + 2] ? 0 : -EINVAL;
	default:
		return -EOPNOTSUPP;
	}
}

/* True when @dword is the data operand of a texture/surface base write. */
static bool parhelia_is_vram_operand(const u32 *cmd, u32 length, u32 dword)
{
	u32 pos = 1;

	while (pos < length) {
		u32 tag = cmd[pos++];
		unsigned int slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;

			if (index == 0x21)
				break;
			if (pos == dword)
				return parhelia_vram_register(index);
			pos++;
		}
	}
	return false;
}

/* The recovered Mesa builder leaves the stream in class 3 immediately before
 * all of its 0x97 draws. A local-VRAM vertex draw changes only that final
 * bind: touching an earlier class-0 loader bind would be unrelated to the
 * draw and could leave a later class-3 bind active. */
static int parhelia_select_vram_vertex_stream(u32 *commands,
		const struct drm_parhelia_packet *packets, u32 count)
{
	u32 *last_stream0 = NULL;
	u32 i;

	for (i = 0; i < count; i++) {
		u32 *cmd = commands + packets[i].offset;
		u32 pos = 1;

		if (cmd[0] >> 24)
			continue;
		while (pos < packets[i].length) {
			u32 tag = cmd[pos++];
			unsigned int slot;

			for (slot = 0; slot < 4; slot++) {
				u32 index = (tag >> (slot * 8)) & 0xff;

				if (index == 0x21)
					break;
				if (index == 0)
					last_stream0 = &cmd[pos];
				pos++;
			}
		}
	}
	/* 3 is the known-good PCI DMA stream class.  Requiring that exact state
	 * keeps this private probe tied to the proven 0x97 path. */
	if (!last_stream0 || *last_stream0 != 3)
		return -EINVAL;
	*last_stream0 = 0;
	return 0;
}

/* The public helper only pins plane framebuffers. Render relocations need
 * the same TTM operation, held until the synchronous batch fence retires. */
int parhelia_vram_pin(struct drm_gem_vram_object *bo)
{
	struct ttm_operation_ctx ctx = { false, false };
	int ret;

	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (ret)
		return ret;
	if (!bo->bo.pin_count) {
		bo->placement.placement = bo->placements;
		bo->placements[0].mem_type = TTM_PL_VRAM;
		bo->placements[0].flags = 0;
		bo->placement.num_placement = 1;
		bo->placements[0].fpfn = 0;
		bo->placements[0].lpfn = 0;
		ret = ttm_bo_validate(&bo->bo, &bo->placement, &ctx);
		if (ret)
			goto out;
	}
	ttm_bo_pin(&bo->bo);
out:
	ttm_bo_unreserve(&bo->bo);
	return ret;
}

void parhelia_vram_unpin(struct drm_gem_vram_object *bo)
{
	if (!ttm_bo_reserve(&bo->bo, false, false, NULL)) {
		ttm_bo_unpin(&bo->bo);
		ttm_bo_unreserve(&bo->bo);
	}
}

static const char *parhelia_fence_get_driver_name(struct dma_fence *fence)
{
	return "parhelia";
}

static const char *parhelia_fence_get_timeline_name(struct dma_fence *fence)
{
	return "parhelia-render";
}

/* A slot owns one fence reference while queued.  dma-resv may retain its own
 * reference after the slot is reusable, so each submission fence is dynamic. */
static void parhelia_fence_release(struct dma_fence *fence)
{
	kfree(container_of(fence, struct parhelia_submit_fence, base));
}

static const struct dma_fence_ops parhelia_fence_ops = {
	.get_driver_name = parhelia_fence_get_driver_name,
	.get_timeline_name = parhelia_fence_get_timeline_name,
	.release = parhelia_fence_release,
};

static int parhelia_slot_fence_init(struct parhelia_device *pdev,
				    struct parhelia_submit_slot *slot, u32 fence)
{
	struct parhelia_submit_fence *out_fence;

	out_fence = kzalloc(sizeof(*out_fence), GFP_KERNEL);
	if (!out_fence)
		return -ENOMEM;
	spin_lock_init(&out_fence->lock);
	dma_fence_init(&out_fence->base, &parhelia_fence_ops, &out_fence->lock,
		       pdev->fence_context, fence);
	slot->out_fence = out_fence;
	return 0;
}

/* Every render relocation is conservatively a write dependency until a
 * complete state-usage table can distinguish sampled inputs.  Lock one
 * reservation at a time so this remains usable as an external module: the
 * target kernel does not export drm_exec, while the dma-resv primitive is
 * available through the DRM GEM ABI.  Deduplication also avoids recursively
 * locking one render target used by several packet relocations. */
static int parhelia_slot_attach_fence(struct parhelia_submit_slot *slot,
				      struct drm_gem_object **objects, u32 count)
{
	u32 i;
	int ret = 0;

	for (i = 0; i < count; i++) {
		struct drm_gem_object *obj = objects[i];
		u32 j;

		if (!obj)
			continue;
		for (j = 0; j < i; j++)
			if (objects[j] == obj)
				break;
		if (j != i)
			continue;
		ret = dma_resv_lock_interruptible(obj->resv, NULL);
		if (ret)
			break;
		ret = dma_resv_reserve_fences(obj->resv, 1);
		if (!ret)
			dma_resv_add_fence(obj->resv, &slot->out_fence->base,
					  DMA_RESV_USAGE_WRITE);
		dma_resv_unlock(obj->resv);
		if (ret)
			break;
	}
	return ret;
}

static void parhelia_slot_release_locked(struct parhelia_device *pdev,
					 struct parhelia_submit_slot *slot, bool error)
{
	u32 i;

	if (slot->state != PARHELIA_SLOT_QUEUED)
		return;
	if (slot->out_fence) {
		if (error)
			dma_fence_set_error(&slot->out_fence->base, -EIO);
		dma_fence_signal(&slot->out_fence->base);
		dma_fence_put(&slot->out_fence->base);
		slot->out_fence = NULL;
	}
	for (i = 0; i < slot->reloc_count; i++) {
		if (slot->pinned[i]) {
			parhelia_vram_unpin(slot->pinned[i]);
			slot->pinned[i] = NULL;
		}
		if (slot->objects[i]) {
			drm_gem_object_put(slot->objects[i]);
			slot->objects[i] = NULL;
		}
	}
	if (slot->mode7_index_copy) {
		parhelia_vram_unpin(slot->mode7_index_copy);
		drm_gem_object_put(&slot->mode7_index_copy->bo.base);
		slot->mode7_index_copy = NULL;
	}
	slot->reloc_count = 0;
	slot->file = NULL;
	slot->state = PARHELIA_SLOT_FREE;
	if (pdev->queued_slots)
		pdev->queued_slots--;
	if (!pdev->queued_slots)
		pdev->primary_get = pdev->primary_put;
	wake_up_all(&pdev->submit_waitq);
}

/* submit_lock serializes both the producer and retirement.  DWGSYNC is the
 * only completion source used here: the vendor IRQ status bits remain KMS
 * vblank-only and are not guessed as render completion interrupts. */
static void parhelia_retire_locked(struct parhelia_device *pdev)
{
	u32 done = parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u;
	u32 i;

	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		if (slot->state != PARHELIA_SLOT_QUEUED)
			continue;
		if ((s32)(done - slot->fence) >= 0) {
			dma_rmb();
			parhelia_slot_release_locked(pdev, slot, false);
		}
	}
	/* A fence means the WARP/DWG work completed, but it does not by itself
	 * prove a dead primary fetcher stopped on a fault.  Bound the queued work
	 * exactly as the old ioctl did and quarantine all live allocations on a
	 * timeout rather than returning them to userspace. */
	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		if (slot->state != PARHELIA_SLOT_QUEUED ||
		    !time_after_eq(jiffies, slot->deadline))
			continue;
		dev_err(&pdev->pdev->dev,
			"asynchronous submit fence %08x timed out; wedged\n", slot->fence);
		pdev->wedged = true;
		parhelia_state_shadow_invalidate(pdev, "asynchronous submit timeout");
		pci_clear_master(pdev->pdev);
		for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
			slot = &pdev->slots[i];
			if (slot->state != PARHELIA_SLOT_QUEUED)
				continue;
			if (slot->out_fence) {
				dma_fence_set_error(&slot->out_fence->base, -EIO);
				dma_fence_signal(&slot->out_fence->base);
			}
			slot->state = PARHELIA_SLOT_QUARANTINED;
		}
		wake_up_all(&pdev->submit_waitq);
		break;
	}
}

static void parhelia_retire_work(struct work_struct *work)
{
	struct parhelia_device *pdev = container_of(to_delayed_work(work),
						     struct parhelia_device, retire_work);

	mutex_lock(&pdev->submit_lock);
	parhelia_retire_locked(pdev);
	if (pdev->queued_slots && !pdev->wedged)
		mod_delayed_work(system_wq, &pdev->retire_work, msecs_to_jiffies(1));
	mutex_unlock(&pdev->submit_lock);
}

/* Drain every queued hardware user, then return with submit_lock held.  A
 * caller which is about to expose local memory through a temporary host
 * aperture must keep this lock until the descriptor has been revoked, so a
 * racing submit cannot begin sampling or rendering the surface mid-copy. */
int parhelia_submit_idle_lock(struct parhelia_device *pdev)
{
	int ret;

	for (;;) {
		mutex_lock(&pdev->submit_lock);
		parhelia_retire_locked(pdev);
		if (pdev->wedged) {
			mutex_unlock(&pdev->submit_lock);
			return -EIO;
		}
		if (!pdev->queued_slots)
			return 0;
		mutex_unlock(&pdev->submit_lock);
		ret = wait_event_interruptible_timeout(pdev->submit_waitq,
			READ_ONCE(pdev->wedged) || !READ_ONCE(pdev->queued_slots),
			msecs_to_jiffies(10));
		if (ret < 0)
			return ret;
	}
}

/* An async request owns its compact secondary heap until DWGSYNC retires its
 * slot.  This runs only while the selected slot is free and submit_lock is
 * held, so replacing its heap cannot invalidate an engine-visible address. */
static int parhelia_async_slot_prepare(struct parhelia_device *pdev,
				       struct parhelia_submit_slot *slot, u32 dwords)
{
	if (!dwords || dwords > PARHELIA_MAX_COMMAND_DWORDS +
		    PARHELIA_MAX_PACKETS * 13u + 16u)
		return -EINVAL;
	if (!slot->secondary_cpu || slot->secondary_dwords < dwords) {
		if (slot->secondary_cpu)
			dma_free_coherent(&pdev->pdev->dev,
				slot->secondary_dwords * sizeof(u32), slot->secondary_cpu,
				slot->secondary_bus);
		slot->secondary_cpu = dma_alloc_coherent(&pdev->pdev->dev,
			dwords * sizeof(u32), &slot->secondary_bus, GFP_KERNEL);
		if (!slot->secondary_cpu) {
			dev_warn_ratelimited(&pdev->pdev->dev,
				"async secondary coherent allocation failed: %u dwords (%zu bytes)\\n",
				dwords, (size_t)dwords * sizeof(u32));
			slot->secondary_bus = 0;
			slot->secondary_dwords = 0;
			return -ENOMEM;
		}
		slot->secondary_dwords = dwords;
	}
	memset(slot->blocks, 0, sizeof(slot->blocks));
	slot->secondary_put = 0;
	return 0;
}

static int parhelia_async_block_assign(struct parhelia_submit_slot *slot,
				       struct parhelia_dma_block *block, u32 dwords)
{
	if (!dwords || dwords > PARHELIA_DMA_BLOCK_DWORDS ||
	    dwords > slot->secondary_dwords || block->cpu ||
	    slot->secondary_put > slot->secondary_dwords - dwords)
		return -EINVAL;
	block->cpu = (u32 *)slot->secondary_cpu + slot->secondary_put;
	block->bus = slot->secondary_bus + slot->secondary_put * sizeof(u32);
	block->dwords = dwords;
	slot->secondary_put += dwords;
	return 0;
}

static void parhelia_async_slot_heap_fini(struct parhelia_device *pdev,
					 struct parhelia_submit_slot *slot)
{
	if (slot->secondary_cpu)
		dma_free_coherent(&pdev->pdev->dev,
			slot->secondary_dwords * sizeof(u32), slot->secondary_cpu,
			slot->secondary_bus);
	slot->secondary_cpu = NULL;
	slot->secondary_bus = 0;
	slot->secondary_dwords = 0;
	slot->secondary_put = 0;
}

int parhelia_submit_init(struct parhelia_device *pdev)
{
	u32 i;
	int ret;

	ret = parhelia_shadow_static_selftest();
	if (!ret)
		ret = parhelia_shadow_plan_static_selftest();
	if (ret) {
		dev_err(&pdev->pdev->dev, "state-shadow static selftest failed\n");
		return ret;
	}

	init_waitqueue_head(&pdev->submit_waitq);
	INIT_DELAYED_WORK(&pdev->retire_work, parhelia_retire_work);
	pdev->fence_context = dma_fence_context_alloc(1);
	pdev->async_primary_cpu = dmam_alloc_coherent(&pdev->pdev->dev,
		PARHELIA_ASYNC_PRIMARY_DWORDS * sizeof(u32), &pdev->async_primary_bus,
		GFP_KERNEL);
	if (!pdev->async_primary_cpu)
		return -ENOMEM;
	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		slot->vertex_cpu = dmam_alloc_coherent(&pdev->pdev->dev,
			PARHELIA_VERTEX_DWORDS * sizeof(u32), &slot->vertex_bus, GFP_KERNEL);
		slot->writeback_cpu = dmam_alloc_coherent(&pdev->pdev->dev, PAGE_SIZE,
			&slot->writeback_bus, GFP_KERNEL);
		slot->objects = devm_kcalloc(&pdev->pdev->dev, PARHELIA_MAX_RELOCS,
			sizeof(*slot->objects), GFP_KERNEL);
		slot->pinned = devm_kcalloc(&pdev->pdev->dev, PARHELIA_MAX_RELOCS,
			sizeof(*slot->pinned), GFP_KERNEL);
		if (!slot->vertex_cpu || !slot->writeback_cpu || !slot->objects ||
		    !slot->pinned)
			return -ENOMEM;
		slot->state = PARHELIA_SLOT_FREE;
	}
	return 0;
}

static struct parhelia_vertex_bo *
parhelia_vertex_bo_lookup(struct parhelia_device *pdev,
			  struct drm_file *file, struct drm_gem_object *obj)
{
	struct parhelia_vertex_bo *entry;

	list_for_each_entry(entry, &pdev->vertex_bos, link) {
		if (entry->file == file && &entry->bo->bo.base == obj) {
			return entry;
		}
	}
	return NULL;
}

/*
 * Mode-7 direct fetch needs more than a range check: its index values select
 * byte addresses in the bound vertex stream.  This records the one-stream,
 * class-0 contract recovered for the P690.  The index slice is only a source
 * snapshot: before a mode-7 packet reaches the engine, submit copies it into a
 * driver-private, pinned local-VRAM object which has no DRM handle or mmap
 * permission.  A TTM reservation does not stop writes through CREATE_VERTEX's
 * existing mmap, so validation is always performed on that private snapshot.
 */
enum parhelia_mode7_transport {
	PARHELIA_MODE7_TRANSPORT_NONE,
	PARHELIA_MODE7_TRANSPORT_VRAM,
	PARHELIA_MODE7_TRANSPORT_DMA,
};

struct parhelia_mode7_submit {
	bool stream_seen;
	bool index_seen;
	enum parhelia_mode7_transport transport;
	u32 stream_packet;
	u32 stream_dword;
	u32 stream_offset;
	u32 stream_size;
	struct parhelia_vertex_bo *stream_bo;
	u32 index_packet;
	u32 index_offset;
	u32 index_length;
	struct parhelia_vertex_bo *index_bo;
	/* Driver-private, unmapped local-VRAM snapshot of the index slice. */
	struct drm_gem_vram_object *index_copy;
	u32 index_copy_offset;
};

struct parhelia_mode3_submit {
	bool stream_seen;
	u32 stream_packet;
	u32 stream_dword;
};

struct parhelia_bound_range_submit {
	bool stream_seen;
	u32 stream_packet;
	u32 stream_dword;
	u32 stream_offset;
	struct parhelia_vertex_bo *stream_bo;
};

struct parhelia_mode7_state {
	u32 stream[8];
	u16 stride[8];
	u32 floor;
	u32 limit;
	u32 prim;
	u32 descriptor_extent;
	unsigned int descriptors;
	bool floor_seen;
	bool limit_seen;
	bool prim_seen;
	bool descriptor_index_seen;
	bool descriptors_done;
	bool invalid;
};

/* Return the indexed-file register written by one exact data operand. */
static int parhelia_reglist_operand_index(const u32 *cmd, u32 length,
					  u32 dword, u32 *index_out)
{
	u32 pos = 1;

	while (pos < length) {
		u32 tag = cmd[pos++];
		unsigned int slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;

			if (index == 0x21)
				break;
			if (pos >= length)
				return -EINVAL;
			if (pos == dword) {
				*index_out = index;
				return 0;
			}
			pos++;
		}
	}
	return -EINVAL;
}

/* The relocated operand must be the final STREAM0 write before mode 7.  A
 * later user-zero write would otherwise supersede the checked class-0 base. */
static int parhelia_mode7_final_stream_operand(
		const u32 *commands, const struct drm_parhelia_packet *packets,
		const struct parhelia_mode7_submit *mode7)
{
	u32 i;
	bool found = false;

	for (i = mode7->stream_packet; i < mode7->index_packet; i++) {
		const u32 *cmd = commands + packets[i].offset;
		u32 pos = 1;

		if (cmd[0] >> 24)
			continue;
		while (pos < packets[i].length) {
			u32 tag = cmd[pos++];
			unsigned int slot;

			for (slot = 0; slot < 4; slot++) {
				u32 index = (tag >> (slot * 8)) & 0xff;

				if (index == 0x21)
					break;
				if (index == 0) {
					if (i != mode7->stream_packet || pos != mode7->stream_dword)
						return -EINVAL;
					found = true;
				}
				pos++;
			}
		}
	}
	return found ? 0 : -EINVAL;
}

/* Decode just the P690 direct-fetch indexed-file state.  A descriptor's
 * component field is a bitmask, so source validation must use its actual
 * number of dwords.  Treating every descriptor as a vec4 incorrectly rejects
 * the normal five-dword screen layout: its RGB descriptor starts at dword two
 * and reads three dwords, ending exactly at byte 20 of a 20-byte vertex.
 */
static void parhelia_mode7_state_packet(struct parhelia_mode7_state *state,
					const u32 *cmd, u32 length)
{
	u32 pos = 1;

	while (pos < length) {
		u32 tag = cmd[pos++];
		unsigned int slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;
			u32 value;

			if (index == 0x21)
				break;
			if (pos >= length) {
				state->invalid = true;
				return;
			}
			value = cmd[pos++];
			if (index < ARRAY_SIZE(state->stream)) {
				state->stream[index] = value;
			} else if (index >= 0x10 && index <= 0x13) {
				unsigned int stream = (index - 0x10) * 2;

				state->stride[stream] = value & 0xffff;
				state->stride[stream + 1] = value >> 16;
			} else if (index == 0x18) {
				state->limit = value;
				state->limit_seen = true;
			} else if (index == 0x19) {
				state->floor = value;
				state->floor_seen = true;
			} else if (index == 0x2c) {
				state->prim = value;
				state->prim_seen = true;
			} else if (index == 0x2d) {
				if (value)
					state->invalid = true;
				state->descriptor_index_seen = true;
				state->descriptors_done = false;
				state->descriptors = 0;
				state->descriptor_extent = 0;
			} else if (index == 0x2e) {
				u32 extent, components, mask;

				if (!state->descriptor_index_seen || state->descriptors_done) {
					state->invalid = true;
					continue;
				}
				if (!value) {
					state->descriptors_done = true;
					continue;
				}
				/* First prototype: every descriptor must select stream 0. */
				if (value & 7) {
					state->invalid = true;
					continue;
				}
				mask = (value >> 22) & 0xf;
				/* Captured direct-fetch descriptors use contiguous XY, XYZ or
				 * XYZW masks only.  Keeping the first ABI to those forms makes
				 * the source-range proof match the decoded hardware layout. */
				if (mask != 0x3 && mask != 0x7 && mask != 0xf) {
					state->invalid = true;
					continue;
				}
				components = hweight8(mask);
				if (check_add_overflow(((value >> 4) & 0xff) * 4,
						       components * 4, &extent)) {
					state->invalid = true;
					continue;
				}
				state->descriptor_extent = max(state->descriptor_extent, extent);
				state->descriptors++;
			}
		}
	}
}

static int parhelia_mode7_validate_indices(const struct iosys_map *map,
					    u32 offset, u32 length,
					    const struct parhelia_mode7_state *state)
{
	u32 pos;

	for (pos = 0; pos < length; pos += sizeof(u32)) {
		u32 ordinal = iosys_map_rd(map, offset + pos, u32);

		if (ordinal < state->floor || ordinal >= state->limit)
			return -EINVAL;
	}
	return 0;
}

/* The DMA diagnostic uses the submit's already copied coherent vertex blob.
 * It is private to the kernel for the full synchronous request, so unlike a
 * CREATE_VERTEX mmap it needs no second VRAM snapshot to close a TOCTOU gap. */
static int parhelia_mode7_validate_dma_indices(const u32 *vertices,
						u32 offset, u32 length,
						const struct parhelia_mode7_state *state)
{
	u32 pos;

	for (pos = 0; pos < length; pos += sizeof(u32)) {
		u32 ordinal = vertices[(offset + pos) / sizeof(*vertices)];

		if (ordinal < state->floor || ordinal >= state->limit)
			return -EINVAL;
	}
	return 0;
}

/* Copy the user-owned source once, then validate and execute only the private
 * local-VRAM copy.  The source reservation prevents eviction while it is
 * mapped; it intentionally cannot serialize a writable CREATE_VERTEX mmap.
 * That race only changes this request's snapshot and cannot alter the checked
 * destination once the copy returns.
 */
static int parhelia_mode7_copy_indices(struct parhelia_device *pdev,
				       struct parhelia_mode7_submit *mode7,
				       const struct parhelia_mode7_state *state)
{
	struct drm_gem_vram_object *src;
	struct drm_gem_vram_object *dst;
	struct iosys_map src_map = { }, dst_map = { };
	void *snapshot;
	u32 address;
	int ret;

	if (mode7->transport == PARHELIA_MODE7_TRANSPORT_DMA)
		return parhelia_mode7_validate_dma_indices(pdev->vertex_cpu,
							   mode7->index_offset,
							   mode7->index_length, state);
	src = mode7->index_bo->bo;

	/* The relocation check already bounds this to CREATE_VERTEX's 1 MiB ABI. */
	snapshot = kvmalloc(mode7->index_length, GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	ret = ttm_bo_reserve(&src->bo, true, false, NULL);
	if (ret)
		goto out_snapshot;
	ret = drm_gem_vram_vmap(src, &src_map);
	if (!ret)
		iosys_map_memcpy_from(snapshot, &src_map, mode7->index_offset,
				      mode7->index_length);
	if (!ret)
		drm_gem_vram_vunmap(src, &src_map);
	ttm_bo_unreserve(&src->bo);
	if (ret)
		goto out_snapshot;

	/* No handle is created for @dst.  Consequently neither a userspace mmap nor
	 * a userspace-visible GPU offset exists for this buffer. */
	dst = drm_gem_vram_create(&pdev->drm, PAGE_ALIGN(mode7->index_length), 1);
	if (IS_ERR(dst)) {
		ret = PTR_ERR(dst);
		goto out_snapshot;
	}
	ret = parhelia_vram_pin(dst);
	if (ret)
		goto out_dst;

	ret = ttm_bo_reserve(&dst->bo, true, false, NULL);
	if (ret)
		goto out_unpin;
	ret = drm_gem_vram_vmap(dst, &dst_map);
	if (ret)
		goto out_unreserve;
	iosys_map_memcpy_to(&dst_map, 0, snapshot, mode7->index_length);
	/* Validate the exact bytes written to the object that the engine will use. */
	ret = parhelia_mode7_validate_indices(&dst_map, 0, mode7->index_length,
						 state);
	drm_gem_vram_vunmap(dst, &dst_map);
out_unreserve:
	ttm_bo_unreserve(&dst->bo);
	if (ret)
		goto out_unpin;

	ret = parhelia_vram_gpu_addr(pdev, dst, 0, mode7->index_length,
				     &address);
	if (ret || (address & 3)) {
		if (!ret)
			ret = -EINVAL;
		goto out_unpin;
	}
	mode7->index_copy = dst;
	mode7->index_copy_offset = address;
	kvfree(snapshot);
	return 0;

out_unpin:
	parhelia_vram_unpin(dst);
out_dst:
	drm_gem_object_put(&dst->bo.base);
out_snapshot:
	kvfree(snapshot);
	return ret;
}

static int parhelia_validate_mode7_contract(const u32 *commands,
					  const struct drm_parhelia_packet *packets,
					  u32 packet_count,
					  const struct parhelia_mode7_submit *mode7,
					  struct parhelia_mode7_state *state)
{
	const struct drm_gem_vram_object *stream_bo;
	u64 stream_bytes;
	u32 operand_index, i;
	int ret;

	if (!mode7->stream_seen || !mode7->index_seen ||
	    mode7->stream_packet >= mode7->index_packet ||
	    mode7->index_packet >= packet_count)
		return -EINVAL;
	ret = parhelia_reglist_operand_index(commands + packets[mode7->stream_packet].offset,
					     packets[mode7->stream_packet].length,
					     mode7->stream_dword, &operand_index);
	if (ret || operand_index != 0)
		return -EINVAL;
	ret = parhelia_mode7_final_stream_operand(commands, packets, mode7);
	if (ret)
		return ret;
	for (i = 0; i < mode7->index_packet; i++) {
		const u32 *cmd = commands + packets[i].offset;

		if (!(cmd[0] >> 24))
			parhelia_mode7_state_packet(state, cmd, packets[i].length);
	}
	if (state->invalid || !state->floor_seen || !state->limit_seen ||
	    !state->prim_seen || state->prim != 0x0002305c ||
	    !state->descriptor_index_seen || !state->descriptors_done ||
	    !state->descriptors || state->floor || state->limit <= state->floor ||
	    !state->stride[0] || (state->stride[0] & 3) ||
	    state->descriptor_extent > state->stride[0] ||
	    state->stream[0] || (mode7->transport != PARHELIA_MODE7_TRANSPORT_DMA &&
				  !mode7->stream_bo))
		return -EINVAL;
	for (i = 1; i < ARRAY_SIZE(state->stream); i++)
		if (state->stream[i])
			return -EINVAL;
	if (mode7->transport == PARHELIA_MODE7_TRANSPORT_DMA) {
		if (!mode7->stream_size || mode7->stream_offset >= mode7->stream_size)
			return -EINVAL;
	} else {
		stream_bo = mode7->stream_bo->bo;
		if (mode7->stream_offset >= stream_bo->bo.base.size)
			return -EINVAL;
	}
	stream_bytes = (u64)(state->limit - state->floor) * state->stride[0] +
		state->descriptor_extent;
	if (stream_bytes > (mode7->transport == PARHELIA_MODE7_TRANSPORT_DMA ?
			mode7->stream_size :
			stream_bo->bo.base.size) - mode7->stream_offset)
		return -EINVAL;
	return 0;
}

/* The only engine-facing mode-7 addresses are derived after ownership and
 * bounds checks.  Class 0 uses the translated local-memory address; no client
 * supplied address bits survive either patch. */
static int parhelia_mode7_patch_addresses(
		u32 *commands, const struct drm_parhelia_packet *packets,
		const struct parhelia_mode7_submit *mode7,
		struct parhelia_device *pdev)
{
	const struct drm_gem_vram_object *stream_bo;
	u32 stream_gpu, stream_end, index_end, stream_size;
	u32 *index_cmd;

	if (mode7->transport == PARHELIA_MODE7_TRANSPORT_DMA) {
		if (!mode7->stream_size || mode7->stream_offset >= mode7->stream_size ||
		    check_add_overflow(lower_32_bits(pdev->vertex_bus), mode7->stream_offset,
				       &stream_gpu) ||
		    check_add_overflow(lower_32_bits(pdev->vertex_bus), mode7->index_offset,
				       &index_end) ||
		    check_add_overflow(index_end, mode7->index_length, &index_end))
			return -EINVAL;
		commands[packets[mode7->stream_packet].offset + mode7->stream_dword] =
			stream_gpu | 3u;
		index_cmd = commands + packets[mode7->index_packet].offset;
		index_cmd[1] = lower_32_bits(pdev->vertex_bus) + mode7->index_offset;
		index_cmd[2] = index_end | 3u;
		return 0;
	}
	stream_bo = mode7->stream_bo->bo;
	stream_size = stream_bo->bo.base.size;
	if (!mode7->index_copy || !stream_size || mode7->stream_offset >= stream_size)
		return -EINVAL;
	if (parhelia_vram_gpu_addr(pdev,
				    (struct drm_gem_vram_object *)stream_bo,
				    mode7->stream_offset,
				    stream_size - mode7->stream_offset,
				    &stream_gpu) ||
	    (stream_gpu & 3) ||
	    check_add_overflow(stream_gpu, stream_size - mode7->stream_offset, &stream_end) ||
	    stream_end > PARHELIA_HOST_APERTURE_BASE ||
	    check_add_overflow(mode7->index_copy_offset, mode7->index_length, &index_end) ||
	    index_end > PARHELIA_HOST_APERTURE_BASE)
		return -EINVAL;

	commands[packets[mode7->stream_packet].offset + mode7->stream_dword] = stream_gpu;
	index_cmd = commands + packets[mode7->index_packet].offset;
	index_cmd[1] = mode7->index_copy_offset;
	index_cmd[2] = index_end;
	return 0;
}

static int parhelia_mode7_validate_packets(const u32 *commands,
					   const struct drm_parhelia_packet *packets,
					   u32 packet_count,
					   const struct parhelia_mode7_submit *mode7)
{
	u32 i, mode7_packets = 0;

	for (i = 0; i < packet_count; i++) {
		u32 opcode = commands[packets[i].offset] >> 24;

		if (opcode == 0x9f) {
			if (i != mode7->index_packet)
				return -EINVAL;
			mode7_packets++;
		} else if (opcode == 0x97) {
			/* A direct-fetch batch has one indexed draw.  Preserve 0x97
			 * support for ordinary submits, but never mix its class-0 stream
			 * conversion with mode-7 state. */
			return -EINVAL;
		}
	}
	return mode7_packets == 1 ? 0 : -EINVAL;
}

/* This is intentionally an exact screen-test recognizer rather than a
 * general mode-3 ABI.  The saved P690 vendor trace proves 0x8f [0,4), while
 * the six-vertex form below is the smallest triangle-list comparison against
 * the existing pair of 0x97 packets. */
static int parhelia_mode3_validate_and_patch(
		u32 *commands, const struct drm_parhelia_packet *packets,
		u32 packet_count, u32 vertex_dwords,
		const struct parhelia_mode3_submit *mode3,
		struct parhelia_device *pdev)
{
	struct parhelia_mode7_state state = { };
	const u32 *stream, *prim, *bounds, *reset, *desc;
	u32 i, first_range, ranges = 0, stride, pos_desc, col_desc;
	bool depth;

	if (!mode3->stream_seen || packet_count < 6 ||
	    (vertex_dwords != 30 && vertex_dwords != 36) ||
	    mode3->stream_packet > packet_count - 6 || mode3->stream_dword != 2)
		return -EINVAL;
	depth = vertex_dwords == 36;
	stride = depth ? 24 : 20;
	pos_desc = depth ? 0x21c00000 : 0x20c00000;
	col_desc = depth ? 0xa1c50030 : 0xa1c50020;
	first_range = mode3->stream_packet + 5;
	stream = commands + packets[mode3->stream_packet].offset;
	prim = commands + packets[mode3->stream_packet + 1].offset;
	bounds = commands + packets[mode3->stream_packet + 2].offset;
	reset = commands + packets[mode3->stream_packet + 3].offset;
	desc = commands + packets[mode3->stream_packet + 4].offset;
	if (packets[mode3->stream_packet].length != 16 || stream[0] != 0xe ||
	    stream[1] != 0x03020100 || stream[2] || stream[6] != 0x07060504 ||
	    stream[11] != 0x13121110 || stream[12] != stride || stream[13] ||
	    stream[14] || stream[15] ||
	    packets[mode3->stream_packet + 1].length != 3 || prim[0] != 1 ||
	    prim[1] != 0x2121212c || prim[2] != 0x0002305c ||
	    packets[mode3->stream_packet + 2].length != 4 || bounds[0] != 2 ||
	    bounds[1] != 0x21211819 || bounds[2] || bounds[3] != 6 ||
	    packets[mode3->stream_packet + 3].length != 3 || reset[0] != 1 ||
	    reset[1] != 0x2121212d || reset[2] ||
	    packets[mode3->stream_packet + 4].length != 5 || desc[0] != 3 ||
	    desc[1] != 0x212e2e2e || desc[2] != pos_desc ||
	    desc[3] != col_desc || desc[4])
		return -EINVAL;
	for (i = 0; i < first_range; i++) {
		const u32 *cmd = commands + packets[i].offset;

		if (!(cmd[0] >> 24))
			parhelia_mode7_state_packet(&state, cmd, packets[i].length);
	}
	if (state.invalid || !state.floor_seen || !state.limit_seen ||
	    !state.prim_seen || state.prim != 0x0002305c ||
	    !state.descriptor_index_seen || !state.descriptors_done ||
	    state.floor || state.limit != 6 || state.stride[0] != stride ||
	    state.descriptors != 2 || state.descriptor_extent != stride || state.stream[0])
		return -EINVAL;
	for (i = 1; i < ARRAY_SIZE(state.stream); i++)
		if (state.stream[i] || state.stride[i])
			return -EINVAL;
	for (i = 0; i < packet_count; i++) {
		u32 opcode = commands[packets[i].offset] >> 24;

		if (opcode == 0x8f) {
			const u32 *range = commands + packets[i].offset;

			if (i != first_range + ranges || ranges >= 2)
				return -EINVAL;
			if (!ranges) {
				if (!((packets[i].length == 3 && range[0] == 0x8f000001 &&
				       !range[1] && (range[2] == 3 || range[2] == 6)) ||
				      (packets[i].length == 5 && range[0] == 0x8f000003 &&
				       !range[1] && range[2] == 3 && range[3] == 3 &&
				       range[4] == 6)))
					return -EINVAL;
			} else if (packets[i].length != 3 ||
				   range[0] != 0x8f000001 || range[1] != 3 || range[2] != 6) {
				return -EINVAL;
			}
			ranges++;
		} else if (opcode == 0x97 || opcode == 0x9f) {
			return -EINVAL;
		} else if (i >= first_range) {
			return -EINVAL;
		}
	}
	if (!ranges || upper_32_bits(pdev->vertex_bus))
		return -EINVAL;
	if (!depth) {
		const u32 *range = commands + packets[first_range].offset;

		if (ranges != 1 || packets[first_range].length != 3 ||
		    range[1] || range[2] != 6)
			return -EINVAL;
	} else if (ranges == 1) {
		const u32 *range = commands + packets[first_range].offset;

		if (!((packets[first_range].length == 3 &&
		       range[0] == 0x8f000001 && !range[1] && range[2] == 6) ||
		      (packets[first_range].length == 5 &&
		       range[0] == 0x8f000003 && !range[1] && range[2] == 3 &&
		       range[3] == 3 && range[4] == 6)))
			return -EINVAL;
	} else if (ranges == 2) {
		const u32 *range = commands + packets[first_range].offset;

		if (packets[first_range].length != 3 ||
		    range[0] != 0x8f000001 || range[1] || range[2] != 3)
			return -EINVAL;
	} else {
		return -EINVAL;
	}
	commands[packets[mode3->stream_packet].offset + mode3->stream_dword] =
		lower_32_bits(pdev->vertex_bus) | 3u;
	return 0;
}

/* Production P690 mode 3 uses one packet for each triangle.  Hardware Z24
 * comparison proved that adding a second range to one packet does not create
 * an interlock, while a second 0x8f packet does.  Keep this recognizer tied to
 * one complete direct-fetch tail and a creator-owned, persistently pinned
 * CREATE_VERTEX range. */
static int parhelia_bound_range_validate_and_patch(
		u32 *commands, const struct drm_parhelia_packet *packets,
		u32 packet_count, const struct parhelia_bound_range_submit *bound,
		struct parhelia_device *pdev)
{
	struct parhelia_mode7_state state = { };
	struct drm_gem_vram_object *bo;
	const u32 *stream, *prim, *bounds, *reset;
	u32 first_range, range_count, bytes, stream_gpu, stream_end, i;

	if (!bound->stream_seen || bound->stream_dword != 2 || packet_count < 6 ||
	    bound->stream_packet > packet_count - 6)
		return -EINVAL;
	first_range = bound->stream_packet + 5;
	range_count = packet_count - first_range;
	if (!range_count || range_count > 180)
		return -EINVAL;

	stream = commands + packets[bound->stream_packet].offset;
	prim = commands + packets[bound->stream_packet + 1].offset;
	bounds = commands + packets[bound->stream_packet + 2].offset;
	reset = commands + packets[bound->stream_packet + 3].offset;
	if (packets[bound->stream_packet].length != 16 || stream[0] != 0xe ||
	    stream[1] != 0x03020100 || stream[2] || stream[6] != 0x07060504 ||
	    stream[11] != 0x13121110 || !stream[12] || (stream[12] & 3) ||
	    stream[12] > 40 || stream[13] || stream[14] || stream[15] ||
	    packets[bound->stream_packet + 1].length != 3 || prim[0] != 1 ||
	    prim[1] != 0x2121212c || prim[2] != 0x0002305c ||
	    packets[bound->stream_packet + 2].length != 4 || bounds[0] != 2 ||
	    bounds[1] != 0x21211819 || bounds[2] ||
	    bounds[3] != range_count * 3 ||
	    packets[bound->stream_packet + 3].length != 3 || reset[0] != 1 ||
	    reset[1] != 0x2121212d || reset[2])
		return -EINVAL;

	for (i = bound->stream_packet; i < first_range; i++) {
		const u32 *cmd = commands + packets[i].offset;

		if (cmd[0] >> 24)
			return -EINVAL;
		parhelia_mode7_state_packet(&state, cmd, packets[i].length);
	}
	if (state.invalid || !state.floor_seen || !state.limit_seen ||
	    !state.prim_seen || state.prim != 0x0002305c ||
	    !state.descriptor_index_seen || !state.descriptors_done ||
	    state.floor || state.limit != range_count * 3 ||
	    !state.stride[0] || state.stride[0] != stream[12] ||
	    !state.descriptors || state.descriptor_extent != state.stride[0] ||
	    state.stream[0])
		return -EINVAL;
	for (i = 1; i < ARRAY_SIZE(state.stream); i++)
		if (state.stream[i] || state.stride[i])
			return -EINVAL;

	for (i = 0; i < packet_count; i++) {
		u32 opcode = commands[packets[i].offset] >> 24;

		if (opcode == 0x8f) {
			const u32 *range = commands + packets[i].offset;
			u32 ordinal;

			if (i < first_range || i - first_range >= range_count)
				return -EINVAL;
			ordinal = (i - first_range) * 3;
			if (packets[i].length != 3 || range[0] != 0x8f000001 ||
			    range[1] != ordinal || range[2] != ordinal + 3)
				return -EINVAL;
		} else if (opcode == 0x97 || opcode == 0x9f) {
			return -EINVAL;
		} else if (i >= first_range) {
			return -EINVAL;
		}
	}

	bo = bound->stream_bo->bo;
	if (check_mul_overflow(range_count * 3, state.stride[0], &bytes) ||
	    bound->stream_offset > bo->bo.base.size ||
	    bytes > bo->bo.base.size - bound->stream_offset)
		return -EINVAL;
	if (parhelia_vram_gpu_addr(pdev, bo, bound->stream_offset, bytes,
				    &stream_gpu) ||
	    (stream_gpu & 3) ||
	    check_add_overflow(stream_gpu, bytes, &stream_end) ||
	    stream_end > PARHELIA_HOST_APERTURE_BASE)
		return -EINVAL;
	commands[packets[bound->stream_packet].offset + bound->stream_dword] =
		stream_gpu;
	return 0;
}

static void parhelia_emit_fences(struct parhelia_dma_block *blocks, u32 slot,
				 u32 begin, u32 finish, u32 dwgctl)
{
	u32 *start = blocks[slot].cpu;
	u32 *eob = blocks[slot + 1].cpu;
	u32 *drain = blocks[slot + 2].cpu;

	/* The recovered cadence is an opening fence, user packets, then EOB and
	 * a DWGSYNC bracketed by the following counter. */
	parhelia_reference_fences(start, eob, drain, begin, finish, dwgctl,
				  replay_fences);
	if (full_sync) {
		/* mtx3144 .text+31c67..31ceb: these nine bare words are
		 * copied into the PRIMARY below, not called as a secondary.
		 * The two counters are consecutive and BOTH armed.
		 */
		drain[0] = 8;
		drain[1] = 0x21c0c0c0;
		drain[2] = 0x307;
		drain[3] = 0x004;
		drain[4] = 0x707;
		drain[5] = 0xc0afafdd;
		drain[6] = 0;
		drain[7] = (finish - 8) | 1;
		drain[8] = finish | 1;
		drain[9] = dwgctl;
		return;
	}
}

/* Preserve the last DWGCTL in this batch, not the state before it executes. */
static u32 parhelia_batch_dwgctl_range(const u32 *commands,
		const struct drm_parhelia_packet *packets, u32 begin, u32 end,
		u32 value)
{
	u32 i;

	for (i = begin; i < end; i++) {
		const u32 *cmd = commands + packets[i].offset;
		u32 pos = 1;

		if (cmd[0] >> 24)
			continue;
		while (pos < packets[i].length) {
			u32 tag = cmd[pos++], slot;

			for (slot = 0; slot < 4; slot++) {
				u32 index = (tag >> (slot * 8)) & 0xff;

				if (index == 0x21)
					break;
				if (index == 0xc0)
					value = cmd[pos];
				pos++;
			}
		}
	}
	return value;
}

static int parhelia_wait_dma_idle(struct parhelia_device *pdev)
{
	u32 waited;

	for (waited = 0; waited < PARHELIA_FENCE_TIMEOUT_US; waited++) {
		if (parhelia_read(pdev, 0x08f0) & BIT(17))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static u32 parhelia_opening_fence_slot_range(const u32 *commands,
		const struct drm_parhelia_packet *packets, u32 begin, u32 end)
{
	u32 i;

	/* p3d_emit_state places its opening fence immediately before this
	 * standalone watermark packet, after surface and write-mask setup.
	 * Restore that ordering without accepting user-owned fence counters.
	 * A pure 2D or transport batch has no such marker and fences at slot 0.
	 */
	for (i = begin; i < end; i++) {
		const u32 *cmd = commands + packets[i].offset;

		if (parhelia_is_fence_boundary(cmd, packets[i].length))
			return i;
	}
	return begin;
}

static int parhelia_wait_fence(struct parhelia_device *pdev, u32 fence)
{
	unsigned int waited;

	for (waited = 0; waited < PARHELIA_FENCE_TIMEOUT_US; waited++) {
		if ((s32)((parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u) -
			  (fence & ~7u)) >= 0)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

int parhelia_create_render_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_parhelia_create_render *args = data;
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_gem_vram_object *bo;
	unsigned long align_pages;
	u8 layout;
	u32 allocation_height, pitch_pixels, pitch;
	size_t bo_size;
	u32 handle;
	int ret;

	if (!args->width || args->width > 2048 || !args->height || args->height > 2048 ||
	    args->flags & ~(PARHELIA_CREATE_RENDER_LINEAR | PARHELIA_CREATE_RENDER_TEXTURE) ||
	    ((args->flags & PARHELIA_CREATE_RENDER_LINEAR) &&
	     (args->flags & PARHELIA_CREATE_RENDER_TEXTURE)) || args->pad ||
	    args->handle || args->pitch || args->size || args->mmap_offset)
		return -EINVAL;
	if (!to_parhelia(drm)->raw_aperture_owned)
		return -EOPNOTSUPP;
	/* SUN90's two local-memory banks are discontinuous in engine address
	 * space.  A 640x480 type-2 surface placed at 63 MiB was reproducibly split
	 * at the physical half-VRAM boundary.  Align each P690 surface base to
	 * 32 MiB so ordinary render targets cannot straddle that boundary while
	 * charging TTM only for the tiled extent actually addressed.  Earlier
	 * silicon retains its hardware-proven 8 KiB surface alignment.
	 */
	align_pages = pdev->pdev->device == 0x2539 &&
		      !(args->flags & (PARHELIA_CREATE_RENDER_LINEAR |
				       PARHELIA_CREATE_RENDER_TEXTURE)) ?
			      (32u << 20) / PAGE_SIZE :
			      max_t(unsigned long, 1, 8192 / PAGE_SIZE);
	/* Tiled surfaces occupy complete hardware tile rows.  Rounding only the
	 * final byte count underallocates a partial row whenever pitch spans more
	 * than one tile (300x300 on P690 was one 4 KiB tile short). */
	if (args->flags & PARHELIA_CREATE_RENDER_LINEAR)
		allocation_height = args->height;
	else if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE)
		allocation_height = ALIGN(args->height, 16);
	else
		allocation_height = ALIGN(args->height,
			pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ? 16 : 32);
	/* Layout 2 addresses 32x16 texture blocks with a power-of-two row
	 * pitch.  Returning the render-target ALIGN(width, 64) pitch here
	 * underallocated NPOT images such as 300x300 before the transient upload
	 * path made that mismatch observable without a raw BAR mapping. */
	if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE)
		pitch_pixels = roundup_pow_of_two(max_t(u32, args->width, 32));
	else
		pitch_pixels = ALIGN(args->width, 64);
	pitch = pitch_pixels * 4;
	bo_size = ALIGN((size_t)pitch * allocation_height, 8192);
	bo = drm_gem_vram_create(drm, bo_size, align_pages);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	if (args->flags & PARHELIA_CREATE_RENDER_LINEAR)
		layout = PARHELIA_BO_LAYOUT_LINEAR;
	else if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE)
		layout = PARHELIA_BO_LAYOUT_TEXTURE;
	else if (pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K)
		layout = PARHELIA_BO_LAYOUT_NATIVE_64X16;
	else
		layout = PARHELIA_BO_LAYOUT_NATIVE_64X32;
	parhelia_bo_set_layout(bo, layout, args->width, args->height, pitch);
	if (layout == PARHELIA_BO_LAYOUT_TEXTURE)
		parhelia_bo_make_unmappable(bo);
	ret = drm_gem_handle_create(file, &bo->bo.base, &handle);
	if (!ret && (layout != PARHELIA_BO_LAYOUT_TEXTURE ||
		     !parhelia_texture_upload_enabled()))
		args->mmap_offset = drm_vma_node_offset_addr(&bo->bo.base.vma_node);
	drm_gem_object_put(&bo->bo.base);
	if (ret)
		return ret;
	args->handle = handle;
	args->pitch = pitch;
	args->size = bo_size;
	return 0;
}

/* This allocation is intentionally narrow: it is a P690 class-0 source, not
 * a generic GEM pin ioctl.  Holding the pin for the creating file keeps the
 * local offset stable across the map/write/submit sequence and avoids ever
 * accepting an arbitrary linear render or imported BO as a vertex address. */
int parhelia_create_vertex_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_parhelia_create_vertex *args = data;
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_gem_vram_object *bo;
	struct parhelia_vertex_bo *entry;
	size_t size;
	u32 handle;
	int ret;

	if (pdev->pdev->device != PARHELIA_P690 || !pdev->raw_aperture_owned ||
	    !vram_vertex)
		return -EOPNOTSUPP;
	if (!args->size || args->size > PARHELIA_VERTEX_BO_MAX_SIZE ||
	    args->flags || args->handle || args->mmap_offset)
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	bo = drm_gem_vram_create(drm, size, 1);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto out_put;
	}
	ret = drm_gem_handle_create(file, &bo->bo.base, &handle);
	if (ret)
		goto out_free;

	mutex_lock(&pdev->submit_lock);
	if (pdev->wedged) {
		ret = -EIO;
	} else {
		ret = parhelia_vram_pin(bo);
		if (!ret) {
			/* Retain the BO independently of its GEM handle. postclose drops
			 * this after unpinning, which also handles an explicit GEM_CLOSE. */
			drm_gem_object_get(&bo->bo.base);
			entry->file = file;
			entry->bo = bo;
			list_add_tail(&entry->link, &pdev->vertex_bos);
		}
	}
	mutex_unlock(&pdev->submit_lock);
	if (ret)
		goto out_handle;

	args->size = size;
	args->handle = handle;
	args->mmap_offset = drm_vma_node_offset_addr(&bo->bo.base.vma_node);
	drm_gem_object_put(&bo->bo.base);
	return 0;

out_handle:
	drm_gem_handle_delete(file, handle);
out_free:
	kfree(entry);
out_put:
	drm_gem_object_put(&bo->bo.base);
	return ret;
}

int parhelia_get_param_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_parhelia_get_param *args = data;

	switch (args->param) {
	case PARHELIA_PARAM_CHIP_ID:
		args->value = pdev->pdev->device;
		break;
	case PARHELIA_PARAM_VRAM_SIZE:
		args->value = pdev->vram_size;
		break;
	case PARHELIA_PARAM_MAX_PACKETS:
		args->value = PARHELIA_MAX_USER_PACKETS;
		break;
	case PARHELIA_PARAM_MAX_COMMAND_DWORDS:
		args->value = PARHELIA_MAX_COMMAND_DWORDS;
		break;
	case PARHELIA_PARAM_WARP_PASSTHROUGH:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_PASSTHROUGH_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_TEXTURED:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_TEXTURED_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_TEXTURED_MULTITEX:
		args->value = parhelia_warp_entry(pdev,
					 PARHELIA_WARP_TEXTURED_MULTITEX_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_REPLACE:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_REPLACE_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_TRANSFORM:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_TRANSFORM_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_LIGHTING:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_LIGHTING_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_CONSTANT_LOADER:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_CONSTANT_LOADER_ENTRY);
		break;
	case PARHELIA_PARAM_TILE_LAYOUT:
		args->value = pdev->chip->tile_layout;
		break;
	case PARHELIA_PARAM_LAST_CLOCK_KHZ:
		args->value = pdev->last_clock_khz;
		break;
	case PARHELIA_PARAM_TEXTURE_UNITS:
		args->value = pdev->chip->texture_units;
		break;
	case PARHELIA_PARAM_TEXTURE_PIPE_MODE:
		args->value = pdev->chip->texture_pipe_mode;
		break;
	case PARHELIA_PARAM_VRAM_VERTEX:
		/* Raw BAR mappings can alias native surfaces on the P690. */
		args->value = pdev->pdev->device == PARHELIA_P690 &&
			      pdev->raw_aperture_owned && vram_vertex;
		break;
	case PARHELIA_PARAM_NATIVE_INDEXED:
		/* This is never a normal rendering feature yet.  It is exposed solely
		 * to the one guarded screen test when the module was explicitly loaded
		 * with mode7_screen_test=1. */
		args->value = mode7_screen_test &&
			pdev->pdev->device == PARHELIA_P690 && pdev->raw_aperture_owned;
		break;
	case PARHELIA_PARAM_BOUND_RANGE_TEST:
		args->value = mode3_screen_test &&
			pdev->pdev->device == PARHELIA_P690 && pdev->raw_aperture_owned;
		break;
	case PARHELIA_PARAM_BOUND_RANGE:
		args->value = mode3_bound_range &&
			pdev->pdev->device == PARHELIA_P690 && pdev->raw_aperture_owned;
		break;
	case PARHELIA_PARAM_ASYNC_SUBMIT:
		args->value = pdev->async_primary_cpu && !trace_submit &&
			!full_sync && !post_draw_wb && coalesce_primary;
		break;
	case PARHELIA_PARAM_STATEFUL_SUBMIT:
		args->value = stateful_submit && pdev->async_primary_cpu &&
			!trace_submit && !full_sync && !post_draw_wb &&
			coalesce_primary;
		break;
	case PARHELIA_PARAM_NATIVE_SHARE:
		args->value = pdev->raw_aperture_owned;
		break;
	case PARHELIA_PARAM_TEXTURE_UPLOAD:
		args->value = pdev->pdev->device == PARHELIA_P690 &&
			pdev->raw_aperture_owned && parhelia_texture_upload_enabled();
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int parhelia_submit_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_parhelia_submit *args = data;
	struct drm_parhelia_packet *packets;
	struct drm_parhelia_reloc *relocs;
	struct drm_gem_object **objects = NULL;
	struct drm_gem_vram_object **pinned = NULL;
	struct parhelia_call_plan *plan;
	struct parhelia_submit_slot *async_slot = NULL;
	struct parhelia_dma_block *blocks = pdev->dma_blocks;
	struct parhelia_mode7_submit mode7 = { };
	struct parhelia_mode7_state mode7_state = { };
	struct parhelia_mode3_submit mode3 = { };
	struct parhelia_bound_range_submit bound_range = { };
	struct parhelia_shadow_txn *shadow_txn = NULL;
	u32 *commands, *primary;
	void *vertex_cpu = pdev->vertex_cpu;
	dma_addr_t vertex_bus = pdev->vertex_bus;
	dma_addr_t writeback_bus = pdev->writeback_bus;
	dma_addr_t primary_bus = pdev->primary_bus;
	bool *draw_relocated;
	bool host_vertex_draw = false;
	bool vram_vertex_draw = false;
	bool async = args->flags & PARHELIA_SUBMIT_ASYNC;
	u32 i, j, end = 0, fence, dma_tag, primary_words, waited, opening_slot;
	u32 boundary_packet = 0, boundary_count = 0;
	u32 call_words = trace_submit ? 8 : PARHELIA_CALL_DWORDS;
	u32 prefix_words = full_sync ? 3 : 0;
	u32 total_calls;
	u32 fence_step = (replay_fences || full_sync) ? 16 : 8;
	u32 fence_span;
	u32 first_draw;
	const char *async_failure_stage = "request setup";
	int ret = 0;

	static_assert(PARHELIA_MAX_PACKETS == PARHELIA_CALL_CAPACITY);

	if (args->flags & ~(PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC) ||
	    !args->packet_count || args->packet_count > PARHELIA_MAX_USER_PACKETS ||
	    !args->command_dwords || args->command_dwords > PARHELIA_MAX_COMMAND_DWORDS ||
	    args->reloc_count > PARHELIA_MAX_RELOCS ||
	    args->vertex_dwords > PARHELIA_VERTEX_DWORDS)
		return -EINVAL;
	if (async && (!(args->flags & PARHELIA_SUBMIT_FENCE) || !coalesce_primary ||
		      trace_submit || full_sync || post_draw_wb))
		return -EINVAL;
	if (args->vertex_dwords && !args->vertices)
		return -EINVAL;
	/* Keep comparisons independent; tracing is for small diagnostic batches. */
	if ((full_sync && (trace_submit || replay_fences)) ||
	    (post_draw_wb && trace_submit))
		return -EINVAL;
	if (trace_submit && args->packet_count > 64)
		return -E2BIG;

	packets = memdup_user(u64_to_user_ptr(args->packets),
			      array_size(args->packet_count, sizeof(*packets)));
	if (IS_ERR(packets))
		return PTR_ERR(packets);
	commands = memdup_user(u64_to_user_ptr(args->commands),
			       array_size(args->command_dwords, sizeof(*commands)));
	if (IS_ERR(commands)) {
		ret = PTR_ERR(commands);
		goto out_packets;
	}
	plan = kzalloc(sizeof(*plan), GFP_KERNEL);
	if (!plan) {
		ret = -ENOMEM;
		goto out_commands;
	}
	if (state_shadow_cache && !trace_submit && !full_sync && !post_draw_wb) {
		shadow_txn = kzalloc(sizeof(*shadow_txn), GFP_KERNEL);
		if (!shadow_txn) {
			ret = -ENOMEM;
			goto out_commands;
		}
	}
	relocs = memdup_user(u64_to_user_ptr(args->relocs),
			     array_size(args->reloc_count, sizeof(*relocs)));
	if (args->reloc_count && IS_ERR(relocs)) {
		ret = PTR_ERR(relocs);
		goto out_commands;
	}
	if (!args->reloc_count)
		relocs = NULL;
	draw_relocated = kcalloc(args->packet_count, sizeof(*draw_relocated), GFP_KERNEL);
	pinned = kcalloc(args->reloc_count, sizeof(*pinned), GFP_KERNEL);
	objects = kcalloc(args->reloc_count, sizeof(*objects), GFP_KERNEL);
	if (!draw_relocated || (args->reloc_count && (!pinned || !objects))) {
		ret = -ENOMEM;
		goto out_relocs;
	}

	for (i = 0; i < args->packet_count; i++) {
		if (packets[i].offset != end ||
		    check_add_overflow(end, packets[i].length, &end) ||
		    end > args->command_dwords ||
		    packets[i].length > PARHELIA_DMA_BLOCK_DWORDS) {
			ret = -EINVAL;
			goto out_relocs;
		}
		ret = parhelia_validate_packet(commands, &packets[i]);
		if (ret) {
			goto out_relocs;
		}
		plan->stage_modes[i] = parhelia_vertex_bind_mode(commands + packets[i].offset,
							       packets[i].length);
	}
	if (end != args->command_dwords) {
		ret = -EINVAL;
		goto out_relocs;
	}
	for (i = 0; i < args->reloc_count; i++) {
		const struct drm_parhelia_reloc *reloc = &relocs[i];

		if (reloc->type != PARHELIA_RELOC_BATCH_BOUNDARY)
			continue;
		if (++boundary_count > 1 || !stateful_submit || !async ||
		    reloc->packet == 0 || reloc->packet >= args->packet_count ||
		    reloc->dword || reloc->handle || reloc->delta ||
		    reloc->pad[0] || reloc->pad[1] || reloc->pad[2]) {
			ret = -EINVAL;
			goto out_relocs;
		}
		boundary_packet = reloc->packet;
	}
	if (boundary_count) {
		u32 opening_slot1;

		opening_slot = parhelia_opening_fence_slot_range(commands, packets,
			0, boundary_packet);
		opening_slot1 = parhelia_opening_fence_slot_range(commands, packets,
			boundary_packet, args->packet_count);
		ret = parhelia_plan_calls_stateful(plan, args->packet_count,
			boundary_packet, opening_slot, opening_slot1);
	} else {
		opening_slot = parhelia_opening_fence_slot_range(commands, packets,
			0, args->packet_count);
		ret = parhelia_plan_calls(plan, args->packet_count, opening_slot,
			full_sync);
	}
	if (ret)
		goto out_relocs;
	total_calls = plan->count;


retry_slot:
	mutex_lock(&pdev->submit_lock);
	parhelia_retire_locked(pdev);
	if (async) {
		u32 slot_index;
		u32 needed = total_calls * PARHELIA_CALL_DWORDS + 3;
		u32 secondary_dwords;

		for (slot_index = 0; slot_index < PARHELIA_ASYNC_SLOTS; slot_index++)
			if (pdev->slots[slot_index].state == PARHELIA_SLOT_FREE)
				break;
		if (slot_index == PARHELIA_ASYNC_SLOTS || pdev->wedged) {
			ret = pdev->wedged ? -EIO : 0;
			mutex_unlock(&pdev->submit_lock);
			if (ret)
				goto out_relocs;
			ret = wait_event_interruptible_timeout(pdev->submit_waitq,
				pdev->wedged || pdev->queued_slots < PARHELIA_ASYNC_SLOTS,
				msecs_to_jiffies(10));
			if (ret < 0)
				goto out_relocs;
			goto retry_slot;
		}
		/* A primary allocation is never wrapped while the device can fetch it.
		 * Once the queue drains, require the proven primary-idle bit before
		 * restarting at its base. */
		if (!pdev->queued_slots && pdev->primary_armed) {
			if (!(parhelia_read(pdev, 0x08f0) & BIT(17))) {
				mutex_unlock(&pdev->submit_lock);
				ret = wait_event_interruptible_timeout(pdev->submit_waitq,
					pdev->wedged || (parhelia_read(pdev, 0x08f0) & BIT(17)),
					msecs_to_jiffies(1));
				if (ret < 0)
					goto out_relocs;
				goto retry_slot;
			}
			pdev->primary_put = 0;
			pdev->primary_get = 0;
			pdev->primary_armed = false;
		}
		if (needed > PARHELIA_SLOT_PRIMARY_DWORDS ||
		    pdev->primary_put > PARHELIA_ASYNC_PRIMARY_DWORDS - needed) {
			/* The coherent primary span is deliberately never wrapped while
			 * fetch is live.  A full span is back-pressure, not a malformed
			 * request: retire its FIFO work, re-arm at zero, then retry. */
			if (pdev->queued_slots) {
				mutex_unlock(&pdev->submit_lock);
				ret = wait_event_interruptible_timeout(pdev->submit_waitq,
					pdev->wedged || !pdev->queued_slots,
					msecs_to_jiffies(10));
				if (ret < 0)
					goto out_relocs;
				goto retry_slot;
			}
			ret = -EBUSY;
			goto out_unlock;
		}
		async_slot = &pdev->slots[slot_index];
		secondary_dwords = args->command_dwords + plan->stages * 13u +
			plan->sync_sections * 14u;
		async_failure_stage = "secondary coherent heap";
		ret = parhelia_async_slot_prepare(pdev, async_slot, secondary_dwords);
		if (ret)
			goto out_unlock;
		blocks = async_slot->blocks;
		vertex_cpu = async_slot->vertex_cpu;
		vertex_bus = async_slot->vertex_bus;
		writeback_bus = async_slot->writeback_bus;
		primary_bus = pdev->async_primary_bus + pdev->primary_put * sizeof(u32);
	}
	if (!async && pdev->queued_slots) {
		mutex_unlock(&pdev->submit_lock);
		ret = wait_event_interruptible_timeout(pdev->submit_waitq,
			pdev->wedged || !pdev->queued_slots, msecs_to_jiffies(10));
		if (ret < 0)
			goto out_relocs;
		goto retry_slot;
	}
	if (pdev->wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = parhelia_engine_init(pdev);
	if (ret)
		goto out_unlock;
	if (args->vertex_dwords &&
	    copy_from_user(vertex_cpu, u64_to_user_ptr(args->vertices),
			   args->vertex_dwords * sizeof(u32))) {
		ret = -EFAULT;
		goto out_unlock;
	}

	for (i = 0; i < args->reloc_count; i++) {
		struct drm_parhelia_reloc *reloc = &relocs[i];
		struct drm_parhelia_packet *packet;
		u32 *packet_cmd;

		if (reloc->packet >= args->packet_count) {
			ret = -EINVAL;
			goto out_unpin;
		}
		packet = &packets[reloc->packet];
		packet_cmd = commands + packet->offset;
		if (reloc->type == PARHELIA_RELOC_BATCH_BOUNDARY)
			continue;
		/* Indexed mode-7 is an opt-in hang-investigation path.  Its private
		 * snapshot helper still targets the synchronous diagnostic buffers, so
		 * keep it outside the production queue until its hardware contract is
		 * proven. */
		if (async && (reloc->type == PARHELIA_RELOC_STREAM_VRAM ||
			      reloc->type == PARHELIA_RELOC_INDEX_VRAM ||
			      reloc->type == PARHELIA_RELOC_STREAM_DMA ||
			      reloc->type == PARHELIA_RELOC_INDEX_DMA ||
			      reloc->type == PARHELIA_RELOC_RANGE_STREAM_DMA)) {
			ret = -EOPNOTSUPP;
			goto out_unpin;
		}
		if (reloc->type == PARHELIA_RELOC_DRAW) {
			if ((packet_cmd[0] >> 24) != 0x97 || draw_relocated[reloc->packet] ||
			    (reloc->dword & 3) || !reloc->delta || (reloc->delta & 3) ||
			    reloc->handle || reloc->dword > args->vertex_dwords * sizeof(u32) ||
			    reloc->delta > args->vertex_dwords * sizeof(u32) - reloc->dword ||
			    vram_vertex_draw) {
				ret = -EINVAL;
				goto out_unpin;
			}
			packet_cmd[1] = lower_32_bits(vertex_bus) + reloc->dword;
			packet_cmd[2] = packet_cmd[1] + reloc->delta;
			draw_relocated[reloc->packet] = true;
			host_vertex_draw = true;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_DRAW_VRAM) {
			struct parhelia_vertex_bo *vertex_bo;
			u32 range_end, gpu_start, gpu_end;

			if (pdev->pdev->device != PARHELIA_P690) {
				ret = -EOPNOTSUPP;
				goto out_unpin;
			}
			if (host_vertex_draw || args->vertex_dwords ||
			    (packet_cmd[0] >> 24) != 0x97 || draw_relocated[reloc->packet] ||
			    (reloc->dword & 3) || !reloc->delta || (reloc->delta & 3) ||
			    check_add_overflow(reloc->dword, reloc->delta, &range_end)) {
				ret = -EINVAL;
				goto out_unpin;
			}
			objects[i] = drm_gem_object_lookup(file, reloc->handle);
			if (!objects[i]) {
				ret = -ENOENT;
				goto out_unpin;
			}
			vertex_bo = parhelia_vertex_bo_lookup(pdev, file, objects[i]);
			if (!vertex_bo) {
				ret = -EACCES;
				goto out_unpin;
			}
			/* CREATE_VERTEX owns the persistent VRAM pin. Re-pinning here
			 * would make the object lifetime depend on submit count; its stable
			 * class-0 offset is exactly why this relocation is a separate ABI. */
			if (range_end > vertex_bo->bo->bo.base.size) {
				ret = -EINVAL;
				goto out_unpin;
			}
			if (parhelia_vram_gpu_addr(pdev, vertex_bo->bo,
						    reloc->dword, reloc->delta,
						    &gpu_start) ||
			    (gpu_start & 3) ||
			    check_add_overflow(gpu_start, reloc->delta, &gpu_end) ||
			    gpu_end > PARHELIA_HOST_APERTURE_BASE) {
				ret = -EINVAL;
				goto out_unpin;
			}
			packet_cmd[1] = gpu_start;
			packet_cmd[2] = gpu_end;
			draw_relocated[reloc->packet] = true;
			vram_vertex_draw = true;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_STREAM_VRAM) {
			struct parhelia_vertex_bo *vertex_bo;
			u32 operand_index;

			if (pdev->pdev->device != PARHELIA_P690 ||
			    !pdev->raw_aperture_owned || mode7.stream_seen ||
			    (mode7.transport && mode7.transport != PARHELIA_MODE7_TRANSPORT_VRAM) ||
			    (packet_cmd[0] >> 24) != 0 ||
			    reloc->dword >= packet->length || (reloc->delta & 3) ||
			    parhelia_reglist_operand_index(packet_cmd, packet->length,
						    reloc->dword, &operand_index) ||
			    operand_index != 0 || packet_cmd[reloc->dword]) {
				ret = -EINVAL;
				goto out_unpin;
			}
			objects[i] = drm_gem_object_lookup(file, reloc->handle);
			if (!objects[i]) {
				ret = -ENOENT;
				goto out_unpin;
			}
			vertex_bo = parhelia_vertex_bo_lookup(pdev, file, objects[i]);
			if (!vertex_bo || reloc->delta >= vertex_bo->bo->bo.base.size) {
				ret = -EACCES;
				goto out_unpin;
			}
			mode7.stream_seen = true;
			mode7.transport = PARHELIA_MODE7_TRANSPORT_VRAM;
			mode7.stream_packet = reloc->packet;
			mode7.stream_dword = reloc->dword;
			mode7.stream_offset = reloc->delta;
			mode7.stream_bo = vertex_bo;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_INDEX_VRAM) {
			struct parhelia_vertex_bo *vertex_bo;
			u32 index_end;

			if (pdev->pdev->device != PARHELIA_P690 ||
			    !pdev->raw_aperture_owned || mode7.index_seen ||
			    (mode7.transport && mode7.transport != PARHELIA_MODE7_TRANSPORT_VRAM) ||
			    packet_cmd[0] != 0x9f000001u || packet_cmd[1] || packet_cmd[2] ||
			    (reloc->dword & 3) || !reloc->delta || (reloc->delta & 3) ||
			    (reloc->delta % (3 * sizeof(u32))) ||
			    check_add_overflow(reloc->dword, reloc->delta, &index_end)) {
				ret = -EINVAL;
				goto out_unpin;
			}
			objects[i] = drm_gem_object_lookup(file, reloc->handle);
			if (!objects[i]) {
				ret = -ENOENT;
				goto out_unpin;
			}
			vertex_bo = parhelia_vertex_bo_lookup(pdev, file, objects[i]);
			if (!vertex_bo || index_end > vertex_bo->bo->bo.base.size) {
				ret = -EACCES;
				goto out_unpin;
			}
			mode7.index_seen = true;
			mode7.transport = PARHELIA_MODE7_TRANSPORT_VRAM;
			mode7.index_packet = reloc->packet;
			mode7.index_offset = reloc->dword;
			mode7.index_length = reloc->delta;
			mode7.index_bo = vertex_bo;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_STREAM_DMA) {
			u32 operand_index, bytes = args->vertex_dwords * sizeof(u32);

			if (pdev->pdev->device != PARHELIA_P690 || !mode7_screen_test ||
			    mode7.stream_seen ||
			    (mode7.transport && mode7.transport != PARHELIA_MODE7_TRANSPORT_DMA) ||
			    !args->vertex_dwords || reloc->handle || (reloc->delta & 3) ||
			    reloc->delta >= bytes || (packet_cmd[0] >> 24) ||
			    reloc->dword >= packet->length ||
			    parhelia_reglist_operand_index(packet_cmd, packet->length,
							reloc->dword, &operand_index) ||
			    operand_index != 0 || packet_cmd[reloc->dword]) {
				ret = -EINVAL;
				goto out_unpin;
			}
			mode7.stream_seen = true;
			mode7.transport = PARHELIA_MODE7_TRANSPORT_DMA;
			mode7.stream_packet = reloc->packet;
			mode7.stream_dword = reloc->dword;
			mode7.stream_offset = reloc->delta;
			mode7.stream_size = bytes;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_INDEX_DMA) {
			u32 index_end, bytes = args->vertex_dwords * sizeof(u32);

			if (pdev->pdev->device != PARHELIA_P690 || !mode7_screen_test ||
			    mode7.index_seen ||
			    (mode7.transport && mode7.transport != PARHELIA_MODE7_TRANSPORT_DMA) ||
			    !args->vertex_dwords || reloc->handle ||
			    packet_cmd[0] != 0x9f000001u || packet_cmd[1] || packet_cmd[2] ||
			    (reloc->dword & 3) || !reloc->delta || (reloc->delta & 3) ||
			    (reloc->delta % (3 * sizeof(u32))) ||
			    check_add_overflow(reloc->dword, reloc->delta, &index_end) ||
			    index_end > bytes) {
				ret = -EINVAL;
				goto out_unpin;
			}
			mode7.index_seen = true;
			mode7.transport = PARHELIA_MODE7_TRANSPORT_DMA;
			mode7.index_packet = reloc->packet;
			mode7.index_offset = reloc->dword;
			mode7.index_length = reloc->delta;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_RANGE_STREAM_DMA) {
			u32 operand_index;

			if (pdev->pdev->device != PARHELIA_P690 || !mode3_screen_test ||
			    mode3.stream_seen || mode7.stream_seen || mode7.index_seen ||
			    host_vertex_draw || vram_vertex_draw ||
			    (args->vertex_dwords != 30 && args->vertex_dwords != 36) ||
			    reloc->handle || reloc->delta ||
			    (packet_cmd[0] >> 24) || reloc->dword >= packet->length ||
			    parhelia_reglist_operand_index(packet_cmd, packet->length,
						    reloc->dword, &operand_index) ||
			    operand_index != 0 || packet_cmd[reloc->dword]) {
				ret = -EINVAL;
				goto out_unpin;
			}
			mode3.stream_seen = true;
			mode3.stream_packet = reloc->packet;
			mode3.stream_dword = reloc->dword;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_RANGE_STREAM_VRAM) {
			struct parhelia_vertex_bo *vertex_bo;
			u32 operand_index;

			if (pdev->pdev->device != PARHELIA_P690 || !mode3_bound_range ||
			    !pdev->raw_aperture_owned || bound_range.stream_seen ||
			    mode3.stream_seen || mode7.stream_seen || mode7.index_seen ||
			    args->vertex_dwords || !reloc->handle || (reloc->delta & 3) ||
			    (packet_cmd[0] >> 24) || reloc->dword >= packet->length ||
			    parhelia_reglist_operand_index(packet_cmd, packet->length,
						    reloc->dword, &operand_index) ||
			    operand_index != 0 || packet_cmd[reloc->dword]) {
				ret = -EINVAL;
				goto out_unpin;
			}
			objects[i] = drm_gem_object_lookup(file, reloc->handle);
			if (!objects[i]) {
				ret = -ENOENT;
				goto out_unpin;
			}
			vertex_bo = parhelia_vertex_bo_lookup(pdev, file, objects[i]);
			if (!vertex_bo || reloc->delta >= vertex_bo->bo->bo.base.size) {
				ret = -EACCES;
				goto out_unpin;
			}
			bound_range.stream_seen = true;
			bound_range.stream_packet = reloc->packet;
			bound_range.stream_dword = reloc->dword;
			bound_range.stream_offset = reloc->delta;
			bound_range.stream_bo = vertex_bo;
			continue;
		}
		if (reloc->type != PARHELIA_RELOC_VRAM || reloc->dword >= packet->length ||
		    (packet_cmd[0] >> 24) != 0x00 ||
		    !parhelia_is_vram_operand(packet_cmd, packet->length, reloc->dword)) {
			ret = -EINVAL;
			goto out_unpin;
		}
		objects[i] = drm_gem_object_lookup(file, reloc->handle);
		if (!objects[i]) {
			ret = -ENOENT;
			goto out_unpin;
		}
		if (reloc->delta >= objects[i]->size) {
			ret = -EINVAL;
			goto out_unpin;
		}
		/* A Q3 state batch names a colour target and texture repeatedly.
		 * Keep one GEM reference for every relocation so the existing cleanup
		 * remains simple, but take only one transient TTM pin per unique BO.
		 * Re-pinning a duplicate holds the same placement count dozens of
		 * times until the async fence retires and exhausts the P690's small
		 * VRAM placement budget. */
		for (j = 0; j < i; j++)
			if (objects[j] == objects[i] && pinned[j])
				break;
		if (j == i) {
			async_failure_stage = "VRAM relocation pin";
			ret = parhelia_vram_pin(drm_gem_vram_of_gem(objects[i]));
			if (ret) {
				if (async)
					dev_warn_ratelimited(&pdev->pdev->dev,
						"async VRAM pin failed: relocation=%u handle=%u size=%zu pin_count=%u ret=%d\n",
						i, reloc->handle, objects[i]->size,
						drm_gem_vram_of_gem(objects[i])->bo.pin_count, ret);
				goto out_unpin;
			}
			pinned[i] = drm_gem_vram_of_gem(objects[i]);
		}
		ret = parhelia_vram_gpu_addr(pdev,
				drm_gem_vram_of_gem(objects[i]), reloc->delta,
				objects[i]->size - reloc->delta,
				&packet_cmd[reloc->dword]);
		if (ret)
			goto out_unpin;
	}
	if (mode7.stream_seen || mode7.index_seen) {
		ret = parhelia_validate_mode7_contract(commands, packets,
					       args->packet_count, &mode7, &mode7_state);
		if (ret)
			goto out_unpin;
		ret = parhelia_mode7_validate_packets(commands, packets,
					      args->packet_count, &mode7);
		if (ret)
			goto out_unpin;
		ret = parhelia_mode7_copy_indices(pdev, &mode7, &mode7_state);
		if (ret)
			goto out_unpin;
		ret = parhelia_mode7_patch_addresses(commands, packets, &mode7, pdev);
		if (ret)
			goto out_unpin;
	}
	if (mode3.stream_seen) {
		ret = parhelia_mode3_validate_and_patch(commands, packets,
					args->packet_count, args->vertex_dwords,
					&mode3, pdev);
		if (ret)
			goto out_unpin;
	}
	if (bound_range.stream_seen) {
		ret = parhelia_bound_range_validate_and_patch(commands, packets,
				args->packet_count, &bound_range, pdev);
		if (ret)
			goto out_unpin;
	}
	if (vram_vertex_draw) {
		ret = parhelia_select_vram_vertex_stream(commands, packets,
						  args->packet_count);
		if (ret)
			goto out_unpin;
	}
	for (i = 0; i < args->packet_count; i++) {
		u32 opcode = commands[packets[i].offset] >> 24;

		/* A 0x9f packet is admitted only after the paired stream/index
		 * relocations snapshot and patch it above. */
		if (opcode == 0x9f && !mode7.index_seen) {
			ret = -EINVAL;
			goto out_unpin;
		}
		if (opcode == 0x8f && !mode3.stream_seen &&
		    !bound_range.stream_seen) {
			ret = -EINVAL;
			goto out_unpin;
		}
		if (opcode == 0x97 && !draw_relocated[i]) {
			ret = -EINVAL;
			goto out_unpin;
		}
	}
	if (shadow_txn) {
		lockdep_assert_held(&pdev->submit_lock);
		parhelia_shadow_filter_plan(&pdev->reg_shadow, commands, packets,
			args->packet_count, relocs, args->reloc_count, plan, shadow_txn);
		total_calls = plan->count;
	}
	fence_span = plan->sync_sections * fence_step +
		(plan->sync_sections - 1) * 8;

	/* Allocate the opening and completion sections before the doorbell. */
	for (i = args->packet_count;
	     i < args->packet_count + 3 * plan->sync_sections + plan->stages;
	     i++) {
		struct parhelia_dma_block *block = &blocks[i];
		u32 sync_index = i - args->packet_count;
		u32 words;

		if (sync_index >= 3 * plan->sync_sections)
			words = 13;
		else if (sync_index % 3 == 0)
			words = 5;
		else if (sync_index % 3 == 1)
			words = post_draw_wb && sync_index == 1 ? 7 : 3;
		else
			words = 6;

		if (async)
			ret = parhelia_async_block_assign(async_slot, block, words);
		else if (!block->cpu) {
			block->cpu = dmam_alloc_coherent(&pdev->pdev->dev,
				PARHELIA_DMA_BLOCK_DWORDS * sizeof(u32), &block->bus, GFP_KERNEL);
			if (!block->cpu)
				ret = -ENOMEM;
		}
		if (ret)
			goto out_unpin;
	}
	if (async) {
		u32 done = parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u;

		/* The recovered replay cadence advances terminal tags by 24: opening
		 * is prior completion + 8, terminal DWGSYNC is opening + 16. */
		fence = pdev->queued_slots ? pdev->next_fence + 8 : done + 8;
		if (fence < 8 || fence > U32_MAX - fence_span ||
		    (s32)((fence + fence_span) - done) < 0) {
			ret = -EOVERFLOW;
			goto out_unpin;
		}
	} else {
		fence = (parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u) + 8;
	}
	/* Leave room for both counters without relying on undocumented wrap. */
	if (fence < 8 || fence > U32_MAX - fence_span) {
		ret = -EOVERFLOW;
		goto out_unpin;
	}
	{
		u32 dwgctl = parhelia_read(pdev, PARHELIA_R_DWGCTL);

		for (i = 0; i < plan->sync_sections; i++) {
			u32 range_begin = i ? boundary_packet : 0;
			u32 range_end = i + 1 < plan->sync_sections ?
				boundary_packet : args->packet_count;

			dwgctl = parhelia_batch_dwgctl_range(commands, packets,
				range_begin, range_end, dwgctl);
			parhelia_emit_fences(blocks, args->packet_count + i * 3,
				fence, fence + fence_step, dwgctl);
			fence += fence_step;
			if (i + 1 < plan->sync_sections)
				fence += 8;
		}
	}

	primary = async ? (u32 *)pdev->async_primary_cpu + pdev->primary_put + prefix_words :
		(u32 *)pdev->primary_cpu + prefix_words;
	dma_tag = ++pdev->next_dma_tag;
	if (!dma_tag)
		dma_tag = ++pdev->next_dma_tag;
	if (full_sync) {
		u32 *arm = pdev->primary_cpu;

		if (dma_tag == U32_MAX) {
			ret = -EOVERFLOW;
			goto out_unpin;
		}
		pdev->next_dma_tag = dma_tag + 1;
		arm[0] = 0x21212425;
		arm[1] = dma_tag;
		arm[2] = lower_32_bits(pdev->writeback_bus) + 0x20;
	}
	if (post_draw_wb) {
		u32 *eob = pdev->dma_blocks[args->packet_count + 1].cpu;

		if (pdev->next_dma_tag == U32_MAX) {
			ret = -EOVERFLOW;
			goto out_unpin;
		}
		/* Annotated capture ring1104: a header-ful writeback after
		 * DRAW, before the sync at ring1108. Do not wait for it here:
		 * the following EOB/flush must also be submitted.
		 */
		memmove(eob + 4, eob, 3 * sizeof(u32));
		eob[0] = 2;
		eob[1] = 0x21212425;
		eob[2] = pdev->next_dma_tag++;
		eob[3] = lower_32_bits(pdev->writeback_bus) + 0x20;
	}
	if (trace_submit) {
		/* Give each marker a fresh serial and use the known-good status
		 * word. Reusing one serial at changing addresses is not validated.
		 */
		if (dma_tag > U32_MAX - total_calls) {
			ret = -EOVERFLOW;
			goto out_unpin;
		}
		pdev->next_dma_tag = dma_tag + total_calls;
	}
	first_draw = total_calls;
	for (i = 0; i < total_calls; i++) {
		u32 block_id = plan->blocks[i];

		if (block_id < args->packet_count &&
		    ((commands[packets[block_id].offset] >> 24) == 0x97 ||
		     (commands[packets[block_id].offset] >> 24) == 0x8f)) {
			first_draw = i;
			break;
		}
	}
	/* Reserved block storage does not determine execution order: insert
	 * the opening fence at the reference state builder's fence boundary.
	 */
	for (i = 0; i < total_calls; i++) {
		u32 block_id = plan->blocks[i];
		struct parhelia_dma_block *block = &blocks[block_id];
		dma_addr_t start, finish;
		u32 primary_slot = i;
		u32 stage_base = args->packet_count + 3 * plan->sync_sections;
		u32 words;

		if (block_id < args->packet_count) {
			if (async)
				ret = parhelia_async_block_assign(async_slot, block,
					packets[block_id].length);
			else if (!block->cpu) {
				block->cpu = dmam_alloc_coherent(&pdev->pdev->dev,
					PARHELIA_DMA_BLOCK_DWORDS * sizeof(u32), &block->bus, GFP_KERNEL);
				if (!block->cpu) {
					ret = -ENOMEM;
				}
			}
			if (ret)
				goto out_unpin;
			words = packets[block_id].length;
			memcpy(block->cpu, commands + packets[block_id].offset, words * sizeof(*commands));
		} else if (block_id >= stage_base) {
			u32 parent = plan->stage_parent[block_id - stage_base];

			parhelia_stage_commands(block->cpu, plan->stage_modes[parent],
				null_stages ? 2 : parhelia_warp_entry(pdev, PARHELIA_WARP_STAGE_A_ENTRY),
				null_stages ? 2 : parhelia_warp_entry(pdev, PARHELIA_WARP_STAGE_B_ENTRY),
				null_stages ? 2 : parhelia_warp_entry(pdev, PARHELIA_WARP_STAGE_C_ENTRY),
				null_stages ? 2 : 0);
			words = 13;
		} else {
			u32 sync_index = block_id - args->packet_count;

			words = sync_index % 3 == 0 ? 5 :
				sync_index % 3 == 1 ?
					(post_draw_wb && sync_index == 1 ? 7 : 3) : 6;
		}
		start = block->bus;
		finish = start + words * sizeof(u32);
		primary[primary_slot * call_words + 0] = PARHELIA_CALL_HEADER;
		primary[primary_slot * call_words + 1] = 0;
		primary[primary_slot * call_words + 2] = lower_32_bits(start) | 2;
		primary[primary_slot * call_words + 3] = lower_32_bits(finish) | 3;
		primary[primary_slot * call_words + 4] = 0;
		if (trace_submit) {
			/* Never interpose a marker between DRAW and EOB/drain:
			 * doing so could introduce the very dependency being tested.
			 */
			primary[primary_slot * call_words + 5] = primary_slot < first_draw ?
				0x21212425 : 0x21212121;
			primary[primary_slot * call_words + 6] = primary_slot < first_draw ?
				dma_tag + primary_slot : 0x21212121;
			primary[primary_slot * call_words + 7] = primary_slot < first_draw ?
				lower_32_bits(pdev->writeback_bus) + 0x20 :
				0x21212121;
			dev_info(&pdev->pdev->dev, "trace CALL %u block=%u words=%u header=%08x tag=%08x\n",
				 primary_slot, block_id, words, ((u32 *)block->cpu)[0],
				 ((u32 *)block->cpu)[1]);
		}
	}
	/* Independent DMA completion, packet recovered in bring-up section 1.7.
	 * Keep the writeback target inside the kernel's status page. This also
	 * distinguishes a fetched primary span from executed register packets.
	 */
	primary_words = total_calls * call_words;
	if (full_sync) {
		memcpy(primary + primary_words,
		       (u32 *)blocks[args->packet_count + 2].cpu + 1,
		       9 * sizeof(u32));
		primary_words += 9;
	}
	primary[primary_words++] = 0x21212425;
	primary[primary_words++] = pdev->next_dma_tag;
	primary[primary_words++] = lower_32_bits(writeback_bus) + 0x20;
	if (async) {
		/* Attach the private completion fence before the producer update.
		 * The slot owns its reference; dma-resv takes further references for
		 * render targets and textures, so CPU/KMS users can never observe a
		 * returned ioctl as permission to reuse the BO early. */
		async_failure_stage = "private dma_fence";
		ret = parhelia_slot_fence_init(pdev, async_slot, fence);
		if (ret)
			goto out_unpin;
		async_failure_stage = "dma-resv fence attachment";
		ret = parhelia_slot_attach_fence(async_slot, objects, args->reloc_count);
		/* The slot itself retains every GEM reference and VRAM pin through
		 * DWGSYNC.  On the 32-bit P690 test target dma-resv's optional fence
		 * array can fail under pressure; keep the explicit waitable submission
		 * usable rather than dropping a complete frame.  Mesa waits that fence
		 * before CPU/scanout handoff, and a later retry can restore implicit
		 * sharing without changing the hardware queue contract. */
		if (ret == -ENOMEM) {
			dev_warn_ratelimited(&pdev->pdev->dev,
				"async dma-resv fence attachment exhausted memory; using slot lifetime fence only\n");
			ret = 0;
		}
		if (ret)
			goto out_unpin;
	}
	if (args->vertex_dwords)
		parhelia_warp_trace_vertex(pdev, "before");
	dma_wmb();
	if (async) {
		/* 0x0404 restarts the primary fetcher.  It is written only for the
		 * first append into an empty allocation; later queue entries advance
		 * 0x0408, the vendor's producer doorbell. */
		if (!pdev->primary_armed) {
			ret = parhelia_wait_dma_idle(pdev);
			if (ret)
				goto out_unpin;
			parhelia_write(pdev, PARHELIA_MMIO_RING_START,
				       lower_32_bits(primary_bus));
			pdev->primary_armed = true;
		}
		parhelia_write(pdev, PARHELIA_MMIO_RING_END,
			lower_32_bits(primary_bus + primary_words * sizeof(u32)) | 3);
		parhelia_write(pdev, 0x0314, 1);
		parhelia_shadow_commit(pdev, shadow_txn);

		async_slot->file = file;
		async_slot->fence = fence;
		async_slot->dma_tag = pdev->next_dma_tag;
		async_slot->primary_begin = pdev->primary_put;
		async_slot->primary_end = pdev->primary_put + primary_words;
		async_slot->call_count = total_calls;
		async_slot->deadline = jiffies +
			usecs_to_jiffies(PARHELIA_FENCE_TIMEOUT_US *
				plan->sync_sections) + 1;
		async_slot->reloc_count = args->reloc_count;
		if (args->reloc_count) {
			memcpy(async_slot->objects, objects,
			       args->reloc_count * sizeof(*objects));
			memcpy(async_slot->pinned, pinned,
			       args->reloc_count * sizeof(*pinned));
			memset(objects, 0, args->reloc_count * sizeof(*objects));
			memset(pinned, 0, args->reloc_count * sizeof(*pinned));
		}
		async_slot->mode7_index_copy = mode7.index_copy;
		mode7.index_copy = NULL;
		async_slot->state = PARHELIA_SLOT_QUEUED;
		if (!pdev->queued_slots)
			pdev->primary_get = pdev->primary_put;
		pdev->primary_put = async_slot->primary_end;
		pdev->queued_slots++;
		pdev->next_fence = fence;
		if (!pdev->first_fence)
			pdev->first_fence = fence;
		args->fence = fence;
		mod_delayed_work(system_wq, &pdev->retire_work, msecs_to_jiffies(1));
		goto out_unpin;
	}
	parhelia_write(pdev, PARHELIA_MMIO_RING_START, lower_32_bits(primary_bus));
	/* The diagnostic modes retain the one-CALL replay cadence.  On the normal
	 * coalesced path the complete DMA-visible primary span is published below,
	 * exactly as the vendor builds it: CALLs, final writeback, one producer
	 * update and the usual front-end strobe. */
	if (coalesce_primary && !trace_submit && !full_sync) {
		parhelia_write(pdev, PARHELIA_MMIO_RING_END,
			lower_32_bits(primary_bus +
				(primary_words * sizeof(u32))) | 3);
		parhelia_write(pdev, 0x0314, 1);
		parhelia_shadow_commit(pdev, shadow_txn);
		for (waited = 0; waited < PARHELIA_FENCE_TIMEOUT_US; waited++) {
			if (READ_ONCE(((u32 *)pdev->writeback_cpu)[8]) ==
			    pdev->next_dma_tag)
				break;
			udelay(1);
		}
		if (waited == PARHELIA_FENCE_TIMEOUT_US) {
			i = total_calls;
			ret = -ETIMEDOUT;
			goto timeout;
		}
		dma_rmb();
		ret = parhelia_wait_fence(pdev, fence);
		if (ret)
			goto timeout;
		args->fence = fence;
		if (args->vertex_dwords)
			parhelia_warp_trace_vertex(pdev, "complete");
		goto out_unpin;
	}
	/* Match the measured replay_kick cadence: one CALL per doorbell. */
	(void)parhelia_read(pdev, PARHELIA_MMIO_RING_START);
	if (full_sync) {
		/* Establish the writeback path before any drawing/707 drain.
		 * Waiting here is safe: only the stand-alone writeback is kicked.
		 */
		parhelia_write(pdev, PARHELIA_MMIO_RING_END,
			lower_32_bits(pdev->primary_bus + prefix_words * sizeof(u32)) | 3);
		for (waited = 0; waited < PARHELIA_FENCE_TIMEOUT_US; waited++) {
			if (READ_ONCE(((u32 *)pdev->writeback_cpu)[8]) == dma_tag)
				break;
			udelay(1);
		}
		if (waited == PARHELIA_FENCE_TIMEOUT_US) {
			i = 0;
			ret = -ETIMEDOUT;
			goto timeout;
		}
		dma_rmb();
		dev_info(&pdev->pdev->dev, "full sync: writeback armed tag=%08x, final fence=%08x\n",
			 dma_tag, fence);
	}
	for (i = 0; i < total_calls; i++) {
		parhelia_write(pdev, PARHELIA_MMIO_RING_END,
			lower_32_bits(pdev->primary_bus + (prefix_words + i * call_words) * sizeof(u32) +
				PARHELIA_CALL_DWORDS * sizeof(u32)) | 3);
		ret = parhelia_wait_dma_idle(pdev);
		if (ret)
			goto timeout;
		if (trace_submit && i < first_draw) {
			parhelia_write(pdev, PARHELIA_MMIO_RING_END,
				lower_32_bits(pdev->primary_bus + (i + 1) * call_words * sizeof(u32)) | 3);
			for (waited = 0; waited < PARHELIA_FENCE_TIMEOUT_US; waited++) {
				if (READ_ONCE(((u32 *)pdev->writeback_cpu)[8]) == dma_tag + i)
					break;
				udelay(1);
			}
			if (waited == PARHELIA_FENCE_TIMEOUT_US) {
				ret = -ETIMEDOUT;
				goto timeout;
			}
			dma_rmb();
			dev_info(&pdev->pdev->dev, "trace CALL %u retired: BM=%08x 908=%08x 924=%08x\n",
				 i, parhelia_read(pdev, 0x08f0), parhelia_read(pdev, 0x0908),
				 parhelia_read(pdev, 0x0924));
		}
	}
	parhelia_write(pdev, PARHELIA_MMIO_RING_END,
		lower_32_bits(pdev->primary_bus + (prefix_words + primary_words) * sizeof(u32)) | 3);
	/* tri_ringreplay issues this front-end strobe after replay_kick and
	 * before inspecting drawing completion. Preserve that final step of
	 * the working example, in addition to the per-CALL PRIMEND cadence.
	 */
	parhelia_write(pdev, 0x0314, 1);
	parhelia_shadow_commit(pdev, shadow_txn);
	for (waited = 0; waited < PARHELIA_FENCE_TIMEOUT_US; waited++) {
		/* Hardware measurement: the word is written, but 0x450 stays at
		 * its idle snapshot, NOT this tag. Wait on coherent memory.
		 */
		if (READ_ONCE(((u32 *)pdev->writeback_cpu)[8]) == pdev->next_dma_tag)
			break;
		udelay(1);
	}
	if (waited == PARHELIA_FENCE_TIMEOUT_US) {
		ret = -ETIMEDOUT;
		goto timeout;
	}
	dma_rmb();
	ret = parhelia_wait_fence(pdev, fence);
	if (ret)
		goto timeout;
	args->fence = fence;
	if (args->vertex_dwords)
		parhelia_warp_trace_vertex(pdev, "complete");
	goto out_unpin;

timeout:
	dev_err(&pdev->pdev->dev,
		"submit timeout at CALL %u/%u: BM=%08x prim=%08x/%08x sec=%08x/%08x fence=%08x want=%08x; wedged\n",
		i, total_calls, parhelia_read(pdev, 0x08f0),
		parhelia_read(pdev, 0x0404), parhelia_read(pdev, 0x0408),
		parhelia_read(pdev, 0x10a0), parhelia_read(pdev, 0x10a4),
		parhelia_read(pdev, 0x041c), fence);
	dma_rmb();
	dev_err(&pdev->pdev->dev, "DMA status450=%08x want=%08x writeback=%08x DWGCTL=%08x 908=%08x 924=%08x\n",
		parhelia_read(pdev, 0x0450), pdev->next_dma_tag,
		READ_ONCE(((u32 *)pdev->writeback_cpu)[8]), parhelia_read(pdev, 0x1300),
		parhelia_read(pdev, 0x0908), parhelia_read(pdev, 0x0924));
	pdev->wedged = true;
	parhelia_state_shadow_invalidate(pdev, "synchronous submit timeout");
	pci_clear_master(pdev->pdev);
	if (args->vertex_dwords)
		parhelia_warp_trace_vertex(pdev, "timeout");
	/* Do not unpin, release, or reuse any buffer the GPU might still use.
	 * The onboard pipeline may remain active even with PCI DMA disabled.
	 */
	pdev->quarantine_objects = objects;
	pdev->quarantine_pinned = pinned;
	pdev->quarantine_count = args->reloc_count;
	pdev->quarantine_mode7_index = mode7.index_copy;
	mode7.index_copy = NULL;
	objects = NULL;
	pinned = NULL;
	goto out_unlock;

out_unpin:
	if (async && ret == -ENOMEM)
		dev_warn_ratelimited(&pdev->pdev->dev,
			"async submit allocation failed in %s (packets=%u commands=%u relocs=%u)\n",
			async_failure_stage, args->packet_count, args->command_dwords,
			args->reloc_count);
	if (async_slot && async_slot->state == PARHELIA_SLOT_FREE &&
	    async_slot->out_fence) {
		dma_fence_set_error(&async_slot->out_fence->base, ret ?: -ECANCELED);
		dma_fence_signal(&async_slot->out_fence->base);
		dma_fence_put(&async_slot->out_fence->base);
		async_slot->out_fence = NULL;
	}
	if (mode7.index_copy) {
		parhelia_vram_unpin(mode7.index_copy);
		drm_gem_object_put(&mode7.index_copy->bo.base);
	}
	for (i = 0; i < args->reloc_count; i++) {
		if (pinned[i])
			parhelia_vram_unpin(pinned[i]);
		if (objects[i])
			drm_gem_object_put(objects[i]);
	}
out_unlock:
	mutex_unlock(&pdev->submit_lock);
out_relocs:
	kfree(objects);
	kfree(pinned);
	kfree(draw_relocated);
	kvfree(relocs);
out_commands:
	kfree(shadow_txn);
	kfree(plan);
	kvfree(commands);
out_packets:
	kvfree(packets);
	return ret;
}

static bool parhelia_fence_is_issued_locked(struct parhelia_device *pdev,
					    u32 fence)
{
	/* Terminal tags are spaced by 24 with the documented replay-fence
	 * cadence.  Keep this intentionally bounded to the current boot's
	 * timeline; a random MMIO-looking tag is not a waitable UAPI object. */
	if (!pdev->first_fence || (fence & 7) ||
	    (s32)(fence - pdev->first_fence) < 0 ||
	    (s32)(pdev->next_fence - fence) < 0)
		return false;
	return (fence - pdev->first_fence) % 24 == 0;
}

static bool parhelia_fence_done_or_wedged(struct parhelia_device *pdev,
					  u32 fence)
{
	return READ_ONCE(pdev->wedged) ||
		(s32)((parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u) - fence) >= 0;
}

int parhelia_wait_fence_ioctl(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_parhelia_wait_fence *args = data;
	long waited;
	unsigned long timeout;
	bool issued;

	if (args->flags || args->pad || args->timeout_ns < -1)
		return -EINVAL;
	args->completed = parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u;
	mutex_lock(&pdev->submit_lock);
	parhelia_retire_locked(pdev);
	issued = parhelia_fence_is_issued_locked(pdev, args->fence);
	mutex_unlock(&pdev->submit_lock);
	if (!issued)
		return -EINVAL;

	if (parhelia_fence_done_or_wedged(pdev, args->fence))
		goto done;
	if (!args->timeout_ns)
		return -ETIME;
	if (args->timeout_ns < 0) {
		waited = wait_event_interruptible(pdev->submit_waitq,
			parhelia_fence_done_or_wedged(pdev, args->fence));
	} else {
		timeout = max_t(unsigned long, 1,
			(unsigned long)nsecs_to_jiffies64(args->timeout_ns));
		waited = wait_event_interruptible_timeout(pdev->submit_waitq,
			parhelia_fence_done_or_wedged(pdev, args->fence), timeout);
		if (!waited)
			return -ETIME;
	}
	if (waited < 0)
		return waited;
done:
	args->completed = parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u;
	return READ_ONCE(pdev->wedged) ? -EIO : 0;
}

void parhelia_submit_fini(struct parhelia_device *pdev)
{
	struct parhelia_vertex_bo *entry, *tmp;
	u32 i;

	cancel_delayed_work_sync(&pdev->retire_work);
	mutex_lock(&pdev->submit_lock);
	pdev->wedged = true;
	parhelia_state_shadow_invalidate(pdev, "submit teardown");
	/* Teardown only: block external DMA before devres frees coherent memory. */
	pci_clear_master(pdev->pdev);
	/* pcim_enable_device owns the final disable; doing it here as well
	 * decrements the enable count twice during managed teardown.
	 */
	for (i = 0; i < pdev->quarantine_count; i++) {
		if (pdev->quarantine_pinned[i])
			parhelia_vram_unpin(pdev->quarantine_pinned[i]);
		if (pdev->quarantine_objects[i])
			drm_gem_object_put(pdev->quarantine_objects[i]);
	}
	kfree(pdev->quarantine_pinned);
	kfree(pdev->quarantine_objects);
	if (pdev->quarantine_mode7_index) {
		parhelia_vram_unpin(pdev->quarantine_mode7_index);
		drm_gem_object_put(&pdev->quarantine_mode7_index->bo.base);
		pdev->quarantine_mode7_index = NULL;
	}
	pdev->quarantine_count = 0;
	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		/* PCI mastering is disabled above, so even a timed-out slot can now
		 * be released before devres frees its coherent DMA backing. */
		if (slot->state == PARHELIA_SLOT_QUARANTINED)
			slot->state = PARHELIA_SLOT_QUEUED;
		parhelia_slot_release_locked(pdev, slot, true);
		parhelia_async_slot_heap_fini(pdev, slot);
	}
	list_for_each_entry_safe(entry, tmp, &pdev->vertex_bos, link) {
		list_del(&entry->link);
		parhelia_vram_unpin(entry->bo);
		drm_gem_object_put(&entry->bo->bo.base);
		kfree(entry);
	}
	mutex_unlock(&pdev->submit_lock);
}

void parhelia_submit_postclose(struct parhelia_device *pdev,
			   struct drm_file *file)
{
	struct parhelia_vertex_bo *entry, *tmp;
	u32 i;

retry:
	mutex_lock(&pdev->submit_lock);
	parhelia_retire_locked(pdev);
	if (!pdev->wedged) {
		for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
			struct parhelia_submit_slot *slot = &pdev->slots[i];

			if (slot->state == PARHELIA_SLOT_QUEUED && slot->file == file) {
				mutex_unlock(&pdev->submit_lock);
				/* postclose must not release a CREATE_VERTEX pin while an
				 * in-flight class-0 batch still owns its GEM lookup. */
				wait_event(pdev->submit_waitq,
					READ_ONCE(pdev->wedged) ||
					READ_ONCE(slot->state) != PARHELIA_SLOT_QUEUED);
				goto retry;
			}
		}
	}

	list_for_each_entry_safe(entry, tmp, &pdev->vertex_bos, link) {
		if (entry->file != file)
			continue;
		/* A timeout quarantines every object which may still be read by the
		 * onboard engine. Keep this persistent pin until module teardown too;
		 * PCI mastering is already disabled, but unpinning could move VRAM. */
		if (pdev->wedged) {
			entry->file = NULL;
			continue;
		}
		list_del(&entry->link);
		parhelia_vram_unpin(entry->bo);
		drm_gem_object_put(&entry->bo->bo.base);
		kfree(entry);
	}
	mutex_unlock(&pdev->submit_lock);
}
