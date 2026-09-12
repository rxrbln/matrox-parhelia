# P690 Q3 `GL_RGB5_A1` sampled-alpha route

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The retained binaries now establish the native texture format join that was
missing from the earlier audit:

```
D3DFMT_A1R5G5B5 25 -> internal format 0x08 -> P3D surface format 0x26
```

This is sufficient to build isolated open-driver probes.  The first P690
pixel result showed that the original linear/MODULATE experiment still read a
zero alpha bit as opaque, so the normal Mesa path must remain unchanged while
the remaining vendor-observed state differences are tested.

The measured result was:

```
FAIL texA0=0x00fe00 texA1=0xff0000 vertexA0=0x204060 untouched=background
```

RGB fetch, blending, vertex alpha, fencing and the untouched control all
worked.  The failure is specifically sampled alpha in that state image.

## Q3 input and fixed-function state

The retained working P690 ioquake3 trace is
`captures/ioq3-p690-working-draw.txt`, SHA-256
`4ade3269b3b6d482dd38c32778ef26dd5759cbaf9e34a05faef85168b2864768`.
Lines 321--327 record:

```
GL glTexImage2D target=0xde1 level=0 ifmt=0x8051 512x256 fmt=0x1908 type=0x1401
GL glEnable cap=0xbe2
GL glBlendFunc src=0x302 dst=0x303
```

Those tokens are `GL_RGB5_A1`, a four-component `GL_RGBA` upload,
`GL_BLEND`, and `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`.

At state-submit 5, key `0x0e`, stage zero requests:

| field | value | meaning |
|---|---:|---|
| `COLOROP` | 4 | `MODULATE` |
| `COLORARG1` | 2 | `TEXTURE` |
| `COLORARG2` | 0 | `DIFFUSE` |
| `ALPHAOP` | 2 | `SELECTARG1` |
| `ALPHAARG1` | 2 | `TEXTURE` |
| `ALPHAARG2` | 0 | `DIFFUSE` |

The original recovered open packet computes `RGB = texture * diffuse` and
`A = texture.A * diffuse.A`, which is identical for Q3's vertex alpha of one
and also provides an independent vertex-alpha-zero control.  Q3's retained
stage record asks for `ALPHAOP=SELECTARG1(TEXTURE)` instead.  The recovered
instruction for that exact operation is `MOV_sat r0.a,t0.a`, encoded as
`0x1260=0x00000000`, `0x1268=0x01400400`.  The new probe matrix tests both
forms without changing production state.

## Exact format proof

### Windows descriptor identity

In `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`:

* `0x83af0` maps D3D format 25 (`A1R5G5B5`) to internal ID `0x08` at
  `0x83b4d`.
* `0x855bc` builds the colour-format objects.  The path at `0x8574f` clones
  the 16-bit base descriptor as ID `0x08`.
* The common mask builder `0x84aa0` gives ID `0x08` the masks
  `A=0x8000`, `R=0x7c00`, `G=0x03e0`, and `B=0x001f`.

This proves the application's format identity and packed bit layout.

### Linux internal-ID to hardware-code join

