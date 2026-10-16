# M9 sRGB render-target write in the Mesa driver

Implements the write half of sRGB.  **Sampler-side sRGB decode is not
implemented and nothing here advertises it**: the vendor's own GL never
exposed `GL_EXT_texture_sRGB`, and
`parhelia-m9-srgb-format-static-closure.md` shows the D3D9 sampler state 29
transport is live but no pinned lowerer consumes its bit, so there is no
joined decode path to copy.

## What the driver does

`m9_fs_compile` takes a `struct m9_fs_blend` with an `srgb` flag.  When it is
set the backend rewrites the fragment colour output with

```
c = max(x, 0)
y = c > 0.0030400000978261232 ? 1.0549999475479126 * exp2(0.4166666567325592
                                                          * log2(c))
                               - 0.054999999701976776
                             : 12.920000076293945 * c
```

for R, G and B, leaving alpha alone.  Those are the vendor's own constants,
including its `0.00304` threshold, taken from the CPU conversion its colour
clear performs at `0x13e169..0x13e23d`.

The `max(x, 0)` is not cosmetic: it keeps `log2` off the negatives, and it
puts the value in a general register, which a special-function chain needs
its operand to be -- without it a shader that writes a texel straight out is
refused as "special function of a texture read".

Blending is refused: `"blending into an sRGB target (destination decode is
not implemented)"`.  GL blends an sRGB target in linear space, which needs
the destination decoded before the blend, and decode does not exist.  The
backend refuses rather than blending in encoded space.

Cost: a plain textured shader grows from 4 slots to 71.

MEASURED offline, `m9-fs-compile-test --srgb`: the lowered NIR, folded,
equals the reference curve to 1e-6 at 0, 0.0005, exactly the threshold,
0.0031, 0.01, 0.05, 0.2, 0.5, 0.8 and 1.0 -- probes on both sides of the
knee -- and alpha comes through untouched.

## Why the vendor's 25-slot suffix is not replayed

`parhelia-m9-srgb-format-static-closure.md` records the exact 25 slots
`0x174ea0` inserts for `D3DRS_SRGBWRITEENABLE` on render target zero.
Decoding them with this backend's field layout (`w0_make`/`w1_make`:
a = W0[31:29] | W1[3:0]<<3, b = W0[28:22], c = W0[21:15], d = W0[14:8],
class = W0[7:5], op0 = W0[4:0], e = W1[20:14], op2 = W1[25:21]) gives:

* slots 0..2 read register `0x10`/`0x12` and write `0x00`, `0x02`, `0x04`;
* the middle works in `0x06`, `0x08`, `0x0a`, `0x0c`;
* slots 21..24 write back `0x10` and `0x12`.

Two things stop a verbatim replay:

1. **The register convention differs.**  The suffix carries the colour in
   `0x10`/`0x12`; every program this backend emits -- and every vendor GLSL
   program in `m9_fs_corpus.h` -- writes the colour output `0x2d`.  Placing
   the suffix would mean guessing how a value moves between the two.
2. **Three of its opcodes are undecoded.**  It uses op2 `0x11`, `0x13` and
   `0x18`.  `0x13` is the shared polynomial step whose semantics
   `tools/m9-native-eval.py` still does not model, so the sequence can be
   neither evaluated offline nor checked against a reference before it is
   put on the hardware.

The lowering above uses only forms the corpus and the hardware fixtures
already cover, and it is checkable offline.  The suffix remains the thing to
replay if a capture ever joins those opcodes; its exact words are in the
static-closure note.

## What is still needed to turn it on

The backend half is complete.  Three lines outside it are not, and they are
what `m9-mesa-glsl srgbwrite` waits for: an sRGB colour format in the format
table, the flag set from the bound surface, and the flag in the variant key.
They are listed in the driver handover.  Until they land, no sRGB capability
is advertised and the case reports "no sRGB render target".
