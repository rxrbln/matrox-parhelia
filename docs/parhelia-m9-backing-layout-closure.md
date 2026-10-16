# Matrox M9 texture backing and index-78 closure

This note reconciles the phase-two and phase-three live resource captures with
the saved Linux and Windows resource, copy, and CPU readback paths. It closes
the address-translation record grammar, pins every class-3 address calculator,
transcribes every non-32-bit aperture permutation family, and closes the
cube-face/volume-slice base rules. It also establishes that the saved bytes at
the numeric index-78 address are not a raw texture-pixel dump. Joining that
PTE-shaped record to the live table root and physical-page owner still needs
the focused capture described below.

The checked Linux core is
`m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/`
`KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.
The checked Windows constructor image is
`xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
The live evidence is under
`captures/m9x-linux-texture-resources-20260914`. Run
`python3 tools/m9-backing-layout-closure-evidence.py` from the repository root
to verify both complete binary hashes, the exact constructor slices, binary
bodies, dispatch relocations, native/formula comparisons, allocation tuples,
address records, and raw-image exclusions below.

## Exact page-entry grammar

The Linux CPU surface-copy path `m9x101` at `.text 0x24e60` supplies the
strongest static evidence. After the selected address calculator produces a
virtual byte address, request flag byte bit 7 selects the translation block at
`0x253f3..0x25434`. It looks up one little-endian dword at

```text
pte_address = request->page_table_90 + 4 * (virtual_address >> 12)
```

and tests PTE bit 3. An invalid entry produces address zero before the saved
low address-class bits are restored by the caller. A valid entry decodes as:

```text
valid             = (pte & 0x00000008) != 0
physical[31:12]   = pte[31:12]
physical[39:32]   = pte[11:4]
physical[11:0]    = virtual_address[11:0]

physical = (pte & 0xfffff000)
         | (((pte >> 4) & 0xff) << 32)
         | (virtual_address & 0xfff)
```

Request flag bit 2 subsequently adds the 64-bit relocation base at request
`+0x80`. These are independent operations: the PTE reconstructs a 40-bit
physical address, then the optional base is added. The verifier pins the
complete `m9x101` body and the exact translation bytes, and checks both a
captured low-address entry and a synthetic entry with nonzero physical bits
39:32.

## What the captured 0x80-byte record is

Every small phase-two/phase-three image saved from the numeric index-78
address is sixteen copies of the same 0x80-byte record. Interpreted as 32
little-endian PTE-shaped dwords, its nonzero slots are:

| slot | captured dword | decoded 4-KiB page |
|---:|---:|---:|
| 0 | `11ee0008` | `11ee0000` |
| 1 | `11ee1008` | `11ee1000` |
| 8 | `11f88008` | `11f88000` |
| 9 | `11f89008` | `11f89000` |
| 10 | `11f8a008` | `11f8a000` |

Depth case 05 preserves those entries and adds:

| slot | captured dword | decoded 4-KiB page |
|---:|---:|---:|
| 11 | `11f8b008` | `11f8b000` |
| 12 | `11f8c008` | `11f8c000` |

The low nibble `8`, 4-KiB-aligned payloads, and the depth-associated extra
entries match the static PTE grammar exactly. The capture does not contain the
request object which would join this byte range to `request+0x90`, however, so
the safe name is *PTE-shaped mapping record*. Treating each decoded address as
a resource-owned texture page remains a role inference until the pointed
pages and the table root are captured together.

The record is decisively not raw texture content. Its complete bytes and hash
are unchanged across:

* rectangle, cube, volume, 1-D, 2-D, mip-selection, subimage, and framebuffer
  copy cases;
* L8, A8, LA8, I8, 16-bit color, 32-bit color, and depth formats;
* different uploaded colors and pre-draw/post-draw snapshots.

The full `0x2c0000` LOD snapshot contains none of the seven exact RGBA upload
colors. Consequently subtraction of `0x10000000` from index 78 followed by a
BAR read did not reach coordinate-coded texels. It exposed mapping metadata,
an aliased mapping view, or an intervening resource record. None of these
possibilities can validate a tiled pixel permutation.

## Allocation addresses and observed lifetime

The allocation response tuple is `(kind, placement, GPU address, size,
alignment)`. Representative exact responses are:

| resource | response tuple |
|---|---|
| RGBA8 4x4 | `(10,4,11f81000,800,80)` |
| RGB565 4x4 | `(10,4,11f81000,400,40)` |
| L8 4x4 | `(10,4,11f81000,200,20)` |
| volume 7x5x4 | `(10,4,11f81000,2000,80)` |
| cube 8x8 | `(10,4,11f81000,2c0000,16000)` |
| later base/max level | `(10,4,11f91000,800,80)` |

