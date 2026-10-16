# M9 S3TC compressed textures

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

Everything a driver author needs to create, place, upload and sample a DXT1,
DXT3 or DXT5 texture on the Matrox M-Series, decoded from the saved 32-bit
XPDM driver and the saved Linux kernel library.  It extends
`parhelia-m9-texture-storage.md` (storage and unit words),
`parhelia-m9-texture-state.md` (descriptor construction) and
`parhelia-m9-backing-layout-closure.md` (address calculators); those notes own
the uncompressed rules and are not restated here.

Labels: **CAPTURED** = read directly from a saved binary or a saved stream,
**DERIVED** = a deterministic join of CAPTURED facts, **INF** = inferred and
not established.

Pinned images (SHA-256 checked by `tools/m9-texture-resource-evidence.py`):

| image | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| `m9xdriver-.../kernel/KernelLibraries.o_shipped` | `bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7` |

`MxMD.dll` addresses are virtual addresses; its image base is `0x10000` and
`.text` RVA equals file offset, so VA minus `0x10000` is the file offset.
`MxMGLX.dll` has image base `0x10000000`.

Run `python3 tools/m9-texture-resource-evidence.py --self-test` from the
`xf86-video-parhelia` root; the storage rules below are executable assertions
in `verify_static_compressed_storage()`.

## 1. Result

The hardware samples three distinct compressed formats.  It stores them as
ordinary tiled surfaces at **4 or 8 bits per texel**, with the 4x4 block as the
addressing unit, and it never decompresses on upload.  Dimensions everywhere
in the hardware descriptor are **texels**, never blocks.

| D3D FourCC | internal ID | hardware code | descriptor byte `79[31:24]` | bits/texel | storage class |
|---|---:|---:|---:|---:|---:|
| `DXT1` | 0 | `0x10` | `0x20` | 4 | 7 |
| `DXT2` | 2 | `0x15` | `0x2a` | 8 | 6 |
| `DXT3` | 3 | `0x15` | `0x2a` | 8 | 6 |
| `DXT4` | 4 | `0x17` | `0x2e` | 8 | 6 |
| `DXT5` | 5 | `0x17` | `0x2e` | 8 | 6 |

**DXT2 and DXT3 are the same hardware format, and so are DXT4 and DXT5**
(CAPTURED).  The premultiplied-alpha variants have no separate code, so the
hardware decodes the block identically and premultiplication is purely an
application convention.  A Gallium driver therefore needs exactly three
sampler codes, `0x10`, `0x15` and `0x17`.

## 2. Public format to internal ID (CAPTURED)

Two independent mappers agree:

* `0x4ee30`, the `D3DFORMAT` decoder.  Its FourCC arm at `0x4ef3f..0x4efc2`
  returns 0 for `DXT1` (`0x4ef6b`), 2 for `DXT2` (`0x4ef79`), 3 for `DXT3`
  (`0x4efa6`), 4 for `DXT4` (`0x4efa0`) and 5 for `DXT5` (`0x4efac`).
* `0x4fd10`, which reads a FourCC from object `+0x08` and returns the same
  five values at `0x4fd2c`, `0x4fd37`, `0x4fd45`, `0x4fd53` and `0x4fd61`.

Internal ID 0, not a sentinel, is DXT1; the rejection value in both mappers is
`-1`.

## 3. The format record, and what the "auxiliary" field is (CAPTURED)

`0x4fe00` is an 86-arm initializer over internal IDs `0x00..0x55`, dispatched
through the jump table at `0x50b48`.  It fills a `0x30`-byte record:

| field | meaning |
|---|---|
| `+0x00` | internal ID |
| `+0x04` | format family: **1 = compressed**, 2 = packed RGB(A), 3 = float/wide, 4 = depth, 5 = single/dual channel, 6/7 = signed/bump, 8 = YUV, 9/0xb/0xc = private |
| `+0x08` | hardware format code |
| `+0x0c` | alpha/variant discriminator (zero for every DXT arm) |
| `+0x14` | FourCC, zero for non-FourCC formats |
| `+0x18` | **bits per texel** |
| `+0x1c,+0x20,+0x24,+0x28` | R, G, B, A channel masks for family 2 |

The six compressed arms are:

| ID | arm | `+0x04` | `+0x08` | `+0x14` | `+0x18` |
|---:|---:|---:|---:|---|---:|
| 0 | `0x4fe3c` | 1 | `0x10` | `DXT1` | 4 |
| 1 | `0x4fe5d` | 1 | `0x10` | `DXTN` | 4 |
| 2 | `0x4fe7e` | 1 | `0x15` | `DXT2` | 8 |
| 3 | `0x4fe9f` | 1 | `0x15` | `DXT3` | 8 |
| 4 | `0x4fec0` | 1 | `0x17` | `DXT4` | 8 |
| 5 | `0x4fee1` | 1 | `0x17` | `DXT5` | 8 |

`+0x18` is **bits per texel**, not a block byte count, a halved byte count or a
dword count.  This settles the `[INF]` left in `parhelia-m9-texture-storage.md`.
Three independent joins:

