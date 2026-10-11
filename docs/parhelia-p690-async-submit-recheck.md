# P690 async submit recheck: first safe queue

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The saved Linux vendor driver proves that the P690 primary DMA producer can
append work without waiting: `mtx1937` advances its persistent ring write
pointer by one 20-byte CALL and writes the resulting bus address to `0x0408`.
It does not read `0x041c` on that path.  Its `mtx2508` restart path instead
waits for primary DMA idle before programming `0x0404` and `0x0400`.

The open driver can therefore start with a three-slot software queue in one
contiguous coherent primary allocation.  It must retain a separate primary
span, secondary blocks, host-vertex storage, GEM lookups and transient pins
for every live slot.  It must *not* infer render retirement from the primary
writeback marker: render retirement is the final DWGSYNC `0x12bc` tag observed
at `0x041c`.

No render-completion IRQ is required or justified.  Polling `0x041c` from a
sleeping wait ioctl and a bounded delayed worker is enough for a first queue.

## Direct binary evidence

`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`, `mtx1937` at
`.text+0x32360`, does this on its normal path:

```text
write += 0x14
write[-5..-1] = { 0x20292820, 0,
                  secondary_start | start_flags,
                  secondary_end | end_flags, 0 }
MMIO[0x0408] = ring_bus_minus_cpu + write
```

There is no read of `0x041c` between the append and the `0x408` doorbell.
The CALL packet is decoded in `parhelia-kernel-abi.md` section 6.2: it writes
`SECADDRESS` and `SECEND`, not a raw client buffer to `0x408`.

`mtx2508` at `.text+0x31880` first waits until `0x08f0 bit 17` is set and
until the indexed-register FIFO reaches its baseline.  Only then it derives a
new ring base, writes `0x404`, and writes the value at shared-status `+0x40`
to `0x400`.  Thus a queue must never switch primary allocations, reset
`0x404`, or wrap the open allocation while a previous primary span remains
live.

The vendor fence constructor `mtx3144` at `.text+0x31c30` writes two `0x12bc`
tags separated by eight and publishes the ring tail through `0x408`.  Its
normal producer does not make the tag blocking.  The hardware source of truth
is still `0x041c` after masking the low flag bits.

## Correction to the earlier async implementation proposal

`parhelia-p690-async-submit-implementation.md` says to advance the terminal
queue tag by 16 (section “Submit, retire, wait, and failure paths”, item 3).
That is wrong for the **current default** `replay_fences=1` stream.

The live open submit code establishes this exact sequence:

```text
base = MMIO[0x041c] & ~7
begin = base + 8
opening fence writes: begin | 1, begin + 8
drain fence writes:   begin + 8 | 1, begin + 16
terminal tag = begin + 16 = base + 24
```

This follows `parhelia_submit.c:1248-1258` and
`parhelia_packets.h:103-120`.  A second queued replay batch must use
`begin = previous_terminal + 8`, therefore its **terminal** tag is the
previous terminal plus 24.  A software allocator that increments terminal
values by 16 duplicates/interleaves a tag already written by the preceding
batch and can make a wait appear complete before its drain has run.

The first queue must store two values per slot:

```text
slot.begin_tag = next_begin;                 /* first 0x12bc sequence */
slot.fence = next_begin + 16;                /* final drain tag */
next_begin = slot.fence + 8;
```

When `replay_fences=0`, the current stream has two tags and a terminal spacing
of 16 instead.  Do not support that diagnostic configuration asynchronously;
reject async while it is disabled, along with `full_sync`, `trace_submit` and
`post_draw_wb`.  This makes the deployed async contract use one verified
cadence only.

The same existing proposal overstates the role of its per-slot writeback page.
The current three-dword primary tail
`{ 0x21212425, next_dma_tag, writeback_bus + 0x20 }` is a primary-DMA
writeback marker.  It is *not* a `0x12bc` emission and cannot retire render
resources.  The first async implementation does not need to make that marker
an API or a scheduling dependency: it may retain the existing single status
page as a diagnostic marker, but it must release a slot only after
`0x041c >= slot.fence` by signed masked comparison.  Per-slot writeback pages
are optional evidence collection, not a prerequisite and not a replacement
for DWGSYNC retirement.

Finally, `PARHELIA_SUBMIT_FENCE` has no distinct kernel behavior today:
`parhelia_submit_ioctl()` accepts either flags value, always waits, and always
returns `args->fence`.  The new ABI may require bit 0 together with ASYNC for
clarity, but the implementation must explicitly add that validation rather
than describing it as preserved behavior.

## Code-level kernel plan

1. Define `PARHELIA_MAX_RELOCS` as `PARHELIA_MAX_USER_PACKETS + 64` and add
   `PARHELIA_ASYNC_SLOTS = 3`.  Increase the one coherent primary allocation
   to `3 * (PARHELIA_CALL_CAPACITY * 5 + 16)` dwords.  This is 13,632 bytes;
   it is smaller than one existing secondary block.  Preserve one allocation,
   never three independently armed rings.
2. Replace `pdev->vertex_cpu`, `pdev->dma_blocks[]`, the live local
   `objects[]`/`pinned[]`, and live mode-7 index-copy ownership with a
   `parhelia_submit_slot`.  Each slot has 224 secondary block descriptors,
   a coherent host-vertex buffer, a retained lookup array, a transient pin
   array, `[primary_begin, primary_end)`, `begin_tag`, terminal `fence`, and
   the client `drm_file`.  Mode 7 remains synchronously rejected for async.
