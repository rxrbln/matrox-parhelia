// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
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
#include <linux/hash.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>

#include <drm/drm_gem.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_resource.h>

#include "parhelia_drv.h"
#include "parhelia_packets.h"
#include "parhelia_firmware.h"
#include "parhelia_warp_state.h"

#define PARHELIA_CALL_HEADER		0x20292820u
#define PARHELIA_CALL_DWORDS		5u
#define PARHELIA_FENCE_TIMEOUT_US	50000u
#define PARHELIA_R_DWGCTL		0x1300

static void parhelia_dump_slot_calls(struct parhelia_device *pdev,
				     const struct parhelia_submit_slot *slot,
				     u32 get);

/* A single textured full-screen draw can take well over 50 ms (minified
 * without mipmaps); only a request without fence progress for this long is
 * treated as an engine hang. */
static unsigned int hang_timeout_ms = 2000;
module_param(hang_timeout_ms, uint, 0600);
MODULE_PARM_DESC(hang_timeout_ms,
		 "Asynchronous request time without fence progress before the engine is declared hung (per fence section)");

static bool trace_submit;
module_param(trace_submit, bool, 0400);
MODULE_PARM_DESC(trace_submit, "Trace coherent retirement of each pre-draw state packet");

static bool submit_stats;
module_param(submit_stats, bool, 0400);
MODULE_PARM_DESC(submit_stats,
		 "Collect async queue and primary publication counters in debugfs");

static u64 parhelia_submit_wait_begin(atomic64_t *waits)
{
	if (!submit_stats)
		return 0;
	atomic64_inc(waits);
	return ktime_get_mono_fast_ns();
}

static void parhelia_submit_wait_end(atomic64_t *elapsed_ns, u64 begin)
{
	if (begin)
		atomic64_add(ktime_get_mono_fast_ns() - begin, elapsed_ns);
}

static void parhelia_submit_high_water(struct parhelia_device *pdev, u32 depth)
{
	s64 old;

	if (!submit_stats)
		return;
	old = atomic64_read(&pdev->submit_stats.queue_high_water);
	while (depth > old) {
		s64 seen = atomic64_cmpxchg(&pdev->submit_stats.queue_high_water,
					old, depth);

		if (seen == old)
			break;
		old = seen;
	}
}

static const char *const parhelia_phase_names[PARHELIA_SUBMIT_PHASES] = {
	"copyin", "pin", "validate", "plan", "lockwait", "retire", "lock",
	"reloc", "admit", "pack",
	"sync", "heap", "fence",
	"kick", "exit", "total",
};

static void parhelia_phase(u64 *t, enum parhelia_submit_phase phase,
			   struct parhelia_submit_stats *st)
{
	u64 now;

	if (!submit_stats)
		return;
	now = ktime_get_ns();
	atomic64_add(now - *t, &st->phase_ns[phase]);
	*t = now;
}

static int parhelia_submit_stats_show(struct seq_file *m, void *data)
{
	struct drm_info_node *node = m->private;
	struct parhelia_device *pdev = to_parhelia(node->minor->dev);
	struct parhelia_submit_stats *stats = &pdev->submit_stats;

	seq_printf(m, "enabled %u\n", submit_stats);
	seq_printf(m, "async_slots %u\n", PARHELIA_ASYNC_SLOTS);
	seq_printf(m, "async_primary_dwords %u\n",
		   PARHELIA_ASYNC_PRIMARY_DWORDS);
	seq_printf(m, "vram_evictions %u\n",
		   atomic_read(&pdev->vram_evictions));
	seq_printf(m, "vram_restores %u\n",
		   atomic_read(&pdev->vram_restores));
	seq_printf(m, "queued_slots %u\n", READ_ONCE(pdev->queued_slots));
	seq_printf(m, "primary_put_dwords %u\n", READ_ONCE(pdev->primary_put));
	seq_printf(m, "primary_get_dwords %u\n", READ_ONCE(pdev->primary_get));
	seq_printf(m, "primary_armed %u\n", READ_ONCE(pdev->primary_armed));
	seq_printf(m, "async_submits %lld\n",
		   atomic64_read(&stats->async_submits));
	seq_printf(m, "async_slot_waits %lld\n",
		   atomic64_read(&stats->slot_waits));
	seq_printf(m, "async_slot_wait_ns %lld\n",
		   atomic64_read(&stats->slot_wait_ns));
	seq_printf(m, "primary_idle_waits %lld\n",
		   atomic64_read(&stats->idle_waits));
	seq_printf(m, "primary_idle_wait_ns %lld\n",
		   atomic64_read(&stats->idle_wait_ns));
	seq_printf(m, "primary_tail_waits %lld\n",
		   atomic64_read(&stats->tail_waits));
	seq_printf(m, "primary_tail_wait_ns %lld\n",
		   atomic64_read(&stats->tail_wait_ns));
	seq_printf(m, "async_ring_start_0x404_writes %lld\n",
		   atomic64_read(&stats->ring_start_writes));
	seq_printf(m, "async_ring_end_0x408_writes %lld\n",
		   atomic64_read(&stats->ring_end_writes));
	seq_printf(m, "queue_high_water %lld\n",
		   atomic64_read(&stats->queue_high_water));
	seq_printf(m, "async_flips %lld\n", atomic64_read(&stats->async_flips));
	seq_printf(m, "sync_flips %lld\n", atomic64_read(&stats->sync_flips));
	seq_printf(m, "flip_late_events %lld\n",
		   atomic64_read(&stats->flip_late_events));
	seq_printf(m, "flip_fence_waits %lld\n",
		   atomic64_read(&stats->flip_fence_waits));
	seq_printf(m, "engine_irqs %lld\n", atomic64_read(&stats->engine_irqs));
	seq_printf(m, "fence_waits %lld\n", atomic64_read(&stats->fence_waits));
	seq_printf(m, "gpu_busy_ns %lld\n", atomic64_read(&stats->gpu_busy_ns));
	seq_printf(m, "fence_wait_ns %lld\n",
		   atomic64_read(&stats->fence_wait_ns));
	seq_printf(m, "packets %lld\n", atomic64_read(&stats->packets));
	seq_printf(m, "command_dwords %lld\n",
		   atomic64_read(&stats->command_dwords));
	seq_printf(m, "relocs %lld\n", atomic64_read(&stats->relocs));
	seq_printf(m, "reloc_pins %lld\n", atomic64_read(&stats->reloc_pins));
	for (int i = 0; i < PARHELIA_SUBMIT_PHASES; i++)
		seq_printf(m, "phase_%s_ns %lld\n", parhelia_phase_names[i],
			   atomic64_read(&stats->phase_ns[i]));
	return 0;
}

static void parhelia_vram_bo_show_list(struct seq_file *m,
				       struct list_head *head, const char *tag)
{
	struct ttm_lru_item *item;

	list_for_each_entry(item, head, link) {
		struct ttm_resource *res;
		struct ttm_buffer_object *bo;

		if (!ttm_lru_item_is_res(item))
			continue;
		res = ttm_lru_item_to_res(item);
		bo = res->bo;
		if (!bo || res->mem_type != TTM_PL_VRAM)
			continue;
		seq_printf(m, "%s 0x%08lx pages %zu pin %u gem_ref %u handles %u dmabuf %d import %d\n",
			   tag, res->start, res->size >> PAGE_SHIFT,
			   bo->pin_count, kref_read(&bo->base.refcount),
			   bo->base.handle_count, !!bo->base.dma_buf,
			   !!drm_gem_is_imported(&bo->base));
	}
}

static int parhelia_vram_bos_show(struct seq_file *m, void *data)
{
	struct drm_info_node *node = m->private;
	struct ttm_device *bdev = &node->minor->dev->vram_mm->bdev;
	struct ttm_resource_manager *man = ttm_manager_type(bdev, TTM_PL_VRAM);
	unsigned int i;

	spin_lock(&bdev->lru_lock);
	for (i = 0; man && i < TTM_MAX_BO_PRIORITY; i++)
		parhelia_vram_bo_show_list(m, &man->lru[i], "lru");
	parhelia_vram_bo_show_list(m, &bdev->unevictable, "pinned");
	spin_unlock(&bdev->lru_lock);
	return 0;
}

static const struct drm_info_list parhelia_submit_debugfs_list[] = {
	{ "parhelia_submit_stats", parhelia_submit_stats_show, 0 },
	{ "parhelia_vram_bos", parhelia_vram_bos_show, 0 },
};

void parhelia_submit_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_create_files(parhelia_submit_debugfs_list,
				 ARRAY_SIZE(parhelia_submit_debugfs_list),
				 minor->debugfs_root, minor);
}

/* Diagnose writes to three pixels in the fixed Quake root-window corruption
 * band.  This is deliberately read-only and default-off.  While enabled,
 * async requests are serialized so a completion-time change can be assigned
 * to one submit rather than to any of several requests in flight. */
static bool submit_root_probe;
module_param(submit_root_probe, bool, 0400);
MODULE_PARM_DESC(submit_root_probe,
		 "Trace submit metadata when known root pixels change (default false)");

static atomic64_t submit_root_probe_seq = ATOMIC64_INIT(0);

struct parhelia_root_probe_reloc {
	u32 type;
	u32 packet;
	u32 dword;
	u32 handle;
	u32 delta;
	u32 target;
	u32 target_end;
};

struct parhelia_root_probe_trace {
	u64 seq;
	u32 before[3];
	u32 packet_count;
	u32 command_dwords;
	u32 reloc_count;
	DECLARE_BITMAP(classes, 256);
	struct parhelia_root_probe_reloc relocs[];
};

static bool parhelia_root_probe_read(struct parhelia_device *pdev, u32 value[3])
{
	static const struct {
		u16 x;
		u16 y;
	} probe[] = { { 0, 402 }, { 64, 402 }, { 0, 418 } };
	u64 bank_size = pdev->vram_size >> 1;
	u32 surface, ctrl, dims, pitch, height;
	u64 physical_base;
	unsigned int i;

	/* Descriptor word 0 is the CPU aperture coordinate.  Word 1 is the
	 * graphics-engine surface address.  The old probe incorrectly added a
	 * linear row offset to word 0; that reads unrelated BAR storage rather
	 * than the root surface's native 64x16 layout. */
	surface = parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1) + 4);
	ctrl = parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1) + 8);
	dims = parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1) + 12);
	pitch = ctrl & GENMASK(13, 0);
	height = dims & GENMASK(13, 0);
	if (!(parhelia_read(pdev, PARHELIA_HOST_APERTURE_SLOT(1)) &
	      PARHELIA_HOST_APERTURE_VALID) || !bank_size ||
	    pdev->chip->tile_layout != PARHELIA_TILE_64X16_4K ||
	    ((ctrl >> 24) & 0x3f) != 0x30 || ((ctrl >> 16) & 7) != 3 ||
	    pitch <= 64 || (pitch & 63) || height <= 418)
		return false;
	/* Undo parhelia_vram_gpu_addr(): bank A is identity, while bank B's
	 * engine window begins at 128 MiB and maps to the second half of BAR0. */
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

		/* P690 64x16/4 KiB native swizzle.  in_tile is a dword index. */
		in_tile = (ix & 1) | (((ix >> 1) & 1) << 1) |
			  (((ix >> 2) & 1) << 2) | ((iy & 1) << 3) |
			  (((ix >> 3) & 1) << 4) | (((ix >> 4) & 1) << 5) |
			  (((iy >> 1) & 1) << 6) | (x5 << 7) |
			  (((iy >> 2) & 1) << 8) | (((iy >> 3) & 1) << 9);
		offset = physical_base +
			 ((u64)trow * tpr + tcol) * SZ_4K + in_tile * sizeof(u32);

		if (offset > pdev->vram_size - sizeof(u32))
			return false;
		value[i] = readl((u8 __iomem *)pdev->vram + offset);
	}
	return true;
}

static struct parhelia_root_probe_trace *
parhelia_root_probe_prepare(struct parhelia_device *pdev,
			    const struct drm_parhelia_submit *args,
			    const struct drm_parhelia_packet *packets,
			    const struct drm_parhelia_reloc *relocs,
			    const u32 *commands)
{
	struct parhelia_root_probe_trace *trace;
	u32 i;

	if (!submit_root_probe)
		return NULL;
	trace = kzalloc(struct_size(trace, relocs, args->reloc_count), GFP_KERNEL);
	if (!trace) {
		dev_warn_ratelimited(&pdev->pdev->dev,
			"root probe metadata allocation failed\n");
		return NULL;
	}
	if (!parhelia_root_probe_read(pdev, trace->before)) {
		dev_warn_ratelimited(&pdev->pdev->dev,
			"root probe cannot read slot-1 native framebuffer view\n");
		kfree(trace);
		return NULL;
	}
	trace->seq = atomic64_inc_return(&submit_root_probe_seq);
	trace->packet_count = args->packet_count;
	trace->command_dwords = args->command_dwords;
	trace->reloc_count = args->reloc_count;
	for (i = 0; i < args->packet_count; i++)
		__set_bit(commands[packets[i].offset] >> 24, trace->classes);
	for (i = 0; i < args->reloc_count; i++) {
		const struct drm_parhelia_reloc *src = &relocs[i];
		struct parhelia_root_probe_reloc *dst = &trace->relocs[i];
		const u32 *cmd;

		dst->type = src->type;
		dst->packet = src->packet;
		dst->dword = src->dword;
		dst->handle = src->handle;
		dst->delta = src->delta;
		if (src->type == PARHELIA_RELOC_BATCH_BOUNDARY ||
		    src->type == PARHELIA_RELOC_CALL_BOUNDARY ||
		    src->packet >= args->packet_count)
			continue;
		cmd = commands + packets[src->packet].offset;
		switch (src->type) {
		case PARHELIA_RELOC_DRAW:
		case PARHELIA_RELOC_DRAW_VRAM:
		case PARHELIA_RELOC_INDEX_VRAM:
		case PARHELIA_RELOC_INDEX_DMA:
			dst->target = cmd[1];
			dst->target_end = cmd[2];
			break;
		default:
			if (src->dword < packets[src->packet].length)
				dst->target = cmd[src->dword];
			break;
		}
	}
	return trace;
}

static void
parhelia_root_probe_complete(struct parhelia_device *pdev,
			     struct parhelia_root_probe_trace *trace,
			     const char *phase)
{
	u32 after[3], i;

	if (!trace || !parhelia_root_probe_read(pdev, after) ||
	    !memcmp(trace->before, after, sizeof(after)))
		return;
	dev_err(&pdev->pdev->dev,
		"root probe changed: seq=%llu phase=%s packets=%u dwords=%u relocs=%u classes=%*pb values=%08x/%08x/%08x->%08x/%08x/%08x\n",
		trace->seq, phase, trace->packet_count, trace->command_dwords,
		trace->reloc_count, 256, trace->classes,
		trace->before[0], trace->before[1], trace->before[2],
		after[0], after[1], after[2]);
	for (i = 0; i < trace->reloc_count; i++) {
		const struct parhelia_root_probe_reloc *reloc = &trace->relocs[i];

		dev_err(&pdev->pdev->dev,
			"root probe seq=%llu reloc=%u type=%u packet=%u dword=%u handle=%u delta=%#x target=%#x..%#x\n",
			trace->seq, i, reloc->type, reloc->packet, reloc->dword,
			reloc->handle, reloc->delta, reloc->target,
			reloc->target_end);
	}
}

/* The vendor queues all primary CALLs, its final writeback, and then rings the
 * producer doorbell once.  The original bring-up path deliberately published
 * one CALL at a time so every boundary could be observed. The coalesced P690
 * path has passed the direct-render, GLX and Quake target gates; trace/full-sync
 * modes retain their exact diagnostic streams regardless. */
static bool coalesce_primary = true;
module_param(coalesce_primary, bool, 0400);
MODULE_PARM_DESC(coalesce_primary,
		 "Queue a normal submit's primary CALLs behind one producer doorbell (default true)");

/* Vendor streams place multiple packet headers in one secondary CALL. Keep
 * the general form opt-in until an exact-order low-level probe has passed on
 * hardware; this packs only proven adjacent 0x8f ranges.  On by default: it
 * is worth 7% in the ioquake3 timedemo (54.6 -> 58.8 fps at 1280x1024,
 * depth 16, with page flipping) and renders identically.
 */
static bool pack_adjacent_packets = true;
module_param(pack_adjacent_packets, bool, 0400);
MODULE_PARM_DESC(pack_adjacent_packets,
		 "Pack adjacent validated packets within explicit logical boundaries (default true)");

static bool pack_draw_packets = true;
module_param(pack_draw_packets, bool, 0400);
MODULE_PARM_DESC(pack_draw_packets,
		 "Pack hardware-tested adjacent relocated 0x97 draws into secondary CALLs (default true)");

/* The documented hardware replay uses --rrfence auto, not the unpatched
 * p3d_state counter literals. Keep that progression on the normal path.
 */
static bool replay_fences = true;
module_param(replay_fences, bool, 0400);
MODULE_PARM_DESC(replay_fences, "Use the working tri_ringreplay --rrfence auto counter progression (default true)");

static bool full_sync;
module_param(full_sync, bool, 0400);
MODULE_PARM_DESC(full_sync, "Compare vendor primary-ring 307/004/707 flush with prior writeback arm");

/* Completed fence as the engine reports it in the status page: every armed
 * 0x12bc tag (bit 0 set) is written to offset 0x10, and with replay fences
 * the register takes that tag + 8 in the same packet; full-sync drains end
 * on an armed tag.  Polling host memory keeps fence waits off the bus the
 * engine fetches from.  Until the first armed tag lands, read the register. */
static u32 parhelia_fence_completed(struct parhelia_device *pdev)
{
	u32 tag = READ_ONCE(((u32 *)pdev->writeback_cpu)[4]);

	if (!tag)
		return parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE) & ~7u;
	return (tag & ~7u) + (replay_fences && !full_sync ? 8 : 0);
}

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

/* Triangles one indexed (0x9f) packet may draw on the P690: 8 is measured
 * clean with shared vertices; 21 sharing a quad across packets stalled. */
static unsigned int p690_indexed_triangles = 8;
module_param(p690_indexed_triangles, uint, 0644);
MODULE_PARM_DESC(p690_indexed_triangles,
		 "Triangles per indexed packet on the P690 (default 8, 0 disables)");

/* The vendor PSER draw layout on the P690: a CALL span that opens with a
 * single 0x97 draw gets the draw's vertices copied right before its header,
 * outside every span, with end == the header's address. */
static bool p690_inline_draws;
module_param(p690_inline_draws, bool, 0644);
MODULE_PARM_DESC(p690_inline_draws, "P690 vertices inline before their draw header (experimental, default false)");
static unsigned long p690_inline_draw_count;
module_param(p690_inline_draw_count, ulong, 0444);
MODULE_PARM_DESC(p690_inline_draw_count, "Draws emitted with inline vertices (read only)");

static bool mode3_screen_test;
module_param(mode3_screen_test, bool, 0400);
MODULE_PARM_DESC(mode3_screen_test,
		 "Enable the isolated P690 mode-3 bound-range screen test (default false)");

static bool mode3_bound_range;
module_param(mode3_bound_range, bool, 0400);
MODULE_PARM_DESC(mode3_bound_range,
		 "Enable P690 one-triangle-per-packet mode-3 rendering (experimental, default false)");

/* BAR0 is a programmable host-aperture window, not a linear CPU view of local
 * memory.  CREATE_VERTEX therefore uses a dedicated DMA32 GEM object and the
 * proven class-3 fetch path.  Keep the historical parameter name because test
 * scripts already use it. */
static bool vram_vertex = true;
module_param(vram_vertex, bool, 0400);
MODULE_PARM_DESC(vram_vertex,
		 "Enable dedicated DMA32 vertex buffers (default true)");

static bool p690_faa;
module_param(p690_faa, bool, 0400);
MODULE_PARM_DESC(p690_faa, "P690 fragment antialiasing: FAA fragment buffers and FAA sync packets (default off)");

/* FAA needs the raw aperture for its buffers and a second 64 MiB bank to
 * keep the fragment buffer apart from native depth. */
bool parhelia_p690_faa(const struct parhelia_device *pdev)
{
	return p690_faa && pdev->pdev->device == PARHELIA_P690 &&
	       pdev->raw_aperture_owned && pdev->vram_size >= SZ_128M;
}

/* A dedicated DMA32 vertex object is the same class-3 source the kernel's own
 * vertex slots are: on the P690 and on the original family, whose draws
 * fetch their ranges from those slots today. */
static bool parhelia_vertex_objects(const struct parhelia_device *pdev)
{
	return vram_vertex && (pdev->pdev->device == PARHELIA_P690 ||
			       parhelia_original_family(pdev));
}

/* Experiment for the P690 multi-primitive packet stall: host-vertex draws of
 * asynchronous requests fetch from a per-slot local-VRAM copy (class 0). */
static bool local_vertex_draws;
module_param(local_vertex_draws, bool, 0400);
MODULE_PARM_DESC(local_vertex_draws,
		 "P690: fetch host-vertex draw ranges from a local-VRAM copy, class 0 (experimental, default false)");

static int parhelia_local_vertex_prepare(struct parhelia_device *pdev,
					 struct parhelia_submit_slot *slot)
{
	const u32 bytes = PARHELIA_VERTEX_DWORDS * sizeof(u32);
	struct drm_gem_vram_object *bo;
	int ret;

	if (slot->local_vertex)
		return 0;
	bo = drm_gem_vram_create(&pdev->drm, PAGE_ALIGN(bytes), 1);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	ret = parhelia_vram_pin(bo);
	if (ret)
		goto out_put;
	ret = parhelia_vram_gpu_addr(pdev, bo, 0, bytes, &slot->local_vertex_gpu);
	if (ret)
		goto out_unpin;
	dev_info(&pdev->pdev->dev, "local vertex copy for slot %td at %#x\n",
		 slot - pdev->slots, slot->local_vertex_gpu);
	slot->local_vertex = bo;
	return 0;

out_unpin:
	parhelia_vram_unpin(bo);
out_put:
	drm_gem_object_put(&bo->bo.base);
	return ret;
}

static void parhelia_local_vertex_fini(struct parhelia_submit_slot *slot)
{
	struct drm_gem_vram_object *bo = slot->local_vertex;

	if (!bo)
		return;
	parhelia_vram_unpin(bo);
	drm_gem_object_put(&bo->bo.base);
	slot->local_vertex = NULL;
}

/* A 0x00 packet holds one register list or, under PARHELIA_PARAM_REGLIST_RUNS,
 * several back to back, each behind its own header.  Step @pos over the next
 * list's header once it reaches @end; false at the end of the packet. */
static inline bool parhelia_reglist_next(const u32 *cmd, u32 length, u32 *pos,
					 u32 *end)
{
	while (*pos >= *end) {
		if (*pos >= length)
			return false;
		*end = *pos + (cmd[*pos] & 0xffff) + 2;
		(*pos)++;
	}
	return *pos < length;
}

/* The draw packets' class-3 stream binds become class 0. */
static void parhelia_local_vertex_streams(u32 *commands,
					  const struct drm_parhelia_packet *packets,
					  u32 count)
{
	u32 i;

	for (i = 0; i < count; i++) {
		u32 *cmd = commands + packets[i].offset;
		u32 pos = 0, end = 0;

		if (cmd[0] >> 24)
			continue;
		while (parhelia_reglist_next(cmd, packets[i].length, &pos, &end)) {
			u32 tag = cmd[pos++];
			unsigned int slot;

			for (slot = 0; slot < 4 && pos < end; slot++) {
				u32 index = (tag >> (slot * 8)) & 0xff;

				if (index == 0x21)
					break;
				if (index == 0 && cmd[pos] == 3)
					cmd[pos] = 0;
				pos++;
			}
		}
	}
}

/* Multi-primitive draws get CALL spans of their own on the P690, and on the
 * original family, whose vendor driver gives every draw its own span too. */
static bool parhelia_split_multi_draws(const struct parhelia_device *pdev)
{
	return pdev->pdev->device == PARHELIA_P690 || parhelia_original_family(pdev);
}

static bool trace_vram_reloc;
module_param(trace_vram_reloc, bool, 0400);
MODULE_PARM_DESC(trace_vram_reloc,
		 "Trace physical and engine addresses patched into render relocations");

static unsigned int async_file_quantum_ms = 16;
module_param(async_file_quantum_ms, uint, 0400);
MODULE_PARM_DESC(async_file_quantum_ms,
		 "Async render queue ownership timeslice in milliseconds (default 16)");

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

/* IRQ status bit 16 is the vendor driver's engine source, but it did not
 * fire for these DWGSYNC packets on P690; keep it an opt-in experiment. */
static bool engine_irq;
module_param(engine_irq, bool, 0400);
MODULE_PARM_DESC(engine_irq,
		 "Wake fence waiters from IRQ status bit 16 (experimental, default false)");

bool parhelia_engine_irq_enabled(void)
{
	return engine_irq;
}

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
	u32 pos = 0, end = 0;

	if (cmd[0] >> 24)
		return result;

	while (parhelia_reglist_next(cmd, length, &pos, &end)) {
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
		    relocs[i].type != PARHELIA_RELOC_CALL_BOUNDARY &&
		    relocs[i].packet == packet)
			return true;
	return false;
}

static int
parhelia_collect_call_boundaries(const struct drm_parhelia_reloc *relocs,
				 u32 reloc_count, u32 packet_count,
				 bool enabled, bool *boundaries,
				 u32 *boundary_count)
{
	u32 count = 0, last = 0, i;

	if (!boundaries || !boundary_count)
		return -EINVAL;
	memset(boundaries, 0, packet_count * sizeof(*boundaries));
	for (i = 0; i < reloc_count; i++) {
		const struct drm_parhelia_reloc *reloc = &relocs[i];

		if (reloc->type != PARHELIA_RELOC_CALL_BOUNDARY)
			continue;
		if (!enabled || !reloc->packet || reloc->packet >= packet_count ||
		    (count && reloc->packet <= last) || reloc->dword ||
		    reloc->handle || reloc->delta || reloc->pad[0] ||
		    reloc->pad[1] || reloc->pad[2])
			return -EINVAL;
		boundaries[reloc->packet] = true;
		last = reloc->packet;
		count++;
	}
	*boundary_count = count;
	return 0;
}

/* The P690 depth test establishes that each triangle needs its own 0x8f
 * hardware packet, not its own secondary CALL. Keep every three-dword packet
 * byte-exact while allowing consecutive packets to share one coherent
 * secondary range. Synthetic stage/fence blocks in the execution plan break
 * a run naturally. Other packet classes stay one-CALL-per-packet until they
 * have an equally direct hardware interlock test. */
static bool parhelia_is_coalescible_range_packet(const u32 *commands,
		const struct drm_parhelia_packet *packet)
{
	return packet->length == 3 && commands[packet->offset] == 0x8f000001;
}

static int
parhelia_packet_span_dwords(const struct drm_parhelia_packet *packets,
			    u32 first, u32 count, u32 *words_out)
{
	u32 i, words = 0;

	for (i = 0; i < count; i++)
		if (check_add_overflow(words, packets[first + i].length,
				       &words))
			return -EOVERFLOW;
	*words_out = words;
	return 0;
}

static void parhelia_coalesce_range_calls(struct parhelia_call_plan *plan,
		const u32 *commands, const struct drm_parhelia_packet *packets,
		u32 packet_count, const bool *call_boundaries)
{
	u32 in = 0, out = 0;

	while (in < plan->count) {
		u32 block = plan->blocks[in];
		u32 run = plan->packet_spans[in];
		u32 words;

		if (block >= packet_count || run != 1 ||
		    !parhelia_is_coalescible_range_packet(commands, &packets[block]))
			goto copy_one;
		words = packets[block].length;
		while (in + run < plan->count) {
			u32 next = plan->blocks[in + run];
			u32 next_words, previous_end;

			if (plan->packet_spans[in + run] != 1 ||
			    next != block + run || next >= packet_count ||
			    (call_boundaries && call_boundaries[next]) ||
			    check_add_overflow(packets[next - 1].offset,
					       packets[next - 1].length,
					       &previous_end) ||
			    packets[next].offset != previous_end ||
			    !parhelia_is_coalescible_range_packet(commands,
							 &packets[next]))
				break;
			next_words = packets[next].length;
			if (words > PARHELIA_DMA_BLOCK_DWORDS - next_words)
				break;
			words += next_words;
			run++;
		}
		plan->blocks[out] = block;
		plan->packet_spans[out] = run;
		out++;
		in += run;
		continue;

copy_one:
		plan->blocks[out] = block;
		plan->packet_spans[out] = plan->packet_spans[in];
		out++;
		in++;
	}
	plan->count = out;
}

/* The P690 stalls when a CALL span holds a 0x97 draw of more than six
 * vertices together with another draw; the vendor gives every draw its own
 * span (parhelia-p690-multi-primitive-packet-hang.md, 2026-10-14). */
#define PARHELIA_SHARED_SPAN_VERTICES	6u

static bool parhelia_multi_draw_packet(const u32 *commands,
				       const struct drm_parhelia_packet *packet)
{
	const u32 *cmd = commands + packet->offset;
	u32 stride, k;

	/* an indexed packet's size field is zero: four bytes per index, two
	 * for the 16-bit form */
	if ((cmd[0] >> 24) != 0x97 && (cmd[0] >> 24) != 0x9f &&
	    (cmd[0] >> 24) != 0x9b)
		return false;
	stride = (cmd[0] >> 24) == 0x9b ? sizeof(u16) :
		 (((cmd[0] >> 17) & 0x3f) + 1) * sizeof(u32);
	/* a relocated indexed end carries the class-3 tag in bits 1:0, a
	 * 16-bit start its halfword skips */
	for (k = 0; k + 3 <= packet->length; k += 3)
		if ((cmd[k + 2] & ~3u) - (cmd[k + 1] & ~3u) >
		    PARHELIA_SHARED_SPAN_VERTICES * stride)
			return true;
	return false;
}

/* Whether a span holding @draws draws, @multi of them large, may take
 * @next_draws more, @next_multi of them large. */
static bool parhelia_span_takes_draws(bool split, u32 draws, bool multi,
				      u32 next_draws, bool next_multi)
{
	return !split || !draws || !next_draws || (!multi && !next_multi);
}

/* Pack only adjacent userspace storage entries already present next to each
 * other in the execution plan. Synthetic fence/stage entries therefore split
 * a run without special cases. Every original packet remains separately
 * validated and parhelia_copy_packet_span() preserves each header byte.
 */
static void parhelia_span_draws(const struct parhelia_call_plan *plan,
				u32 first, u32 count, u32 *draws, bool *multi)
{
	u32 i;

	*draws = 0;
	*multi = false;
	for (i = 0; i < count; i++) {
		const u8 kind = plan->pkt_draw[first + i];

		*draws += kind & PARHELIA_PKT_DRAW;
		*multi |= kind & PARHELIA_PKT_MULTI;
	}
}