1. Across the whole 86-arm table `+0x18` is unambiguously bits per texel:
   `0x20` for every 32-bit format, `0x10` for every 16-bit one, `0x40` for
   `A16B16G16R16`, `0x80` for `A32B32G32R32F`, `8` for `L8`/`A8`.  4 and 8 are
   the only values the DXT arms could carry under that reading, and
   `8 bytes / 16 texels = 4 bits` and `16 bytes / 16 texels = 8 bits` are
   exactly the S3TC bit rates (CAPTURED + DERIVED).
2. Layout constructor `0x45eb0` re-derives the same numbers from the hardware
   code alone, in a different table (section 4).
3. The saved OpenGL ICD stores 4 for its DXT1 ids and 8 for its DXT3/DXT5 ids
   at descriptor `+0xa8` (`MxMGLX` `0x100dc650`, `0x100dc65c`), and uses them
   with the `(w+3)>>2 * blockBytes` size helper at `0x10125a90` (CAPTURED).

Internal ID 1 (`DXTN`, hardware code `0x10`, 4 bits per texel) exists only in
this initializer.  Neither FourCC mapper produces ID 1 and the literal `DXTN`
occurs exactly once in the image, at `0x4fe75`, inside that arm.  It is a dead
table entry; do not expose it (CAPTURED).

## 4. Storage class and allocation geometry (CAPTURED)

Layout constructor `0x45eb0` is the authority for VRAM placement.  Its switch
at `0x45f6c` (byte table `0x46514`, jump table `0x464f0`) maps the hardware
code to a bits-per-texel value at `+0x164` and a **storage class** at `+0x168`:

| storage class | bits/texel | codes |
|---:|---:|---|
| 0 | 128 | `0x34` |
| 1 | 64 | `0x38..0x3f` |
| 2 | 32 | default |
| 3 | - | forced when resource flags `+0x14c` bit 6 is set |
| 4 | 16 | `0x46,0x4a,0x4e,0x50..0x56,0x58,0x59,0x5c,0x5d` |
| 5 | 8 | `0x30..0x33` |
| **6** | **8** | **`0x15`, `0x17`** (`0x45f9e`) |
| **7** | **4** | **`0x10`** (`0x45f88`) |
| 8 | 1 | `0x00,0x01,0x03` |

Compressed textures are therefore not a variant of the 32-bit family; they are
two storage classes of their own, and the ordinary
`ph_mip_texel_offset_cpp()`-style per-texel arithmetic cannot be reused.

### 4.1 Alignment granularities

Four dword tables are indexed by `mode + 3*sel + 6*class`, where `class` is the
storage class above, `mode` comes from device object `+0x35f8 -> +0x24`
(0 when that field is neither 0 nor 1, 1 when it is 1, 2 when it is 0) and
`sel` is a resource bit selected at `0x46151..0x46163`:

| table | VA | meaning |
|---|---:|---|
| `T_W` | `0x1f19a0` | width granularity in texels |
| `T_H` | `0x1f1a78` | height granularity in texels |
| `T_A` | `0x1f1b50` | per-level byte alignment |
| `T_G` | `0x1f1c28` / `0x1f1c40` | final allocation byte granularity |

The compressed rows are:

| class | `T_W` sel0 | `T_W` sel1 | `T_H` sel0 | `T_H` sel1 | `T_A` |
|---:|---:|---:|---:|---:|---:|
| 6 (DXT2..5) | 16 | 64/32/32 | 8 | 32 | `0x200` (`0x400` at mode 2, sel 1) |
| 7 (DXT1) | 16 | 64/32/32 | 16 | 64 | `0x200` (`0x400` at mode 2, sel 1) |

Two overrides matter:

* **Tiled path, `0x4627a..0x462a5`.**  For classes 6 and 7 only, the width
  granularity is `align(T_W, 32)`, so a compressed level's width is always
  padded to at least **32 texels** (8 blocks).  Every other class takes the
  `sel = 1` row unchanged.
* **Row-major path, `0x4608d..0x460c1`** (taken when the kind field `+0x138`
  is zero and resource flag `0x200` is clear).  The width granularity is 8 and
  the height granularity is **4 for classes 6 and 7, 1 otherwise** - that is,
  one whole block row.

The resulting padded extents are stored at `+0x184` (width) and `+0x188`
(height).

### 4.2 Small levels and the packed tail

`0x46307..0x4637b`: when width and height are both at most `16 - 3*(flags&1)`
and depth is below `15 - 2*(flags&1)`, the padded extent is forced to at least
**32 x 16 texels**.  This is the same 32x16 packed-mip tail the uncompressed
capture already proves, and it applies to compressed levels unchanged.  The
level origins inside that tail are X `[0,16,24,16,24]`, Y `[0,0,0,8,8]`; in a
linear staging image the corresponding byte offsets are `[0,32,48,160,176]` for
DXT1 and `[0,64,96,320,352]` for DXT2..5 (CAPTURED,
`parhelia-m9-backing-layout-closure.md`).  All those origins are multiples of
8 texels, so every tail level starts on a block boundary (DERIVED).

### 4.3 Pitch and allocation size

