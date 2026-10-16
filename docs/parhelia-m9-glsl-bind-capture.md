# M9 programmable-stage bind capture

This note closes the live ownership boundary between a linked GLSL program and
the M9 external program-state packet.  It uses two controlled same-context
A/B/A captures in `captures/m9x-linux-glsl-ab-20260914`: fixed vertex shader
with two fragment shaders, then fixed fragment shader with two vertex shaders.
The binary-side compiler, constant, and final-emitter paths remain documented
in `parhelia-m9-program-stage-entry-re.md` and
`parhelia-m9-program-constants-re.md`.

Facts read directly from a capture are marked **CAP**, static-binary joins
**BIN**, and conclusions **DER**.

## Experiment

`tools/m9x-glsl-ab.c` creates all four shaders and the three linked programs
before the first marked draw.  It keeps one GLX context and one drawable, then
submits A, B, and A again with `glFlush` and a pause after each draw.  The
shaders differ in one stage only:

* FS-A passes interpolated color; FS-B multiplies it by a constant color;
* VS-A performs the fixed-function matrix transform; VS-B adds `0.125*w` to
  clip-space X.

All six draws use the vendor renderer, direct rendering, OpenGL 2.0, and return
`GL_NO_ERROR`.  The marked draw submissions are ExecuteDma sequences 15, 19,
and 23 in each process.  Every submission has the same 38-packet preprocess,
37-packet raster, and three-packet completion shape as the earlier `gltri`
fixture.  **CAP**

## Atomic linked-pair selection is through index 6c

Both stage experiments produce the same exact program-state tuple in both
draw passes:

| selection | `69` | `6a` | `6b` | `6c` |
|---|---:|---:|---:|---:|
| A | `11e42000` | `25010000` | `00001030` | `40c3a301` |
| B | `11e42000` | `25010000` | `00001030` | `40c3a881` |
| A repeat | `11e42000` | `25010000` | `00001030` | `40c3a301` |

Thus changing only the fragment shader or only the vertex shader selects a new
record through `6c`; restoring the original pair restores the exact prior
value.  The shared external resource at `69` and controls `6a/6b` do not change.
The selected unit in this live configuration is a linked vertex-plus-fragment
pair, not two independently switched command-register entries.  A driver must
cache and bind the complete linked pair atomically for this path.  **CAP/DER**

This resolves the prior final-writer ambiguity.  Static emitter analysis says
`6c` is produced by relocating a resource with usage `0x1a` and descriptor 4.
Allocation-ioctl capture identifies the owner: allocation zero returns GPU
base `0x40c3a000` at result offset `+0x208`, and its corresponding userspace
mapping is 0x21000 bytes.  Clearing the low seven alignment/tag bits gives
record offsets `0x300` for A and `0x880` for B.  The B record is all zero before
its first use and populated at the first B submission; A is retained and is
selected again without rebuilding its address.  **CAP/BIN/DER**

The low bit in `40c3a301/40c3a881` is demonstrably outside the 0x80-aligned
record offset.  Its exact hardware name remains unknown, so relocation code
must preserve it as a typed tag rather than treating the full value as a byte
address.  The captured record bodies contain floats, tables, and pointers and
do not contain the proved native terminal instruction.  They are linked state
records, not native instruction streams.  **CAP/DER**

## Vertex-stage discriminator

Only the vertex-shader A/B change alters index `23` in the preprocessing pass.
The corresponding fragment-only A/B change leaves the complete `23` write
sequence unchanged.  This assigns the changed preprocessing vector/data-port
record to the vertex side without relying on vendor `Ps` or `Ce` names.
**CAP/DER**

The other A/B deltas include allocation addresses, serials, and resource
records.  They are intentionally left numeric because A/B/A identity, rather
than cross-process address equality, is the reliable discriminator.

## What the indexed stores are not

No draw switch emits data through `90..9f`.  Index `8f` receives only two zero
writes per pass.  The 80-slot `8f/90..9f` image seen at context startup is also
identical across fixed-function gltri, glxgears, and ioquake3 captures.  It is
context/invariant microcode or table state and is not the selected application
GLSL vertex or fragment program.  The `68/60..67` inline uploader remains a
separate vendor path.  **CAP/DER**

## Driver contract and remaining shader work

The external path can now be represented without opaque application state:

1. compile and link the vertex/fragment pair into an immutable kernel-tracked
   program object;
2. retain the `69` external resource and the `6c` selected linked-state record
   as relocations, preserving their captured tag bits;
3. emit the invariant `6a/6b` controls for the proved configuration;
4. bind the same linked object for preprocess and raster passes;
5. keep the object resident until the completion writeback retires.

The capture does not yet reveal the native instruction bytes referenced by the
linked-state record, distinguish the internal vertex and fragment entry fields,
or establish the mode-2 constant backing layout.  The static constant path is
otherwise closed through GL locations, stage-separated persistent arrays,
`0x65/0x66` updates, and the compiler bank reader.  A uniform-sentinel A/B/A
capture is still required before bus-master constants can be generated safely.
Remaining ISA work is limited to operation/source/modifier combinations and
texture metadata not covered by the audited lowerer corpus; those are compiler
coverage gaps rather than final program-state ownership gaps.

Run `python3 tools/m9-glsl-ab-evidence.py --self-test` to decode all six draw
streams and verify the tuple, resource allocation join, record offsets,
indexed-store exclusion, and vertex-only index-`23` delta.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
