# M9 Direct3D format-operation matrix

This note closes the static per-format capability-table gap in the saved M9
Windows driver.  `MxMD.dll` contains a complete 52-record baseline table at VA
`0x20a008`, not merely a runtime-generated list of format names.  Constructor
`0x784e0` copies that table and can only remove operation bits.  A second pass
at `0x76dc0` rewrites multisample masks and applies one anti-aliasing-related
render-target restriction.

`tools/m9-format-operation-evidence.py` pins the image, all 1,664 table bytes,
every decoded record, the copy/intersection loop, and both restriction passes.
`[BIN]` below means direct binary evidence and `[DER]` a deterministic decode.

## Record ABI and construction

Each 32-byte template is an XPDM `DDPIXELFORMAT` carrying Direct3D format
operations [BIN]:

| offset | baseline value or role |
|---:|---|
| `+0x00` | size `0x20` |
| `+0x04` | `DDPF_D3DFORMAT = 0x00200000` |
| `+0x08` | public `D3DFORMAT` enum or FourCC |
| `+0x0c` | zero |
| `+0x10` | `D3DFORMAT_OP_*` bit mask |
| `+0x14` | packed 16-bit flip and blit multisample-type masks |
| `+0x18,+0x1c` | zero in every template |

Constructor `0x76e90` fixes the count at `0x34`, multiplies it by `0x20`, and
stores the allocated array at object `+0x7e8` (`0x77034..0x77065`) [BIN].
The ordinary path at `0x79ad0..0x79b23` copies each 32-byte record from
`0x20a008`; when a matching restriction exists it then computes [BIN][DER]:

```text
destination[i].Operations &= per_format_restriction
```

No branch in this builder adds an operation bit.  Parser failure takes the
bulk-copy fallback at `0x79b62`, which copies all `count * 32` baseline bytes.

## Operation masks

The masks use the Direct3D 9 DDI meanings below [DER].  Implied bits such as
`SAME_FORMAT_RENDERTARGET` need not be present when
`OFFSCREEN_RENDERTARGET` is set; the Direct3D runtime supplies implications.

| bit | D3D operation |
|---:|---|
| `000001` | 2-D/mip texture |
| `000002` | volume texture |
| `000004` | cube texture |
| `000008` | offscreen render target |
| `000040,000080` | depth/stencil; arbitrary color depth |
| `000400,000800` | display mode; 3-D acceleration |
| `002000,004000` | convert to ARGB; offscreen plain/blit/fill |
| `008000` | sRGB sampler read/linearization |
| `010000` | bump-map texture |
| `080000` | ARGB conversion target group |
| `100000` | sRGB render-target write/delinearization |
| `400000` | automatic mipmap generation |
| `800000` | vertex texture |

The template uses none of `SAME_FORMAT_RENDERTARGET`, `PIXELSIZE`, `DMAP`,
`NOFILTER`, or `NOALPHABLEND` explicitly [BIN].

The 52 records collapse to these exact groups [BIN][DER]:

| Operations | formats | decoded operations |
|---:|---|---|
| `00d8ec0f` | `X8R8G8B8`, `R5G6B5` | 2-D, volume, cube, RT, display, 3-D, ARGB conversion, plain, sRGB read/write, ARGB group, autogen, vertex texture |
| `00d8e00f` | `A8R8G8B8`, `X1R5G5B5`, `A1R5G5B5`, `A2B10G10R10`, `X8B8G8R8`, `A2R10G10B10`, `A16B16G16R16` | same except display/3-D mode bits |
| `00d0e00f` | `A4R4G4B4`, `X4R4G4B4`, `A8B8G8R8` | 2-D, volume, cube, RT, ARGB conversion, plain, sRGB read/write, autogen, vertex texture |
| `00c0e00f` | `L8`, `A8L8` | 2-D, volume, cube, RT, ARGB conversion, plain, sRGB read, autogen, vertex texture |
| `00c0c00f` | `L16` | 2-D, volume, cube, RT, plain, sRGB read, autogen, vertex texture |
| `00c0600f` | `R16F`, `G16R16F`, `A16B16G16R16F`, `R32F`, `G32R32F`, `A32B32G32R32F` | 2-D, volume, cube, RT, ARGB conversion, plain, autogen, vertex texture |
| `00c0200f` | private `0x102b0002` | 2-D, volume, cube, RT, ARGB conversion, autogen, vertex texture |
| `0080a007` | `A8`, `A4L4`, `DXT1..DXT5` | 2-D, volume, cube, ARGB conversion, sRGB read, vertex texture |
| `00810007` | `V8U8`, `L6V5U5`, `X8L8V8U8` | 2-D, volume, cube, bump-map, vertex texture |
| `00800007` | `Q8W8V8U8`, `V16U16`, `A2W10V10U10`, `Q16W16V16U16` | 2-D, volume, cube, vertex texture |
| `00006001` | `UYVY`, `YUY2` | 2-D texture, ARGB conversion, plain/blit/fill |
| `00006000` | `NV12`, `YV12`, private `MXP0` | ARGB conversion and plain/blit/fill only |
| `000000c0` | `D32`, `D15S1`, `D24S8`, `D24X8`, `D24X4S4`, `D16`, `D16_LOCKABLE` | depth/stencil with arbitrary color depth |
| `00000000` | `VERTEXDATA`, `INDEX16`, `INDEX32` | no surface-format operations |