`0x4641a..0x464d9`:

```c
/* +0x18c is a second width alignment, by class:
   class 4 -> 16, class 5 -> 32, class 7 -> 16, otherwise 8  (0x4641a..0x46448) */
pitch_bytes  = aligned_pitch_width * bits_per_texel / 8;            /* +0x190 */
texels       = padded_width * padded_height * depth + edge_term;    /* 64-bit */
size_bytes   = texels * bits_per_texel / 8;                         /* 64-bit */
allocation   = ALIGN(size_bytes, T_G);                              /* +0x180 */
```

`edge_term` is zero unless resource flag `+0x14c`/arg `0x100` is set
(`0x463af..0x46416`); an ordinary texture takes the zero path.  For DXT1 the
result is `padded_w * padded_h / 2` bytes and for DXT2..5 `padded_w *
padded_h` bytes - exactly the block byte counts, with no decompression reserve
(DERIVED).

**Do not implement this as written.**  Section 12.2 measures that a level
below 128x128 reaches well past its block byte count, and section 12.5
measures that the stride from one level of a chain to the next is neither
this product nor that reach.  Section 12.5 is the rule to implement; this is
what the Windows binary computes for a standalone surface.

A second, looser transform exists at `0xa3ba0` for the Direct3D-visible
surface description (`0xa3de6..0xa3e28` DXT1, `0xa3e72..0xa3eb2` DXT2..5).  It
rewrites the runtime's width/height/pitch to `(W/2, 4H)` at 4 bits per texel
for DXT1 and `(W/4, 4H)` at 8 bits per texel for DXT2..5, with
`pitch = max(W/4, 2)` and `max(W/4, 4)` respectively, and clamps a minimum of
four (the "minimum-four block geometry").  Its product `pitch * rows` equals
`W*H` for both families, which is exact for DXT2..5 and **twice** the DXT1
block size.  Do not use `0xa3ba0` as an allocator: use section 4.3.  Whether
the DXT1 factor of two is deliberate slack in the D3D surface description or a
vendor inconsistency is **INF**; it does not affect the VRAM allocation, which
`0x45eb0` alone computes.

## 5. The descriptor (CAPTURED)

Descriptor builder `0x467e0` writes a triple at
`layout + 12*selector + 8`, which the emitter sends as indices `78`, `79`,
`7a` (`parhelia-m9-texture-state.md`).  Selector 2 is the sampler and selector
8 the render target.  Relevant to compressed textures:

| field | source | site |
|---|---|---|
| `79[31:25]` | hardware format code, `layout+0x140` | `0x46a35..0x46a4a` |
| `79[24]` | not written by this builder for selectors 2/8; `layout+0x14c` bit 1 for selectors 5..7 | `0x46c41..0x46c52` |
| `79[23]` | `layout+0x14c` bit 2 | `0x46a38..0x46a4a` |
| `79[18:16]` | kind low field, `layout+0x138[2:0]` | `0x46a16..0x46a28` |
| `79[15:0]` | padded width `layout+0x184` | `0x469fe`, `0x46bbf` |
| `7a[15:0]` | height `layout+0x12c` | `0x46864..0x4686e` |
| `7a[27:16]` | depth | `0x46a7f..0x46aa7` |
| `7a[31:28]` | resident level count, `layout+0x148` | `0x46c6e..0x46c85` |

The format code occupies `79[31:25]`, so the byte visible in a capture is
`code << 1 | bit24`.  This is confirmed independently by the render-class
helper's caller at `0x182126`, which recovers the code with
`shr eax, 0x19` before looking it up (CAPTURED), and by the captured Linux
descriptor `79 = 68180800` whose byte `0x68` is `0x34 << 1` for the
`A32B32G32R32F` code `0x34`.

### 5.1 Dimensions are texels, rounded up to whole blocks

`0x46aaa..0x46ae7`, reached only for **selector 3**:

```text
cmp ebx, 3                 ; selector 3
cmp edx, 0x10 / 0x15 / 0x17 ; the three DXT codes
  d1[15:0] = (d1[15:0] + 3) & 0xfffc
  d2[15:0] = (d2[15:0] + 3) & 0xfffc
```

Selector 3 sources its width from the **unpadded** request `layout+0x128`
(`0x46a0b`).  Rounding a raw requested extent up to a multiple of four is only
meaningful if the field counts texels, so the descriptor's width and height
fields are texel counts for compressed formats as for every other format, and
they must be whole multiples of four (CAPTURED + DERIVED).  Nothing in the
builder ever divides a compressed dimension by four.

### 5.2 Open: padded or logical width in the sampler descriptor

For selector 2 the Windows builder takes `layout+0x184`, the **padded** width
from section 4.1, while the captured Linux sampler descriptor for a 13x7
texture carries the logical width 13 (`79 = c801000d`,
`parhelia-m9-texture-storage.md`).  For a power-of-two texture at or above the
granularity the two agree, and the Windows Direct3D 9 path only creates such
textures, so the binaries do not separate the two readings.  This is an
**open question**, not a rule.  It is settled by one capture: create a 48x48
and a 13x7 DXT1 texture through the Direct3D path and log the emitted `79`,
per hook 1 of `parhelia-m9-hardware-gap-capture-harness.md`.  Until then a new
driver should emit the logical width, matching the Linux capture, and must
keep the storage padded per section 4.

