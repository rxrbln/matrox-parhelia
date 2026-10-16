# M9148 fill rate, overdraw and the hierarchical-Z plane

Everything here is MEASURED on the M9148 (102b:0540) through the m9x Mesa
driver, with `tools/m9-fillrate.c`:

```sh
gcc -O2 -o m9-fillrate tools/m9-fillrate.c -lEGL -lGL -lm
m9-fillrate list
m9-fillrate CASE [quads] [width height]
```

The probe renders whole-screen quads into an RGBA8 + D24S8 FBO from a
surfaceless EGL context, times a fixed number of them and divides, so every
figure below is "milliseconds per 1920x1200 screen of that kind of fill".
2304000 pixels; 1.00 ms per screen is 2.30 Gpixel/s.

## Two traps the probe is built around

* **`async_submit=1` makes a `glFinish` time the previous job.** The submit
  ioctl posts the job and returns; the job is retired by the *next* submission
  (`m9_submit.c`, `async_submit`).  Every number here was taken with
  `async_submit=0`.  With `async_submit=1` the probe reported 31 Gpixel/s for
  ordinary flat fill, because it was timing the set-up job.
* **A set-up pass that writes depth makes the timed pass reject almost
  everything.**  Cases that mean to shade every fragment use `GL_ALWAYS`.  With
  `GL_LEQUAL` the result read as "enabling the depth test makes rendering 3x
  faster", which is really "the second pass was thrown away".  The `addz` case
  counts the surviving quads with an additive blend and proves this directly:
  N far-to-near quads accumulate N+1 times over two passes, not 2N.

## The engine retires one fragment operation per pixel per 1.157 ms/screen

The `aluN` cases are one bilinear-mipmapped texture fetch followed by N extra
`c = c * k1 + v_col * k2` operations:

| case | ms/screen | delta |
|---|---:|---:|
| alu0 | 1.331 | |
| alu1 | 1.778 | +0.447 |
| alu2 | 2.933 | +1.155 |
| alu4 | 5.242 | +1.155 each |
| alu8 | 9.870 | +1.157 each |
| alu16 | 19.129 | +1.157 each |

Perfectly linear: **one fragment instruction costs 1.157 ms per 1920x1200
screen, i.e. 1.99 Gpixel-operations/s** (the first added operation is cheaper,
so the back end pairs it into an existing slot).  A texture fetch costs the
same as an arithmetic operation, and a trilinear fetch is two.

**The rate is not uniform across every slot.**  It was measured by adding
`c = c * k1 + v_col * k2` operations, which read an interpolated value, and it
holds for texture fetches; it does **not** hold for moves out of the constant
span.  The constant-colour case below is 6 instructions longer than the
interpolated one and costs only half an operation more, so a constant move is
roughly a twelfth of an arithmetic operation.  Program *length* is therefore
the wrong budget: count the operations that read interpolated or sampled data.

That single rate explains every other case:

| fill | ms/screen | Mpixel/s |
|---|---:|---:|
| interpolated colour, no depth | 1.171 | 1968 |
| the same quad drawn 4x oversized, so no edge crosses the screen | 0.904 | 2549 |
| constant colour, no depth | 1.757 | 1312 |
| interpolated colour + depth test and write | 1.429 | 1612 |
| interpolated colour + depth test, no write | 1.428 | 1613 |
| interpolated colour + alpha blend | 1.758 | 1311 |
| colour + depth clear (one full-screen quad) | 1.225 | 1881 |
| colour-only clear | 1.211 | 1902 |
| depth-only clear | 1.228 | 1877 |
| 1 texture, bilinear + mipmap | 1.304 | 1768 |
| 1 texture, trilinear (fractional LOD) | 2.340 | 985 |
| 2 textures, bilinear + mipmap | 2.367 | 974 |
| 2 textures, trilinear (fractional LOD) | 4.676 | 493 |
| 2 textures trilinear + depth write | 4.950 | 466 |
| 2 textures trilinear + depth write + blend | 5.075 | 454 |
| 3 textures, bilinear + mipmap | 3.547 | 650 |
| 1 texture, bilinear, no mipmap, 10x minified | 2.841 | 811 |
| **depth-rejected fragment (hidden behind an occluder)** | **0.177** | **13010** |

