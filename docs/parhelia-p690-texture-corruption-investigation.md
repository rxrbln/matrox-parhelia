# The reported texture corruption on the P690: what it was, and what it was not

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

The user reported, watching ioquake3 on the P690 at 1280x1024: textures with
"black stripes through them at repeated intervals, like every 2nd line", some
textures rendering "only about half", the menu mouse cursor drawn top-half
only, and the same appearance in a 32-bit visual as in a 16-bit one.  This
records what was found, what was fixed, and - equally useful next time - the
explanations that measurement refuted.

## 1. The defects that were real, and are fixed

* **Small sprites lose a half, larger images come out row-interleaved.**  The
  `A1R5G5B5` repack wrote the vendor's *generic* 16-bit texel order while its
  descriptor selected storage order 4, this chip's own order.  The two share
  only three address bits, so on a 16x16 image rows 8-15 went where a 16-wide
  texture is never read.  Mesa `6664797`.
* **The same fault in a second path.**  The CPU upload fallback chose its
  address function by hand: the generic order for every 8- and 16-bit sampler
  image, and the plain 64x16 order for 32-bit ones without the coordinate
  permutation a native level carries.  Mesa `0b8009f`, which also moves the
  decision into one function (`parhelia_texel.h`) so a third path cannot
  disagree.
* **Every mip level from 1 on displaced, for any width that is not a multiple
  of 64.**  The chain was packed by rounding each level up to a whole 4 KiB
  tile.  The hardware packs it in **32x16-texel half tiles**: a 64x16 tile is
  two 32-column halves interleaved every 512 bytes, and a level begins at the
  half tile where the previous one ends.  Mesa `de0d9ad`, `ba4ea3e`.
* **Alpha sampled over the wrong extent.**  The native sampled-alpha vertex
  path recomputed the companion's padded extent instead of reading the extent
  the companion was allocated with.  Mesa `276e6dd`.

Hardware state after those: **121 of 121 probed mip levels byte exact** (68
before) and the whole `--mip-address-probe` battery clean over pitches 64..224
and heights 16..65.

A fourth cause was found after this document was first written, and it is the
reason large images still looked wrong:

* **The texture unit addresses at most 512 texels per axis.**  Bit 9 of both
  the x and the y texel coordinate is dropped, so half of a 1024- or 640-wide
  image reads the other half.  Linear storage wraps identically, so it is the
  address unit and not the tile order.  The driver now advertises
  `max_texture_2d_size = 512` on the P690, with render targets, viewports and
  scanout still reaching 2048 through a new `max_render_target_size` pipe cap.
  Mesa `eb2b7c3`, `mesa-max-render-target-size-cap.patch`,
  `parhelia-texture-size-limit-re.md`.

## 2. The explanations that measurement refuted

Each of these was a plausible mechanism for "stripes at a two-row pitch".  All
were tested on the live game or with a probe, and none is the cause.  They are
listed because the next person to see a striping artefact will think of them
too.

* **A sub-texel sampling bias.**  The best candidate: invisible at 1:1, where
  the whole battery ran, and crossing a texel boundary on alternate rows once
  an image is magnified - which is how every 2-D element of the game is drawn.
  `Q3FMT_GROUPS=bias` measures the sample point from the 50 % crossing of a
  magnified sharp edge: **+0.000 texel at 8x, +0.002 at 2x**, both axes.  The
  sampler is exact.
  Note the trap: the expected crossing sits *half a destination pixel* before
  the texel boundary, because destination pixel centres are at
  `(i + 0.5) / mag` texels.  The first version of the probe compared against
  the boundary and reported -0.062 texel at 8x and -0.248 at 2x - exactly
  minus half a destination pixel at each magnification, which is how it was
  caught.  A "bias" that scales with 1/mag is a measurement artefact.
* **The state aggregation and the compact state packets.**  Ran the game with
  `PARHELIA_BATCH_AGGREGATE=0 PARHELIA_COMPACT_STATE_PACKETS=0`: the HUD
  artefacts are pixel for pixel identical.
* **Mipmapping and LOD selection.**  Ran with `r_textureMode GL_LINEAR`, which
  takes mip filtering out of the picture: identical, in fact slightly stronger.
* **The native sampled-alpha companion route.**  Ran with
  `PARHELIA_NATIVE_SAMPLED_ALPHA=0`: identical.
* ~~**Large textures.**  A 640x480 image is byte exact at 1:1.~~  **Wrong - it
  was the real cause for large images.**  This bullet originally read that the
  earlier "fails everything" report came from the GL harness coding x and y in
  nine bits each.  The harness bug was real, but fixing it to ten bits per axis
  did not make 640x480 pass: it exposed a genuine wrap at 512 texels in the
  texture unit (section 1, fourth cause).  The lesson is that a test whose
  coding range coincides with the limit under test cannot measure that limit.

## 3. What the game actually shows now

Measured on the current build, P690 at 1280x1024, depth 24, trilinear:

* **No period-2 row striping anywhere.**  Per-row mean brightness over wall,
  corridor and floor regions gives an adjacent-row delta *smaller* than the
  two-row delta in every region (ratios 0.57 to 1.38, absolute deltas below
  2/255).  A striped image gives the opposite by a wide margin.
* **HUD glyphs render correctly.**  Dumping the digit "5" of "50 Health" as a
  bitmap shows the glyph intact, with faint partial rows at the top and bottom
  of its destination quad.  Those are the neighbouring atlas cells blending in
  at the quad edge - ordinary behaviour for a tightly packed font atlas sampled
  with `GL_LINEAR` and no texel-centre inset, not a driver defect.
* **The menu logo's "shear" is the artwork.**  Cross-correlating adjacent rows
  of the QUAKE III banner gives a constant **+1 pixel per row** with no
  discontinuity at any tile-row boundary: the logo is italic.

## 4. Still open in the texture paths

* Bilinear at 1:1 on a non-power-of-two width or an odd height misses one or
  two texels at the last column or row (96x64 differs in 64 of 6144 texels,
  64x48 in 49 of 3072).  Every point-sampled case of the same shapes is exact,
  so this is a filter-edge question, not addressing.
* `GL_TEXTURE_BASE_LEVEL` pointing into the shared tail block: the descriptor
  carries one base address, and a tail level's texels start at an x/y offset
  inside a 64x16 block, so a base address cannot name one.  Needs a different
  strategy rather than a layout tweak.
* The GL harness cannot code coordinates above 512 (nine bits per axis), so
  sizes beyond that are only checked at 1:1, where the coding cancels.

## 5. How to re-measure quickly

	# on the target, X running, driver loaded
	Q3FMT_GROUPS=bias  ./parhelia-gl-q3formats-test   # sample point
	Q3FMT_GROUPS=cell  ./parhelia-gl-q3formats-test   # magnified cells
	Q3FMT_GROUPS=full,linear,sub,mips,base ./parhelia-gl-q3formats-test
	./parhelia-drm-test --mip-address-probe /dev/dri/card0 96 40

An in-game frame is worth more than any of them for this class of defect:
`+demo bench +wait 250 +screenshot` writes a TGA under
`~/.q3a/baseq3/screenshots`, and a per-row brightness profile over a flat wall
region settles "is it striped" in one command.
