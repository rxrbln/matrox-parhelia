# P690 RGB-companion alpha cutout

Copyright 2026 Rene Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

P690's broken sampled-alpha path can be bypassed without CPU rasterization and
without a depth or colour shadow.  Store source alpha in the red channel of an
ordinary opaque A8R8G8B8 companion texture, sample the original and companion
on the P690-proven two-unit route, and explicitly replicate `t1.r` into the
fragment alpha result.  The normal hardware alpha comparator then rejects
transparent fragments before their colour and depth writes.

This route is materially different from the earlier A8-companion probe.  That
probe used `t1.a`, the channel P690 fails to deliver.  It never exercised the
fragment alpha instruction's independently programmable RGB replicate field.

An offscreen probe is implemented as
`parhelia-drm-test --p690-rgb-companion-cutout-probe`.  Its packet construction
and local selftest pass.  Target results remain the boundary between the
binary-proven instruction encoding and production Mesa use.

## Vendor-binary evidence

The fragment instruction has separate RGB (`0x1264/0x126c`) and alpha
(`0x1260/0x1268`) halves.  Both halves have three component-replicate fields in
the destination/opcode word.

The saved Linux vendor object
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` proves that RGB
replication is accepted in the alpha instruction:

* `mtx2619+0x1a2..0x1b4`, VMA `0x6a3a2..0x6a3b4`, masks source-A replicate
  with `& 3` and inserts it in alpha destination bits `[1:0]`.
* `mtx2619+0x28d..0x2a2`, VMA `0x6a48d..0x6a4a2`, does the same for source B
  in bits `[4:3]`.
* `mtx2619+0x379..0x38d`, VMA `0x6a579..0x6a58d`, does the same for source C
  in bits `[7:6]`.
* `mtx530` at `0x691f0` emits this compiled half through `0x1260/0x1268` when
  the slot's separate-alpha flag is set.

The saved Windows D3D driver independently proves the same encoding:

* `MTXPARD.dll:0x183b2e..0x183b74` maps source swizzles `.r/.g/.b` to
  replicate codes `3/2/1`.
* `MTXPARD.dll:0x183cfa..0x183d0e` inserts source-A's component code in the
  low three bits of the instruction destination word.
* The ps.1.4 compiler uses the same code path for the separate alpha
  instruction, while the emitter at `0x170a10` stores that half in
  `0x1260/0x1268`.

Code 3 therefore means `.r` in the exact field the proposed packet changes.
This is compiler and emitter evidence, rather than an inferred unused bit.
The existing video colour-conversion builder also emits codes 3, 2 and 1 for
red, green and blue replication in successive fragment stages
(`mtx_drv.so` `BuildColorConvertShader` at `0x6dda1` and `0x6df03`).

P690 two-texture sampling is already hardware-proven with:

```
0x1200 = 0x00040101             unit 0 -> physical pipe 0
0x1204 = 0x00040104             unit 1 -> physical pipe 2
TA0: 0x1264 = 0x00004000        address texture 0, slot 0
     0x1260 = 0x00004000
     0x126c = 0x00000c00
     0x1268 = 0x00000c00
TA1: 0x1264 = 0x00004042        address physical pipe 2 to t1, slot 2
     0x1260 = 0x00004040
     0x126c = 0x00100c00
     0x1268 = 0x00100c00
```

The physical-pipe-2 TA source is the result-correlated P690 route documented
in `parhelia-multitexture.md` section 6.1.  The old `0x00040103` paired-pipe
state is not used.

## Exact cutout program

Unit 0 is the application's colour texture.  Unit 1 is a same-size companion
whose texels are `(A,A,A,255)`, using format `0x32`, layout 2, and the same
filter, wrap and mip selection as unit 0.  Duplicate the application's ST
coordinates into the second texture-coordinate attribute.

The first colour stage remains the normal fixed-function MODULATE:

```
slot 4 RGB:   0x1264 = 0x08000004
              0x126c = 0x01400000       MUL_sat r0.rgb, t0.rgb, v0.rgb