The table does not contain `R8G8B8`, although mapper `0x4ee30` aliases that
public enum to the 32-bit `X8R8G8B8` native descriptor.  Mapping and API
advertisement are consequently distinct [BIN][DER].  The table additionally
contains video-only `NV12`, `YV12`, and `MXP0` records which the ordinary 3-D
resource mapper does not accept.  Those rows are copy/video capability evidence,
not sampler descriptors.

## Direct consequences for the new driver

The baseline declares all conventional uncompressed, compressed, signed, and
floating texture rows as cube-, volume-, and vertex-texture capable.  This is
the strongest static capability evidence yet for those dimensions.  The
native dimension kind and packet selector are independently joined in
`parhelia-m9-texture-state.md`, so these are no longer unsupported guesses.
Result validation is still required before exposing uncommon formats because
the operation list does not prove conversion precision, filtering, or correct
physical allocation by a new driver.

Both native GigaColor formats, `A2B10G10R10` and `A2R10G10B10`, have the full
`00d8e00f` group [BIN].  The vendor baseline therefore declares 2-D, cube,
volume, render-target, sRGB read/write, automatic mipmap, and vertex-texture
use for each.  Their exact hardware descriptor codes and clear packers are
independently known.  This closes the static capability declaration, while
render/sample/blend/readback results remain the exposure gate.

All DXT formats have the same `0080a007` operations [BIN].  The driver declares
DXT1 through DXT5 for 2-D, cube, volume, sRGB read, and vertex sampling; it does
not declare them as render targets or automatic-mipmap destinations.  Combined
with the known `0x10/0x15/0x17` descriptor codes and packed-tail layout, this
closes the intended resource classes without inferring renderability.

Every float format has the same `00c0600f` baseline [BIN].  It includes
offscreen rendering and automatic mipmap generation as well as all three
texture dimensions and vertex sampling.  A policy bit can remove all these
operations as described below.  No float row declares sRGB conversion.

## Runtime restrictions

The configuration parser accepts uppercase letters, digits, underscore, and a
`*` token after a leading `+` or `-` (`0x7858c..0x78613`) [BIN].  For a named
format, `+` produces mask `0xffffffff`, while `-` produces `0xfffffff8`.
The later intersection therefore gives [DER]:

```text
+FORMAT  leaves Operations unchanged
-FORMAT  clears only TEXTURE | VOLUMETEXTURE | CUBETEXTURE
```

This is a restriction language, not an enable override.  A malformed token
falls back to copying the baseline table intact at `0x79b62` [BIN].

If device policy object `+0x280 -> +0x18 bit 0` is set,
`0x799bf..0x79abc` replaces `Operations` with zero for all six float enums
`0x6f..0x74` [BIN].  This gate must be checked in any captured board-specific
capability result.

When `AAMode` has both bit 31 and bit 0 set, `0x76e3f..0x76e6d` clears
`OFFSCREEN_RENDERTARGET` only for `R5G6B5`, `X1R5G5B5`, `A1R5G5B5`,
`A4R4G4B4`, and `X4R4G4B4` [BIN].  It leaves both 10-bit GigaColor formats
unchanged.

## Multisample correction

The baseline marks exactly thirteen format records with nonzero flip and blit
fields `0x0100`: four conventional 32-bit color formats, both GigaColor
formats, six non-lockable depth formats, and private `0x102b0002` [BIN].  The
post-pass treats these values as eligibility markers and always replaces them;
the template value itself must not be assigned a sample count.

The constructor call at `0x772c4` passes argument two equal to one into
post-pass `0x76dc0`.  That branch forces its chosen mask to one and replaces
both nonzero 16-bit fields.  Thus the constructed table contains packed
`0x00010001`, advertising the nonmaskable multisample type for every eligible
row; it does not retain packed `0x01000100` [BIN][DER].  If context `+0x24` is
nonzero, the same helper writes zero instead.

A second direct call exists in the GetDriverInfo format-record path at
`0x3e5b9` [BIN].  That path copies one record and passes argument two equal to
zero, causing `0x76dc0` to recompute the mask from `AAMode`.  Default type 8
becomes `0x0080` in both fields.  A sibling branch at `0x3e555..0x3e580`
instead copies the stored record without this recomputation.  The branch is
selected by a request field at `+0x14`; its interface/version meaning is not
named in the binary.

The exact public result is therefore path-dependent: construction stores
nonmaskable masks, while one record-query path returns configured type-eight
masks.  The default independently returns eight quality levels.  Together
with the saved Linux FBConfigs exposing no sample buffers, this static evidence
is still insufficient to enable FAA, but both Windows declarations are now
explicit rather than contradictory observations from unrelated functions.

## Remaining gates

The operation matrix is now exact; a live D3D format query is useful only to
record which restrictions the target board/configuration applies.  Hardware
enablement still needs:

1. result matrices for uncommon signed, float, YUV, DXT, depth-sample, cube,
   volume, and vertex-texture paths;
2. GigaColor render, blend, sample, mip, and raw backing/readback validation;
3. a multisample allocation, coverage, partial-mask, resolve, query-unit, and
   presentation capture which records which stored or recomputed format-query
   branch the runtime consumes;
4. the class-3 in-block address permutation for independently allocated
   blocked resources;
5. new-driver replay of every capability before Mesa exposes it.

The saved IRQ and recovery audits still contain no requester/3-D reset leaf.
No format-table code reaches those paths, so the new evidence does not weaken
the existing fail-closed recovery requirement.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
