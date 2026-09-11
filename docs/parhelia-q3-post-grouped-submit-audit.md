# Q3 after grouped submit: remaining cost and next packet optimization

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope and evidence

This audit covers the 640x480 windowed Q3DM1 run after Mesa commit `e50a100`
(`parhelia: group state streams within hardware CALL limit`).  It is based on
the saved batch-only trace `/tmp/q3-batch-trace.log`, SHA-256
`95c727069a87d8f82b2b026bd90b456e4e06604d35fe83a61c5e9deaff5a27c2`, and
the current Mesa and kernel sources.  It does not change either driver or the
target.

The trace contains 2,217 aggregate-drain records including startup.  A stable
frame begins with the exact recurring record

```text
submits=8 commands=3779 packets=974 stages=7 relocs=865 vertices=23004
```

There are 257 occurrences.  The 256 complete intervals between them contain
216 seven-drain frames and 40 eight-drain frames.  The latter have one small
additional state group, rather than a different steady transport mode.  This
gives a reproducible frame delimiter without using timestamps or guessing
from the displayed FPS counter.

## Per-frame command load

| Quantity | Mean | Median | Minimum | Maximum |
|---|---:|---:|---:|---:|
| Mesa aggregate drains | 7.156 | 7 | 7 | 8 |
| logical state submits | 131.801 | 130 | 130 | 139 |
| command dwords | 34,886.012 | 34,643 | 34,302 | 36,636 |
| packet descriptors | 6,775.844 | 6,722.5 | 6,643 | 7,112 |
| WARP stage binds | 130.801 | 129 | 129 | 138 |
| relocations | 4,771.832 | 4,737.5 | 4,664 | 5,008 |
| vertex dwords | 111,045.094 | 110,286 | 108,438 | 116,118 |

All seven regular drains are near the 1,024-packet negotiated limit.  Their
vertex payloads range from about 7,700 to 23,000 dwords, below the 30,720-dword
CREATE_VERTEX slot.  The aggregate command count is also far below 16,384
dwords.  Packet descriptors, including the separately accounted hidden WARP
stage CALL, therefore determine the current drain cadence.

Every native triangle has one relocated `0x97` draw packet.  A 3D state batch
has one colour relocation, normally one depth relocation, and zero to two
texture relocations; the one non-stage logical batch per frame is a clear.
Consequently the relocation totals bound the draw count to 4,247--4,509
triangles per frame.  The center estimate for the predominantly textured
workload is about 4,378 draw packets.  The remaining roughly 2,398 packet
descriptors per frame are state packets, about 18.3 per 3D state batch even
after Mesa's conservative register-list compaction.

The Draw software frontend expands every indexed Q3 triangle to three
transformed vertices in `ph_stage_tri()` and `p3d_emit_draws()` emits one draw
packet per triangle.  The bounds above therefore mean 12,741--13,527 expanded
vertices and 424--454 KiB of vertex data per frame (mean payload 433.8 KiB).
At the observed approximately 55 FPS this is about 6.11 million dwords or
23.3 MiB/s of mapped vertex writes, 233k--248k draw packets/s, about 373k
validated packet descriptors/s, and about 262k relocations/s.  The byte
bandwidth is modest; the per-packet validation, relocation, secondary-CALL
construction, and software transform/de-index work are much more credible CPU
costs.

## Alpha and Present share

The batch-only trace does not label alpha states.  The immediately preceding
fixed-scene selector trace recorded 1,444 GPU RGB-alpha two-pass batches over
405 native drawable flushes: 3.565 source-over batches and 16.410 source
triangles per frame.  Each source-over batch becomes two complete hardware
states, so the workaround accounts for about 7.13 of the 131.8 logical state
submits per frame and adds 3.565 states over an ideal one-pass route.  It also
replays about 16.4 triangles, roughly 394 depth-textured vertex dwords, per
frame.  A correct native sampled-alpha route is desirable, but this normalized
share is only 2.7% of logical states and less than 0.4% of vertex data.  It is
unlikely by itself to provide the remaining ten-percent throughput increase.

The trace is the Q3 client context only.  It cannot contain the X server's
DRI2 copy, because the X server was not launched with `PARHELIA_TRACE_BATCH`.
The native flush path hands the queued back buffer to X without waiting, after
which X performs one synchronous GPU blit on the same DRM device.  At 55
swaps/s a 640x480x4 full-window copy touches 16.9 million pixels/s, or about
64.5 MiB/s each of source reads and destination writes.  Present therefore
adds one ordered submission and measurable VRAM traffic per frame, but it does
not explain the seven client drains or their packet-cap pattern.  Rendering
directly into a window's front pixels would need correct window movement,
clipping and overlap semantics and is not the narrow next optimization.

## Likely current limit

The trace proves a frontend/submit pressure point and does not prove GPU fill
saturation.  The workload reaches the descriptor cap in every regular drain,
while leaving substantial vertex and command capacity unused.  Mesa then
splits each drain again because its packed-CALL estimator understands only
adjacent relocated `0x97` draws and exact `0x8f` ranges.  Each logical state
still contributes about 18 non-draw CALLs plus its synthetic WARP-stage CALL.
With about 131 states per frame, this is roughly 2,600 CALL candidates before
the per-submit opening/EOB/drain records.  Applying the 224-CALL limit to each
of the seven independent aggregate records projects about 14--16 physical
async submissions per frame despite only seven Mesa drains.  The exact
physical count was not logged in this trace, so that range is a model using
the two-to-four fixed VRAM relocations per state, not a measurement.