Values are hexadecimal. Ordinary level zero consistently starts at
`11f81000`; a later independently allocated level or replacement starts at
`11f91000`. Repeated processes reuse these deterministic addresses, so the
tuples establish allocation policy and within-capture descriptor identity,
not persistence of one allocation across process lifetimes. The large cube
and mip allocations are vendor policy and should not be copied into a new
allocator without the physical-page join.

Subimage case 12 keeps the same descriptor address while changing visible
content. Copy case 13 writes destination `b0=11f81000` and the following draw
samples index 78 at the same address. Both prove resource-address stability
across an update. They do not locate its physical pixels because the old BAR
snapshot stayed unchanged.

## Complete class-3 calculator selection

`m9x745` initializes a calculator and selects one leaf address function.
Memory-controller register `0x0f04[1:0]` supplies `device_mode` and
`0x0f04[20:19]` supplies `channel_mode`; surface flag byte 0 bit 3 chooses the
local-memory rather than aperture branch. Each table row below is ordered as
mode-2 local, mode-2 aperture, other-mode local, other-mode aperture:

| element or block family | exact functions |
|---|---|
| 1 bit | `m9x439`, `m9x531`, `m9x163`, `m9x323` |
| 8 bit | `m9x297`, `m9x638`, `m9x417`, `m9x1115` |
| 16 bit | `m9x977`, `m9x259`, `m9x1199`, `m9x460` |
| 32 bit | `m9x474`, `m9x659`, `m9x587`, `m9x602` |
| 64 bit | `m9x1029`, `m9x256`, `m9x885`, `m9x934` |
| 128 bit | `m9x1010`, `m9x898`, `m9x177`, `m9x307` |
| DXT1 | `m9x710`, `m9x331`, `m9x341`, `m9x273` |
| DXT2--5 | `m9x521`, `m9x8`, `m9x911`, `m9x118` |

Zero tiled mode uses linear `m9x252`, or linear DXT `m9x990`. Surface flag
`+0x154 bit 6` selects special override `m9x382`. The mode-2 aperture
128-bit function `m9x898` is an explicit zero stub; that placement/mode/width
combination is unsupported by this implementation and must not silently use a
different routine.

The verifier pins all 35 complete bodies by SHA-256 and all 35 dispatcher
relocations. It directly invokes every leaf-only, hash-checked vendor body
with a fixed calculator state and compares 11 golden addresses per routine.
The 352 checked addresses cover origin, adjacent X/Y, microblock edges,
8/16/32-coordinate transitions, Z changes, and coarse-tile transitions.

Maintainable Python now expresses both aperture variants for every non-32-bit
family: 1, 8, 16, 64, and 128 bit, DXT1, and DXT2--5. The verifier compares
those 14 formulas with the pinned native bodies over every `(x,y)` in one
complete in-block extent, then exercises two calculator states, both relevant
controller/channel branches, nonzero bases, Z changes, and boundary
coordinates. That is 104,144 native/formula comparisons. The explicit-zero
`m9x898` route is included. `tools/m9-blocked-layout-evidence.py` separately
expresses all four 32-bit formulas. The non-32-bit local-memory coarse-address
halves remain available through the hash-pinned native oracle; the clean
aperture transcriptions close the requested non-32-bit in-block permutation
families without claiming a clean source form for those local coarse terms.

These functions return physical byte addresses for logical `(x,y,z)`. For DXT
the copy routine first rounds the rectangle to 4x4 blocks and the DXT
calculator addresses one 8-byte DXT1 or 16-byte DXT2--5 payload per block.
**Correction (2026-09-18): the coordinates it takes are texels, not block
indices.** DXT1 uses `(x & 0xC) << 1` and `(y & 0xC) << 3`, so `x[1:0]` and
`y[1:0]` are discarded and `x[3:2]`/`y[3:2]` become the low block indices;
`x = 0..3` all return one address and `x = 4` advances by the payload size.
Under a block-index reading `bx[1:0]` would be dropped and four distinct
blocks would alias. Pass the texel coordinate of the block's top-left corner.
The in-block extent is 16x16 texels for DXT1 and 16x8 for DXT2--5, both 128
bytes, which is exactly the width/height granularity the Windows layout
constructor uses for storage classes 7 and 6; see
`parhelia-m9-s3tc-compressed-textures.md`. Final bank-bit rotation depends on
both controller modes, so a new driver must read register `0x0f04` after
memory-controller initialization.

## Mip, face, and slice placement

The resource constructors close the physical base passed to the calculator:

```text
width[L]  = max(width[0]  >> L, 1)
height[L] = max(height[0] >> L, 1)
depth[L]  = max(depth[0]  >> L, 1)
offset[0] = 0
offset[L] = offset[L-1] + size[L-1]
allocation_size = offset[last] + size[last]
```