static void
parhelia_pack_adjacent_packet_calls(struct parhelia_call_plan *plan,
				    const u32 *commands,
				    const struct drm_parhelia_packet *packets,
				    u32 packet_count,
				    const bool *call_boundaries,
				    bool split_multi_draws, bool inline_draws)
{
	u32 in = 0, out = 0;

	while (in < plan->count) {
		u32 block = plan->blocks[in];
		u32 run = plan->packet_spans[in];
		u32 entries = 1, words, draws;
		bool multi;

		if (block >= packet_count || !run || run > packet_count - block ||
		    parhelia_packet_span_dwords(packets, block, run, &words) ||
		    words > PARHELIA_DMA_BLOCK_DWORDS)
			goto copy_one;
		parhelia_span_draws(plan, block, run, &draws, &multi);
		while (in + entries < plan->count) {
			u32 next = plan->blocks[in + entries];
			u32 next_span = plan->packet_spans[in + entries];
			u32 next_words, previous_end, next_draws;
			bool next_multi;

			if (next >= packet_count || !next_span ||
			    next_span > packet_count - next || next != block + run ||
			    (call_boundaries && call_boundaries[next]) ||
			    check_add_overflow(packets[next - 1].offset,
					       packets[next - 1].length,
					       &previous_end) ||
			    packets[next].offset != previous_end ||
			    parhelia_packet_span_dwords(packets, next, next_span,
							&next_words) ||
			    next_words > PARHELIA_DMA_BLOCK_DWORDS - words ||
			    run > U16_MAX - next_span)
				break;
			parhelia_span_draws(plan, next, next_span,
					    &next_draws, &next_multi);
			if (!parhelia_span_takes_draws(split_multi_draws, draws, multi,
						       next_draws, next_multi))
				break;
			/* an inline draw's vertices go right before its header */
			if (inline_draws && next_multi)
				break;
			draws += next_draws;
			multi |= next_multi;
			words += next_words;
			run += next_span;
			entries++;
		}
		plan->blocks[out] = block;
		plan->packet_spans[out] = run;
		out++;
		in += entries;
		continue;

copy_one:
		plan->blocks[out] = block;
		plan->packet_spans[out] = plan->packet_spans[in];
		out++;
		in++;
	}
	plan->count = out;
}

/* This is the first narrow extension beyond the proven 0x8f range packer.
 * Requiring the relocation bitmap means no raw 0x97 address pair can become
 * part of a shared CALL, even though the later submit audit is also defensive.
 */
static void
parhelia_coalesce_draw_calls(struct parhelia_call_plan *plan,
			     const u32 *commands,
			     const struct drm_parhelia_packet *packets,
			     const bool *draw_relocated, u32 packet_count,
			     bool split_multi_draws)
{
	u32 in = 0, out = 0;

	while (in < plan->count) {
		u32 block = plan->blocks[in];
		u32 run = plan->packet_spans[in];
		u32 words;
		bool multi;

		if (block >= packet_count || run != 1 || !draw_relocated[block] ||
		    packets[block].length != 3 ||
		    (commands[packets[block].offset] >> 24) != 0x97)
			goto copy_one;
		words = packets[block].length;
		multi = parhelia_multi_draw_packet(commands, &packets[block]);
		while (in + run < plan->count) {
			u32 next = plan->blocks[in + run];
			u32 previous_end;

			if (plan->packet_spans[in + run] != 1 ||
			    next != block + run || next >= packet_count ||
			    !draw_relocated[next] || packets[next].length != 3 ||
			    !parhelia_span_takes_draws(split_multi_draws, run, multi, 1,
					parhelia_multi_draw_packet(commands,
								   &packets[next])) ||
			    (commands[packets[next].offset] >> 24) != 0x97 ||
			    check_add_overflow(packets[next - 1].offset,
					       packets[next - 1].length,
					       &previous_end) ||
			    packets[next].offset != previous_end ||
			    words > PARHELIA_DMA_BLOCK_DWORDS - packets[next].length)
				break;
			words += packets[next].length;
			run++;
		}
		plan->blocks[out] = block;
		plan->packet_spans[out] = run;
		out++;
		in += run;
		continue;

copy_one:
		plan->blocks[out] = block;
		plan->packet_spans[out] = plan->packet_spans[in];
		out++;
		in++;
	}
	plan->count = out;
}

/* Upper bound of the vertex dwords inline draws may copy into one request:
 * host draws by their relocation, draw-range ones by their operands. */
#define PARHELIA_MAX_INLINE_DWORDS	(256u * 1024u)

static u32 parhelia_inline_draw_bound(const u32 *commands,
				      const struct drm_parhelia_packet *packets,
				      u32 packet_count,
				      const struct drm_parhelia_reloc *relocs,
				      u32 reloc_count)
{
	u64 total = 0;
	u32 i;

	for (i = 0; i < reloc_count; i++)
		if (relocs[i].type == PARHELIA_RELOC_DRAW)
			total += relocs[i].delta / sizeof(u32);
	for (i = 0; i < packet_count; i++) {
		const u32 *cmd = commands + packets[i].offset;

		if (packets[i].length == 3 && (cmd[0] >> 24) == 0x97 &&
		    cmd[2] > cmd[1])
			total += (cmd[2] - cmd[1]) / sizeof(u32);
	}
	return total > PARHELIA_MAX_INLINE_DWORDS ? 0 : total;
}

/* The CPU copy of a span's opening 0x97 draw, or NULL when the span cannot
 * take its vertices inline. */
static const void *parhelia_inline_draw_source(const u32 *commands,
		const struct drm_parhelia_packet *packet, bool relocated,
		const void *host_cpu, u32 host_bus, u32 host_bytes,
		const u8 *ranges_cpu, u32 ranges_bus, u32 ranges_bytes,
		u32 *dwords)
{
	const u32 *cmd = commands + packet->offset;
	const u32 bytes = cmd[2] - cmd[1];

	if (packet->length != 3 || (cmd[0] >> 24) != 0x97 || !relocated ||
	    cmd[2] <= cmd[1] || (bytes & 3))
		return NULL;
	*dwords = bytes / sizeof(u32);
	if (ranges_cpu && cmd[1] >= ranges_bus &&
	    cmd[2] - ranges_bus <= ranges_bytes)
		return ranges_cpu + (cmd[1] - ranges_bus);
	if (host_cpu && cmd[1] >= host_bus && cmd[2] - host_bus <= host_bytes)
		return (const u8 *)host_cpu + (cmd[1] - host_bus);
	return NULL;
}

static u32 parhelia_copy_packet_span(u32 *dst, const u32 *commands,
		const struct drm_parhelia_packet *packets, u32 first, u32 count)
{
	u32 i, run, words = 0;

	/* Copy each individually validated packet. This never relies on gaps in a
	 * caller-provided offset table containing harmless command data; packets
	 * that adjoin are copied as one run. */
	for (i = 0; i < count; i += run) {
		u32 offset = packets[first + i].offset;
		u32 length = packets[first + i].length;

		for (run = 1; i + run < count &&
		     packets[first + i + run].offset == offset + length; run++)
			length += packets[first + i + run].length;
		memcpy(dst + words, commands + offset, length * sizeof(*commands));
		words += length;
	}
	return words;
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
		plan->blocks[out] = block;
		plan->packet_spans[out] = plan->packet_spans[i];
		out++;
	}
	plan->count = out;
}

static int parhelia_range_call_static_selftest(void)
{
	const struct drm_parhelia_packet gapped_packets[] = {
		{ .offset = 0, .length = 3 },
		{ .offset = 4, .length = 3 },
	};
	const struct drm_parhelia_packet word_limit_packets[] = {
		{ .offset = 0, .length = 3000 },
		{ .offset = 3000, .length = 2000 },
	};
	const u32 gapped_commands[] = {
		0x8f000001, 0, 3, 0xdeadbeef,
		0x8f000001, 3, 6,
	};
	const struct drm_parhelia_packet generic_packets[] = {
		{ .offset = 0, .length = 3 },
		{ .offset = 3, .length = 3 },
		{ .offset = 6, .length = 8 },
		{ .offset = 14, .length = 3 },
		{ .offset = 17, .length = 3 },
		{ .offset = 20, .length = 3 },
	};
	const struct drm_parhelia_packet alternating_packets[] = {
		{ .offset = 0, .length = 3 },
		{ .offset = 3, .length = 3 },
		{ .offset = 6, .length = 8 },
		{ .offset = 14, .length = 3 },
		{ .offset = 17, .length = 3 },
		{ .offset = 20, .length = 3 },
		{ .offset = 23, .length = 3 },
		{ .offset = 26, .length = 3 },
		{ .offset = 29, .length = 8 },
		{ .offset = 37, .length = 3 },
		{ .offset = 40, .length = 3 },
		{ .offset = 43, .length = 3 },
	};
	struct drm_parhelia_reloc call_relocs[] = {
		{ .packet = 6, .type = PARHELIA_RELOC_CALL_BOUNDARY },
	};
	bool logical_boundaries[ARRAY_SIZE(alternating_packets)] = { };
	u32 generic_commands[] = {
		1, 0x212121c9, 0xf,
		1, 0x212121ac, 0x3fff8842,
		6, 0x212121dd, 0, 0x3e403c3c,
		0x20000002, 0x20000002, 0, 2,
		1, 0x212121b2, 7,
		0x97080001, 0x11111003, 0x1111103f,
		0x97080001, 0x22222003, 0x2222203f,
	};
	const struct drm_parhelia_packet packets[] = {
		{ .offset = 0, .length = 3 },
		{ .offset = 3, .length = 3 },
		{ .offset = 6, .length = 3 },
		{ .offset = 9, .length = 3 },
		{ .offset = 12, .length = 3 },
	};
	u32 commands[] = {
		1, 0x212121ac, 0x3fff8842,
		0x8f000001, 0, 3,
		0x8f000001, 3, 6,
		0x8f000001, 6, 9,
		0x97080001, 0x11111003, 0x1111103f,
	};
	u32 copied[9] = { };
	struct parhelia_call_plan *plan;
	int ret = -EINVAL;

	plan = kvzalloc_obj(*plan, GFP_KERNEL);
	if (!plan)
		return -ENOMEM;
	/* A required stage after packet 2 must split the two 0x8f runs. */
	plan->stage_modes[2] = 1;
	if (parhelia_plan_calls(plan, ARRAY_SIZE(packets), 0, 0))
		goto out;
	parhelia_coalesce_range_calls(plan, commands, packets,
		ARRAY_SIZE(packets), NULL);
	if (plan->count != 8 || plan->blocks[0] != 5 ||
	    plan->packet_spans[0] || plan->blocks[1] != 0 ||
	    plan->packet_spans[1] != 1 || plan->blocks[2] != 1 ||
	    plan->packet_spans[2] != 2 || plan->blocks[3] != 8 ||
	    plan->packet_spans[3] || plan->blocks[4] != 3 ||
	    plan->packet_spans[4] != 1 || plan->blocks[5] != 4 ||
	    plan->packet_spans[5] != 1 || plan->blocks[6] != 6 ||
	    plan->packet_spans[6] || plan->blocks[7] != 7 ||
	    plan->packet_spans[7])
		goto out;

	/* Relocation patching happens after planning. The eventual bulk copy must
	 * therefore contain the patched operands while retaining both headers. */
	commands[7] = 0x22222003;
	commands[8] = 0x2222203f;
	if (parhelia_copy_packet_span(copied, commands, packets,
				       plan->blocks[2], plan->packet_spans[2]) != 6 ||
	    memcmp(copied, commands + 3, 6 * sizeof(*copied)) ||
	    copied[0] != 0x8f000001 || copied[3] != 0x8f000001 ||
	    copied[4] != 0x22222003 || copied[5] != 0x2222203f)
		goto out;

	/* A logical state boundary keeps the runs on its two sides separate and
	 * leaves both opening/EOB/drain triples in their original order. */
	commands[0] = 0x8f000001;
	commands[1] = 0;
	commands[2] = 3;
	memset(plan, 0, sizeof(*plan));
	if (parhelia_plan_calls_stateful(plan, 4, 2, 0, 2))
		goto out;
	parhelia_coalesce_range_calls(plan, commands, packets, 4, NULL);
	if (plan->count != 8 || plan->blocks[0] != 4 ||
	    plan->packet_spans[0] || plan->blocks[1] != 0 ||
	    plan->packet_spans[1] != 2 || plan->blocks[2] != 5 ||
	    plan->packet_spans[2] || plan->blocks[3] != 6 ||
	    plan->packet_spans[3] || plan->blocks[4] != 7 ||
	    plan->packet_spans[4] || plan->blocks[5] != 2 ||
	    plan->packet_spans[5] != 2 || plan->blocks[6] != 8 ||
	    plan->packet_spans[6] || plan->blocks[7] != 9 ||
	    plan->packet_spans[7])
		goto out;

	/* Plan compaction is independently defensive about offsets. A gap never
	 * becomes unvalidated content in a shared secondary CALL. */
	memset(plan, 0, sizeof(*plan));
	parhelia_plan_append(plan, 0, 1);
	parhelia_plan_append(plan, 1, 1);
	parhelia_coalesce_range_calls(plan, gapped_commands, gapped_packets, 2,
				      NULL);
	if (plan->count != 2 || plan->blocks[0] || plan->packet_spans[0] != 1 ||
	    plan->blocks[1] != 1 || plan->packet_spans[1] != 1)
		goto out;
	parhelia_pack_adjacent_packet_calls(plan, NULL, gapped_packets, 2, NULL,
					    false, false);
	if (plan->count != 2 || plan->blocks[0] || plan->packet_spans[0] != 1 ||
	    plan->blocks[1] != 1 || plan->packet_spans[1] != 1)
		goto out;

	/* Contiguity is insufficient when the resulting secondary CALL would
	 * exceed the independently enforced 4096-dword fetch limit.
	 */
	memset(plan, 0, sizeof(*plan));
	parhelia_plan_append(plan, 0, 1);
	parhelia_plan_append(plan, 1, 1);
	parhelia_pack_adjacent_packet_calls(plan, NULL, word_limit_packets, 2,
					    NULL, false, false);
	if (plan->count != 2 || plan->blocks[0] || plan->packet_spans[0] != 1 ||
	    plan->blocks[1] != 1 || plan->packet_spans[1] != 1)
		goto out;

	/* Match a complete userspace state section: the opening watermark splits
	 * its prefix, the WARP bind remains at the end of its packet span and is
	 * immediately followed by the synthetic stage CALL, and ordinary state
	 * plus both relocated-draw packet shapes share the final span.
	 */
	memset(plan, 0, sizeof(*plan));
	plan->stage_modes[2] = 1;
	if (parhelia_plan_calls(plan, ARRAY_SIZE(generic_packets), 1, false))
		goto out;
	parhelia_pack_adjacent_packet_calls(plan, NULL, generic_packets,
				    ARRAY_SIZE(generic_packets), NULL, false, false);
	if (plan->count != 7 || plan->blocks[0] != 0 ||
	    plan->packet_spans[0] != 1 ||
	    plan->blocks[1] != ARRAY_SIZE(generic_packets) ||
	    plan->packet_spans[1] || plan->blocks[2] != 1 ||
	    plan->packet_spans[2] != 2 ||
	    plan->blocks[3] != ARRAY_SIZE(generic_packets) + 3 ||
	    plan->packet_spans[3] || plan->blocks[4] != 3 ||
	    plan->packet_spans[4] != 3 ||
	    plan->blocks[5] != ARRAY_SIZE(generic_packets) + 1 ||
	    plan->packet_spans[5] ||
	    plan->blocks[6] != ARRAY_SIZE(generic_packets) + 2 ||
	    plan->packet_spans[6])
		goto out;
	generic_commands[18] = 0x33333003;
	generic_commands[19] = 0x3333303f;
	if (parhelia_copy_packet_span(copied, generic_commands, generic_packets,
				      plan->blocks[4], plan->packet_spans[4]) != 9 ||
	    memcmp(copied, generic_commands + 14, sizeof(copied)) ||
	    copied[3] != 0x97080001 || copied[4] != 0x33333003 ||
	    copied[6] != 0x97080001 || copied[7] != 0x22222003)
		goto out;

	/*
	 * Two alternating logical state batches share the submit's terminal
	 * bracket, but never the secondary CALL spanning draw A to state B.
	 */
	{
		u32 call_boundary_count;

		if (parhelia_collect_call_boundaries(call_relocs,
						     ARRAY_SIZE(call_relocs),
						     ARRAY_SIZE(alternating_packets), true,
						     logical_boundaries,
						     &call_boundary_count) ||
		    call_boundary_count != 1 || !logical_boundaries[6])
			goto out;
		memset(plan, 0, sizeof(*plan));
		plan->stage_modes[2] = 1;
		plan->stage_modes[8] = 2;
		if (parhelia_plan_calls(plan, ARRAY_SIZE(alternating_packets), 1,
					false))
			goto out;
		parhelia_pack_adjacent_packet_calls(plan, NULL, alternating_packets,
						    ARRAY_SIZE(alternating_packets),
						    logical_boundaries, false, false);
		if (plan->count != 10 || plan->blocks[0] != 0 ||
		    plan->packet_spans[0] != 1 ||
		    plan->blocks[1] != ARRAY_SIZE(alternating_packets) ||
		    plan->packet_spans[1] || plan->blocks[2] != 1 ||
		    plan->packet_spans[2] != 2 ||
		    plan->blocks[3] != ARRAY_SIZE(alternating_packets) + 3 ||
		    plan->packet_spans[3] || plan->blocks[4] != 3 ||
		    plan->packet_spans[4] != 3 || plan->blocks[5] != 6 ||
		    plan->packet_spans[5] != 3 ||
		    plan->blocks[6] != ARRAY_SIZE(alternating_packets) + 4 ||
		    plan->packet_spans[6] || plan->blocks[7] != 9 ||
		    plan->packet_spans[7] != 3 ||
		    plan->blocks[8] != ARRAY_SIZE(alternating_packets) + 1 ||
		    plan->packet_spans[8] ||
		    plan->blocks[9] != ARRAY_SIZE(alternating_packets) + 2 ||
		    plan->packet_spans[9])
			goto out;

		call_relocs[0].packet = 0;
		if (!parhelia_collect_call_boundaries(call_relocs,
						      ARRAY_SIZE(call_relocs),
						      ARRAY_SIZE(alternating_packets), true,
						      logical_boundaries,
						      &call_boundary_count))
			goto out;
		call_relocs[0].packet = 6;
		if (!parhelia_collect_call_boundaries(call_relocs,
						      ARRAY_SIZE(call_relocs),
						      ARRAY_SIZE(alternating_packets), false,
						      logical_boundaries,
						      &call_boundary_count))
			goto out;
	}
	ret = 0;
out:
	kvfree(plan);
	return ret;
}

static int parhelia_large_range_call_static_selftest(void)
{
	struct drm_parhelia_packet *packets;
	struct parhelia_call_plan *plan;
	bool *draw_relocated;
	u32 *commands;
	u32 i, words;
	int ret = -EINVAL;

	packets = kvmalloc_array(PARHELIA_USER_PACKET_CAPACITY,
				 sizeof(*packets), GFP_KERNEL);
	commands = kvmalloc_array(PARHELIA_USER_PACKET_CAPACITY * 3u,
				  sizeof(*commands), GFP_KERNEL);
	draw_relocated = kvcalloc(PARHELIA_USER_PACKET_CAPACITY,
				  sizeof(*draw_relocated), GFP_KERNEL);
	plan = kvzalloc_obj(*plan, GFP_KERNEL);
	if (!packets || !commands || !draw_relocated || !plan) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_USER_PACKET_CAPACITY; i++) {
		packets[i].offset = i * 3u;
		packets[i].length = 3;
		commands[i * 3u] = 0x8f000001;
		commands[i * 3u + 1] = i * 3u;
		commands[i * 3u + 2] = i * 3u + 3u;
	}

	/* The logical plan may exceed the hardware CALL table only while it is
	 * still eligible for validated adjacent-range compaction.
	 */
	if (parhelia_plan_calls(plan, PARHELIA_USER_PACKET_CAPACITY, 0, false) ||
	    plan->count != PARHELIA_USER_PACKET_CAPACITY + 3u ||
	    !parhelia_plan_fits_hardware(plan))
		goto out;
	parhelia_coalesce_range_calls(plan, commands, packets,
				      PARHELIA_USER_PACKET_CAPACITY, NULL);
	if (plan->count != 4 || plan->blocks[0] != PARHELIA_USER_PACKET_CAPACITY ||
	    plan->packet_spans[0] || plan->blocks[1] ||
	    plan->packet_spans[1] != PARHELIA_USER_PACKET_CAPACITY ||
	    plan->blocks[2] != PARHELIA_USER_PACKET_CAPACITY + 1u ||
	    plan->packet_spans[2] ||
	    plan->blocks[3] != PARHELIA_USER_PACKET_CAPACITY + 2u ||
	    plan->packet_spans[3] || parhelia_plan_fits_hardware(plan))
		goto out;
	if (parhelia_packet_span_dwords(packets, plan->blocks[1],
					plan->packet_spans[1], &words) ||
	    words != PARHELIA_USER_PACKET_CAPACITY * 3u ||
	    words > PARHELIA_DMA_BLOCK_DWORDS)
		goto out;

	/* Non-range packets do not gain admission merely because the userspace
	 * descriptor table is larger: the hardware plan stays capped at 224.
	 */
	for (i = 0; i < PARHELIA_USER_PACKET_CAPACITY; i++)
		commands[i * 3u] = 0x97000001;
	memset(plan, 0, sizeof(*plan));
	if (parhelia_plan_calls(plan, PARHELIA_USER_PACKET_CAPACITY, 0, false))
		goto out;
	parhelia_coalesce_range_calls(plan, commands, packets,
				      PARHELIA_USER_PACKET_CAPACITY, NULL);
	if (plan->count != PARHELIA_USER_PACKET_CAPACITY + 3u ||
	    !parhelia_plan_fits_hardware(plan))
		goto out;
	memset(draw_relocated, 1,
	       PARHELIA_USER_PACKET_CAPACITY * sizeof(*draw_relocated));
	parhelia_coalesce_draw_calls(plan, commands, packets, draw_relocated,
				     PARHELIA_USER_PACKET_CAPACITY, false);
	if (plan->count != 4 || plan->blocks[0] != PARHELIA_USER_PACKET_CAPACITY ||
	    plan->packet_spans[0] || plan->blocks[1] ||
	    plan->packet_spans[1] != PARHELIA_USER_PACKET_CAPACITY ||
	    plan->blocks[2] != PARHELIA_USER_PACKET_CAPACITY + 1u ||
	    plan->packet_spans[2] ||
	    plan->blocks[3] != PARHELIA_USER_PACKET_CAPACITY + 2u ||
	    plan->packet_spans[3] || parhelia_plan_fits_hardware(plan))
		goto out;

	/* One unrelocated draw splits the two safe spans and stays a standalone
	 * CALL; the production validation will subsequently reject that packet.
	 */
	draw_relocated[PARHELIA_USER_PACKET_CAPACITY / 2u] = false;
	memset(plan, 0, sizeof(*plan));
	if (parhelia_plan_calls(plan, PARHELIA_USER_PACKET_CAPACITY, 0, false))
		goto out;
	parhelia_coalesce_draw_calls(plan, commands, packets, draw_relocated,
				     PARHELIA_USER_PACKET_CAPACITY, false);
	if (plan->count != 6 || plan->blocks[1] ||
	    plan->packet_spans[1] != PARHELIA_USER_PACKET_CAPACITY / 2u ||
	    plan->blocks[2] != PARHELIA_USER_PACKET_CAPACITY / 2u ||
	    plan->packet_spans[2] != 1 ||
	    plan->blocks[3] != PARHELIA_USER_PACKET_CAPACITY / 2u + 1u ||
	    plan->packet_spans[3] != PARHELIA_USER_PACKET_CAPACITY / 2u - 1u)
		goto out;
	draw_relocated[PARHELIA_USER_PACKET_CAPACITY / 2u] = true;

	memset(plan, 0, sizeof(*plan));
	if (parhelia_plan_calls(plan, PARHELIA_USER_PACKET_CAPACITY, 0, false))
		goto out;
	parhelia_pack_adjacent_packet_calls(plan, NULL, packets,
					    PARHELIA_USER_PACKET_CAPACITY, NULL,
					    false, false);
	if (plan->count != 4 || plan->blocks[0] != PARHELIA_USER_PACKET_CAPACITY ||
	    plan->packet_spans[0] || plan->blocks[1] ||
	    plan->packet_spans[1] != PARHELIA_USER_PACKET_CAPACITY ||
	    plan->blocks[2] != PARHELIA_USER_PACKET_CAPACITY + 1u ||
	    plan->packet_spans[2] ||
	    plan->blocks[3] != PARHELIA_USER_PACKET_CAPACITY + 2u ||
	    plan->packet_spans[3] || parhelia_plan_fits_hardware(plan))
		goto out;

	memset(plan, 0, sizeof(*plan));
	plan->stage_modes[PARHELIA_USER_PACKET_CAPACITY / 2u - 1u] = 1;
	if (parhelia_plan_calls(plan, PARHELIA_USER_PACKET_CAPACITY, 0, false))
		goto out;
	parhelia_coalesce_draw_calls(plan, commands, packets, draw_relocated,
				     PARHELIA_USER_PACKET_CAPACITY, false);
	if (plan->count != 6 || plan->blocks[0] != PARHELIA_USER_PACKET_CAPACITY ||
	    plan->packet_spans[0] || plan->blocks[1] ||
	    plan->packet_spans[1] != PARHELIA_USER_PACKET_CAPACITY / 2u ||
	    plan->blocks[2] != PARHELIA_USER_PACKET_CAPACITY + 3u ||
	    plan->packet_spans[2] ||
	    plan->blocks[3] != PARHELIA_USER_PACKET_CAPACITY / 2u ||
	    plan->packet_spans[3] != PARHELIA_USER_PACKET_CAPACITY / 2u ||
	    plan->blocks[4] != PARHELIA_USER_PACKET_CAPACITY + 1u ||
	    plan->packet_spans[4] ||
	    plan->blocks[5] != PARHELIA_USER_PACKET_CAPACITY + 2u ||
	    plan->packet_spans[5] || parhelia_plan_fits_hardware(plan))
		goto out;

	memset(plan, 0, sizeof(*plan));
	if (!parhelia_plan_calls(plan, PARHELIA_USER_PACKET_CAPACITY + 1u,
				 0, false))
		goto out;
	ret = 0;
out:
	kvfree(plan);
	kvfree(draw_relocated);
	kvfree(commands);
	kvfree(packets);
	return ret;
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

	test = kvzalloc_obj(*test, GFP_KERNEL);
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
	kvfree(test);
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

/* What a register-list index may carry, one lookup per written register. */
enum parhelia_reg_class {
	PARHELIA_REG_STATE,
	PARHELIA_REG_END,		/* 0x21 pads the tag */
	PARHELIA_REG_INVALID,
	PARHELIA_REG_KERNEL,
	PARHELIA_REG_VRAM,
	PARHELIA_REG_WARP_ENTRY,
	PARHELIA_REG_DESC_BURST,	/* rebind tracking */
	PARHELIA_REG_DESC_ATTR,
	PARHELIA_REG_DWGCTL,		/* restored by the terminal drain */
	PARHELIA_REG_STREAM0,		/* indexed draws: direct-fetch state */
	PARHELIA_REG_STRIDE01,
	PARHELIA_REG_VTX_LIMIT,
	PARHELIA_REG_VTX_FLOOR,
};

static const u8 parhelia_reg_class[256] = {
	[0x00] = PARHELIA_REG_STREAM0,
	[0x10] = PARHELIA_REG_STRIDE01,
	[0x18] = PARHELIA_REG_VTX_LIMIT, [0x19] = PARHELIA_REG_VTX_FLOOR,
	[0x21] = PARHELIA_REG_END,
	/* DMA addresses and the global sync counter are kernel-owned.
	 * Render-node clients only provide state values and relocatable
	 * VRAM operands; primary-ring and writeback addresses remain
	 * entirely kernel-owned. */
	[0x24] = PARHELIA_REG_KERNEL, [0x25] = PARHELIA_REG_KERNEL,
	[0x28] = PARHELIA_REG_KERNEL, [0x29] = PARHELIA_REG_KERNEL,
	[0xaf] = PARHELIA_REG_KERNEL,
	[0x2d] = PARHELIA_REG_DESC_BURST, [0x2e] = PARHELIA_REG_DESC_ATTR,
	[0xc0] = PARHELIA_REG_DWGCTL,
	/* The WARP entry (0x1100, 0x1104) is a class-3 instruction-fetch
	 * address.  A client that may write it freely can point the vertex
	 * engine at arbitrary system memory and have the resulting program
	 * write the surface-base block, so only entries this kernel handed out
	 * are accepted.  See docs/parhelia-p690-warp-isa-re.md section 9.3. */
	[0x40] = PARHELIA_REG_WARP_ENTRY, [0x41] = PARHELIA_REG_WARP_ENTRY,
	/* Texture base and the four colour/depth surface base slots. */
	[0x77] = PARHELIA_REG_VRAM,
	[0x88] = PARHELIA_REG_VRAM, [0x8a] = PARHELIA_REG_VRAM,
	[0x8c] = PARHELIA_REG_VRAM, [0x8e] = PARHELIA_REG_VRAM,
	/* 0x138c (index e3) is the documented 2D fill extent/trigger used by
	 * p3d_emit_clear; it lies above the WARP state window.  The remaining
	 * high indices are rejected. */
	[0xe0 ... 0xe2] = PARHELIA_REG_INVALID,
	[0xe4 ... 0xff] = PARHELIA_REG_INVALID,
};

static bool parhelia_vram_register(u32 index)
{
	return parhelia_reg_class[index] == PARHELIA_REG_VRAM;
}

/* A stream whose descriptors fetch no colour (attribute kind 5) leaves WARP
 * input v5 at the previous draw's value, from any client.  warp_textured and
 * warp_textured_multitex read it, so a stale colour with a channel above its
 * alpha made the setup unit reject such draws.  For every draw whose bound
 * entry is one of them and whose descriptor burst has no colour, rewrite the
 * entry writes that bound it to the no-colour variant, unless the same
 * entry writes also serve a draw that does fetch a colour.  Entries bound in
 * an earlier submit are left as they are.  Tracked by the validation pass. */
struct parhelia_entry_bind {
	u32 pos[2];
	u32 count;
	u32 entry;
	bool no_colour, colour;
};

struct parhelia_rebind {
	struct parhelia_entry_bind bind;
	u32 textured, multitex;
	bool colour, descriptors;
};

/* What the validation pass collects on its way through the request. */
/* The direct-fetch state an indexed (0x9f) packet draws with. */
struct parhelia_indexed_draw {
	u32 packet;
	u32 stream0, stream0_pos;	/* raw base operand and its dword */
	u32 stride, limit, floor;
	u32 extent;			/* bytes the descriptors read per vertex */
	u32 index_bytes;		/* 4 for 0x9f, 2 for 0x9b */
	bool stream0_seen, other_stream;
};

struct parhelia_validate_state {
	struct parhelia_rebind *rb;	/* P690 only */
	u32 packet, packet_count;
	u32 dwgctl, dwgctl_packet;
	bool dwgctl_seen;
	struct parhelia_indexed_draw fetch;	/* current direct-fetch state */
	unsigned long *vram_ops;	/* command dwords that are VRAM operands */
	struct parhelia_indexed_draw *indexed;	/* one per 0x9f packet */
	u32 indexed_count, indexed_cap;
	u32 faa_fences, faa_first;	/* P690 FAA sync packets, the first one */
};

static void parhelia_rebind_apply(struct parhelia_device *pdev, u32 *commands,
				  const struct parhelia_entry_bind *bind)
{
	u32 to, i;

	if (!bind->count || !bind->no_colour || bind->colour)
		return;
	if (bind->entry == parhelia_warp_entry(pdev, PARHELIA_WARP_TEXTURED_ENTRY))
		to = parhelia_warp_entry(pdev, PARHELIA_WARP_TEXTURED_NO_COLOUR_ENTRY);
	else
		to = parhelia_warp_entry(pdev,
				PARHELIA_WARP_TEXTURED_MULTITEX_NO_COLOUR_ENTRY);
	for (i = 0; i < bind->count; i++)
		commands[bind->pos[i]] = to;
}

static int parhelia_validate_reglist(struct parhelia_device *pdev,
				     struct drm_file *file, u32 *commands,
				     u32 offset, u32 length,
				     struct parhelia_validate_state *vs)
{
	struct parhelia_rebind *rb = vs->rb;
	const u32 *cmd = commands + offset;
	u32 pos = 1, end = (cmd[0] & 0xffff) + 2;

