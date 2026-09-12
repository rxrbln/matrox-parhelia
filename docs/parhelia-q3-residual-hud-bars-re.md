# Q3 residual HUD glyph bars audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This is an offline audit of the four near-white regions left in the
1024x768 P690 Q3DM1 capture after the separately gated CPU sampled-alpha
cutout and alpha-over paths were enabled.  It changes no target state,
driver source, kernel source, or vendor binary.

## Result

The residual regions are opaque **HUD glyph quads**, not a scanout/present
fault, alpha-tested world geometry, an untextured white primitive, or a
blend-factor encoding error.

The current CPU alpha-over selector did not select these draws.  The
available evidence does not identify which selector predicate differs, so it
does not justify adding another image hash or broadening the CPU compositor.
The existing blend-state trace is sufficient to obtain that missing fact
without changing any code or probing an unproven hardware state.

## Screenshot evidence

The settled capture with both fallbacks enabled,
`/tmp/parhelia-q3-cpu-alpha-all-settled.png`, has four near-white connected
regions:

| bounds (inclusive) | size | software-capture correspondence |
|---|---:|---|
| `(763,146)-(826,161)` | 64 x 16 | the four glyph cells of `2fps` |
| `(756,552)-(787,567)` | 32 x 16 | the two glyph cells of `10` |
| `(812,552)-(827,567)` | 16 x 16 | the one glyph cell of `0` |
| `(500,372)-(523,383)` | 24 x 12 | a separate small blended glyph/detail; it cannot be classified further from this capture |

`/tmp/q3-soft.png` shows `2fps` at the first location and `10` plus `0` at
the second and third locations, with only the intended covered pixels
visible.  The P690 capture instead paints every pixel in each cell's
rectangle.  Their widths are exactly four, two, and one times the 16-pixel
glyph width, which is incompatible with a presentation tear.  The residual
component list is identical after the cutout path is additionally enabled,
so these are not alpha-tested cutout stages.

The earlier target-matched capture contains the same four regions plus five
larger opaque rectangles.  The alpha-over fallback removed those other
rectangles; it did not select this residual draw family.  The captures are
not pixel-identical frame pairs, so this conclusion uses the stable component
bounds and visible glyph content, not a whole-frame image difference.

## Q3 source evidence

`code/cgame/cg_draw.c:732-765` forms the upper-right counter as
`"%ifps"`, calls `CG_DrawBigString`, and sets its colour to opaque white.
`CG_DrawStringExt` in `code/cgame/cg_drawtools.c:164-218` emits one
`CG_DrawChar` call per non-space character.  `CG_DrawChar` at
`cg_drawtools.c:122-153` selects a 1/16 by 1/16 texture rectangle from
`cgs.media.charsetShader`; `cg_main.c:1862` registers that shader as
`gfx/2d/bigchars`.  At the observed resolution, this produces precisely the
four 16-pixel-wide textured quads in the first row.

The lower two rows have the same glyph-cell geometry and match the visible
numeric HUD text in the software capture.  The base cgame's ordinary status
numbers are separately emitted as textured quads by `CG_DrawField`
(`cg_draw.c:206-254`) through `numberShaders[]`, registered from
`gfx/2d/numbers/*_32b` in `cg_main.c:807-833`.  The saved screenshots alone
cannot tie the lower-right coordinates to one particular status call or to a
user HUD extension, but they do establish textured text rather than
untextured geometry.

Thus the relevant input is sampled texture alpha used by a blended text
stage.  It is the same class of P690 defect demonstrated by the A8R8G8B8
test: RGB is sampled while the sampled alpha does not become fragment source
alpha.  The proven vertex-alpha stream and `SRC_ALPHA` blender do not repair
that missing input.

## Why the existing fallbacks do not prove a selector bug

`ph_use_cpu_alpha_cutout()` cannot apply: it requires active Z write,
`GEQUAL(.5)`, and blending disabled.  Q3 HUD text is a blended 2D stage.

`ph_use_cpu_alpha_blend()` is intentionally narrow.  In addition to P690,
one texture, tiled RGBA storage, `MODULATE`, no alpha/stencil test, and the
exact alpha-over factor pair, it accepts only one 256x256 image hash or nine
32x32 hashes.  It also requires particular filtering, wrapping, write-mask,
and raster state.  A residual glyph draw may have a different texture
identity, dimensions, source-alpha blend shape, texenv, sampler state, or
another rejected condition.  The screenshots cannot distinguish those cases.

It would be unsafe to infer that the `bigchars` atlas is the accepted
256x256 hash, or that a lower status digit is one of the accepted 32x32
hashes.  Image conversion, picmip, registration path, and a Q3 configuration
can all change the resource observed by Gallium.  No broad fallback or hash
addition follows from the visual match alone.

## Safest next diagnostic

Run one already-instrumented Q3 frame with the existing fallbacks retained
and add only:

```
PARHELIA_TRACE_BLEND_ALPHA=1
```

`ph_trace_blend_alpha()` is invoked only after native batch-state construction.
CPU-composited draws return before that point, so the log describes the
still-native blended draws, including the unresolved cells.  Each bounded
unique line records texture width/height/format, tiled layout, FNV image hash,
zero/nonzero alpha counts, depth state, vertex-alpha stream, texenv, sampler
mask, RGB and alpha blend factors, filtering/wrap, draw bounding box, and
triangle count.  Correlate a line whose bounding box intersects the first
three regions above with a same-run screenshot.

That single trace separates the actionable cases:

* an otherwise exact alpha-over state with a new immutable image hash can be
  considered for a separately tested, fingerprinted CPU-compositor extension;
* a `SRC_ALPHA, ONE` additive state belongs to the separately proven additive
  gate and needs its own exact CPU blend arithmetic;
* a different format, texenv, sampler, depth/stencil, or multi-texture state
  requires a new proof before any fallback is selected.

Do not change sampler registers, select the P690 paired pipe, alter texture
format, or add an image hash before this trace identifies the actual rejected
state.  The archive contains no native sampled-texture-alpha route to test in
their place.