The saved unstripped Linux vendor object
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` retains symbols.
`mtx1646` at `0x7c050` constructs the runtime format table by calling
`mtx1179` at `0x7bf60`.  The constructor stores:

```
[record+0x04] = internal format ID (RSI)
[record+0x08] = surface view       (EDX)
[record+0x0c] = hardware code      (ECX)
```

The ID-`0x08` records are byte-explicit in `mtx1646`:

| call site | view (`EDX`) | hardware code (`ECX`) |
|---:|---:|---:|
| `0x7cc2c` | 2 | `0x26` |
| `0x7cc7c` | 3 | `0x26` |
| `0x7cccc` | 1 | `0x26` |
| `0x7cd1c` | 5 | `0x26` |
| `0x7cd69` | 0 | `0x20` |
| `0x7cdb9` | 7 | `0x26` |
| `0x7ce09` | 6 | `0x26` |

All ordinary views use `0x26`; only special view zero uses `0x20`.  This is a
direct mapping, not a guess from bits per texel or a display format.

### Which view the sampler actually selects

The later Linux sampler-variant selector `mtx2577` at `0x70eb0` removes the
remaining ambiguity.  It first tests the format pointer at object offset
`+0x30`, which `mtx2373` populated with **view 3**, then falls back to object
offset `+0x28`, **view 2**.  It never consults view 0.  Both selected records
carry code `0x26` for internal format 8, so replacing the sampler descriptor
with view-0 code `0x20` is ruled out by the vendor implementation.

### No hidden alpha-present descriptor bit

`mtx3424` at `0x1eab0` constructs all nine surface descriptors.  At
`0x1ec67` it copies `record+0x0c & 0x3f` into descriptor control bits
`[29:24]`.  It then copies `record+0x10 bit 0` and `record+0x11 bit 0` into
control bits 30 and 31.  The ID-8/view-2 and ID-8/view-3 constructor calls set
both record bytes to zero.  Thus the complete vendor format/control prefix is
`0x26`, with neither high bit set.  There is no omitted alpha-present bit in
`0x11e0` for this format.

This also explains why the A/X distinction belongs in the six-bit format
code: A1R5G5B5 uses `0x26`, while X1R5G5B5 uses `0x27` in these sampler views.

## Exact probe packet

The baseline standalone command in Mesa's `parhelia-drm-test` uses a 64x64
linear A1R5G5B5 texture.  Its required state is checked in the CLI path and in
the packet-only self-test:

| register | value | purpose |
|---:|---:|---|
| `0x1200` | `0x00040101` | P690-safe unit 0 / physical pipe 0 |
| `0x11e0` | `0x26000040` | format `0x26`, linear, pitch 64 texels |
| `0x11e4` | `0x00010040` | one 64-row 2D image |
| `0x11e8` | `0x00500000` | vendor one-unit texture-cache partition |
| `0x1260` | `0x08000000` | final alpha MUL inputs, texture and diffuse |
| `0x1268` | `0x01400000` | saturated alpha MUL operation |
| `0x12e8` | `0x0080eced` | source-alpha / inverse-source-alpha ADD |

The independent cutout form can use
`0x12e4 = 0x00008009` (`GL_GREATER`, reference `0x80`).  No format-specific
alpha enable was found in either vendor path: alpha originates in format
`0x26`, flows through the ordinary texture-address result and alpha combiner,
then reaches alpha test or blending.

The additional CLI command tests the three remaining exact combinations:

| variant | `0x11e0` | final alpha instruction | reason |
|---|---:|---|---|
| layout2/MODULATE | `0x26020040` | `MUL_sat r0.a,t0.a,v0.a` | isolates the vendor texture layout |
| linear/SELECTARG1 | `0x26000040` | `MOV_sat r0.a,t0.a` | isolates Q3's alpha operation |
| layout2/SELECTARG1 | `0x26020040` | `MOV_sat r0.a,t0.a` | exact conjunction used by the retained Q3 route |

Layout-2 pixels are packed with the recovered 16-bit texture swizzle
`p3d_texture_tile_offset_cpp(..., 2)`.  The allocation carries
`PARHELIA_CREATE_RENDER_TEXTURE`, while the linear control retains
`PARHELIA_CREATE_RENDER_LINEAR`.  Every variant remains a private offscreen
GEM draw with the same fence timeout and P690 gates.

Run the offscreen probe as:

```
parhelia-drm-test --p690-rgb5a1-alpha-probe /dev/dri/cardN
parhelia-drm-test --p690-rgb5a1-alpha-variants /dev/dri/cardN
```

It performs three separately cleared, fenced draws:

1. texel `0x03e0` (transparent green), vertex alpha one: the clear colour
   must remain;
2. texel `0xfc00` (opaque red), vertex alpha one: the sample must become red;
3. texel `0xfc00`, vertex alpha zero: the clear colour must remain for
   MODULATE and must become red for SELECTARG1, which deliberately ignores
   diffuse alpha.

It also checks an untouched sample each pass.  Every texture allocation uses a
direct `DRM_IOCTL_PARHELIA_CREATE_RENDER` request.  The linear cases carry
`PARHELIA_CREATE_RENDER_LINEAR`; the layout-2 cases carry
`PARHELIA_CREATE_RENDER_TEXTURE`.  A direct request matters because the
current `parhelia_create_render()` helper rebuilds its argument and drops
pre-filled flags.

The command is restricted to PCI ID `0x2539`, P690 single-pipe routing, a
valid textured WARP entry, and async fence support.  It renders only into a
private offscreen GEM and does not advertise a Gallium capability.

## Promotion condition

The packet self-test proves composition, exact variant deltas and relocation
shape.  A production native format route still requires a successful P690
result and regression coverage for upload pitch, subimage writes, mip levels,
and both alpha-test and blend consumers.  Do not substitute the failed
A8R8G8B8 sampled-alpha result for this native-format experiment, and do not
use the P690-unsafe paired-pipe value `0x00040103`.

## The storage-order defect this route carried (fixed 2026-09-12)

The repack that feeds this route wrote the vendor's **generic** 16-bit texel
order after the descriptor's storage order for a two-byte texel became 4, the
order measured on this chip, when native 8- and 16-bit sampler storage landed.
The two orders share only three of their address bits.

On a small image that is not a scramble but a **missing half**: the generic
order puts the top Y bit where the unit reads its own x bit 5, so the rows 8-15
of a 16x16 image went where a 16-wide texture is never read.  User-visible as
the ioquake3 menu cursor drawn top-half only, and as missing lines or an
apparent Y scaling on larger decals and HUD elements.  It is
format-independent, which is why it looked identical at colour depth 24 and 16
and why the 16-bit render path was wrongly suspected first.

Fixed by writing the repack in the order its own descriptor selects
(Mesa `6664797`).  Verified on hardware at depth 24 with
`tools/parhelia-gl-q3formats-test.c`: a 16x16 image went from 240 of 256
texels wrong to 0, a 128x128 from 16128 of 16384 to 0, and every
`RGBA_OVER` shape from 8x8 to 256x256 now passes for a full upload, both
filters, all six sub-image shapes, every mip level and every base level.

## Open gaps in the same sweep

* A 640x480 image fails every check while `max_texture_2d_size` advertises
  2048.  The sampler extent measured on this chip is 512 texels, so either the
  cap is wrong or large images need a different path; the Q3 atlases are all
  16..512 and never hit it.
* Bilinear filtering at 1:1 on an image with a non-power-of-two width or an
  odd height misses one or two texels at the last column or row.  Every
  point-sampled case of the same shapes is exact, so this is a filter-edge
  question, not the layout - the chain itself now reads back byte exact for
  121 of 121 probed levels (`parhelia-p690-mip-chain-half-tiles.md`).
