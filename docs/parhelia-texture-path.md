# The Parhelia texture path, end to end

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

How a texture reaches VRAM and becomes sampled. From `mtx_parhelia.o`,
`libGL.so.1.3.0` and `MTXPARD.dll`.

## Correction first: the 9-dword array is NOT a sampler descriptor

`parhelia-record-stream.md` describes the key-0x46 blob's `item[375..410]` as
"4 x 9-dword texture-unit descriptors". **That is wrong.** It is the
**texture-address (TA) stage array** -- a 4-entry dependency graph of shader
t-ops. It carries no base, no format, no dimensions, no pitch, no wrap, no LOD:

    +0x00 internal opcode (0 = stage unused)   +0x18 sampling hint
    +0x04 performs a texture fetch             +0x1c texture type
    +0x08 nargs-1                              +0x20 texel format
    +0x0c..+0x14 args (arg[1] = source stage)

Real sampler state never appears in region 2 at all. The kernel composes it from
its own texture object and emits it as DMA packets.

## The 4 MB heap is not texture memory

It is a **CPU-written DMA staging ring** in system memory (memory type 4), a
bump-pointer allocator with per-8KiB-page frame fences. The client writes pixels
into it, then emits an upload record; the kernel **memcpys** out of it into the
real surface allocation (`mtx2222` @0x50fb0: clamp, map, `LocMemCpy`, unmap).

**There is no engine blit and no DMA descriptor for pixel data.** The only ring
traffic is a fence so the engine has released the surface first.

Upload record: **opcode 0x0a, element size 0x48**

    +0x00 u64 kernel ptr of the staging heap   +0x24 x1 = width
    +0x08 u64 byte offset of the pixels        +0x28 y1 = height
    +0x10 u64 destination surface id           +0x2c z1 = depth
    +0x18/1c/20 x0, y0, z0                     +0x30 mip level
    +0x38 cube face                            +0x40 source-format flag

There is no bytes-per-texel field; a previous reading of one, and of "three
1.0f" in a capture, were uninitialised ring bytes.

Surfaces are created by a **synchronous mailbox ioctl**, not a record:
`0x4621` with a 0x220-byte payload, **cmd 0x0c** = CreateSurface
`{flags, targetClass, formatId, w, h, d, levelCount}`, reply gives a surface id.
Buffers use **cmd 0x05**, whose reply carries the mmap offset.

## The sampler registers

The entire sampler window is **`0x11c0`-`0x11e8` plus `0x1200`-`0x120c`** -- a
sweep of every DMA-header immediate in the kernel finds no other texture
register. Emitted by `mtx1381` @0x3f680 as three packets from a 0x38-byte
per-unit block:

    A (any dirty)  0x1200+u*4, 0x11dc, 0x11e0, 0x11e4
    B (bit 1)      0x11c0, 0x11c4, 0x11d4, 0x11d8
    C (bit 2)      0x11cc, 0x11d0, 0x11c8, 0x11e8

### `0x1200 + unit*4` -- unit control

    [3:0]   texture-pipe one-hot; the union across active units must cover 0xf
    [11:8]  constant 1
    [18:16] coordinate mode / enable
    [23:19] HARDWARE TA OPCODE          <- not a filter code
    [24]    signed/bump texel format
    [27]    TEXKILL comparison sense

`[18:16]`: 0 dependent read, 1 disabled, 3 coordinate-only, 4/5 two-coordinate
(2D/1D, plain/projective), 6/7 three-coordinate (cube/3D).

### `0x11c0` -- wrap **and LOD bias**

    [1:0] U wrap   [4:3] V wrap   [7:6] W wrap   [12] always 1
    [24:16] 9-bit LOD bias, 4.4 fixed point with a -4.0 bias, clamped +/-4.0

### `0x11c4` -- filter **and LOD clamps**

    [2:0] MAG   [5:3] MIN   [8:6] MIP        <- that field order, settled
    [9]   border-colour enable
    [23:16] LOD min clamp   [31:24] LOD max clamp   (8-bit, 4 fractional bits)

The reset default `0xff000000` is exactly max LOD = 15.9375, which cross-checks
the scale.

### `0x11dc` / `0x11e0` / `0x11e4` -- the surface

    0x11dc  [31:2] byte offset in pool, [1:0] ADDRESS SPACE (0 = local FB, 2/3 = AGP/system)
    0x11e0  [13:0] width in texels, rounded up to 32 | [18:16] tiling | [20:19] type | [29:24] format
    0x11e4  [13:0] height | [27:16] depth/slices

**There is no pitch register** -- the hardware derives byte pitch from the
rounded width times bytes per texel.

## Mipmaps: an explicit table, not a formula and not a hardware chain

Levels are a running sum in one allocation (`offset[l] = offset[l-1] +
size[l-1]`; cube maps multiply by 6 per level). The kernel texture object holds
a **9-entry descriptor array** at `tex+0xd8 + lod*0x14`, and **only one level is
programmed at a time** -- `mtx1271` copies the selected level's three words into
the live registers when the requested base level changes.

The ICD sends every level as separate `0x0a` records, but **only when the min
filter is one of the mipmapping enums**; otherwise base level only.

## Minimum state to sample one 2D texture on unit 0

    packet A: 0x1200 = 0x00040101      pipe0 | [11:8]=1 | type 4 (2D) | opcode 0 (TEX)
              0x11dc = poolByteOffset | 0        (0 = local frame buffer)
              0x11e0 = align(width,32) | (0x32 << 24)
              0x11e4 = height | (1 << 16)
    packet B: 0x11c0 = 0x00001000 | U | (V<<3) | (W<<6)
              0x11c4 = 0xff000049
              0x11d4 = 0 ; 0x11d8 = 0
    packet C: 0x11cc = 0xffffffff ; 0x11d0 = 0 ; 0x11c8 = 0 ; 0x11e8 = 0x00500000
              0x12f4 = 0x42000000                (guard band 32.0)
    unused units: 0x1204 = 0x0001000e, and the pipe union must still reach 0xf

Plus, in the fragment blob: `blob[0x5d0] = 4`, TA descriptor 0 = TEX with a
fetch, and one combiner instruction reading texture register 0.

## Corrections to existing docs

* `parhelia-fragment-isa.md` §11.2: `0x1200[23:19]` is the TA opcode, not a
  filter code -- proven in two binaries.
* §11.4: `0x11c4` order is MAG, MIN, MIP; the enum is `mtx3018`, not the hint
  tables, which are overwritten with wrap codes before use.
* §11.5: `0x11e0[20:19]` is the texture type.
* `parhelia-texture-layout.md` §8.2: "LOD bias and clamp CONFIRMED absent" is
  **wrong** -- both exist, in `0x11c0` and `0x11c4`.
* `0x11dc[1:0]` is a memory-space selector, not software flags.
* The bump matrix lives in `0x11e0`/`0x11e4`, not `0x1240`-`0x125c`.

## Still open

* Which wrap code is mirror versus clamp -- the kernel and MTXPARD disagree.
* `0x11e0[23:21]`, `[31:30]`; `0x11e4[15:14]`, `[31:28]`; `0x11c4[13:10]`.
* The internal-format-id to 6-bit hardware-code join table.