The exact 32x16 packed-mip tail places relative levels at X
`[0,16,24,16,24]`, Y `[0,0,0,8,8]`, hence uncompressed texel offsets
`[0,16,24,272,280]`. DXT1 byte offsets are `[0,32,48,160,176]`; DXT2--5
offsets are `[0,64,96,320,352]`. Upload helpers add those X/Y origins before
the GPU transfer. Pitch, allocation extent, and class-3 alignment tables are
fully recorded in `parhelia-m9-texture-state.md`.

The Windows cube constructor resolves faces as six distinct two-dimensional
subresources. At `0x53e84+0x1d`, each next mip offset advances by six times
the preceding per-face mip size. The six-object loop at `0x53f22+0x117`
stores dimension kind 1 and face ordinals 0 through 5. Finally,
`0x5435e+0x54` publishes the exact per-face offset:

```text
cube_base(level, face) = allocation_base
                       + level_offset[level]
                       + face * face_size[level]
```

The final allocation calculation at `0x5404a+0x2a` uses the same six-times
extent. The verifier pins SHA-256 hashes for each exact MxMD slice and checks
deterministic sample face offsets.

The volume constructor instead creates one dimension-kind-2 object per mip.
At `0x53220+0xa1` it computes all three dimensions as
`max(base >> level, 1)`; `0x534b0+0xc7` advances the next mip by the preceding
full-depth level size. Linux `m9x101` at `0x25333+0x15` passes the current Z
coordinate directly to the chosen calculator, and the loop at
`0x25846+0x22` increments Z. Thus a tiled volume has no independent linear
slice base:

```text
volume_address(level, x, y, z) =
    calculator(allocation_base + level_offset[level], x, y, z)
```

This closes the static face/slice base convention. The capture still lacks
the physical pages needed to compare those predicted addresses with stored
texels.

## Coherency and copy hazards

The Windows GPU copy builder synchronizes both source and destination against
the class-2 resource fence and a second legacy fence channel before emitting a
transfer. The normal `0x1b8d0` callers post the transfer asynchronously;
special caller `0x499c0` passes the optional flag that invokes `0x1ac40(1)` and
polls completion. No normal-copy path attaches the known 3D TaskMgr class-1
fence to the destination after the copy.

A new driver therefore needs an explicit destination dependency. During
bring-up, request the synchronous post-copy drain before rebinding the
destination for sampling. An optimized path can replace that drain only after
the transfer queue completion is represented by a fence consumed by the 3D
submission. Reusing an unchanged index-78 descriptor after subimage or copy is
valid only with this ordering; descriptor stability is not cache coherence.
`parhelia-m9-texture-coherency-re.md` contains the exact producer/consumer
callback and indexed-fence joins.

## Minimal remaining capture

One carefully structured run can close the remaining root/page ownership and
hardware-layout boundary without repeating the API and descriptor matrix:

1. Save memory-controller register `0x0f04`, the selected surface local/
   aperture flag, index 78, its allocation response, and the CPU-copy request
   fields `+0x80`, `+0x90`, and flag byte.
2. Dump the page-table root at request `+0x90`. Parse every valid dword with
   the formula above and dump each decoded 4-KiB physical page. Preserve the
   root and pages before upload, after upload completion, and after sampling.
3. Also retain the old BAR view at both the historical
   `index78 - 0x10000000` offset and the unmodified GPU address. This will
   identify which view produced the repeated PTE-shaped record without
   assuming that it is the table root.
4. Upload coordinate-coded, nonrepeating data large enough to cross a tile and
   a 4-KiB page for 8-, 16-, 32-, 64-, and 128-bit families. Use payloads in
   which every byte position is recoverable from `(x,y,z,component)`.
5. Upload valid DXT1 and DXT5 blocks through the D3D path, since the saved
   Linux OpenGL ICD rejects the public compressed-texture tokens. Give every
   block a unique decodable payload.
6. Capture an odd 1-D texture, odd 2-D texture, the full five-level mip tail,
   six uniquely coded cube faces, and at least four uniquely coded 3-D slices
   at two mip levels to validate the now-static base formulas. Add a subimage
   and framebuffer-copy rectangle crossing both a tile edge and a page edge.
7. For each page, compare raw bytes at the exact addresses predicted by the
   selected calculator, including the packed-tail X/Y origin and any candidate
   face/slice base. Record allocation/free events so reused GPU addresses do
   not get mistaken for one persistent backing.

That capture will validate the hardware-selected calculator and root-address
convention, join the PTE-shaped record to its owner, and validate the static
cube/volume formulas against physical storage. Static binaries cannot supply
the runtime value of `request+0x90`, identify which captured address view owns
the repeated record, or dump the pages named by its PTEs. Those are the exact
hardware-capture-only gates. Until that capture exists, the formulas are
enough to build and unit-test an address library, but the old VRAM images do
not prove the physical pixel layout.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