At 55 FPS the already measured scene needs only a 9.1% throughput gain to
reach 60 FPS.  Reducing the projected physical submission count and primary
CALL traffic has ample headroom to provide it.  A GPU-clock or fill-rate limit
may appear afterward, but this trace does not support choosing clock changes
before eliminating the visible submit amplification.

## Next optimization: validated generic adjacent-packet CALL packing

The narrowly implementable next step is to enable the existing kernel
`pack_adjacent_packets` mechanism after a hardware gate and teach Mesa the
same execution-plan estimator.  It does not combine triangle packets or erase
their headers.  It places consecutive, individually validated packet byte
sequences inside one secondary CALL, capped at 4,096 dwords.  The hardware
still parses every original `0x97` header, preserving the packet boundary that
the overlapping-depth tests require.

The kernel implementation already exists at
`parhelia/parhelia_submit.c:620` and is selected at lines 3276--3286.  It runs
after packet validation and relocation patching, requires exact packet-index
and byte-offset contiguity, stops on synthetic opening/stage/EOB/drain plan
entries, and uses `parhelia_copy_packet_span()` so the copied span contains the
patched operands.  Its static tests cover gapped storage and the 4,096-dword
limit.  Keep the module option default false until the mixed state/draw screen
probe passes.

Implement the capability and estimator as follows:

1. Add `PARHELIA_PARAM_PACK_ADJACENT = 24` to both copies of
   `parhelia_drm.h`.  `parhelia_get_param_ioctl()` should report one only when
   `pack_adjacent_packets`, `coalesce_primary`, async submission, and the
   normal non-trace/non-full-sync/non-post-writeback path are all active.  It
   must report zero when `state_shadow_cache` is active until the interaction
   below is fixed.
2. Add a `pack_adjacent_packets` capability bit to `struct ph_screen`, query
   it in `ph_screen_create()`, and pass it to the packed-CALL estimator rather
   than inferring a kernel module setting from an environment variable.
3. Extend `parhelia_batch_packed_call_count()` in Mesa `parhelia_batch.c:58`
   with a generic mode.  Construct the same conceptual plan as the kernel:
   start or extend a packet-storage run only when offsets are exactly
   contiguous and the total is at most 4,096 dwords; flush the run before the
   kernel opening record; include a WARP-bind parent at the end of its current
   run, then count the synthetic stage CALL and reset the run; finally count
   EOB and drain.  Duplicate the kernel's exact standalone-watermark
   recognition to locate the opening slot.  A blanket `three synthetic calls`
   constant is insufficient for generic packing because the opening record
   can split an otherwise contiguous packet run.
4. Use that mode in `ph_submit_state_stream_groups()` and `ph_drain_batch()`
   at Mesa `parhelia_context.c:582--661`.  Retain the recursive `-E2BIG`
   bisection as a compatibility and estimator-mismatch guard.
5. Add matched userspace and kernel selftests with a state prefix, an opening
   watermark, a WARP bind plus synthetic stage, ordinary post-bind state, and
   multiple relocated `0x97` packets.  Assert both the exact CALL count and
   the original packet bytes after span copy.

With generic packing, the 4.4k draw packets and 2.4k state packets remain
validated and executed, but contiguous storage needs roughly two packet-span
CALLs around each WARP-stage insertion instead of one CALL per state packet.
Each existing near-1,024-packet aggregate should then fit under 224 CALLs and
become one physical ioctl.  That projects a reduction from roughly 14--16
physical client submits per frame to the seven descriptor-limited drains,
while reducing primary CALL parsing much more sharply.  Even a fraction of
that projected reduction is enough for a ten-percent frame-rate gain.

## Correctness constraints and gates

* Never merge packet encodings themselves.  In particular, retain one `0x97`
  header per triangle; combining triangle operands inside one draw packet has
  already failed overlapping depth tests.
* Stop a storage span at every synthetic plan entry and at 4,096 dwords.  The
  WARP-stage CALL must execute immediately after its parent bind.  The opening
  fence must remain immediately before the first watermark packet.
* Keep validation and every relocation/pin/fence attachment per original
  packet before packing.  Packing changes only the primary CALL plan.
* Do not combine generic packing with the current `state_shadow_cache` order.
  The shadow filter runs after packing and tests only `plan->blocks[i]`, the
  first packet of a multi-packet span.  If that first packet is redundant it
  can incorrectly omit non-redundant later packets; if a later packet is
  redundant it remains embedded.  Either report the capability false while
  shadowing is enabled, or filter the one-packet plan first and pack only the
  surviving contiguous entries afterward, with new static tests.
* Validate first with a staged `/tmp` module: one triangle, far/near overlap in
  both orders, depth-write-disabled replay, one/two-texture draws, source-over
  alpha, alpha cutout, clear-followed-by-draw, and Present.  Then compare Q3
  screenshots and run the required uncapped `vblank_mode=0` glxgears gate.
* Record kernel `async_submits`, slot waits, tail waits and queue high-water
  before and after.  The optimization is confirmed only when physical submits
  and/or waits fall, Q3 exceeds 60 FPS, and images remain correct.
