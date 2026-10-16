<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 depth textures and shadow comparison in the m9x Mesa driver

What the M9 does when a depth surface it rendered is bound as a texture, and
what one fragment fetch costs, measured on the M9148 with the m9x Gallium
driver on September 17, 2026.  It carries out the two features
`parhelia-m9-openbits-gl-capture.md` closed by capture, and corrects two
things that capture could only infer.

`MEASURED` = rendered on this hardware and checked against a CPU reference.
`CAPTURED` = read out of a vendor command stream.  `DERIVED` = reasoned from
either.  `UNTESTED` = not exercised.

Fixtures: `tools/m9-mesa-glsl.c` cases `shadow:FUNC`, `depthtex`,
`depthtex16`, `depthval`, `depthprobe`, `depthchan`.  Every case builds an
8x8 grid of permuted depths — cell `k = 8i + j` is rendered at
`((29k + 7) mod 64 + 0.5)/64` and sampled with a reference from a second
permutation `((37k + 11) mod 64 + 0.5)/64` that meets the first in no cell
and stays two steps away from it everywhere — so an inverted store, an
inverted reference and a mirrored comparison each produce a different image.
All of them pass on llvmpipe, which stores the plain window depth.

## 1. One texture unit is one fetch (MEASURED, CAPTURED)

A unit is a fetch, not a texture: the engine samples at the coordinate it
interpolates and the fragment program has no fetch instruction (CAPTURED,
`parhelia-m9-native-glsl-corpus.md` section 6).  The backend allocated a unit
per `nir_tex_instr`, which cost a second unit for a repeated fetch and
sampled the same place twice.  Fetches that agree in sampler, coordinate
stream, component, projector and comparison now share one unit, and four
fetches per pass is the limit the hardware has:

* two `texture2D(s, tc)` of one sampler at one coordinate take **one** unit;
* four taps of one sampler at four uniform offsets take **four** units and
  compile (the `bump-height` shape);
* five taps are refused with *more texture fetches than one raster pass can
  bind*, which is the true reason.

A negated offset parses now as well.  `texture2D(s, tc - vec2(o, 0.0))`
reaches the coordinate parser as `fadd(varying, fneg(uniform))`, which it
refused, so a symmetric kernel was refused for the wrong reason.

**What is not implemented.**  More than four fetches in one pass is not a
driver limit to be lifted but a framebuffer pass: the vendor renders to a
scratch surface and binds it back through index `78`, one pass per three
further application textures (CAPTURED, `parhelia-m9-openbits-gl-capture.md`
section 2).  glmark2 `effect2d` (9 and 25 taps of one sampler) and `terrain`
(six samplers) need that scheduling; both are still refused.  The earlier
reading in `parhelia-m9-affine-texture-coordinates.md`, that every tap costs
a unit, is confirmed — what it got wrong is that this puts the scene out of
reach: it puts it out of reach of a *single pass*.

## 2. A rendered depth surface samples, and nothing needs complementing (MEASURED)

`GL_DEPTH_COMPONENT24` attached to a framebuffer, rendered, then bound as a
texture, samples back **the plain window depth**:

```text
depthtex   cell 0,1 wrote 0.5703 sampled 0.5686: PLAIN
depthtex   cell 2,3 wrote 0.7266 sampled 0.7255: PLAIN
depthtex   cell 4,5 wrote 0.8828 sampled 0.8824: PLAIN
depthtex   cell 6,7 wrote 0.0391 sampled 0.0392: PLAIN
depthtex   0 of 76800 pixels differ: PASS
```

and all eight comparison functions reproduce the reference image exactly,
with the reference and the function passed through unchanged:

```text
shadow:200 (NEVER) .. shadow:207 (ALWAYS)   0 of 76800 pixels differ: PASS
```

**This contradicts the vendor ICD's own readback**, which sampled a rendered
depth texture as `1 - z` (MEASURED there, `parhelia-m9-openbits-gl-capture.md`
section 1).  The compensation that finding calls for was implemented first —
complement the reference, mirror the function, since `mirror(f)(1-r, 1-d) ==
f(r, d)` and either alone inverts the comparison — and it made the four
ordering functions wrong in **exactly half** the cells (38400 of 76800
pixels, 32 of 64 cells), while the four functions that do not order
(`NEVER`, `EQUAL`, `NOTEQUAL`, `ALWAYS`) stayed right.  Removing it made all
eight exact.  So on this pipeline the complement does not exist, and the
vendor's is somewhere in its own ICD — its viewport z mapping most likely,
since the depth unit is the same part.  Both compensations were run back to
back in one session against the same fixture, so this is a measurement, not
a preference.

The m9x driver feeds the captured vertex program `1 - 2z` in the position
record, exactly as the vendor's does, and the depth test works in GL
semantics end to end: `depthprobe` reads the rendered surface back through
the depth test itself — quads at rising depths paint where they pass
`GL_LESS`, leaving the stored depth quantized to 1/8 — and agrees with
llvmpipe cell for cell.

## 3. A plain fetch returns the depth in the second texel scalar (MEASURED)

This is what made a rendered depth texture read as zero at first.  Sampling
one without comparison and reading `.r` gave `0.0000` for every texel of a
surface whose content the depth test proves is there.  `depthchan` leaves the
view unswizzled and prints the four raw channels:

