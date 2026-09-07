# Parhelia: GL primitive and feature encodings

Every entry below is implemented in `tools/gen-stream.py`, which generates our
own command stream (no vendor bytes), and exercised by
`tools/test-gl-matrix.sh`.  Results are judged by `tools/verify-draw.py`
(de-tiles the render target and compares coverage and per-pixel colour against
exact barycentric interpolation).

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Primitives — all 10 encodings measured

`--prim` sets four registers: the primitive word `0x10b0`, the class in
`0x10f0`, `0x1130` bits [25:24], and the low nibble of DWGCTL `0x1300`.

| GL primitive | `0x10b0` | class | DWG nibble | HW verified |
|---|---|---|---|---|
| `POINTS` | `0x00060020` | 0 | `0xd` | 0 px until `--feature points` (below) |
| `LINES` | `0x0006103d` | 1 | `0x5` | 58 px |
| `LINE_STRIP` | `0x0006203d` | 1 | `0x5` | 170 px |
| `LINE_LOOP` | `0x0006203d` | 1 | `0x5` | 170 px |
| `TRIANGLES` | `0x0006305c` | 2 | `0x4` | 15140 px, colour-exact |
| `TRIANGLE_STRIP` | `0x0006405c` | 2 | `0x4` | 26696 px |
| `TRIANGLE_FAN` | `0x0006505c` | 2 | `0x4` | 17007 px |
| `POLYGON` | `0x0006505c` | 2 | `0x4` | 23654 px |
| `QUADS` | `0x0006605c` | 2 | `0x4` | 31323 px |
| `QUAD_STRIP` | `0x0006705c` | 2 | `0x4` | 35840 px |

`LINE_LOOP` and `LINE_STRIP` share a hardware primitive (the ICD appends the
closing vertex); so do `POLYGON` and `TRIANGLE_FAN`.

## Features — measured deltas

