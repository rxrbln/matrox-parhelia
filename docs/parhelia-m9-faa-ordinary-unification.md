<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# M9 FAA and ordinary rendering: differences and unification design

Status 2026-10-08, agent m9unify. Baseline DRM `956e813`, Mesa `894fe3f`.
Labels: **PROVEN** (vendor RE or hardware evidence cited), **MEASURED**
(our hardware run), **DERIVED** (follows from proven facts plus code reading),
**INFERRED** (plausible, not yet tested), **OFFLINE** (host build/test only).

## 1. The stacks as they exist today

There are three FAA mechanisms, not two:

| # | Mechanism | Gate | Command producer | Result |
| --- | --- | --- | --- | --- |
| A | Ordinary stream with FAA edits (`m9_faa.c`: `ad`/`af`, `ae`/`ef`/`f0` AA mode, `FAA_POOL` store reloc) | `M9X_FAA` | Mesa `m9_batch`, kernel validator | MEASURED never produced FAA pixels; `af` bit 11 stops the engine, the store at `a0..a6` is not bound |
| B | Kernel template graph (`m9_faa_native.c`, `m9_faa_graph*.c`, ~45 encoders, firmware templates) | `M9X_FAA_APPLICATION`, `CONFIG_DRM_M9X_FAA_GRAPH_INIT_CONTROL` | kernel: vendor-template images patched from live owners | MEASURED FAA glxgears 48.3–48.7 fps at 1024x768 |
| C | Kernel private diagnostics (`m9_faa_diag`, per-phase firmware profiles) | module params + Kconfig choice | kernel templates | proof fixtures, not an application path |

The ordinary path is mechanism-free: Mesa encodes, the kernel validates.

### 1.1 DRM transport

| Aspect | Ordinary (`M9_SUBMIT`) | FAA graph (B) |
| --- | --- | --- |
| Primary namespace | Physical DMA32 staging slots, `COMMAND_BASE = bus|SYSTEM_TAG`, rebased per slot switch (`m9_submit_kernel_stream_common`) | One retained GPU-virtual flags30/B primary, `COMMAND_BASE = gpuva|1`, written once (`m9x_submit_faa_virtual_primary_locked`) |
| Growth | Fresh primary copy per job, memcpy into the slot | Same primary extended by moving `COMMAND_END` (`m9x_extend_faa_virtual_primary_locked`, `m9x_post_faa_graph_end_locked`) |
| Secondary link | `45452d2c start|1, end|2` into a class1 GPUVA secondary (`m9_private_stream_range`) | Same `45452d2c` link words written by each encoder; counted-pool close uses an untagged end (`m9x_faa_counted_secondary_encode`) |
| Completion | One bm-12 footer (`1b` fence, `e3 target|1`, `43 target`) per job, async queue of `M9_QUEUE_SLOTS` jobs | Synchronous wait per END post; counted secondary/pool points |
| Validation | `m9_parse_commands`, per-index whitelist, reloc kinds | Kernel-generated words only; userspace supplies typed parameters and GEM handles |
| Lifetime | `drm_exec` reservation, `dma_resv` fences, async retire list | Graph binding kref + per-call reservations; 17 copies of the posted-failure quarantine block |

### 1.2 Mesa

| Aspect | Ordinary | FAA graph (B) |
| --- | --- | --- |
| VS | `m9_vs_compile` native/CPU T&L | Same compiler, FAA variants `m9_vs_compile_faa_gl*`, `m9_vs_emit_faa*` |
| FS | `m9_fs_compile` general NIR backend | `m9_faa_program.c`/`m9_faa_live*.c` pattern-matched colour/texture-combine programs |
| Vertex input | Native VS input records or draw-module CPU T&L | `m9_vs_source_records*` snapshot copied into the ioctl |
| Textures | `m9_texture_bind_view` + `m9_batch_emit_texture_unit` | `m9_texture_bind_view`, then a typed kernel view (`profile 12`, filters, address modes) |
| State | `m9_batch_emit_regs` / packets | Typed request structs (`m9_faa_application_state`) lowered by kernel encoders |
| Submission | `m9_batch_submit` → `M9_SUBMIT` | FAA graph ioctls (`COLOR_FRAME_BATCH`, `DUAL_BATCH`, `PRESENT`, `UNLOCK`, …) |
| Presentation | Ordinary flip/blit | Lock (F021 row view) → CPU rows → Unlock → destination descriptor |

