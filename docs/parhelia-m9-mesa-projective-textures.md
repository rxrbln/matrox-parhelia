<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 projective texture coordinates: the sampler does not divide

What the M9148 raster engine does with the fourth component of a texture
coordinate stream, established on September 17, 2026 while looking for a way
to compile glmark2's `shadow` scene in the clean-room `m9x` fragment backend.

Labels: **MEASURED** is an M9 (`102b:0540`) result read back from the
framebuffer and compared against a CPU reference for every one of 76800
pixels, through the `tools/m9-mesa-glsl.c` fixtures; **CAPTURED** is read from
the vendor programs in `captures/m9x-vendor-glsl-programs-20260915`;
**DERIVED** is inferred from those.

## Result

**The sampler does not divide the interpolated coordinate by any further
component of its stream (MEASURED).**  There is therefore no way to express a
per-fragment projective fetch by shaping the coordinate stream, and glmark2's
`shadow` stays refused.  The vendor's own answer to a projector it cannot fold
is a second raster pass that computes the coordinate (CAPTURED); that is the
route to reopen, and its job structure lives in `m9_draw.c`/`m9_context.c`,
not in the fragment backend.

## 1. The probe (MEASURED)

`m9-mesa-glsl projq` renders a quad whose coordinate varying is four
components wide:

```glsl
/* vertex */   float q = 1.0 + pos.x * (3.0 / 320.0);
               v_probe = vec4(tc, tc.y * 0.5, q);
/* fragment */ gl_FragColor = vec4(texture2D(tex, v_probe.xy).rg, v_probe.z,
                                   v_probe.w * 0.25);
```

