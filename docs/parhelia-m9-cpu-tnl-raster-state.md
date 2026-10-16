# M9 raster state for a CPU T&L driver

This note gives the raster-pass encodings a CPU T&L Mesa driver needs on the
M9148 (`102b:0540`) when it submits window-space vertices through the verified
two-pass pass-through draw (`m9-user-triangle`).  Labels: **MEASURED** means
reproduced on the M9148 through `DRM_IOCTL_PARHELIA_M9_SUBMIT`, **CAPTURED**
means read from vendor ICD captures, **STATIC** means from the saved binaries.

Test tool: `tools/m9-user-raster-state.c` (build next to
`m9-user-triangle-data.h`, `gcc -O2 -Wall -I<uapi>`; run `./m9-user-raster-state
[test...]`).  It rebuilds the first-triangle stream, overrides raster words,
gives every vertex a unique exactly representable color, and reports per-color
pixel counts from the tiled BO plus bounding boxes from a linear readback
through host-aperture slot 0 (surface found by a per-dword marker page in
BAR0, then `d1=page, d2=cc210140, d3=000100f0`).

Vertices are 5 floats `(x, y, r, g, b)`, surface 320x240, depth test off
(`a8=0038038a`).  All values below were measured on September 15, 2026 against
module `m9x_drm.ko` with `m9_user_submit=1`.

## Coordinate conventions (MEASURED)

* Submitted window `y` is bottom-up: triangle `(20,20),(100,20),(60,100)`
  covers linear rows 141..219.  Linear row = `240 - y` (top-down de-tiled rows).
* Pixel coverage equals the ideal area: 80x80/2 triangle = 3200 pixels,
  80x80 quad = 6400, fan triangle area 10000 = 10000 pixels.
* Flat color is not bit-exact over the whole primitive: about 25 % of the
  pixels have one channel one LSB low (for example `5aba86` with `5aba85` and
  `5ab986` sub-regions).  Tests must match colors with a ±1 tolerance.  The
  vendor first triangle is byte-identical to the clean-room output, so this is
  hardware/program behaviour, not a replay error.

## Topology

Raster pass (MEASURED for every row; selector values CAPTURED in
`parhelia-m9-vertex-topology-capture.md`):

```c
raster_22 = (flat ? 0x6400 : 0x2400) | t;
raster_d7 = 0x200058a0 | t;
raster_f0 = lines ? 0xc8008400 : 0xc4008400;  /* width 1 */
```

| topology | `t` | measured result | provoking color |
|---|---:|---|---|
| line list | 1 | 4 lines, exact lengths 280/180/200/140 | second vertex |
| line strip | 2 | 7 connected segments | segment end vertex |
| triangle list | 3 | 2 triangles, 3200 pixels each | third vertex |
| triangle strip | 4 | 5 vertices -> 3 triangles | `i+2` |
| triangle fan | 5 | 5 vertices -> 3 triangles around vertex 0 | `i+2` |
| quads | 6 | 8 vertices -> 2 quads, 6400 pixels | fourth vertex |
| quad strip | 7 | 6 vertices -> 2 quads, 4800 pixels | `2i+3` |
| polygon | `d` | convex pentagon | first vertex |

Fans, quads, quad strips and polygons are native; no CPU conversion is needed.
The provoking vertex follows OpenGL `GL_LAST_VERTEX_CONVENTION` (polygon: first).

Lines need only the `f0` class byte `c8`: with `f0=c8008400` the line draw is
identical whether preprocessing index `01`/`6a` carry the vendor line values
(`01=80000000`, `6a=25012505`) or the triangle values (`01=f2fe014f`,
`6a=25010000`).  With the triangle `f0=c4008400` the same line stream
rasterizes as filled triangles.  Width 3 uses `f0=cc008400, f4=3` (CAPTURED
case 70) and measured 3-pixel-wide lines (horizontal line x[20,299] rows
218..220).  Vertical lines at `x=20` land on column 19.

`f0` bits 27:24 therefore select the raster class: `4` triangle, `8` line,
`c` wide line, `2` point (CAPTURED); bit 24 is set together with the blend
enable (`c5008200`).