## 2. What the hardware and vendor require (PROVEN)

1. **One requester, one primary namespace.** The vendor driver posts both
   normal and FAA work through the same requester (`0x400` base, `0x404`
   end; `parhelia-m9-3d-submission.md`). The normal `101b8` and FAA `101f8`
   profiles have the identical 48-entry stage chronology, the same two
   launcher calls and byte-identical commands outside three producer-derived
   classes (`parhelia-m9-faa-profile-comparison.md`).
2. **The vendor FAA primary is GPU virtual.** MxMD `23a09` selects the
   retained flags30/B primary GPUVA|1 and `2461e` posts the untagged
   exclusive end (comment above `m9x_submit_faa_virtual_primary_locked`;
   `parhelia-m9-faa-post-init-native-seam.md`).
3. **FAA draws use the ordinary raster lowerer.** `fc0e0` emits header
   `45d7eff0` with F0/EF/D7 for every stage, FAA or not; only the values
   differ (`parhelia-m9-faa-raster-state.md`). The ordinary Mesa driver
   already emits this packet.
4. **FAA-specific state is a bounded set of additions:** the sampled primary
   descriptor (`B0/B1`, selector9 layout, sample count 2), companion and HZ
   bindings `B8..BB`, the `ad`/`af` multisample pair, the AA mode fields in
   `ae`/`ef`/`f0`, the pool6 and sentinel fragment pointer pairs with the
   `RESET_FRAG_PTR` save/restore (`A1/A3/A4`), and the resolve output request
   at Lock/Unlock (`parhelia-m9-faa-private-graph.md`,
   `parhelia-m9-faa-renderer-lifecycle.md`).
5. **Lifetime rules are identical.** The FAA owner must retain every
   referenced resource through the completion after resolve; posted
   failures quarantine until DMA stop; BOs are released off the transport
   lock. These are the rules the ordinary submit already implements
   (`parhelia-m9-faa-private-graph.md`, "Full-frame owner").
6. **Pool6 contents are GPU-produced** and cannot be CPU-initialised or
   zero-filled (`parhelia-m9-faa-private-graph.md`). Any unified encoder
   must keep the vendor producer order for the pool range `284..2c4`.

## 3. What is only a duplicated implementation (DERIVED)

1. **Primary posting.** Four FAA posting functions repeat the same PTE
   walk, `wmb()`, `COMMAND_END` write/readback and fence wait; 14 further
   PTE walks live in `m9_faa_native.c`. The ordinary path has its own
   END/wait. Only the namespace selection (item 2.2) differs.
2. **Fence finish/quarantine.** 17 copies of
   "set error, wedge + quarantine if posted, else signal, put".
3. **Byte encoding helpers.** 18 private `get`/`word`/`read32`, 28 `put`
   variants and 16 `overlap` variants across the FAA encoders, while the
   kernel already provides `get_unaligned_le32`/`put_unaligned_le32`.
4. **Packet vocabulary.** The secondary link (`45452d2c`), the fence
   footer (`4545421a`, `4545e3e6`) and other headers are open-coded in
   about 25 places instead of named encoders shared with `m9_triangle.c`
   and `m9_private_stream.h`.
5. **Two FAA paths in Mesa.** Mechanism A (ordinary stream edits) and
   mechanism B (kernel graph) duplicate the AA-mode and store concepts;
   only B renders. A is the right *shape* (FAA as an ordinary-stream mode)
   with incomplete content; B has the proven content in the wrong place.
6. **Fragment programs.** `m9_faa_program.c` builds colour and
   texture-combine programs separately from `m9_fs_compile`, although both
   target the same native fragment ISA (INFERRED that the FAA interface can
   be expressed through `m9_fs_compile` output plus its slot layout).
7. **Constant/vertex snapshots** in `m9_context_faa_build_draw` repeat the
   CPU-only buffer validation twice (VS and FS constant buffers).

## 4. Unification design

Target: one command vocabulary and one transport; FAA is a property of the
render target (sampled primary + graph) and of the job (bindings,
resolve), not a separate stack.

### Stage 1 — shared kernel primitives (behaviour neutral, OFFLINE verifiable)

* `m9_cmd.h`: little-endian get/put on command bytes, range overlap, named
  packet headers and the secondary-link encoder. `m9_private_stream_range`,
  `m9_triangle.c` and the FAA encoders use it. Verification: the module's
  `.text` is compared before/after; the encoders' host checks still pass.