Taken from `captures/gl-matrix` with `tools/attrib-at-draw.py`, which snapshots
the running register/block state **as of a 3D draw** rather than diffing whole
captures (a capture holds 1000+ draws and the trailing ones are X's).

| GL feature | encoding |
|---|---|
| `GL_CULL_FACE` | `0x1130` bit 0 |
| flat shading | `0x1130` bit 12 |
| `GL_DEPTH_TEST` | `0x12c8` bit 0 |
| depth func | `0x12c8` low nibble (`e4` -> `eb`) |
| depth mask | `0x12c8` |
| `GL_ALPHA_TEST` | **`0x12e4` fully decoded**: bit0 enable, `[3:1]` compare func, `[15:8]` 8-bit reference = `round(ref*255)`. Funcs: NEVER 0, EQUAL 1, LESS 2, LEQUAL 3, GREATER 4, GEQUAL 5, NOTEQUAL 6, ALWAYS 7 (a 3-bit predicate mask: bit0 EQUAL, bit1 LESS, bit2 GREATER) |
| `GL_BLEND` | **`0x12e8` fully decoded**: bit0 enable, `[4:1]` srcRGB, `[8:5]` dstRGB, `[12:9]` srcA, `[16:13]` dstA, `[22:17]` equation (written twice, no separate RGB/alpha equation), bit23 dither. Factors: 0 ZERO, 1 ONE, 2 CONST_COLOR, 3 INV_CONST_COLOR, 4 SRC_COLOR, 5 INV_SRC_COLOR, 6 SRC_ALPHA, 7 INV_SRC_ALPHA, 8 DST_COLOR, 9 INV_DST_COLOR, a DST_ALPHA, b INV_DST_ALPHA, c SRC_ALPHA_SATURATE, e CONST_ALPHA, f INV_CONST_ALPHA. Equations: ADD 0, SUBTRACT 1, REV_SUBTRACT 2, MIN 5, MAX 6 (MIN/MAX force all factors to ONE). Blend constants `0x12ec`=(R<<16)\|G, `0x12f0`=(B<<16)\|A, each **10-bit** `trunc(f*1023)` |
| `GL_FOG` | `c[0x154]` = (0, 1, 1, 1.4427 = 1/ln2) |
| polygon offset | `0x1000`=2, `0x10f0`=0x86, `c[0x178]` |
| `GL_LIGHTING` | `c[0x0E0..0x0E6]` (7 vec4), `c[0x14E]` |
| `glColorMask` | **`0x1324`**, a per-byte ARGB write mask (`ffffffff` -> `ffff00ff` disables green) |
| `GL_SCISSOR_TEST` | `0x1328` min, `0x132c` max, each `(y<<16)\|x` |
| `glPointSize` | `c[0x172].w` = **half** the size |
| **multitexture** | **4 units**, addressed through a *windowed* register file: write `0x1200 + unit*4` to select, then that unit's `0x11e8, 0x11c0, 0x11c4, 0x11c8, 0x11cc, 0x11d0, 0x11d4, 0x11d8, 0x11dc, 0x11e0, 0x11e4`. Format/filter/size are fully independent per unit. Confirmed in three binaries and on the wire (a single 36-dword section programs all four units through the one window). `0x1118 = 0x00006543` routes unit *i* to output `o(3+i)`; `0x1110` bit `8+i` enables texcoord set *i* and is **silently cleared** by the kernel if the bound WARP program does not write that output |
| texenv `GL_ADD` | rgb `0x1264 = 0x00040000\|S`, `0x126c = 0x01400c00`; separate alpha `0x1260 = 0x08000000`, `0x1268 = 0x01400000` |
| texenv `GL_DECAL` | rgb `0x1264 = 0x00040000\|S`, `0x126c = 0x01400b00` (`LRP r0,TEX,PREV,TEX.a`); alpha `0x1260 = 0x00000200`, `0x1268 = 0x01400400` |
| **texture filter** | `0x11c0 = 0x01c01000 \| MIN \| MAG<<3 \| MIP<<6`. Codes: 0 nearest, 2 linear, 3 anisotropic; MIP 0 none, 1 nearest, 2 linear. `[24:16]` is a signed 4.4 LOD bias biased -4.0 (so `0x01c00000` = bias 0). **MIN/MAG order is the reverse of the software struct order** - both binaries cross them when packing |
| **texture wrap** | `0x11c4 = 0xf0000000 \| S \| T<<3 \| R<<6`. Codes: REPEAT 0, CLAMP_TO_EDGE 1, MIRRORED_REPEAT 4, mirror-once 5, `GL_CLAMP` 6, CLAMP_TO_BORDER 7. `[31:16]` are LOD min/max clamps in unsigned 4.4 (`0xf0` = 15.0) |
| **mipmaps** | `size[l] = align(align(w,32) * align(h,Ha) * bpp/8 * d, A)` with running-sum offsets and **32-texel pitch rounding at every level**; cube maps x6. Levels below 16x16 share one hard-coded 32x16 "mip tail" block. `0x11e4[31:28]` = maximum level; the hardware walks the chain from a single base address. No auto-generation - an incomplete chain leaves the unit **unbound** rather than degrading |
| texturing | `0x11dc` base\|space, `0x11e0` = rawWidth \| tiling<<16 \| fmt<<24, `0x11e4` = height \| depth<<16, `0x11c0` FILTER, `0x11c4` WRAP, `0x1110` bit 8 = texcoord set 0, combiner slot 0 = texture-address stage (`0x1264=0x00004000`, `0x126c=0x00000c00`) |

Two-unit vertex is `x,y,r,g,b,s0,t0,s1,t1` = 9 dwords, header **`0x97100001`**,
descriptors `0x20c70050` (tc0 -> v7) and `0xa0c80070` (tc1 -> v8), `0x1110 =
0x00000303`, and the WARP program must write `o4` as well
(`parhelia-warpasm.py --gen passthrough --tex 2`).

The diffuse x lightmap combiner is 5 slots (0, 2, 4, 8, 10): two texture-address
stages, `MUL_sat r0,t0,v0`, `MUL_sat r0,t1,r0`, and a pad. `0x1260` never
carries a slot address - it is `0x1264` with bits [4:0] cleared, and
`0x1268 = 0x126c & ~0x124`.

## Draw packet and draw modes

The draw header carries a **3-bit draw mode at bits [28:26]**:

```
header = 1<<31 | mode<<26 | 0x03<<24 | (dwordsPerVertex-1)<<17 | (totalDwords-2)
```

| mode | header | packet |
|---|---|---|
| 5 | `0x97080001` | inline vertex bytes `[hdr, start, end]` - what we use today |
| 6 | `0x9b000001` | **16-bit index array** `[hdr, start, end]` |
| 7 | `0x9f000001` | **32-bit index array** - ioquake3's `glDrawElements` path |
| 3 | `0x8f000001` | draw from bound streams `[hdr, first, endExclusive]` |
| 0/1/2 | | inline vertices / inline 16- and 32-bit indices |

The model reproduces every measured header exactly: 5 dwords `0x97080001`,
7 `0x970c0001`, 8 `0x970e0001`, 9 `0x97100001`.

There is **no index count, no index base and no base-vertex register** - the
packet's start/end give the index byte range, so `SHADER_MAX_INDEXES` (98304)
needs no splitting. Indices are 4-byte aligned; the memory class rides on the
`end` operand. For 16-bit indices, `start` bit 1 skips the first halfword and
bit 0 the last.

**Vertex streams**: 8 streams, `0x1000 + 4*s` = base | class, `0x1040..0x104c`
= per-stream strides. Attribute descriptors go through `0x10b4`/`0x10b8`
(max 31):

```
[2:0] stream | [11:4] dword offset | [16] non-position | [19:17] semantic
(0 pos, 1 normal, 2 colour, 3 texcoord0) | [25:22] component mask
(xy=3, xyz=7, xyzw=0xf) | [29:27] origin (4 = client) | [31] last
```

So the hardware fetches strided arrays directly - no CPU de-indexing needed,
provided the arrays live in GPU-visible memory. The vendor GL never exercises
this because a client array has no GPU address, so it de-indexes on the CPU.

**CONFLICT to resolve on hardware**: this RE says `0x1040..0x104c` are
per-stream **byte** strides (20 for our 5-dword vertex), but we have measured
`0x1040 = 4` on a 20-byte vertex, and an earlier RE pass read it as
dwords-per-vertex minus one. Both cannot be right. Resolve before relying on
the stream path.

Vertex layout follows the enabled attributes:

### Dependencies and constraints

- **DWGCTL gates blending**: the enable bit is force-cleared unless the ROP
  nibble `0x1300[7:4]` is `0xC` (GXcopy). Our 3D baseline `0x00a60ec4` has
  `[7:4] = 0xc`, so the gate is open.
- It is also force-cleared for the no-op identity ONE/ZERO with ADD/SUBTRACT.
- The render-target format constrains only the dither encoding and the
  `DST_ALPHA` factors, which degrade to ONE/ZERO on a target with no
  destination alpha.
- No dependency on `0x12fc` RASTERMODE, `0x12c8`, or the fragment combiner.

All four encodings reproduce the hardware-measured values bit-exactly
(`0080eced`, `00800202`, `00008009`, `0000000e`) and are implemented as
`blendctl()` / `alphatest()` in `tools/gen-stream.py`.

## Hardware test status — and a retraction

A 34-variant matrix was run and reported, and **most of those numbers were
void**: the engine had stopped drawing, `3dtri`'s own health line said
"drew 0/120000 px -- ENGINE NOT HEALTHY", `--ringreplay` was aborting before
submitting, and every pixel count was the GL client's leftover triangle
(16128 px). The engine reported `idle 1` throughout and nothing stalled, so
the guard passed. Only the fact that all 34 variants reported an *identical*
count exposed it.

The cause was **not** a missing `dma32buf` module or a wrong kernel - both were
wrong guesses of mine. `2.6.30-dist` is the only kernel the vendor `mtx` driver
runs on and is what we have always used, and `dma32buf` has never been loaded,
including during the runs that worked. The real signal was in dmesg:

```
[mtx] *ERROR* Parhelia OS: Failed to allocate locked pages (nb: 801, type: 2)
[mtx] *ERROR* Failed to create device buffer (size: 3280896, type: 1)
```

The vendor driver had exhausted its locked-page pool after repeated X and
client restarts, so no GL context could be created and our submissions did
nothing. A reboot clears it.

Genuinely measured (boot `b2f5c639`, engine healthy, counts varied as they
must):

| variant | px | reading |
|---|---|---|
| lines / linestrip / lineloop | 58 / 170 / 170 | line primitives rasterise |
| triangles / tristrip / trifan | 15140 / 26696 / 17007 | |
| quads / quadstrip / polygon | 31323 / 35840 / 23647 | |
| base, flat, depthfunc, alpha, blend, blend_add, blend_sat, fog | 15140 | draw unaffected, as expected |
| **alpha_lt** | **0** | `LESS 0.5` correctly rejects alpha 1.0 - **validates the alpha encoding** |
| **blend_mod** | **0** | `DST_COLOR x ZERO` on a black target - **validates the blend encoding** |
| cull | 0 | our winding is culled |
| depth / depthmask | 0 | needs a real depth buffer to be meaningful |
| polyoff | **stalled the engine** | caused by writing `0x1000 = 2`; see below |
| light, colormask | 15140 | (boot `7e6d7010`, before the engine died) |

**Matrix now complete - every primitive and feature has a hardware result.**
Measured across boots `b2f5c639`, `2b7ac7da` and the texture pass:

| variant | px | reading |
|---|---|---|
| polyoff | 15140 | passes once the bogus `0x1000 = 2` write is removed |
| **scissor** | **11909** | **clipped from 15140 - the `0x1328/0x132c` rectangle works** |
| psize | 15140 | unchanged, correct: point size cannot affect triangles |
| texenv_add | 15140 | no visible change without a bound texture |
| **texenv_decal** | **2764** | **real effect - the decoded `LRP` blend changes the output** |
| tex_linear / mipmap / clamp / mirror | 15140 | unchanged, correct: sampler state is inert with no texture bound |
| texture / multitex | 0 | point `0x11dc` at the vendor's `0x09006000`, a texture we do not own - needs our own upload before this is a real test |
| points (primitive and feature) | **stalls the engine** | open defect, not a coverage gap |

So of the 34 variants: 31 render as expected, `texture`/`multitex` need our own
texture in place first, and `points` is a genuine open defect.

Two defects found and fixed:
- `polyoff` wrote `0x1000 = 2`. `0x1000` is stream 0's base|class, so that
  repointed the vertex fetch from PCI/system to AGP and stalled the engine.
  The delta came from the capture's stream binding, not from polygon offset.
- `colormask` set `0x1324 = ffff00ff` (green off) and the base stream never
  writes `0x1324`, so the mask stayed **latched for every later variant** -
  green was zero in all of them. The generator now emits explicit defaults for
  every register a feature can insert.

## Open

- **`GL_POINTS`: cause found, awaiting hardware.** Selecting only the 6 draws
  in `points-secs.txt` that actually carry the points primitive word (of 455
  total) shows four registers the vendor sets for points which the triangle
  stream never writes at all:

  | register | value |
  |---|---|
  | `0x1108` | `00000000` |
  | `0x1154` | `00000001` |
  | `0x1170` | `00000002` |
  | `0x11f4` | `00000001` |

  `--prim points` only rewrote the four primitive registers, which already
  matched - so nothing was wrong with them, the state was simply incomplete.
  Implemented as the `points` feature in `gen-stream.py`, which now inserts a
  register write before the draw when the base stream never writes it.
  **Not yet confirmed on hardware.**
- **line width / line smooth are NOT yet known.** The `lwidth`, `linesmooth`
  and `psize` captures in `captures/gl-matrix` are mislabeled - all three
  produce *identical* state including texture registers, so they captured a
  textured variant, not the intended feature. They must be re-captured.
- **indexed draws** were never captured (`GLTEST_INDEXED` exists in the client).
- The texture encoding has not yet been exercised on hardware.

### Texture memory layout - NOT the render target's swizzle

Textures are tiled, but with a **different** tile from the colour target:
**32x16 texels built from 16-byte micro-tiles, with no XOR parity term**, versus
the render target's 64x32 tile with `x5 XOR (tile_row & 1)`. A prior RE pass
asserted they shared a layout; that was wrong and is now corrected.

**Layout code 0 is a real linear mode** and is the recommended first
implementation - no swizzling needed to get textures working.

Other sampler registers, corrected: `0x11c8` = border colour; `0x11cc`/`0x11d0`
= colour key and mask (not "border"); `0x11d4`/`0x11d8` = bump matrix (not
"always 0"). Max texture 2048, POT-only through the ICD, 4 units, anisotropy 2.

Also corrected: the earlier note that "texture memory is LINEAR, confirmed on
hardware" had measured the **CPU staging heap**, not the VRAM surface that
`0x11dc` points at.


## Texturing — state of the investigation (2026-09-07)

Established on hardware, by bisection rather than inference:

| step | result |
|---|---|
| 5-dword Gouraud baseline | 15140 px |
| 7-dword vertex + `0x10b8` descriptors, no texture state | 15140 px - **the wide-vertex path is entirely correct** |
| texture descriptor (`0x11dc/e0/e4/c0/c4`) alone | 15140 px - harmless |
| texcoord enable (`0x1110` bit 8) alone | 15140 px - harmless |
| combiner slots alone | 0 px - **the combiner is what gates the fragments** |
| + `0x1200 = 0x00040103` (texture-unit select) | **15140 px textured** |

So a textured triangle rasterises: correct geometry, correct fragment coverage,
a working combiner, our own texcoord WARP program (verified to write `o3`).

**What is still wrong: the texel fetch does not read our texture.** Three solid
textures - green `ff00ff00`, blue `ff0000ff`, red `ffff0000` - all render the
identical constant `0x78008010`. The poke is verified to land (`--peek` shows
`ff00ff00` at `0x3800000` right after writing it), so the memory is correct and
`0x11dc = 0x03800000` is simply not steering the sampler there.

One thing narrows it: `0x1200`'s ordering matters - emitting the unit select
before the per-unit registers changed the sampled constant from `0x00204060` to
`0x78008010`, confirming `0x11c0..0x11e4` land in a **windowed** register file.

Tried and rejected: `0x11f4 = 1` and `0x11fc = 0x00800202` (both in the same
per-unit staging group in `mtx_parhelia.o`) zero the draw entirely.

### The cause: the combiner's ALPHA pipe was never programmed

A solid-colour texture is invariant under layout, dimensions and almost any
format reinterpretation, so *none* of those can produce one identical constant
across three different source colours. That rules out the whole descriptor and
leaves only: the sampler never performs the fetch.

Decoding the combiner writes of all five vendor texture captures
(`tex`, `texrgb`, `texlum`, `texclamp`, `texrepl`) gives a byte-identical
program in every one:

```
0x1264 = 0x00004000   RGB   slot 0, texture-address stage
0x1260 = 0x00004000   ALPHA slot 0
0x126c = 0x00000c00   RGB   instruction
0x1268 = 0x00000c00   ALPHA instruction
0x1264 = 0x08000002   RGB   slot 2, MODULATE
0x1260 = 0x08000000   ALPHA slot 0
0x126c = 0x01400000   RGB   instruction
0x1268 = 0x01400000   ALPHA instruction
0x1264 = 0x00000084 / 0x126c = 0x00400400     slot 4  pad, RGB only
0x1264 = 0x00000088 / 0x126c = 0x00400400     slot 8  pad
0x1264 = 0x0000008a / 0x126c = 0x00400400     slot 10 pad
```

Every stage writes **both pipes**, and both slot addresses precede both
instructions. Our texture features wrote the RGB pipe only, and `tex_enable`
additionally set `0x1260 = 0`, zeroing the alpha slot address. The base
untextured recipe had always written all four (`gl_recipe.py`, slot 0), so the
texture path was the regression, not the rule.

This also explains a result previously recorded as a hardware fact: the RE
doc's MODULATE encoding `0x08000002`/`0x01400000` "draws nothing". It is
exactly what the vendor emits; it drew nothing because the alpha pipe was
holding whatever the previous program left.

Corrected at the same time:
- `0x11e0[13:0]` is the **raw width in pixels**, confirmed by `gltest.c`: the
  2x2 texture captures read `w=2` and the 8x8 `texclamp` capture reads `w=8`.
- The vendor's textured draw uses `0x11e0 = 0x32020002` - layout 2, format
  0x32 - for GL_RGBA, GL_RGB *and* GL_LUMINANCE alike; the ICD converts every
  upload to one internal format.
- The "bank B" reading of `0x11dc = 0x09006000` was unfounded: VRAM is 256 MB
  (`fb_size = 0x10000000`), so `0x9006000` is an ordinary byte offset at
  150.5 MB. It is kept only as a control for the address translation.

### Verified by a full state diff, not just by inspection

Snapshotting *every* register as of a textured draw and diffing our generated
`tex_full` stream against the vendor's leaves **no register the vendor sets
that we never write**, and 14 value differences, all accounted for:

| register(s) | why they differ |
|---|---|
| `0x1060`, `0x1090`, `0x1094`, `0x12bc` | fence tag and write-back address, per submission |
| `0x10c8`-`0x10d8`, `0x1100` | WARP program entries; boot-specific, ours patched at submit |
| `0x10b8` | snapshot artefact - the vendor's descriptor list ends with 0, ours with the last descriptor |
| `0x11c4` | see below |
| `0x11dc` | our texture address vs the vendor's, deliberate |
| `0x132c` | window scissor, different window size |

Two apparent differences were run down rather than assumed:

- **`0x1110` was really wrong.** The vendor writes `0x00000103` at every
  textured draw; we wrote `0x00000101`. Confirmed by isolating each capture's
  own client through its window scissor (`tex`, `texlin`, `texrepl` all agree).
  Bit 1 comes on with texturing. Fixed.
- **`0x11c4` was a false alarm.** `texrepl`'s `0xf0000036` is that capture's own
  state (`GLTEST_TEXENV=1`); the plain `tex` and `texlin` captures both use
  `0xf0000000`, so wrap code 0 = `GL_REPEAT` and our encoder was right. The
  first sample had picked a draw belonging to a different client in the same
  capture - always isolate by scissor before comparing.

`tools/gen-stream.py` carries the vendor program as `TEXCOMB` and a `tex_full`
variant that is the complete textured draw.

### Measured on hardware (2026-09-07) — the combiner was NOT the cause

With the ALPHA pipe programmed and `0x1110 = 0x103`:

| variant | texture | result |
|---|---|---|
| `tex_full`, base `0x3800000` | solid green | 15140 px, `78008010` / `78007f10` |
| `tex_full`, base `0x3800000` | solid blue | 15140 px, **identical** |
| `tex_full`, base `0x3800000` | four-colour, adjacent texels differ | 15140 px, **identical** |
| `tex_full`, base `0x4000000`, same texture poked there | four-colour | 15140 px, **`f467d92a` / `f467d82a`** |
| `tex_full` + `0x11e8 = 0x00500000` | any | **0 px, and it latched the engine** |
| `tex_bankb`, base `0x9006000` | any | 0 px |

Our texcoords are correct (`s,t` = (0,0), (0.5,1), (1,0), verified in
`p-textri.hex`), so a four-colour texture rendering identically to a solid one
means **no texel from our memory reaches the combiner**.

But the base is not ignored: **changing `0x11dc` changed the output colour**.
So the sampler really does fetch VRAM — it just fetches somewhere other than
where we poke. Everything else in the path is now excluded:

- not the combiner (the alpha-pipe fix changed nothing)
- not the layout, size or format (a solid texture is invariant under all three,
  yet three different solid colours give one result)
- not the texcoords (measured, they span 0..1)
- not the texture-cache partition — `0x11e8 = 0x00500000`, the value from the
  vendor's blit path, **zeroes the draw and latches the engine**. No capture
  ever writes it; do not write it.

**The remaining defect is exactly one thing: the address transformation between
`0x11dc` and the memory the sampler fetches.** `tools/parhelia-3dtri.c` gained
`--pokepattern BASE LEN`, which fills VRAM with dwords that encode their own
offset, so a texel read back names the address it came from — the same
colour-encodes-position method that solved the render target tiling. Two
different bases give the mapping's slope as well as its offset.


## TEXTURING WORKS (2026-09-07)

A triangle textures correctly from a texture we create ourselves. Verified with
a four-quadrant texture: the quadrants map across the triangle with a straight
boundary at `s = 0.5`, blue/white along `t = 1` and red/green along `t = 0`.
Solid-colour uploads also track exactly - green renders `0000ff00`, blue
`000000ff`, red `00ff0000`.

Two defects had to be fixed, and neither was in the sampler.

### 1. The CPU cannot write VRAM above 16 MB — upload with the engine

Writes through BAR0 above `0x1000000` are silently dropped by the hardware
(measured in-process, X and the client frozen; see
`parhelia-engine-context-bringup.md` §2.3). Every CPU-poked texture wrote
nothing, so the sampler read whatever was already there - which is why three
different textures rendered one identical colour while changing `0x11dc`
changed it.

`tools/parhelia-3dtri.c` gained `--texfill BASE COLOUR W H [X Y]` and
`--texpitch P`, which fill a rectangle **with the 2D engine** using the same
proven ring recipe as `--dmafill`. This is how the vendor uploads too, and it
has the further advantage of writing the texture in the surface's own tiled
layout, which is exactly what the sampler expects.

`--texpitch` matters: the cfg pitch is the **surface** pitch, never the fill
width. Deriving it from the rect makes a narrow fill address a differently
shaped surface and clobber far more than the rectangle asked for.

### 2. The vertex-fetch descriptor list needs a reset and a terminator

The vendor's own packets are:

```
0x10b4 = 0x00000000        <- resets the descriptor list index
0x10b8 = 0x20c00000        position,  vertex dword 0
0x10b8 = 0x21c50020        colour,    vertex dword 2
0x10b8 = 0xa0c70050        texcoord0, vertex dword 5, last
0x10b8 = 0x00000000        <- terminates the list
```

We wrote only the three middle descriptors. Without `0x10b4 = 0` they
**append** to the list already loaded rather than replacing it, so the
texcoord attribute was never fetched: `v7` stayed constant and every fragment
sampled texel (0,0). The low byte of a descriptor is the attribute's offset in
the vertex as `dword << 4` - `0x00`, `0x20`, `0x50` for dwords 0, 2 and 5.

The symptom was diagnostic once seen: filling green at texel (0,0) coloured the
whole triangle green, while filling blue at (32,32) did not appear at all.

### What was NOT the cause

Recorded because each cost a round of hardware time:

- **the combiner's ALPHA pipe.** The vendor really does program `0x1260`/
  `0x1268` at every stage and we did not, and that is now fixed - but it was
  not the fault; texturing stayed broken after it.
- **layout, format or texture dimensions.** A solid texture is invariant under
  all three.
- **texture coordinate scale.** Coordinates scaled by 64 changed nothing.
- **`0x11e8`.** `0x00500000`, the value in the vendor's blit-path unit state,
  **zeroes the draw and latches the engine**. No capture ever writes it.
