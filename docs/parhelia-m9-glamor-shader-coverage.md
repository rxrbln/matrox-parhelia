<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# glamor's GLSL programs on the M9148 native fragment backend

This note lists every GLSL program glamor builds on the clean-room m9x Mesa
driver, the vendor M9 native program captured for each, and how far the m9x
fragment backend (`src/gallium/drivers/m9x/m9_fs_compile.c`) reproduces it.

Labels:
* **CAPTURED**: read from `captures/m9x-vendor-glamor-shaders-20260915`.
* **MEASURED**: vendor readback, or the host llvmpipe reference.
* **STATIC**: read from the glamor sources in `xorg-server-parhelia/glamor`.
* **DERIVED**: fitted to captures; the evidence is named.
* **OFFLINE**: checked by `m9-fs-compile-test` or `tools/m9-native-eval.py`.

Nothing here ran on the clean-room driver.

## 1. What glamor builds on m9x (STATIC)

* **Version.**  The m9x screen reports `glsl_feature_level` 120, so glamor
  sets `glsl_version = 120`.
* **Integers.**  `glamor_glsl_has_ints()` is false unless GL_EXT_gpu_shader4
  is exposed together with GL_ARB_instanced_arrays.
* **Init requirements** (`glamor_init`): GL 2.1 or later,
  GL_ARB_texture_border_clamp (compatibility context) and
  GL_ARB_vertex_array_object.  Otherwise glamor fails to initialise.
* **Dual-source blending** needs GL_ARB_blend_func_extended plus integers or
  GL_ARB_ES2_compatibility.  Without it, component-alpha composites use the
  two-pass `ca_first`/`ca_second` programs listed below.
* **a8 pictures** are GL_ALPHA textures unless both GL_ARB_texture_rg and
  GL_ARB_texture_swizzle are exposed.  Only then do the
  `dest_swizzle_alpha_to_red` composite variants exist; they are not listed.

Programs that the 1.20 path never builds, with glamor's behaviour:

| path | reason | glamor |
|---|---|---|
| core text (`glamor_text.c`, `glamor_font.c`) | needs ints | `glamor_font_init` skips; PolyText/ImageText use the glyph-blt program |
| dashed lines (`glamor_dash.c`) | facet version 130, build fails | software fallback |
| CopyPlane | `can_copyplane` needs GL 3.0 | software fallback |
| instanced rects, spans, glyphs (`*_130`) | ints | 1.20 facets below |

`tools/glamor-shader-dump.py` generates the programs from the preprocessed
sources: facets, location variables, combine strings, the
`glamor_build_program` templates, and the composite and gradient builders.
The result is 99 programs with 81 distinct fragment shaders.

## 2. Coverage per glamor operation (OFFLINE)

Statuses:
* **EXACT**: program and constants byte-identical to the vendor.
* **EQUIV**: a different layout, proven equal by evaluation (section 4).
* **no**: the backend refuses the program, and the draw is skipped.

| operation | program(s) | status |
|---|---|---|
| PolyFillRect, FillSpans, PolySegment, PolyLines, PolyPoint, glyph blt: solid | `fill-*-solid` | EXACT (21 programs, 4 fill styles) |
| same, tiled | `fill-*-tile` | EXACT |
| same, stippled | `fill-*-stipple` | EXACT (compare + KIL template) |
| same, opaque stippled | `fill-*-opaque-stipple` | EXACT (select template) |
| CopyArea, CopyWindow | `copy-area` | EXACT |
| CompositeGlyphs, solid source | `glyphs-solid-{normal,ca-first,ca-second}` | EXACT |
| CompositeGlyphs, picture source | `glyphs-picture-*` | EXACT |
| CompositeGlyphs, 1x1 repeat source | `glyphs-1x1-*` | no: constant coordinate |
| Composite, CompositeRects, trapezoid/triangle masks (repeat mode below RepeatFix) | `composite-*` (30) and `composite-stub-*` (30) | EQUIV |
| Composite with RepeatFix (large pixmap, or transformed RepeatNone without alpha) | `composite-*` | no: control flow remains |
| Xv RGB32/RGB565 | `xv-rgb-raw` | EXACT |
| Xv YV12/I420/NV12/UYVY | `xv-planar-3`, `xv-planar-2`, `xv-uyvy` | no |
| gradient sources (linear, radial) | `gradient-*` | no |

Totals from `m9-fs-compile-test`:
* 89 of 99 programs are accepted.
* 29 are byte-exact, and all 29 are in the required set.
* `m9-native-eval.py --compare` finds the other 60 equivalent to the vendor
  program: 79 EQUIV.  The 10 stipple programs are exact but cannot be
  evaluated (compare classes).

**Composite and `rel_sampler`.**  glamor always links the `rel_sampler`
shaders: a link failure never comes from m9x, because Gallium cannot fail a
link after `finalize_nir`.
* Their int uniforms `source_repeat_mode` and `mask_repeat_mode` select
  `if` branches.