	if (end > length)
		return -EINVAL;
	while (pos < length) {
		u32 tag;
		unsigned int slot;

		/* the next list of a run: a plain register list header */
		if (pos == end) {
			if (cmd[pos] >> 16 ||
			    (cmd[pos] & 0xffff) + 2 > length - pos)
				return -EINVAL;
			end = pos + (cmd[pos] & 0xffff) + 2;
			pos++;
			continue;
		}
		tag = cmd[pos++];
		/* four plain state writes: nothing to check or track */
		if (end - pos >= 4 &&
		    !(parhelia_reg_class[tag & 0xff] |
		      parhelia_reg_class[(tag >> 8) & 0xff] |
		      parhelia_reg_class[(tag >> 16) & 0xff] |
		      parhelia_reg_class[tag >> 24])) {
			pos += 4;
			continue;
		}
		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;
			u8 class = parhelia_reg_class[index];
			u32 value;

			if (class == PARHELIA_REG_END)
				break;
			if (pos >= end)
				return -EINVAL;
			if (class == PARHELIA_REG_STATE) {
				pos++;
				continue;
			}
			value = cmd[pos];
			switch (class) {
			case PARHELIA_REG_INVALID:
				return -EINVAL;
			case PARHELIA_REG_KERNEL:
				return -EACCES;
			case PARHELIA_REG_VRAM:
				/* This operand must be populated by a GEM relocation. */
				if (value)
					return -EACCES;
				if (vs->vram_ops)
					__set_bit(offset + pos, vs->vram_ops);
				break;
			case PARHELIA_REG_WARP_ENTRY:
				if (!parhelia_warp_entry_allowed(pdev, file, value))
					return -EACCES;
				if (!rb)
					break;
				if (value != rb->bind.entry || rb->bind.count >= 2) {
					parhelia_rebind_apply(pdev, commands, &rb->bind);
					rb->bind = (struct parhelia_entry_bind){
						.entry = value };
				}
				rb->bind.pos[rb->bind.count++] = offset + pos;
				break;
			case PARHELIA_REG_DESC_BURST:
				if (rb) {
					rb->colour = false;
					rb->descriptors = true;
				}
				vs->fetch.extent = 0;
				vs->fetch.other_stream = false;
				break;
			case PARHELIA_REG_DESC_ATTR:
				if (rb && value && ((value >> 16) & 0xf) == 5)
					rb->colour = true;
				if (value) {
					u32 extent = ((value >> 4) & 0xff) * 4 +
						     hweight8((value >> 22) & 0xf) * 4;

					vs->fetch.other_stream |= !!(value & 7);
					vs->fetch.extent = max(vs->fetch.extent, extent);
				}
				break;
			case PARHELIA_REG_STREAM0:
				vs->fetch.stream0 = value;
				vs->fetch.stream0_pos = offset + pos;
				vs->fetch.stream0_seen = true;
				break;
			case PARHELIA_REG_STRIDE01:
				vs->fetch.stride = value & 0xffff;
				break;
			case PARHELIA_REG_VTX_LIMIT:
				vs->fetch.limit = value;
				break;
			case PARHELIA_REG_VTX_FLOOR:
				vs->fetch.floor = value;
				break;
			case PARHELIA_REG_DWGCTL:
				vs->dwgctl = value;
				vs->dwgctl_packet = vs->packet;
				vs->dwgctl_seen = true;
				break;
			}
			pos++;
		}
	}

	return pos == length ? 0 : -EINVAL;
}

/* Sync operations the P690 vendor driver brackets with a counter pair while
 * FAA is armed (mtxpserd FAA arm, resolve stamps and fragment flush): plain
 * 4, the 0x400 flush and the 0x100/0x200/0x300 pixel-pipe drains. */
static bool parhelia_faa_sync_op(u32 value)
{
	return value == 0x004 || value == 0x107 || value == 0x207 ||
	       value == 0x307 || value == 0x407;
}

/* A P690 FAA sync packet: one register list of sync DWGCTL writes, a 0x12bc
 * counter pair the kernel fills (both placeholders zero), then at most one
 * DWGCTL resume or MemLevels write.  @tags receives the dword offsets of the
 * two counters, @resume that of the resume write or 0. */
static bool parhelia_faa_fence_form(const u32 *cmd, u32 length, u32 tags[2],
				    u32 *resume)
{
	u32 pos = 1, end, writes = 0, counters = 0, after = 0;
	bool prev_counter = false;

	*resume = 0;
	if (length < 4 || cmd[0] >> 16 || (cmd[0] & 0xffff) + 2 != length)
		return false;
	end = length;
	while (pos < end) {
		u32 tag = cmd[pos++], slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;

			if (index == 0x21)
				break;
			if (pos >= end)
				return false;
			if (index == 0xaf) {
				/* the pair is adjacent and preceded by a sync */
				if (cmd[pos] || counters == 2 || !writes ||
				    (counters && !prev_counter))
					return false;
				tags[counters++] = pos;
			} else if (counters == 2) {
				if ((index != 0xc0 && index != 0xac) || after++)
					return false;
				if (index == 0xc0)
					*resume = pos;
			} else if (index != 0xc0 || counters ||
				   !parhelia_faa_sync_op(cmd[pos])) {
				return false;
			}
			if (index != 0xaf)
				writes++;
			prev_counter = index == 0xaf;
			pos++;
		}
	}
	return counters == 2;
}

/* With replay fences the opening leaves begin + 8 completed, and a status
 * page tag T|1 reads as T + 8 completed.  FAA sync k arms begin + 8 + 8k,
 * each armed tag at most 8 ahead of the completed one (the 0528's measured
 * limit), and the drain pads with up to two counter pairs so that it arms
 * finish - 8: the request's fence then stays on the 24-tag cadence of
 * waitable fences and is what the status page reports. */
static u32 parhelia_faa_fence_pads(u32 faa_fences)
{
	return (3 - faa_fences % 3) % 3;
}

static u32 parhelia_faa_fence_step(u32 faa_fences)
{
	return 16 + 8 * (faa_fences + parhelia_faa_fence_pads(faa_fences));
}

/* The drain words for @faa_fences sync packets, at most 11. */
static u32 parhelia_faa_drain_words(u32 faa_fences)
{
	u32 writes = 4 + 2 * parhelia_faa_fence_pads(faa_fences);

	return 1 + DIV_ROUND_UP(writes, 4) + writes;
}

static void parhelia_faa_patch_fences(u32 *commands,
				      const struct drm_parhelia_packet *packets,
				      u32 count, u32 begin)
{
	u32 i, k = 0, tags[2], resume;

	for (i = 0; i < count; i++) {
		u32 *cmd = commands + packets[i].offset;
		u32 tag = begin + 8 + 8 * k;

		if ((cmd[0] >> 24) ||
		    !parhelia_faa_fence_form(cmd, packets[i].length, tags,
					     &resume))
			continue;
		cmd[tags[0]] = tag | 1;
		cmd[tags[1]] = tag;
		k++;
	}
}

/* DWGSYNC, the padding pairs, then the drain's own armed tag and finish. */
static void parhelia_faa_drain(u32 *drain, u32 begin, u32 faa_fences,
			       u32 dwgctl)
{
	const u32 pads = parhelia_faa_fence_pads(faa_fences);
	const u32 finish = begin + parhelia_faa_fence_step(faa_fences);
	u32 values[8], writes = 0, pos = 1, i, j;
	u8 regs[8];

	regs[writes] = 0xc0;
	values[writes++] = 0x307;
	for (i = 0; i < pads; i++) {
		u32 tag = begin + 8 + 8 * (faa_fences + i);

		regs[writes] = 0xaf;
		values[writes++] = tag | 1;
		regs[writes] = 0xaf;
		values[writes++] = tag;
	}
	regs[writes] = 0xaf;
	values[writes++] = (finish - 8) | 1;
	regs[writes] = 0xaf;
	values[writes++] = finish;
	regs[writes] = 0xc0;
	values[writes++] = dwgctl;
	drain[0] = parhelia_faa_drain_words(faa_fences) - 2;
	for (i = 0; i < writes; i += 4) {
		u32 tag = 0x21212121;

		for (j = 0; j < 4 && i + j < writes; j++)
			tag = (tag & ~(0xffu << (8 * j))) | (u32)regs[i + j] << (8 * j);
		drain[pos++] = tag;
		for (j = 0; j < 4 && i + j < writes; j++)
			drain[pos++] = values[i + j];
	}
}

/* A draw packet: note whether its bound textured entry has a colour fetch. */
static void parhelia_rebind_draw(struct parhelia_rebind *rb)
{
	if (!rb || !rb->descriptors || (rb->bind.entry != rb->textured &&
					 rb->bind.entry != rb->multitex))
		return;
	if (rb->colour)
		rb->bind.colour = true;
	else
		rb->bind.no_colour = true;
}

/* Triangles one indexed (0x9f) packet may draw; the original family takes
 * runs, each in a CALL span of its own (span rule). */
static u32 parhelia_indexed_triangles(const struct parhelia_device *pdev)
{
	if (!parhelia_vertex_objects(pdev))
		return 0;
	if (pdev->pdev->device == PARHELIA_P690)
		return min(READ_ONCE(p690_indexed_triangles), 64u);
	return parhelia_original_family(pdev) ? 64 : 0;
}

/* 16-bit address-mode index lists (0x9b, draw mode 6), on the families
 * parhelia-drm-test --indexed-run-probe INDEX16=1 measured: the P690 and
 * the original Parhelia. */
static bool parhelia_index16(const struct parhelia_device *pdev)
{
	if (!parhelia_indexed_triangles(pdev))
		return false;
	return pdev->pdev->device == PARHELIA_P690 ||
	       parhelia_original_family(pdev);
}

/* Copy @n 16-bit indices from @src (dword aligned) to @dst, the odd last
 * one padded with itself; true when one lies outside [floor, floor+span). */
static bool parhelia_copy_index16(u32 *dst, const u32 *src, u32 n, u32 floor,
				  u32 span)
{
	const u32 pairs = n / 2;
	u32 outside = 0, i = 0;

	/* two pairs a quadword where both ends share the alignment */
	if (!(((uintptr_t)src ^ (uintptr_t)dst) & 7)) {
		if ((uintptr_t)src & 7 && pairs) {
			const u32 pair = READ_ONCE(src[0]);

			dst[0] = pair;
			outside |= ((pair & 0xffff) - floor) >= span;
			outside |= ((pair >> 16) - floor) >= span;
			i = 1;
		}
		for (; i + 2 <= pairs; i += 2) {
			const u64 quad = READ_ONCE(*(const u64 *)&src[i]);

			*(u64 *)&dst[i] = quad;
			outside |= ((u32)(quad & 0xffff) - floor) >= span;
			outside |= ((u32)((quad >> 16) & 0xffff) - floor) >= span;
			outside |= ((u32)((quad >> 32) & 0xffff) - floor) >= span;
			outside |= ((u32)(quad >> 48) - floor) >= span;
		}
	}
	for (; i < pairs; i++) {
		const u32 pair = READ_ONCE(src[i]);

		dst[i] = pair;
		outside |= ((pair & 0xffff) - floor) >= span;
		outside |= ((pair >> 16) - floor) >= span;
	}
	if (n & 1) {
		const u32 last = READ_ONCE(src[pairs]) & 0xffff;

		dst[pairs] = last | last << 16;
		outside |= (last - floor) >= span;
	}
	return outside;
}

/* Relocate an indexed packet drawing from the submit's vertex range.  Its
 * operands are the byte range of its indices inside the vertex range; they
 * are checked against the direct-fetch state the packet draws with, copied
 * to kernel memory the client cannot reach and the packet pointed there.
 * The stream base operand of that state becomes the range's bus address. */
static int parhelia_relocate_indexed(struct parhelia_device *pdev,
				     u32 *commands, u32 *cmd,
				     const struct parhelia_indexed_draw *d,
				     const u8 *range_cpu, u32 range_bytes,
				     u32 range_bus, u32 *index_cpu,
				     u32 index_bus, u32 index_capacity,
				     u32 *index_used, u32 *stream0_patched)
{
	const u32 begin = cmd[1], end = cmd[2], size = d->index_bytes;
	u32 base, count, i;

	/* 16-bit lists start on a dword and may end mid-dword */
	if ((size != 4 && size != 2) || (begin & 3) || (end & (size - 1)) ||
	    end <= begin || end > range_bytes ||
	    (end - begin) % (3 * size) ||
	    (end - begin) / (3 * size) > parhelia_indexed_triangles(pdev))
		return -EINVAL;
	if (!d->stream0_seen || (d->stream0 & 3) != 3 || d->other_stream ||
	    !d->stride || d->extent > d->stride || !d->limit ||
	    d->floor >= d->limit)
		return -EINVAL;
	base = d->stream0 & ~3u;
	if (base >= range_bytes ||
	    (u64)base + (u64)d->limit * d->stride > range_bytes)
		return -EINVAL;
	if (size == 2) {
		const u32 n = (end - begin) / 2, dwords = DIV_ROUND_UP(n, 2);
		u32 start;

		if ((((uintptr_t)(range_cpu + begin) ^
		      (uintptr_t)(index_cpu + *index_used)) & 7) &&
		    *index_used < index_capacity)
			(*index_used)++;
		if (dwords > index_capacity - *index_used)
			return -E2BIG;
		if (parhelia_copy_index16(index_cpu + *index_used,
					  (const u32 *)(range_cpu + begin), n,
					  d->floor, d->limit - d->floor))
			return -EINVAL;
		/* start bit 0: the last dword's high half is no index; the
		 * end is rounded up to that dword and tagged class 3 */
		start = index_bus + *index_used * 4;
		cmd[1] = start | (n & 1);
		cmd[2] = (start + dwords * 4) | 3;
		*index_used += dwords;
		goto stream0;
	}
	count = (end - begin) / 4;
	/* land the copy at the source's 8-byte parity, one dword apart if
	 * need be, so it moves two ordinals at a time */
	if ((((uintptr_t)(range_cpu + begin) ^
	      (uintptr_t)(index_cpu + *index_used)) & 7) &&
	    *index_used < index_capacity)
		(*index_used)++;
	if (count > index_capacity - *index_used)
		return -E2BIG;
	{
		const u32 *src = (const u32 *)(range_cpu + begin);
		u32 *dst = index_cpu + *index_used;
		const u32 floor = d->floor, span = d->limit - d->floor;
		u32 outside = 0;

		i = 0;
		/* two ordinals a quadword where both ends share the alignment */
		if (!(((uintptr_t)src ^ (uintptr_t)dst) & 7)) {
			if ((uintptr_t)src & 7 && count) {
				const u32 ordinal = READ_ONCE(src[0]);

				dst[0] = ordinal;
				outside |= (ordinal - floor) >= span;
				i = 1;
			}
			for (; i + 2 <= count; i += 2) {
				const u64 pair = READ_ONCE(*(const u64 *)&src[i]);

				*(u64 *)&dst[i] = pair;
				outside |= ((u32)pair - floor) >= span;
				outside |= ((u32)(pair >> 32) - floor) >= span;
			}
		}
		/* the copy is what gets fetched: check that, not the source */
		for (; i < count; i++) {
			const u32 ordinal = READ_ONCE(src[i]);

			dst[i] = ordinal;
			outside |= (ordinal - floor) >= span;
		}
		if (outside)
			return -EINVAL;
	}
	/* an untagged start and a class-3 tagged exclusive end */
	cmd[1] = index_bus + *index_used * 4;
	cmd[2] = (index_bus + (*index_used + count) * 4) | 3;
	*index_used += count;
stream0:
	/* the state's base write serves every packet up to the next one */
	if (d->stream0_pos != *stream0_patched) {
		commands[d->stream0_pos] = (range_bus + base) | 3;
		*stream0_patched = d->stream0_pos;
	}
	return 0;
}

static int parhelia_validate_packet(struct parhelia_device *pdev,
				    struct drm_file *file, u32 *cmd,
				    const struct drm_parhelia_packet *packet,
				    struct parhelia_validate_state *vs)
{
	u32 header, words;

	if (packet->length < 2)
		return -EINVAL;
	header = cmd[packet->offset];
	if ((header >> 24) == 0x41)
		return parhelia_validate_constant_group(cmd + packet->offset, packet->length);
	words = (header & 0xffff) + 2;
	/* a run of draws repeats one three-dword header, a run of register
	 * lists follows the first with headers of their own */
	if (words != packet->length &&
	    ((header >> 24) != 0x97 || words != 3 || packet->length % 3) &&
	    ((header >> 24) != 0x00 || words > packet->length))
		return -EINVAL;

	switch (header >> 24) {
	case 0x00: {
		const u32 *c = cmd + packet->offset;
		u32 tags[2], resume;

		if (pdev && parhelia_p690_faa(pdev) &&
		    parhelia_faa_fence_form(c, packet->length, tags, &resume)) {
			if (!vs->faa_fences++)
				vs->faa_first = vs->packet;
			/* only a resume is drawing state for the drain to keep */
			if (resume) {
				vs->dwgctl = c[resume];
				vs->dwgctl_packet = vs->packet;
				vs->dwgctl_seen = true;
			}
			return 0;
		}
		return parhelia_validate_reglist(pdev, file, cmd, packet->offset,
						 packet->length, vs);
	}
	case 0x97:
		if (words != 3)
			return -EINVAL;
		for (u32 k = 3; k < packet->length; k += 3)
			if (cmd[packet->offset + k] != header)
				return -EINVAL;
		parhelia_rebind_draw(vs->rb);
		return 0;
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
	case 0x9b:
	case 0x9f:
		/* Modes 6 and 7 are structurally three-dword packets.  Their
		 * operands are checked and relocated with the direct-fetch state
		 * recorded here, once the relocations name the vertex range. */
		if (words != 3 || (header != 0x9f000001u &&
				   (header != 0x9b000001u || !pdev ||
				    !parhelia_index16(pdev))))
			return -EINVAL;
		/* grown with the indexed packets: sizing it for every packet
		 * of a large submit took a high-order or vmalloc allocation */
		if (vs->indexed_count == vs->indexed_cap) {
			u32 cap = max(vs->indexed_cap * 2, 64u);
			struct parhelia_indexed_draw *grown =
				kvrealloc(vs->indexed, array_size(cap,
					  sizeof(*vs->indexed)), GFP_KERNEL);

			if (!grown)
				return -ENOMEM;
			vs->indexed = grown;
			vs->indexed_cap = cap;
		}
		vs->fetch.packet = vs->packet;
		vs->fetch.index_bytes = (header >> 24) == 0x9b ? 2 : 4;
		vs->indexed[vs->indexed_count++] = vs->fetch;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* True when @dword is the data operand of a texture/surface base write. */
static bool parhelia_vram_operand_reg(const u32 *cmd, u32 length, u32 dword,
				      u32 *reg)
{
	u32 pos = 0, end = 0;

	while (parhelia_reglist_next(cmd, length, &pos, &end)) {
		u32 tag = cmd[pos++];
		unsigned int slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;

			if (index == 0x21)
				break;
			if (pos == dword) {
				if (!parhelia_vram_register(index))
					return false;
				if (reg)
					*reg = 0x1000 + index * sizeof(u32);
				return true;
			}
			pos++;
		}
	}
	return false;
}

/* Keep one engine-visible allocation wholly inside either physical VRAM bank.
 * SUN90 encodes bank B at 128 MiB, so a surface may not cross the half-VRAM
 * discontinuity.  Render-target alignment is imposed by CREATE_RENDER and is
 * preserved while TTM chooses one of these bounded placements.
 */
static int parhelia_vram_placement_range(struct drm_gem_vram_object *bo,
					  u64 vram_size, bool split_banks,
					  u64 range_fpfn, u64 range_lpfn)
{
	u64 vram_pages = vram_size >> PAGE_SHIFT;
	u64 bank_pages = vram_pages >> 1;
	u64 bo_pages = PFN_UP(bo->bo.base.size);

	bo->placement.placement = bo->placements;
	bo->placements[0].mem_type = TTM_PL_VRAM;
	bo->placements[0].flags = 0;
	bo->placements[0].fpfn = range_fpfn;
	bo->placements[0].lpfn = range_lpfn;
	bo->placement.num_placement = 1;
	if (range_lpfn) {
		if (range_fpfn >= range_lpfn || range_lpfn > vram_pages ||
		    bo_pages > range_lpfn - range_fpfn)
			return -E2BIG;
		return 0;
	}

	if (!split_banks)
		return 0;
	bo->placements[0].lpfn = bank_pages;
	if (!bank_pages || bo_pages > bank_pages)
		return -E2BIG;
	bo->placements[1].mem_type = TTM_PL_VRAM;
	bo->placements[1].flags = 0;
	bo->placements[1].fpfn = bank_pages;
	bo->placements[1].lpfn = vram_pages;
	bo->placement.num_placement = 2;
	return 0;
}

/* VRAM offset of the 16 MiB window at engine address @window, false when
 * no VRAM backs all of it. */
static bool parhelia_window_vram(const struct parhelia_device *pdev,
				 u64 window, u64 *lo)
{
	u64 bank_size = pdev->vram_size >> 1;

	if (window >= PARHELIA_LOCAL_BANK_B_BASE) {
		*lo = window - PARHELIA_LOCAL_BANK_B_BASE + bank_size;
		return *lo + SZ_16M <= pdev->vram_size;
	}
	*lo = window;
	return window + SZ_16M <= bank_size;
}

/* The original family's Z unit takes the depth window from the colour
 * address per pixel, so a render target crossing a 16 MiB boundary sent
 * depth into colour rows (0528: colour 0x1f5e000 + 3 MiB).  One place per
 * window lets TTM put such an object into any window with room before it
 * evicts anything; window boundaries in VRAM offsets stay boundaries in
 * engine addresses, as the bank split is 16 MiB aligned. */
int parhelia_window_places_init(struct parhelia_device *pdev)
{
	u32 i;

	spin_lock_init(&pdev->pair_lock);
	pdev->pair_colour_gpu = pdev->pair_depth_gpu = ~0ull;
	if (!parhelia_original_family(pdev))
		return 0;
	pdev->window_count = pdev->vram_size / SZ_16M;
	pdev->window_places = devm_kcalloc(&pdev->pdev->dev, pdev->window_count,
					   sizeof(*pdev->window_places),
					   GFP_KERNEL);
	if (!pdev->window_places)
		return -ENOMEM;
	for (i = 0; i < pdev->window_count; i++) {
		pdev->window_places[i].mem_type = TTM_PL_VRAM;
		pdev->window_places[i].fpfn = ((u64)i * SZ_16M) >> PAGE_SHIFT;
		pdev->window_places[i].lpfn = ((u64)(i + 1) * SZ_16M) >> PAGE_SHIFT;
	}
	return 0;
}

static int parhelia_vram_placement_class(struct drm_gem_vram_object *bo,
					  u64 vram_size, bool split_banks)
{
	struct parhelia_bo *pbo = container_of(bo, struct parhelia_bo, gbo);
	struct parhelia_device *pdev = to_parhelia(bo->bo.base.dev);
	u64 band_pages = SZ_32M >> PAGE_SHIFT;
	int ret;

	if (pbo->placement_class == PARHELIA_PLACEMENT_WINDOWED &&
	    pdev->window_count) {
		bo->placement.placement = pdev->window_places;
		bo->placement.num_placement = pdev->window_count;
		return 0;
	}

	/* Colour and depth live in different 64 MiB banks: with both in bank B
	 * a depth-tested fill ran at 202 instead of 666 Mpixel/s (P690 probe). */
	if (pbo->placement_class == PARHELIA_PLACEMENT_NATIVE_COLOR)
		return parhelia_vram_placement_range(bo, vram_size, false,
						     band_pages, 2 * band_pages);
	if (pbo->placement_class == PARHELIA_PLACEMENT_NATIVE_DEPTH)
		return parhelia_vram_placement_range(bo, vram_size, false,
						     3 * band_pages, 4 * band_pages);
	/* Scanout from bank A cost textured fill 30% (179 vs 262 Mpixel/s at
	 * 1920x1200), from this band nothing on texture-bound fill. */
	if (pbo->placement_class == PARHELIA_PLACEMENT_NATIVE_SCANOUT)
		return parhelia_vram_placement_range(bo, vram_size, false,
						     2 * band_pages, 3 * band_pages);
	/* The fragment buffer needs a 32 MiB band of its own: sharing native
	 * colour's (band 1) or depth's (band 3) corrupted both on the P690,
	 * bands 0 and 2 resolved cleanly (MEASURED, FAA fixture); the vendor
	 * keeps all three apart too.  Band 0 is also the bank opposite depth,
	 * the 0528's rule.  Top-down leaves its bottom to other objects. */
	if (pbo->placement_class == PARHELIA_PLACEMENT_FAA_FRAGMENT) {
		ret = parhelia_vram_placement_range(bo, vram_size, false, 0,
						    band_pages);
		if (!ret)
			bo->placements[0].flags |= TTM_PL_FLAG_TOPDOWN;
		return ret;
	}
	/* Bank A first: TTM takes free room in either place before it evicts. */
	if (pbo->placement_class == PARHELIA_PLACEMENT_NATIVE_OFFSCREEN)
		return parhelia_vram_placement_range(bo, vram_size, true, 0, 0);
	ret = parhelia_vram_placement_range(bo, vram_size, split_banks, 0, 0);
	/* Original family: textures fill VRAM from the top, so they leave the
	 * windows render targets pair colour and depth in (bottom up) free. */
	if (!ret && pbo->layout == PARHELIA_BO_LAYOUT_TEXTURE &&
	    parhelia_original_family(pdev))
		bo->placements[0].flags |= TTM_PL_FLAG_TOPDOWN;
	return ret;
}

/* A framebuffer stays where the display controller can fetch it: windowed
 * targets in the windows below the limit, anything else clamped to it. */
static int parhelia_vram_placement(struct drm_gem_vram_object *bo,
				    u64 vram_size, bool split_banks)
{
	struct parhelia_bo *pbo = container_of(bo, struct parhelia_bo, gbo);
	struct parhelia_device *pdev;
	unsigned int i, n = 0;
	u64 limit;
	int ret;

	ret = parhelia_vram_placement_class(bo, vram_size, split_banks);
	if (ret || !pbo->scanout)
		return ret;
	pdev = to_parhelia(bo->bo.base.dev);
	limit = parhelia_display_vram_limit(pdev) >> PAGE_SHIFT;
	if (limit >= vram_size >> PAGE_SHIFT)
		return 0;
	if (bo->placement.placement == pdev->window_places) {
		n = min_t(u32, pdev->window_count, (limit << PAGE_SHIFT) / SZ_16M);
		bo->placement.num_placement = n;
		return n ? 0 : -E2BIG;
	}
	for (i = 0; i < bo->placement.num_placement; i++) {
		struct ttm_place place = bo->placements[i];

		if (!place.lpfn || place.lpfn > limit)
			place.lpfn = limit;
		if (place.fpfn + PFN_UP(bo->bo.base.size) > place.lpfn)
			continue;
		bo->placements[n++] = place;
	}
	bo->placement.num_placement = n;
	return n ? 0 : -E2BIG;
}

/* Register-list runs: later lists keep their headers, a header is never an
 * operand, and malformed or overrunning lists are refused. */
static int parhelia_reglist_run_static_selftest(void)
{
	u32 run[] = { 3, 0x21b4b3b2, 7, 0xff, 0x100, 1, 0x21212188, 0 };
	u32 flagged[] = { 3, 0x21b4b3b2, 7, 0xff, 0x100, 0x10001, 0x212121b2, 5 };
	u32 overrun[] = { 3, 0x21b4b3b2, 7, 0xff, 0x100, 2, 0x212121b2, 5 };
	u32 short_list[] = { 1, 0x2121b3b2, 7, 1, 0x212121b2, 9 };
	struct drm_parhelia_packet packet = { .length = ARRAY_SIZE(run) };
	struct parhelia_validate_state vs = { };
	u32 reg = 0;

	if (parhelia_validate_packet(NULL, NULL, run, &packet, &vs) ||
	    !parhelia_vram_operand_reg(run, ARRAY_SIZE(run), 7, &reg) ||
	    reg != 0x1220 ||
	    parhelia_vram_operand_reg(run, ARRAY_SIZE(run), 5, NULL) ||
	    parhelia_vram_operand_reg(run, ARRAY_SIZE(run), 6, NULL))
		return -EINVAL;
	if (parhelia_validate_packet(NULL, NULL, flagged, &packet, &vs) != -EINVAL ||
	    parhelia_validate_packet(NULL, NULL, overrun, &packet, &vs) != -EINVAL)
		return -EINVAL;
	packet.length = ARRAY_SIZE(short_list);
	if (parhelia_validate_packet(NULL, NULL, short_list, &packet, &vs) != -EINVAL)
		return -EINVAL;
	/* a first header longer than the packet */
	packet.length = 4;
	if (parhelia_validate_packet(NULL, NULL, run, &packet, &vs) != -EINVAL)
		return -EINVAL;
	return 0;
}

/* The FAA sync forms the P690 FAA path uses, their counter offsets and the
 * forms that must stay refused. */
static int parhelia_faa_fence_static_selftest(void)
{
	static const struct {
		u32 cmd[8], length, tag0, tag1, resume;
	} good[] = {
		{ { 4, 0xc0afafc0, 0x207, 0, 0, 0x2000c4 }, 6, 3, 4, 5 },
		{ { 4, 0xacafafc0, 0x307, 0, 0, 0x3ffe0842 }, 6, 3, 4, 0 },
		{ { 4, 0xafafc0c0, 0x004, 0x407, 0, 0 }, 6, 4, 5, 0 },
		{ { 6, 0x21afc0c0, 0x004, 0x407, 0, 0x2121c0af, 0, 0x960ec4 },
		  8, 4, 6, 7 },
	};
	static const struct {
		u32 cmd[8], length;
	} bad[] = {
		/* a client counter value */
		{ { 4, 0xc0afafc0, 0x207, 5, 0, 0x2000c4 }, 6 },
		/* not a sync operation before the pair */
		{ { 4, 0xc0afafc0, 0x20a, 0, 0, 0x2000c4 }, 6 },
		/* no sync before the pair */
		{ { 3, 0x21c0afaf, 0, 0, 0x2000c4 }, 5 },
		/* split pair */
		{ { 4, 0xafc0afc0, 0x207, 0, 0x307, 0 }, 6 },
		/* two writes after the pair */
		{ { 5, 0x21c0c0af, 0x307, 0, 0, 1, 2 }, 7 },
		/* another register after the pair */
		{ { 4, 0x88afafc0, 0x207, 0, 0, 0 }, 6 },
		/* a single counter */
		{ { 3, 0x21c0afc0, 0x207, 0, 0x2000c4 }, 5 },
		/* a register-list run */
		{ { 4, 0xc0afafc0, 0x207, 0, 0, 0x2000c4, 0, 0 }, 8 },
	};
	u32 commands[6], tags[2], resume, i;
	struct drm_parhelia_packet packet = { .length = 6 };

	for (i = 0; i < ARRAY_SIZE(good); i++)
		if (!parhelia_faa_fence_form(good[i].cmd, good[i].length, tags,
					     &resume) ||
		    tags[0] != good[i].tag0 || tags[1] != good[i].tag1 ||
		    resume != good[i].resume)
			return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		if (parhelia_faa_fence_form(bad[i].cmd, bad[i].length, tags,
					    &resume))
			return -EINVAL;
	/* counters step by 8, pads keep the finish on the 24-tag cadence */
	memcpy(commands, good[0].cmd, sizeof(commands));
	parhelia_faa_patch_fences(commands, &packet, 1, 0x100);
	if (commands[3] != 0x109 || commands[4] != 0x108 ||
	    parhelia_faa_fence_step(0) != 16)
		return -EINVAL;
	for (i = 1; i <= 7; i++) {
		u32 drain[11], seq[8], regs[8], n = 0, pos = 1, slot;
		u32 step = parhelia_faa_fence_step(i);
		u32 words = parhelia_faa_drain_words(i);
		u32 last = 0x100 + 8 * i + 8;	/* completed after the syncs */

		parhelia_faa_drain(drain, 0x100, i, 0x960ec4);
		if ((step - 16) % 24 || step < 16 + 8 * i || words > 11 ||
		    drain[0] != words - 2)
			return -EINVAL;
		while (pos < words) {
			u32 tag = drain[pos++];

			for (slot = 0; slot < 4; slot++) {
				u32 reg = (tag >> (8 * slot)) & 0xff;

				if (reg == 0x21)
					break;
				if (pos >= words || n == 8)
					return -EINVAL;
				regs[n] = reg;
				seq[n++] = drain[pos++];
			}
		}
		if (n < 4 || regs[0] != 0xc0 || seq[0] != 0x307 ||
		    regs[n - 1] != 0xc0 || seq[n - 1] != 0x960ec4 ||
		    seq[n - 2] != 0x100 + step ||
		    seq[n - 3] != ((0x100 + step - 8) | 1))
			return -EINVAL;
		/* every armed tag at most 8 ahead of the completed one */
		for (slot = 1; slot + 1 < n; slot += 2) {
			if (regs[slot] != 0xaf || regs[slot + 1] != 0xaf ||
			    !(seq[slot] & 1) || (seq[slot] & ~7u) > last)
				return -EINVAL;
			last = (seq[slot] & ~7u) + 8;
		}
	}
	return 0;
}

static int parhelia_vram_placement_static_selftest(void)
{
	struct drm_gem_vram_object bo = { };
	struct parhelia_bo pbo = { };
	u64 bank_pages = SZ_64M >> PAGE_SHIFT;
	u64 band_pages = SZ_32M >> PAGE_SHIFT;

	bo.bo.base.size = SZ_2M;
	if (parhelia_vram_placement_range(&bo, SZ_128M, true, 0, 0) ||
	    bo.placement.placement != bo.placements ||
	    bo.placement.num_placement != 2 || bo.placements[0].fpfn ||
	    bo.placements[0].lpfn != bank_pages ||
	    bo.placements[1].fpfn != bank_pages ||
	    bo.placements[1].lpfn != 2 * bank_pages)
		return -EINVAL;
	bo.bo.base.size = SZ_64M + PAGE_SIZE;
	if (parhelia_vram_placement_range(&bo, SZ_128M, true, 0, 0) != -E2BIG)
		return -EINVAL;
	bo.bo.base.size = SZ_2M;
	if (parhelia_vram_placement_range(&bo, SZ_128M, false, 0, 0) ||
	    bo.placement.num_placement != 1 || bo.placements[0].lpfn)
		return -EINVAL;
	bo.bo.base.size = SZ_2M;
	if (parhelia_vram_placement_range(&bo, SZ_128M, false, bank_pages,
					    bank_pages + (SZ_32M >> PAGE_SHIFT)) ||
	    bo.placement.num_placement != 1 ||
	    bo.placements[0].fpfn != bank_pages ||
	    bo.placements[0].lpfn != bank_pages + (SZ_32M >> PAGE_SHIFT))
		return -EINVAL;
	bo.bo.base.size = SZ_32M + PAGE_SIZE;
	if (parhelia_vram_placement_range(&bo, SZ_128M, false, bank_pages,
					    bank_pages + (SZ_32M >> PAGE_SHIFT)) != -E2BIG)
		return -EINVAL;
	pbo.gbo.bo.base.size = SZ_2M;
	pbo.placement_class = PARHELIA_PLACEMENT_NATIVE_COLOR;
	if (parhelia_vram_placement(&pbo.gbo, SZ_128M, true) ||
	    pbo.gbo.placement.num_placement != 1 ||
	    pbo.gbo.placements[0].fpfn != band_pages ||
	    pbo.gbo.placements[0].lpfn != 2 * band_pages)
		return -EINVAL;
	pbo.placement_class = PARHELIA_PLACEMENT_NATIVE_DEPTH;
	if (parhelia_vram_placement(&pbo.gbo, SZ_128M, true) ||
	    pbo.gbo.placement.num_placement != 1 ||
	    pbo.gbo.placements[0].fpfn != 3 * band_pages ||
	    pbo.gbo.placements[0].lpfn != 4 * band_pages)
		return -EINVAL;
	pbo.placement_class = PARHELIA_PLACEMENT_NATIVE_SCANOUT;
	if (parhelia_vram_placement(&pbo.gbo, SZ_128M, true) ||
	    pbo.gbo.placement.num_placement != 1 ||
	    pbo.gbo.placements[0].fpfn != 2 * band_pages ||
	    pbo.gbo.placements[0].lpfn != 3 * band_pages)
		return -EINVAL;
	pbo.placement_class = PARHELIA_PLACEMENT_NATIVE_OFFSCREEN;
	if (parhelia_vram_placement(&pbo.gbo, SZ_128M, false) ||
	    pbo.gbo.placement.num_placement != 2 ||
	    pbo.gbo.placements[0].fpfn || pbo.gbo.placements[0].lpfn != bank_pages ||
	    pbo.gbo.placements[1].fpfn != bank_pages ||
	    pbo.gbo.placements[1].lpfn != 2 * bank_pages)
		return -EINVAL;
	return 0;
}

/* The public helper only pins plane framebuffers. Render relocations need
 * the same TTM operation, held until the synchronous batch fence retires. */
int parhelia_vram_pin_locked(struct drm_gem_vram_object *bo)
{
	struct parhelia_device *pdev = to_parhelia(bo->bo.base.dev);
	struct ttm_operation_ctx ctx = { false, false };
	int ret;

	dma_resv_assert_held(bo->bo.base.resv);
	if (!bo->bo.pin_count) {
		ret = parhelia_vram_placement(bo, pdev->vram_size,
			pdev->pdev->device == PARHELIA_P690);
		if (ret)
			return ret;
		ret = ttm_bo_validate(&bo->bo, &bo->placement, &ctx);
		if (ret)
			return ret;
	}
	ttm_bo_pin(&bo->bo);
	return 0;
}

int parhelia_vram_pin(struct drm_gem_vram_object *bo)
{
	int ret;

	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (ret)
		return ret;
	ret = parhelia_vram_pin_locked(bo);
	ttm_bo_unreserve(&bo->bo);
	return ret;
}

/* Pin a framebuffer for the display controller.  The object keeps the
 * display range for every later placement; one found outside it is moved
 * unless something else holds it there. */
int parhelia_vram_pin_scanout(struct parhelia_device *pdev,
			      struct drm_gem_vram_object *gbo)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);
	u64 limit = parhelia_display_vram_limit(pdev) >> PAGE_SHIFT;
	struct ttm_resource *res;
	int ret;

	ret = ttm_bo_reserve(&gbo->bo, true, false, NULL);
	if (ret)
		return ret;
	bo->scanout = true;
	if (gbo->bo.pin_count) {
		res = gbo->bo.resource;
		if (!res || res->mem_type != TTM_PL_VRAM ||
		    res->start + PFN_UP(gbo->bo.base.size) > limit)
			ret = -ERANGE;
		else
			ttm_bo_pin(&gbo->bo);
	} else {
		ret = parhelia_vram_pin_locked(gbo);
	}
	ttm_bo_unreserve(&gbo->bo);
	return ret;
}