The fetch is a plain `texture2D` of `v_probe.xy`, so nothing in the driver
divides: `m9_scale_coords` only divides where the backend set
`texcoord_projective`, which a non-projective fetch never does.  `v_probe.z`
and `v_probe.w` are read by the fragment shader, so the linker keeps the
varying four components wide instead of packing the live ones down, and `q`
reaches component 3 of the stream.  The stream is declared `0x0688` at
attribute `5 + k` with stride `0x10` — four components, sixteen bytes
(`m9_draw_params.c`, "texture k reads its coordinate stream as attribute
5+k, xyzw"), and `m9_render_record` copies all four floats of the draw
output into it.

`q` runs 1 to 4 across the quad.  Without a divide the fetch walks the whole
128x128 texture; with one it stays inside its first quarter.  Every texel
names itself (`texel(i,j) = (2i, 2j, (5i+3j)&0xff, 255-i-j)`), so the two
answers cannot be confused, and neither lands outside the image, so clamping
cannot hide the difference.

```
projq-plain    0 of 76800 pixels differ: PASS
projq-divided  75569 of 76800 pixels differ: FAIL
projq       verdict: sampler does NOT divide by the fourth component
```

The probe also rules out a divide by the third component: `v_probe.z` is
`tc.y * 0.5`, and dividing `s` by it would move the fetch far further still.

## 2. What the corpus says (CAPTURED)

Two vendor cases sample with a projector, and they answer differently:

* **`fs-tex2d-proj4`** — `texture2DProj(s0, gl_TexCoord[0])`.  Its decoded
  capture is byte-identical to `fs-tex2d`'s `texture2D(s0,
  gl_TexCoord[0].xy)`: same fragment program, same vertex programs, same
  `88 = 0010ffe3`, same `8c`, same declaration, same constants.  Nothing in
  it divides.  This looks like evidence that the hardware divides by `q` on
  its own — but the capture harness sets `glMultiTexCoord4f(GL_TEXTURE0, s,
  t, 0.5f, 1.0f)` (`tools/m9x-glsl-program-corpus.c`), so `q` is 1 for every
  vertex of every captured draw and the case carries no information either
  way.  It is not evidence of a hardware divide.

* **`fs-tex2d-proj3`** — `texture2DProj(s0, vec3(gl_TexCoord[0].xy * 0.5,
  0.5))`.  Here the vendor emits **two raster passes**.  The first
  (`c3 = 00`, `88 = 0110fffc`, `8c = 00500100`, no sampler bound) runs a
  five-slot program that interpolates the coordinate, multiplies it by the
  constant pair `(0.5, 0.5)` into `0x13` and leaves `(0.5, 0.5)` in `0x11`;
  the second (`c3 = 05`, `88 = 0010ffe3`, `8c = 00500200`, sampler bound) is
  the ordinary `fs-tex2d` texel move.  The vendor did not even constant-fold
  `0.5 / 0.5`.

  A compiler that could put an arbitrary `q` in the coordinate stream and let
  the sampler divide would have emitted one pass with `q = 0.5`.  It emitted
  two.  Together with the probe above, the two-pass form is the mechanism
  (DERIVED).

All six other captured two-pass cases share the pass-1 raster state exactly:
`88 = 0110fffc`, `8c = 00500100` (`00500200` for `fs-texrect`), sampler word
`7c = 512` for "none bound", a four or five slot program, and a constant span
that ends where pass 2's begins.  Pass 2 is always `88 = 0010ffe3`,
`8c = 00500200` with the sampler bound.  The two passes share one `c2`
program buffer and one `6c`.  The cases are the same shape — a fetch whose
coordinate the raster interpolator cannot produce directly:
`fs-shadow2d`, `fs-tex2d-bias`, `fs-tex2d-swizzle`, `fs-tex3d`,
`fs-texcube`, `fs-texrect`.

## 3. The defect this uncovered (MEASURED)

The backend accepts a projective fetch whose projector sits in component 3 of
the coordinate varying, and `m9_scale_coords` then divides **per vertex**,
which its own comment calls exact only "while q is constant over a
primitive".  Nothing refuses the case where it is not, and the result is
silently wrong.

A draw the compiled vertex program takes refuses a projective coordinate
outright ("the texture coordinate is divided by a projector"): its
coordinate stream is a two-scalar multiply-add with no divide in it
(`docs/parhelia-m9-native-vertex-backend.md` section 4a), so the defect
below belongs to the draw-module path alone.

`m9-mesa-glsl projtexz` measures it.  It is `projtex` with `v_proj.z` made
live so the linker keeps the varying four components wide and the projector
stays in component 3 — the only shape the backend accepts:

```glsl
v_proj = vec4(tc * q, tc.y * 0.25, q);          /* q = 1 .. 4 */
gl_FragColor = vec4(texture2DProj(tex, v_proj).rg, v_proj.z, 1.0);
```

```
projtexz     75373 of 76800 pixels differ: FAIL
```

The fetched texels are those of the plain affine coordinate `tc`, which is
exactly what `(tc * q) / q` per vertex leaves behind, so the per-vertex
divide is confirmed as the mechanism and not merely the suspect.

`m9-mesa-glsl projtex` — the same shader without a live `.z` — is refused
instead, but only by accident: the linker packs the three live components
down, the projector lands in component 2, and `build_tex`'s check that the
projector is component 3 rejects it.  Loosening that check would turn a
refusal into a wrong picture, so it must not be loosened before the divide
is right.

Both fixtures are dispatched but deliberately not listed in the suite, like
`projtex` before them: `projq` is a diagnostic that prints which answer the
hardware gives, and `projtexz` fails by design until the divide is fixed.

## 4. What glmark2 `shadow` needs

Its fragment shader divides a `vec4` varying by its own `w` per fragment,
samples an ordinary depth `sampler2D` and compares in the shader.  With no
sampler divide, that needs the vendor's two-pass form: one raster pass that
computes the coordinate, then the pass that fetches and compares.  How the
first pass hands the coordinate to the second is **not established** — the
captured first pass writes only registers `0x11` and `0x13` and never the
colour output `0x2d`, so it is not a render to an intermediate surface, but
nothing confirms that the next pass's texture unit reads those registers.
That is the first thing to settle.  It is the same job-splitting
machinery the multi-pass fragment scheduling work needs for `effect2d`,
`terrain` and the `desktop` blur, and it lives in `m9_draw.c` and
`m9_context.c`.  The fragment backend can only refuse until it exists, which
is what it does, with the accurate message "texture sampling needs a computed
coordinate pass".

A cheaper partial fix, worth having on its own, is to make the per-vertex
divide correct where it is used: the driver can compare `q` at the three
vertices of a primitive and skip the draw, or fall back, when they disagree,
instead of rendering a wrong picture.

## Reproduction

```sh
scp tools/m9-mesa-glsl.c root@TARGET:/root/parhelia-m9x-mesa/src/
ssh root@TARGET 'cd /root/parhelia-m9x-mesa/src &&
    gcc -O2 -o m9-mesa-glsl m9-mesa-glsl.c -lEGL -lGL -lm'
# inside flock -w 1800 /root/tmp/m9-hw.lock, with the module loaded
./m9-mesa-glsl projq ; ./m9-mesa-glsl projtexz
# the captured two-pass form:
python3 tools/m9-glsl-program-extract.py \
    captures/m9x-vendor-glsl-programs-20260915/fs-tex2d-proj3
```
