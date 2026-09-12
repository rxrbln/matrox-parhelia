# P690 mipmap probe status

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

The Parhelia-512/0528 vendor chain layout is decoded in
`parhelia-texture-sampling.md`, including the filter encoding, level packing,
and 16x16 mip tail.  It is not sufficient evidence to enable mipmapping on a
P690.

## Isolated P690 result

`parhelia-drm-test --mipmap-probe` allocated a private 64x96 VRAM block and
placed the proven two-level code-2 chain in it:

| level | dimensions | byte offset | colour |
|---|---:|---:|---|
| 0 | 64x64 | `0x0000` | opaque red |
| 1 | 32x32 | `0x4000` | opaque green |

The emitted sampler state was deliberately minimal:

```
0x1200 = 0x00040101        P690's proven single texture pipe
0x11e0 = 0x32020040        A8R8G8B8, layout 2, width 64
0x11e4 = 0x10010040        height 64, maximum level 1
0x11c0 = 0x01c01040        nearest min/mag, mip-nearest
```

A textured private triangle used coordinates from zero to 64, so a working
mip-nearest sampler had to select the green child.  The target returned
`mip probe submit: Connection timed out`; its previous kernel session recorded
the standard submit timeout/wedged event.  NanoKVM reset and recovery restored
Q3.  This was not a visual failure that can be hidden by falling back to level
zero.

## What the result establishes

The same test uses the established P690 single-pipe state, code-2 base texture
format, private render allocation, triangle path, and fence mechanism.  The
new inputs are the level-one data, `maximum level = 1`, and the mip-nearest
filter field.  A bad offset or tail placement would ordinarily select the
wrong colour; a fence that never retires instead shows that this P690 command
combination reaches an unrecovered execution precondition.

The 0528 chain geometry and packet fields remain useful reverse-engineering
evidence, but are not a P690 enablement recipe.  The unresolved prerequisite
is likely a P690-only sampler/context register or texture-surface allocation
class installed by the vendor stack.  Capture and compare a same-chip vendor
mip draw, including all state before the texture descriptor and the allocation
ioctls, before any further hardware test.  Do not retry variations of this
packet on the live target.

## Driver policy

Mesa leaves the P690 sampler at `MIP_NONE`, maximum level zero, and makes
lower image transfer maps staging-only.  It does not reserve expanded mip
allocations for normal textures.  The reusable layout and packet selftest stay
in-tree because they exactly describe the 0528/vendor contract, but no normal
application can enable the P690 path.

The probe has a second explicit safety latch:

```
PARHELIA_ACK_UNSAFE_MIP_PROBE=1 parhelia-drm-test --mipmap-probe /dev/dri/renderD128
```

Do not use it on a live desktop.  Recover the missing P690 context or
texture-address state from a same-chip vendor capture before retesting.