/* Place and pin a new object.  Freed objects stay pinned until the queued
 * work using them retires: when a band is full, wait for those fences, which
 * releases the pins, and retry while the queue held anything to reclaim. */
int parhelia_vram_pin_new(struct parhelia_device *pdev,
			  struct drm_gem_vram_object *bo)
{
	int ret = parhelia_vram_pin(bo);

	while ((ret == -ENOMEM || ret == -ENOSPC) &&
	       pdev->submit_owner != current && READ_ONCE(pdev->queued_slots)) {
		ret = parhelia_submit_idle_lock(pdev);
		if (ret)
			return ret;
		parhelia_submit_unlock(pdev);
		ret = parhelia_vram_pin(bo);
	}
	return ret;
}

void parhelia_vram_unpin(struct drm_gem_vram_object *bo)
{
	if (!ttm_bo_reserve(&bo->bo, false, false, NULL)) {
		ttm_bo_unpin(&bo->bo);
		ttm_bo_unreserve(&bo->bo);
	}
}


/* Submissions share one TTM pin per object, counted in submit_pins: while
 * queued work holds an object, pinning it again needs no reservation.  Only
 * the 0 <-> 1 transitions take the reservation, which serialises them. */
static int parhelia_submit_pin(struct drm_gem_vram_object *gbo)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);
	int ret;

	if (atomic_inc_not_zero(&bo->submit_pins))
		return 0;
	ret = ttm_bo_reserve(&gbo->bo, true, false, NULL);
	if (ret)
		return ret;
	if (!atomic_inc_not_zero(&bo->submit_pins)) {
		ret = parhelia_vram_pin_locked(gbo);
		if (!ret)
			atomic_set(&bo->submit_pins, 1);
	}
	ttm_bo_unreserve(&gbo->bo);
	return ret;
}

static void parhelia_submit_unpin(struct drm_gem_vram_object *gbo)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);

	if (atomic_add_unless(&bo->submit_pins, -1, 1))
		return;
	if (!ttm_bo_reserve(&gbo->bo, false, false, NULL)) {
		if (atomic_dec_and_test(&bo->submit_pins))
			ttm_bo_unpin(&gbo->bo);
		ttm_bo_unreserve(&gbo->bo);
	}
}

static bool parhelia_resource_within(struct drm_gem_vram_object *gbo,
				     unsigned long fpfn, unsigned long lpfn)
{
	return gbo->bo.resource && gbo->bo.resource->start >= fpfn &&
	       gbo->bo.resource->start + PFN_UP(gbo->bo.base.size) <= lpfn;
}

/* parhelia_submit_pin() inside VRAM pages [fpfn, lpfn), moving the object
 * there if no pin holds it elsewhere; -EBUSY if one does. */
static int parhelia_submit_pin_range(struct drm_gem_vram_object *gbo,
				     unsigned long fpfn, unsigned long lpfn)
{
	struct parhelia_bo *bo = container_of(gbo, struct parhelia_bo, gbo);
	struct parhelia_device *pdev = to_parhelia(gbo->bo.base.dev);
	struct ttm_operation_ctx ctx = { false, false };
	int ret = 0;

	if (atomic_inc_not_zero(&bo->submit_pins)) {
		if (parhelia_resource_within(gbo, fpfn, lpfn))
			return 0;
		parhelia_submit_unpin(gbo);
		return -EBUSY;
	}
	ret = ttm_bo_reserve(&gbo->bo, true, false, NULL);
	if (ret)
		return ret;
	if (atomic_inc_not_zero(&bo->submit_pins)) {
		if (!parhelia_resource_within(gbo, fpfn, lpfn)) {
			if (atomic_dec_and_test(&bo->submit_pins))
				ttm_bo_unpin(&gbo->bo);
			ret = -EBUSY;
		}
	} else {
		if (gbo->bo.pin_count) {
			if (!parhelia_resource_within(gbo, fpfn, lpfn))
				ret = -EBUSY;
		} else {
			ret = parhelia_vram_placement_range(gbo, pdev->vram_size,
							    false, fpfn, lpfn);
			if (!ret)
				ret = ttm_bo_validate(&gbo->bo, &gbo->placement,
						      &ctx);
		}
		if (!ret) {
			ttm_bo_pin(&gbo->bo);
			atomic_set(&bo->submit_pins, 1);
		}
	}
	ttm_bo_unreserve(&gbo->bo);
	return ret;
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

/* The last few distinct objects of a relocation list, checked before a full
 * scan: a ring's batches name the same colour, depth and texture objects
 * over and over among hundreds of draw relocations. */
#define PARHELIA_RECENT_OBJECTS 4

struct parhelia_recent_objects {
	struct drm_gem_object *obj[PARHELIA_RECENT_OBJECTS];
	u32 index[PARHELIA_RECENT_OBJECTS];
	unsigned int next;
};

static bool parhelia_recent_find(const struct parhelia_recent_objects *r,
				 const struct drm_gem_object *obj, u32 *index)
{
	unsigned int k;

	for (k = 0; k < PARHELIA_RECENT_OBJECTS; k++)
		if (r->obj[k] == obj) {
			*index = r->index[k];
			return true;
		}
	return false;
}

static void parhelia_recent_add(struct parhelia_recent_objects *r,
				struct drm_gem_object *obj, u32 index)
{
	r->obj[r->next] = obj;
	r->index[r->next] = index;
	r->next = (r->next + 1) % PARHELIA_RECENT_OBJECTS;
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

	struct parhelia_recent_objects recent = { };

	for (i = 0; i < count; i++) {
		struct drm_gem_object *obj = objects[i];
		u32 j;

		if (!obj || parhelia_recent_find(&recent, obj, &j))
			continue;
		for (j = 0; j < i; j++)
			if (objects[j] == obj)
				break;
		parhelia_recent_add(&recent, obj, j);
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
		parhelia_host_views_revoke_cpu(to_parhelia(obj->dev), obj);
	}
	return ret;
}

static void parhelia_slot_release_locked(struct parhelia_device *pdev,
					 struct parhelia_submit_slot *slot, bool error)
{
	u32 i;

	if (slot->state != PARHELIA_SLOT_QUEUED)
		return;
	if (slot->root_probe) {
		parhelia_root_probe_complete(pdev, slot->root_probe,
					     error ? "async-error" : "async-complete");
		kfree(slot->root_probe);
		slot->root_probe = NULL;
	}
	if (slot->out_fence) {
		/* A timed-out slot's fence was already failed when it was
		 * quarantined; teardown releases it once more. */
		if (error && !test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
				       &slot->out_fence->base.flags))
			dma_fence_set_error(&slot->out_fence->base, -EIO);
		dma_fence_signal(&slot->out_fence->base);
		dma_fence_put(&slot->out_fence->base);
		slot->out_fence = NULL;
	}
	for (i = 0; i < slot->reloc_count; i++) {
		if (slot->pinned[i]) {
			parhelia_submit_unpin(slot->pinned[i]);
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
	if (!pdev->queued_slots) {
		pdev->primary_get = pdev->primary_put;
	}
	wake_up_all(&pdev->submit_waitq);
}

struct parhelia_file_waiter {
	struct list_head link;
	struct drm_file *file;
	bool queued;
};

static struct parhelia_file_waiter *
parhelia_async_first_waiter(struct parhelia_device *pdev)
{
	if (list_empty(&pdev->queue_waiters))
		return NULL;
	return list_first_entry(&pdev->queue_waiters,
		struct parhelia_file_waiter, link);
}

static void parhelia_async_refresh_next(struct parhelia_device *pdev)
{
	struct parhelia_file_waiter *first = parhelia_async_first_waiter(pdev);

	pdev->queue_next = first ? first->file : NULL;
}

static void parhelia_async_waiter_add(struct parhelia_device *pdev,
				       struct parhelia_file_waiter *waiter)
{
	if (waiter->queued)
		return;
	list_add_tail(&waiter->link, &pdev->queue_waiters);
	waiter->queued = true;
	parhelia_async_refresh_next(pdev);
}

static void parhelia_async_waiter_del(struct parhelia_device *pdev,
				       struct parhelia_file_waiter *waiter)
{
	if (!waiter->queued)
		return;
	list_del_init(&waiter->link);
	waiter->queued = false;
	parhelia_async_refresh_next(pdev);
}

static void parhelia_async_waiters_del_file(struct parhelia_device *pdev,
					     struct drm_file *file)
{
	struct parhelia_file_waiter *waiter, *tmp;

	list_for_each_entry_safe(waiter, tmp, &pdev->queue_waiters, link) {
		if (waiter->file != file)
			continue;
		list_del_init(&waiter->link);
		waiter->queued = false;
	}
	parhelia_async_refresh_next(pdev);
}

static void parhelia_async_file_claim(struct parhelia_device *pdev,
				       struct drm_file *file)
{
	unsigned int quantum = clamp_val(async_file_quantum_ms, 1, 1000);

	pdev->queue_owner = file;
	pdev->queue_owner_deadline = jiffies + msecs_to_jiffies(quantum);
	pdev->queue_handoff = false;
	/* Wake every submit already queued by this DRM file.  It shares the new
	 * owner's complete state stream and does not need another file switch. */
	parhelia_async_waiters_del_file(pdev, file);
	wake_up_all(&pdev->submit_waitq);
}

/* Native targets use a small set of hardware-proven placements.  Give one DRM
 * file a short FIFO lease so a frame-sized burst does not migrate its targets
 * at every ioctl.  Once the lease expires, the oldest contender stops owner
 * appends and DWGSYNC retirement drains the queue.  TTM can then preserve and
 * evict the old targets before pinning the new file's surfaces. */
/* The lease protects the P690's native placement bands; the original family
 * places by window and lets every file append in submission order. */
static bool parhelia_file_lease(const struct parhelia_device *pdev)
{
	return pdev->pdev->device == PARHELIA_P690;
}

static bool parhelia_async_file_turn(struct parhelia_device *pdev,
				     struct drm_file *file,
				     struct parhelia_file_waiter *waiter)
{
	struct parhelia_file_waiter *first;

	if (!pdev->queue_owner) {
		first = parhelia_async_first_waiter(pdev);
		if (first && first != waiter) {
			parhelia_async_waiter_add(pdev, waiter);
			return false;
		}
		parhelia_async_file_claim(pdev, file);
		return true;
	}
	if (pdev->queue_owner == file) {
		if (pdev->queue_next == file) {
			parhelia_async_waiters_del_file(pdev, file);
			pdev->queue_handoff = false;
		}
		if (pdev->queue_handoff) {
			parhelia_async_waiter_add(pdev, waiter);
			return false;
		}
		if (!pdev->queued_slots && pdev->queue_next) {
			pdev->queue_handoff = true;
			parhelia_async_waiter_add(pdev, waiter);
			return false;
		}
		if (pdev->queue_next &&
		    time_after_eq(jiffies, pdev->queue_owner_deadline)) {
			pdev->queue_handoff = true;
			parhelia_async_waiter_add(pdev, waiter);
			return false;
		}
		parhelia_async_waiter_del(pdev, waiter);
		return true;
	}

	parhelia_async_waiter_add(pdev, waiter);
	first = parhelia_async_first_waiter(pdev);
	/* A lease protects an active burst, not an idle file.  Once DWGSYNC has
	 * retired the owner's last slot, the FIFO head can take ownership without
	 * waiting out the remainder of the timeslice. */
	if (!pdev->queued_slots && first == waiter) {
		parhelia_async_file_claim(pdev, file);
		return true;
	}
	if (first == waiter &&
	    time_after_eq(jiffies, pdev->queue_owner_deadline))
		pdev->queue_handoff = true;
	if (!pdev->queue_handoff || pdev->queued_slots || first != waiter)
		return false;

	parhelia_async_file_claim(pdev, file);
	return true;
}

static bool parhelia_async_file_turn_ready(struct parhelia_device *pdev,
					    struct drm_file *file)
{
	struct drm_file *owner = READ_ONCE(pdev->queue_owner);

	if (!owner)
		return true;
	if (owner == file)
		return !READ_ONCE(pdev->queue_handoff);
	if (READ_ONCE(pdev->queue_next) != file)
		return false;
	if (!READ_ONCE(pdev->queued_slots))
		return true;
	if (!READ_ONCE(pdev->queue_handoff))
		return time_after_eq(jiffies,
			READ_ONCE(pdev->queue_owner_deadline));
	return false;
}

static void parhelia_async_file_turn_cancel(struct parhelia_device *pdev,
					     struct parhelia_file_waiter *waiter)
{
	bool was_first;

	parhelia_submit_lock(pdev);
	was_first = parhelia_async_first_waiter(pdev) == waiter;
	parhelia_async_waiter_del(pdev, waiter);
	if (was_first && pdev->queue_handoff) {
		pdev->queue_handoff = pdev->queue_next &&
			time_after_eq(jiffies, pdev->queue_owner_deadline);
	}
	wake_up_all(&pdev->submit_waitq);
	parhelia_submit_unlock(pdev);
}

static int parhelia_async_file_turn_static_selftest(void)
{
	struct parhelia_device *pdev;
	struct drm_file *a = (struct drm_file *)1UL;
	struct drm_file *b = (struct drm_file *)2UL;
	struct drm_file *c = (struct drm_file *)3UL;
	struct parhelia_file_waiter wa = { .file = a };
	struct parhelia_file_waiter wb = { .file = b };
	struct parhelia_file_waiter wc = { .file = c };
	int ret = -EINVAL;

	pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
	if (!pdev)
		return -ENOMEM;
	init_waitqueue_head(&pdev->submit_waitq);
	INIT_LIST_HEAD(&pdev->queue_waiters);
	INIT_LIST_HEAD(&wa.link);
	INIT_LIST_HEAD(&wb.link);
	INIT_LIST_HEAD(&wc.link);
	if (!parhelia_async_file_turn(pdev, a, &wa) || pdev->queue_owner != a)
		goto out;
	/* Keep the non-expiry checks deterministic even if init is preempted. */
	pdev->queue_owner_deadline = jiffies + msecs_to_jiffies(1000);
	/* B and C wait in arrival order while A appends through its lease. */
	pdev->queued_slots = 2;
	if (parhelia_async_file_turn(pdev, b, &wb) ||
	    parhelia_async_file_turn(pdev, c, &wc) || pdev->queue_next != b ||
	    !parhelia_async_file_turn(pdev, a, &wa))
		goto out;
	/* An empty A queue transfers immediately to FIFO head B, before expiry. */
	pdev->queued_slots = 0;
	if (parhelia_async_file_turn(pdev, a, &wa) || !pdev->queue_handoff ||
	    parhelia_async_file_turn(pdev, c, &wc) ||
	    !parhelia_async_file_turn(pdev, b, &wb) ||
	    pdev->queue_owner != b || pdev->queue_next != c || wb.queued)
		goto out;
	pdev->queue_owner_deadline = jiffies + msecs_to_jiffies(1000);
	/* A live B burst retains the lease.  On expiry B joins the tail and only
	 * FIFO head C can claim after its slots drain. */
	pdev->queued_slots = 2;
	if (!parhelia_async_file_turn(pdev, b, &wb))
		goto out;
	pdev->queue_owner_deadline = jiffies - 1;
	if (parhelia_async_file_turn(pdev, b, &wb) || !pdev->queue_handoff ||
	    parhelia_async_file_turn(pdev, c, &wc))
		goto out;
	pdev->queued_slots = 0;
	if (!parhelia_async_file_turn(pdev, c, &wc) ||
	    pdev->queue_owner != c || pdev->queue_next != a || wc.queued)
		goto out;
	ret = 0;
out:
	kfree(pdev);
	return ret;
}

/* submit_lock serializes both the producer and retirement.  DWGSYNC is the
 * only completion source used here: the vendor IRQ status bits remain KMS
 * vblank-only and are not guessed as render completion interrupts. */
static void parhelia_retire_locked(struct parhelia_device *pdev)
{
	u32 done = parhelia_fence_completed(pdev);
	struct parhelia_submit_slot *oldest = NULL;
	u32 i;

	if (done != pdev->progress_fence) {
		pdev->progress_fence = done;
		pdev->progress_jiffies = jiffies;
	}

	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		if (slot->state != PARHELIA_SLOT_QUEUED)
			continue;
		if ((s32)(done - slot->fence) >= 0) {
			dma_rmb();
			if (submit_stats) {
				struct parhelia_submit_stats *st = &pdev->submit_stats;
				u64 now = ktime_get_ns();

				atomic64_add(now - max(slot->submit_ns,
						       st->last_retire_ns),
					     &st->gpu_busy_ns);
				st->last_retire_ns = now;
			}
			parhelia_slot_release_locked(pdev, slot, false);
		}
	}
	/* A fence means the WARP/DWG work completed, but it does not by itself
	 * prove a dead primary fetcher stopped on a fault.  Only the oldest
	 * queued request can be executing: give it its per-request budget from
	 * the later of its submission and the last fence progress, so a deep
	 * queue of long but advancing work is never mistaken for a hang.
	 * Quarantine all live allocations on a timeout rather than returning
	 * them to userspace. */
	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		if (slot->state == PARHELIA_SLOT_QUEUED &&
		    (!oldest || (s32)(slot->fence - oldest->fence) < 0))
			oldest = slot;
	}
	if (oldest) {
		unsigned long start = oldest->submitted;
		struct parhelia_submit_slot *slot = oldest;

		if (time_after(pdev->progress_jiffies, start))
			start = pdev->progress_jiffies;
		if (!time_after_eq(jiffies, start + oldest->budget))
			return;
		dev_err(&pdev->pdev->dev,
			"asynchronous submit fence %08x made no progress for %u ms (done %08x); wedged\n",
			slot->fence, jiffies_to_msecs(jiffies - start), done);
		dev_err(&pdev->pdev->dev,
			"BM=%08x prim=%08x/%08x sec=%08x/%08x 41c=%08x status450=%08x writeback=%08x DWGCTL=%08x 908=%08x 924=%08x\n",
			parhelia_read(pdev, 0x08f0), parhelia_read(pdev, 0x0404),
			parhelia_read(pdev, 0x0408), parhelia_read(pdev, 0x10a0),
			parhelia_read(pdev, 0x10a4), parhelia_read(pdev, 0x041c),
			parhelia_read(pdev, 0x0450),
			READ_ONCE(((u32 *)pdev->writeback_cpu)[8]),
			parhelia_read(pdev, 0x1300), parhelia_read(pdev, 0x0908),
			parhelia_read(pdev, 0x0924));
		/* Which queued request the fetcher stopped in, by primary dword. */
		dev_err(&pdev->pdev->dev, "primary get %u put %u dwords at %#llx\n",
			pdev->primary_get, pdev->primary_put,
			(unsigned long long)pdev->async_primary_bus);
		for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
			slot = &pdev->slots[i];
			if (slot->state == PARHELIA_SLOT_QUEUED)
				dev_err(&pdev->pdev->dev,
					"queued fence %08x tag %08x primary %u-%u calls %u age %u ms\n",
					slot->fence, slot->dma_tag, slot->primary_begin,
					slot->primary_end, slot->call_count,
					jiffies_to_msecs(jiffies - slot->submitted));
		}
		parhelia_dump_slot_calls(pdev, oldest, parhelia_read(pdev, 0x0404));
		slot = oldest;
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
	}
}

static void parhelia_retire_work(struct work_struct *work)
{
	struct parhelia_device *pdev = container_of(to_delayed_work(work),
						     struct parhelia_device, retire_work);

	parhelia_submit_lock(pdev);
	parhelia_retire_locked(pdev);
	if (pdev->queued_slots && !pdev->wedged)
		mod_delayed_work(system_wq, &pdev->retire_work, msecs_to_jiffies(1));
	parhelia_submit_unlock(pdev);
}

/* Drain every queued hardware user, then return with submit_lock held.  A
 * caller which is about to expose local memory through a temporary host
 * aperture must keep this lock until the descriptor has been revoked, so a
 * racing submit cannot begin sampling or rendering the surface mid-copy. */
