# Native S3TC in the Mesa Parhelia driver

**This note is Parhelia/P690 only.**  The M-Series uses a different code
space - DXT1 `0x10`, DXT2/3 `0x15`, DXT4/5 `0x17` - and a different tiling;
see `parhelia-m9-s3tc-compressed-textures.md`.  Do not carry the `0x08`,
`0x0a`, `0x0c` codes or the 32x16-texel macrotile across.

This note is a source-level implementation recipe for adding native DXT1,
DXT3 and DXT5 sampling to the current Gallium driver.  Its hardware claims are
derived only from the binary drivers and documentation kept in this tree.  No
Mesa source was changed while preparing it.

The important result is that compressed textures have their own sampler format
codes and their own block-tiled layout.  They are not 32-bit textures with a
different format selector, and they cannot use `ph_mip_texel_offset_cpp()`.

## Recovered descriptor encodings

The exact native format codes are:

| Gallium format | S3TC block | `0x11e0[29:24]` | `0x11e0[18:16]` |
|---|---:|---:|---:|
| `PIPE_FORMAT_DXT1_RGB` | 8 bytes | `0x08` | `5` |
| `PIPE_FORMAT_DXT1_RGBA` | 8 bytes | `0x08` | `5` |
| `PIPE_FORMAT_DXT3_RGBA` | 16 bytes | `0x0a` | `5` |
| `PIPE_FORMAT_DXT5_RGBA` | 16 bytes | `0x0c` | `5` |

The unused-by-GL DXT2 and DXT4 codes are `0x09` and `0x0b`.  DXT1 RGB and
RGBA share code `0x08`; the endpoint ordering inside each BC1 block determines
whether the punch-through-alpha case is present.

For a base-level 2D texture, program the existing `p3d_texture` fields as:

```c
surface_base = relocated_vram_byte_address;
surface_w = (width & 0x3fff) | (5u << 16) | (format_code << 24);
surface_h = (height & 0x3fff) | (1u << 16); /* depth 1, max level 0 */
```

Keep the existing filter, wrap, LOD and texture-environment words.  Width and
height remain logical texel dimensions.  Layout 5 makes the storage and copy
paths interpret them as 4x4 blocks.  Do not put block counts in the descriptor.

This mapping is proven by the following independent joins:

* Windows `MTXPARD.dll` `0x83f80` maps FOURCC DXT1 through DXT5 to internal
  ids 1 through 5.  `0xa3b40` classifies hardware code `0x08` as 4 bits per
  texel and `0x09` through `0x0c` as 8 bits per texel.
* Linux `mtx_parhelia.o` constructor `mtx1179` at `.text+0x7bf60` stores
  internal id at record `+0x04`, variant at `+0x08`, and the hardware sampler
  code at `+0x0c`.  `mtx1646` builds the DXT records with `ecx=0x08` at
  `0x7e813/0x7e863/0x7e8b3`, `0x09` at
  `0x7e950/0x7e99d/0x7e9f0`, `0x0a` at
  `0x7ea8d/0x7eadd/0x7eb2d`, `0x0b` at
  `0x7ebca/0x7ec1a/0x7ec6a`, and `0x0c` at
  `0x7ed07/0x7ed57/0x7eda7`.  `mtx3424` at `0x1ec67` copies record `+0x0c`
  into the descriptor's six-bit format field.
* Linux `mtx2029` at `.text+0x1e9a0` forces layout 5 for compressed variants;
  `mtx3100` selects compressed surface class 4 at `0x272c6`.  The native
  compressed path never selects layout 0 or layout 2.

The older `0x2e` DXT guess is false: both binaries classify `0x2e` as a
16-bit-per-texel format.

## Block pitch and address calculation

For the power-of-two textures accepted by the Linux vendor ICD, let:

```c
Q   = format == DXT1 ? 8 : 16;             /* bytes per 4x4 block */
P   = max(32, next_power_of_two(width));    /* pitch in logical texels */
NBX = DIV_ROUND_UP(width, 4);
NBY = DIV_ROUND_UP(height, 4);
```

The logical block-row pitch is `(P / 4) * Q`: `2*P` bytes for DXT1 and `4*P`
bytes for DXT3/5.  This pitch describes a linear staging image.  VRAM is
macro-tiled, so it is not a byte stride that can be added to a mapped base.

Linux `mtx2799` at `.text+0x20720` gives the exact VRAM address.  Its outer
loops advance x and y by four texels (`0x209a4`, `0x20a08`, `0x20ae0`), copy
`bpp*16/8` bytes (`0x209ea`-`0x20a03`), and generate these offsets at
`0x209af`-`0x20ad9`:

```c
static size_t
dxt_block_offset(unsigned P, unsigned bx, unsigned by, unsigned Q)
{
   const unsigned macro = (by >> 2) * (P >> 5) + (bx >> 3);
   return (size_t)macro * (32 * Q) +
          (size_t)(by & 3) * (8 * Q) +
          (size_t)(bx & 7) * Q;
}
```

