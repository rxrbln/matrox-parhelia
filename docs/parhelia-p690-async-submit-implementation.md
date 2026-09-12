# P690 async submit: implementable kernel and Mesa contract

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Decision

The existing render ioctl cannot be made asynchronous by removing either of
its waits.  `parhelia_submit_ioctl()` currently retains one device-global
primary allocation, one `vertex_cpu` upload, `dma_blocks[]`, and the objects
and pins in local arrays until `writeback_cpu[8]` and `0x041c` retire.  Those
objects are immediately reusable after the ioctl returns.

The recovered vendor contract does support an asynchronous producer.  The
safe first implementation is a three-entry software queue sharing **one**
persistent primary allocation.  It has no render-completion IRQ dependency:
retirement is driven by a delayed worker and a sleeping wait ioctl that read
the proven DWGSYNC completion register `0x041c`.

This document deliberately does not turn on an unidentified Windows IRQ
source, does not alter KMS page-flip timing, and does not permit a primary
ring wrap while work is active.

## UAPI additions

Make the following identical additions to:

* `linux-7.2/include/uapi/drm/parhelia_drm.h`
* `mesa-26.2.2/include/drm-uapi/parhelia_drm.h`

```c
#define DRM_PARHELIA_WAIT_FENCE          0x04

#define DRM_IOCTL_PARHELIA_WAIT_FENCE \
        DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_WAIT_FENCE, \
                 struct drm_parhelia_wait_fence)

#define PARHELIA_PARAM_ASYNC_SUBMIT      17

/* Existing PARHELIA_SUBMIT_FENCE remains bit 0. */
#define PARHELIA_SUBMIT_ASYNC             (1u << 1)

/* timeout_ns: 0 = poll, -1 = interruptible unbounded wait, otherwise
 * relative timeout.  completed is always 0x041c & ~7 at return. */
struct drm_parhelia_wait_fence {
        __u32 fence;             /* in: tag returned by SUBMIT */
        __u32 flags;             /* in: must be zero initially */
        __s64 timeout_ns;        /* in */
        __u32 completed;         /* out */
        __u32 pad;
};
```

`PARHELIA_SUBMIT_ASYNC` is valid only together with
`PARHELIA_SUBMIT_FENCE`; an async submit returns its terminal DWGSYNC tag in
the existing `submit.fence` field.  The existing flag set remains synchronous
and must retain its current observable ordering.  The new get-param returns
nonzero only after all slot allocation, retirement, and teardown code is in
place.  Mesa can then fall back cleanly after `-EOPNOTSUPP` against a staged
older module.

Add `DRM_IOCTL_DEF_DRV(PARHELIA_WAIT_FENCE, parhelia_wait_fence_ioctl,
DRM_RENDER_ALLOW)` to `parhelia_drv.c`, and declare it in `parhelia_drv.h`.

Do not add a user-provided primary-ring address, producer offset, status-page
mapping, or raw render-fence MMIO operation.  The tag is the only public
completion identifier.

## Per-device and per-slot memory

Replace the submission-only backing in `struct parhelia_device` in
`parhelia_drv.h` with the following ownership model.  The exact names are
suggestions; the ownership and lifetime are not optional.

```c
#define PARHELIA_ASYNC_SLOTS              3u
#define PARHELIA_SLOT_PRIMARY_DWORDS      \
        (PARHELIA_MAX_PACKETS * PARHELIA_CALL_DWORDS + 16u)
#define PARHELIA_PRIMARY_DWORDS           \
        (PARHELIA_ASYNC_SLOTS * PARHELIA_SLOT_PRIMARY_DWORDS)

enum parhelia_slot_state {
        PARHELIA_SLOT_FREE,
        PARHELIA_SLOT_QUEUED,
        PARHELIA_SLOT_RETIRED,
};

struct parhelia_submit_slot {
        enum parhelia_slot_state state;
        struct drm_file *file;
        u32 fence;               /* terminal 0x12bc tag, low flags clear */
        u32 dma_tag;             /* terminal 0x21212425 diagnostic tag */
        u32 primary_begin;       /* dword offset in device primary_cpu */
        u32 primary_end;         /* exclusive dword offset */
        u32 call_count;

        struct parhelia_dma_block blocks[PARHELIA_CALL_CAPACITY];
        void *vertex_cpu;
        dma_addr_t vertex_bus;
        void *writeback_cpu;
        dma_addr_t writeback_bus;

        struct drm_gem_object **objects;
        struct drm_gem_vram_object **pinned;
        u32 reloc_count;
        struct drm_gem_vram_object *mode7_index_copy;
        struct dma_fence *out_fence;
};
```

`blocks[]` hold every copied, engine-readable secondary command span,
including opening/EOB/drain/stage blocks.  `vertex_cpu` holds only class-3
fallback vertices for this one slot.  The per-slot status page makes the
terminal `0x21212425` marker independently observable and avoids sharing a
DMA write target with a still executing request.  Each slot owns every GEM
lookup reference and transient `parhelia_vram_pin()` returned for that
request.  It also owns the private immutable mode-7 index BO when that path
is enabled.  A persistent `CREATE_VERTEX` object retains its existing
file-lifetime pin, but its lookup reference still belongs to the slot.