## 6. Block storage and tiling (CAPTURED)

The saved Linux kernel library selects a dedicated address calculator per
element family.  The compressed rows are `m9x710`/`m9x331`/`m9x341`/`m9x273`
for DXT1 and `m9x521`/`m9x8`/`m9x911`/`m9x118` for DXT2..5
(`parhelia-m9-backing-layout-closure.md`), already transcribed in
`tools/m9-backing-layout-closure-evidence.py`.  Two properties an implementer
must not guess:

**The calculators take texel coordinates, not block coordinates.**  DXT1 uses
`(x & 0xC) << 1` and `(y & 0xC) << 3`, so `x[1:0]` and `y[1:0]` are discarded
and `x[3:2]`/`y[3:2]` become the low block indices; `x = 0,1,2,3` all return
the same address while `x = 4` advances by the block size.  Under a
block-coordinate reading, `bx[1:0]` would be dropped entirely and four distinct
blocks would alias, which is impossible (DERIVED, verified numerically).  Pass
the texel coordinate of the block's top-left corner.

**The in-block permutation is a 128-byte microtile whose texel extent equals
the class's width and height granularity.**  Evaluating the transcribed
formulas with a flat calculator state gives:

```text
DXT1 (class 7): 4x4 blocks, 16x16 texels, 128 bytes
  offset(bx,by) = by*0x20 + bx*0x08
DXT2-5 (class 6): 4x2 blocks, 16x8 texels, 128 bytes
  offset(bx,by) = (bx>>1)*0x40 + by*0x20 + (bx&1)*0x10
```

16x16 and 16x8 are exactly `T_W`/`T_H` for classes 7 and 6 in the *Windows*
driver's alignment tables (section 4.1).  Two unrelated binaries agree on the
microtile extent, which is why the compressed width and height padding rules
are safe to implement.  The same relation holds for the uncompressed classes:
class 2 is 16x4 texels, matching the captured 32-bit calculator's
`A[4:2]=x[2:0], A[6:5]=y[1:0], A7=x3` cell (DERIVED).

Above the microtile the coarse bits are the ordinary XOR/bank swizzle of the
class-3 calculator family, with the DXT1 page shift 5 and the DXT2..5 page
shift 4 where the uncompressed 16-bit family uses 3.  Those terms depend on
memory-controller register `0x0f04`, so a driver must read it after controller
initialisation rather than hard-code a permutation.

There is **no separate compressed layout family beyond these two classes**: the
zero-tiled mode uses the single linear DXT routine `m9x990`, and surface flag
`+0x154` bit 6 selects the override `m9x382` shared with uncompressed formats
(CAPTURED).

## 7. Mip chain (CAPTURED, superseded by section 12.5)

The static reading below is what the Windows binary implies.  **Section 12.5
measures the chain on an M9128 and the measured rule differs**: the stride is
not `size[L-1]` of section 4.3, and the padding granularities depend on the
storage kind the level is written through.  Implement section 12.5.

The chain rules are the uncompressed ones, applied to texel extents:

```c
width[L]  = MAX2(width[0]  >> L, 1);
height[L] = MAX2(height[0] >> L, 1);
depth[L]  = MAX2(depth[0]  >> L, 1);
offset[0] = 0;
offset[L] = offset[L-1] + size[L-1];      /* size from section 4.3 */
```

with each level start aligned to `T_A` and the whole chain to `T_G`.  Once a
level's padded extent falls to 16x16 or below, the small-level clamp of
section 4.2 puts it in the shared 32x16 tail at the origins listed there.

Levels below 4x4 texels still occupy one whole 4x4 block.  The saved OpenGL
ICD makes this explicit: `MxMGLX 0x100dccb0..0x100dcd05` clamps a compressed
level to `w' = MAX2(w,4)`, `h' = MAX2(h,4)` before computing
`rowPitch = ((w'+3)>>2) * blockBytes` and `bytes = ((h'+3)>>2) * rowPitch`
(CAPTURED).  **The minimum compressed level is 4x4 texels = one block**, and a
2x2 or 1x1 level is stored as one block whose upper-left texels are the level
(DERIVED).

`7a[31:28]` counts the resident levels, exactly as for uncompressed textures.

## 8. Upload (CAPTURED)

Compressed data uses the same path as uncompressed data, and the hardware
never decompresses during upload:

* The GPU copy/blit descriptor builder `0x47890`, called only from the
  resource-copy relocation builder at `0x1b6e6`, normalises the surface's
  format code to a raw storage code of the same element size
  (`0x478f5..0x4799d`): 8 bpt -> `0x30`, 16 bpt -> `0x50`, 32 bpt -> `0x78`,
  64 bpt -> `0x3b`, 128 bpt -> `0x34`, **DXT1 -> `0x10`** (`0x47901`), **DXT2..5
  -> `0x14`** (`0x4798d`).  The compressed surfaces keep a compressed code, so
  the transfer uses the compressed tiling and moves blocks verbatim.  `0x14` is
  a raw-copy code with no entry in the format table; do not use it as a sampler
  format.
