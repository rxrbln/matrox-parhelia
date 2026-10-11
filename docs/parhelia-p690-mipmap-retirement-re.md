# P690 mipmap retirement: vendor evidence and controlled fix order

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note isolates the P690 mipmap timeout from the mip-chain questions that
can only affect sampled data.  It compares the saved Matrox Linux and Windows
drivers, the 0528 command captures, and the current DRM test and Mesa code.
No vendor driver was executed and no hardware was touched for this analysis.

## Result

The stopped-retiring result is not yet evidence that P690 rejects the decoded
mipmap descriptor.  The `mipmap_probe()` source inspected before the current
fix did not initialize `p3d_state.single_texture_pipe`.  The reference emitter
therefore selected its older-chip default:

```
0x1200 = 0x00040103             unit 0 owns physical pipes 0 and 1
```

That value is independently known to wedge the P690.  The safe P690 route is:

```
0x1200 = 0x00040101             unit 0 owns physical pipe 0
```

The earlier `parhelia-p690-mipmap-probe.md` says that the timed-out packet
contained `0x00040101`, but it retains no packet dump.  That statement conflicts
with the then-current C initializer.  The current source now explicitly sets
`single_texture_pipe = true`; packet inspection must establish which source
actually produced any future hardware result.

There is a second, independent mismatch.  The probe allocates a normal render
BO, mmaps it, and fills both levels with `p3d_texture_tile_offset()`, the
original 32x16 texture permutation.  The working production P690 texture path
uses `PARHELIA_CREATE_RENDER_TEXTURE`, `DRM_IOCTL_PARHELIA_UPLOAD_RENDER`, and
the P690 64x16/4 KiB permutation for its base image.  This mismatch need not
cause a fence timeout, but it makes any sampled color inconclusive.

## Exact vendor sampler state

The controlled 0528 captures in `captures/gl-matrix` give the complete draw
deltas.  No mip-specific WARP program, extra base address, or level-offset
register appears.

| state | `0x1200` | `0x11c0` | `0x11e0` | `0x11e4` |
|---|---:|---:|---:|---:|
| base nearest | `00040103` | `01c01000` | `32020002` | `00010002` |
| 32x32 mip-nearest chain | `00040103` | `01c01042` | `32020020` | `50010020` |
| 32x32 trilinear chain | `0004010f` | `01c01082` | `32020020` | `50010020` |

The mip-nearest draw retains the normal two-pipe route.  Trilinear changes
only the filter and assigns all four old-chip pipes with `0x0004010f`.  Neither
old-chip route is a P690 candidate.  P690 one-pipe ownership must remain
`0x00040101`; a later trilinear experiment must not copy `0x0004010f` until a
same-chip route is proven.

The filter packing recovered independently from Linux `mtx_parhelia.o` and
Windows `MTXPARD.dll` is:

```
0x11c0 = bias_and_rest | MIN[1:0] | MAG[4:3] | MIP[7:6]
bias_and_rest at zero GL bias = 0x01c01000
MIP_NONE = 0, MIP_NEAREST = 1, MIP_LINEAR = 2
```

Thus nearest image filtering plus mip-nearest is `0x01c01040`, and Quake 3's
usual `GL_LINEAR_MIPMAP_NEAREST` plus linear magnification is `0x01c01052`.
The signed 4.4 LOD bias occupies `0x11c0[24:16]` after the hardware's -4.0
encoding offset; zero bias is the field value `0x1c0`.  `0x11c4[23:16]` is the
unsigned 4.4 minimum LOD and `[31:24]` is maximum LOD.  The ordinary
`0xf0000000` means 0.0 through 15.0, so it does not clamp the test to level 0.
The maximum available level is `0x11e4[31:28]`.

The P-series Windows driver confirms that these fields did not change for the
later chip family.  `MtxPser/mtxpserd.dll` has the same filter builder as
`MTXPARD.dll` modulo its code/data displacement: its tables are at
`0x1d6558`, `0x1d6570`, and `0x1d6588`, and the packer is
`0x17c2aa..0x17c339`.  The MIN/MAG/MIP packing and LOD computation are
instruction-identical.  There is no P690 derivative-enable bit in this path.

## Exact chain allocation and the remaining P690 address question

The saved Linux driver supports PCI device `0x2539` (`mtx2539`) and uses its
common texture-chain builder for this operation.  The independent P690 Windows
path agrees exactly: `mtxpserd.dll`'s mip builder at `0xc639d..0xc6975` calls
the per-level helpers at `0xa5670` and `0xa5590`.  Those helpers are
opcode-identical to the MTXPARD versions modulo the P-series displacement.
For local tiled type 2/3 and 32-bit texels both paths compute
`align(w,32) * align(h,32) * 4`, rounded to 4 KiB.  Allocation ends on an 8 KiB
boundary.  The P690 tail X/Y/index tables are also byte-identical: levels at or
below 16x16 share one 32x16 tail block with positions `(0,0)`, `(16,0)`,
`(24,0)`, `(16,8)`, and `(24,8)`.

For the probe's 64x64 chain the exact vendor geometry is:

| level | logical size | logical pitch | offset | reserved bytes |
|---|---:|---:|---:|---:|
| 0 | 64x64 | 64 | `0x0000` | `0x4000` |
| 1 | 32x32 | 32 | `0x4000` | `0x1000` |
| 2..6 tail, if present | 16x16..1x1 | 32 | `0x5000` | `0x1000` shared |
| allocation | | | | `0x6000` |