Points (NOT VERIFIED): the captured point raster words `22=00006500`,
`f0=c2008400`, `f4=size`, `6a=250128ab` on the ordinary four-stream
consumer drew nothing (no fault, the following triangle still rendered).  The
vendor point path uses three preprocessing passes and five generated raster
inputs (`parhelia-m9-vertex-topology-capture.md`).  A CPU T&L driver should
expand points into two triangles or a quad until that path is replayed.

## Draw records and multiple draws per pass

Numeric record: `0x23000001, start, end` with exclusive `end` in vertex units
(`0x63000001` sets optional launch bit 30; not needed).  Index-buffer launches
`0x26000001`/`0x27000001` need `DRAW16_*`/`DRAW32_*` relocations (kernel
`m9_validate_draw_reloc_pairs`); not exercised here.

Preprocessing draw ranges must end on a four-vertex boundary (MEASURED): a
9-vertex triangle list with preprocess `[0,9)` drew the first two triangles
correctly and the third (vertices 6..8) as a `ffffffff` triangle at a wrong
position.  The vendor rounds identically (gltri `[0,4)`/raster `[0,3)`, gears
`[0,44)`/`[0,42)`).  Rule: pad the vertex buffer by repeating the last vertex so
each draw starts on a multiple of four and preprocess `[start, align4(end))`.

Multiple raster draws in one pass (MEASURED, `multi`): draw 0 uses the pass
setup words; before every following draw emit exactly what glxgears emits:

```text
45456a22  22=<topology/shade>  6a=<same 6a>
f4d7eff0  f0=<class>  ef=<cull/depth>  d7=0x200058a0|t  f4=<size/width>
23000001  start end
```

A triangle list `[0,3)`, strip `[4,8)` and fan `[8,13)` in one pass render with
correct provoking colors and positions.  The preprocessing pass can use either
one draw per raster draw (`[0,4),[4,8),[8,16)`) or one covering draw
`[0,16)`; both give identical output (MEASURED, `multi-single-pre`).

## Shading

Flat: raster `22` bit 14 set and bit 31 set in the two raster `23` link words
(`80100d88 801006b6`).  Smooth: `22` bit 14 clear and link words
`00100d88 001006b6` (MEASURED; CAPTURED pair case 37).  Smooth interpolation
was confirmed with red/green/blue corners (25118 distinct colors, blue apex).

## Face culling (MEASURED, CAPTURED pair cases 32..34)

Index `ef` bits 25:24, with winding measured in the submitted bottom-up window
coordinates (positive signed area = counter-clockwise):

| `ef` | result |
|---:|---|
| `00000000` | no culling |
| `01000000` | no culling (bit 25 is the enable) |
| `02000000` | counter-clockwise triangles culled |
| `03000000` | clockwise triangles culled |

GL mapping: cull back with `GL_CCW` front and cull front with `GL_CW` front ->
`03000000`; cull front with `GL_CCW` and cull back with `GL_CW` -> `02000000`.
The vendor also changes `6a` from `25010000` to `24cc0000` when culling is
enabled; culling results were identical with either `6a` value.  `ef` shares
the word with depth (`0000f000` depth write, `00007000` no write; glxgears
`ef=0300f000`), so pack both fields.  `GL_FRONT_AND_BACK` has no single code
here; skip the draw on the CPU.

## Scissor and clip (MEASURED, CAPTURED extended case 66)

`f5=c4 = left | top << 16`, `f6=c5 = right | bottom << 16`, exclusive, in
top-down surface rows (not the bottom-up vertex convention).  A full-surface
quad with `(17,23)..(118,102)` covered linear x[17,117] y[23,101], exactly
7979 pixels; `(200,100)..(320,240)` covered 16800 pixels.  With scissor off
emit the viewport/surface bounds (`0, H<<16|W`).  GL scissor
`(x,y,w,h)` on a height-`H` drawable becomes left `x`, top `H-(y+h)`, right
`x+w`, bottom `H-y`.

## Color write mask (MEASURED, CAPTURED pair cases 24/25)

Low 16 bits of `ab`; channel nibble R=`4`, G=`2`, B=`1`, A=`8`.  All four
nibbles must be set identically: nibble `n` (bits `4n+3..4n`) gates only
pixels with `x % 4 == n` (mask `0004` wrote red on columns 0,4,8..; `0040` on
columns 1,5,..).  Measured masks `4444` (R only), `2222`, `1111`, `8888` (alpha
only), `7777` (RGB), `0000` (nothing written).  The upper 16 bits in vendor
streams carry a process-varying fragment; the clean-room stream uses `ffff`
there and in `ac`.