Notes, all measured:

* **Off-screen triangle area is free.**  The same quad scaled to 2x or 4x the
  viewport in each direction, so that 3/4 or 15/16 of it is off screen, costs
  0.904 ms rather than 1.191 - less than the exact-fit quad, because no
  triangle edge then crosses a visible tile.  There is nothing to win by
  clipping large geometry on the CPU.
* **The rate is resolution independent.**  Interpolated colour: 1968 / 1856 /
  1853 Mpixel/s at 1920x1200, 1280x800 and 960x600; two-texture trilinear:
  493 / 481 / 484.  Fill cost is linear in pixels.
* The **clear costs the same whether it writes colour, depth or both**
  (1.211 / 1.228 / 1.225).  It is one full-screen quad of one operation, not a
  bandwidth event, so there is nothing to win by clearing less; only by not
  clearing.
* **Masking depth writes off saves nothing** (1.429 vs 1.428).  The cost is in
  enabling the depth unit, not in the write.
* **Trilinear is free wherever the texture is magnified.**  Measured with the
  same case at several texture-coordinate ranges: at LOD < 0 trilinear and
  bilinear are identical to three digits; only a fractional minifying LOD costs
  the second fetch, and even then a small, cache-resident level costs less than
  a full one (1.47x rather than 2x at LOD 2.4).  A texture with a single level
  and a trilinear filter also costs exactly one fetch (2.880 against 2.879), so
  nothing is wasted there either.
* A **constant-colour fragment program costs 0.59 ms/screen more than one that
  interpolates a varying** (1.757 vs 1.171), and it is much longer: the
  backend compiles `gl_FragColor = vec4(...)` to **8 slots against 4** for
  `gl_FragColor = v_col` (OFFLINE, `m9-fs-compile-test --glsl` with
  `M9_FS_PROGRAM=1`; the varying program's last two slots are the stop pair, so
  it is 2 real instructions against 8).  This is **not a missed fold**: the
  same source is corpus case `fs-const`, which the backend reproduces byte for
  byte against the captured vendor program, so the vendor compiler emits the
  same shape.  The output move cannot take a constant operand, so every
  component is staged through a register first.
  What the pair does show is that those six extra instructions cost half an
  operation between them, which is why the rate above cannot be applied to slot
  counts blindly.

## Hidden fragments are already cheap, and the ba/bb plane is not why

A full screen of fragments that fail the depth test costs **0.177 ms**, 26
times less than a shaded two-texture trilinear fragment and 6.6 times less than
the cheapest shaded pixel.  Measured as the slope of the `hidden` case over 4,
8 and 16 quads drawn behind one near occluder.

The driver allocates an auxiliary "hierarchical Z" plane per depth target
(`m9_hz_bytes`, one word per 8x4 cell), fills it with the vendor's conservative
`0x007ff000`, and binds it at `ba/bb`.  The kernel parameter
`m9x_drm.hierarchical_z=0` emits the vendor's disabled `ba` form (a small
literal, which `m9_index_literal` already permits) instead of the plane's
address, so the two states can be compared directly:

| case | HZ bound | HZ unbound |
|---|---:|---:|
| hidden, 4 quads | 0.2011 | 0.2013 |
| hidden, 8 quads | 0.1878 | 0.1887 |
| hidden, 16 quads | 0.1826 | 0.1828 |
| hidflat, 4 quads | 0.1914 | 0.1912 |
| 2 textures trilinear + depth (`q3like`) | 5.0017 | 4.9998 |

In ioquake3 at 1920x1200 the same comparison is `19.37 ms` with the plane
bound and `19.33 ms` without it (3000-frame window, `refused 0 skipped 0`,
2303978 differing pixels in both), and the rendered frame is correct either
way.

**Binding the plane is worth nothing.**  Rejection runs at the same 13 Gpixel/s
either way, and shading costs the same either way.  This agrees with the
earlier userspace result in `parhelia-m9-cpu-tnl-draw-parameters.md`, where an
auxiliary BO initialised to `0` lost no visible fragment: the engine *writes*
the plane (one word per touched 8x4 cell) but does not appear to read it back
for rejection.  Whatever coarse rejection the engine does is internal to it.