int parhelia_submit_idle_lock(struct parhelia_device *pdev)
{
	int ret;

	for (;;) {
		parhelia_submit_lock(pdev);
		parhelia_retire_locked(pdev);
		if (pdev->wedged) {
			parhelia_submit_unlock(pdev);
			return -EIO;
		}
		if (!pdev->queued_slots)
			return 0;
		parhelia_submit_unlock(pdev);
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
/* The stalled request's CALLs and the leading dwords of each secondary span;
 * @get is the primary fetcher's bus address at the stall. */
static void parhelia_dump_slot_calls(struct parhelia_device *pdev,
				     const struct parhelia_submit_slot *slot,
				     u32 get)
{
	const u32 *primary = pdev->async_primary_cpu;
	const u64 sec_bus = slot->secondary_bus;
	const u64 sec_end = sec_bus + (u64)slot->secondary_dwords * sizeof(u32);
	u32 get_dword = ((get & ~3u) - lower_32_bits(pdev->async_primary_bus)) / 4;
	u32 pos, call = 0;

	for (pos = slot->primary_begin; pos + PARHELIA_CALL_DWORDS <= slot->primary_end; pos++) {
		u32 start, end, words, n;
		const u32 *span;
		char line[96];
		int len = 0;

		if (primary[pos] != PARHELIA_CALL_HEADER)
			continue;
		start = primary[pos + 2] & ~3u;
		end = primary[pos + 3] & ~3u;
		words = end > start ? (end - start) / 4 : 0;
		if (start < sec_bus || end > sec_end || end < start)
			span = NULL;
		else
			span = (const u32 *)slot->secondary_cpu + (start - sec_bus) / 4;
		for (n = 0; span && n < min(words, 6u); n++)
			len += scnprintf(line + len, sizeof(line) - len, " %08x", span[n]);
		dev_err(&pdev->pdev->dev, "%s call %u primary %u span %08x-%08x %u dwords:%s\n",
			get_dword >= pos && get_dword < pos + PARHELIA_CALL_DWORDS ? "->" : "  ",
			call++, pos, start, end, words, span ? line : " (not in the slot heap)");
		pos += PARHELIA_CALL_DWORDS - 1;
	}
}

/* A spare max-size command heap, if one is free. */
static bool parhelia_heap_take(struct parhelia_device *pdev,
			       struct parhelia_cmd_heap *heap)
{
	bool found = false;

	spin_lock(&pdev->spare_lock);
	if (pdev->spare_count) {
		*heap = pdev->spare_heaps[--pdev->spare_count];
		found = true;
	}
	spin_unlock(&pdev->spare_lock);
	return found;
}

/* Keep a max-size heap as a spare, free anything else. */
static void parhelia_heap_put(struct parhelia_device *pdev,
			      struct parhelia_cmd_heap *heap)
{
	bool kept = false;

	if (!heap->cpu)
		return;
	if (heap->dwords == PARHELIA_ASYNC_SECONDARY_DWORDS) {
		spin_lock(&pdev->spare_lock);
		if (pdev->spare_count < PARHELIA_SPARE_HEAPS) {
			pdev->spare_heaps[pdev->spare_count++] = *heap;
			kept = true;
		}
		spin_unlock(&pdev->spare_lock);
	}
	if (!kept)
		dma_free_coherent(&pdev->pdev->dev, heap->dwords * sizeof(u32),
				  heap->cpu, heap->bus);
	heap->cpu = NULL;
}

/* @heap, when set, already holds the request's commands at its start: the
 * slot takes it and hands back its previous heap in its place. */
static int parhelia_async_slot_prepare(struct parhelia_device *pdev,
				       struct parhelia_submit_slot *slot, u32 dwords,
				       struct parhelia_cmd_heap *heap)
{
	if (!dwords || dwords > PARHELIA_ASYNC_SECONDARY_DWORDS)
		return -EINVAL;
	if (heap) {
		struct parhelia_cmd_heap previous = {
			.cpu = slot->secondary_cpu, .bus = slot->secondary_bus,
			.dwords = slot->secondary_dwords,
		};

		if (heap->dwords < dwords)
			return -EINVAL;
		slot->secondary_cpu = heap->cpu;
		slot->secondary_bus = heap->bus;
		slot->secondary_dwords = heap->dwords;
		*heap = previous;
	} else if (!slot->secondary_cpu || slot->secondary_dwords < dwords) {
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
	/* Only blocks handed out by the previous request need clearing. */
	memset(slot->blocks, 0, array_size(slot->blocks_used, sizeof(*slot->blocks)));
	slot->blocks_used = 0;
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
	slot->blocks_used = max_t(u32, slot->blocks_used, block - slot->blocks + 1);
	return 0;
}

/* A block of commands that already sit in the slot's heap at @offset. */
static int parhelia_async_block_place(struct parhelia_submit_slot *slot,
				      struct parhelia_dma_block *block,
				      u32 offset, u32 dwords)
{
	if (!dwords || dwords > PARHELIA_DMA_BLOCK_DWORDS || block->cpu ||
	    offset > slot->secondary_dwords ||
	    dwords > slot->secondary_dwords - offset)
		return -EINVAL;
	block->cpu = (u32 *)slot->secondary_cpu + offset;
	block->bus = slot->secondary_bus + offset * sizeof(u32);
	block->dwords = dwords;
	slot->blocks_used = max_t(u32, slot->blocks_used, block - slot->blocks + 1);
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

static void parhelia_kvfree_action(void *data)
{
	kvfree(data);
}

static int parhelia_staging_get(struct parhelia_device *pdev);
static int parhelia_upload_ring_get(struct parhelia_device *pdev);

/* A pooled backing of at least @size bytes, else a new one.  A pooled chunk
 * more than twice the size is taken only when a new allocation fails. */
static void *parhelia_vertex_chunk_get(struct parhelia_device *pdev,
				       size_t size, dma_addr_t *bus,
				       size_t *chunk_size)
{
	struct parhelia_dma_chunk *c, *best = NULL;
	void *cpu = NULL;

	mutex_lock(&pdev->vertex_pool_lock);
	list_for_each_entry(c, &pdev->vertex_pool, link)
		if (c->size >= size && (!best || c->size < best->size))
			best = c;
	if (best && best->size > 2 * size) {
		mutex_unlock(&pdev->vertex_pool_lock);
		cpu = dma_alloc_coherent(&pdev->pdev->dev, size, bus,
					 GFP_KERNEL | __GFP_NOWARN);
		if (cpu) {
			*chunk_size = size;
			return cpu;
		}
		mutex_lock(&pdev->vertex_pool_lock);
		best = NULL;
		list_for_each_entry(c, &pdev->vertex_pool, link)
			if (c->size >= size && (!best || c->size < best->size))
				best = c;
	}
	if (best) {
		list_del(&best->link);
		pdev->vertex_pool_bytes -= best->size;
	}
	mutex_unlock(&pdev->vertex_pool_lock);
	if (best) {
		cpu = best->cpu;
		*bus = best->bus;
		*chunk_size = best->size;
		kfree(best);
		/* the chunk held another object's, maybe another client's, data */
		memset(cpu, 0, size);
		return cpu;
	}
	cpu = dma_alloc_coherent(&pdev->pdev->dev, size, bus,
				 GFP_KERNEL | __GFP_NOWARN);
	if (cpu)
		*chunk_size = size;
	return cpu;
}

static void parhelia_vertex_chunk_put(struct parhelia_device *pdev, void *cpu,
				      dma_addr_t bus, size_t size)
{
	struct parhelia_dma_chunk *c = NULL;

	mutex_lock(&pdev->vertex_pool_lock);
	if (!pdev->vertex_pool_closed &&
	    pdev->vertex_pool_bytes + size <= PARHELIA_VERTEX_POOL_BYTES)
		c = kmalloc(sizeof(*c), GFP_KERNEL);
	if (c) {
		c->cpu = cpu;
		c->bus = bus;
		c->size = size;
		list_add(&c->link, &pdev->vertex_pool);
		pdev->vertex_pool_bytes += size;
	}
	mutex_unlock(&pdev->vertex_pool_lock);
	if (!c)
		dma_free_coherent(&pdev->pdev->dev, size, cpu, bus);
}

static void parhelia_vertex_pool_fini(struct parhelia_device *pdev)
{
	struct parhelia_dma_chunk *c, *tmp;
	LIST_HEAD(chunks);

	mutex_lock(&pdev->vertex_pool_lock);
	pdev->vertex_pool_closed = true;
	list_splice_init(&pdev->vertex_pool, &chunks);
	pdev->vertex_pool_bytes = 0;
	mutex_unlock(&pdev->vertex_pool_lock);
	list_for_each_entry_safe(c, tmp, &chunks, link) {
		dma_free_coherent(&pdev->pdev->dev, c->size, c->cpu, c->bus);
		kfree(c);
	}
}

/* Large coherent buffers are allocated at load, largest first, while host
 * memory is least fragmented; what fails here is still allocated on first
 * use as before. */
static void parhelia_dma_preallocate(struct parhelia_device *pdev)
{
	unsigned int slots = 0, chunks = 0;
	u32 i;

	mutex_lock(&pdev->staging_lock);
	parhelia_staging_get(pdev);
	mutex_unlock(&pdev->staging_lock);
	mutex_lock(&pdev->upload_lock);
	parhelia_upload_ring_get(pdev);
	mutex_unlock(&pdev->upload_lock);
	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		slot->secondary_cpu = dma_alloc_coherent(&pdev->pdev->dev,
			PARHELIA_ASYNC_SECONDARY_DWORDS * sizeof(u32),
			&slot->secondary_bus, GFP_KERNEL | __GFP_NOWARN);
		if (!slot->secondary_cpu)
			break;
		slot->secondary_dwords = PARHELIA_ASYNC_SECONDARY_DWORDS;
		slots++;
	}
	for (i = 0; i < PARHELIA_SPARE_HEAPS; i++) {
		struct parhelia_cmd_heap heap = {
			.dwords = PARHELIA_ASYNC_SECONDARY_DWORDS,
		};

		heap.cpu = dma_alloc_coherent(&pdev->pdev->dev,
			heap.dwords * sizeof(u32), &heap.bus,
			GFP_KERNEL | __GFP_NOWARN);
		if (!heap.cpu)
			break;
		parhelia_heap_put(pdev, &heap);
	}
	for (i = 0; parhelia_vertex_objects(pdev) &&
		    i < PARHELIA_VERTEX_POOL_PREALLOC; i++) {
		dma_addr_t bus;
		void *cpu = dma_alloc_coherent(&pdev->pdev->dev,
			PARHELIA_VERTEX_BO_MAX_SIZE, &bus,
			GFP_KERNEL | __GFP_NOWARN);

		if (!cpu)
			break;
		parhelia_vertex_chunk_put(pdev, cpu, bus,
					  PARHELIA_VERTEX_BO_MAX_SIZE);
		chunks++;
	}
	dev_info(&pdev->pdev->dev,
		 "preallocated: staging %zu KiB, upload ring %zu KiB, %u/%u async command heaps + %u spare, %u vertex chunks\n",
		 pdev->staging_size / SZ_1K, pdev->upload_size / SZ_1K, slots,
		 PARHELIA_ASYNC_SLOTS, pdev->spare_count, chunks);
}

int parhelia_submit_init(struct parhelia_device *pdev)
{
	u32 i;
	int ret;

	spin_lock_init(&pdev->spare_lock);
	ret = parhelia_async_file_turn_static_selftest();
	if (!ret)
		ret = parhelia_vram_placement_static_selftest();
	if (!ret)
		ret = parhelia_reglist_run_static_selftest();
	if (!ret)
		ret = parhelia_shadow_static_selftest();
	if (!ret)
		ret = parhelia_shadow_plan_static_selftest();
	if (!ret)
		ret = parhelia_range_call_static_selftest();
	if (!ret)
		ret = parhelia_large_range_call_static_selftest();
	if (!ret)
		ret = parhelia_faa_fence_static_selftest();
	if (ret) {
		dev_err(&pdev->pdev->dev, "submit static selftest failed\n");
		return ret;
	}

	init_waitqueue_head(&pdev->submit_waitq);
	INIT_LIST_HEAD(&pdev->queue_waiters);
	INIT_DELAYED_WORK(&pdev->retire_work, parhelia_retire_work);
	atomic64_set(&pdev->submit_stats.async_submits, 0);
	atomic64_set(&pdev->submit_stats.slot_waits, 0);
	atomic64_set(&pdev->submit_stats.slot_wait_ns, 0);
	atomic64_set(&pdev->submit_stats.tail_waits, 0);
	atomic64_set(&pdev->submit_stats.tail_wait_ns, 0);
	atomic64_set(&pdev->submit_stats.ring_start_writes, 0);
	atomic64_set(&pdev->submit_stats.ring_end_writes, 0);
	atomic64_set(&pdev->submit_stats.queue_high_water, 0);
	pdev->fence_context = dma_fence_context_alloc(1);
	pdev->async_primary_cpu = dmam_alloc_coherent(&pdev->pdev->dev,
		PARHELIA_ASYNC_PRIMARY_DWORDS * sizeof(u32), &pdev->async_primary_bus,
		GFP_KERNEL);
	if (!pdev->async_primary_cpu)
		return -ENOMEM;
	for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
		struct parhelia_submit_slot *slot = &pdev->slots[i];

		slot->blocks = kvcalloc(PARHELIA_DMA_BLOCK_CAPACITY,
					sizeof(*slot->blocks), GFP_KERNEL);
		if (slot->blocks) {
			ret = devm_add_action_or_reset(&pdev->pdev->dev,
						       parhelia_kvfree_action,
						       slot->blocks);
			if (ret)
				return ret;
		}
		slot->vertex_cpu = dmam_alloc_coherent(&pdev->pdev->dev,
			PARHELIA_VERTEX_DWORDS * sizeof(u32), &slot->vertex_bus, GFP_KERNEL);
		slot->index_cpu = dmam_alloc_coherent(&pdev->pdev->dev,
			PARHELIA_INDEX_DWORDS * sizeof(u32), &slot->index_bus,
			GFP_KERNEL | __GFP_NOWARN);
		slot->writeback_cpu = dmam_alloc_coherent(&pdev->pdev->dev, PAGE_SIZE,
			&slot->writeback_bus, GFP_KERNEL);
		slot->objects = devm_kcalloc(&pdev->pdev->dev, PARHELIA_MAX_RELOCS,
			sizeof(*slot->objects), GFP_KERNEL);
		slot->pinned = devm_kcalloc(&pdev->pdev->dev, PARHELIA_MAX_RELOCS,
			sizeof(*slot->pinned), GFP_KERNEL);
		if (!slot->blocks || !slot->vertex_cpu || !slot->writeback_cpu ||
		    !slot->objects || !slot->pinned)
			return -ENOMEM;
		slot->state = PARHELIA_SLOT_FREE;
	}
	parhelia_dma_preallocate(pdev);
	return 0;
}

static struct parhelia_vertex_bo *
parhelia_vertex_bo_lookup(struct parhelia_device *pdev,
			  struct drm_file *file, struct drm_gem_object *obj)
{
	struct parhelia_vertex_bo *entry;

	list_for_each_entry(entry, &pdev->vertex_bos, link) {
		if (entry->file == file && &entry->dma.base == obj) {
			return entry;
		}
	}
	return NULL;
}

static void parhelia_vertex_object_free(struct drm_gem_object *gem)
{
	struct parhelia_vertex_bo *entry =
		container_of(to_drm_gem_dma_obj(gem), struct parhelia_vertex_bo, dma);

	if (entry->dma.vaddr)
		parhelia_vertex_chunk_put(to_parhelia(gem->dev), entry->dma.vaddr,
					  entry->dma.dma_addr, entry->chunk_size);
	drm_gem_object_release(gem);
	kfree(entry);
}

/* Vertex objects are coherent memory mapped cacheable: the client stages
 * vertices into them in place with strided writes, which a write-combining
 * map would turn into partial line writes, and reads clipped triangles
 * back.  The engine's class-3 fetch snoops, as it does for the kernel's own
 * coherent vertex slots. */
static int parhelia_vertex_object_mmap(struct drm_gem_object *gem,
				       struct vm_area_struct *vma)
{
	struct drm_gem_dma_object *dma = to_drm_gem_dma_obj(gem);
	int ret;

	vma->vm_pgoff -= drm_vma_node_start(&gem->vma_node);
	vm_flags_mod(vma, VM_DONTDUMP | VM_DONTEXPAND, VM_PFNMAP);
	ret = dma_mmap_coherent(drm_dev_dma_dev(gem->dev), vma, dma->vaddr,
				dma->dma_addr, vma->vm_end - vma->vm_start);
	if (ret)
		drm_gem_vm_close(vma);
	return ret;
}

static const struct drm_gem_object_funcs parhelia_vertex_object_funcs = {
	.free = parhelia_vertex_object_free,
	.print_info = drm_gem_dma_object_print_info,
	.vmap = drm_gem_dma_object_vmap,
	.mmap = parhelia_vertex_object_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

static struct parhelia_vertex_bo *
parhelia_vertex_bo_create(struct drm_device *drm, size_t size)
{
	struct parhelia_vertex_bo *entry;
	int ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return ERR_PTR(-ENOMEM);
	ret = drm_gem_object_init(drm, &entry->dma.base, size);
	if (ret)
		goto out_free;
	entry->dma.base.funcs = &parhelia_vertex_object_funcs;
	ret = drm_gem_create_mmap_offset(&entry->dma.base);
	if (ret)
		goto out_put;
	entry->dma.vaddr = parhelia_vertex_chunk_get(to_parhelia(drm), size,
						     &entry->dma.dma_addr,
						     &entry->chunk_size);
	if (!entry->dma.vaddr) {
		ret = -ENOMEM;
		goto out_put;
	}
	if (upper_32_bits(entry->dma.dma_addr) ||
	    entry->dma.dma_addr > U32_MAX - size) {
		ret = -ERANGE;
		goto out_put;
	}
	return entry;

out_put:
	drm_gem_object_put(&entry->dma.base);
	return ERR_PTR(ret);
out_free:
	kfree(entry);
	return ERR_PTR(ret);
}

/*
 * Mode-7 direct fetch needs more than a range check: its index values select
 * byte addresses in the bound vertex stream.  This records the one-stream,
 * class-3 contract recovered for the P690.  The index slice is only a source
 * snapshot: before a mode-7 packet reaches the engine, submit copies it into a
 * driver-private, pinned local-VRAM object which has no DRM handle or mmap
 * permission.  A GEM reference does not stop writes through CREATE_VERTEX's
 * mmap, so validation is always performed on that private snapshot.
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
	u32 pos = 0, end = 0;

	while (parhelia_reglist_next(cmd, length, &pos, &end)) {
		u32 tag = cmd[pos++];
		unsigned int slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;

			if (index == 0x21)
				break;
			if (pos >= end)
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
		u32 pos = 0, end = 0;

		if (cmd[0] >> 24)
			continue;
		while (parhelia_reglist_next(cmd, packets[i].length, &pos,
					     &end)) {
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
	u32 pos = 0, end = 0;

	while (parhelia_reglist_next(cmd, length, &pos, &end)) {
		u32 tag = cmd[pos++];
		unsigned int slot;

		for (slot = 0; slot < 4; slot++) {
			u32 index = (tag >> (slot * 8)) & 0xff;
			u32 value;

			if (index == 0x21)
				break;
			if (pos >= end) {
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
	struct drm_gem_vram_object *dst;
	struct iosys_map dst_map = { };
	void *snapshot;
	u32 address;
	int ret;

	if (mode7->transport == PARHELIA_MODE7_TRANSPORT_DMA)
		return parhelia_mode7_validate_dma_indices(pdev->vertex_cpu,
							   mode7->index_offset,
							   mode7->index_length, state);
	/* The relocation check already bounds this to CREATE_VERTEX's 1 MiB ABI.
	 * Copy first, then validate only the private immutable destination. */
	snapshot = kvmalloc(mode7->index_length, GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	memcpy(snapshot,
	       (u8 *)mode7->index_bo->dma.vaddr + mode7->index_offset,
	       mode7->index_length);

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
		if (mode7->stream_offset >= mode7->stream_bo->dma.base.size)
			return -EINVAL;
	}
	stream_bytes = (u64)(state->limit - state->floor) * state->stride[0] +
		state->descriptor_extent;
	if (stream_bytes > (mode7->transport == PARHELIA_MODE7_TRANSPORT_DMA ?
			mode7->stream_size :
			mode7->stream_bo->dma.base.size) - mode7->stream_offset)
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
	stream_size = mode7->stream_bo->dma.base.size;
	if (!mode7->index_copy || !stream_size || mode7->stream_offset >= stream_size)
		return -EINVAL;
	if (check_add_overflow(lower_32_bits(mode7->stream_bo->dma.dma_addr),
			       mode7->stream_offset, &stream_gpu) ||
	    check_add_overflow(stream_gpu, stream_size - mode7->stream_offset, &stream_end) ||
	    check_add_overflow(mode7->index_copy_offset, mode7->index_length, &index_end) ||
	    index_end > PARHELIA_HOST_APERTURE_BASE)
		return -EINVAL;

	commands[packets[mode7->stream_packet].offset + mode7->stream_dword] =
		stream_gpu | 3u;
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
		} else if (opcode == 0x97 || opcode == 0x9b) {
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
		} else if (opcode == 0x97 || opcode == 0x9f || opcode == 0x9b) {
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
	const u32 *stream, *prim, *bounds, *reset;
	u32 first_range, range_count, vertices, bytes, stream_bus, stream_end, i;

	if (!bound->stream_seen || bound->stream_dword != 2 || packet_count < 6 ||
	    bound->stream_packet > packet_count - 6)
		return -EINVAL;
	first_range = bound->stream_packet + 5;
	range_count = packet_count - first_range;
	if (!range_count || check_mul_overflow(range_count, 3u, &vertices))
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
	    bounds[3] != vertices ||
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
	    state.floor || state.limit != vertices ||
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
			if (check_mul_overflow(i - first_range, 3u, &ordinal))
				return -EOVERFLOW;
			if (packets[i].length != 3 || range[0] != 0x8f000001 ||
			    range[1] != ordinal || range[2] != ordinal + 3)
				return -EINVAL;
		} else if (opcode == 0x97 || opcode == 0x9f || opcode == 0x9b) {
			return -EINVAL;
		} else if (i >= first_range) {
			return -EINVAL;
		}
	}

	if (check_mul_overflow(vertices, state.stride[0], &bytes) ||
	    bound->stream_offset > bound->stream_bo->dma.base.size ||
	    bytes > bound->stream_bo->dma.base.size - bound->stream_offset)
		return -EINVAL;
	if (check_add_overflow(lower_32_bits(bound->stream_bo->dma.dma_addr),
			       bound->stream_offset, &stream_bus) ||
	    check_add_overflow(stream_bus, bytes, &stream_end))
		return -EINVAL;
	commands[packets[bound->stream_packet].offset + bound->stream_dword] =
		stream_bus | 3u;
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
		u32 pos = 0, end = 0;

		if (cmd[0] >> 24)
			continue;
		while (parhelia_reglist_next(cmd, packets[i].length, &pos,
					     &end)) {
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

/* Busy-wait budget for the primary-idle bit before sleeping on it. */
#define PARHELIA_IDLE_POLL_US	100u

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
		if ((s32)(parhelia_fence_completed(pdev) -
			  (fence & ~7u)) >= 0)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

#define PARHELIA_CREATE_RENDER_WINDOW_PAIR \
	(PARHELIA_CREATE_RENDER_WINDOW_COLOR | PARHELIA_CREATE_RENDER_WINDOW_DEPTH)

/* The window a new WINDOW_COLOR/DEPTH target of @file pairs with: below its
 * latest depth surface or above its latest colour target. */
static bool parhelia_pair_window(struct parhelia_device *pdev,
				 struct drm_file *file, u32 flags, u64 *lo)
{
	u64 colour, depth;

	spin_lock(&pdev->pair_lock);
	if (pdev->pair_client != file->client_id) {
		pdev->pair_client = file->client_id;
		pdev->pair_colour_gpu = pdev->pair_depth_gpu = ~0ull;
	}
	colour = pdev->pair_colour_gpu;
	depth = pdev->pair_depth_gpu;
	spin_unlock(&pdev->pair_lock);
	if (flags & PARHELIA_CREATE_RENDER_WINDOW_DEPTH)
		return colour != ~0ull &&
		       parhelia_window_vram(pdev, ALIGN_DOWN(colour, SZ_16M) +
					    SZ_16M, lo);
	return depth != ~0ull && depth >= SZ_16M &&
	       parhelia_window_vram(pdev, ALIGN_DOWN(depth, SZ_16M) - SZ_16M, lo);
}

/* Place and pin a new target in the VRAM window at @lo if it has room there
 * without evicting anything, otherwise wherever a windowed target goes. */
static int parhelia_vram_pin_new_window(struct parhelia_device *pdev,
					struct drm_gem_vram_object *bo, u64 lo)
{
	struct ttm_operation_ctx ctx = { false, false };
	struct ttm_placement placement;
	struct ttm_place *places;
	int ret;

	places = kmalloc_array(pdev->window_count + 1, sizeof(*places),
			       GFP_KERNEL);
	if (!places)
		return -ENOMEM;
	places[0] = (struct ttm_place){
		.mem_type = TTM_PL_VRAM, .flags = TTM_PL_FLAG_DESIRED,
		.fpfn = lo >> PAGE_SHIFT, .lpfn = (lo + SZ_16M) >> PAGE_SHIFT };
	memcpy(places + 1, pdev->window_places,
	       pdev->window_count * sizeof(*places));
	placement.placement = places;
	placement.num_placement = pdev->window_count + 1;
	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (!ret) {
		ret = ttm_bo_validate(&bo->bo, &placement, &ctx);
		if (!ret)
			ttm_bo_pin(&bo->bo);
		ttm_bo_unreserve(&bo->bo);
	}
	kfree(places);
	return ret ? parhelia_vram_pin_new(pdev, bo) : 0;
}

static void parhelia_pair_record(struct parhelia_device *pdev,
				 struct drm_file *file, u32 flags,
				 struct drm_gem_vram_object *bo)
{
	u32 gpu;

	if (parhelia_vram_gpu_addr(pdev, bo, 0, bo->bo.base.size, &gpu))
		return;
	spin_lock(&pdev->pair_lock);
	if (pdev->pair_client == file->client_id) {
		if (flags & PARHELIA_CREATE_RENDER_WINDOW_DEPTH)
			pdev->pair_depth_gpu = gpu;
		else
			pdev->pair_colour_gpu = gpu;
	}
	spin_unlock(&pdev->pair_lock);
}

int parhelia_create_render_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_parhelia_create_render *args = data;
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_gem_vram_object *bo;
	unsigned long align_pages;
	u8 layout;
	u32 allocation_height, pitch_pixels, pitch, cpp, tile_width, pair;
	bool plain;
	u64 window;
	size_t bo_size;
	u32 handle;
	int ret;

	if (!args->width || args->width > 2048 || !args->height || args->height > 2048 ||
	    args->flags & ~(PARHELIA_CREATE_RENDER_LINEAR |
			    PARHELIA_CREATE_RENDER_TEXTURE |
			    PARHELIA_CREATE_RENDER_NATIVE_COLOR |
			    PARHELIA_CREATE_RENDER_NATIVE_DEPTH |
			    PARHELIA_CREATE_RENDER_NATIVE_SCANOUT |
			    PARHELIA_CREATE_RENDER_NATIVE_OFFSCREEN |
			    PARHELIA_CREATE_RENDER_WINDOW_PAIR |
			    PARHELIA_CREATE_RENDER_FAA_FRAGMENT) ||
	    hweight32(args->flags) > 1 || (args->cpp && args->cpp != 4 &&
					   args->cpp != 2) ||
	    args->handle || args->pitch || args->size || args->mmap_offset)
		return -EINVAL;
	cpp = args->cpp ? args->cpp : 4;
	/* The sampler keeps its own storage order for narrow texture images.
	 * 16 bpp targets on the 64x32 layout exist to measure their order;
	 * userspace takes the order from p3d, which knows only the 64x16 one. */
	if (cpp == 2 && (args->flags & PARHELIA_CREATE_RENDER_TEXTURE))
		return -EOPNOTSUPP;
	if (!to_parhelia(drm)->raw_aperture_owned)
		return -EOPNOTSUPP;
	if ((args->flags & (PARHELIA_CREATE_RENDER_NATIVE_COLOR |
			    PARHELIA_CREATE_RENDER_NATIVE_DEPTH |
			    PARHELIA_CREATE_RENDER_NATIVE_SCANOUT |
			    PARHELIA_CREATE_RENDER_NATIVE_OFFSCREEN)) &&
	    (pdev->pdev->device != PARHELIA_P690 || pdev->vram_size < SZ_128M))
		return -EOPNOTSUPP;
	if ((args->flags & PARHELIA_CREATE_RENDER_WINDOW_PAIR) &&
	    !parhelia_original_family(pdev))
		return -EOPNOTSUPP;
	if ((args->flags & PARHELIA_CREATE_RENDER_FAA_FRAGMENT) &&
	    (!parhelia_p690_faa(pdev) || cpp != 4))
		return -EOPNOTSUPP;
	pair = args->flags & PARHELIA_CREATE_RENDER_WINDOW_PAIR;
	plain = !(args->flags & ~PARHELIA_CREATE_RENDER_WINDOW_PAIR);
	/* Legacy flags=0 retains the exact-anchor policy.  Explicit P690 colour
	 * and depth classes use the vendor allocator's distinct 32 MiB bands and
	 * hardware-verified 8 KiB in-band surface alignment. */
	align_pages = pdev->pdev->device == PARHELIA_P690 &&
		      !(args->flags & (PARHELIA_CREATE_RENDER_LINEAR |
				       PARHELIA_CREATE_RENDER_FAA_FRAGMENT |
				       PARHELIA_CREATE_RENDER_TEXTURE |
				       PARHELIA_CREATE_RENDER_NATIVE_COLOR |
				       PARHELIA_CREATE_RENDER_NATIVE_DEPTH |
				       PARHELIA_CREATE_RENDER_NATIVE_SCANOUT |
				       PARHELIA_CREATE_RENDER_NATIVE_OFFSCREEN)) ?
			      SZ_32M / PAGE_SIZE :
			      max_t(unsigned long, 1, SZ_8K / PAGE_SIZE);
	/* Tiled surfaces occupy complete hardware tile rows.  Rounding only the
	 * final byte count underallocates a partial row whenever pitch spans more
	 * than one tile (300x300 on P690 was one 4 KiB tile short). */
	if (args->flags & (PARHELIA_CREATE_RENDER_LINEAR |
			   PARHELIA_CREATE_RENDER_FAA_FRAGMENT))
		allocation_height = args->height;
	else if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE)
		allocation_height = ALIGN(args->height, 16);
	else
		allocation_height = ALIGN(args->height,
			pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ? 16 : 32);
	/* The texture address unit rounds the descriptor's width up to 32
	 * texels, not to the next power of two: parhelia-drm-test
	 * --mip-address-probe measures tile rows 96/64 and 160/64 texels apart
	 * for widths 96 and 129.  Every earlier measurement used a
	 * power-of-two width, where the two rules agree. */
	tile_width = 64 * 4 / cpp;
	if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE)
		pitch_pixels = max_t(u32, ALIGN(args->width, 32), 32);
	else
		pitch_pixels = ALIGN(args->width, tile_width);
	pitch = pitch_pixels * cpp;
	bo_size = (size_t)pitch * allocation_height;
	/* A tiled texture indexes its tiles by the linear texel index over the
	 * pitch, so a pitch that is not a multiple of 64 lets a row band cross
	 * a tile boundary and pitch * height stops covering the last tile. */
	if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE &&
	    pitch_pixels % 64) {
		u32 tile_h = cpp == 4 ? 16 : 32;
		size_t tile_bytes = cpp == 1 ? SZ_2K : SZ_4K;
		u32 bands = max_t(u32, 1, ALIGN(args->height, tile_h) / tile_h);
		size_t tiled = DIV_ROUND_UP((size_t)bands * pitch_pixels, 64) *
			       tile_bytes;

		bo_size = max(bo_size, tiled);
	}
	/* The original's sampler reads 32-bit textures from 64x32-texel 8 KiB
	 * tiles at (row * pitch + x) >> 6, so cover every tile a row band
	 * reaches (glprobe, 16x16 to 200x72 exact). */
	if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE && cpp == 4 &&
	    pdev->chip->tile_layout == PARHELIA_TILE_64X32_8K) {
		u32 rows = max_t(u32, 1, ALIGN(args->height, 32) / 32);

		bo_size = max(bo_size, DIV_ROUND_UP((size_t)rows * pitch_pixels,
						    64) * SZ_8K);
	}
	bo_size = ALIGN(bo_size, 8192);
	bo = drm_gem_vram_create(drm, bo_size, align_pages);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	if (args->flags & (PARHELIA_CREATE_RENDER_LINEAR |
			   PARHELIA_CREATE_RENDER_FAA_FRAGMENT))
		layout = PARHELIA_BO_LAYOUT_LINEAR;
	else if (args->flags & PARHELIA_CREATE_RENDER_TEXTURE)
		layout = PARHELIA_BO_LAYOUT_TEXTURE;
	else if (pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K)
		layout = PARHELIA_BO_LAYOUT_NATIVE_64X16;
	else
		layout = PARHELIA_BO_LAYOUT_NATIVE_64X32;
	parhelia_bo_set_layout(bo, layout, args->width, args->height, pitch);
	if (args->flags & PARHELIA_CREATE_RENDER_NATIVE_COLOR)
		parhelia_bo_set_placement_class(bo,
			PARHELIA_PLACEMENT_NATIVE_COLOR);
	else if (args->flags & PARHELIA_CREATE_RENDER_NATIVE_DEPTH)
		parhelia_bo_set_placement_class(bo,
			PARHELIA_PLACEMENT_NATIVE_DEPTH);
	else if (args->flags & PARHELIA_CREATE_RENDER_NATIVE_SCANOUT)
		parhelia_bo_set_placement_class(bo,
			PARHELIA_PLACEMENT_NATIVE_SCANOUT);
	else if (args->flags & PARHELIA_CREATE_RENDER_NATIVE_OFFSCREEN)
		parhelia_bo_set_placement_class(bo,
			PARHELIA_PLACEMENT_NATIVE_OFFSCREEN);
	else if (args->flags & PARHELIA_CREATE_RENDER_FAA_FRAGMENT)
		parhelia_bo_set_placement_class(bo,
			PARHELIA_PLACEMENT_FAA_FRAGMENT);
	else if (plain && parhelia_original_family(pdev) && bo_size <= SZ_16M)
		parhelia_bo_set_placement_class(bo, PARHELIA_PLACEMENT_WINDOWED);
	else
		pair = 0;
	if (args->flags & PARHELIA_CREATE_RENDER_FAA_FRAGMENT) {
		parhelia_bo_force_unmappable(bo);
	} else if (layout == PARHELIA_BO_LAYOUT_TEXTURE) {
		parhelia_bo_make_unmappable(bo);
	} else if (args->flags & (PARHELIA_CREATE_RENDER_NATIVE_COLOR |
				  PARHELIA_CREATE_RENDER_NATIVE_DEPTH |
				  PARHELIA_CREATE_RENDER_NATIVE_SCANOUT |
				  PARHELIA_CREATE_RENDER_NATIVE_OFFSCREEN)) {
		ret = parhelia_bo_init_host_view(bo, cpp, allocation_height);
		if (ret) {
			drm_gem_object_put(&bo->bo.base);
			return ret;
		}
	}
	/* TTM's CPU clear over BAR0 lands wherever that translated window
	 * currently points, so the driver zeroes new objects itself.  The pin is
	 * held for the clear only: parhelia_ttm_move() copies through the
	 * aperture too, so TTM may evict these objects afterwards. */
	ret = pair && parhelia_pair_window(pdev, file, pair, &window) ?
		parhelia_vram_pin_new_window(pdev, bo, window) :
		parhelia_vram_pin_new(pdev, bo);
	if (ret) {
		drm_gem_object_put(&bo->bo.base);
		return ret;
	}
	if (pair)
		parhelia_pair_record(pdev, file, pair, bo);
	/* the original family has no native classes: its plain targets */
	if (((args->flags & PARHELIA_CREATE_RENDER_NATIVE_COLOR) ||
	     (plain && parhelia_original_family(pdev))) && cpp == 4)
		parhelia_t2_prime_once(pdev, file, bo, args->width, args->height,
				       pitch_pixels);
	ret = parhelia_bo_clear(pdev, file, bo);
	parhelia_vram_unpin(bo);
	if (ret) {
		drm_gem_object_put(&bo->bo.base);
		return ret;
	}
	ret = drm_gem_handle_create(file, &bo->bo.base, &handle);
	if (!ret && !(args->flags & (PARHELIA_CREATE_RENDER_NATIVE_COLOR |
				  PARHELIA_CREATE_RENDER_FAA_FRAGMENT |
				  PARHELIA_CREATE_RENDER_NATIVE_DEPTH |
				  PARHELIA_CREATE_RENDER_NATIVE_SCANOUT |
				  PARHELIA_CREATE_RENDER_NATIVE_OFFSCREEN)) &&
	    (layout != PARHELIA_BO_LAYOUT_TEXTURE ||
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

/* This allocation is intentionally narrow: it is a class-3 vertex source, not
 * a generic GEM pin ioctl.  DMA32 backing gives the engine a stable bus address
 * while keeping every CPU mapping out of the programmable BAR0 aperture. */
int parhelia_create_vertex_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_parhelia_create_vertex *args = data;
	struct parhelia_device *pdev = to_parhelia(drm);
	struct parhelia_vertex_bo *entry;
	size_t size;
	u32 handle;
	int ret;

	if (!parhelia_vertex_objects(pdev))
		return -EOPNOTSUPP;
	if (!args->size || args->size > PARHELIA_VERTEX_BO_MAX_SIZE ||
	    args->flags || args->handle || args->mmap_offset)
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	entry = parhelia_vertex_bo_create(drm, size);
	if (IS_ERR(entry))
		return PTR_ERR(entry);
	ret = drm_gem_handle_create(file, &entry->dma.base, &handle);
	if (ret)
		goto out_put;

	parhelia_submit_lock(pdev);
	if (pdev->wedged) {
		ret = -EIO;
	} else {
		/* Retain the BO independently of its GEM handle.  postclose drops
		 * this reference after every queued use has retired. */
		drm_gem_object_get(&entry->dma.base);
		entry->file = file;
		list_add_tail(&entry->link, &pdev->vertex_bos);
	}
	parhelia_submit_unlock(pdev);
	if (ret)
		goto out_handle;

	args->size = size;
	args->handle = handle;
	args->mmap_offset = drm_vma_node_offset_addr(&entry->dma.base.vma_node);
	drm_gem_object_put(&entry->dma.base);
	return 0;

out_handle:
	drm_gem_handle_delete(file, handle);
out_put:
	drm_gem_object_put(&entry->dma.base);
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
	case PARHELIA_PARAM_WARP_TRANSFORM_TEXTURED:
		args->value = parhelia_warp_entry(pdev,
					 PARHELIA_WARP_TRANSFORM_TEXTURED_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_TRANSFORM_MULTITEX:
		args->value = parhelia_warp_entry(pdev,
					 PARHELIA_WARP_TRANSFORM_MULTITEX_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_LIGHTING:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_LIGHTING_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_CONSTANT_LOADER:
		args->value = parhelia_warp_entry(pdev, PARHELIA_WARP_CONSTANT_LOADER_ENTRY);
		break;
	case PARHELIA_PARAM_WARP_PROGRAM_INSTRUCTIONS:
		args->value = pdev->warp_user_cpu ?
			PARHELIA_WARP_USER_IMAGE_INSTRUCTIONS : 0;
		break;
	case PARHELIA_PARAM_DRAW_RANGES:
		args->value = parhelia_vertex_objects(pdev);
		break;
	case PARHELIA_PARAM_INDEXED_DRAWS:
		args->value = parhelia_indexed_triangles(pdev);
		break;
	case PARHELIA_PARAM_DRAW_RUNS:
		args->value = parhelia_vertex_objects(pdev);
		break;
	case PARHELIA_PARAM_SPLIT_MULTI_DRAWS:
		args->value = parhelia_split_multi_draws(pdev);
		break;
	case PARHELIA_PARAM_INLINE_DRAWS:
		args->value = READ_ONCE(p690_inline_draws) &&
			pdev->pdev->device == PARHELIA_P690;
		break;
	case PARHELIA_PARAM_WINDOW_PAIRS:
		args->value = parhelia_original_family(pdev);
		break;
	case PARHELIA_PARAM_INDEX_DWORDS:
		/* half the buffer: each indexed packet may skip a dword to
		 * keep the copy quadword-aligned */
		args->value = pdev->slots[0].index_cpu ? PARHELIA_INDEX_DWORDS / 2 :
			      PARHELIA_VERTEX_DWORDS / 2;
		break;
	case PARHELIA_PARAM_VERTEX_BO_MAX:
		args->value = parhelia_vertex_objects(pdev) ?
			      PARHELIA_VERTEX_BO_MAX_SIZE : 0;
		break;
	case PARHELIA_PARAM_REGLIST_RUNS:
		args->value = 1;
		break;
	case PARHELIA_PARAM_FAA:
		args->value = parhelia_p690_faa(pdev);
		break;
	case PARHELIA_PARAM_INDEX16:
		args->value = parhelia_index16(pdev);
		break;
	case PARHELIA_PARAM_WARP_PROGRAM_SLOTS:
		args->value = pdev->warp_user_cpu ?
			PARHELIA_WARP_USER_FILE_SLOTS : 0;
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
		args->value = parhelia_vertex_objects(pdev);
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
	case PARHELIA_PARAM_ASYNC_SLOTS:
		args->value = pdev->async_primary_cpu && !trace_submit &&
			!full_sync && !post_draw_wb && coalesce_primary ?
			PARHELIA_ASYNC_SLOTS : 0;
		break;
	case PARHELIA_PARAM_PACK_ADJACENT:
		/*
		 * The original capability did not require userspace to preserve
		 * logical state boundaries and failed its first Q3 live gate.
		 */
		args->value = 0;
		break;
	case PARHELIA_PARAM_PACK_ADJACENT_BOUNDARIES:
		args->value = pack_adjacent_packets && pdev->async_primary_cpu &&
			!trace_submit && !full_sync && !post_draw_wb &&
			coalesce_primary && !state_shadow_cache;
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
		/* Both families: a texture object carries no mmap offset while the
		 * upload path is on, and the original's raw BAR0 view aliases. */
		args->value = pdev->raw_aperture_owned &&
			parhelia_texture_upload_enabled();
		break;
	case PARHELIA_PARAM_NATIVE_RT_CLASSES:
		args->value = pdev->pdev->device == PARHELIA_P690 &&
			pdev->raw_aperture_owned && pdev->vram_size >= SZ_128M;
		break;
	case PARHELIA_PARAM_RENDER_TRANSFER:
		/* Both families: on the original board BAR0 is descriptor
		 * translated too, so a render object's mmap offset does not reach
		 * its VRAM and the aperture copy is the only CPU view that does. */
		args->value = pdev->raw_aperture_owned;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Look up and pin every VRAM relocation target, one pin per unique object.
 * This runs before submit_lock is taken because a pin that has to evict
 * another object calls parhelia_ttm_move(), which drains the engine and
 * retargets the shared aperture: neither is possible once this path owns the
 * lock and has an engine batch half built.  Unwinds itself on failure. */
/* The register a VRAM relocation's operand writes.  Packets are validated
 * only after pinning, so bound everything here. */
static bool parhelia_reloc_target_reg(const u32 *commands, u32 command_dwords,
				      const struct drm_parhelia_packet *packets,
				      u32 packet_count,
				      const struct drm_parhelia_reloc *reloc,
				      u32 *reg)
{
	const struct drm_parhelia_packet *packet;

	if (reloc->type != PARHELIA_RELOC_VRAM || reloc->packet >= packet_count)
		return false;
	packet = &packets[reloc->packet];
	if (packet->offset >= command_dwords ||
	    packet->length > command_dwords - packet->offset ||
	    reloc->dword >= packet->length ||
	    commands[packet->offset] >> 24)
		return false;
	return parhelia_vram_operand_reg(&commands[packet->offset],
					 packet->length, reloc->dword, reg);
}

static bool trace_depth_window;
module_param(trace_depth_window, bool, 0644);
MODULE_PARM_DESC(trace_depth_window,
	"Log each original-Parhelia colour/depth window pairing (diagnostic)");

/* The engine address of an object's current VRAM placement, pinned or not:
 * drm_gem_vram_offset() warns on unpinned objects, so read the resource under
 * the reservation.  -ENODEV when the object is not in VRAM. */
static int parhelia_vram_resident_gpu(struct parhelia_device *pdev,
				      struct drm_gem_vram_object *bo, u64 *gpu)
{
	u64 bank_size = pdev->vram_size >> 1, phys;
	int ret;

	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (ret)
		return ret;
	if (!bo->bo.resource || bo->bo.resource->mem_type != TTM_PL_VRAM) {
		ttm_bo_unreserve(&bo->bo);
		return -ENODEV;
	}
	phys = (u64)bo->bo.resource->start << PAGE_SHIFT;
	ttm_bo_unreserve(&bo->bo);
	*gpu = phys < bank_size ? phys :
	       PARHELIA_LOCAL_BANK_B_BASE + phys - bank_size;
	return 0;
}

/* The original Parhelia's depth unit takes the upper bits of the Z surface
 * address from the colour target: Z only lands in the 16 MiB engine window
 * directly above the colour target's (glprobe, every other window mixes both
 * buffers).  Pin @depth there, draining queued work that still holds it. */
static int parhelia_pin_depth_window(struct parhelia_device *pdev,
				     struct drm_gem_vram_object *colour,
				     struct drm_gem_vram_object *depth)
{
	u64 lo, hi;
	u32 gpu;
	int ret, tries;

	ret = parhelia_vram_gpu_addr(pdev, colour, 0, colour->bo.base.size, &gpu);
	if (ret)
		return ret;
	if (!parhelia_window_vram(pdev, ALIGN_DOWN((u64)gpu, SZ_16M) + SZ_16M,
				  &lo)) {
		dev_warn_ratelimited(&pdev->pdev->dev,
			"no depth window above colour target %#x\n", gpu);
		return parhelia_vram_pin(depth);
	}
	hi = lo + SZ_16M;
	if (trace_depth_window) {
		u64 before = 0;

		parhelia_vram_resident_gpu(pdev, depth, &before);
		dev_info_ratelimited(&pdev->pdev->dev,
			"depth window: colour %#x+%#zx depth %#llx+%#zx pinned %u window %#llx-%#llx\n",
			gpu, colour->bo.base.size, (unsigned long long)before, depth->bo.base.size,
			depth->bo.pin_count, (unsigned long long)lo,
			(unsigned long long)hi);
	}
	for (tries = 0; tries < 4; tries++) {
		ret = parhelia_submit_pin_range(depth, lo >> PAGE_SHIFT,
					      hi >> PAGE_SHIFT);
		if (ret != -EBUSY)
			break;
		ret = parhelia_submit_idle_lock(pdev);
		if (ret)
			return ret;
		parhelia_submit_unlock(pdev);
	}
	if (trace_depth_window) {
		u64 after = 0;

		parhelia_vram_resident_gpu(pdev, depth, &after);
		dev_info_ratelimited(&pdev->pdev->dev,
			"depth window: result %d after %d tries, depth now %#llx\n",
			ret, tries, (unsigned long long)after);
	}
	if (ret == -EBUSY || ret == -ENOMEM || ret == -ENOSPC) {
		dev_warn_ratelimited(&pdev->pdev->dev,
			"depth surface kept outside window %#llx: %d\n",
			(unsigned long long)lo, ret);
		return parhelia_vram_pin(depth);
	}
	return ret;
}

/* Move an unpinned colour target into the window below its depth surface.
 * A client's back buffers otherwise sit in different windows and each swap
 * moves the depth surface to the other one: Q3 at 1280x1024 moved 5 MiB of
 * depth every frame and corrupted the game and X's front buffer, while at
 * 1024x768 both buffers shared a window and it rendered clean.  With the
 * colour following the depth, the back buffers converge on one window. */
static void parhelia_move_colour_below_depth(struct parhelia_device *pdev,
					     struct drm_gem_vram_object *colour,
					     struct drm_gem_vram_object *depth)
{
	struct ttm_operation_ctx ctx = { false, false };
	u64 bank_size = pdev->vram_size >> 1;
	u64 dphys, cphys, dgpu, cgpu, window, lo;
	u64 moved = 0;
	int ret = -EBUSY;

	/* drm_gem_vram_offset() refuses unpinned objects, and both usually are
	 * between frames: read the placements under their reservations. */
	if (ttm_bo_reserve(&depth->bo, true, false, NULL))
		return;
	if (!depth->bo.resource || depth->bo.resource->mem_type != TTM_PL_VRAM) {
		ttm_bo_unreserve(&depth->bo);
		return;
	}
	dphys = (u64)depth->bo.resource->start << PAGE_SHIFT;
	ttm_bo_unreserve(&depth->bo);
	dgpu = dphys < bank_size ? dphys :
	       PARHELIA_LOCAL_BANK_B_BASE + dphys - bank_size;
	window = ALIGN_DOWN(dgpu, SZ_16M);
	if (window < SZ_16M || !parhelia_window_vram(pdev, window - SZ_16M, &lo))
		return;
	if (ttm_bo_reserve(&colour->bo, true, false, NULL))
		return;
	if (!colour->bo.resource || colour->bo.resource->mem_type != TTM_PL_VRAM) {
		ttm_bo_unreserve(&colour->bo);
		return;
	}
	cphys = (u64)colour->bo.resource->start << PAGE_SHIFT;
	cgpu = cphys < bank_size ? cphys :
	       PARHELIA_LOCAL_BANK_B_BASE + cphys - bank_size;
	if (ALIGN_DOWN(cgpu, SZ_16M) + SZ_16M == window) {
		ttm_bo_unreserve(&colour->bo);
		return;
	}
	/* a framebuffer only follows its depth inside the display range */
	if (!colour->bo.pin_count &&
	    (!container_of(colour, struct parhelia_bo, gbo)->scanout ||
	     lo + SZ_16M <= parhelia_display_vram_limit(pdev))) {
		ret = parhelia_vram_placement_range(colour, pdev->vram_size,
						    false, lo >> PAGE_SHIFT,
						    (lo + SZ_16M) >> PAGE_SHIFT);
		if (!ret)
			ret = ttm_bo_validate(&colour->bo, &colour->placement,
					      &ctx);
	}
	if (colour->bo.resource)
		moved = (u64)colour->bo.resource->start << PAGE_SHIFT;
	ttm_bo_unreserve(&colour->bo);
	if (trace_depth_window)
		dev_info_ratelimited(&pdev->pdev->dev,
			"depth window: colour %#llx below depth %#llx -> %#llx: %d\n",
			(unsigned long long)cphys, (unsigned long long)dphys,
			(unsigned long long)moved, ret);
}

/* Per-submit map from a relocation's handle (or kernel object) to the first
 * relocation naming it, so each object is looked up, referenced and pinned
 * once.  Entries of earlier submits are told apart by generation; the map
 * is cached per device and grows with the relocation count. */
struct parhelia_reloc_map {
	struct parhelia_reloc_map_entry {
		unsigned long key;
		u32 gen;
		u32 index;
	} *entry;
	u32 *first;
	u32 mask, capacity, gen;
	/* validation marks the VRAM operands, so a relocation's target
	 * register is a bit test instead of a walk of its packet */
	unsigned long *vram_ops;
	u32 vram_ops_bits;
};

static void parhelia_reloc_map_free(struct parhelia_reloc_map *map)
{
	if (!map)
		return;
	kvfree(map->entry);
	kvfree(map->first);
	kvfree(map->vram_ops);
	kfree(map);
}

static struct parhelia_reloc_map *
parhelia_reloc_map_get(struct parhelia_device *pdev, u32 count, u32 dwords)
{
	struct parhelia_reloc_map *map = xchg(&pdev->reloc_map_cache, NULL);
	u32 size = roundup_pow_of_two(max(2 * count, 16u));

	if (!map) {
		map = kzalloc_obj(*map);
		if (!map)
			return NULL;
	}
	if (count > map->capacity || size > map->mask + 1) {
		kvfree(map->entry);
		kvfree(map->first);
		map->entry = kvcalloc(size, sizeof(*map->entry), GFP_KERNEL);
		map->first = kvmalloc_array(size / 2, sizeof(*map->first),
					    GFP_KERNEL);
		map->mask = size - 1;
		map->capacity = size / 2;
		map->gen = 0;
		if (!map->entry || !map->first) {
			parhelia_reloc_map_free(map);
			return NULL;
		}
	}
	if (dwords > map->vram_ops_bits) {
		kvfree(map->vram_ops);
		map->vram_ops_bits = roundup_pow_of_two(dwords);
		map->vram_ops = kvmalloc_array(BITS_TO_LONGS(map->vram_ops_bits),
					       sizeof(long), GFP_KERNEL);
		if (!map->vram_ops) {
			parhelia_reloc_map_free(map);
			return NULL;
		}
	}
	bitmap_zero(map->vram_ops, dwords);
	if (!++map->gen) {
		memset(map->entry, 0, (map->mask + 1) * sizeof(*map->entry));
		map->gen = 1;
	}
	return map;
}

static void parhelia_reloc_map_put(struct parhelia_device *pdev,
				   struct parhelia_reloc_map *map)
{
	if (!map)
		return;
	parhelia_reloc_map_free(xchg(&pdev->reloc_map_cache, map));
}

/* The first relocation of @key, or @index after recording it as such. */
static u32 parhelia_reloc_map_first(struct parhelia_reloc_map *map,
				    unsigned long key, u32 index)
{
	u32 h = hash_long(key, 32) & map->mask;

	for (;; h = (h + 1) & map->mask) {
		struct parhelia_reloc_map_entry *e = &map->entry[h];

		if (e->gen != map->gen) {
			*e = (struct parhelia_reloc_map_entry){ key, map->gen, index };
			return index;
		}
		if (e->key == key)
			return e->index;
	}
}

/* Pin @colour and @depth without reservations when queued work already
 * holds both with the depth surface in the window above the colour target. */
static bool parhelia_submit_pin_pair(struct parhelia_device *pdev,
				     struct drm_gem_vram_object *colour,
				     struct drm_gem_vram_object *depth)
{
	struct parhelia_bo *c = container_of(colour, struct parhelia_bo, gbo);
	struct parhelia_bo *d = container_of(depth, struct parhelia_bo, gbo);
	u32 cgpu, dgpu, window;

	if (!atomic_inc_not_zero(&c->submit_pins))
		return false;
	if (!atomic_inc_not_zero(&d->submit_pins)) {
		parhelia_submit_unpin(colour);
		return false;
	}
	if (!parhelia_vram_gpu_addr(pdev, colour, 0, colour->bo.base.size,
				    &cgpu) &&
	    !parhelia_vram_gpu_addr(pdev, depth, 0, depth->bo.base.size,
				    &dgpu)) {
		window = ALIGN_DOWN(cgpu, SZ_16M) + SZ_16M;
		if (dgpu >= window && dgpu - window <= SZ_16M - depth->bo.base.size)
			return true;
	}
	parhelia_submit_unpin(depth);
	parhelia_submit_unpin(colour);
	return false;
}

/* Look up and pin each distinct VRAM target once.  objects[] and pinned[]
 * hold a reference and a pin at the first relocation naming an object only;
 * first[] maps every VRAM relocation to that entry. */
static int parhelia_pin_reloc_targets(struct parhelia_device *pdev,
				      struct drm_file *file,
				      struct drm_gem_object **kernel_objects,
				      const u32 *commands, u32 command_dwords,
				      const struct drm_parhelia_packet *packets,
				      u32 packet_count,
				      const struct drm_parhelia_reloc *relocs,
				      u32 count, struct parhelia_reloc_map *map,
				      struct drm_gem_object **objects,
				      struct drm_gem_vram_object **pinned)
{
	struct drm_gem_object *colour = NULL, *depth = NULL, *last_colour = NULL;
	const bool pairs = pdev->pdev->device == PARHELIA_512 ||
			   pdev->pdev->device == PARHELIA_8X;
	bool paired = false;
	u32 i, f, reg;
	int ret;

	for (i = 0; i < count; i++) {
		struct drm_gem_object *obj;

		if (relocs[i].type != PARHELIA_RELOC_VRAM)
			continue;
		f = parhelia_reloc_map_first(map, kernel_objects ?
				(unsigned long)kernel_objects[i] :
				relocs[i].handle, i);
		map->first[i] = f;
		if (f == i) {
			if (kernel_objects) {
				objects[i] = kernel_objects[i];
				drm_gem_object_get(objects[i]);
			} else {
				objects[i] = drm_gem_object_lookup(file,
							relocs[i].handle);
			}
			if (!objects[i]) {
				ret = -ENOENT;
				goto unwind;
			}
		}
		obj = objects[f];
		/* Pair a draw's depth with the colour bound before it.  A clear
		 * binds one surface into both slots, so its 0x1228 names the
		 * 0x1220 object and is not a pair. */
		if (pairs && !depth &&
		    parhelia_reloc_target_reg(commands, command_dwords, packets,
					      packet_count, &relocs[i], &reg)) {
			if (reg == 0x1220) {
				last_colour = obj;
			} else if (reg == 0x1228 && last_colour &&
				   last_colour != obj) {
				colour = last_colour;
				depth = obj;
			}
		}
	}
	if (colour && depth) {
		paired = parhelia_submit_pin_pair(pdev, drm_gem_vram_of_gem(colour),
						  drm_gem_vram_of_gem(depth));
		if (!paired)
			parhelia_move_colour_below_depth(pdev,
					drm_gem_vram_of_gem(colour),
					drm_gem_vram_of_gem(depth));
	}
	for (i = 0; i < count; i++) {
		if (!objects[i] || (depth && objects[i] == depth))
			continue;
		if (paired && objects[i] == colour) {
			pinned[i] = drm_gem_vram_of_gem(colour);
			continue;
		}
		ret = parhelia_submit_pin(drm_gem_vram_of_gem(objects[i]));
		if (ret)
			goto unwind;
		pinned[i] = drm_gem_vram_of_gem(objects[i]);
	}
	if (depth) {
		for (i = 0; objects[i] != depth; i++)
			;
		ret = paired ? 0 : colour ?
			parhelia_pin_depth_window(pdev, drm_gem_vram_of_gem(colour),
						  drm_gem_vram_of_gem(depth)) :
			parhelia_submit_pin(drm_gem_vram_of_gem(depth));
		if (ret)
			goto unwind;
		pinned[i] = drm_gem_vram_of_gem(depth);
	}
	return 0;

unwind:
	if (paired) {
		/* the pair's pins not yet recorded in pinned[] */
		for (i = 0; i < count && objects[i] != colour; i++)
			;
		if (i == count || !pinned[i])
			parhelia_submit_unpin(drm_gem_vram_of_gem(colour));
		for (i = 0; i < count && objects[i] != depth; i++)
			;
		if (i == count || !pinned[i])
			parhelia_submit_unpin(drm_gem_vram_of_gem(depth));
	}
	for (i = 0; i < count; i++) {
		if (pinned[i]) {
			parhelia_submit_unpin(pinned[i]);
			pinned[i] = NULL;
		}
		if (objects[i]) {
			drm_gem_object_put(objects[i]);
			objects[i] = NULL;
		}
	}
	return ret;
}

static int parhelia_submit_check(const struct drm_parhelia_submit *args)
{
	bool async = args->flags & PARHELIA_SUBMIT_ASYNC;
	bool boundary_aware = args->flags & PARHELIA_SUBMIT_CALL_BOUNDARIES;
	bool generic_packet_packing = async && boundary_aware &&
		pack_adjacent_packets && coalesce_primary && !trace_submit &&
		!full_sync && !post_draw_wb && !state_shadow_cache;

	if (args->flags & ~(PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC |
			    PARHELIA_SUBMIT_CALL_BOUNDARIES) ||
	    !args->packet_count || args->packet_count > PARHELIA_MAX_USER_PACKETS ||
	    !args->command_dwords || args->command_dwords > PARHELIA_MAX_COMMAND_DWORDS ||
	    args->reloc_count > PARHELIA_MAX_RELOCS ||
	    args->vertex_dwords > PARHELIA_VERTEX_DWORDS)
		return -EINVAL;
	if (async && (!(args->flags & PARHELIA_SUBMIT_FENCE) || !coalesce_primary ||
		      trace_submit || full_sync || post_draw_wb))
		return -EINVAL;
	if (boundary_aware && !generic_packet_packing)
		return -EOPNOTSUPP;
	if (!async && args->packet_count > PARHELIA_LEGACY_USER_PACKETS)
		return -E2BIG;
	if (args->vertex_dwords && !args->vertices)
		return -EINVAL;
	/* Keep comparisons independent; tracing is for small diagnostic batches. */
	if ((full_sync && (trace_submit || replay_fences)) ||
	    (post_draw_wb && trace_submit))
		return -EINVAL;
	if (trace_submit && args->packet_count > 64)
		return -E2BIG;
	return 0;
}

/* Runs a checked request whose arrays are kernel copies; it frees them.
 * @kernel_objects, when set, supplies the VRAM reloc targets in place of
 * @file's handles. */
/* Every field of a submit plan is written before it is read: the counters by
 * parhelia_plan_calls*(), stage_modes for each packet by the validation loop. */
static struct parhelia_call_plan *parhelia_plan_get(struct parhelia_device *pdev)
{
	struct parhelia_call_plan *plan = xchg(&pdev->plan_cache, NULL);

	return plan ? plan : kvmalloc_obj(*plan, GFP_KERNEL);
}

static void parhelia_plan_put(struct parhelia_device *pdev,
			      struct parhelia_call_plan *plan)
{
	if (plan && cmpxchg(&pdev->plan_cache, NULL, plan))
		kvfree(plan);
}

static int parhelia_submit_run(struct parhelia_device *pdev,
			       struct drm_file *file,
			       struct drm_parhelia_submit *args,
			       struct drm_parhelia_packet *packets,
			       u32 *commands,
			       struct drm_parhelia_reloc *relocs,
			       struct drm_gem_object **kernel_objects,
			       const u32 *kernel_vertices,
			       struct parhelia_cmd_heap *heap)
{
	struct parhelia_submit_stats *stats = &pdev->submit_stats;
	struct drm_gem_object **objects = NULL;
	struct drm_gem_vram_object **pinned = NULL;
	struct drm_gem_object *vertex_obj = NULL;
	struct parhelia_vertex_bo *vertex_cached = NULL;
	u32 vertex_handle = 0;
	struct parhelia_call_plan *plan;
	struct parhelia_submit_slot *async_slot = NULL;
	struct parhelia_dma_block *blocks = pdev->dma_blocks;
	struct parhelia_mode7_submit mode7 = { };
	struct parhelia_mode7_state mode7_state = { };
	struct parhelia_mode3_submit mode3 = { };
	struct parhelia_bound_range_submit bound_range = { };
	struct parhelia_shadow_txn *shadow_txn = NULL;
	struct parhelia_root_probe_trace *root_probe = NULL;
	struct parhelia_file_waiter file_waiter = { .file = file };
	struct parhelia_rebind rebind_state;
	struct parhelia_validate_state validate = { };
	struct parhelia_reloc_map *reloc_map = NULL;
	u32 *primary;
	void *vertex_cpu = pdev->vertex_cpu;
	dma_addr_t vertex_bus = pdev->vertex_bus;
	dma_addr_t writeback_bus = pdev->writeback_bus;
	dma_addr_t primary_bus = pdev->primary_bus;
	bool *draw_relocated, *call_boundaries;
	bool host_vertex_draw = false;
	const bool split_multi_draws = parhelia_split_multi_draws(pdev);
	struct parhelia_submit_slot *local_vertex = NULL;
	bool vertex_object_draw = false;
	u32 draw_ranges = 0, draw_ranges_bus = 0;
	const u8 *draw_ranges_cpu = NULL;
	u32 index_used = 0, indexed_next = 0, stream0_patched = U32_MAX;
	bool async = args->flags & PARHELIA_SUBMIT_ASYNC;
	const bool lease = async && parhelia_file_lease(pdev);
	u32 i, end = 0, fence, dma_tag, primary_words, waited, opening_slot;
	u32 boundary_packet = 0, boundary_count = 0, call_boundary_count = 0;
	u32 call_words = trace_submit ? 8 : PARHELIA_CALL_DWORDS;
	u32 prefix_words = full_sync ? 3 : 0;
	u32 total_calls;
	u32 fence_step = (replay_fences || full_sync) ? 16 : 8;
	u32 fence_span;
	u32 first_draw;
	bool defer_packet_packing, generic_packet_packing;
	bool boundary_aware = args->flags & PARHELIA_SUBMIT_CALL_BOUNDARIES;
	bool inline_draws = READ_ONCE(p690_inline_draws) &&
		pdev->pdev->device == PARHELIA_P690;
	u32 inline_dwords = 0, inline_used = 0;
	const char *async_failure_stage = "request setup";
	u64 phase_t = submit_stats ? ktime_get_ns() : 0;
	int ret = 0;

	static_assert(PARHELIA_MAX_USER_PACKETS == PARHELIA_USER_PACKET_CAPACITY);
	static_assert(PARHELIA_MAX_USER_PACKETS <= U16_MAX);
	static_assert(PARHELIA_PLAN_CAPACITY >=
		      2u * PARHELIA_MAX_USER_PACKETS + 6u);
	static_assert(PARHELIA_MAX_DRAW_DWORDS <= PARHELIA_DMA_BLOCK_DWORDS);
	INIT_LIST_HEAD(&file_waiter.link);
	generic_packet_packing = async && boundary_aware && pack_adjacent_packets &&
		coalesce_primary && !trace_submit && !full_sync && !post_draw_wb &&
		!state_shadow_cache;
	defer_packet_packing = coalesce_primary && !trace_submit && !full_sync &&
		!post_draw_wb && (pack_draw_packets || generic_packet_packing);

	plan = parhelia_plan_get(pdev);
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
	draw_relocated = kcalloc(args->packet_count, sizeof(*draw_relocated), GFP_KERNEL);
	call_boundaries = kcalloc(args->packet_count, sizeof(*call_boundaries),
				  GFP_KERNEL);
	pinned = kcalloc(args->reloc_count, sizeof(*pinned), GFP_KERNEL);
	objects = kcalloc(args->reloc_count, sizeof(*objects), GFP_KERNEL);
	if (!draw_relocated || !call_boundaries ||
	    (args->reloc_count && (!pinned || !objects))) {
		ret = -ENOMEM;
		goto out_relocs;
	}
	reloc_map = parhelia_reloc_map_get(pdev, args->reloc_count,
					   args->command_dwords);
	if (!reloc_map) {
		ret = -ENOMEM;
		goto out_relocs;
	}
	ret = parhelia_pin_reloc_targets(pdev, file, kernel_objects, commands,
					 args->command_dwords, packets,
					 args->packet_count, relocs,
					 args->reloc_count, reloc_map, objects,
					 pinned);
	if (ret)
		goto out_relocs;
	if (submit_stats) {
		u32 pins = 0;

		for (i = 0; i < args->reloc_count; i++)
			pins += !!pinned[i];
		atomic64_add(args->packet_count, &stats->packets);
		atomic64_add(args->command_dwords, &stats->command_dwords);
		atomic64_add(args->reloc_count, &stats->relocs);
		atomic64_add(pins, &stats->reloc_pins);
	}
	parhelia_phase(&phase_t, PARHELIA_PHASE_PIN, stats);

	if (pdev->pdev->device == PARHELIA_P690) {
		rebind_state = (struct parhelia_rebind){
			.textured = parhelia_warp_entry(pdev,
					PARHELIA_WARP_TEXTURED_ENTRY),
			.multitex = parhelia_warp_entry(pdev,
					PARHELIA_WARP_TEXTURED_MULTITEX_ENTRY),
		};
		validate.rb = &rebind_state;
	}
	validate.vram_ops = reloc_map->vram_ops;
	for (i = 0; i < args->packet_count; i++) {
		if (packets[i].offset != end ||
		    check_add_overflow(end, packets[i].length, &end) ||
		    end > args->command_dwords ||
		    packets[i].length > PARHELIA_DMA_BLOCK_DWORDS) {
			ret = -EINVAL;
			goto out_relocs;
		}
		validate.packet = i;
		validate.packet_count = args->packet_count;
		ret = parhelia_validate_packet(pdev, file, commands, &packets[i],
					       &validate);
		if (ret) {
			goto out_relocs;
		}
		plan->stage_modes[i] = parhelia_vertex_bind_mode(commands + packets[i].offset,
							       packets[i].length);
		plan->pkt_op[i] = commands[packets[i].offset] >> 24;
		plan->pkt_draw[i] = 0;
	}
	if (end != args->command_dwords) {
		ret = -EINVAL;
		goto out_relocs;
	}
	if (validate.rb)
		parhelia_rebind_apply(pdev, commands, &validate.rb->bind);
	parhelia_phase(&phase_t, PARHELIA_PHASE_VALIDATE, stats);
	ret = parhelia_collect_call_boundaries(relocs, args->reloc_count,
					       args->packet_count, generic_packet_packing,
					       call_boundaries, &call_boundary_count);
	if (ret)
		goto out_relocs;
	for (i = 0; i < args->reloc_count; i++) {
		const struct drm_parhelia_reloc *reloc = &relocs[i];

		if (reloc->type != PARHELIA_RELOC_BATCH_BOUNDARY)
			continue;
		if (++boundary_count > 1 || boundary_aware || call_boundary_count ||
		    !stateful_submit || !async ||
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
		/* FAA sync counters follow the opening fence */
		if (validate.faa_fences && validate.faa_first < opening_slot)
			opening_slot = 0;
		ret = parhelia_plan_calls(plan, args->packet_count, opening_slot,
			full_sync);
	}
	if (ret)
		goto out_relocs;
	if (coalesce_primary && !trace_submit && !full_sync && !post_draw_wb)
		parhelia_coalesce_range_calls(plan, commands, packets,
			args->packet_count,
			generic_packet_packing ? call_boundaries : NULL);
	if (!defer_packet_packing) {
		ret = parhelia_plan_fits_hardware(plan);
		if (ret)
			goto out_relocs;
	}
	total_calls = plan->count;
	parhelia_phase(&phase_t, PARHELIA_PHASE_PLAN, stats);


retry_slot:
	parhelia_submit_lock(pdev);
	parhelia_phase(&phase_t, PARHELIA_PHASE_LOCKWAIT, stats);
	parhelia_retire_locked(pdev);
	parhelia_phase(&phase_t, PARHELIA_PHASE_RETIRE, stats);
	if (pdev->wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	if (lease && !parhelia_async_file_turn(pdev, file, &file_waiter)) {
		parhelia_submit_unlock(pdev);
		ret = wait_event_interruptible_timeout(pdev->submit_waitq,
			pdev->wedged || parhelia_async_file_turn_ready(pdev, file),
			msecs_to_jiffies(10));
		if (ret < 0) {
			goto out_relocs;
		}
		goto retry_slot;
	}
	if (async && submit_root_probe && pdev->queued_slots) {
		parhelia_submit_unlock(pdev);
		ret = wait_event_interruptible_timeout(pdev->submit_waitq,
			pdev->wedged || !pdev->queued_slots, msecs_to_jiffies(10));
		if (ret < 0)
			goto out_relocs;
		goto retry_slot;
	}
	if (inline_draws) {
		inline_dwords = parhelia_inline_draw_bound(commands, packets,
				args->packet_count, relocs, args->reloc_count);
		inline_draws = inline_dwords;
	}
	if (async) {
		u32 slot_index;
		u32 needed, reserved_calls, secondary_dwords;
		u32 stage_dwords, sync_dwords;

		reserved_calls = min(total_calls, PARHELIA_CALL_CAPACITY);
		if (check_mul_overflow(reserved_calls, PARHELIA_CALL_DWORDS,
				       &needed) ||
		    check_add_overflow(needed, 3u, &needed) ||
		    check_mul_overflow(plan->stages, 13u, &stage_dwords) ||
		    check_mul_overflow(plan->sync_sections, 14u, &sync_dwords) ||
		    check_add_overflow(args->command_dwords, stage_dwords,
				       &secondary_dwords) ||
		    check_add_overflow(secondary_dwords, sync_dwords,
				       &secondary_dwords) ||
		    check_add_overflow(secondary_dwords, inline_dwords,
				       &secondary_dwords)) {
			ret = -EOVERFLOW;
			goto out_unlock;
		}

		for (slot_index = 0; slot_index < PARHELIA_ASYNC_SLOTS; slot_index++)
			if (pdev->slots[slot_index].state == PARHELIA_SLOT_FREE)
				break;
		if (slot_index == PARHELIA_ASYNC_SLOTS || pdev->wedged) {
			u64 wait_begin;

			ret = pdev->wedged ? -EIO : 0;
			wait_begin = ret ? 0 :
				parhelia_submit_wait_begin(&stats->slot_waits);
			parhelia_submit_unlock(pdev);
			if (ret)
				goto out_relocs;
			ret = wait_event_interruptible_timeout(pdev->submit_waitq,
				pdev->wedged || pdev->queued_slots < PARHELIA_ASYNC_SLOTS,
				msecs_to_jiffies(10));
			parhelia_submit_wait_end(&stats->slot_wait_ns, wait_begin);
			if (ret < 0)
				goto out_relocs;
			goto retry_slot;
		}
		/* A primary allocation is never wrapped while the device can fetch it.
		 * Once the queue drains, require the proven primary-idle bit before
		 * restarting at its base. */
		if (!pdev->queued_slots && pdev->primary_armed) {
			/* The bit follows the last fence within microseconds,
			 * and nothing wakes the queue for it: a sleep cost a
			 * whole jiffy on most submits into an idle queue. */
			for (waited = 0; waited < PARHELIA_IDLE_POLL_US &&
			     !(parhelia_read(pdev, 0x08f0) & BIT(17)); waited++)
				udelay(1);
			if (!(parhelia_read(pdev, 0x08f0) & BIT(17))) {
				u64 wait_begin =
					parhelia_submit_wait_begin(&stats->idle_waits);

				parhelia_submit_unlock(pdev);
				ret = wait_event_interruptible_timeout(pdev->submit_waitq,
					pdev->wedged || (parhelia_read(pdev, 0x08f0) & BIT(17)),
					msecs_to_jiffies(1));
				parhelia_submit_wait_end(&stats->idle_wait_ns, wait_begin);
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
				u64 wait_begin =
					parhelia_submit_wait_begin(&stats->tail_waits);

				parhelia_submit_unlock(pdev);
				ret = wait_event_interruptible_timeout(pdev->submit_waitq,
					pdev->wedged || !pdev->queued_slots,
					msecs_to_jiffies(10));
				parhelia_submit_wait_end(&stats->tail_wait_ns, wait_begin);
				if (ret < 0)
					goto out_relocs;
				goto retry_slot;
			}
			ret = -EBUSY;
			goto out_unlock;
		}
		async_slot = &pdev->slots[slot_index];
		async_failure_stage = "secondary coherent heap";
		ret = parhelia_async_slot_prepare(pdev, async_slot, secondary_dwords,
						  heap);
		if (ret)
			goto out_unlock;
		/* the request's commands now open the slot's heap */
		if (heap) {
			async_slot->secondary_put = args->command_dwords;
			commands = async_slot->secondary_cpu;
		}
		blocks = async_slot->blocks;
		vertex_cpu = async_slot->vertex_cpu;
		vertex_bus = async_slot->vertex_bus;
		writeback_bus = async_slot->writeback_bus;
		primary_bus = pdev->async_primary_bus + pdev->primary_put * sizeof(u32);
		if (local_vertex_draws && args->vertex_dwords &&
		    pdev->pdev->device == PARHELIA_P690) {
			async_failure_stage = "local vertex copy";
			ret = parhelia_local_vertex_prepare(pdev, async_slot);
			if (ret)
				goto out_unlock;
			local_vertex = async_slot;
		}
	}
	if (!async && pdev->queued_slots) {
		parhelia_submit_unlock(pdev);
		ret = wait_event_interruptible_timeout(pdev->submit_waitq,
			pdev->wedged || !pdev->queued_slots, msecs_to_jiffies(10));
		if (ret < 0)
			goto out_relocs;
		goto retry_slot;
	}
	parhelia_phase(&phase_t, PARHELIA_PHASE_LOCK, stats);
	ret = parhelia_engine_init(pdev);
	if (ret)
		goto out_unlock;
	if (args->vertex_dwords && kernel_vertices)
		memcpy(vertex_cpu, kernel_vertices,
		       args->vertex_dwords * sizeof(u32));
	else if (args->vertex_dwords &&
		 copy_from_user(vertex_cpu, u64_to_user_ptr(args->vertices),
				args->vertex_dwords * sizeof(u32))) {
		ret = -EFAULT;
		goto out_unlock;
	}
	if (local_vertex) {
		ret = parhelia_vram_write(pdev, local_vertex->local_vertex, 0,
					  vertex_cpu, args->vertex_dwords * sizeof(u32));
		if (ret)
			goto out_unlock;
	}

	for (i = 0; i < args->reloc_count; i++) {
		struct drm_parhelia_reloc *reloc = &relocs[i];
		struct drm_parhelia_packet *packet;
		struct drm_gem_object *obj;
		u32 *packet_cmd;

		if (reloc->packet >= args->packet_count) {
			ret = -EINVAL;
			goto out_unpin;
		}
		packet = &packets[reloc->packet];
		packet_cmd = commands + packet->offset;
		if (reloc->type == PARHELIA_RELOC_BATCH_BOUNDARY)
			continue;
		if (reloc->type == PARHELIA_RELOC_CALL_BOUNDARY)
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
			    packets[reloc->packet].length != 3 ||
			    (reloc->dword & 3) || !reloc->delta || (reloc->delta & 3) ||
			    reloc->delta > PARHELIA_MAX_DRAW_DWORDS * sizeof(u32) ||
			    reloc->handle || reloc->dword > args->vertex_dwords * sizeof(u32) ||
			    reloc->delta > args->vertex_dwords * sizeof(u32) - reloc->dword ||
			    vertex_object_draw) {
				ret = -EINVAL;
				goto out_unpin;
			}
			packet_cmd[1] = (local_vertex ? local_vertex->local_vertex_gpu :
					 lower_32_bits(vertex_bus)) + reloc->dword;
			packet_cmd[2] = packet_cmd[1] + reloc->delta;
			draw_relocated[reloc->packet] = true;
			host_vertex_draw = true;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_DRAW_RANGES) {
			struct parhelia_vertex_bo *vertex_bo;
			u32 range_end, bus_end;

			if (!parhelia_vertex_objects(pdev) || draw_ranges ||
			    host_vertex_draw || vertex_object_draw ||
			    args->vertex_dwords || reloc->packet ||
			    !reloc->dword || ((reloc->dword | reloc->delta) & 3) ||
			    check_add_overflow(reloc->delta, reloc->dword, &range_end)) {
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
			if (range_end > vertex_bo->dma.base.size ||
			    check_add_overflow(lower_32_bits(vertex_bo->dma.dma_addr),
					       reloc->delta, &draw_ranges_bus) ||
			    check_add_overflow(draw_ranges_bus, reloc->dword, &bus_end)) {
				ret = -EINVAL;
				goto out_unpin;
			}
			/* The draw packets are relocated in the opcode pass below. */
			draw_ranges = reloc->dword;
			draw_ranges_cpu = (const u8 *)vertex_bo->dma.vaddr + reloc->delta;
			vertex_object_draw = true;
			continue;
		}
		if (reloc->type == PARHELIA_RELOC_DRAW_VRAM) {
			struct parhelia_vertex_bo *vertex_bo;
			u32 range_end, bus_start, bus_end;

			if (!parhelia_vertex_objects(pdev)) {
				ret = -EOPNOTSUPP;
				goto out_unpin;
			}
			if (host_vertex_draw || args->vertex_dwords || draw_ranges ||
			    (packet_cmd[0] >> 24) != 0x97 || draw_relocated[reloc->packet] ||
			    packets[reloc->packet].length != 3 ||
			    (reloc->dword & 3) || !reloc->delta || (reloc->delta & 3) ||
			    reloc->delta > PARHELIA_MAX_DRAW_DWORDS * sizeof(u32) ||
			    check_add_overflow(reloc->dword, reloc->delta, &range_end)) {
				ret = -EINVAL;
				goto out_unpin;
			}
			/* A ring names one vertex object in every draw packet:
			 * the first relocation's reference keeps it alive for the
			 * whole request, a repeat neither looks it up nor holds it. */
			if (vertex_obj && reloc->handle == vertex_handle) {
				vertex_bo = vertex_cached;
			} else {
				objects[i] = drm_gem_object_lookup(file, reloc->handle);
				if (!objects[i]) {
					ret = -ENOENT;
					goto out_unpin;
				}
				vertex_bo = parhelia_vertex_bo_lookup(pdev, file,
								      objects[i]);
				if (!vertex_bo) {
					ret = -EACCES;
					goto out_unpin;
				}
				vertex_obj = objects[i];
				vertex_handle = reloc->handle;
				vertex_cached = vertex_bo;
			}
			/* CREATE_VERTEX owns a stable DMA32 allocation.  The GEM lookup held
			 * by this request prevents its bus address from being freed before
			 * the terminal fence retires. */
			if (range_end > vertex_bo->dma.base.size ||
			    check_add_overflow(lower_32_bits(vertex_bo->dma.dma_addr),
					       reloc->dword, &bus_start) ||
			    check_add_overflow(bus_start, reloc->delta, &bus_end)) {
				ret = -EINVAL;
				goto out_unpin;
			}
			packet_cmd[1] = bus_start;
			packet_cmd[2] = bus_end;
			draw_relocated[reloc->packet] = true;
			vertex_object_draw = true;
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
			if (!vertex_bo || reloc->delta >= vertex_bo->dma.base.size) {
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
			if (!vertex_bo || index_end > vertex_bo->dma.base.size) {
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
			    host_vertex_draw || vertex_object_draw ||
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
			if (!vertex_bo || reloc->delta >= vertex_bo->dma.base.size) {
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
		if (reloc->type == PARHELIA_RELOC_KERNEL_BUS) {
			if (!kernel_objects || reloc->dword >= packet->length ||
			    (packet_cmd[0] >> 24) != 0x00 ||
			    !test_bit(packet->offset + reloc->dword,
				      reloc_map->vram_ops)) {
				ret = -EINVAL;
				goto out_unpin;
			}
			packet_cmd[reloc->dword] = reloc->delta | 3;
			continue;
		}
		if (reloc->type != PARHELIA_RELOC_VRAM || reloc->dword >= packet->length ||
		    (packet_cmd[0] >> 24) != 0x00 ||
		    !test_bit(packet->offset + reloc->dword, reloc_map->vram_ops)) {
			ret = -EINVAL;
			goto out_unpin;
		}
		obj = objects[reloc_map->first[i]];
		if (!obj) {
			ret = -ENOENT;
			goto out_unpin;
		}
		if (reloc->delta >= obj->size) {
			ret = -EINVAL;
			goto out_unpin;
		}
		ret = parhelia_vram_gpu_addr(pdev, drm_gem_vram_of_gem(obj),
				reloc->delta, obj->size - reloc->delta,
				&packet_cmd[reloc->dword]);
		if (ret)
			goto out_unpin;
		if (trace_vram_reloc && reloc_map->first[i] == i) {
			struct parhelia_bo *pbo = to_parhelia_bo(obj);
			u32 target_reg = 0, bar_addr = 0;
			s64 node = drm_gem_vram_offset(&pbo->gbo);

			parhelia_vram_operand_reg(packet_cmd, packet->length,
				reloc->dword, &target_reg);
			parhelia_vram_bar_addr(pdev, &pbo->gbo, 0,
				obj->size, &bar_addr);
			dev_info(&pdev->pdev->dev,
				 "VRAM reloc pid=%d file=%p handle=%u reg=%#x layout=%u size=%zu node=%#llx bar=%#x delta=%#x gpu=%#x pin_count=%u\n",
				 task_pid_nr(current), file, reloc->handle, target_reg,
				 pbo->layout, obj->size, (unsigned long long)node,
				 bar_addr, reloc->delta, packet_cmd[reloc->dword],
				 pbo->gbo.bo.pin_count);
		}
	}
	parhelia_phase(&phase_t, PARHELIA_PHASE_RELOC, stats);
	if (local_vertex && host_vertex_draw)
		parhelia_local_vertex_streams(commands, packets, args->packet_count);
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
	/* CREATE_VERTEX uses the same class-3 stream state as copied @vertices.
	 * Its range operands differ, but no client-controlled bus address survives
	 * relocation and no BAR0 aperture descriptor is involved. */
	for (i = 0; i < args->packet_count; i++) {
		u32 opcode = plan->pkt_op[i];

		/* A 0x9f packet is admitted only after the paired stream/index
		 * relocations snapshot and patch it above, or as an indexed draw
		 * from the submit's vertex range. */
		if (opcode == 0x9b || (opcode == 0x9f && !mode7.index_seen)) {
			if (!draw_ranges || indexed_next >= validate.indexed_count ||
			    validate.indexed[indexed_next].packet != i) {
				ret = -EINVAL;
				goto out_unpin;
			}
			ret = async_slot && async_slot->index_cpu ?
				parhelia_relocate_indexed(pdev, commands,
					commands + packets[i].offset,
					&validate.indexed[indexed_next++],
					draw_ranges_cpu, draw_ranges,
					draw_ranges_bus, async_slot->index_cpu,
					lower_32_bits(async_slot->index_bus),
					PARHELIA_INDEX_DWORDS, &index_used,
					&stream0_patched) :
				parhelia_relocate_indexed(pdev, commands,
					commands + packets[i].offset,
					&validate.indexed[indexed_next++],
					draw_ranges_cpu, draw_ranges,
					draw_ranges_bus, vertex_cpu,
					lower_32_bits(vertex_bus),
					PARHELIA_VERTEX_DWORDS, &index_used,
					&stream0_patched);
			if (ret)
				goto out_unpin;
		}
		if (opcode == 0x8f && !mode3.stream_seen &&
		    !bound_range.stream_seen) {
			ret = -EINVAL;
			goto out_unpin;
		}
		if (opcode == 0x97 && draw_ranges) {
			/* a run shares one span */
			if (split_multi_draws && packets[i].length > 3 &&
			    parhelia_multi_draw_packet(commands, &packets[i])) {
				ret = -EINVAL;
				goto out_unpin;
			}
			for (u32 k = 0; k < packets[i].length; k += 3) {
				u32 *cmd = commands + packets[i].offset + k;
				u32 begin = cmd[1], end = cmd[2];

				if (((begin | end) & 3) || end <= begin ||
				    end - begin > PARHELIA_MAX_DRAW_DWORDS * sizeof(u32) ||
				    end > draw_ranges) {
					ret = -EINVAL;
					goto out_unpin;
				}
				cmd[1] = draw_ranges_bus + begin;
				cmd[2] = draw_ranges_bus + end;
			}
			draw_relocated[i] = true;
		}
		if (opcode == 0x97 && !draw_relocated[i]) {
			ret = -EINVAL;
			goto out_unpin;
		}
		/* the relocated operands give a draw's range */
		if (opcode == 0x97 || opcode == 0x9f || opcode == 0x9b)
			plan->pkt_draw[i] = PARHELIA_PKT_DRAW |
				(parhelia_multi_draw_packet(commands, &packets[i]) ?
				 PARHELIA_PKT_MULTI : 0);
	}
	parhelia_phase(&phase_t, PARHELIA_PHASE_ADMIT, stats);
	if (defer_packet_packing) {
		if (generic_packet_packing)
			parhelia_pack_adjacent_packet_calls(plan, commands,
							    packets,
							    args->packet_count,
							    call_boundaries,
							    split_multi_draws,
							    inline_draws);
		else
			parhelia_coalesce_draw_calls(plan, commands, packets,
						     draw_relocated,
						     args->packet_count,
						     split_multi_draws);
		ret = parhelia_plan_fits_hardware(plan);
		if (ret)
			goto out_unpin;
		total_calls = plan->count;
	}
	if (shadow_txn) {
		lockdep_assert_held(&pdev->submit_lock);
		parhelia_shadow_filter_plan(&pdev->reg_shadow, commands, packets,
			args->packet_count, relocs, args->reloc_count, plan, shadow_txn);
		total_calls = plan->count;
	}
	parhelia_phase(&phase_t, PARHELIA_PHASE_PACK, stats);
	/* In-stream FAA sync packets take counters between the opening fence
	 * and the drain: one section, replay cadence. */
	if (validate.faa_fences) {
		if (plan->sync_sections != 1 || full_sync || !replay_fences ||
		    validate.faa_first < opening_slot) {
			ret = -EINVAL;
			goto out_unpin;
		}
		fence_step = parhelia_faa_fence_step(validate.faa_fences);
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
			words = validate.faa_fences ?
				parhelia_faa_drain_words(validate.faa_fences) : 6;

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
		/* The last DWGCTL the request writes, from the validation pass;
		 * the register only when it writes none. */
		u32 last = validate.dwgctl_seen ? validate.dwgctl :
			   parhelia_read(pdev, PARHELIA_R_DWGCTL);

		for (i = 0; i < plan->sync_sections; i++) {
			u32 dwgctl = last;

			/* a first section ending before the last write rescans */
			if (i + 1 < plan->sync_sections && validate.dwgctl_seen &&
			    validate.dwgctl_packet >= boundary_packet)
				dwgctl = parhelia_batch_dwgctl_range(commands,
					packets, 0, boundary_packet,
					parhelia_read(pdev, PARHELIA_R_DWGCTL));
			parhelia_emit_fences(blocks, args->packet_count + i * 3,
				fence, fence + fence_step, dwgctl);
			if (validate.faa_fences) {
				parhelia_faa_patch_fences(commands, packets,
					args->packet_count, fence);
				parhelia_faa_drain(blocks[args->packet_count + i * 3 + 2].cpu,
					fence, validate.faa_fences, dwgctl);
			}
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
	parhelia_phase(&phase_t, PARHELIA_PHASE_SYNC, stats);
	first_draw = total_calls;
	for (i = 0; i < total_calls; i++) {
		u32 block_id = plan->blocks[i];

		if (block_id < args->packet_count &&
		    (plan->pkt_op[block_id] == 0x97 ||
		     plan->pkt_op[block_id] == 0x8f)) {
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
			u32 packet_span = plan->packet_spans[i];
			const void *inline_src = NULL;

			if (!packet_span || packet_span > args->packet_count - block_id) {
				ret = -EINVAL;
				goto out_unpin;
			}
			ret = parhelia_packet_span_dwords(packets, block_id,
							  packet_span, &words);
			if (ret || !words || words > PARHELIA_DMA_BLOCK_DWORDS) {
				if (!ret)
					ret = -EINVAL;
				goto out_unpin;
			}
			if (inline_draws)
				inline_src = parhelia_inline_draw_source(commands,
					&packets[block_id], draw_relocated[block_id],
					local_vertex ? NULL : vertex_cpu,
					lower_32_bits(vertex_bus),
					args->vertex_dwords * sizeof(u32),
					draw_ranges_cpu, draw_ranges_bus, draw_ranges,
					&inline_used);
			if (inline_src && (inline_used > inline_dwords ||
			    inline_used > PARHELIA_DMA_BLOCK_DWORDS - words))
				inline_src = NULL;
			if (inline_src) {
				inline_dwords -= inline_used;
				words += inline_used;
				WRITE_ONCE(p690_inline_draw_count,
					   p690_inline_draw_count + 1);
			} else {
				inline_used = 0;
			}
			if (async && heap && !inline_used)
				ret = parhelia_async_block_place(async_slot, block,
					packets[block_id].offset, words);
			else if (async)
				ret = parhelia_async_block_assign(async_slot, block,
					words);
			else if (!block->cpu) {
				block->cpu = dmam_alloc_coherent(&pdev->pdev->dev,
					PARHELIA_DMA_BLOCK_DWORDS * sizeof(u32), &block->bus, GFP_KERNEL);
				if (!block->cpu) {
					ret = -ENOMEM;
				}
			}
			if (ret)
				goto out_unpin;
			/* vertices, then the header whose end is its own address */
			if (inline_used) {
				u32 *header = block->cpu + inline_used;

				memcpy(block->cpu, inline_src, inline_used * sizeof(u32));
				parhelia_copy_packet_span(header, commands, packets,
							 block_id, packet_span);
				header[1] = lower_32_bits(block->bus);
				header[2] = lower_32_bits(block->bus) +
					    inline_used * sizeof(u32);
			} else if (!heap) {
				parhelia_copy_packet_span(block->cpu, commands, packets,
							 block_id, packet_span);
			}
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
					(post_draw_wb && sync_index == 1 ? 7 : 3) :
				validate.faa_fences ?
					parhelia_faa_drain_words(validate.faa_fences) : 6;
		}
		start = block->bus + inline_used * sizeof(u32);
		finish = block->bus + words * sizeof(u32);
		inline_used = 0;
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
	parhelia_phase(&phase_t, PARHELIA_PHASE_HEAP, stats);
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
		/* Fence publication is part of accepting the submission.  If any
		 * reservation cannot take the fence, abort before ringing the producer
		 * doorbell.  Already attached references are completed with this error
		 * by out_unpin, so no observer can wait forever. */
		if (ret)
			goto out_unpin;
	}
	parhelia_phase(&phase_t, PARHELIA_PHASE_FENCE, stats);
	if (args->vertex_dwords)
		parhelia_warp_trace_vertex(pdev, "before");
	root_probe = parhelia_root_probe_prepare(pdev, args, packets, relocs,
						 commands);
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
			if (submit_stats)
				atomic64_inc(&stats->ring_start_writes);
			pdev->primary_armed = true;
		}
		async_slot->root_probe = root_probe;
		root_probe = NULL;
		parhelia_write(pdev, PARHELIA_MMIO_RING_END,
			lower_32_bits(primary_bus + primary_words * sizeof(u32)) | 3);
		if (submit_stats)
			atomic64_inc(&stats->ring_end_writes);
		parhelia_write(pdev, 0x0314, 1);
		parhelia_shadow_commit(pdev, shadow_txn);

		async_slot->file = file;
		async_slot->fence = fence;
		async_slot->dma_tag = pdev->next_dma_tag;
		async_slot->primary_begin = pdev->primary_put;
		async_slot->primary_end = pdev->primary_put + primary_words;
		async_slot->call_count = total_calls;
		async_slot->submitted = jiffies;
		async_slot->submit_ns = ktime_get_ns();
		async_slot->budget =
			msecs_to_jiffies(max(hang_timeout_ms, 50u) *
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
		if (submit_stats)
			atomic64_inc(&stats->async_submits);
		parhelia_submit_high_water(pdev, pdev->queued_slots);
		pdev->next_fence = fence;
		if (!pdev->first_fence)
			pdev->first_fence = fence;
		args->fence = fence;
		mod_delayed_work(system_wq, &pdev->retire_work, msecs_to_jiffies(1));
		parhelia_phase(&phase_t, PARHELIA_PHASE_KICK, stats);
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
	if (!async && root_probe)
		parhelia_root_probe_complete(pdev, root_probe,
					     ret ? "sync-error" : "sync-complete");
	/* An async failure before the trace becomes slot-owned also ends here. */
	if (root_probe) {
		if (async && ret)
			parhelia_root_probe_complete(pdev, root_probe, "async-error");
		kfree(root_probe);
		root_probe = NULL;
	}
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
out_unlock:
	parhelia_submit_unlock(pdev);
out_relocs:
	if (lease)
		parhelia_async_file_turn_cancel(pdev, &file_waiter);
	/* Every exit after pinning lands here, including a slot wait interrupted
	 * by a signal: the restarted ioctl pins its targets again. */
	if (mode7.index_copy) {
		parhelia_vram_unpin(mode7.index_copy);
		drm_gem_object_put(&mode7.index_copy->bo.base);
	}
	for (i = 0; objects && pinned && i < args->reloc_count; i++) {
		if (pinned[i])
			parhelia_submit_unpin(pinned[i]);
		if (objects[i])
			drm_gem_object_put(objects[i]);
	}
	parhelia_reloc_map_put(pdev, reloc_map);
	kfree(objects);
	kfree(pinned);
	kfree(call_boundaries);
	kfree(draw_relocated);
	kvfree(validate.indexed);
	kvfree(relocs);
	relocs = NULL;
out_commands:
	kvfree(relocs);
	kfree(shadow_txn);
	parhelia_plan_put(pdev, plan);
	/* the heap holds the commands, or the slot's previous heap */
	if (heap)
		parhelia_heap_put(pdev, heap);
	else
		kvfree(commands);
	kvfree(packets);
	parhelia_phase(&phase_t, PARHELIA_PHASE_EXIT, stats);
	return ret;
}

int parhelia_submit_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_parhelia_submit *args = data;
	struct parhelia_submit_stats *stats = &to_parhelia(drm)->submit_stats;
	struct drm_parhelia_packet *packets;
	struct drm_parhelia_reloc *relocs = NULL;
	u64 start = submit_stats ? ktime_get_ns() : 0, t = start;
	struct parhelia_cmd_heap heap = { };
	bool in_place;
	u32 *commands;
	int ret;

	ret = parhelia_submit_check(args);
	if (ret)
		return ret;
	packets = memdup_user(u64_to_user_ptr(args->packets),
			      array_size(args->packet_count, sizeof(*packets)));
	if (IS_ERR(packets))
		return PTR_ERR(packets);
	/* An async request's commands go straight into a spare command heap,
	 * where they are validated and fetched in place. */
	in_place = (args->flags & PARHELIA_SUBMIT_ASYNC) &&
		   !READ_ONCE(p690_inline_draws) &&
		   parhelia_heap_take(to_parhelia(drm), &heap);
	if (in_place) {
		commands = heap.cpu;
		if (copy_from_user(commands, u64_to_user_ptr(args->commands),
				   args->command_dwords * sizeof(*commands))) {
			parhelia_heap_put(to_parhelia(drm), &heap);
			kvfree(packets);
			return -EFAULT;
		}
	} else {
		commands = memdup_user(u64_to_user_ptr(args->commands),
				       array_size(args->command_dwords,
						  sizeof(*commands)));
	}
	if (IS_ERR(commands)) {
		kvfree(packets);
		return PTR_ERR(commands);
	}
	if (args->reloc_count) {
		relocs = memdup_user(u64_to_user_ptr(args->relocs),
				     array_size(args->reloc_count, sizeof(*relocs)));
		if (IS_ERR(relocs)) {
			if (in_place)
				parhelia_heap_put(to_parhelia(drm), &heap);
			else
				kvfree(commands);
			kvfree(packets);
			return PTR_ERR(relocs);
		}
	}
	parhelia_phase(&t, PARHELIA_PHASE_COPYIN, stats);
	ret = parhelia_submit_run(to_parhelia(drm), file, args, packets,
				  commands, relocs, NULL, NULL,
				  in_place ? &heap : NULL);
	parhelia_phase(&start, PARHELIA_PHASE_TOTAL, stats);
	return ret;
}

/* The largest power-of-two width up to @max_width pixels whose whole tile
 * rows fit in @bytes; 0 when not even a 64-pixel tile row does. */
static u32 parhelia_clear_width(u64 bytes, u32 tile_rows, u32 max_width)
{
	u32 width;

	for (width = max_width; width >= 64; width >>= 1)
		if ((u64)width * 4 * tile_rows <= bytes)
			return width;
	return 0;
}

/* Zero a new, pinned object with the 2-D solid fill of p3d_emit_clear()
 * rather than uncached CPU stores through the aperture (4 MiB took 30 ms).
 * The fill is queued like a client submit of @file, so the object's
 * reservation carries its fence for CPU views; a tail below one tile row
 * and a device that cannot take the fill use the CPU clear. */
int parhelia_bo_clear(struct parhelia_device *pdev, struct drm_file *file,
		      struct drm_gem_vram_object *gbo)
{
	/* Fill extents stay within the 2048x2048 targets clients clear. */
	const u32 max_extent = 2048;
	u32 tile_rows = pdev->chip->tile_layout == PARHELIA_TILE_64X16_4K ? 16 : 32;
	u64 size = gbo->bo.base.size, offset = 0, chunk;
	struct drm_parhelia_submit args = { };
	struct drm_parhelia_packet *packets;
	struct drm_parhelia_reloc *relocs;
	struct drm_gem_object **objects;
	u32 *commands, gpu, fills = 0, i, width, rows;
	int ret;

	if (!file || !pdev->raw_aperture_owned ||
	    parhelia_vram_gpu_addr(pdev, gbo, 0, size, &gpu))
		return parhelia_vram_clear(pdev, gbo);
	for (chunk = size; parhelia_clear_width(chunk, tile_rows, max_extent);
	     fills++) {
		width = parhelia_clear_width(chunk, tile_rows, max_extent);
		rows = min_t(u64, chunk / (width * 4), max_extent);
		chunk -= (u64)ALIGN_DOWN(rows, tile_rows) * width * 4;
	}
	if (!fills || fills > PARHELIA_LEGACY_USER_PACKETS)
		return parhelia_vram_clear(pdev, gbo);

	packets = kvcalloc(fills, sizeof(*packets), GFP_KERNEL);
	commands = kvcalloc(fills, 15 * sizeof(*commands), GFP_KERNEL);
	relocs = kvcalloc(fills * 2, sizeof(*relocs), GFP_KERNEL);
	objects = kcalloc(fills * 2, sizeof(*objects), GFP_KERNEL);
	if (!packets || !commands || !relocs || !objects) {
		kvfree(packets);
		kvfree(commands);
		kvfree(relocs);
		kfree(objects);
		return -ENOMEM;
	}
	for (i = 0; i < fills; i++) {
		u32 *cmd = commands + i * 15;
		u32 cfg;

		width = parhelia_clear_width(size - offset, tile_rows, max_extent);
		rows = ALIGN_DOWN(min_t(u64, (size - offset) / (width * 4),
					max_extent), tile_rows);
		cfg = width | (2u << 16) | (0x32u << 24);
		cmd[0] = 13;
		cmd[1] = 0x8b8a8988;	/* SURF0 base/cfg, SURF1 base/cfg */
		cmd[3] = cfg;
		cmd[5] = cfg;
		cmd[6] = 0xc5c0c9b0;	/* 0x12c0, PLNWT, DWGCTL, FCOL */
		cmd[7] = 0xffffffff;
		cmd[8] = 0xffffffff;
		cmd[9] = 0xc0;		/* opaque solid fill */
		cmd[11] = 0x21e3c2c1;	/* SRC, DSTXY, LEN fires */
		cmd[14] = (rows << 16) | width;
		packets[i].offset = i * 15;
		packets[i].length = 15;
		relocs[2 * i] = (struct drm_parhelia_reloc){
			.packet = i, .dword = 2, .delta = offset,
			.type = PARHELIA_RELOC_VRAM };
		relocs[2 * i + 1] = relocs[2 * i];
		relocs[2 * i + 1].dword = 4;
		objects[2 * i] = &gbo->bo.base;
		objects[2 * i + 1] = &gbo->bo.base;
		offset += (u64)rows * width * 4;
	}
	args.command_dwords = fills * 15;
	args.packet_count = fills;
	args.reloc_count = fills * 2;
	if (pdev->async_primary_cpu && coalesce_primary && !trace_submit &&
	    !full_sync && !post_draw_wb)
		args.flags = PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC;
	ret = parhelia_submit_check(&args);
	if (!ret)
		ret = parhelia_submit_run(pdev, file, &args, packets, commands,
					  relocs, objects, NULL, NULL);
	else {
		kvfree(packets);
		kvfree(commands);
		kvfree(relocs);
	}
	kfree(objects);
	if (ret == -EINTR || ret == -ERESTARTSYS)
		return ret;
	if (ret) {
		dev_warn_ratelimited(&pdev->pdev->dev,
			"engine clear of %llu bytes failed (%d), clearing by CPU\n",
			(unsigned long long)size, ret);
		return parhelia_vram_clear(pdev, gbo);
	}
	return parhelia_vram_clear_range(pdev, gbo, offset, size - offset);
}

/* Every P690 program that does not write WARP T2 - the RGBA, textured and
 * transform programs - inherits it, and the setup unit rejects a vertex
 * against it like a clip-space position (-w <= x,y,z <= w, w > 0).  The latch
 * exists once per primitive slot of a draw packet, and after power-on the
 * slots past the first held rejecting values: Mesa's two-triangle packets
 * lost their second triangle until some client happened to draw a
 * multi-primitive pass-through packet (docs/parhelia-raster-clip.md).  The
 * pass-through writes T2 from the vertex colour, so one packet of
 * PARHELIA_T2_PRIME_SLOTS black triangles (T2 = (0,0,0,1)) primes every slot
 * a client can use (Mesa packs at most eight).  The words are the Mesa state
 * emitter's own image for that draw into a 64x16 target (parhelia-drm-test
 * --t2-prime-dump 8 64); the WARP entry, pitch and guard band are patched. */
#define PARHELIA_T2_PRIME_SLOTS	8u
/* The original family's clients pack at most two primitives per packet,
 * and only a two-primitive prime is measured there (0528). */
#define PARHELIA_T2_PRIME_SLOTS_ORIGINAL	2u

static u32 parhelia_t2_prime_slots(const struct parhelia_device *pdev)
{
	return parhelia_original_family(pdev) ?
	       PARHELIA_T2_PRIME_SLOTS_ORIGINAL : PARHELIA_T2_PRIME_SLOTS;
}

static const u32 parhelia_t2_prime_words[] = {
		0x00000023, 0xbebfcbca, 0x00000000, 0x00100040, 0x0bb80024, 0x007fe000,
		0x2121210a, 0x0001ffff, 0x21b7b6b5, 0x00000000, 0x00000000, 0x00ffffff,
		0xbbbcb9ba, 0x01800202, 0x0000000e, 0x00000000, 0x00000000, 0xb0b4b3b2,
		0x001000e4, 0x000000ff, 0x000000ff, 0x80000000, 0x212121b1, 0x7fffffff,
		0x21b7b6b5, 0x00000000, 0x00000000, 0x00ffffff, 0x4b4a4948, 0x45ffc5ff,
		0xc5ff45ff, 0x00000000, 0x3f800000, 0x21464544, 0x00000003, 0x00287210,
		0x00006543, 0x00000008, 0x212121dd, 0x00000000, 0x41403c3c, 0x20000002,
		0x20000002, 0x7e57e000, 0x7e57e000, 0x2121213e, 0x00000002, 0x00000008,
		0x3c3c3cdd, 0x00000000, 0x20000002, 0x20000002, 0x00000002, 0x2151474c,
		0x0200200c, 0x00000fdb, 0x00000000, 0x00000003, 0x212d1819, 0x00000000,
		0x000280cc, 0x00000000, 0x00000003, 0x212e2e2e, 0x20c00000, 0xa1c50020,
		0x00000000, 0x00000004, 0x8d8c8988, 0x00000000, 0x32020040, 0x00000000,
		0x32020000, 0x00000003, 0x21c9c5c6, 0x00000000, 0x00000000, 0xffffffff,
		0x00000001, 0x212121ac, 0x3fff8842, 0x00000001, 0x212121c0, 0x00a60ec4,
		0x00000001, 0x212121bd, 0x42000000, 0x00000006, 0x82818087, 0x00000000,
		0x0001000f, 0x00000000, 0x00000000, 0x21212183, 0x00000000, 0x0000001d,
		0x9a9b9899, 0x00000200, 0x00000200, 0x01400400, 0x01400400, 0xa1a09f9e,
		0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x9a9b9899, 0x00000082,
		0x00000080, 0x00400400, 0x00400400, 0x9a9b9899, 0x00000084, 0x00000080,
		0x00400400, 0x00400400, 0x9a9b9899, 0x00000088, 0x00000080, 0x00400400,
		0x00400400, 0x9a9b9899, 0x0000008a, 0x00000080, 0x00400400, 0x00400400,
		0x00000001, 0x2121212c, 0x0006305c, 0x00000002, 0x21211000, 0x00000003,
		0x00000004, 0x97080001, 0x00000000, 0x00000000,
};

static const struct { u16 offset, length; } parhelia_t2_prime_packets[] = {
		{ 0, 37 }, { 37, 10 }, { 47, 10 }, { 57, 5 },
		{ 62, 5 }, { 67, 6 }, { 73, 5 }, { 78, 3 },
		{ 81, 3 }, { 84, 3 }, { 87, 8 }, { 95, 31 },
		{ 126, 3 }, { 129, 4 }, { 133, 3 },
};

/* IEEE single bits of a small non-negative integer, without kernel FPU use */
static u32 parhelia_f32_from_small(u32 n)
{
	u32 e = n ? fls(n) - 1 : 0;

	return n ? ((127 + e) << 23) | ((n << (23 - e)) & 0x7fffff) : 0;
}

static int parhelia_t2_prime(struct parhelia_device *pdev, struct drm_file *file,
			     struct drm_gem_vram_object *gbo, u32 pitch_pixels)
{
	const u32 npackets = ARRAY_SIZE(parhelia_t2_prime_packets);
	const u32 ndwords = ARRAY_SIZE(parhelia_t2_prime_words);
	struct drm_parhelia_submit args = { };
	struct drm_parhelia_packet *packets;
	struct drm_parhelia_reloc *relocs;
	struct drm_gem_object *objects[3] = { };
	u32 vertices[PARHELIA_T2_PRIME_SLOTS * 3 * 5] = { };
	const u32 slots = parhelia_t2_prime_slots(pdev);
	u32 *commands, v;
	int ret;

	packets = kvcalloc(npackets, sizeof(*packets), GFP_KERNEL);
	commands = kvmemdup(parhelia_t2_prime_words, sizeof(parhelia_t2_prime_words),
			    GFP_KERNEL);
	relocs = kvcalloc(3, sizeof(*relocs), GFP_KERNEL);
	if (!packets || !commands || !relocs) {
		kvfree(packets);
		kvfree(commands);
		kvfree(relocs);
		return -ENOMEM;
	}
	for (v = 0; v < npackets; v++) {
		packets[v].offset = parhelia_t2_prime_packets[v].offset;
		packets[v].length = parhelia_t2_prime_packets[v].length;
	}
	commands[43] = commands[44] =
		parhelia_warp_entry(pdev, PARHELIA_WARP_PASSTHROUGH_ENTRY);
	commands[70] = pitch_pixels | (2u << 16) | (0x32u << 24);
	/* the vendor's LX guard band, KRN mtx2572; the original keeps 32.0 */
	if (!parhelia_original_family(pdev))
		commands[86] = 0x41f80000;
	/* x, y, r, g, b: distinct small triangles, colour black */
	for (v = 0; v < slots * 3; v++) {
		vertices[v * 5] = parhelia_f32_from_small(
			(v / 3) * 4 % 60 + (v % 3 == 1) * 3);
		vertices[v * 5 + 1] = parhelia_f32_from_small((v % 3 == 2) * 3);
	}
	relocs[0] = (struct drm_parhelia_reloc){
		.packet = 5, .dword = 2, .type = PARHELIA_RELOC_VRAM };
	relocs[1] = (struct drm_parhelia_reloc){
		.packet = 5, .dword = 4, .type = PARHELIA_RELOC_VRAM };
	relocs[2] = (struct drm_parhelia_reloc){
		.packet = npackets - 1, .dword = 0,
		.delta = slots * 3 * 5 * sizeof(u32), .type = PARHELIA_RELOC_DRAW };
	objects[0] = objects[1] = &gbo->bo.base;
	args.command_dwords = ndwords;
	args.packet_count = npackets;
	args.reloc_count = 3;
	args.vertices = (uintptr_t)vertices;
	args.vertex_dwords = slots * 3 * 5;
	args.flags = PARHELIA_SUBMIT_FENCE;
	ret = parhelia_submit_check(&args);
	if (!ret)
		return parhelia_submit_run(pdev, file, &args, packets, commands,
					   relocs, objects, vertices, NULL);
	kvfree(packets);
	kvfree(commands);
	kvfree(relocs);
	return ret;
}

/* Prime the T2 slots once per engine life, with the first native colour
 * target large enough, before that target's clear makes it zero again. */
void parhelia_t2_prime_once(struct parhelia_device *pdev, struct drm_file *file,
			    struct drm_gem_vram_object *gbo, u32 width,
			    u32 height, u32 pitch_pixels)
{
	int ret;

	if ((pdev->pdev->device != PARHELIA_P690 &&
	     !parhelia_original_family(pdev)) || pdev->t2_primed ||
	    width < 64 || height < 16)
		return;
	ret = parhelia_t2_prime(pdev, file, gbo, pitch_pixels);
	if (!ret) {
		pdev->t2_primed = true;
		dev_info(&pdev->pdev->dev,
			 "WARP T2 prime: %u slots on a %ux%u target\n",
			 parhelia_t2_prime_slots(pdev), width, height);
	}
	else if (ret != -EINTR && ret != -ERESTARTSYS)
		dev_warn_ratelimited(&pdev->pdev->dev,
				     "WARP T2 prime failed (%d)\n", ret);
}

static bool parhelia_fence_done_or_wedged(struct parhelia_device *pdev,
					  u32 fence);

static int parhelia_staging_get(struct parhelia_device *pdev)
{
	size_t size;

	lockdep_assert_held(&pdev->staging_lock);
	if (pdev->staging_cpu)
		return 0;
	for (size = SZ_4M; size >= SZ_64K; size >>= 1) {
		pdev->staging_cpu = dmam_alloc_coherent(&pdev->pdev->dev, size,
				&pdev->staging_bus, GFP_KERNEL | __GFP_NOWARN);
		if (pdev->staging_cpu) {
			pdev->staging_size = size;
			return 0;
		}
	}
	return -ENOMEM;
}

/* One linear 32 bpp BITBLT row band: GXcopy from slot 2 to slot 0, the
 * vendor's mtx3388 readback form (SURF0 = page bus address | 3). */
static u32 *parhelia_emit_linear_blit(u32 *cmd, u32 x, u32 y, u32 w, u32 h)
{
	/* type 1 (linear), format 0x30, 1024-pixel pitch: one 4 KiB row */
	const u32 cfg = 1024 | (1u << 16) | (0x30u << 24);

	cmd[0] = 11;
	cmd[1] = 0x8d8c8988;	/* SURF0 base/cfg, SURF2 base/cfg */
	cmd[2] = 0;
	cmd[3] = cfg;
	cmd[4] = 0;
	cmd[5] = cfg;
	cmd[6] = 0xc2c1c0c9;	/* PLNWT, DWGCTL, SRC, DSTXY */
	cmd[7] = 0xffffffff;
	cmd[8] = 0x2c2;
	cmd[9] = (y << 16) | x;
	cmd[10] = (y << 16) | x;
	cmd[11] = 0x212121e3;	/* LEN fires */
	cmd[12] = (h << 16) | w;
	return cmd + 13;
}

/* Submit the blits moving @bytes at @skew past the 4 KiB-aligned VRAM
 * @anchor of @gbo to or from the class-3 buffer at @bus: both ends are
 * linear grids of 4 KiB rows, so a range is a partial head row, whole rows
 * and a partial tail row.  @scratch_bus is a page the trailing flush blit
 * may overwrite.  Async requests return after queueing with *@fence set. */
static int parhelia_engine_blit_submit(struct parhelia_device *pdev,
				       struct drm_file *file,
				       struct drm_gem_vram_object *gbo,
				       u64 anchor, u32 skew, u32 bytes,
				       dma_addr_t bus, dma_addr_t scratch_bus,
				       bool download, bool async, u32 *fence)
{
	struct drm_parhelia_submit args = { };
	struct drm_parhelia_packet *packets;
	struct drm_parhelia_reloc *relocs;
	struct drm_gem_object *objects[8] = { };
	u32 x, y, w, rows, n = 0, i;
	u32 *commands, *cmd;
	int ret;

	packets = kvcalloc(4, sizeof(*packets), GFP_KERNEL);
	commands = kvcalloc(4 * 13, sizeof(*commands), GFP_KERNEL);
	relocs = kvcalloc(8, sizeof(*relocs), GFP_KERNEL);
	if (!packets || !commands || !relocs) {
		kvfree(packets);
		kvfree(commands);
		kvfree(relocs);
		return -ENOMEM;
	}
	cmd = commands;
	x = skew / 4;
	y = 0;
	for (i = 0; i < bytes / 4; i += w * rows) {
		u32 left = bytes / 4 - i;

		if (x) {
			w = min(1024 - x, left);
			rows = 1;
		} else if (left >= 1024) {
			w = 1024;
			rows = left / 1024;
		} else {
			w = left;
			rows = 1;
		}
		cmd = parhelia_emit_linear_blit(cmd, x, y, w, rows);
		packets[n].offset = n * 13;
		packets[n].length = 13;
		/* SURF0 is the destination, SURF2 the source. */
		relocs[2 * n] = (struct drm_parhelia_reloc){
			.packet = n, .dword = download ? 2 : 4,
			.delta = lower_32_bits(bus),
			.type = PARHELIA_RELOC_KERNEL_BUS };
		relocs[2 * n + 1] = (struct drm_parhelia_reloc){
			.packet = n, .dword = download ? 4 : 2,
			.delta = anchor, .type = PARHELIA_RELOC_VRAM };
		objects[2 * n + 1] = &gbo->bo.base;
		n++;
		x = (x + w) % 1024;
		y += x ? 0 : rows;
	}
	/* The engine's last ~2 KiB of class-3 writes were still in flight
	 * at the fence (MEASURED): push them out with one row into the
	 * scratch page, as the vendor's one-row-late readback (mtx3388)
	 * implicitly does. */
	cmd = parhelia_emit_linear_blit(cmd, 0, 0, 1024, 1);
	packets[n].offset = n * 13;
	packets[n].length = 13;
	relocs[2 * n] = (struct drm_parhelia_reloc){
		.packet = n, .dword = 2, .delta = lower_32_bits(scratch_bus),
		.type = PARHELIA_RELOC_KERNEL_BUS };
	relocs[2 * n + 1] = (struct drm_parhelia_reloc){
		.packet = n, .dword = 4, .delta = anchor,
		.type = PARHELIA_RELOC_VRAM };
	objects[2 * n + 1] = &gbo->bo.base;
	n++;
	args.command_dwords = n * 13;
	args.packet_count = n;
	args.reloc_count = n * 2;
	if (async)
		args.flags = PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC;
	ret = parhelia_submit_check(&args);
	if (!ret)
		ret = parhelia_submit_run(pdev, file, &args, packets,
					  commands, relocs, objects, NULL, NULL);
	else {
		kvfree(packets);
		kvfree(commands);
		kvfree(relocs);
	}
	if (!ret && fence)
		*fence = args.fence;
	return ret;
}

/* Move [@offset, @offset + @size) of a VRAM object to or from @data with the
 * 2-D engine and a coherent class-3 staging buffer, instead of uncached CPU
 * accesses through the aperture (downloads ran at 18 MB/s). */
int parhelia_engine_transfer(struct parhelia_device *pdev,
			     struct drm_file *file,
			     struct drm_gem_vram_object *gbo, u64 offset,
			     void *data, u64 size, bool download)
{
	u64 done = 0;
	int ret;

	if (!IS_ALIGNED(offset, 4) || !IS_ALIGNED(size, 4) ||
	    offset > gbo->bo.base.size || size > gbo->bo.base.size - offset)
		return -EINVAL;
	mutex_lock(&pdev->staging_lock);
	ret = parhelia_staging_get(pdev);
	while (!ret && done < size) {
		u64 pos = offset + done, anchor = ALIGN_DOWN(pos, SZ_4K);
		u32 skew = pos - anchor;
		u32 bytes = min_t(u64, size - done,
				  pdev->staging_size - SZ_4K - skew);

		if (!download)
			memcpy((u8 *)pdev->staging_cpu + skew, (u8 *)data + done,
			       bytes);
		ret = parhelia_engine_blit_submit(pdev, file, gbo, anchor, skew,
				bytes, pdev->staging_bus,
				pdev->staging_bus + pdev->staging_size - SZ_4K,
				download, false, NULL);
		if (ret)
			break;
		if (download)
			memcpy((u8 *)data + done,
			       (u8 *)pdev->staging_cpu + skew, bytes);
		done += bytes;
	}
	mutex_unlock(&pdev->staging_lock);
	return ret;
}

struct parhelia_upload_span {
	struct list_head link;
	size_t start, end;
	u32 fence;
};

static int parhelia_upload_ring_get(struct parhelia_device *pdev)
{
	size_t size;

	lockdep_assert_held(&pdev->upload_lock);
	if (pdev->upload_cpu)
		return 0;
	for (size = SZ_4M; size >= SZ_64K; size >>= 1) {
		pdev->upload_cpu = dmam_alloc_coherent(&pdev->pdev->dev, size,
				&pdev->upload_bus, GFP_KERNEL | __GFP_NOWARN);
		if (pdev->upload_cpu) {
			pdev->upload_size = size;
			pdev->upload_head = 0;
			return 0;
		}
	}
	return -ENOMEM;
}

static void parhelia_upload_ring_drop(struct parhelia_device *pdev,
				      struct parhelia_upload_span *span)
{
	list_del(&span->link);
	kfree(span);
}

/* Reserve @len bytes (4 KiB multiple) of the upload ring, waiting for the
 * oldest in-flight spans' fences; the last page is the flush scratch. */
static int parhelia_upload_ring_alloc(struct parhelia_device *pdev,
				      size_t len, size_t *start)
{
	const size_t limit = pdev->upload_size - SZ_4K;

	if (len > limit)
		return -E2BIG;
	for (;;) {
		struct parhelia_upload_span *oldest;
		size_t head = pdev->upload_head, tail;
		int ret;

		while ((oldest = list_first_entry_or_null(&pdev->upload_busy,
				struct parhelia_upload_span, link)) &&
		       parhelia_fence_done_or_wedged(pdev, oldest->fence))
			parhelia_upload_ring_drop(pdev, oldest);
		if (READ_ONCE(pdev->wedged))
			return -EIO;
		if (!oldest) {
			*start = head + len <= limit ? head : 0;
			return 0;
		}
		tail = oldest->start;
		if (head > tail) {
			if (head + len <= limit) {
				*start = head;
				return 0;
			}
			if (len <= tail) {
				*start = 0;
				return 0;
			}
		} else if (head < tail && head + len <= tail) {
			*start = head;
			return 0;
		}
		ret = wait_event_interruptible(pdev->submit_waitq,
			parhelia_fence_done_or_wedged(pdev, oldest->fence));
		if (ret)
			return ret;
	}
}

/* Async P690 upload: copy the caller's data once into the coherent ring and
 * queue the blits under a fence without waiting.  Later submits follow in
 * ring order; CPU views and the aperture path order against the fence the
 * submit attached to the object.  Spans recycle by fence. */
int parhelia_engine_upload_async(struct parhelia_device *pdev,
				 struct drm_file *file,
				 struct drm_gem_vram_object *gbo, u64 offset,
				 const void __user *data, u64 size)
{
	u64 done = 0;
	int ret;

	if (!IS_ALIGNED(offset, 4) || !IS_ALIGNED(size, 4) ||
	    offset > gbo->bo.base.size || size > gbo->bo.base.size - offset)
		return -EINVAL;
	if (!pdev->async_primary_cpu || !coalesce_primary || trace_submit ||
	    full_sync || post_draw_wb)
		return -EOPNOTSUPP;
	mutex_lock(&pdev->upload_lock);
	ret = parhelia_upload_ring_get(pdev);
	while (!ret && done < size) {
		struct parhelia_upload_span *span;
		u64 pos = offset + done, anchor = ALIGN_DOWN(pos, SZ_4K);
		u32 skew = pos - anchor, bytes;
		size_t start;

		/* Quarter-ring chunks let the copy of one overlap the blit of
		 * the previous. */
		bytes = min_t(u64, size - done,
			      (pdev->upload_size - SZ_4K) / 4 - skew);
		span = kmalloc(sizeof(*span), GFP_KERNEL);
		if (!span) {
			ret = -ENOMEM;
			break;
		}
		ret = parhelia_upload_ring_alloc(pdev,
				ALIGN(skew + bytes, SZ_4K), &start);
		if (!ret && copy_from_user((u8 *)pdev->upload_cpu + start + skew,
					   (const u8 __user *)data + done, bytes))
			ret = -EFAULT;
		if (!ret)
			ret = parhelia_engine_blit_submit(pdev, file, gbo, anchor,
					skew, bytes, pdev->upload_bus + start,
					pdev->upload_bus + pdev->upload_size -
						SZ_4K,
					false, true, &span->fence);
		if (ret) {
			kfree(span);
			break;
		}
		span->start = start;
		span->end = start + ALIGN(skew + bytes, SZ_4K);
		list_add_tail(&span->link, &pdev->upload_busy);
		pdev->upload_head = span->end;
		done += bytes;
	}
	mutex_unlock(&pdev->upload_lock);
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
		(s32)(parhelia_fence_completed(pdev) - fence) >= 0;
}

int parhelia_wait_fence_ioctl(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct parhelia_device *pdev = to_parhelia(drm);
	struct drm_parhelia_wait_fence *args = data;
	u64 wait_begin = 0;
	ktime_t poll_end;
	long waited;
	unsigned long timeout;
	bool issued;

	if (args->flags || args->pad || args->timeout_ns < -1)
		return -EINVAL;
	args->completed = parhelia_fence_completed(pdev);
	parhelia_submit_lock(pdev);
	parhelia_retire_locked(pdev);
	issued = parhelia_fence_is_issued_locked(pdev, args->fence);
	parhelia_submit_unlock(pdev);
	if (!issued)
		return -EINVAL;

	if (parhelia_fence_done_or_wedged(pdev, args->fence))
		goto done;
	if (!args->timeout_ns)
		return -ETIME;
	if (submit_stats) {
		wait_begin = ktime_get_ns();
		atomic64_inc(&pdev->submit_stats.fence_waits);
	}
	/* Retirement runs every jiffy.  Most render waits are shorter, so poll
	 * the fence register at hrtimer resolution for up to 2 ms first. */
	poll_end = ktime_add_us(ktime_get(), 2000);
	while (!parhelia_fence_done_or_wedged(pdev, args->fence) &&
	       ktime_before(ktime_get(), poll_end)) {
		if (signal_pending(current))
			return -ERESTARTSYS;
		usleep_range(40, 80);
	}
	if (parhelia_fence_done_or_wedged(pdev, args->fence))
		goto accounted;
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
accounted:
	if (wait_begin)
		atomic64_add(ktime_get_ns() - wait_begin,
			     &pdev->submit_stats.fence_wait_ns);
done:
	args->completed = parhelia_fence_completed(pdev);
	return READ_ONCE(pdev->wedged) ? -EIO : 0;
}

#define PARHELIA_QUIESCE_MS		2000u
#define PARHELIA_BM_PRIMARY_IDLE	BIT(17)
#define PARHELIA_BM_WARP_BUSY		(BIT(24) | BIT(25))

/* Teardown, as the vendor's context close does it: wait for every issued
 * fence and an idle fetcher before bus mastering is revoked and the memory
 * it reads is freed, then refuse further submits. */
bool parhelia_submit_quiesce(struct parhelia_device *pdev)
{
	unsigned long end = jiffies + msecs_to_jiffies(PARHELIA_QUIESCE_MS);
	bool idle = false;
	u32 bm;

	parhelia_submit_lock(pdev);
	for (;;) {
		if (!pdev->wedged)
			parhelia_retire_locked(pdev);
		bm = parhelia_read(pdev, 0x08f0);
		if (!pdev->wedged && !pdev->queued_slots &&
		    (bm & PARHELIA_BM_PRIMARY_IDLE) && !(bm & PARHELIA_BM_WARP_BUSY)) {
			idle = true;
			break;
		}
		if (pdev->wedged || time_after(jiffies, end))
			break;
		parhelia_submit_unlock(pdev);
		usleep_range(200, 400);
		parhelia_submit_lock(pdev);
	}
	pdev->wedged = true;
	parhelia_state_shadow_invalidate(pdev, "submit teardown");
	parhelia_submit_unlock(pdev);
	cancel_delayed_work_sync(&pdev->retire_work);
	wake_up_all(&pdev->submit_waitq);
	dev_info(&pdev->pdev->dev,
		 "teardown: engine %s, BM=%08x prim=%08x/%08x fence=%08x queued=%u\n",
		 idle ? "idle" : "NOT idle", bm,
		 parhelia_read(pdev, PARHELIA_MMIO_RING_START),
		 parhelia_read(pdev, PARHELIA_MMIO_RING_END),
		 parhelia_read(pdev, PARHELIA_MMIO_FENCE_DONE), pdev->queued_slots);
	return idle;
}

void parhelia_submit_fini(struct parhelia_device *pdev)
{
	struct parhelia_vertex_bo *entry, *tmp;
	struct parhelia_upload_span *span;
	u32 i;

	cancel_delayed_work_sync(&pdev->retire_work);
	kvfree(xchg(&pdev->plan_cache, NULL));
	parhelia_reloc_map_free(xchg(&pdev->reloc_map_cache, NULL));
	parhelia_submit_lock(pdev);
	pdev->wedged = true;
	parhelia_state_shadow_invalidate(pdev, "submit teardown");
	/* Teardown only: block external DMA before devres frees coherent memory. */
	pci_clear_master(pdev->pdev);
	/* pcim_enable_device owns the final disable; doing it here as well
	 * decrements the enable count twice during managed teardown.
	 */
	for (i = 0; i < pdev->quarantine_count; i++) {
		if (pdev->quarantine_pinned[i])
			parhelia_submit_unpin(pdev->quarantine_pinned[i]);
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
		parhelia_local_vertex_fini(slot);
	}
	list_for_each_entry_safe(entry, tmp, &pdev->vertex_bos, link) {
		list_del(&entry->link);
		drm_gem_object_put(&entry->dma.base);
	}
	parhelia_submit_unlock(pdev);
	while (pdev->spare_count) {
		struct parhelia_cmd_heap *heap =
			&pdev->spare_heaps[--pdev->spare_count];

		dma_free_coherent(&pdev->pdev->dev, heap->dwords * sizeof(u32),
				  heap->cpu, heap->bus);
	}
	parhelia_vertex_pool_fini(pdev);
	mutex_lock(&pdev->upload_lock);
	while ((span = list_first_entry_or_null(&pdev->upload_busy,
			struct parhelia_upload_span, link)))
		parhelia_upload_ring_drop(pdev, span);
	mutex_unlock(&pdev->upload_lock);
}

void parhelia_submit_postclose(struct parhelia_device *pdev,
			   struct drm_file *file)
{
	struct parhelia_vertex_bo *entry, *tmp;
	u32 i;

retry:
	parhelia_submit_lock(pdev);
	parhelia_retire_locked(pdev);
	if (!pdev->queued_slots && pdev->queue_owner == file)
		pdev->queue_owner = NULL;
	if (!pdev->queue_owner)
		wake_up_all(&pdev->submit_waitq);
	if (!pdev->wedged) {
		for (i = 0; i < PARHELIA_ASYNC_SLOTS; i++) {
			struct parhelia_submit_slot *slot = &pdev->slots[i];

			if (slot->state == PARHELIA_SLOT_QUEUED && slot->file == file) {
				parhelia_submit_unlock(pdev);
				/* postclose must not release a CREATE_VERTEX allocation while an
				 * in-flight class-3 batch still owns its GEM lookup.  A retired
				 * slot can be reused by another DRM client before this waiter runs;
				 * ownership changing away from @file is therefore completion for
				 * this close, even when the slot is already queued again. */
				wait_event(pdev->submit_waitq,
					READ_ONCE(pdev->wedged) ||
					READ_ONCE(slot->state) != PARHELIA_SLOT_QUEUED ||
					READ_ONCE(slot->file) != file);
				goto retry;
			}
		}
	}

	list_for_each_entry_safe(entry, tmp, &pdev->vertex_bos, link) {
		if (entry->file != file)
			continue;
		/* A timeout quarantines every object which may still be read by the
		 * onboard engine. Keep the allocation until module teardown; PCI
		 * mastering is already disabled, but freeing a live DMA target is unsafe. */
		if (pdev->wedged) {
			entry->file = NULL;
			continue;
		}
		list_del(&entry->link);
		drm_gem_object_put(&entry->dma.base);
	}
	parhelia_submit_unlock(pdev);
}