## Blend (partially verified)

CAPTURED: enabling `GL_BLEND` changes only `88` (textured `0110dfe3` ->
`0110ff03`), `f0` (`c4008400` -> `c5008200`) and `8c` (`00500200` ->
`00500300`); constant color adds `8c` bit 10 (`00500700`), separate blend sets
`88` bit 7.  Factor and equation choices (`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`,
`ONE/ONE`, `DST_COLOR/ZERO`, subtract, min, max) produce identical direct
words and identical per-draw system resources in pairs 20..23 and extended
60..62, so they live in the generated program selected through `6c`/`8d`/`8e`
(STATIC: `parhelia-m9-blend-raster-lowering.md`, native fragment generation).

MEASURED: with the fixed first-triangle program, the enable words
(`f0=c5008200`, `8c=00500300`, `88` = `0100ff10`, `0100fff0` or `0100dff0`)
execute without error and a second quad replaces the destination exactly as
with blending off.  The vertex format has no alpha, so this does not separate
"blend inactive" from "SRC_ALPHA with alpha 1".  A driver must not advertise
blending on this path until a destination-sensitive fixture with the vendor
blend program resources is replayed.  glxgears does not blend.

## Clear

No GPU clear packet is available through the current userspace validator:
the vendor whole-allocation clear needs `e6=00000002`, `ac=clear_word` and the
`2a/2b` inline page-run record (STATIC,
`parhelia-m9-render-surfaces-clear.md`), and `m9_submit.c` rejects `e6` values
other than `3`/`0x42000000` and indices `2a/2b`.

MEASURED usable paths:

* CPU fill of the color BO mapping.  Tiled storage only permutes pixels inside
  the allocation, so writing one clear value to every dword of the BO clears
  the surface (every test here pre-fills the BO from the CPU and all counts and
  linear positions are consistent with it).  Partial-rectangle clears must go
  through the linear descriptor window instead.
* A full-surface quad draw with the clear color (the vendor rectangle-draw
  clear fallback); used as the first pass of the blend tests (57600 + 19200
  pixels, whole surface covered).
* Depth: CPU fill of the depth BO (`0xffffffff` in all tests).  A GPU depth
  clear by draw requires depth-write/always-compare state owned by the
  depth/z work.

## glxgears state summary

Raster setup: `22=00006406/00006407` (flat quads / flat quad strips),
`d7=200058a6/200058a7`, `f0=c4008400`, `ef=0300f000` (cull back CCW + depth
write), `a8=0038038b`, `6a=24cc0000`, `f5=c4=0`, `f6=c5=012c012c`, five raster
draws with the per-draw restate above and preprocess ranges rounded to four.
Every raster word in that list is covered by the measurements above except
the depth fields.

## Mixed flat and smooth draws in one pass (MEASURED)

`tools/m9-user-raster-state` tests `mixed-shade-*` and `mixed-link-*` submit
several triangle-list draws in one two-pass package, alternating flat and
smooth (two triangles in each of the first two draws, one in the others) and
check every interior pixel (at least 1 px from each edge) of every triangle
in the linear `SURFACE_ACCESS` readback: flat draws must equal the provoking
(last) vertex color within 1, smooth draws the barycentric interpolation
within 4.

| order | setup link bit 31 | per-draw 23 re-emit | result |
|---|---|---|---|
| FSFS | from draw 0 (flat) | no | all 6 triangles pass |
| FSFS | from draw 0 (flat) | yes | all 6 pass |
| SFSF | from draw 0 (smooth) | yes | flat draw 1 shaded (fail), flat draw 3 passed |
| SFSF, FSFS, SFFSF | forced set (flat form) | no | all pass (6, 6, 7 triangles) |
| FSFS | forced clear (smooth form) | no | every flat triangle shaded |
| SFFSF | from draw 0 (smooth) | yes | every flat triangle shaded |

Rule for a driver: emit the raster declaration link words at the pass setup
with bit 31 set (`80100d88 801006b6`) and select shading per draw only with
`22` bit 14 in the `45456a22` restate (`0x6400` flat, `0x2400` smooth).
Smooth rendering ignores the set link bit; flat rendering requires it.
Re-emitting the `23` declaration packets inside the raster pass does not
reliably change the link state and must not be used.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
