# M9148 texture storage layout

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

This note derives the texture storage layout and the texture-unit words.
It uses the vendor GL 1.5 groups 2/3 capture
(`captures/m9x-linux-gl15-ff-20260915`, `texture/batch.tgz`), whose texture
resources were read raw through host-aperture slot 0.  Every uploaded texel is
coordinate coded (`tools/m9x-texture-capture.c`).
`tools/m9-texture-layout-analyze.py` rebuilds every texel from that pattern,
places it with the model below and compares the dump bytes.  It also checks the
captured `70/79/7a/7c/7d/7e..81` words:

```text
python3 tools/m9-texture-layout-analyze.py
... cases 85 texels 9911 failing cases 0
```

Labels: **CAPTURED** = these vendor dumps and submissions, **STATIC** = saved
vendor binaries (`parhelia-m9-blocked-layout-boundary.md`,
`parhelia-m9-backing-layout-closure.md`), **MEASURED** = clean-room hardware.
Bytes the model does not own are VRAM left over from earlier cases; the analyzer
never compares them.

## 1. Storage is tiled, with a texture-specific permutation (CAPTURED)

Textures are not stored linearly.  The low address bits match the 8x4 cell
family of render targets: x[2:0], x3, y[1:0].  The coarse bits, however, follow
the vendor mode-2 aperture calculator `m9x659`, not the local calculator
`m9x474` that render targets use (MEASURED in `parhelia-m9-host-aperture-slots.md`).
The calculator state is depth 1, category 0, bank masks 0:

```text
A[1:0] = 0            A[4:2] = x[2:0]      A[6:5] = y[1:0]     A7 = x3
A8 = x4 ^ y2          A9 = y2 ^ y4 ^ x5 ^ z ^ y3
offset = A + (z + (y >> 3)) * 0x400
```

The two calculators differ from row 8 on.  The texel at (0,8) is at `0x600` and
the one at (0,12) at `0x500`.  The render-target formula would give `0xa00` and
`0x900`.

All rows 0..15, x up to 31 (through the mip tail) and z 0..3 are CAPTURED.
`glCopyTexImage2D` renders on the GPU into the texture at `11f89000`, and the
copied texels land at the same addresses as CPU uploads.  The layout is
therefore the hardware's, not a vendor CPU convention.

## 2. Mip chain: 32x16 packed tail (CAPTURED)

Every level whose extent is at most 16x16 shares one 32x16 atlas, starting at
the first such level.  The levels sit at X `[0,16,24,16,24]`, Y `[0,0,0,8,8]`,
which confirms the STATIC tail tables.  For the 16x16 chain:

| level | origin | example texel -> offset |
|---:|---|---|
| 0 16x16 | (0,0) | (8,0) `0x080`, (0,4) `0x300`, (0,8) `0x600`, (15,15) `0x5fc` |
| 1 8x8 | (16,0) | (0,0) `0x100`, (0,4) `0x200` |
| 2 4x4 | (24,0) | (0,0) `0x180`, (3,3) `0x1ec` |
| 3 2x2 | (16,8) | (0,0) `0x700`, (1,1) `0x724` |
| 4 1x1 | (24,8) | `0x780` |

* **Resident levels (CAPTURED).**  The vendor stores the base level plus
  `7a[31:28]` further levels.  `7a[31:28]` is 0 unless the minification filter
  is mipmapped.  Storage is additionally cut at `GL_TEXTURE_MAX_LEVEL`, although
  `7a` still counts the whole chain: `mip_max_level_2` keeps level 3 and 4
  regions stale.  `glGenerateMipmap` levels are exact 2x2 box averages.
* **Base level (CAPTURED).**  With `GL_TEXTURE_BASE_LEVEL` 1 the vendor rebuilds
  the resource from the base.  `79/7a` carry the 8x8 base dimensions, and level
  1 is stored at the tail origin (0,0).
* **NPOT and rectangle (CAPTURED).**  13x7 uses the same placement.
  `79[15:0]` is the unaligned width (`c801000d`), and the rectangle target only
  changes the R wrap in `7c`.
* **Cube (CAPTURED 4x4).**  Face f starts at `f * 0x800`, one 32x16 32-bit face
  allocation; the faces are ordered +X, -X, +Y, -Y, +Z, -Z.