* The saved OpenGL ICD's `glCompressedTexImage2D` at `MxMGLX 0x100d5630`
  validates `imageSize` against the `(w+3)>>2 * blockBytes` helper
  (`0x10125a90`) and then `memcpy`s the user blocks into the level
  (`0x100d5dd2` calling `0x1072e470`).  The blocks are stored unchanged.
* The ICD does contain a complete software DXT1/DXT3/DXT5 decoder at
  `MxMGLX 0x10125b10`, but it is a fallback, used only when the destination
  level already exists in an uncompressed format or when the forced-decompress
  LOD override at context `+0x208c` / `+0x2084` matches; that path re-uploads
  the level as `GL_RGBA8`.  There is no DXT encoder anywhere in the image, and
  no generic `GL_COMPRESSED_*` internal format.
* The allocation is the exact block byte count (section 4.3), which leaves no
  room for a decompressed copy (DERIVED).

## 9. What the vendor advertises (CAPTURED)

`MxMD`'s per-format Direct3D operation table gives all five DXT rows the same
mask `0x0080a007` (`parhelia-m9-format-operation-matrix.md`): 2-D texture,
volume texture, cube texture, convert-to-ARGB, sRGB sampler read, vertex
texture.  Notably **absent**: offscreen render target, automatic mipmap
generation, and display/3-D-acceleration mode bits.  Compressed surfaces are
sampler sources only.

`MxMGLX` advertises `GL_ARB_texture_compression` (`0x107e1680`),
`GL_S3_s3tc` (`0x107e17c8`) and `GL_EXT_texture_compression_s3tc`
(`0x107e1a20`), enumerates exactly four formats through
`GL_COMPRESSED_TEXTURE_FORMATS` - `0x83f0`, `0x83f1`, `0x83f2`, `0x83f3`
(`0x100cc9f9..0x100cca0d`) - and reports `GL_NUM_COMPRESSED_TEXTURE_FORMATS`
= 4 (`0x100ccbb4`).  The legacy `GL_S3_s3tc` enums fold onto the same four:
`0x83a0`/`0x83a1` -> DXT1 RGB, `0x83a2` -> DXT1 RGBA, `0x83a3` -> DXT3
(`0x100f0f90` jump table).  There is no `GL_ATI_texture_compression_3dc`,
`GL_EXT_texture_compression_dxt1`, LATC or RGTC anywhere in the image.

The whole S3TC path in the ICD is gated on a device capability bit:
`0x1000bb00` returns bit 0 of the word at device `+0x1c8`/`+0x1a0`, and every
S3TC enum rejects with `GL_INVALID_ENUM` when it is clear.  The saved *Linux*
ICD is the negative counterpart: it rejects the public compressed tokens
outright, which is why the existing live capture reports
`compressed_count=4 returned_formats=0`.

## 10. Implementation summary

```c
struct m9_dxt {
   enum pipe_format  format;
   uint8_t           code;        /* 79[31:25] */
   uint8_t           bits;        /* 4 or 8    */
   uint8_t           block_bytes;
};
/* DXT1_RGB, DXT1_RGBA -> 0x10, 4, 8
   DXT3_RGBA           -> 0x15, 8, 16
   DXT5_RGBA           -> 0x17, 8, 16 */
```

1. Advertise the three formats for `PIPE_TEXTURE_2D`, `CUBE` and `3D` sampler
   views and for vertex sampling.  Do **not** advertise render target, blit
   destination with format conversion, or automatic mipmap generation.  sRGB
   read is declared by the vendor but has no recovered decode; leave it off.
2. Allocate with the section 4.3 formula on texel extents padded by the
   section 4.1 granularities (width to at least 32, height to 16 for DXT1 and
   8 for DXT2..5), then the section 4.2 small-level clamp, `T_A` per level and
   `T_G` for the object.
3. Place blocks with the DXT1 or DXT2..5 class-3 calculator, passing the
   block's top-left **texel** coordinate.  A Gallium transfer hands out
   block-linear rows (`util_format_get_stride`, `util_format_get_nblocksy`);
   unmap copies one block at a time into the calculator address.
4. Build the descriptor as `79 = code<<25 | kind<<16 | width`,
   `7a = levels<<28 | depth<<16 | height`, with width and height multiples of
   four, and keep the existing filter, wrap and LOD words unchanged.
5. Upload through the existing texture window/`UPLOAD_RENDER` path with the
   surface described by its compressed code.  Never route a compressed
   resource through the 4-byte-texel copy, A1 packing, linear-resolve or
   BITBLT helpers.
6. Treat DXT2 as DXT3 and DXT4 as DXT5 if a state tracker ever presents them.

## 11. Open questions, and the capture that closes each

