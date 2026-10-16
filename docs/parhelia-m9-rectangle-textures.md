# M9 rectangle textures

`GL_ARB_texture_rectangle` asks for a texture addressed in texels over
`[0, width] x [0, height]`, with exactly one level, non-power-of-two extents
as the normal case, and only the two clamp wrap modes.  This note records
what the hardware does about the unnormalized coordinate, and how the m9x
Gallium driver implements it.

## The engine has no unnormalized addressing mode

CAPTURED, `captures/m9x-linux-texture-resources-20260914`.  Phase 2 case 02
is the vendor ICD sampling a 13x7 rectangle with unnormalized coordinates;
case 01 is the same 13x7 image as an ordinary NPOT 2-D texture.  Decoding
both selected draws and diffing every indexed write:

| index | rectangle | 2-D plane | meaning |
|---|---:|---:|---|
| `79` | `c801000d` | `c801000d` | format `0x64`, kind `01`, width 13 |
| `7a` | `00010007` | `00010007` | depth 1, height 7 |
| `7c` | `12409200` | `00009200` | sampler filter and wrap |
| `70` | `00040118` | `000c0118` | LOD control enable, from the mip case |
| `7d` | `0ff80000` | `00000000` | LOD bounds, from the mip case |

The texture descriptor is **byte identical**.  Index `7c` differs only in
bits `30:22`, which are ADDRESSW/ADDRESSV/ADDRESSU: the rectangle sets all
three to wrap code 1, CLAMP.  Indices `70` and `7d` differ because case 01
selects a base level and case 02 does not; the phase-3 plane draws emit the
rectangle's own `70=00040118, 7d=0ff80000`.  No bit anywhere in the bound
state distinguishes a rectangle from a plane.

The scale is in the program instead.  The rectangle draw's system-resource
image carries one extra 16-byte constant vector the plane draw does not
have, at offset `0x70`:

```text
0070: 00000000 3f800000 3d9d89d9 3e124925    0  1  0.0769231  0.142857
```

`0x3d9d89d9` is `1/13` and `0x3e124925` is `1/7` as IEEE single precision:
the exact reciprocals of that rectangle's extents.  Every constant after it
is the plane draw's pool shifted by one vector, and the draw's program
pointer at index `1b` differs accordingly.  The vendor stack compiles the
reciprocal into the shader and multiplies the coordinate before the fetch.

DERIVED: the answer to "does the sampler take unnormalized coordinates
natively" is **no**.  The backlog's "unnormalized sampling passes" is a
result at the GL API, not a hardware capability.  Anything implementing this
extension has to fold `1/width` and `1/height` into the coordinate itself.

`tools/m9-texture-resource-evidence.py --self-test` asserts all of the above
(`verify_rectangle_scale`), so the finding cannot rot silently.

## Where this driver folds the reciprocal

The vendor folds it into the fragment program.  This driver has a cheaper
place: texture coordinates already reach the raster pass as CPU varying
streams, and `m9_scale_coords()` already multiplies them by
`m9_job_key::unit_scale`, the term that lets a render target of unaligned
width be sampled through its 32-aligned descriptor span.  A rectangle is the
same mechanism with a different factor:

```text
ordinary target   unit_scale = content extent / descriptor extent
rectangle target  unit_scale = 1 / descriptor extent
```

`m9_job_textures()` selects it on the bound view's target.  The factor is
per unit, not per draw, so a rectangle and an ordinary texture in one shader
each get their own; and because it is applied after the affine fold and
after the projective divide, shader arithmetic on the coordinate stays in
texel space exactly as the extension defines it.

The fragment backend accepts `GLSL_SAMPLER_DIM_RECT` wherever it accepts
`GLSL_SAMPLER_DIM_2D` (`sampler_dim_plane()`); the emitted program is
identical to the 2-D one, because the hardware makes no distinction.