* **3-D (CAPTURED 4x4x4).**  z is a calculator input: slices z=1,2,3 start at
  `0x600`, `0x800` and `0xe00`.  With this calculator state the slice pages
  collide with rows 8 and up, so heights above 8 need a different state
  (not captured).
* **1-D (CAPTURED).**  Descriptor kind `0x18` stores a linear row: texel x is at
  `4*x`.

Levels larger than 16x16 are not covered by any dump.  STATIC: their bases follow
`offset[L] = offset[L-1] + size[L-1]`, and the vendor allocation contract gives
the sizes.  The in-level permutation for them is **not captured**; see section 6.

**Large levels (CAPTURED, `captures/m9x-vendor-texture-layout-20260915`,
`tools/m9-texture-large-analyze.py`).**  Every level of the POT 32..1024 chains,
NPOT 100x37/300x300 and the 300x200 rectangle is present exactly once.  Levels
are consecutive: 64x64 level 0 at `0x0`, level 1 at `0x4000`, the 16x16 tail
region at `0x5000`; 1024x1024 level 1 at `0x400000`.

* **POT levels are GF(2)-linear.**  `offset = base + (X[x] ^ Y[y]) * 4` with
  X/Y linear in the coordinate bits, for every texel of every POT level (the
  1024x1024 level 0 has all 1048576 texels separable).  The basis depends only
  on the level size: X = `1 2 4 20 40 80` up to 64, and `1 2 4 20 40 780 900
  1300 2000 4000` (prefix by size) from 128; Y = `8 10 1c0 280 480 800` at 64,
  and `8 10 c0 280 100` followed by `w*32|200`, `w*64`, `w*128`, ... from 128.
* **NPOT levels above 64 are not separable** (300x300 level 0: 53632 of 90000
  texels, 150x150 and 75x75 similar, 300x200 rectangle 35968 of 60000); their
  calculator state differs and is not recovered.  NPOT levels of 64 or less
  are separable with small-size bases.
* **Descriptor windows (CAPTURED, read-only on the vendor host).**  Slot 0 with
  the texture descriptor presents level 0 as linear rows: 64x64 with kind `01`
  (`c8010040`) 4096/4096, 128x128 with kind `21` (`c8210080`) 16384/16384; the
  other kind gives 256.  NPOT 100x37 and the 300x200 rectangle with their
  unaligned width in the descriptor gave no linear view (32-aligned spans were
  not captured).

Driver consequence: POT textures can be placed from the basis model or written
through a kind 01 (<= 64) or kind 21 window; NPOT textures above 64 should use
the render-target storage family, whose `cc21` window is hardware-verified,
until their texture calculator state is recovered.

## 3. Formats and byte order (CAPTURED)

`79[31:24]` is the hardware format code `<<1`, plus bit 24.  Bit 24 is set for
the alpha-less RGB8, RGB5, L8 and D24 codes.  Quantization is
`(v * max + 127) / 255`.

| internal format | `79[31:24]` | element | stored bytes |
|---|---:|---:|---|
| RGBA8 | `c8` | 4 | R G B A |
| RGB8 | `c9` | 4 | R G B, uploaded A kept |
| RGBA8 uploaded as BGRA | `cc` | 4 | B G R A |
| RGB5, R3_G3_B2 | `9d` | 2 | LE `R5<<11 \| G6<<5 \| B5` |
| RGBA4 | `8c` | 2 | LE `A<<12 \| R<<8 \| G<<4 \| B` |
| RGB5_A1 | `94` | 2 | LE `A1<<15 \| R<<10 \| G<<5 \| B` |
| ALPHA8 | `66` | 1 | A |
| LUMINANCE8 | `61` | 1 | L |
| LUMINANCE8_ALPHA8 | `a2` | 2 | L A |
| INTENSITY8 | `a2` | 2 | I I (stored as LA) |
| DEPTH16 | `b8` | 2 | LE `round(d * 0xffff)` |
| DEPTH24 | `fa` | 4 | LE `round(d * 0xffffff) << 8` |
| DEPTH32 | `f8` | 4 | LE `round(d * 0xffffffff)` |