The device also needs:

```c
        wait_queue_head_t submit_waitq;
        struct delayed_work retire_work;
        struct parhelia_submit_slot slots[PARHELIA_ASYNC_SLOTS];
        u32 primary_put;         /* next unused dword, never wraps live */
        u32 primary_get;         /* first unretired dword, for assertions */
        u32 queued_slots;
        bool primary_armed;
        u64 fence_context;
        spinlock_t fence_lock;
```

Allocate the primary once at probe, with the expanded size above.  Allocate
each slot's vertex and status pages at probe with `dmam_alloc_coherent()`.
The 224 secondary blocks may stay lazy, but they must be allocated for the
chosen slot before its references are attached and before the doorbell.  The
slot's `objects` and `pinned` arrays are `kcalloc(PARHELIA_MAX_RELOCS, ...)`
at probe, so teardown never has to allocate memory.  Add
`PARHELIA_MAX_RELOCS` as the current UAPI upper bound (`MAX_USER_PACKETS +
64`) instead of duplicating an expression in cleanup paths.

For resource and KMS ordering, `out_fence` must be a real private
`struct dma_fence`, not merely the u32 hardware tag.  Embed a driver fence in
the slot or allocate it from a small slot-local fence object, initialize its
timeline with `dma_fence_context_alloc(1)` in probe, and signal it when its
DWGSYNC tag retires.  The u32 tag stays the UAPI wait key; the dma-fence is
the reservation/KMS dependency object.

## Primary-ring rules

The vendor sequence proves that `0x0408` is a producer doorbell.  For every
queued slot, append all its five-dword CALL records, then its terminal
writeback record, execute `dma_wmb()`, write `0x0408 = primary_bus + end * 4
| 3`, and strobe `0x0314` exactly as the current coalesced path does.

`0x0404` is a restart register, not a per-submit doorbell.  Only arm it when
`queued_slots == 0`, after `parhelia_wait_dma_idle()` succeeds.  Then set it
to `primary_bus + primary_put * 4` and set `primary_armed = true`.  Do not
write it for a second or third queued request.

The saved binaries describe a secondary limit at `0x0400`, while the
currently hardware-validated open code programs `0x0400` as
`PARHELIA_MMIO_WRITEBACK` during `parhelia_engine_init()`.  This register's
dual interpretation is not resolved well enough to change it for async work.
Keep the established status-page programming and enforce the primary capacity
in software; do not add a speculative `0x0400 = ring limit` write.

The first implementation does not wrap the primary allocation.  If
`primary_put + needed_words > PARHELIA_PRIMARY_DWORDS`, retire what can be
retired.  If any slot remains queued, wait for the oldest terminal tag.  Once
all slots are retired and `0x08f0 bit 17` says primary DMA idle, reset
`primary_put = primary_get = 0`, clear `primary_armed`, and arm the next
append at the allocation base.  This is intentionally conservative but
matches the recovered restart condition.  It is safe to reuse a completed
slot's secondary/vertex/status storage only after that slot's fence retires;
its primary bytes are still not reusable until the full allocation is idle.

`full_sync`, `trace_submit`, and `post_draw_wb` remain synchronous diagnostic
modes in the first implementation.  Reject `PARHELIA_SUBMIT_ASYNC` with
`-EINVAL` when any is enabled.  The known-good normal coalesced stream is the
only async candidate.

## Submit, retire, wait, and failure paths

Refactor `parhelia_submit_ioctl()` into a common build-and-publish helper plus
two completion policies.  Preserve all existing validation and relocation
checks before a slot is claimed.

1. Under `submit_lock`, run `parhelia_retire_locked()` first.  Find a FREE
   slot; if none exists, drop the mutex and use a sleeping wait for the oldest
   tag, then retry.  Never spin or sleep with `submit_lock` held.
2. Copy user vertices into that slot's `vertex_cpu`; copy every checked packet
   into that slot's `blocks[]`; emit fences into that slot's opening/EOB/drain
   blocks.  Relocations populate the slot's reference and pin arrays rather
   than function-local arrays.
3. Allocate the terminal 0x12bc tag from a device-global software sequence,
   not by rereading `0x041c` for each queue entry.  Start from the retired
   hardware tag once when the queue becomes empty, step by 16 while retaining
   the currently proven replay cadence, skip zero, and reject an interval
   reaching `INT_MAX` ahead of the last completed tag.  This preserves the
   current signed modular completion comparison.
4. Create the slot `dma_fence`, reserve each unique object's reservation, and
   attach it before the doorbell.  Conservatively attach
   `DMA_RESV_USAGE_WRITE` for every render relocation in phase one.  A later
   refinement may mark texture/source-only objects READ after a complete
   register-index usage table is covered by tests.  Use `drm_exec` to lock
   all unique `obj->resv` entries in a deadlock-safe order, call
   `dma_resv_reserve_fences()`, add the fence, and release the reservations.
   Do not call this while holding a TTM reservation.
