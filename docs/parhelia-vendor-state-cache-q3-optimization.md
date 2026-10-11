# Vendor state caching and bounded Q3 packet compaction

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

Both retained vendor drivers cache 3D state across draws.  The Linux OpenGL
ICD does this twice: GL setters update an application-state shadow and dirty
word, while validation compares each resulting item against a per-device
shadow before writing a record.  The old Linux kernel driver then compares
the decoded state record with its own slot cache, sets hardware-group dirty
bits only for changed slots, and emits only those dirty groups.  The Windows
P690 driver uses the same final shape in its `ValidateState` routine.

The open Mesa driver instead calls `p3d_emit_state()` for every batch.  It
therefore emits every recovered register value again, split across roughly
twenty to forty secondary packets depending on texture count and state.  A
full persistent dirty-state cache cannot safely omit render-target, depth, or
texture base relocations: those relocations are also the kernel's BO ownership,
pinning, and fence list for that submission.  Caching them only as hardware
register values would leave the kernel unaware of an object the GPU accesses.

The first implementation is consequently narrower.  It compacts adjacent
ordinary register-list packets into a single register-list packet while
retaining every indexed register write in its original order.  It does not
join packets carrying relocations and does not cross fetch, WARP, texture-bank,
surface, arm, fence, end-of-batch, or strobe registers.  Each output list is
capped at 64 dwords.  The P690 image, depth, texture, multitexture,
vertex-alpha, async and hang gates passed, so normal window-space batches now
enable it by default.  `PARHELIA_COMPACT_STATE_PACKETS=0` restores the original
packet separation for diagnosis.  A range-stream batch compacts only the
ordinary prefix: its stream/primitive/bounds/descriptor/range tail remains
byte-exact and its relocation owner is remapped.  Transform/constant-loader
batches retain their exact CALL cadence.

## Vendor cache evidence

The Linux ICD entry-point tables named `GLSendStateHardwareTnL`,
`GLSendStateHardwareTnLVS`, and `GLSendStateSoftwareTnL` initially point at a
validation shim.  `libGL64` `0x116140` validates dirty state and replaces the
thread-local dispatch table with the already-valid table.  A later GL state
change reinstalls the shim.  In the large state emitter at `0x1117c0`, every
item follows this form:

```
if (new_value != device_shadow[index]) {
        device_shadow[index] = new_value;
        emit(key, index, new_value);
}
```

The pipeline key has a six-dword shadow at device offset `+0xb10`; individual
values use shadows such as `+0x96c`, `+0x274`, `+0x23c`, and `+0x910`.  The
coarse dirty word at the primitive object's `+0x238` selects pipeline-key,
vertex-declaration, texture, vertex-data, and program records.  Buffer
exhaustion or an explicit end-of-frame/finish submits the accumulated record
stream; an ordinary draw does not force a submission.

In `mtx_parhelia.o`, key `0x05` and `0x0c` records first pass through caches at
`HWSTATE+0x3a0+slot*4` and `HWSTATE+0x10+slot*4`.  Equal values are discarded
before their slot handlers run.  Changed values update the bit-packed state
shadow and per-group dirty flags.  `mtx3712` at `0x188f0` validates those
groups into the hardware-register shadow, and `mtx1342` at `0x90010` walks its
dirty bitmap to emit register packets.  Thus repeated GL calls and repeated
derived values are both suppressed.

The Windows `MTXPARD.dll` routine at `0xc9ac0` is an explicit `ValidateState`.
It conditionally calls the raster, depth, cull, surface, and fragment-group
emitters, then emits the drain/arm sequence.  Its per-draw prolog confirms that
state groups may be omitted, while the draw arm and ordering-sensitive latches
remain explicit.

The detailed binary addresses and dirty-bit maps are in
`parhelia-state-slot-map.md` sections 1 and 3,
`parhelia-gl-limits.md` sections 7.1 through 7.5, and
`parhelia-fragment-gating-re.md` section 5.4b.

## Open-driver implementation

`parhelia_batch_compact_reglists()` in Mesa's `parhelia_batch.c` accepts one
already validated `struct parhelia_batch` and builds a second batch.  It:

1. validates every packet range and relocation owner;
2. recognizes exact opcode-zero register-list framing;
3. rejects a packet from compaction if any relocation points at it;
4. treats known latch, FIFO, address-window, surface, and synchronization
   registers as boundaries;
5. concatenates only adjacent eligible payloads, rewriting the opcode-zero
   length field; and
6. remaps relocation packet indices after earlier packet contraction.

The selftest constructs two mergeable lists followed by a strobe boundary and
a relocated draw.  It verifies the exact merged dwords, retained strobe
boundary, contracted packet offsets, and remapped draw relocation.  It also
compacts a normal generated state batch and checks its scissor and draw-arm
values.  On that reference batch, packet count falls from 24 to 15 and command
size from 131 to 122 dwords.  The command-size reduction is only the removed
per-packet headers; the larger intended saving is nine fewer secondary CALLs
and validator iterations.

## Texture and WARP observations

The normal Mesa texture upload path already follows the vendor dirty model.
`ph_resource_upload_tiled()` returns unless the linear image is newer, and it
clears `linear_dirty` after one GPU or CPU copy.  No ordinary Q3 draw repeatedly
uploads an unchanged texture.

The `R8G8B8A8` to `B5G5R5A1` alpha workaround used to be an exception.
`ph_prepare_rgb5a1()` now caches both binary-alpha eligibility and packed data
against `content_generation`.  CPU texture writes clear the packed flag and
advance that generation; restoration refuses to overwrite content from a
newer generation.  Repeated HUD batches therefore avoid rescanning and
repacking an unchanged atlas.

WARP program upload is already cached at the right lifetime.  Context creation
asks the kernel for stable entry points, and state batches bind those entry
addresses; they do not upload instruction memory per draw.  Omitting or
coalescing WARP bind packets is unsafe without hardware evidence because the
bind includes repeated mode latches and a strobe whose packet shape was needed
during bring-up.

## Hardware validation result

The compacted path passed one-triangle, depth-texture, one-texture, P690
one-pass two-texture MODULATE, vertex-alpha, async-queue and fixed Q3 screenshot
tests without a timeout or new pixel error.  The known sampled-texture-alpha
failure is unchanged and remains outside this optimization.

On the same P690 module and Mesa build, the fixed 640x480 Q3DM1 window with
swap interval disabled changed from 120.06 ms average backend time (8.33 FPS)
to 84.62 ms (11.82 FPS).  Uncapped glxgears produced 249.22 and 249.99 FPS,
compared with 193.93 and 178.09 FPS in the preceding uncompact run.  The fixed
Q3 screenshot retained the same static world pixels; remaining differences
were confined to animated actors, HUD counters, and the already documented
sampled-alpha rectangles.

The production P690 bound-range transport reduced average Q3 relocation count
from roughly 101 to 5 per submit, but its extra direct-fetch setup CALLs left
backend performance at 8.42 FPS even with prefix compaction.  It therefore
remains a default-off kernel capability while the normal `0x97` path keeps the
larger measured packet-compaction win.