Compressed (S3TC) and native 10-bit formats are not in this table.  Both are
decoded in `parhelia-m9-s3tc-compressed-textures.md` and
`parhelia-m9-gigacolor-10bit.md`: the 10-bit codes `0x68`/`0x6a` are ordinary
32-bit-per-texel surfaces with this same placement, while the DXT codes
`0x10`/`0x15`/`0x17` are 4- and 8-bit-per-texel storage classes of their own.
The format byte is `code << 1 | bit24` in every case.

Pixel-type uploads (`UNSIGNED_SHORT`, `FLOAT`, `4444`, `5551`, `565`, `8888`,
`8888_REV`) are converted into the RGBA8 layout.  16-bit elements use rows of
16 elements (`0x20` bytes), and 8-bit elements rows of `0x10` bytes.  This
matches the STATIC `m9x259`/`m9x638` transcriptions.  For those widths only
4x4 textures (x, y < 4) are CAPTURED.

## 4. Texture-unit words (CAPTURED)

| word | encoding |
|---|---|
| `70+u` | `00040018 \| form<<8 \| lodctl<<19 \| cmp<<26 \| func<<27`; form 1 = 2-D/1-D/rect, 2 = 3-D or compare, 3 = cube; `lodctl` set when `7d != 0ff80000`; func NEVER 0, LESS 1, LEQUAL 2, EQUAL 3, NOTEQUAL 4, GEQUAL 5, GREATER 6, ALWAYS 7 |
| `78` | resource base (TEXTURE relocation, offset 0 for every level and face) |
| `79` | `format<<24 \| kind<<16 \| width`; kind `01` 2-D, `09` cube, `11` 3-D, `18` 1-D, `21` ordinary tiled (128x128 FBO and q3 textures) |
| `7a` | `resident_levels<<28 \| depth<<16 \| height` |
| `7c` | `0x9200 \| minLINEAR<<1 \| magLINEAR<<4 \| mipNEAREST<<7 \| mipLINEAR<<8 \| NMN+magLINEAR<<10 \| wrapS<<22 \| wrapT<<25 \| wrapR<<28 \| DEPTH16raw<<31` |
| `7d` | bias `[9:0]` signed 5 fraction bits; min LOD `[18:10]` and max LOD `[27:19]` in 1/32; default `0ff80000`; `GL_TEXTURE_MAX_LEVEL` and `MAX_LOD` both lower `[27:19]` (relative to the base) |
| `7e`,`7f` | border `G<<24 \| R<<8`, `A<<24 \| B<<8` (8-bit channels); `80=ffffffff`, `81=0` |

Wrap codes: REPEAT 0, CLAMP_TO_EDGE 1, MIRRORED_REPEAT 4, CLAMP 6,
CLAMP_TO_BORDER 7.  R comes from the object state (2-D default REPEAT, rectangle
EDGE).  Exceptions the analyzer lists explicitly:

* copy targets get `7a=2001xxxx`, because the vendor allocates a chain for
  `glCopyTexImage2D`;
* bit 10 is captured only for NEAREST_MIPMAP_NEAREST with LINEAR mag.  ioquake3
  LINEAR_MIPMAP_NEAREST with LINEAR mag is `0x9292`, without bit 10.
  NEAREST_MIPMAP_LINEAR and LINEAR_MIPMAP_LINEAR with LINEAR mag are not
  captured.

**Packet order.**  Each unit is emitted as `70+u`, then `78 79 7a`, then
`7c 7d 87`, then `80 81 7e 7f`.  The raster pass emits `74..77=100`, the bound
units, and zero `70+u` for the rest.  Fixed-function streams add `82..85` after
`87`; GLSL streams (`fs-tex2d`, `fs-tex2d-two`) do not.  Texel pair k is
hardware unit k.

**Raster declaration.**  A texture coordinate stream follows the varying
streams.  Texture k reads its coordinate stream s as the single word
`(5+k)<<20 | s<<16 | 0688`: `fs-tex2d` `00500688`, `fs-tex2d-two`
`00500688 00600688`, `pr-tex-mul-color` `00510688`.

## 5. Kernel change specification (not implemented)

The captured tail permutation lets Mesa place levels up to 16x16 directly
through the raw VRAM mapping.  Larger levels need the hardware to apply the
permutation itself.  The proposal extends `DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS`
without changing the ABI size:

1. `flags` bit 0 `PARHELIA_M9_ACCESS_TEXTURE`: `pad` is the byte offset of the
   level/face inside the BO (256-aligned).  All other flag bits stay zero.