* `m9_fs_find_inlinable_uniforms` records them.
  `nir_find_inlinable_uniforms` misses them, because m9x lowers uniforms to
  `load_ubo_vec4`.
* `m9_fs_inline_uniforms` folds them.  With a repeat mode below 10 the NIR
  equals the stub shader.
* The test therefore compares each `composite-*` case with its
  `composite-stub-*` capture.

**Fallback.**  glamor falls back to software only when building a program
fails.  A program that links on m9x but is refused by the backend is not a
fallback: its draws are skipped with one warning.  This affects the 1x1
glyph source, the Xv planar formats, gradients, and composites with
RepeatFix.  Two conditions still make the link itself fail:
* Mesa link limits, e.g. `xv-planar-3` fails to link when fewer than 3
  fragment samplers are advertised.
* A gradient build failure, which clears `enable_gradient_shader`.

## 3. Vendor captures (CAPTURED, MEASURED)

* **Harness.**  `tools/m9x-glamor-shader-corpus.{c,sh}`, one process per
  case in `/tmp/m9cap2.lock`.
  * 128x128 window, programs from the GLSL 1.10 conversion.
  * Every active uniform set by name from traceable values.
  * Three traceable 4x4 RGBA textures; unit 0 alpha is 0 on a checkerboard,
    so the stipple compare takes both branches.
  * `c2` read through host-aperture slot 0.
* **Conversion.**  The glamor text with its compat macros (`in`, `out`,
  `frag_color`, `texture`, `#if __VERSION__`, `#ifdef GL_ES`) expanded, and
  `#version 110`.  Nothing else in the 1.20 text needs 1.20.
* **Attribute locations.**  Binding a user attribute to location 0 makes the
  vendor `glLinkProgram` fault in libGL.so (MEASURED: 5 of 5 cases, the same
  shader links with base 1).  The harness binds glamor's attributes to
  locations 1..3 and emits the vertex with `glVertex`.
* **Results.**  83 programs were captured, all linked and drawn.  The vendor
  readbacks equal the host llvmpipe run of the same harness to within 1 over
  all pixels of every case (`reference-llvmpipe.json`).
* **Captured vendor forms worth knowing:**
  * A texture coordinate that reaches `texture2D` through a function
    parameter (`rel_sampler`) makes the vendor emit a coordinate pass: raster
    draw 1 interpolates into 11/13, raster draw 2 samples.  48 composite
    captures have this form; glamor's direct forms (`fill_pos`, `glyph_pos`)
    stay single-pass.
  * `vec2(0.5)` as a coordinate (1x1 glyph source) uses the same two-pass
    form with `11 <- (0.5, 0.5)`.
  * The vendor packs two vec2 varyings into one slot (xy, zw), as Mesa does.
  * The vendor does not fold `x * 1.0` or `vec4(t.rgb, 1.0).a`; NIR does.
    This is why 12 composite programs are EQUIV rather than EXACT.

## 4. Instruction semantics and equivalence (DERIVED, MEASURED)

`tools/m9-native-eval.py` executes native fragment programs with these
semantics:

* **Registers and writes.**  A register holds two scalars.  `op0=1c` writes
  both; `1d`/`1e` write the first/second scalar of a temporary, or the R,G /
  B,A lanes of output 2d.
* **Operand swizzle.**  2 bits `(entry0, entry1) = (bit1, bit0)`, where 0
  selects the first scalar and 1 the second.
* **MOV** `op2=14`: source register c or constant e, swizzle W1[5:4].
* **MUL** `op2=01`: register a or constant e (swizzle W1[9:8]) times
  register b (swizzle W1[7:6]).
* **ADD** `op2=04`: register a or constant e, plus register c, or constant
  c when W1[28] is set.  W1[7:6] negate the second operand.
* **Interpolation** `op2=10`: `a = 40 + 8k + 2*lane`.
* **Textures.**  Texel pairs 00/02 and 04/06; a coordinate pass writes 11
  (first texture) and 13 (second).

Validation against the vendor readback at 5 points
(`m9-native-eval.py --validate`):
* 48 vendor programs match.
* 35 use compare/select classes and are not evaluated.
* 1 mismatch: `glyphs-1x1-normal`.  The binding of pass-1 coordinates to
  the second texture is not decoded.

`--compare` evaluates the backend program (exported by the test with
`M9_FS_JSON`) and the vendor program on the same inputs.  It only accepts a
vendor program that reproduces its own readback.

## 5. Backend rules added for glamor

All forms are CAPTURED, and all rules are DERIVED from the named captures.

* **Swizzled operands.**  A swizzle that keeps each result lane inside one
  source lane becomes a lane plus a 2-bit operand swizzle: W1[9:8] for the
  register/constant operand, W1[7:6] for register b.  Examples: `mask.wwww`,
  `fg.wwww` (`glyphs-*`).