3. Add `primary_put`, `queued_slots`, a FIFO slot list, `next_begin_tag`,
   a wait queue and `delayed_work`.  Allocate `next_begin_tag` from
   `0x41c & ~7` only when the queue becomes empty; otherwise use the arithmetic
   above.  Reject a tag if signed modular ordering would reach half the u32
   sequence space.
4. Keep all existing packet validation and relocation checks.  After a slot is
   claimed, copy host vertices to that slot and build all secondary blocks
   there.  For a VRAM vertex relocation the persistent CREATE_VERTEX pin stays
   file-owned, but the GEM lookup reference remains slot-owned.  Deduplicate
   GEM objects before attaching reservations or releasing references.
5. Before publishing, create one private `dma_fence` for the slot and attach
   it to every unique relocation object through `drm_exec`.  Use WRITE for
   colour/depth targets and READ for texture-only operands once the register
   use table is carried through the batch builder.  A temporary all-WRITE
   policy is safe but causes needless serialisation of shared immutable Q3
   textures.  The one hardware engine already orders primary spans; reservation
   fences protect CPU maps, object destruction and scanout/export.
6. With `submit_lock` held, retire already completed FIFO entries.  If this is
   the first live entry, wait for `0x08f0 bit 17`, program `0x404` to
   `primary_bus + primary_put * 4`, and leave `0x400` at its established
   writeback-page value.  There is no evidence supporting a new `0x400`
   programming value on the open path.  Emit the CALLs, final nonblocking
   replay fences and the existing primary writeback marker into the slot’s
   non-overlapping primary span; `dma_wmb()`; write only the new tail to
   `0x408`; strobe `0x314`; then mark the slot queued.
7. `parhelia_retire_locked()` reads `done = MMIO[0x41c] & ~7` and retires FIFO
   slots while `(s32)(done - slot->fence) >= 0`.  On each retirement call
   `dma_rmb()`, signal its dma-fence, unpin only transient BOs, drop all slot
   lookup references and wake waiters.  Do not wait for the DMA marker here.
8. A delayed worker merely calls the same retirement function and enforces the
   existing 50 ms timeout policy for the oldest slot.  On timeout it must set
   `wedged`, clear PCI mastering and quarantine every live slot exactly as the
   synchronous path does.  `postclose` must wait for slots owned by the closing
   file before it releases CREATE_VERTEX objects; `submit_fini` cancels work
   before managed coherent allocations disappear.
9. Do not wrap `primary_put` until all slots are retired *and* primary DMA is
   idle.  At that point reset the software cursor to zero and arm the next
   base.  If capacity is exhausted before that, sleep for the oldest terminal
   fence outside `submit_lock`.

## Mesa plan required for a useful queue

Mesa cannot simply set an async bit.  `ph_submit_batch()` copies each request
into `ctx->vram_vertices` at offset zero, while the current CREATE_VERTEX
allocation is exactly one `ctx->vertices` array.  A second async draw would
overwrite bytes that the first P690 class-0 fetch can still read.

Allocate `3 * sizeof(ctx->vertices)` for the P690 vertex object and make each
Mesa submission record own one subrange.  Pass that record's byte offset to
`parhelia_batch_submit_vram()`.  The host-DMA fallback needs three copied
buffers too, which the kernel slot provides.  Do not reuse a Mesa subrange
until `WAIT_FENCE` completes its terminal tag.

The two P690 lightmap submissions at `parhelia_context.c:1569-1609` are
ordered GPU work but they are not independent: pass two blends against the
colour output of pass one.  They may be appended without a CPU wait because
the primary ring preserves ordering, but both must carry the same colour and
(depth when present) reservation dependencies.  The existing document's
phrase “three independent batches” is inaccurate for this dominant Q3 path.

Keep CPU alpha fallback, clears, tiled/linear resolves, resource maps,
resource destruction and exports synchronous until each path waits for its
resource's submitted fence.  `ph_flush()` must return a real refcounted
`pipe_fence_handle`; `ph_fence_finish()` issues `WAIT_FENCE`.  The current
implementation returns NULL and `ph_fence_finish()` always returns true, so it
cannot represent async completion.

## Staged validation order

1. Add a test-only submit flag that uses one slot, waits through the new wait
   ioctl, and byte-compares untextured, textured, depth and vertex-alpha
   renders with the synchronous reference.
2. Append three P690 class-0 draws using three vertex subranges.  Log primary
   ranges and begin/terminal tags; assert `0x404` is written once and no live
   ranges overlap.  Run at least 10,000 iterations and return all pin/ref
   counts to baseline.
3. Queue the ordered two-pass lightmap route, then verify its output against
   the synchronous Q3 render.  Exercise X present/export and CPU maps while a
   render target is live.
4. Enable Mesa async only after the previous gates pass, then measure Q3 FPS
   and system CPU time separately from uncapped glxgears.

## Sources

* `scratchpad/mtx_parhelia_ko.asm`: `mtx1937` at `0x32360`, `mtx2508` at
  `0x31880`, and `mtx3144` at `0x31c30`.
* `docs/parhelia-kernel-abi.md`, sections 6.1--7.1.
* `linux-7.2/drivers/gpu/drm/parhelia/parhelia_submit.c:1248-1489` and
  `parhelia_packets.h:103-120`.
* `mesa-26.2.2/src/gallium/drivers/parhelia/parhelia_context.c:195-254` and
  `:1536-1609`.
