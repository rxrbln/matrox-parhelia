# Parhelia Q3 UI and alpha audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a focused audit of the remaining incorrect ioquake3 UI and
alpha-tested draws.  It distinguishes missing texture coordinates, texture
alpha, and vertex alpha.  The first is a present correctness bug; the latter
two are separate follow-up work.

## Status update: hardware-verified depth + RGBA stream

The exact one-texture stream below is now verified on the P690.  It is enabled
in Mesa only for one-texture `MODULATE`, a vertex-colour fragment input, an
enabled Z compare, no alpha test, source-alpha `ADD`, and a matching destination
factor of either one-minus-source-alpha or one on RGB and alpha.  The offscreen
`parhelia-drm-test --depth-vertex-alpha-blend` diagnostic
drew a half-alpha red triangle through a white RGBA texture
and a Z24 `LESS` surface, producing `0x8f2030` for expected `0x902030` (the
three-count difference is the normal hardware dither/rounding allowance).
The same triangle was replayed after clearing Z to zero; its sample remained
unchanged, proving the depth compare rejected it.  Both submits retired their
fences and produced no DRM timeout.

```
vertices:    x, y, z, r, g, b, a, s, t       (9 dwords)
descriptors: 0x21c00000, 0x23c50030, 0xa0c70070, 0
```

`0x23c50030` is `v5 = rgba` at dword 3; `0xa0c70070` is `v7 = st` at dword
7 and is final.  The old depth branch in `p3d_descriptors()` returned before
considering RGBA, emitted RGB plus ST at dword 6, and consequently consumed
the input alpha as S.  That explains the prior scene corruption; it was not a
failure of the descriptor values themselves.

The batch validator requires `p3d_state.depth_vertex_alpha` for this form and
rejects alpha-test, non-MODULATE, non-source-alpha, no-blend, inactive-depth,
multi-texture, and transformed packets.  Mesa sets the marker only after
repeating the same selection predicate against the resolved resources.  The
serial two-texture packer still needs its independent 11-dword proof.

## 1. Depth-plus-texture descriptor history

The current RGB depth-plus-texture path is already valid:

```
0x21c00000       xyz position at dword 0
0x21c50030       rgb colour at dword 3
0xa0c70060       st at dword 6, final
```

An older source revision returned from the `with_z` branch before emitting ST;
that historical flaw is not present in the tested source.  The later failed
experiment was different: it supplied a nine-dword RGBA stream but the depth
branch still emitted the **RGB** descriptors above.  It therefore read alpha
as S and S as T.  The verified diagnostic at the top of this document fixes
that composition under a private gate.

The ordinary RGB descriptor sets are:

| layout | descriptors, in emission order |
|---|---|
| depth + 1 texture | `0x21c00000`, `0x21c50030`, `0xa0c70060` |
| depth + 2 textures | `0x21c00000`, `0x21c50030`, `0x20c70060`, `0xa0c80080` |

The first two fields preserve the existing proven `xyz` position/colour
layout.  In the one-texture case `0xa0c70060` is the last descriptor: slot 7,
two components, offset six dwords.  In the two-texture case the first
coordinate descriptor loses the last bit and the second maps two components
at offset eight dwords to slot 8.  This is the depth equivalent of the
already-proven non-depth sequences
`0x20c00000,0x21c50020,0xa0c70050` and
`0x20c00000,0x21c50020,0x20c70050,0xa0c80070`.

The descriptor fields are confirmed by the vendor capture, kernel parser, and
the depth-RGBA hardware diagnostic:
`[11:4]` is the dword offset, `[19:16]` is the WARP destination (`5`, `7`,
`8`), `[25:22]` is the component mask, and bit 31 is last.  See
`parhelia-textured-draw.md` sections 4.4--4.5 and
`parhelia-vertex-fetch-and-memory-classes.md` section 3.

## 2. Texture-alpha result: storage is correct; sampler-to-alpha is not

The Q3 trace creates `PIPE_FORMAT_R8G8B8A8_UNORM` textures and Mesa selects
sampler format `0x32` (vendor A8R8G8B8).  The transfer path retains byte 3.
That proves the storage choice, but it does **not** prove that the P690 sampler
feeds byte 3 into the alpha pipe.

`parhelia-drm-test --depth-alpha-cutout` is an isolated depth-plus-one-texture
probe.  It uses the exact depth/RGBA stream

```
x, y, z, r, g, b, a, s, t
0x21c00000, 0x23c50030, 0xa0c70070, 0
0x12e4 = 0x00008009       /* GREATER, reference 0x80, enabled */
0x1200 = 0x00040101       /* P690 single-pipe ownership */
0x11e0 = 0x32020040       /* 64-wide A8R8G8B8, sampler layout 2 */
0x1260 = 0x08000000; 0x1268 = 0x01400000
```

The P690 result is decisive:

* An all-green `A=0` tiled texture appears green at the target, so the sampler
  address and RGB channel are correct, but it passes alpha test
  (`texA0=0x00fe00`).
* Replaying the same packet with opaque red and vertex `A=0` leaves the
  background unchanged.  Vertex `A=1` then paints red at the same Z, and a
  cleared-near Z replay is rejected.  Therefore `0x12e4`, the compare,
  depth-before-write ordering, the RGBA descriptor, and the alpha pipe itself
  are all hardware-proven.
* Programming the vendor one-stage texture-cache image `0x11e8=0x00500000`
  does not alter the result.  Forcing the paired `0x1200=0x00040103` ownership
  times out on this P690, so it is not a usable fix.  A diagnostic linear
  layout-0 allocation produces malformed RGB and cannot establish alpha
  routing.