* **Texel pairs.**  With two textures, the texture read through a replicated
  scalar takes 00/02 (`glyphs-picture-normal` vs `-ca-first`).  Otherwise
  the last sampled texture takes 00/02.
* **Lane order.**  A product with a replicated register operand starts with
  the R,G lane (`glyphs-solid-normal`, `glyphs-picture-normal`).
* **Composed alpha.**  `vec4(texture.xyz, w)` writes the literal into the
  second scalar of the texel zw register: `ffff821e a280330f`,
  `composite-stub-*`.
* **Constant times constant.**  One factor is moved into a register:
  `ffff821c a280731f`.
* **Packed coordinates.**  A texture coordinate may be the zw half of a
  packed vec2 slot.  `m9_fs_program.texcoord_components[]` records it; the
  raster streams must copy those components.
* **Stipple template** (12 slots).  `if (texture.w == lit) discard;
  gl_FragColor = uniform;`:
  * constants `u.zw, (0,1), (1,0.5), u.xy, (lit,lit), (2.00000024,1)`;
  * `88 = 0010fbc3`.
* **Opaque stipple template** (13 slots).  The same compare selecting
  between two uniforms:
  * constants `false.zw, false.xy, true.zw, true.xy, (lit,lit),
    (2.00000024,1), (0,1), (1,0.5)`;
  * `88 = 0010ffe3`.
* **Inlinable uniforms.**  Uniforms read by `if` conditions are recorded as
  constant buffer dwords, at most 4, and folded (section 2).

## 6. Open items

1. **Driver wiring (not done here).**
   * The screen's `finalize_nir` must call `m9_fs_find_inlinable_uniforms`.
   * The context must implement `set_inlinable_constants`.  st/mesa then
     sends the repeat modes.
   * The fragment state must keep one native program per value set, built
     by `m9_fs_inline_uniforms` on a clone plus `m9_fs_compile`.
   * Until then, all `rel_sampler` composites are refused in the driver.
2. **Raster `88` without varyings.**  glamor's solid programs capture
   `0000fff2`; `fs-uniform`, with the same raster declaration, has
   `0010fff0`.  The solid composites capture `0000fff0` or `0000fff4`.
   Bits 20 and 3:0 are not decoded, and the backend keeps `0010fff0`.
3. **Constant texture coordinates** (`glyphs-1x1-*`) need a coordinate
   raster pass in the driver.
4. **Xv planar/UYVY** need vec3 arithmetic, `clamp` as `min`/`max` (the
   captured form is `op2=19` cls 2/3), three samplers, and for UYVY `mod`
   plus `float(int)` in a 38-slot second pass.
5. **Gradients** need loops over uniform arrays, `sqrt`, and division.

## 7. Hardware test plan (clean-room driver, safest first)

Each step is one lock section and one job, followed by a readback through
`SURFACE_ACCESS` and a requester retirement check.  Compare each result with
the vendor readback in the capture of the same name.

1. `fill-rect-solid`: uniform moves only.  Solid fill on a 64x64 pixmap.
2. `copy-area`: texel moves.  CopyArea of a pixmap whose texels encode
   their position.
3. `glyphs-solid-normal`: `fg * atlas.a` (swizzle W1[7:6]=11).  One glyph
   string with an 8-bit atlas.
4. `glyphs-picture-normal`: two textures and a packed coordinate slot.  Check
   that texel pair 00/02 is bound to the atlas.
5. `fill-rect-stipple`: KIL.  Pixels under zero stipple bits must keep the
   destination.
6. `fill-rect-opaque-stipple`: select, with the hi=4 conditional moves.
7. After the specialization wiring: `composite-texture-texturealpha-normal`
   with repeat mode 0 (in-place composed alpha, `.wwww` mask).  Then
   `composite-solid-solid-cafirst` (constant moved into a register).
8. X with glamor: xterm text, window moves, `xrender` composites.  Check the
   scanout with `--scanout-dump`, not X logs.

## Reproduction

```sh
python3 tools/glamor-shader-dump.py captures/m9x-vendor-glamor-shaders-20260915/shaders
sh tools/m9x-glamor-shader-corpus.sh captures/m9x-vendor-glamor-shaders-20260915 [CASE...]
python3 src/gallium/drivers/m9x/m9_fs_corpus_gen.py ~/src/parhelia/xf86-video-parhelia \
    > src/gallium/drivers/m9x/m9_fs_corpus.h                         # Mesa tree
M9_FS_JSON=backend.json objs-m9x/src/gallium/drivers/m9x/m9-fs-compile-test
python3 tools/m9-native-eval.py --validate captures/m9x-vendor-glamor-shaders-20260915
python3 tools/m9-native-eval.py --compare backend.json captures/m9x-vendor-glamor-shaders-20260915
```