```text
depthchan cell 0,1 stored 0.5703 rgba 00 91 ff ff   (plain 91, complement 6e)
depthchan cell 2,3 stored 0.7266 rgba 00 b9 ff ff   (plain b9, complement 46)
depthchan cell 4,5 stored 0.8828 rgba 00 e1 00 00   (plain e1, complement 1e)
depthchan cell 6,7 stored 0.0391 rgba 00 0a 00 00   (plain 0a, complement f5)
```

* **Green carries the window depth**, to within the 8-bit readback — the
  second scalar of the first texel register pair.
* Red is zero, and blue and alpha are not consistent between cells.
* A **comparing** fetch returns its result in the *first* scalar instead:
  `shadow2D(...).r` is exact with no channel move at all.

The driver therefore splats the second scalar over the result of a
non-comparing fetch of a depth sampler, and leaves a comparing one alone.
That lowering has to run **after** the view swizzle: `nir_lower_tex` rewrites
every use of the fetch, this one included, so a channel pick before it reads
the swizzled value rather than the texel.  Every `GL_DEPTH_TEXTURE_MODE` then
selects from a result whose four components all hold the depth, which is what
`LUMINANCE` and `INTENSITY` want; `GL_ALPHA` is a software concern, as the
vendor capture found.

UNTESTED what red, blue and alpha hold, and UNTESTED whether a depth texture
uploaded from the CPU behaves the same — the driver cannot upload one, see
below.

## 4. Descriptor and sampler words (CAPTURED, confirmed MEASURED)

A depth image carries the same descriptor code in the render and the sampler
role, and the driver binds the very same word in both, as the vendor does
(`cap-shadowmap_d24_lequal`: `b9=fa010040` as a target, `79=fa010040` as a
texture):

| format | `m9_format_info` code | descriptor `79[31:24]` |
|---|---|---|
| `GL_DEPTH_COMPONENT16` | `0x5c` | `0xb8` |
| `GL_DEPTH_COMPONENT24` | `0x7d` | `0xfa` |

The kind follows the size like any render target — the 2-D kind at 64x64 and
below, the tiled render kind above it — and the comparison words are the ones
`m9_texture_unit_init` already carried from the static recovery, now
confirmed live: index `0x70 + unit` bit 26 enables the comparison, bits
`29:27` are the function in the order `NEVER LESS LEQUAL EQUAL NOTEQUAL
GEQUAL GREATER ALWAYS`, and bits `11:8` carry `2`, the three-component
lookup, instead of `1`.  `m9-batch-test` checks those words against the
capture, which is why the driver's own policy stays out of that file.

The comparison reference travels as the third component of the coordinate
stream, alongside `s` and `t` in the float4 CPU record the driver already
synthesises per vertex, and a shadow fetch therefore owns the low half of its
varying slot.  Our declaration word stays `0x00500688`; the vendor emits
`0x00500c88` where its own vertex shader declares a `vec3` varying, and the
difference does not matter, because the comparison is exact with `0688`.

## 5. What a depth texture still cannot do

* **`GL_DEPTH_COMPONENT16` as a render target.**  The raster parameters name
  a D24S8 depth descriptor for the drawable, and only a D24S8 image gets the
  hierarchical buffer `m9_context_depth` requires, so a D16 attachment draws
  with no depth at all (with a warning) and the texture stays zero.
  `depthtex16` is dispatched but not listed for that reason.
* **Uploading one from the CPU.**  A depth image is a render surface in the
  tiled layout and the transfer path maps it linearly, so `glTexImage2D` of
  `GL_DEPTH_COMPONENT` data does not land where the sampler reads.  The
  vendor probe's uploaded-texture cases are therefore not reproduced here.
* **Reading one back with `glReadPixels`.**  The first map after a render
  returns the right value and every later one returns the first value again
  (`depthval`), which predates this work and is not understood.  It is why
  the depth convention was settled through the sampler and the depth test
  rather than through a CPU readback.
* **glmark2 `shadow`.**  Its fragment shader is not a `sampler2DShadow` at
  all: it divides a `vec4` varying by its own `w` per fragment, samples an
  ordinary `sampler2D` depth texture, and compares in the shader.  The depth
  texture half of that is what this note closes; the per-fragment divide by a
  varying is still refused by the coordinate parser, which folds only
  per-draw coefficients.  That refusal is now known to be the right answer
  and not merely a gap: the sampler performs no divide of its own, so the
  scene needs the vendor's two-pass computed-coordinate form
  (`parhelia-m9-mesa-projective-textures.md`, MEASURED).

## Reproduction

```sh
scp tools/m9-mesa-glsl.c root@TARGET:/root/parhelia-m9x-mesa/src/
ssh root@TARGET 'cd /root/parhelia-m9x-mesa/src &&
    gcc -O2 -o m9-mesa-glsl m9-mesa-glsl.c -lEGL -lGL -lm'
# inside flock -w 1800 /root/tmp/m9-hw.lock, with the module loaded
#   insmod /root/tmp/m9x_drm.ko m9_enable=1 m9_render_diag=1 m9_user_submit=1
# (m9_kms=1 is refused on a box whose display another run has touched, and
#  the surfaceless fixture only needs the render node)
./m9-mesa-glsl depthtex ; ./m9-mesa-glsl shadow:203 ; ./m9-mesa-glsl depthchan
```

The whole fixture list is 90 PASS, 0 FAIL (MEASURED 2026-09-17): the 80 cases
that passed before this work, plus `depthtex`, the eight `shadow:FUNC` cases
and `pow0`.