```

The final slot preserves RGB and replaces fragment alpha from companion red:

```
slot 8 RGB:   0x1264 = 0x00000088
              0x126c = 0x01400400       MOV_sat r0.rgb, r0.rgb
slot 8 alpha: 0x1260 = 0x00000020
              0x1268 = 0x01400403       MOV_sat r0.a, t1.r
```

`0x01400403` is the ordinary saturated MOV-to-r0 word `0x01400400` plus
source-A replicate code 3.  No guessed sampler or context bit is involved.

For Q3's `GEQUAL, 0.5` cutout, the comparator is:

```
0x12e4 = 0x0000800b             enable | GEQUAL(5)<<1 | 0x80<<8
```

Mesa must retain the application's depth function and write mask.  The probe
uses LESS specifically so an equal-Z replay demonstrates whether the accepted
fragment wrote depth:

```
0x12c8 = 0x001000e5             depth enable, LESS
0x12d0 = 0x000001ff             depth write enabled
```

Q3's traced LEQUAL state instead produces `0x12c8 = 0x001000e7` and must stay
LEQUAL in the production path.

## What the hardware probe establishes

The offscreen test performs three draws at Z=0.25 after clearing Z to FAR:

1. red source with companion RGB=0: colour must remain `0x204060`;
2. red source with companion RGB=255 at the same Z: colour must become red,
   proving the rejected first draw did not update Z;
3. green source with companion RGB=255 at equal Z: colour must remain red
   under LESS, proving the accepted second draw did update Z.

It also checks an untouched pixel and requires every async fence to retire.
The expected success line is:

```
PASS: P690 RGB-companion t1.r alpha cutout mask0=0x204060 \
mask255=0xff0000 equal-Z=0xff0000 untouched=0x204060 ...
```

## Why the current micro-cutout path stripes

`ph_stage_alpha_micro()` divides each original triangle into a fixed 16 by 16
grid.  It samples alpha once at each microtriangle centroid, then either emits
or drops the entire microtriangle.  The GPU still draws emitted geometry, but
the discard decision is constant over each relatively large triangle.  When a
small atlas glyph is magnified, the diagonal tessellation boundaries become
the visible stripes seen in the lower HUD blocks.  Increasing subdivision
only trades the artifact for more CPU sampling and hundreds of extra draws.

The RGB-companion path restores per-fragment filtered alpha and emits one draw
for the original primitive, so it removes both the stripe mechanism and its
submission amplification.

## Discard, stencil and coverage boundary

The existing P690 vertex-alpha test proves that a fragment rejected by
`0x12e4` does not update depth: an opaque replay at the same Z subsequently
passes LESS.  Once the probe confirms `t1.r` reaches that comparator, colour
and depth cutout are therefore hardware-native in one pass.

Stencil uses the same depth/stencil block (`0x12c8..0x12d0`) and should be
gated by the same fragment rejection, but no retained P690 result isolates an
alpha-rejected stencil operation.  It is unnecessary for this one-pass route
and should not be added as an intermediate mask.

The texture-address ISA has a `TEXKILL` opcode (`0x03`), but its threshold,
polarity side flag and final unit-control placement are not completely mapped
to a safe P690 packet.  It offers no advantage over the proven alpha
comparator and is not part of this implementation.

FAA coverage is a separate fragment-buffer path.  No vendor packet connects
sampled alpha or `0x12e4` to FAA coverage, and blending forces FAA resolves in
the Windows path.  Alpha-to-coverage must not be inferred from this result.

## Production integration boundary

After the offscreen probe passes, replace `alpha_micro_cutout` for the strict
Q3 state with one multitexture batch:

* reuse the already-lazy `alpha_rgb_handle` companion created by
  `ph_prepare_alpha_companions()`;
* duplicate ST into the two-coordinate vertex stream;
* bind original and companion through the P690 one-hot unit route;
* emit the four final slot-8 words above;
* retain the application's alpha comparison, colour mask, depth/stencil state,
  culling, scissor, and raster state;
* generate every defined mip level in the companion before generalizing
  beyond Q3's base-level-only cutout images.

This is one GPU draw with one extra texture fetch.  It requires no CPU
framebuffer access, no shadow target, no prepass, and no post-render resolve.