The hardware walks this chain from the one base in `0x11dc`.  No per-level
TEXORG exists.  `0x11e0` carries the base width, layout, and format, and
`0x11e4` carries base height and maximum level.

The exact within-level permutation for a P690 32x32 child remains unproven.
Live base-image testing established that P690 layout code 2 uses the native
64x16/4 KiB permutation for 32-bit texels.  That test covered a 64-pixel-wide
base.  The P690 Windows builder proves that its 32x32 child still occupies only
one 4 KiB area; a 32 texel logical pitch cannot be passed to the current native
helper, which divides the pitch by 64.  The silicon therefore packs the child
with an implicit sub-64 permutation that the retained evidence does not yet
identify.  Neither the saved 0528 captures nor the retained P690 base-level
tests expose those address bits.

This distinction is unnecessary for the first retirement test.  A solid
color can fill whole allocation bands without knowing the permutation:

```
[0x0000, 0x4000) = opaque red
[0x4000, 0x5000) = opaque green
[0x5000, 0x6000) = opaque blue guard/tail
```

The green band is exactly the vendor's 4 KiB level-1 allocation.  The blue
band keeps an erroneous larger walk in bounds and distinguishes it from the
decoded child.  A coordinate ramp should recover the child permutation only
after this packet retires.

## Current code comparison

`parhelia_mipmap.h` correctly reproduces the saved Linux chain geometry for
the older texture path: the probe gets `level[1].offset = 0x4000`, pitch 32,
and allocation size `0x6000`.  Its tail table also matches the binary.

The remaining current-code differences are concrete:

* `mipmap_probe()` fills every level through `mip_texture_offset()`, which
  always calls the old 32x16 `p3d_texture_tile_offset()`.
* It creates a normal 64x96 render object and writes through an mmap.  P690's
  tested sampler-storage ABI is `CREATE_RENDER_TEXTURE` plus `UPLOAD_RENDER`.
* Mesa forces `mip_levels = 1` in resource creation, and `ph_use_mips()` always
  returns false.  Lower-level maps therefore cannot reach the sampled BO.
* The production base-image path sets `native_64x16_32`, widens its pitch to at
  least 64, and uses the native P690 permutation.  Extending that rule blindly
  to level 1 is invalid because the decoded child has logical pitch 32.
* `p3d_state.c` otherwise emits the recovered descriptor, filter, LOD clamp,
  texture bank selection, and textured WARP entry.  The vendor mip captures
  select no different WARP program.

## Minimal deltas and validation order

Do the next target work in this order.  Each step preserves the known P690
one-pipe route and changes one class of input.

1. Add a packet-only assertion that the mip probe contains
   `0x1200=0x00040101`, `0x11e0=0x32020040`,
   `0x11e4=0x10010040`, `0x11c0=0x01c01040`, and
   `0x11c4=0xf0000000`.  Reject the run if `0x00040103` or `0x0004010f`
   occurs anywhere in its texture-enable writes.
2. Allocate 64x96 with `PARHELIA_CREATE_RENDER_TEXTURE`; require a 256-byte
   pitch, at least `0x6000` bytes, no mmap offset, and the advertised upload
   capability.  Upload the red/green allocation bands above in one
   `UPLOAD_RENDER` call.
3. Run the two harmless controls first on the same BO and known-good triangle:
   `MIP_NONE/max_level=1`, then `MIP_NEAREST/max_level=0`.  Both must retire and
   sample red.  Use a timeout and stop after any non-retiring submit.
4. Run `MIP_NEAREST/max_level=1`.  A retiring green result proves the sampler
   descriptor and level offset without assuming the level-1 swizzle.  A red
   result isolates LOD selection; a timeout then points beyond data layout
   because the whole bounded child region is valid.
5. After a green result, use a 32x32 coordinate ramp and compare a small set of
   candidate permutations.  Test the binary-derived one-4-KiB child first.
   Do not alter routing, filter, WARP, or descriptor while recovering it.
6. Only then restore real resource level counts and enable mip-nearest in Mesa.
   Require contiguous defined levels.  Keep trilinear disabled until the
   one-pipe P690 behavior is tested independently; do not import the 0528
   `0x0004010f` route.
7. Validate Quake 3 with its default `GL_LINEAR_MIPMAP_NEAREST`, then trilinear,
   and finish with the standard no-vsync `glxgears` correctness/performance
   regression.  The mip test must run offscreen with no unrelated X clients.

The minimal immediate fix is therefore sampler ownership plus representative
P690 texture allocation/upload.  No new WARP state, LOD bias, or hidden level
base should be added from speculation.

## Evidence identity

The Windows binaries used here are:

```
9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf  MtxPar/MTXPARD.dll
52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a  MtxPser/mtxpserd.dll
db0ab86fd25fe2cfb37bae5252d825aac8a2cc16d88191dedb165d4ba57dbcf0  MtxPser/mtxpsogl.dll
```

The detailed Linux chain-builder addresses, tail tables, upload records, and
LOD derivation are retained in `parhelia-texture-sampling.md`.  The P690 upload
allocation and aperture path is retained in
`parhelia-p690-texture-upload-aperture-alias.md`.