2. The BO must be `MEMORY_CLASS_VRAM`.  It does not have to be `BO_TILED`:
   texture BOs are plain VRAM objects that userspace may also map raw.
3. `descriptor0 = format<<24 | kind<<16 | width` with:
   * format in `{c8,c9,cc,9d,8c,94,66,61,a2,b8,fa,f8}`;
   * kind `01` (width and height at most 64) or `21` (either dimension above
     64, `width` a multiple of 32);
   * `surface_width == width`.
   `descriptor1 = 1<<16 | height`, with `[31:28]` zero.
4. The element size follows from the format (4, 2 or 1).  `data_stride` must
   be at least `width * element`, and the copy loop steps by element.
5. Bounds:
   * `pad + extent <= gem->size`;
   * the window extent must not exceed `M9_ADDRESS_SURFACE_BYTES`;
   * extent is `0x800` for a level inside the 32x16 tail, otherwise
     `m9_surface_layout_bytes(width, height, element)`.
6. Program the slot-1 window with `d1 = vram_offset + pad`, `d2 = descriptor0`
   and `d3 = descriptor1`.  Locking and fence waits are the same as for the
   colour path: writes wait for readers, since sampling jobs hold READ.
7. **Prerequisite, MEASURED first.**  A slot-1 window carrying a texture
   descriptor must present linear rows.  Its writes must land at the CAPTURED
   tail addresses (16x16 chain) before the kernel accepts kind `01`.  Then a
   64x64 and a 128x128 coordinate-coded texture must be written, and their raw
   placement recorded, before those sizes are enabled.
8. Optional hardening in `m9_submit.c`: for index 78, check that the `79/7a`
   extent fits the relocated BO (minimum `0x800`) so the sampler cannot read
   past the object.

## 6. Mesa m9x implementation

* `m9_texture.c`:
  * layout from the tail rule;
  * `m9x659` placement;
  * `-ENOTSUP` outside the captured domain (levels above 16x16, 3-D taller
    than 8);
  * store/fetch with format conversion;
  * unit words.
* **32-bit storage.**  All sampled formats are stored at 32 bits, the only
  width with captured placement beyond 4x4:
  * RGBA8, BGRA4, BGR5A1, A8, L8A8 and I8 as `c8`;
  * RGBX8, BGRX8, B5G6R5 and L8 as `c9`;
  * BGRA8 as `cc`.
* **Transfers.**  `texture_map` hands out pipe-format rows; unmap places them.
  Levels without captured placement use the proposed texture window (section 5),
  which today's kernel rejects.  Such levels are marked unplaced, and draws that
  sample them are skipped with a warning.
* **Sampling.**
  * Sampler states and views are tracked per fragment slot.
  * The raster pass emits the captured unit block with TEXTURE relocations.
  * Coordinate streams come from CPU transient views with the section 4
    declaration.
  * Up to two samplers (`M9_FS_MAX_TEXTURES`).
* **Tiled render targets.**  A tiled B8G8R8A8 target is sampled as kind `21` if
  its width is a multiple of 32 and it is not small.
* **Cube and volume.**  Implemented and measured on the M9128; the face and
  slice placement, the height/depth domain the captured calculator state
  actually covers, and what the driver advertises are in
  `parhelia-m9-cube-volume-textures.md`.
* **A base level above 0 is a LOD clamp** (MEASURED on the M9128).  The
  descriptor addresses the whole chain from one base, so a view with
  `first_level = N` keeps the chain resident and pins `7d`'s low bound to
  `N * 32`; no re-based copy is needed.  A packed tail level cannot be
  re-based this way and is refused, because the tail has no placement of its
  own.
* **A level is a render target** (MEASURED on the M9128).  Every level
  outside the packed tail is laid out by `m9_render_surface_layout` at its
  own extent, on a 256-aligned chain offset, and the colour attachment is
  relocated there -- which is what `glGenerateMipmap` needs to build the
  chain with draws instead of CPU transfers.  `m9_texture_renders_level`
  names the levels this holds for; the fixtures are `genmip:WxH[:rt]`.
* **Not implemented.**  Swizzled views and 1-D sampling.

## 7. Hardware test plan (T2 OS, clean-room kernel)

