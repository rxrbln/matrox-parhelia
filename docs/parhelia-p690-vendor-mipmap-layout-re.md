# P690 vendor mipmap chain and probe

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note isolates the P690 evidence needed before enabling hardware mipmap
selection.  It answers the narrow question left by the first P690 probe:
whether a 64x64, 32-bpp, layout-2 texture has a 4 KiB or an 8 KiB level 1.

The answer from the saved Windows P690 driver is **4 KiB**.  Level 1 begins at
`base + 0x4000`, level 2 and the smaller levels share the block at
`base + 0x5000`, and the complete 64x64 through 1x1 allocation is `0x6000`
bytes.  The driver does not widen a 32x32 child to a 64-texel pitch for
allocation purposes.

## Sources and confidence

The package INF binds `mtxpserd.dll`/`mtxpserm.sys` to PCI device 102b:2539
and names its boards Millennium P690 (`mtxpser.inf` lines 44-50 and 197-203).
The binaries used here are:

```
52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a  MtxPser/mtxpserd.dll
da47324eae893160d0c1813984e1875a08a69fc72c47f123a666699e2d46b4b7  MtxPser/mtxpserm.sys
9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf  MtxPar/MTXPARD.dll
5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47  kernel/mtx_parhelia.o
```

Addresses below are image virtual addresses in `mtxpserd.dll`.  Statements
marked **P690 BIN** come directly from that device's Windows driver.  The 0528
driver and Linux HAL are comparisons, not substitutes for the P690 result.

## Exact P690 chain construction

The P690 2-D mip-chain constructor starts at `0xc6370`.  Its per-level loop is
`0xc642a` through `0xc6764`:

```
w_l = max(base_width  >> level, 1)
h_l = max(base_height >> level, 1)

if (w_l > 16 || h_l > 16):
        level.offset = next
        level descriptor = descriptor(w_l, h_l, ..., level_count-level)
        bytes = size(level descriptor, allocation_flag=0)
        total += bytes
        next  += bytes
else:
        level.offset = next                 /* unchanged for the whole tail */
        level descriptor = descriptor(32, 16, ...)
        level.tail_x = tail_x[tail]
        level.tail_y = tail_y[tail]
        if tail == 0:
                total += size(level descriptor, allocation_flag=0)
        tail++
```

The evidence for the running offset is direct:

* `0xc66d7` stores the current offset in the large-level object at `+0x1fc`.
* `0xc6737` calls the size helper `0xa5670`; `0xc6740` adds the result to the
  total and `0xc6748` calls it again; `0xc674d` adds it to the next offset.
* `0xc6528` stores the same current offset in a tail-level object.  The tail
  path never changes that offset.
* `0xc65be` tests the tail slot.  Only slot zero calls `0xa5670` and adds one
  block to the total at `0xc6607`; later tail slots add no allocation bytes.

The final parent is built at `0xc6822` through `0xc687b`.  It passes the base
width and height and `level_count - 1` to the descriptor builder, then stores
the computed whole-chain size.  Child objects retain their offsets for upload
and surface views; they are not additional sampler base registers.  This is
the Windows-side counterpart of the Linux HAL's single-base chain.

### P690 level-size helper

`0xa5670` jumps to `0xa5590`.  For non-linear layouts that routine performs:

```
bpp    = descriptor_format_bpp()
width  = descriptor_word_at_+4 & 0x3fff
height = descriptor_word_at_+8 & 0x3fff
A      = per_level_alignment(descriptor, flag=0)
Ha     = height_granularity(descriptor)

bytes = align(align(width, 32) * align(height, Ha) * bpp / 8 * depth, A)
```

The two dimension names reflect their positions in the internal descriptor;
for an ordinary 2-D layout-2 image they produce the familiar 32-texel by
32-row allocation quantum.  The relevant instructions are:

* `0xa561a` masks the height to 14 bits and `0xa5620` aligns it to `Ha`.
* `0xa562c` masks the width and `0xa5632`-`0xa5635` rounds it to 32.
* `0xa5638`-`0xa5647` multiply dimensions, bpp and depth.
* `0xa564a`-`0xa5654` align the byte count to `A`.

For local-memory layout 2/3 at 32 bpp, `0xa5330` is the alignment selector.
Its 32-bpp branch at `0xa5376`-`0xa5393` returns `A=0x1000` when called with
the per-level flag and `0x2000` for the final allocation.  `0xa54c0` returns
the 32-row granularity for this case.  These routines are instruction-for-
instruction equivalents of the MTXPARD helpers, relocated within MtxPser.

Therefore:

| level | logical size | offset | allocated bytes |
|---:|---:|---:|---:|
| 0 | 64x64 | `0x0000` | `0x4000` |
| 1 | 32x32 | `0x4000` | **`0x1000`** |
| 2 | 16x16 | `0x5000` | `0x1000` shared tail block |
| 3 | 8x8 | `0x5000` | 0 additional |
| 4 | 4x4 | `0x5000` | 0 additional |
| 5 | 2x2 | `0x5000` | 0 additional |
| 6 | 1x1 | `0x5000` | 0 additional |
| | whole allocation | | **`0x6000`** after allocation alignment |

This settles the allocation question independently of any assumed CPU
swizzle.  In particular, an open-driver layout that assigns 32x32 level 1 a
64-texel pitch and 32 rows (`0x2000` bytes) does not match the P690 vendor
surface builder.

### Tail placement is unchanged