The rejection rate itself proves it is coarse: a screen of rejected fragments
costs 0.177 ms, while a screen of colour writes (9.2 MB) costs 1.17 ms, i.e.
7.9 GB/s.  Reading one 4-byte depth word per rejected pixel would need 52 GB/s
on the same memory, so the engine tests at least 6.6 pixels per access.

The plane is therefore write-only overhead: `align(W,32)/8 * align(H,32)/4 * 4`
bytes of VRAM per depth target (285 KiB at 1920x1200), one extra BO reference
and one extra relocation in every job, and one metadata word written per 8x4
cell of every depth-writing draw.  None of that was measurable in time, but
none of it buys anything either.

## What the ioquake3 GPU half is made of

ioquake3 (`q3dm1`, opengl2, 1920x1200, 3000-frame window) runs at **19.37 ms**
per frame.  With `async_submit=0`, so that the CPU and the GPU do not overlap,
the same frame is **22.85 ms**, and `M9X_DRYRUN=1`, which builds and validates
every job without executing it, is **9.29 ms**.  The GPU half is therefore
**13.56 ms**: one colour+depth clear at 1.22 ms and 121 textured draws at
12.34 ms.  At 1.157 ms per operation per screen that is **10.7
screen-operations of fragment work per frame**.

The compiled fragment programs are long.  `M9X_DEBUG_FS=1` over a live match
reports these programs, counted by how often the back end emitted each one
(34580 emissions over a 25-second match); that is a proxy for how often they
are used, not for the pixels they cover:

| slots | textures | varyings | share of emissions |
|---:|---:|---:|---:|
| 10 | 1 | 2 | 43% |
| 14 | 1 | 2 | 22% |
| 12 | 1 | 2 | 13% |
| 12 | 2 | 1 | 11% |
| 16 | 2 | 1 | 9% |
| 14 | 2 | 1 | 2% |
| 6 | 1 | 0 | 1% |

The invocation-weighted average is **11.9 slots**.  10.7 screen-operations at
11.9 slots per pixel means the frame shades about **0.9 screens of fragments** -
there is no overdraw problem to solve, and the depth unit is already throwing
the rest away at 13 Gpixel/s.  The GPU half of this frame is program length,
almost nothing else.

**One slot removed from the average program is worth about 1.04 ms of GPU per
frame** (0.9 screens x 1.157 ms).  That is the only large lever left on this
side.

Measured levers, end to end in the game:

| change | frame time | delta |
|---|---:|---:|
| baseline | 19.37 ms | |
| `hierarchical_z=0` (unbind the ba/bb plane) | 19.33 ms | -0.04 |
| `cg_draw2D 0` (no HUD: 43 jobs/107 draws instead of 49/121) | 18.56 ms | -0.81 |
| `r_textureMode GL_LINEAR_MIPMAP_NEAREST` (bilinear) | 18.06 ms | -1.33 |
| `r_fastsky 1` (flat sky: 47 jobs/119 draws) | 14.45 ms | **-4.92** |

Trilinear is worth only 1.33 ms of the frame, not the 2x the probe shows for a
fractional LOD: most of what q3 draws at 1920x1200 is magnified, where the
hardware already takes one fetch.  That is not a trade worth making - 6.9% of
the frame for a visibly worse image.

The sky is 2 draws and **4.92 ms**, 36% of the whole GPU half, because a long
program runs over a large part of the screen.  Shortening the compiled sky
program is worth more than anything else measured here.

90 fps is 11.1 ms.  The GPU alone needs 13.56 ms, so no scheduling change
reaches it: even perfect overlap of the 9.29 ms CPU floor with the GPU leaves
13.56 ms, 73.7 fps.  The frame has to lose 2.5 ms of fragment work, and at
1.04 ms per average slot that is two to three slots off the compiled programs -
work for the fragment back end, not for the raster state, and it still needs
the remaining 3.5 ms of CPU/GPU serialisation removed on top.  Failing that,
fill is linear in pixels, so the same scene fits an 11.1 ms GPU budget at about
1.89 Mpixel, i.e. 1600x1200 lands at roughly 11.3 ms of GPU.