* Transport helpers in `m9_requester.c`:
  `m9x_primary_mapped_locked()` (the PTE walk) and
  `m9x_transport_post_end_locked()` (wmb, END, readback, wait, extend),
  used by every FAA posting function; `m9x_transport_fence_finish_locked()`
  replaces the quarantine block copies.

### Stage 2 — one transport namespace (needs hardware)

* Let the ordinary submit continue the retained GPUVA primary when an FAA
  graph owns the requester, instead of refusing with `-EACCES`. The job
  shape is unchanged (2c/2d link into the class1 secondary plus the bm-12
  footer); only the primary location and END posting move to the shared
  helper. This removes the unresolved FAA→ordinary handoff. PROVEN that the
  vendor does exactly this (2.1, 2.2); UNTESTED on our hardware.
* Async queueing for FAA END posts then reuses the ordinary queue
  bookkeeping (target/point/retire lists); the synchronous waits that exist
  only for diagnostics stay behind the diagnostic profile.

### Stage 3 — FAA as an ordinary-stream mode (needs hardware, per feature)

* Replace mechanism A's guessed edits with the proven additions from 2.4,
  emitted by Mesa's ordinary encoder into the same job, with the kernel
  validator admitting the graph-owned addresses through typed relocations
  that name the authenticated graph (never raw addresses; capture policy).
* The kernel template encoders become the reference oracle: for each
  template, a host test compares the ordinary-encoder output for the same
  typed input against the template image (excluding live addresses).
  A template is retired only when its oracle passes and hardware pixels
  match (FAA gears, the 320x240 dual-texture oracle).
* Fragment programs from `m9_fs_compile` replace `m9_faa_program.c` once
  the FAA slot/interface layout is expressed as a compile option.

Pool6/sentinel ownership, the resolve request and the Lock/Unlock F021/F022
views stay kernel-owned: they hold GPU-produced state (2.6) and private
owners that must never be exposed to userspace.

## 5. Non-goals and constraints

* No change of fence deadlines, validator admission or module parameters.
* Every refactor commit is behaviour neutral and separately verifiable;
  any step that changes emitted bytes needs FAA gears (1024x768, 48 fps)
  and ordinary scanout pixel identity before integration.
* Diagnostic profiles (mechanism C) stay compiled and selectable; they are
  validated features, not dead code.

## 6. Implementation log

Stage 1 is done (OFFLINE, agent m9unify3, 2026-10-08). The 25 commits are
on DRM branch `m9unify3-stage1` (`f5d9c11..e41aca9` on `956e813`), in clone
`/home/ai/tmp/agents/m9unify-linux/linux-7.2/drivers/gpu/drm`; they are not
yet integrated.

* `m9_cmd.h` is adopted by every built FAA encoder and by the
  ordinary/counted link builders.
* The requester gains `m9x_system_pages_mapped()`,
  `m9_transport_publish_end()`, `m9_transport_finish_wait_locked()` and
  `m9x_transport_fence_finish_locked()`.
* Proof: 120 host tests emit byte-identical streams against `956e813`
  (12.2 MB serialized). These include a new MMIO-stream serializer for
  the posting functions. Every style commit keeps code and rodata
  identical, and checkpatch `--strict` is clean on the series.
* Mesa is unchanged.

Results, gaps and the stage-2 plan are in
[tests/m9-unify-stage1-20261008](../tests/m9-unify-stage1-20261008/README.md).
Stage 2 (one transport namespace) has its host part done but is not yet
measured. Agent m9unify4 did it on 2026-10-08 (OFFLINE), with these results:

* Stage 1 is rebased and integrated: main DRM `4c9070f`, plus the m9faa3/m9faa4
  conversions and the repair of 31 host tests.
* The ordinary primary placement is split out of
  `m9_submit_kernel_stream_common()`. This is behaviour-neutral: the
  `transport-post-cpu.py` stream is byte-identical.
* With `faa_ordinary_continue=1` (default 0), ordinary jobs continue the
  retained FAA primary at the graph's last END. They share the context that
  the retired FAA source INIT activated.
* An FAA frame after an ordinary job is still refused by FAA timeline
  admission. Allowing it is stage 2b.
* The card test (FAA gears, then an ordinary textured triangle, scanout check)
  is prepared but has not run.

Results, the overlap-helper analysis and the card plan are in
[tests/m9-unify-stage2-20261008](../tests/m9-unify-stage2-20261008/README.md).

