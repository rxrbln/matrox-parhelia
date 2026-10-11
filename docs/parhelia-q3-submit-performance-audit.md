# Q3 submit-performance audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This audit examines the active P690 (`102b:2539`) Mesa and kernel submission
path, the retained working vendor ioquake3 capture, and one read-only runtime
sample from the test target.  It identifies the present cause of Q3's roughly
one-frame-per-second behaviour and separates safe work from attractive but
unsafe batching ideas.  It changes neither the target nor driver source.

## Result

The immediate bottleneck is **synchronous kernel retirement of every Mesa
batch**.  It is not vblank, the CPU alpha workarounds, a diagnostic submission
mode, or a detected GPU hang.

During a five-second observation of the live Q3 process, its `/proc/PID/stat`
accounting changed as follows:

| counter | first | five seconds later | elapsed CPU time |
|---|---:|---:|---:|
| `utime` | 459 | 471 | 0.12 s |
| `stime` | 14595 | 15089 | 4.94 s |

`getconf CLK_TCK` on the target is 100.  The process was runnable and used one
core, but 97.6% of that CPU time was kernel time.  At the same time the loaded
module parameters were `coalesce_primary=Y`, `trace_submit=N`, `full_sync=N`,
`replay_fences=Y`, and `post_draw_wb=N`; the kernel log had no Parhelia timeout,
error, or wedge.  Q3's environment contained only `DISPLAY` and the isolated
Mesa `LD_LIBRARY_PATH`, so neither `PARHELIA_CPU_ALPHA_CUTOUT` nor
`PARHELIA_CPU_ALPHA_BLEND` was active.

That is direct evidence for time spent in normal DRM submission/retirement,
rather than in the software alpha fallback or a refresh-rate cap.

A fresh isolated-Mesa check makes the separation decisive.  With Q3 stopped,
uncapped `glxgears` rendered 1,362 frames in 5.0 seconds (272.379 FPS) and
1,366 frames in 5.0 seconds (273.119 FPS).  While Q3 was active, the same
check fell to 0.735 and 0.581 FPS because Q3's single synchronous submit
thread saturated a core.  A six-second `strace -c` sample of Q3 recorded
1,502 `ioctl` calls consuming 5.606960 seconds (3,732 us/call, 99.99% of the
sample); no other syscall accounted for material time.  Thus the healthy
simple-workload rate and the Q3 collapse have one direct explanation: per-batch
kernel retirement serializes Q3, not display refresh or a general GPU stall.

## Why the normal path remains synchronous

The previous primary-ring optimization is active and valuable, but it only
coalesces CALLs *inside one ioctl*.  In the normal path the kernel publishes
the completed primary span once (`parhelia_submit.c:892-896`), then busy-polls
the coherent writeback (`897-902`) and the hardware fence (`909`).  The latter
is also an explicit `udelay(1)` loop (`330-340`).  The entire operation is
inside `submit_lock`, acquired at line 614 and released only after retirement.

Mesa always requests `PARHELIA_SUBMIT_FENCE`
(`parhelia_batch.c:319-334` and `386-400`).  Each `ph_submit_batch()` therefore
does not return until that batch has drained, even though the returned fence is
not represented as a Gallium fence object.  The zero-copy class-0 vertex ring
removes the host-vertex copy but deliberately has the same blocking ioctl
contract (`parhelia_context.c:226-250`).

Each native hardware draw is a separate address-mode `0x97` packet:
`p3d_emit_draws()` emits one packet per primitive
(`src/parhelia3d/p3d_state.c:676-697`).  Mesa may put at most 180 of those
packets in one batch, and the 4096-dword vertex limit can lower that count for
textured/depth layouts (`parhelia_context.c:1663-1668`).  State bindings drain
the pending batch before changing their state, while `ph_draw_vbo()` invokes
the Gallium software draw frontend for every Q3 `glDrawElements` call
(`1787-1827`).  Thus Q3 creates many independently retiring batches.

For the normal diffuse × lightmap path the P690 reports one physical texture
pipe.  Mesa consequently serializes the two texture stages into two complete
hardware submits (`ph_submit_vertices()`, approximately lines 1450-1535).
That doubles the number of synchronous retirements for the dominant world
pass.  It is a correctness restriction, not an optional slowdown.

The vendor capture supports the distinction between a queued doorbell and a
GL completion point.  It contains a sequence of `ioctl(..., nr=33)` doorbells
before its first recorded `glFinish` (for example
`captures/ioq3-p690-working-draw.txt:5-74`), and later separates draws from
explicit `glFinish` calls.  The trace does not expose ioctl timing, so it does
not by itself prove the vendor kernel was asynchronous; it does prove that the
open driver's forced completion after every translated batch is not a captured
GL-level requirement.

