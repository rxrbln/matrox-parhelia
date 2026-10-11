# glxgears as the driver's correctness and performance test

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

`src/parhelia3d/p3d_gears.c` renders glxgears through the hardware and checks
the result against a CPU render of the same scene.  It is the first test that
exercises the whole pipeline at once on real geometry rather than one triangle.

## The scene

glxgears' `gear()` vertex for vertex, by way of the port in
`npot-wrap/tools/baremetal/gears.c`: three gears at 20/10/10 teeth with their
radii, widths, colours, positions, rotation rates and phases, the eye 40 back,
the 20/30 degree tilt and `glFrustum` with the short axis fixed.  800 triangles,
2400 vertices.

Everything the hardware can do runs on the hardware:

| stage | where |
|---|---|
| model-view-projection | WARP, matrix in c0..c3 |
| perspective divide | WARP, `smul` = x rcp(w) |
| viewport | WARP, c8/c9 |
| lighting | WARP: `globalAmbient*C + C*max(0, N.L)`, light c4, material c5/c6 |
| clipping | viewport, no extra state |
| hidden surface | Z24 depth buffer, LESS |
| clear | in-stream 2D fill |

Only the matrix arithmetic is on the CPU, which is where a driver would do it.
The vertex data is object space and does not change between frames - only the
three matrices do - so an animation is a constant re-upload and three draws.

## Correctness

Six frames, 5 degrees apart, each compared pixel by pixel against
`p3d_gears --reference`, which applies the same transform, the same lighting
model and the same 24-bit depth quantisation:

| frame | hw px | ref px | delta | exact | within 2/255 |
|---|---|---|---|---|---|
| 0 | 167423 | 167431 | -8 | 86.31% | 95.42% |
| 1 | 167736 | 167741 | -5 | 86.35% | 95.42% |
| 2 | 167915 | 167901 | +14 | 86.41% | 95.46% |
| 3 | 167532 | 167520 | +12 | 86.42% | 95.46% |
| 4 | 167505 | 167515 | -10 | 86.35% | 95.49% |
| 5 | 167924 | 167926 | -2 | 86.42% | 95.46% |

**Coverage agrees to within 14 pixels out of ~167500** - 0.008% - on every
frame, so the geometry, the transform, the clip and the depth test are right.

The pixels that differ by more than 2/255 are about 21% of the gear area, and
**97.3% of them carry a colour that is a valid face colour of that gear** - a
different, adjacent facet won at that pixel.  Only 0.4% of gear pixels are a
genuinely different shade.  The cause is the reference, not the hardware: it is
a naive rasteriser with no top-left fill rule, so on the shared edges of 800
small facets it resolves coverage differently.  Quantising the reference's
depth to 24 bits changes the count by one pixel, so it is not depth precision.

## Performance

Measured with `parhelia-3dtri --rrrepeat N`, which re-submits a built frame N
times inside one process, advancing the batch fence each time, and times the
loop.  The submission uses the vendor's cadence - one packet per doorbell,
polling idle between - which is what these streams are known to survive.

| gear | triangles | per submission | rate |
|---|---|---|---|
| 0 (red, 20 teeth, plus the frame clear) | 400 | 3.564 ms | 280.6/s |
| 1 (green, 10 teeth) | 200 | 0.894 ms | 1119.1/s |
| 2 (blue, 10 teeth) | 200 | 0.893 ms | 1119.4/s |

A frame is the three together: **5.35 ms, or 187 frames per second at
1024x768**, 800 triangles per frame, about 150k triangles/s.

Gear 0 costs four times gear 1 for twice the triangles because it also carries
the frame's two full-screen clears; the difference, ~1.8 ms for 1.6 Mpixel of
2D fill, is the clear rather than the geometry.

This is a floor, not a ceiling: the one-packet-per-doorbell cadence spends a
doorbell and an idle poll on each of the 35 packets in a gear's stream.  A
driver that batches packets between doorbells has that headroom available.

## Reproducing

```
p3d_gears --emit F0 --angle 0          # three gears: vertices + streams
p3d_gears --reference F0-ref.ppm --angle 0
parhelia-3dtri --warpstore --warpidx 32 --warpbind 2 loader.warp.hex 0 \
    --warpprog light5-prog.hex --rrfence auto \
    --ringreplay F0-g0-{ring,sec}.hex F0-g0-reloc.txt --ringvtx F0-g0.vtx.hex
```
then the same for `g1` and `g2`; add `--rrrepeat 101` for the timing.

## Batching: safe only for triangles that do not overlap each other

Across all 45 capture files the vendor emits 31329 draw packets: 22330 of three
vertices, but also 1643 of four, 1224 of six, 631 of sixteen and 815 of
twenty-four - and the 24-vertex ones carry the ordinary TRIANGLES primword
`0x0006305c`, so they are eight independent triangles in one packet.  Batching
is supported.

