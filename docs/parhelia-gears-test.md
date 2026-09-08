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

## Batching a draw packet breaks the depth interlock

Every one of the 1235 draw packets in `captures/gl-matrix/depth-secs.txt` covers
exactly **three vertices**.  That is not the vendor being naive: the hardware
does accept a larger vertex range - two disjoint triangles in one packet both
draw - but the depth test between triangles inside one packet is not honoured.

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

### What the driver has to do

* **One draw packet per primitive**, as the vendor does.  `p3d_emit_draws()`
  emits `nprim` packets of three vertices each.
* **Cap the packets per submission.**  434 ring calls stalls the engine after
  257; `P3D_MAX_DRAWS` keeps a chunk to 180 draws (~214 calls).
* **Do not put two surfaces that overlap in screen space in the same
  submission.**  p3d_gears builds the mesh one face group at a time and gives
  each group its own submission, which is what makes the silhouette exact.

With all three, the three-gear scene agrees with the CPU reference on
**167163 of 167431 covered pixels** and 97.4% of the frame matches within
2/255, against 86% and a 16% facet-disagreement rate when batched.

### Still open: the shading degrades as submissions accumulate

Each face group drawn on its own, or any PAIR of them, is essentially exact:

| submitted | pixels wrong vs the reference |
|---|---|
| front faces alone | 29 |
| tooth flanks alone | 32 |
| front faces + flanks (2 submissions) | 48 |
| front faces + back faces (2 submissions) | 29 |
| **all six face groups (6 submissions)** | **9594** |

So neither the geometry nor the shading of any group is wrong - they are right
when drawn alone and right in pairs.  Something accumulates across submissions.

What the wrong pixels are is pinned down: 93.6% of them have a depth that
agrees with the reference to within 64/2^24, so the SAME surface won and only
its colour differs.  Recovering N.L from the colour shows the reference always
matches the geometric n.L (0.109 against 0.107, 0.447 against 0.449, ...) while
the hardware uses a value belonging to a different normal - 0.663 is exactly
(0,1,0).L, another tooth's flank normal.  The faces that go wrong are precisely
those whose neighbouring vertices carry DIFFERENT normals: the front and back
faces all share (0,0,+/-1), so fetching a neighbour's normal there is
invisible, while every flank has its own.

Ruled out by direct measurement: culling (proven honoured - culling the
opposite face changes the image completely), winding, depth precision (24-bit
quantisation of the reference, per vertex or per pixel, moves the count by one
pixel), the normal's sign (normal (0,0,-1) against light (0,0,-1) gives full
brightness), and a one-pixel coverage shift (explains only 9%).