## Safe implementation order

### 1. Add a bounded asynchronous-submit window

This is the highest-value correct direction.  Add a new explicit asynchronous
submit flag and a `WAIT_FENCE` ioctl (or a DRM syncobj export) instead of
changing the meaning of the existing synchronous path.  Give each in-flight
submission its own primary span, secondary DMA blocks, writeback sequence,
and pinned GEM-object list.  Retire and unpin that slot only after its fence
has completed.

Start with a two-slot ring.  Mesa can queue batch N+1 while the P690 executes
N, and must wait before reusing a slot or before an operation that needs its
result: readback, CPU texture resolve, a render-target-to-texture dependency,
resource destruction, and presentation.  `glFinish`, `pipe_context::flush`
when a fence is requested, and KMS flip preparation are natural mandatory
wait points.  The normal kernel entry must retain the present synchronous
behaviour for old userspace and for all diagnostics.

This is safe only with per-slot storage.  Returning early while retaining the
current single `pdev->primary_cpu`, global `dma_blocks[]`, global writeback
word, and immediate unpin loop would let the next ioctl overwrite packets or
move a referenced BO while the engine still reads it.  Simply deleting the
two polling loops is therefore a corruption and wedge risk, not an
optimization.

Validation must first compare the existing synchronous and two-slot paths on
the single triangle, depth, texture, vertex-alpha, and full Q3 image gates;
then verify no pending slot remains across an X shutdown or module unload.
The measurement should include batches/frame, queue-full waits, final-fence
latency, and target CPU `utime`/`stime` deltas.  It is successful only if
Q3's kernel-time share and frame time fall with unchanged images.

### 2. Implement native indexed Q3 draws

This is the next material reduction in CPU work and packet count.  Q3 GL1
uses `glDrawElements(GL_TRIANGLES, GL_UNSIGNED_INT)` over
`glLockArraysEXT` arrays.  The hardware supports a 32-bit index-address draw
(`0x9f000001, start, end`) plus bound vertex streams, without per-draw
de-indexing or repacking; the exact format and Q3 mapping are documented in
`parhelia-indexed-draws.md:527-579`.

The present driver instead maps those arrays into the Gallium draw frontend,
which emits transformed triangles one at a time.  Start with the documented
single interleaved class-3 DMA32 stream and index buffer, retaining their
ownership from `glLockArraysEXT` through `glUnlockArraysEXT`.  Do not claim
the current persistent local-VRAM vertex ring solves this: it avoids the
per-batch vertex copy, but it still starts from expanded triangles and does
not provide an indexed buffer path.

The index mode needs an independent P690 rendering proof before enabling it
for Q3, especially for the two-coordinate lightmap form.  It should not be
combined with asynchronous submission in the first target test; independent
image gates make failures attributable.

### 3. Keep primary coalescing and the current packet boundary rule

`coalesce_primary=Y` is already the correct normal setting.  It changed the
kernel from a producer update per CALL to one update per whole submit and
raised the uncapped gears result from about 225 to 275 FPS.  There is no
additional safe "coalesce every Q3 draw" toggle.

The retained hardware test shows that overlapping triangles in one hardware
draw packet violate depth interlock; 400 triangles in one packet produced
18,516 wrong pixels, while known non-overlapping groups were correct
(`parhelia-gears-test.md:95-154`).  The 180-draw cap also avoids the measured
ring-call stall above 257 calls.  Keep one primitive per packet unless an
independent screen-space non-overlap proof exists.  Any later Q3-specialized
2D glyph batching must be separately gated from world geometry.

## Non-fixes

* Replacing the `udelay(1)` loops with sleeping waits reduces CPU burn but
  cannot increase a single-threaded Q3 submit rate while the ioctl remains
  synchronous.  It belongs only after an asynchronous completion design has
  a reliable wake/timeout mechanism.
* Disabling vblank cannot explain the measured kernel time.  The Q3 launch is
  windowed at 640×480 and the active stall happens before presentation.
* Re-enabling a simultaneous two-texture P690 pipe is unsafe: this silicon's
  tested route serializes stages because the alternate sampler-pipe setting
  wedges it.
* Widening CPU alpha fallback is unrelated to this run because neither alpha
  environment variable was set.
* AGP/GART is not a P690 PCIe optimization.  The target has raw class-3
  DMA plus the verified class-0 local-VRAM vertex path; it has no usable GPU
  GART aperture.

## Conclusion

The live target evidence makes the next performance project concrete: retain
the existing coalesced primary submission, then build a correctness-preserving
two-slot asynchronous kernel/Mesa submission contract.  Native indexed draws
are the following major reduction in Q3 work.  Neither requires guessing a
new register value or changing the staged-only module deployment policy.