The P690 tables are present verbatim at file offsets `0x18e918`, `0x18e92c`
and `0x18e940`, or image addresses `0x19e918`, `0x19e92c`, `0x19e940`:

```
tail_x     = { 0, 16, 24,  16,  24 }
tail_y     = { 0,  0,  0,   8,   8 }
texelIndex = { 0, 16, 24, 272, 280 }
```

The constructor reads them at `0xc6544`, `0xc6551`, and `0xc65a1`.
`texelIndex == tail_y * 32 + tail_x`.  MTXPARD has the same tables at file
offsets `0x18c918`, `0x18c92c`, and `0x18c940`; the corresponding constructor
reads them at `0xc4e34`, `0xc4e41`, and `0xc4e91`.  This is strong evidence
that P690 preserves both the shared-tail contract and its logical coordinates.

## Descriptor and sampler state

The parent descriptor uses one local-memory byte base in `0x11dc`, the base
width/layout/format in `0x11e0`, and the base height/depth plus maximum level
in `0x11e4`.  For the two-level test below:

```
0x11dc = base                         /* low pool bits zero for local VRAM */
0x11e0 = 0x32020040                   /* A8R8G8B8, layout 2, width 64 */
0x11e4 = 0x10010040                   /* max level 1, depth 1, height 64 */
```

No child address, mip stride, or derivative register is emitted.  The child
objects and `+0x1fc` offsets are used by the driver to upload/view a level; the
parent descriptor presents the chain to the sampler from one base.

The P690 filter builder at `0x17c2aa` through `0x17c339` is also instruction-
equivalent to MTXPARD's builder at `0x17a2ea` through `0x17a379`.  Its P690
translation tables are:

```
0x1d6558  MIN = { 0, 0, 2, 3, 2, 2 }
0x1d6570  MAG = { 0, 0, 2, 3, 2, 2 }
0x1d6588  MIP = { 0, 1, 2, 2, 2, 2 }
```

The emitter installs `MIP & 3` at filter bits `[7:6]`, MAG at `[4:3]`, and MIN
at `[1:0]`.  Thus MIP nearest remains code 1 and the established register is:

```
0x11c0 = 0x01c01040                   /* nearest/min, nearest/mip */
```

The LOD is derived by the texture unit from interpolated coordinates.  The
vendor P690 binary contains no evidence for an extra P690 derivative-enable
bit or a different mip-enable bit.  LOD bias remains in `0x11c0[24:16]`; LOD
minimum and maximum remain unsigned 4.4 in `0x11c4[23:16]` and `[31:24]`.
`0x11c4=0x10100000` clamps both to level 1.

## What differs from MGA 0528

The saved P690 D3D driver retains the 0528 chain arithmetic, tail constants,
descriptor fields, and filter encoding.  This is positive evidence against
inventing a P690-specific 8 KiB child or another mip register.

P690 does differ in two independently measured areas that must not be folded
into the mip contract:

1. Live P690 sampling shows layout code 2 using the chip's 64x16/4-KiB native
   permutation at 64x64.  The exact within-block permutation for a 32x32 child
   is not recovered here.  The vendor allocation nevertheless proves that the
   child occupies one 4-KiB block.  A compact/small-level interpretation must
   exist in the P690 hardware or its upload path.
2. The safe P690 one-texture route is `0x1200=0x00040101`.  The first mip probe
   accidentally used the multi-pipe value `0x00040103`, a route already known
   to stop retirement on P690.  That timeout is not evidence against the
   descriptor, offset, filter, or max-level fields.

The first probe also bypassed the currently proven P690 texture allocation and
upload path.  A mip result should use `PARHELIA_CREATE_RENDER_TEXTURE` plus the
kernel upload ioctl, in one fresh DRM client, before changing sampler fields.

## Safest discriminating probe

Use a private `64x96` P690 texture allocation.  At a 256-byte physical row it
is `0x6000` bytes, large enough for both the vendor 4-KiB level 1 and the
competing 8-KiB interpretation.  Upload these raw regions through the proven
texture upload ioctl:

```
0x0000..0x3fff  solid red             /* level 0 */
0x4000..0x4fff  solid green           /* vendor's complete level 1 block */
0x5000..0x5fff  solid blue            /* bytes touched only by 8-KiB theory */
```

A solid 4-KiB block deliberately avoids assuming the unresolved 32x32
within-block permutation.  Render the already-proven textured quad with strong
minification, route `0x00040101`, clamp LOD to 1 with `0x10100000`, and run
these cases in order:

| case | maximum level | mip filter | expected result |
|---|---:|---:|---|
| control A | 0 | none | red |
| control B | 1 | none | red |
| control C | 0 | nearest, LOD clamp 1 | red (clamped to available level 0) |
| discriminant | 1 | nearest, LOD clamp 1 | green only for vendor 4-KiB child; green/blue use for 8-KiB child |

Change only the listed sampler field between cases and wait for normal
retirement after each draw.  If A-C do not retire and render red, stop: that
failure is outside mip addressing.  If the discriminant is green, implement
the vendor `0x4000`/`0x1000` child and shared tail.  If blue appears, preserve
the capture because it would be the first evidence that silicon's small-level
walk differs from the P690 vendor allocator.

After the forced-level test passes, reopen the LOD range (`0x11c4` high word
`0xf000`) and test derivative-selected levels and trilinear filtering.  Keeping
that as a later test separates physical chain addressing from LOD calculation.
