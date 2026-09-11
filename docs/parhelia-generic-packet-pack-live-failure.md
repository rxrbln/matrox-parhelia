# P690 generic adjacent-packet packing: live failure diagnosis

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The first live test of kernel commit `81b91be` and Mesa commit `477ccb4`
rejects the generic adjacent-packet CALL packer.  With
`pack_adjacent_packets=1`, uncapped glxgears remained stable at
574.4--574.8 FPS, but Q3 lost part of its world rendering after initially
producing a complete frame.  The second reported frame left part of the
window at its magenta clear colour.  The kernel logged no new submit fault.
The target was returned to the known-good module and userspace immediately.

This is a semantic failure, rather than an estimator, allocation, or Present
failure.  The generic packer erases the secondary-CALL boundary between the
relocated `0x97` draw tail of one logical Mesa state batch and the non-draw
state prefix of the following batch.  The established draw-only packer never
crosses that transition, and the vendor path keeps draw and subsequent state
builder sections in distinct CALLs.  On the P690, a CALL boundary at this
transition is part of the command-stream execution contract.

Keep parameter 24 disabled and do not advertise its capability until a
boundary-preserving replacement passes the validation ladder below.

## Live evidence

The two full-screen captures saved from the failed run are:

| Capture | SHA-256 |
|---|---|
| `/tmp/q3-pack1.png` | `9f2ad5633b79065d553c768ce05269b4311c9bd5c4e9f48b614358cc3deb97dd` |
| `/tmp/q3-pack2.png` | `6b03e8047ce59de09aad692d415c3d5bd5dce2c09cc666726e8577e59bbf19bf` |

Both are 1920x1200 RGB images.  Cropping the known Q3 drawable at
`640x480+640+360` shows valid game pixels across the full width in both saved
images: neither right half contains any exact `#ff00ff` pixel, and each right
half contains more than 35,000 distinct RGB values.  The later crop instead
loses substantial world geometry, including scene surfaces and the central
object, while the weapon, HUD, and other surfaces still render.  The display
observation of magenta therefore came from missing 3D work, not a framebuffer
pitch split, a half-width Present copy, or a hard scissor boundary.

That distinction agrees with the absence of a DRM error.  Every submitted
packet remained syntactically validated, address relocations remained
patched, and the primary completed.  Some state/draw work was interpreted
incorrectly after legal-looking CALLs had been combined.

Glxgears is a useful throughput regression, but it does not exercise this
boundary heavily.  Its draw stream repeats a small, homogeneous set of
states.  The saved Q3 grouped-submit trace has about 130 logical 3D states and
roughly 4,400 relocated draw packets per frame across seven normal aggregate
drains.  Q3 therefore exercises draw-to-next-state transitions continuously.

## The boundary that was lost

Mesa's state-stream aggregation appends each complete state-and-draw batch to
`pending_batch` with `parhelia_batch_append_limited()`.  After every append it
records the resulting command, packet, relocation, and vertex counts in
`pending_marks[]`.  Those marks retain the original logical submission
boundaries for grouping, slicing, and `-E2BIG` bisection in
`ph_submit_state_stream_groups()`.

The current submit UAPI does not transmit those marks.  It sends only packet
offsets and lengths plus address relocations.  `PARHELIA_RELOC_BATCH_BOUNDARY`
is a different operation: it inserts a full internal EOB/DWGSYNC boundary,
supports the existing stateful path, and is neither emitted by state-stream
aggregation nor suitable for every state transition.

The kernel's `parhelia_pack_adjacent_packet_calls()` sees only the flattened
packet array.  It joins consecutive plan entries whenever packet indices and
command offsets are contiguous and the combined storage is at most 4,096
dwords.  It stops at kernel-created opening, WARP-stage, EOB, and terminal
drain plan entries, but it cannot stop at a Mesa `pending_marks[]` boundary it
never received.

A normal aggregated sequence has this shape:

```text
state A prefix ... WARP bind | synthetic stage CALL |
state A post-bind registers ... relocated 0x97 draws
state B prefix ... WARP bind | synthetic stage CALL |
state B post-bind registers ... relocated 0x97 draws
```

The WARP-stage entry splits each state around its bind.  Generic packing then
forms the unproven span:

```text
[state A post-bind registers ... A's 0x97 draws ... state B prefix ... B bind]
```

The novel part is specifically the transition from A's final relocated
`0x97` packet back to B's non-draw register/opcode packets.  Q3 supplies this
transition many times per drain.  The kernel preserves every packet header
and byte, but it removes the secondary-CALL end/start that previously reset
the parser/execution section before B's state.

The successful packers do not make this assumption:

* `parhelia_coalesce_draw_calls()` joins only consecutive relocated,
  three-dword `0x97` draws.  Its live depth and Q3 gates have established that
  repeated draws may share one CALL.
* `parhelia_coalesce_range_calls()` joins only its exact admitted `0x8f`
  range class.