5. Append the primary span under `submit_lock` using the rules above, publish
   it, set slot state QUEUED, increment `queued_slots`, and schedule
   `retire_work` with a short bounded delay.  If ASYNC is set, copy the tag to
   userspace and return now.  If it is clear, use the same slot wait helper,
   then return only after that exact slot retires.
6. `parhelia_retire_locked()` reads `0x041c & ~7`, walks queued slots in
   submission order, and treats `((s32)(done - slot->fence) >= 0)` as
   complete.  For each: `dma_rmb()`, signal its dma-fence, unpin every
   transient VRAM BO, drop every GEM ref and private index-copy pin/ref, mark
   the slot FREE, decrement `queued_slots`, and `wake_up_all()`.
7. The delayed worker takes `submit_lock`, retires completed work, and
   reschedules only while slots remain queued.  It must use a bounded elapsed
   deadline for the oldest tag.  On timeout, set `wedged`, disable PCI
   mastering, signal outstanding dma-fences with error, retain/quarantine all
   objects exactly as today's timeout path does, and wake waiters.  There is
   no completion IRQ in this design.
8. `parhelia_wait_fence_ioctl()` accepts only an issued tag from this device's
   timeline or an already-completed tag.  It calls the same retirement helper,
   sleeps on `submit_waitq`, handles signals and `timeout_ns`, and returns the
   current masked `0x041c` value.  It must never use `udelay()`.

`parhelia_submit_postclose()` must first wait for or retire every queued slot
owned by that `drm_file`; it cannot drop a CREATE_VERTEX pin merely because a
userspace process exited.  `parhelia_submit_fini()` first cancels
`retire_work`, sets `wedged`, clears PCI mastering, signals all queue fences
with error, then releases every slot's retained resource exactly once before
managed coherent memory disappears.

## Mesa dependency points

Mesa must not simply set the new submit bit in both existing batch helpers.
Those helpers reuse `ctx->vertices`, `ctx->packed_vertices`, the optional
mapped `ctx->vram_vertices` at offset zero, and render resources immediately
after `ph_submit_batch()` returns.

Use a small `struct ph_submission` ring in `parhelia_context.c`, one entry per
kernel queue slot.  It contains a `parhelia_batch`, owned transformed vertex
storage (or a distinct persistent-VRAM subrange), the returned u32 tag, and a
set of written/read resources.  Add batch entry points in `parhelia_batch.h`
and `parhelia_batch.c` that take submit flags; keep the existing helpers as
synchronous wrappers for compatibility.

The mandatory waits are:

* before reusing a Mesa submission entry or its class-0 vertex subrange;
* in `ph_flush()` when Gallium asks for an output fence, and in `glFinish`;
* before `ph_resource_resolve_linear()`, `ph_resource_upload_tiled()`, CPU
  map/readback, texture update, copy/blit source use, and resource destroy;
* before closing `vram_vertex_handle` in `ph_destroy_vertex_ring()` and before
  `ph_destroy_context()` destroys the draw module;
* before a resource exported through `ph_resource_get_handle()` is exposed to
  KMS/Present.  KMS must consume the attached reservation fence before a
  scanout address is selected; the existing vblank path remains unchanged.

`struct pipe_fence_handle` must stop being a no-op.  Add a refcounted
`struct ph_fence { struct pipe_reference reference; uint32_t tag; }`, return
it from `ph_flush()`, and implement `ph_fence_finish()` with
`DRM_IOCTL_PARHELIA_WAIT_FENCE`.  `ph_fence_reference()` must take/release the
pipe reference.  This covers Gallium callers that explicitly require a flush
boundary even when normal Q3 drawing runs ahead.

Start Mesa async only after the new get-param succeeds and only for the
ordinary P690 coalesced class-0/class-3 draw routes.  Keep CPU alpha fallback,
mode-7 bring-up, clears/blits used for resolve, and any error recovery
synchronous until their resource dependency tracking is exercised.  The Q3
world path can then queue up to three independent batches; queue-full waits
are sleeping waits rather than the present per-batch busy loop.

## Validation sequence

1. Unit-test UAPI flag rejection, exact wait timeout semantics, signed tag
   comparison around low tag bits, slot resource release, and the assertion
   that `0x0404` is written once for a multi-slot run.
2. On the staged module only, compare one untextured triangle, texture/depth,
   vertex-alpha, clear, and blit synchronously and with one async slot plus
   `WAIT_FENCE` byte-for-byte.
3. Alternate three slots for at least 10,000 submissions.  Log each primary
   `[begin,end)`, assert no live overlap or live wrap, check all per-slot GEM
   refs/pins return to baseline, and verify no timeout or wedge.
4. Exercise CPU maps, resource destruction, X Present copy and vblank events
   while a tagged render target is pending.  No unfinished BO may become the
   scanout START address.
5. Only then measure Q3 OpenGL 1 and uncapped glxgears.  Record image hashes,
   Q3 FPS, and process user/system time separately.  Success requires the
   synchronous submit loop to disappear without reintroducing the alpha or
   Present corruption seen in earlier experiments.