| question | why static analysis cannot close it | capture |
|---|---|---|
| Does `79[15:0]` carry the logical or the padded width for a compressed NPOT texture? | The Windows path only ever creates POT textures at or above the granularity, where the two coincide (section 5.2) | Create 48x48 and 13x7 DXT1 textures through Direct3D and log the emitted `79`/`7a`; harness hook 1 |
| Are the `T_W`/`T_H` `sel = 1` rows (64/32 wide, 64/32 tall) ever selected for a texture? | `sel` comes from a resource bit whose producer is a device-policy path, not a format property | Log `layout+0x16c`, `+0x170`, `+0x184`, `+0x188`, `+0x180` at `0x45eb0` return for DXT1 and DXT5 at 8x8, 64x64, 256x256 |
| Does the sampler follow standard BC1 punch-through when `color0 <= color1`? | DXT1 RGB and DXT1 RGBA share code `0x10` and the ICD's own decoder is a host fallback | Draw a DXT1 texture with blocks on both sides of the endpoint condition and read back the rendered pixels |
| Does bit `79[24]` mean anything for a compressed sampler descriptor? | The builder never writes it on selector 2 or 8; it is left at whatever the previous descriptor held | Log the full `79` for a freshly created DXT resource before any other bind |
| Is the DXT1 factor of two in `0xa3ba0` deliberate? | Only the D3D surface description is affected, and no consumer of `+0xf4` was joined to a VRAM size | Log `0xa3ba0`'s outputs `+0xe8/+0xec/+0xf0/+0xf4` alongside `0x45eb0`'s `+0x180` for the same DXT1 surface |
| Do compressed cube faces and volume slices follow the uncompressed face/slice base rules? | The constructors are format-independent, but no compressed case is in any capture | Upload six uniquely coded DXT1 faces and four DXT5 slices and compare raw VRAM against the calculator |

The first target-hardware test remains the one already named in
`parhelia-mesa-s3tc-implementation.md`: a 32x16 DXT1 texture in which each of
the 32 blocks has a unique solid colour detects block order, x/y swap, microtile
extent and the descriptor code in one image.  On the M9 it must be repeated at
32x16 for DXT5, because the two families have different microtile heights.

## 12. MEASURED on an M9128

The first hardware data on M9 S3TC storage, from
`tools/m9-compressed-tiling-probe.c`.  It writes a block image through the
descriptor-translated surface window with a compressed format code, then
reads the object's raw bytes back through its CPU mapping, so every block
names the address it landed on.  Each block carries its own index, and the
same run against the uncompressed `0xcc` code reproduces the 32-bit address
calculator the Gallium driver already uses -- `y = 4, x = 0` at `0x300`,
`A[4:2] = x[2:0]`, `A[6:5] = y[1:0]`, `A7 = x3` -- so the method is checked
before it is trusted.

### 12.1 The window de-tiles a compressed surface, in blocks

The hardware window takes codes `0x20`, `0x2a` and `0x2e` and presents the
level as a **linear block image**: window byte offset `(by * blocks_per_row +
bx) * block_bytes` is block `(bx, by)`.  Every block comes back whole at one
raw address, which the block reading predicts and a 4-bit-per-texel scanline
reading does not -- that would split each block across four window rows.  A
driver therefore does not need the compressed address calculator at all: the
window places the blocks, exactly as it already does for large uncompressed
levels.

The measured placement inside a level (DXT1, 8 bytes a block, the first
twelve block columns of the first eight block rows; the table is the same at
64 and at 128 texels wide):

```text
row 0:      0      8     10     18    200    208    210    218    d00 ...
row 1:     20     28     30     38    220    228    230    238    d20 ...
row 2:     40     48     50     58    240    248    250    258    d40 ...
row 3:     60     68     70     78    260    268    270    278    d60 ...
row 4:    300    308    310    318    100    108    110    118    e00 ...
```

Inside a 4x4-block microtile the offset is `by*0x20 + bx*0x08`, which is
section 6's `offset(bx,by)` confirmed, and the microtile is 128 bytes over
16x16 texels as section 6 derives.  Above it the microtile bases are
`(BX,BY) = (0,0) 0x000, (1,0) 0x200, (2,0) 0xd00, (0,1) 0x300, (1,1) 0x100,
(2,1) 0xe00` -- the bank swizzle, which does **not** depend on the surface
width in the measured range.  DXT3 places 16-byte blocks as `(bx>>1)*0x40 +
by*0x20 + (bx&1)*0x10` inside a 4x2-block microtile, again as section 6
derives.  No formula is fitted here, because none is needed.

### 12.2 Allocation

The last byte a level reaches, against the block byte count it holds:

| level | DXT1 last / content | DXT3 last / content |
|---|---:|---:|
| 32x16 | 0x280 / 256 | 0x380 / 512 |
| 32x32 | 0x380 / 512 | 0xb80 / 1024 |
| 64x16 | 0xf80 / 512 | 0xf80 / 1024 |
| 64x32 | 0xf80 / 1024 | 0xf80 / 2048 |
| 64x64 | 0xf80 / 2048 | 0x1000 / 4096 |
| 96x48 | 0xf80 / 2304 | 0x1b80 / 4608 |
| 128x32 | 0xf80 / 2048 | 0x1000 / 4096 |
| 128x128 | 0x2000 / 8192 | 0x4000 / 16384 |
| 256x256 | 0x8000 / 32768 | 0x10000 / 65536 |