`m9_scale_coords()` applies the scale per vertex to a CPU coordinate stream.
A coordinate an earlier raster pass produced reaches the fetch through the
inter-pass record instead, where there is no vertex to apply it to, so a unit
that needs a scale and is steered by a route is now refused by name rather
than sampled at the wrong texel.  The refusal is deliberately **not**
rectangle-specific: `unit_scale` is already not 1.0 for a render target whose
width the 32-pixel descriptor grain does not divide, and that combination
could reach the same hole before rectangles existed.  `coordroute:WxH` in
`tools/m9-mesa-glsl.c` is the probe for it - dispatched by name and
deliberately not listed, because it renders wrong before the refusal and is
skipped after it, and passes in neither state.

Looking for that case exposed a defect that predates rectangles, fixed in
its own commit.  `m9_scale_coords()` resolved every unit's stream with
`st->coord_extra[st->texture_coord[k]]`, and `texture_coord[k]` is
`M9_RASTER_COORD_NONE` (`0xff`) for exactly those routed units, so it
indexed an `int8_t[4]` with 255.  MEASURED by offset: `coord_extra` sits 12
bytes into a 32-byte `m9_fs_pass_streams` and `stream[]` holds eight of
them, so a routed unit in pass 0 read 267 bytes in, landing on
`slot_pass[7]` of the same variant.  That member holds pass indices, so the
value is non-negative, the `j < 0` guard did not fire, and the routed unit
ran its packing, affine-fold, projective-divide and scale bookkeeping
against whatever stream that byte named - claiming, in particular, that
stream's `scaled` bit, which suppresses the real scale for the unit that
owns it.  It stayed invisible only because the spurious multiply is by
`unit_scale`, and `unit_scale` is 1.0 for every texture whose content fills
its descriptor extent - which is almost all of them.  The mask is that
identity multiply, not the index.  It is already breakable without
rectangles: a render target whose width the 32-pixel grain does not divide
has a scale of its own today, and `coordroute:84x84` is that shape in one
case.  Reachable by `coordroute`, `coordchain` and the `conv*` family; the
suite never caught it because no listed case puts a routed unit and a scaled
unit in one job.

`caps.texrect` is set explicitly in `m9_screen.c`: the state tracker keeps
the coordinate unnormalized and hands it to the driver, which is what this
implementation wants.  `ARB/EXT/NV_texture_rectangle` themselves are not
driver-gated - `_mesa_init_extensions()` enables `NV_texture_rectangle`
unconditionally in this Mesa - so before this change the driver advertised
an extension whose every shader the fragment backend refused
("texture sampling needs a computed coordinate pass", OFFLINE:
`m9-fs-compile-test`, corpus case `fs-texrect`).

## Required refusals

The extension's errors are core Mesa behaviour, not driver behaviour, and
the fixture `texrecterr` asserts each one on the live stack:

| request | error |
|---|---|
| `glTexImage2D` level 1 | `GL_INVALID_VALUE` |
| `GL_TEXTURE_WRAP_S = GL_REPEAT` | `GL_INVALID_ENUM` |
| `GL_TEXTURE_MIN_FILTER = GL_NEAREST_MIPMAP_NEAREST` | `GL_INVALID_ENUM` |
| `GL_TEXTURE_BASE_LEVEL = 1` | `GL_INVALID_OPERATION` |
| `GL_TEXTURE_MAX_LEVEL = 1` | `GL_INVALID_VALUE` |

The first three are exactly what the vendor ICD produced (CAPTURED, case 02
`stage=rectangle-level-1 error=0x501`, `rectangle-repeat error=0x500`,
`rectangle-mipmap-filter error=0x500`).  Base level is `GL_INVALID_OPERATION`
rather than `GL_INVALID_VALUE` because Mesa implements the GL 4.5 correction
to that error on every version.

## Extent boundary

A rectangle is stored by whichever family `m9_resource_create()` picks, and
st asks for `SAMPLER_VIEW | RENDER_TARGET` on every colour texture, so a
rectangle lands in the colour-target family.  `m9_texture_bind_view()` then
gives it an exact descriptor extent while both dimensions are at most 64,
and the 32-aligned row span above that.  Sampling inside the content is
exact either way, because the coordinate is normalized over whatever extent
the descriptor carries.  Outside the content a large rectangle clamps at the
padded span rather than at the content edge, which is reported once per
context through `M9_WARN_WRAP`.  A rectangle of 64x64 or less - and the
vendor's own captured shape is 13x7 - clamps exactly.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
