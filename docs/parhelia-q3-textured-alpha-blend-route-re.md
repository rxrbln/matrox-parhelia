# Q3 textured alpha-over route audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This is an offline audit of the retained vendor Linux/Windows binaries and
their command/state captures.  It asks whether the archive contains a native,
replayable P690 route for the Q3 fixed-function shape:

```
RGBA texture -> texture alpha -> source alpha ->
GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA blending
```

It makes no Mesa, kernel, target, or vendor-driver change.

## Result

The archive proves both end components, but **does not contain a combined
final packet or state delta that joins them on the P690**.

* The Q3 trace proves that the vendor ICD accepts an RGBA upload, requests
  alpha-over blending, and compiles the texture stage with texture alpha as
  its alpha result.
* The vendor capture matrix proves the native alpha-over blender encoding is
  `P3D_R_BLENDCTL (0x12e8) = 0x0080eced`.
* Separate textured captures prove the RGBA sampler format and the paired RGB
  and alpha `GL_MODULATE` fragment instructions.
* No retained command section combines an enabled alpha test or alpha-over
  blend with a textured RGBA draw and an A=0/A=255 result.  The P690 direct
  probe furthermore shows that composing the known texture and alpha pieces
  still treats an A=0 texel as passing.

Thus the known packet values are sufficient for vertex-alpha blending, which
is hardware-tested, but they do not implement sampled-texture alpha for Q3
sprites, UI, fog, or alpha-tested art.

## Direct Q3 evidence

The saved working P690 session has SHA-256:

```
4ade3269b3b6d482dd38c32778ef26dd5759cbaf9e34a05faef85168b2864768
captures/ioq3-p690-working-draw.txt
```

The captured GL sequence records `GL_TEXTURE_2D` enabled (line 84).  Immediately
before the retained draw submits, it records:

```
GL glTexImage2D target=0xde1 level=0 ifmt=0x8051 512x256 fmt=0x1908 type=0x1401
GL glEnable cap=0xbe2
GL glBlendFunc src=0x302 dst=0x303
```

The numeric values are `GL_RGBA`, `GL_BLEND`, `GL_SRC_ALPHA`, and
`GL_ONE_MINUS_SRC_ALPHA`.  This is direct API trace evidence that this Q3 draw
family requests conventional alpha-over compositing with four-byte texture
input.

At state-submit 5 offset `0x378`, key `0x0e` contains the unit-zero stage
fields:

| field | value | D3D texture-stage meaning |
|---:|---:|---|
| 1 | 4 | `COLOROP = MODULATE` |
| 2 | 2 | `COLORARG1 = TEXTURE` |
| 3 | 0 | `COLORARG2 = DIFFUSE` |
| 4 | 2 | `ALPHAOP = SELECTARG1` |
| 5 | 2 | `ALPHAARG1 = TEXTURE` |
| 6 | 0 | `ALPHAARG2 = DIFFUSE` |

Consequently the vendor context expresses `RGB = texture * diffuse` and
`A = texture.A`, rather than deriving source alpha only from a vertex colour.
This explains why opaque RGB stored in transparent texture texels produces
full quad rectangles when sampled alpha fails to arrive at the fragment path.

The retained trace is a state/submit extract, however.  It contains neither a
correlated final `0x12e8` word, the terminal sampler/combiner image, an alpha
test state, nor a readback for this particular textured alpha-over draw.

## Native blender route, independently proven

`tools/capture-gl-matrix.sh` defines the `blend` control as only
`GLTEST_BLEND=1`; it does **not** set `GLTEST_TEX=1`.  Its one-variable
command-stream diff in `captures/gl-matrix/attribution.txt` is therefore a
clean blender measurement, not proof of a textured draw.  It changes only:

```
0x12e8: 00800202 -> 0080eced
```

The saved Windows and Linux translators compose the Q3 GL factors to the
following native nibble values:

| GL factor | RGB factor nibble | alpha factor nibble |
|---|---:|---:|
| `GL_SRC_ALPHA` | 6 | 6 |
| `GL_ONE_MINUS_SRC_ALPHA` | 7 | 7 |

With ADD, dither, and blend enable, those fields pack as:

```
0x12e8 = 0x0080eced
```

This is a complete native route from a **valid source-alpha fragment result**
to alpha-over blending.  The P690 `--vertex-alpha-blend` diagnostic separately
proves that route with a real RGBA vertex attribute and texture coordinates.
It cannot establish source alpha from the sampled texture byte.

## Native texture route, independently proven

The vendor `GL_RGBA` texture capture uses A8R8G8B8 (`0x11e0` format code
`0x32`) and co-issues both fragment halves for `GL_MODULATE`:

```
RGB:   0x1264 = 0x08000002; 0x126c = 0x01400000
alpha: 0x1260 = 0x08000000; 0x1268 = 0x01400000
```

The alpha half is the documented `MUL_sat r0.a, t0.a, v0.a` form.  The
texture-only archive packets keep alpha test disabled (`0x12e4 = 0x0000000e`).
The alpha-test control packets instead use an untextured `MOV r0,v0` combiner.
They are distinct final draws; concatenating their state words would fabricate
a route not observed in the vendor stack.

The Linux vendor kernel does retain Q3's stage-alpha inputs through a real
compiler path:

```
key 0x0e fields 4/5/6
  -> raw descriptor +0x3c/+0x44/+0x48
  -> compiled record +0x10/+0x14/+0x18
  -> mtx950 sparse-map copies selected by runtime map nodes
```

The runtime nodes and their later map-to-command-buffer consumer are absent
from the static object and the retained Q3 extract.  This establishes a
plausible location for the missing distinction, but does not name a register
or authorize a guessed context write.

## P690 consequence

The P690 direct A8R8G8B8 test uses the known alpha half and comparator.  Its
`A=0` texel still paints RGB, while changing only vertex alpha to zero rejects
the fragment.  Retaining the vendor cache image and disabled unit-1 reservation
does not alter that result.  The alpha comparator, blender, vertex interpolation,
texture storage, and known combiner words are therefore not the missing
mechanism.

No native sampled-alpha blend implementation should be enabled from this
archive.  The existing narrow CPU alpha-cutout workaround also cannot be
extended to alpha-over draws until a separate CPU compositor is proven for
filtering, clipping, depth/stencil behavior, blend math, and tiled-target
coherency.

## Required evidence for a native implementation

Capture two otherwise identical draws on a separate known-good vendor P690
stack, using one RGBA texture, `GL_MODULATE`, vertex alpha one, alpha-over
blend, and A=255 versus A=0 texels.  Retain the raw key-`0x0e` record, the
post-`mtx2223` record, all eight `mtx950` map containers and selected node
payloads, key `0x46`, final sampler and fragment packets, `0x12e8`, and a
result pixel.  Only a same-draw, result-correlated delta can define a safe
native P690 path.

See `parhelia-blend-alpha-encoding.md`,
`parhelia-vendor-alpha-texture-emission-audit.md`,
`parhelia-q3-alpha-stage-packet-route-re.md`, and
`parhelia-vendor-stage-context-map-re.md` for the underlying disassembly and
capture decoding.