Stage 2b and stage 3 (agent m9unify6, 2026-10-08/09):

* Stage 2b, MEASURED on nano:
  * FAA gears runs on a requester that ordinary jobs have used.
  * Ordinary jobs wait in the kernel for the FAA idle boundary. This is a
    waitqueue that progress and fence retirement wake, bounded at ten fence
    timeouts without graph progress.
  * A released graph detaches at an idle boundary and rearms the staging
    slots, with BASE and END left as an empty pair.
  * In Q3 td-faa, 27 INIT/detach cycles ran.
  * A Lock held past the wait bound is force-unlocked, and the graph is
    revoked (-ECANCELED). This PASSed with DRM `7fb97ce`.
  * An ordinary submit from the blocking graph's own file fails at once
    (`df309db`, OFFLINE).
* Stage 3, shared encoder header, done and integrated:
  * `m9x/m9_hw_encode.h` (GPL-2.0-only OR MIT) carries the surface
    descriptor d0/d1, the compare codes, depth a8/ef and colour mask ab, the
    packet header and E6/TRANSITION framing words, and the texture/sampler
    words 70/79/7a/7c/7d.
  * The kernel FAA producers and Mesa m9x share it. Mesa carries a
    byte-identical copy, enforced by `tests/hw-encode-cpu.py`.
  * Every conversion was proven by stream identity in the host tests.
  * The inventory and order are in
    `campaign/agents/m9unify6/stage3-builders.md`.
* Open, the interlude stall: an FAA part after ordinary jobs can stall
  (Q3, three reproductions).
  * Page-aligning the FAA part does not help (MEASURED, FAILED).
  * The leading cause (DERIVED from decoding) is engine state that ordinary
    jobs change and later FAA frames never rewrite: 6a/6b/6c (the linked
    program, with a reloc into a Mesa buffer), strides 11..13, slots 05/07
    and b2/b3.
* FAA context shadow (`0982baa`, merged as `cf418aa`, OFFLINE), behind
  `faa_context_restore` (default 0):
  * It records the FAA context's register values from every FAA END post
    and INIT, including linked secondaries.
  * It marks what each ordinary stream on the retained primary writes.
  * In the interlude settle it writes the changed indices back to their FAA
    values.
  * The card A/B against `faa_interlude_restore=1` (the fixed-zero
    diagnostic) is scheduled in m9faa6's Q3 group.
  * Design: `campaign/agents/m9unify6/faa-context-shadow.md`.
* Remaining steps:
  1. The card A/B of the context shadow, Q3 td-faa first.
  2. The gears interlude harness. It needs a DSO that submits its shadow
     ordinary work before preparing the next FAA frame; m9faa6's DSO
     39adab6a prepares first, so its own job is refused.
  3. Template to synthesis, one producer at a time, with byte identity
     against the template as the oracle. It has not started. The next step
     is the PRESENT encoder, which is outside m9faa6's later/dual/depth-frame
     files. Proposed file list:
     * `m9x/m9_faa_graph_present_encode.c` is the only code change. Its
       25-word literal, patched at fixed offsets, becomes packets built with
       `M9_HW_HDR` and `m9_hw_surface_desc0/1` from the live views and the
       Unlock point U. The decoded packets are:
       * `e6,e6,e3` = 1, 2, (U+0x10)|1;
       * `e6,8f,92,90` = 0x04000001, 0xc0000600, ~0, 0;
       * `91,b0,b1,f0` = 0, dst|1, dst desc0 as format 0x78, 0x080c040c;
       * `78,79,f3,f2` = src|1, src desc0, 0, 0;
       * `ef,fc` = 0xaa, the negated extents;
       * `e6,e3` = 1, (U+0x20)|1.

       Values with no proven meaning stay numeric, with their capture cited.
     * `m9x/tests/faa-present-encode-cpu.py` gains a byte-identity check of
       the synthesized image against the template encoder (taken from git)
       over every admitted runtime shape, several page-aligned bases and
       Unlock points, refusals included. `faa-present-runtime-cpu.py` must
       stay green unchanged.
     * `m9_hw_encode.h` is not touched, so the Mesa copy is unaffected.
  4. Kernel style for the FAA skip-list functions, once m9faa6 is no longer
     editing them.
  5. The queue initialization shared between FAA INIT and sequence0 (needs a
     card check).