One macrotile is therefore 8x4 S3TC blocks, or 32x16 logical texels.  It is
256 bytes for DXT1 and 512 bytes for DXT3/5.  Blocks are ordinary S3TC byte
sequences inside each macrotile; macrotiles, rather than complete block rows,
are row-major.

The equivalent expressions visible directly in the binary are:

```text
DXT1:   macro*256 | ((y & 12) << 4) | ((x & 28) << 1)
DXT3/5: macro*512 | ((y & 12) << 5) | ((x & 28) << 2)
```

where `x=4*bx` and `y=4*by`.  The ORs are safe because the fields do not
overlap.  Windows `MTXPARD.dll` `0x2f260` independently confirms the units:
layout 5 multiplies bits-per-texel by 16/8 for an 8- or 16-byte copy, divides
both dimensions by four, and multiplies its abstract pitch by four.

`mtx2799` uses `align(width,32)` for its macro row.  That equals `P` for every
power-of-two width and for every width below 32, which covers the Linux ICD's
accepted inputs.  It does not equal `P` for a general NPOT width above 32.
Native compressed NPOT storage must remain disabled until the hardware rule is
measured.

## Native allocation requirements

The Linux local-VRAM surface helpers give the strict allocation constants for
layout 5 with the auxiliary surface flags clear, as they are for textures:

| format | bits/texel | per-level alignment `A` | final allocation alignment `G` |
|---|---:|---:|---:|
| DXT1 | 4 | `0x200` | `0x800` |
| DXT2/3/4/5 | 8 | `0x400` | `0x1000` |

These values come from `mtx1219` `.text+0x5b610`: its layout-5 arms are
`0x5b6f2`-`0x5b709` and `0x5b769`-`0x5b77e`.  Argument zero selects `A` and
argument one selects `G`.

`mtx625` `.text+0x5b7f0` supplies the vertical granularity.  For layout 5 at
4 or 8 bits per texel it is 32 texel rows when the logical height is at most
32, otherwise 64 texel rows.  This is the `0x5b8c0` arm.  The nearby
16/32-row arm at `0x5b8a6` is for 30/32-bpp formats, not DXT.  Thus a standalone
level requires:

```c
Ha         = height <= 32 ? 32 : 64;
level_size = ALIGN((size_t)ALIGN(width, 32) * ALIGN(height, Ha) * bpp / 8, A);
bo_size    = ALIGN(level_size, G);
```

This is intentionally more conservative than the last byte reached by the
32x16 block address formula.  It is the size generated by Linux `mtx1444`
at `.text+0x5b900` and should be used when implementing a native-size
allocator.

The current kernel ABI does not accept a format or a byte size in
`CREATE_RENDER`.  `PARHELIA_CREATE_RENDER_TEXTURE` always reserves a 32-bpp
texture-shaped BO with `create.pitch = 4*P`, height rounded to 16 and total
size rounded to 8 KiB.  That BO is larger than every DXT requirement above and
its 8 KiB base alignment satisfies both `G` values.  The first Mesa
implementation can use this ABI unchanged:

* pass the logical width and height with `PARHELIA_CREATE_RENDER_TEXTURE`;
* retain `P = create.pitch / 4` as a texel pitch, but do not treat
  `create.pitch` as the compressed byte stride;
* allocate the CPU hardware-layout shadow at the returned `create.size`, zero
  it, write S3TC blocks with `dxt_block_offset()`, and upload it with the
  existing `UPLOAD_RENDER` ioctl;
* check that the returned size is at least the computed native DXT allocation.

This wastes VRAM by a factor of roughly eight for DXT1 and four for DXT3/5.
A later UAPI may add format-aware byte-size allocation, but that is not needed
to prove native sampling.

For a complete vendor-style mip chain, Linux `mtx1444` sizes each level with
the formula above, aligns each level start to `A`, and aligns the final chain
to `G`.  Once width and height are both at most 16, the vendor builders pack
the remaining levels into a shared 32x16-texel tail at logical positions
`(0,0), (16,0), (24,0), (16,8), (24,8)` for 16, 8, 4, 2 and 1 texel levels.
All coordinates are block-aligned for S3TC and `mtx2799` includes those x/y
origins in its address calculation.  The current P690 Mesa driver deliberately
backs only level zero because the general mip descriptor has not retired on
hardware; keep S3TC at level zero under that same gate initially.

## Mesa implementation recipe

1. Add a small format classifier returning `{compressed, format_code,
   block_bytes}` for the four non-sRGB Gallium formats in the first table.
   Keep the current blanket sRGB rejection: this sampler has no recovered
   sRGB decode.
2. In `ph_format_supported()`, accept those formats only for one-sample
   `PIPE_TEXTURE_2D` sampler views.  Reject render-target, depth/stencil,
   scanout, display, shared and image-style binds.  Initially reject compressed
   NPOT resource creation as described above.