* The remaining capture-only ownership difference was tested separately:
  with the live P690 sampler held at `0x1200=0x00040101`, retaining the
  vendor's *disabled* unit-1 reservation
  `0x1204=0x0001000c` still yields `texA0=0x00fe00`; the vertex-A-zero replay
  rejects and the opaque replay paints red.  Thus the missing contribution is
  not caused by Mesa clearing the inactive vendor pipe reservation.  This was
  run only through `PARHELIA_CUTOUT_VENDOR_IDLE=1 parhelia-drm-test
  --depth-alpha-cutout /dev/dri/renderD128`; it is a test-only packet form and
  is not selectable by Gallium.

The vendor `GL_RGBA` MODULATE combiner words above are still the correct
captured lowering.  The remaining open item is the P690-specific
texture-address/sampler-to-alpha route or a required context state outside the
captured per-draw combiner image.  Do not enable Mesa alpha-tested texture
packets from this evidence and do not change `0x32` to `0x33`; `0x33` is XRGB
and is known to force opaque alpha.

The confirmed alpha-test encoding is

```
0x12e4 = enable | (compare-mask << 1) | (reference8 << 8)
```

The test-only `depth_alpha_test_probe` marker accepts only this exact
`GREATER .5`, depth-enabled, unblended descriptor form.  It is never selected
by Mesa production state.

## 3. Vertex alpha is currently forced to one

Mesa currently emits `x,y[,z],r,g,b[,s,t]`; it drops `c[3]`.  Its normal
textured descriptor is RGB (`0x21c50020`), so the interpolator supplies
`v5.w = 1.0`.  The existing kernel `warp_textured` already performs
`add o1, v5, r1.0000`, which copies all four components; a new WARP program
is unnecessary.

The existing self-test proves the needed fetch encoding: changing the
colour descriptor component mask from `0x7` to `0xf` supplies a real
per-fragment alpha.  The exact descriptor is:

```
RGB  colour: 0x21c50020       RGBA colour: 0x23c50020
```

For an alpha-capable Mesa packet, retain `c[3]` and shift each following ST
offset one dword:

| layout | descriptors after position |
|---|---|
| no depth, one texture | `0x23c50020`, `0xa0c70060` |
| no depth, two textures | `0x23c50020`, `0x20c70060`, `0xa0c80080` |
| depth, one texture | `0x23c50030`, `0xa0c70070` |
| depth, two textures | `0x23c50030`, `0x20c70070`, `0xa0c80090` |

This requires a state bit such as `colour_alpha`, not a blind replacement:
RGB-only draws must retain their existing stride/descriptor.  It also requires
the Mesa staging arrays and `dw_per_vertex` accounting to grow by one dword
when that bit is set.  The largest alpha + depth + two-ST packet is 11 dwords.

Vertex alpha is now proven to reach blending and `0x12e4` in the depth/RGBA
stream.  The vendor MODULATE lowering encodes `A = textureA * vertexA`, but the
P690 probe currently observes only the vertex contribution at alpha test.
Texture-alpha cutouts remain disabled until the missing sampler-to-alpha route
is recovered.

### 3.1 Mesa integration constraint: do not widen every textured draw

The fetch encoding and the untextured depth+RGBA path are hardware-verified,
but the depth+RGBA+texture sequence above has not yet been captured as a raw
vendor ring packet.  It is the direct descriptor-field construction and is a
good probe candidate, not a blanket replacement for every Mesa texture draw.

This distinction matters on P690.  Mesa serialises a two-texture draw through
`ph_pack_multitex_vertices()`.  Its current source layout is fixed at ten
dwords with depth (`x,y,z,r,g,b,s0,t0,s1,t1`) and it packs an eight-dword
one-texture draw.  Adding `a` changes those layouts to eleven and nine dwords:

```
source: x,y,z,r,g,b,a,s0,t0,s1,t1
packed: x,y,z,r,g,b,a,s,t
```

Therefore the existing helper reads `a` as `s0`, `s0` as `t0`, and so on if a
global `colour_alpha` bit changes the staging layout.  This exactly explains a
corrupted world pass after the broad depth-alpha experiment; it is not evidence
against `0x23c50030` itself.

The safe first Mesa scope is a *single-texture* draw whose RGB blend factors
read source alpha (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`, or
`SRC_ALPHA_SATURATE`).  Such a draw must be flushed before changing blend
state, which the existing `ph_bind_blend()` already does.  Carry alpha only in
that batch, with an explicit `p3d_state.colour_alpha` flag.  The batch validator
must derive its allowed stride as

```
5 + has_depth + colour_alpha + 2 * texture_units
```

rather than inferring texture-unit count from the numeric stride.  The current
numeric rule mistakes an alpha+depth+one-texture nine-dword packet for a
two-texture packet.  Leave alpha disabled for the serial multi-texture path
until its packer, staging allocation, descriptors, and validation are widened
together and validated by a small hardware triangle before Quake is retried.

## 4. Resource-copy caveat

Q3 textures are allocated with both `PIPE_BIND_RENDER_TARGET` and
`PIPE_BIND_SAMPLER_VIEW` (`bind=0xa`).  Transfer unmap writes their native
image with the code-2 sampler swizzle, whereas the current
`ph_copy_texture_box()` selects the render-target swizzle for this combined
bind mask.  A `resource_copy_region` or blit into such a resource can therefore
produce a texture whose descriptor says code 2 but whose bytes use the RT
layout.  Track the storage representation at the operation, rather than
inferring it from the broad bind mask, before changing this path.
