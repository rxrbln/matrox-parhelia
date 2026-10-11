# P690 sampled-alpha emission audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a local-only audit of the saved Linux and Windows vendor drivers,
their captured command streams, and the current open-driver state builder.  It
addresses one narrow failure: on the P690, an A8R8G8B8 texel with `A=0` passes
the otherwise correct `GL_ALPHA_TEST(GEQUAL, .5)` path, while a zero vertex
alpha correctly fails it.

It does not change Mesa, the kernel, the target, or a vendor driver.

## Result

The missing path is **not** an omitted alpha-combiner packet.  The saved
vendor `GL_MODULATE` streams write both halves of both live fragment stages,
and the open driver emits the same four words.  The visible RGB/RGBA packet
difference is only the sampler format code (`0x33` X8R8G8B8 versus `0x32`
A8R8G8B8).  The direct P690 probe already uses `0x32` and still passes an
`A=0` texel.  Therefore neither re-emitting `0x1260/0x1268` nor changing the
format descriptor is a justified fix.

The remaining documented input which distinguishes a sampled-alpha operation
from an RGB-only operation is the vendor texture-stage context compiler.  Its
output reaches heap-backed context maps, rather than a recoverable fixed MMIO
word in the saved static objects.  A same-draw vendor capture is required to
turn that observation into a hardware programming sequence.

## Direct packet comparison

Run from `xf86-video-parhelia`:

```
python3 tools/decode-gl-secs.py --regs 1200,11e0,1260,1264,1268,126c,12e4 \
  captures/gl-matrix/tex-secs.txt captures/gl-matrix/texrgb-secs.txt
```

The final textured sections in both captures contain this complete co-issued
program (the output has repeated draws; the tuple is invariant):

```
                     RGBA tex                 RGB tex
0x1200              0x00040103              0x00040103
0x11e0              0x32020002              0x33020002
0x1264, 0x1260      0x00004000, 0x00004000  0x00004000, 0x00004000
0x126c, 0x1268      0x00000c00, 0x00000c00  0x00000c00, 0x00000c00
0x1264, 0x1260      0x08000002, 0x08000000  0x08000002, 0x08000000
0x126c, 0x1268      0x01400000, 0x01400000  0x01400000, 0x01400000
0x12e4              0x0000000e              0x0000000e
```

The first pair is texture address slot 0.  The second pair is the ALU at slot
2: saturated `MUL r0, t0, v0` for RGB and alpha respectively.  This is the
exact `textureA * vertexA` formulation needed by `GL_MODULATE`; it is already
present even in the RGB capture, where `0x33` makes the sampler supply an
opaque alpha.  It follows that a new alpha-source word cannot distinguish the
two formats.

The saved alpha-only stream has the different, untextured program:

```
0x1264, 0x1260 = 0x00000200, 0x00000200   /* MOV r0, v0 */
0x126c, 0x1268 = 0x01400400, 0x01400400
0x12e4              = 0x00008009          /* GREATER, 0.5 */
```

It proves the comparator operates on the alpha result, but it cannot be
composed with the textured capture because the two final draws are distinct.

## Open-driver cross-check

`src/gallium/drivers/parhelia/reference/p3d_state.c` emits, for every
one-texture MODULATE draw:

```
0x1264 = 0x00004000;  0x1260 = 0x00004000
0x126c = 0x00000c00;  0x1268 = 0x00000c00
...
0x1264 = 0x08000002;  0x1260 = 0x08000000
0x126c = 0x01400000;  0x1268 = 0x01400000
```

This is byte-for-byte the vendor sequence above.  The active Gallium path
uses that builder, so any failure of sampled alpha cannot be attributed to a
missing alpha texture-address stage, a missing co-issued alpha ALU, or the
known comparator encoding.  This also explains why vertex alpha works: it
arrives through `v0`, whose routing is already proven by the same program.

The PCI-X capture's `0x1200=0x00040103` is deliberately not a P690 candidate.
The P690 test path uses its known-safe `0x00040101`; `03` wedges that hardware
and must never be retried as an alpha experiment.

## Vendor stage-context boundary

The saved Linux object makes the pre-MMIO state path concrete:

* `mtx673` (`mtx_parhelia.o+0x17260`) accepts texture-stage state.  Fields 4,
  5, and 6 land at raw descriptor offsets `+0x3c`, `+0x44`, and `+0x48`:
  `ALPHAOP`, `ALPHAARG1`, and `ALPHAARG2`.
* `mtx2223` (`+0x17e70`) copies those three values to output-record offsets
  `+0x10`, `+0x14`, and `+0x18`, respectively, for each stage.
* `mtx3136` gets the stage-zero output through `mtx580`, then calls `mtx950`.
  `mtx950` walks eight 0x74-byte source slices and installs values in
  heap-backed map nodes.  It does not emit a register list or write the known
  sampler (`0x11c0..0x11e8`), combiner (`0x1260..0x1284`), or alpha-test
  (`0x12e4`) registers.

The working P690 ioquake3 record shows a live unit-0 prefix
`COLOROP=MODULATE(4), COLORARG1=TEXTURE(2), COLORARG2=DIFFUSE(0),
ALPHAOP=SELECTARG1(2), ALPHAARG1=TEXTURE(2), ALPHAARG2=DIFFUSE(0)`.  That
record is useful proof that alpha-stage fields are real compiler inputs.  It
is not an alpha-cutout capture: it lacks a correlated format, comparator, and
pixel result.  Replacing its `SELECTARG1` by `MODULATE` is consequently a
hypothesis, not a safe register write.

## Minimal safe diagnostic

The next diagnostic must start with a matched vendor capture on a separate
known-good P690 vendor stack.  It must not load a vendor driver on the current
target.  Use a single 2D unit with `GL_MODULATE`, vertex alpha one, blending
and depth disabled, fixed clear colour, and four draws:

| draw | texture | alpha test | expected result |
|---|---|---|---|
| control | RGBA, A=255 | disabled | paint |
| opaque | RGBA, A=255 | `GEQUAL,.5` | paint |
| cutout | same RGB, A=0 | `GEQUAL,.5` | retain clear colour |
| vertex control | A=255, vertex A=0 | `GEQUAL,.5` | retain clear colour |

For the final submission of each draw, retain the complete key-`0x0e` stage
record, its copied output record, dynamic map-node contents for IDs 4--6,
the full key-`0x46` object, final sampler window, fragment packet, and a
readback pixel.  Diff opaque against cutout before proposing any replay.

Only an observed complete delta may be used in a private diagnostic.  That
diagnostic must retain the safe P690 pipe value `0x00040101`, have a timeout,
verify all four pixels and a retired fence, and remain outside normal Gallium
selection.  If a module is involved, stage it only in `/tmp/parhelia-stage`
and use `modprobe -d /tmp/parhelia-stage`; never install it below
`/lib/modules`.

No register sweep, cache-word guess, XRGB substitution, paired-pipe test, or
blind stage-record injection can distinguish the remaining hypothesis from an
unrelated sampler/context failure.