3. Give `parhelia_resource` an explicit compressed-layout marker and block
   byte count.  Do not pass compressed resources through the 4-byte-texel
   `ph_mip_layout_init()`, `p690_native_texture32`, A1 packing, linear resolve,
   or BITBLT paths.  Compute `P` and the native size with the formulas in this
   note, then use the existing texture allocation/upload ABI.
4. Make compressed transfer maps block-linear.  Gallium boxes remain in
   texels, while the returned stride and layer stride are in block bytes:

   ```c
   map_stride  = util_format_get_stride(format, box->width);
   block_rows  = util_format_get_nblocksy(format, box->height);
   layer_stride = map_stride * block_rows;
   ```

   Require x and y to be multiples of four unless that coordinate is the
   image edge, following Gallium's compressed-box rules.  On unmap, use
   `bx=(box->x/4)+i`, `by=(box->y/4)+j` and copy one `Q`-byte block between the
   linear transfer and `map + dxt_block_offset(P,bx,by,Q)`.  Reads perform the
   inverse copy.  No R/B conversion is applied to encoded blocks.
5. Add an identical-format compressed block-copy path to
   `resource_copy_region`; decline format conversion and scaling so the state
   tracker can choose its fallback.  Never enter the current `cpp == 4`
   per-texel loop for S3TC.
6. At draw submission, force the native texture handle and layout 5 for every
   compressed resource.  Select `0x08`, `0x0a` or `0x0c` from the resource
   format, use logical width/height, `depth=1`, and keep `max_level=0` while
   the existing P690 mip gate remains.  DXT must not fall back to the layout-0
   linear companion.
7. Upload only after all block writes are in the hardware-layout shadow.  The
   existing eager `ph_resource_upload_texture()` and bind-time retry are
   suitable.  Keep padding zeroed and upload the returned BO size because the
   current ioctl requires a bounded span inside that BO.

## Hardware checks still required

The binary evidence resolves the encodings and address formula.  These points
still need a P690 run before enabling the extension by default:

* Draw a 32x16 DXT1 texture in which every one of the 32 blocks has a unique
  solid colour.  This detects block byte order, x/y swaps, macro-row pitch and
  the layout-5 descriptor in one image.  Repeat with DXT3 and DXT5 alpha ramps
  to verify codes `0x0a/0x0c` and sampled alpha through the current fragment
  path.
* Test 1x1, 2x2, 4x4, 8x8 and rectangular 64x16/16x64 images.  The upload loop
  uses ceiling block counts, while the Windows copy helper's local dimension
  arithmetic is a right shift; edge dimensions must be settled on silicon.
* Test a width with two macrotiles, then a power-of-two height crossing 32
  rows, to catch allocation or pitch mistakes hidden by the current oversized
  BO.
* Keep compressed NPOT widths above 32 disabled.  The recovered Linux uploader
  uses `align(width,32)` while the descriptor pitch helper uses the next power
  of two, and the Linux ICD never supplies such an image.
* Leave compressed mip filtering disabled until the existing P690 mip probe
  retires and then verify the shared compressed tail independently.  Base-level
  S3TC does not depend on that result.
* Verify BC1 RGB versus RGBA with blocks on both sides of the `color0 <= color1`
  endpoint condition.  The descriptor code is shared, so correctness depends
  on the hardware following standard BC1 punch-through semantics.

## Hardware result (P690, 2026-10-14)

Measured with `parhelia-drm-test --texture-dxt-probe` (PROBE_MAP,
PROBE_LINEAR, PROBE_LEVELS, PROBE_PIN); Mesa abe15f1 implements it.

* The 32x16 macrotile above holds for DXT3/5 only within one 32-row band.
  Storage is 64x32-texel tiles: 1 KiB of DXT1, 2 KiB of DXT3/5, made of
  512-byte units 32 texels wide (8x8 DXT1 blocks, 8x4 DXT3/5 blocks, blocks
  row-major inside a unit), units left-right then top-down inside a tile.
  Tile rows advance by pitch/64 tiles - at a 32-texel pitch two 32-row bands
  share one tile - and every odd tile row swaps the unit columns (texel x
  bit 5 ^ texel y bit 5), as in the uncompressed texture order.
* Mip chains (16x16 to 512x512, 8:1 rectangles, both block sizes, exact):
  levels follow one another at pitch max(32, next power of two of the
  width), each taking its 32-row bands of tiles; the first level no larger
  than 16x16 starts a tail 512 bytes into the previous level's tile (texel
  x 32), or at 0 as the base; later levels at (+16,0), (+24,0), (+16,+8),
  (+24,+8) of it.
* Non-power-of-two bases are addressed exactly at the next power-of-two
  pitch (48x48 .. 200x24).  Their chains place levels by the unit columns
  the previous level occupies (96x32: level 1 at 1.5 tiles); not modelled
  yet, so Mesa samples such chains through a decoded copy.