So section 4.3's "exactly the block byte count, no decompression reserve" is
true only for a level at or above 128x128; below that the swizzle spreads the
blocks and the level needs up to eight times its content.  Every measured
case is covered by

```c
pad_w  = ALIGN(width, 32);
pad_h  = ALIGN(height, bits_per_texel == 4 ? 64 : 32);
bytes  = ALIGN(pad_w * pad_h * bits_per_texel / 8, 0x1000);
```

which is section 4.1's `sel = 1` granularity rows with a 4 KiB quantum -- so
the open question "are the `sel = 1` rows ever selected" is answered **yes**
for a tiled compressed level, by measurement.  `ALIGN(content, 0x1000)`
happens to cover the same cases but is tight at 64x64 DXT3 and 128x32 DXT3,
where it equals the last byte exactly; the padded rule keeps a margin.

### 12.3 Sampling

The Gallium driver stores a level through the window and samples it, and the
fixture `tools/m9-mesa-glsl` builds the blocks itself: block *n* gets the
endpoint pair `(37n+1, 91n+0x2345)` as RGB565 and a selector pattern
`(tx + ty + n) & 3` that uses all four codes, so a decoder that ignored the
selector bits, swapped the block axes or mis-ordered the blocks would not
reproduce the reference.  Never a solid fill.

MEASURED on an M9128, every case pixel-exact against a software BC1 decode
within one 5-bit endpoint step:

| case | what it covers |
|---|---|
| `dxt1` 64x32, `dxt1:132x64`, `dxt1:256x128`, `dxt1:32x16` | four-colour DXT1 at the small and the tiled storage sizes |
| `dxt1pt` | every block on the punch-through side of the endpoint condition |
| `dxt3`, `dxt3:128x68` | explicit four-bit alpha, code `0x2a` |
| `dxt5`, `dxt5:52x28` | interpolated three-bit alpha, code `0x2e` |
| `dxt1:128x68`, `dxt1:68x36`, `dxt1:52x28` | heights that do not reach a whole microtile, widths that are not their span |

Three of the document's open questions are answered:

* **`79[15:0]` carries the logical width, not the padded one.**
  `dxt1:132x64` samples exactly with `132` in the descriptor while its
  storage span is `160`.  The Linux capture's reading is the right one and
  the Windows builder's `layout+0x184` is not what a sampler needs
  (section 5.2).
* **The sampler follows standard BC1 punch-through.**  `dxt1pt` puts every
  block on the `color0 <= color1` side, and the three-colour decode with a
  transparent fourth code is what comes back, alpha included.
* **The `sel = 1` granularity rows are selected for a tiled compressed
  level** (section 12.2), and the storage kind is the one the level was
  written through: the small 2-D kind up to 64 in both dimensions and the
  ordinary tiled kind above it, exactly as for an uncompressed level.  A
  64x32 level read as the tiled kind came back wrong in red and green while
  the same blocks read as the small kind are exact.

The level's height must reach a whole microtile in the *window* descriptor
-- 16 texel rows for DXT1 and 8 for DXT2..5 -- or the last rows come back
wrong; the rows past the level are never written, so the padding costs
nothing.  The sampler descriptor keeps the logical height.

The whole fixture suite, with the S3TC and the 10-bit GigaColor cases in it,
is **221 of 221** on the M9128 with `GL_RENDERER` asserted on every case.

### 12.5 The mip chain, MEASURED

The sampler computes each level's address itself, from level 0's extent and
the format alone: moving a level with the Gallium driver's
`M9X_DXT_LEVEL_OFFSETS` override does not move the level above it.  A chain
can therefore only be placed where the hardware looks for it, and the sweep
that finds each level is the measurement -- the fixture writes level *L* at a
candidate offset, pins the sampler's LOD to *L* and compares the decode.

```c
/* the level's own storage kind, as in section 12.3 */
span32 = ALIGN(w, 32);
lines  = ALIGN(h, bits_per_texel == 4 ? 16 : 8);
small  = span32 <= 64 && lines <= 64;

/* what the next level starts after */
stride = small ? span32 * h * bits_per_texel / 8
               : ALIGN(w, 64) * ALIGN(h, bits_per_texel == 4 ? 64 : 32)
                              * bits_per_texel / 8;
offset[L] = ALIGN(offset[L-1] + stride[L-1], 0x200);
```

The measured bases, each found by a sweep in 0x100 or 0x200 steps (the
window's own surface address quantises to 0x200, so two adjacent candidates
can both pass):