1. Offline: `tools/m9-texture-layout-analyze.py` and `m9-batch-test`.
2. After `insmod`, draw one untextured job first; the first textured draw is
   known to lock up otherwise.
3. Upload the 16x16 coordinate-coded chain through Mesa.  Read the BO raw and
   compare the captured offsets (section 2).
4. Run `fs-tex2d` with a 4x4 NEAREST/EDGE texture.  Read back through
   `SURFACE_ACCESS` and check every texel colour against its coordinate.
5. Run `tools/m9x-texture-capture.c` cases on Mesa: `mip_*`, `npot_*`,
   `wrap_*`, `fmt_*`, `type_*`.  Its built-in reference checks every sample.
6. `fs-tex2d-two`: unit 1 and the shared coordinate stream.
7. A 64x64 texture must warn and skip the draw without locking up.
8. Kernel texture window: step 7 of section 5, then repeat 5 with 64x64 and
   128x128 textures.

## 8. The kind byte is two fields, and one of its values is linear

Added 2026-09-18; the evidence is in `parhelia-m9-texture-upload-and-copy-re.md`.
`79[23:16]` is not an enum.  `[18:16]` is the allocation-layout selector
(0 = row-major, 1 = blocked), `[20:19]` the dimension (0 2-D, 1 cube, 2 volume,
3 1-D, 4 large 2-D) and `[21]` the local/aperture address family.  So kind `18`
is "1-D, **linear**", which is why its texels sit at `4*x`, and kinds `01` and
`21` differ only in the dimension and address-family bits.

Two consequences this note previously ruled out:

* the sampler reads **linear** storage — kind `18` is the captured proof, and
  a linear 2-D shape (kind byte `00` or `20`) has simply never been captured
  either way;
* a texture base may live in **system memory** — the vendor binds
  `78 = 40c3a001`, a class-1 GPUVA, with `79 = 68180800` (format `34`
  `A32B32G32R32F`, kind `18`, 2048 elements), to lower a dynamically indexed
  GLSL uniform array.

## 9. Mip chains and small surfaces, MEASURED 2026-10-02

* **A small level advances the chain by its aperture pages.**  A level of at
  most 64 in both dimensions is stored by the aperture calculator, and the
  sampler finds the next level `ceil(h / 8)` 1 KiB pages on at span 32 and
  `ceil(h / 4)` at span 64 -- not the render surface contract, whose 8-row
  height quantum is larger when the height is not a multiple of 8 at span
  64.  `M9X_TEXTURE_ADDRCODE` on a 100x37 chain read level 2 from `0x9400`
  (50x18 advanced by `0x1400`, the layout had `0x1800`).  POT chains are
  unchanged (64x64: tail at `0x5000`).
* **The tail levels sample wherever the chain places the atlas.**  With the
  rule above, every level of 16x16, 17x9, 1x16, 32x32, 32x64, 64x32, 40x31,
  36x60, 50x18, 64x20, 60x120, 100x37, 128x40, 128x128 and 256x64 samples
  exactly (`tools/m9-texlevel.c`: texels name column, row and level, one
  level forced by MIN_LOD = MAX_LOD).  The earlier clamp to the first tail
  level, and the refusal of a view based on a tail level, are gone.
* **`7d`'s bounds count from the descriptor's level 0**, Gallium's min/max
  LOD from the view's base level: a view based on level B uses
  `lo = 32 * (B + min_lod)`, `hi = 32 * (B + max_lod)`.  Views based on
  levels 1..4, tail levels included, sample the level asked for.
* **The small CPU window carries the 32-aligned span.**  Reading or writing
  a small render target through the window with its own width returned
  zeros for 1x1/4x4/9x9 BGRA images (st's blit upload target), which broke
  every later copy of such a level.
* **Both calculators place 2-D levels too.**  The aperture calculator at
  span 64 reproduces the captured 64x64 basis, and `m9x474` the 128+ basis
  (`x5 -> 0x780` texels).  Placing every 2-D level on the CPU with them
  sampled exactly in 44 level probes up to 2048x8 and 1024x1024, but was
  not faster than the kernel window on the VX900 host, so the driver keeps
  the window for levels above the tail.
* **DEPTH24 chains.**  A mipmapped depth image is stored as a texture with
  code `fa` (X8Z24/S8Z24 as is, Z16 widened); its levels sample their own
  depth.  The window moves such a level as `c8`.
