# P690 Q3 sampled-alpha final audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The saved vendor material proves that a P690 Q3 OpenGL-1 draw asks for
sampled texture alpha and uses the standard alpha-over blend factors.  It does
**not** contain a replayable P690 state that makes sampled alpha reach the
fragment alpha result.  Do not enable a native Mesa sampled-alpha route from
these values.

This is deliberately a narrow conclusion.  Vertex-alpha blend is proven and
works; texture-alpha cutouts and texture-alpha source blending remain
unproven on P690.

## Exact vendor Q3 input

The checked trace is
`captures/ioq3-p690-working-draw.txt` (SHA-256
`4ade3269b3b6d482dd38c32778ef26dd5759cbaf9e34a05faef85168b2864768`).
It records a `GL_RGBA` upload followed by:

```
GL_BLEND
GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
```

At state-submit 5, key `0x0e`, offset `0x378`, unit zero has:

| field | value | interpretation |
|---:|---:|---|
| 1 | 4 | `COLOROP = MODULATE` |
| 2 | 2 | `COLORARG1 = TEXTURE` |
| 3 | 0 | `COLORARG2 = DIFFUSE` |
| 4 | 2 | `ALPHAOP = SELECTARG1` |
| 5 | 2 | `ALPHAARG1 = TEXTURE` |
| 6 | 0 | `ALPHAARG2 = DIFFUSE` |

Thus this vendor context explicitly requests `RGB = texture * diffuse` and
`A = texture.A`.  Key `0x0e` is a driver context input, not a raw P3D packet.

## Exact compatible packet components

The separately captured one-unit RGBA route has the P690-safe sampler and
the expected alpha combiner:

```
0x1200 = 0x00040101       unit 0, enabled 2D
0x11e0 = 0x32020040       A8R8G8B8, layout 2, width 64
0x1260 = 0x00004000       alpha texture-address slot
0x1268 = 0x00000c00
0x1260 = 0x08000000       saturated alpha MUL t0.a, v0.a
0x1268 = 0x01400000
0x12e8 = 0x0080eced       SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD
```

The alpha-test packer is independently known.  For example,
`GL_GREATER, 0.5` is `0x12e4 = 0x00008009`.  These values prove the
comparator, blender, vertex-alpha interpolation, texture storage, and known
combiner encoding independently.

## Why this is not an implementation

The direct P690 probe used the safe `0x00040101` control, A8R8G8B8, the
co-issued alpha `MUL`, and a correct comparator.  A texel with texture alpha
zero still painted RGB.  Changing only vertex alpha to zero rejected the
same primitive.  The known packet therefore reaches `v0.a` but does not prove
that `t0.a` is delivered to the fragment alpha path on P690.

The archive has no single draw that contains all of: an RGBA A=255/A=0
comparison, final sampler and combiner packet, enabled alpha test or
alpha-over blend, a rendered result, and the compiler-produced delta from
key `0x0e`.  The Linux vendor object carries fields 4--6 through:

```
stage descriptor +0x3c/+0x44/+0x48
  -> compiled record +0x10/+0x14/+0x18
  -> mtx950/mtx713 heap-backed sparse maps
```

but the dynamic map destinations and their later hardware consumer are not
retained.  They cannot be reconstructed as fixed P3D registers from this
binary alone.

`0x00040103` is a PCI-X paired-pipe state that wedges this P690.  Adding
`0x11e8`, injecting key `0x0e`, interpreting key `0x46` as a packet, or
changing the known alpha ALU would be speculation and must not enter Mesa.

## Needed proof

Capture a known-good vendor P690 run with otherwise identical one-texture
RGBA A=255 and A=0 draws, fixed clear colour, `GL_MODULATE`, vertex alpha one,
and alpha test or alpha-over blend.  Preserve the complete key-`0x0e` record,
the copied compiled record, the dynamic sparse-map values for IDs 4--6, the
final sampler/combiner/alpha packet, and a pixel result.  Only a correlated
delta that paints A=255 and rejects A=0 can define a private diagnostic, then
a native Mesa implementation.