| chain | level | extent | base | what it settles |
|---|---:|---|---:|---|
| 64x32 DXT1 | 1 | 32x16 | `0x400` | the stride is the content, not the 4 KiB reach |
| 64x32 DXT1 | 2 | 16x8 | `0x600` | a level of 0x100 bytes still advances by 0x200 |
| 64x16 DXT1 | 1 | 32x8 | `0x200` | a small level's height is not padded |
| 64x64 DXT1 | 2 | 16x16 | `0xa00` | a 32x32 small level advances by 0x200 |
| 64x32 DXT3 | 1 | 32x16 | `0x800` | the 8-bit family doubles it |
| 64x32 DXT3 | 2 | 16x8 | `0xa00` | |
| 256x64 DXT1 | 1 | 128x32 | `0x2000` | |
| 256x64 DXT1 | 2 | 64x16 | `0x3000` | a tiled level pads its height to 64 |
| 132x68 DXT1 | 1 | 66x34 | `0x3000` | a tiled level pads its width to **64**, not 32 |
| 256x256 DXT3 | 4 | 16x16 | `0x15400` | the tail follows a 32x32 level by 0x400 |

Two things the earlier sections get wrong:

* **The width granularity of a tiled level is 64 texels, not the 32 the
  window descriptor carries.**  A 132-wide DXT1 level is stored through a
  160-column window and advances the chain by 192 columns.  Section 4.1's
  `T_W` `sel = 1` row is the one selected, as section 12.2 already found for
  the height.
* **A small-kind level is charged its own height**, with no microtile
  padding.  The 0x200 per-level alignment absorbs the difference in every
  case, because 0x200 bytes is exactly one microtile row group at both
  spans.

**The packed tail of section 4.2 is real for compressed levels.**  Every
level of 16 texels or less in both dimensions shares one 32x16-texel surface
at the uncompressed capture's origins, X `[0,16,24,16,24]` and Y
`[0,0,0,8,8]`; a chain never has more than five such levels.  The tail base
is the next 0x200-aligned offset after the last level with a surface of its
own, and each tail level is written through the window as a sub-rectangle of
that shared surface -- span 32, height 16, the small storage kind, and the
level's origin in blocks.  Level 2 of a 64x32 DXT1 chain is the only base all
five of its levels sample from, so the tail is measured here rather than
inherited.

The allocation still has to hold the bank swizzle's reach of section 12.2,
which below 128x128 is several times a level's own bytes, so an object is
sized by the larger of the stride and that padded bound.

MEASURED on an M9128, every level pixel-exact against a software BC decode
(fixture `dxtmip1`, `dxtmip1pt`, `dxtmip3`, `dxtmip5`, with the same `:WxH`
suffix the single-level cases take): 15 chains over DXT1, DXT3 and DXT5 at
256x256, 256x128, 256x64, 128x128, 132x68, 64x64, 64x32 and 52x28, 155 levels
in all, with the ten single-level `dxt*` cases and a known-good fixture
passing in the same run.  The whole `m9-mesa-glsl` suite is 276 of 276.

### 12.6 A level of 64 columns or less that is too tall for the small kind

`dxt1:64x128` and `dxt1:32x128` do **not** sample correctly when they are
written through the tiled kind: 15 of every 16 pixels are wrong at 64
columns, and 7 of every 8 at 32 (MEASURED).  The emulated vendor layout
explains why (`captures/m9-vendor-dxt-layout-20261009`, 2026-10-09).  The
vendor stores both levels through the **small** kind, because the small kind
reaches a span of 64 and up to 256 bytes of blocks per column: 128 texel rows
for DXT1 and 64 for DXT2..5.  DXT1 64x129, 32x144 and 16x144 and DXT3 64x65
are tiled, and the vendor's aperture view of a narrow tiled level such as
DXT3 64x69 is `2a21 0040` / `0001 0060`.  The chain offsets of section 12.5
follow once the small kind uses this limit.  A 257x277 DXT1 chain advances
by `0xa00` past its 64x69 level, not `0x1000`.

Mesa m9x f499643 uses that limit and no longer refuses a narrow tiled level.
The kernel window admits the taller small kind only with drm 927f76a (pending
approval).  Until then, a DXT1 level of 65..128 rows at a span of 64 or less
is refused cleanly ("cannot be stored (-22)") and the sampler is clamped
below it.

The sampler descriptor carries the **logical** extent, not the extent rounded
to whole blocks (Mesa m9x bccb095, MEASURED).  The sampler derives every
level from it, so a 257x277 chain read as 260x280 put level 1 at 130 columns
and a 192-texel pitch, while the chain stores it 128 wide.
`dxtmip3/5:257x277`, `dxtmip3:130x70` and `dxtmip1:66x34` are pixel-exact at
every level with the logical extent and fail with the rounded one.

### 12.7 The kernel module has to match this tree

The window path for a compressed level is kernel-side
(`drivers/gpu/drm/m9x/m9_surface.c`, commits *address an S3TC level through
the surface window* and *a small S3TC level takes the small storage kind*).
With a module built before them, every compressed level store returns
`-EINVAL`, every compressed texture stays empty and every textured draw is
refused -- which is exactly how a stale module on the test target turned
ioquake3 into a black window with one line in the log.  MEASURED: the ten
`dxt*` fixtures fail against such a module and pass against one built from
this tree.

### 12.8 What is still not measured

* Cube faces and volume slices, which section 11 also lists.
* `79[24]`: the driver leaves it clear and everything above works.
* sRGB decode, which has no recovered encoding for this sampler.
* The narrow-tall DXT1 level of section 12.6 on the card (needs the kernel
  change).