* The saved vendor submission reconstructs state in builder-owned sections.
  Captured direct draws are CALL-delimited from state; in the hybrid path the
  three-dword draw range and its following 16-byte fence are copied/called as
  separate sections.  The vendor evidence does not show a draw tail followed
  by the next logical state's prefix in one generic 4,096-dword section.

This explains why the first static tests passed.  They checked contiguous
storage, patched-byte preservation, synthetic stage separation, the opening
slot, and the 4,096-dword cap.  Their synthetic stream contained only one
logical state and no userspace-only mark.  They proved plan accounting, not
that every packet-class transition is semantically transparent to the P690.

## Ruled-out primary causes

The async secondary allocation is exact and contiguous.  It reserves the
original `command_dwords` plus fixed stage and sync payload sizes, and
`parhelia_async_block_assign()` advances `secondary_put` by each copied
section's dwords.  Packing changes section boundaries but not the sum of
copied packet bytes.  The primary allocation is based on the final
`total_calls`, and the plan is rejected above the 224-CALL hardware bound.
There is no corresponding under-allocation in the failing path.

Likewise, the Mesa estimator reproduces the kernel's generic contiguity,
opening, stage, and 4,096-dword rules.  A disagreement there would normally
produce `-E2BIG` and invoke the existing grouping/bisection fallback.  It
would not produce an accepted submit with missing geometry.

A hidden maximum safe secondary section below 4,096 dwords remains a useful
secondary hypothesis.  A scan of the available framed vendor capture corpus
found no naturally produced section above roughly 462 dwords.  That is not a
hardware limit: the builders may simply keep their sections small, and
glxgears successfully exercised generic packing.  Test the length limit
independently, but do not use it to justify re-enabling cross-class packing.

## Fix design

The immediate containment is to make parameter 24 unavailable.  The kernel
must return zero for `PARHELIA_PARAM_PACK_ADJACENT`, regardless of the module
option, so userspace retains the established draw-only and range-only
packers.  This avoids another accidental live gate while retaining the code
for controlled probes.

The first hardware-testable replacement should preserve packet execution
classes:

1. Classify every relocated three-dword `0x97` as draw.
2. Classify the exact admitted/coalescible `0x8f` form as range.
3. Treat remaining validated packets as state/control.
4. End a packed span whenever the class changes, as well as at every existing
   synthetic plan entry and the dword cap.
5. Keep the existing stricter rules inside draw and range runs.  A raw
   address-like `0x97` must never enter the draw class without its validated
   draw relocation.

For the present Q3 stream, class splitting restores the missing boundary
between a state batch's draw tail and the next state's prefix.  It should
still reduce the roughly 18 individual state-packet CALLs per state to a
small number of state spans and keep repeated draws packed.  Mesa must use
the identical classifier and split rules in its estimator.

The robust UAPI design is to transmit logical CALL boundaries explicitly.
A versioned packet flag or a separate boundary-index array is preferable to
overloading address relocations.  A boundary identifies the first packet of
each appended state batch and only terminates a storage span; it does not emit
EOB, DWGSYNC, an opening fence, or a new producer update.  The kernel must
validate that indices are sorted, unique, in range, and do not conflict with
synthetic stage placement.  Mesa's estimator must consume the same indices.

Using one new relocation entry per `pending_marks[]` item is less attractive:
the current aggregate reserves only 64 relocations beyond its packet-related
budget, while Q3 produces about 130 logical states per frame.  If a
`PARHELIA_RELOC_CALL_BOUNDARY` encoding is chosen, its capacity and UAPI
validation need an explicit increase rather than silently consuming the
existing relocation headroom.

Add matched tests containing state A prefix, opening, WARP bind and stage,
post-bind state, multiple relocated draws, an explicit logical CALL boundary,
then state B with the same structure.  Assert that:

* A's draw tail never shares a section with B's prefix;
* the kernel and Mesa compute the same CALL count;
* all patched packet bytes remain present and ordered;
* class, explicit-boundary, stage, opening, and 4,096-dword splits compose;
* grouping and recursive `-E2BIG` fallback retain every original state mark.

## Hardware validation ladder

1. Run a controlled two-state probe with separate CALLs as the reference.
   State B must visibly change scissor, texture, blend/depth state, and draw
   location.  Compare class-split and explicit-boundary variants by readback.
2. Alternate the two states for thousands of frames.  A later-state surface
   must never remain at its clear colour, and DRM completion/fault telemetry
   must remain clean.
3. Sweep homogeneous secondary spans around 256, 512, 1,024, 2,048, and
   4,096 dwords to determine whether a separate section-length ceiling exists.
4. Run the full Q3 screen/capture loop for at least one minute, comparing
   multiple frames rather than accepting the first intact image.
5. Finish with the required `vblank_mode=0` glxgears correctness/FPS gate and
   record async submit, slot-wait, tail-wait, and queue high-water telemetry.

Do not deploy a replacement merely because it restores glxgears performance.
The Q3 alternating-state gate is the test that exposed the semantic boundary.