What is NOT supported is batching triangles that **overlap each other in screen
space**: the depth test between triangles inside one packet is not honoured.

The measurement that isolates it, on gear 0 alone:

| geometry | silhouette | pixels wrong vs the CPU reference |
|---|---|---|
| front faces only | 95385 | 29 |
| front + back faces, ONE draw packet | 95385 | **11759** |
| front + back faces, TWO submissions | 95385 | **29** |

The back faces are entirely hidden - they add **no** pixels to the silhouette -
yet sharing a draw packet with the front faces corrupts 11759 of them.  Split
across submissions the same triangles are exact.  Hidden geometry cannot
legitimately change anything, so this is a pipeline hazard, not a depth-value
or precision problem: the reference's depth quantised to 24 bits moves the
count by one pixel, and the hardware's own Z buffer agrees with the reference
to within 64/2^24 on 96% of covered pixels.

### Measured, gear 0 against the CPU reference

| triangles per draw packet | pixels wrong |
|---|---|
| 1 | **45** |
| 2 | 1150 |
| 4 | 10701 |
| 10 | 8171 |
| 20 | 10571 |
| all 400 | 18516 |

But with triangles that never overlap - the gear's front face group, 60
triangles tiling a plane - **all 60 in ONE packet is pixel-identical to 60
separate packets** (both 100.00% within 2/255, the same 20 pixels, the same
coverage).  So the limit is overlap, not count.

Applying that: batch each face group into one packet, except the two groups
whose triangles occlude one another - the four outward faces of a tooth, and
the inner cylinder, a tube whose near and far walls overlap.  Gear 0 then draws
in 204 packets instead of 400 with **exactly the same 45 wrong pixels**, and a
frame costs 11.6 ms instead of 14.6 - **86 fps** for 800 triangles at 1024x768.

These batching measurements are from the 0528.  On the P690 a multi-triangle
0x97 range currently draws only its first triangle and 4 triangles hang
(`parhelia-p690-q3-performance-analysis.md`).

### What the driver has to do

* **Never put two triangles that overlap in screen space in one draw packet.**
  `p3d_emit_draws()` emits `nprim` packets; a driver that cannot prove
  non-overlap should emit one primitive per packet, as the vendor does for
  22330 of its 31329 packets.
* **Cap the packets per submission.**  434 ring calls stalls the engine after
  257; `P3D_MAX_DRAWS` keeps a chunk to 180 draws (~214 calls).
* **Do not put two surfaces that overlap in screen space in the same
  submission.**  p3d_gears builds the mesh one face group at a time and gives
  each group its own submission, which is what makes the silhouette exact.

With all three, the three-gear scene agrees with the CPU reference on
**167163 of 167431 covered pixels** and 97.4% of the frame matches within
2/255, against 86% and a 16% facet-disagreement rate when batched.

### SOLVED: the loader's descriptor list has to be terminated

Each face group drawn alone, or any PAIR of them, was essentially exact - front
faces alone 29 pixels off, flanks alone 32, front+flanks 48, front+back 29 -
and only the full six-group sequence was wrong, at 9594.  Nothing was wrong
with any group's geometry or shading; state was leaking between submissions.

93.6% of the wrong pixels had a depth agreeing with the reference to within
64/2^24, so the same surface won and only its colour differed.  Recovering N.L
from the colour showed the reference always matching the geometric n.L while
the hardware used a value belonging to a **different normal** - 0.663 is
exactly (0,1,0).L, another tooth's flank normal.  The faces that went wrong
were precisely those whose neighbouring vertices carry different normals: the
front and back faces all share (0,0,+/-1), so picking up a neighbour's normal
there is invisible, while every tooth flank has its own.

The cause: the constant loader is fed through its own one-attribute descriptor
(`0x10b4 = 0`, `0x10b8 = 0x83c00000`), and that list was never closed.  A draw
descriptor list ends with a zero terminator; leaving the loader's open left
descriptor state behind, and the next submission's attribute fetch picked up
the wrong vertex's normal.  One more register write fixes it:

```
0x10b4 = 0
0x10b8 = 0x83c00000     one xyzw attribute, offset 0, last
0x10b8 = 0              terminator - without this the list stays open
```

| | pixels wrong |
|---|---|
| gear 0, list left open | 9594 |
| gear 0, list terminated | **45** |
| all three gears, terminated | **100 of 167431 covered (0.06%)** |

The three-gear scene now covers **167423 of the reference's 167431 pixels** -
eight apart - with **99.99% of the frame within 2/255**.

### A measurement trap worth recording

Three "the render drew nothing" results in a row were the test box's disk being
100% full: `nzdump` reported the correct pixel count and wrote an empty file.
Check `df` before believing a sudden regression.
