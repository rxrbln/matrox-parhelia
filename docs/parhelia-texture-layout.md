# Parhelia texture memory layout and format table

Companion to docs/parhelia-fragment-isa.md §11 (sampler register map) and §12
(texenv lowering) and docs/parhelia-gl-feature-map.md §8 (live capture of the
texture path). Those two are *not* repeated here; this document answers the
memory-layout questions they left open.

Sources: `MTXPARD.dll` (Windows XDDM D3D driver, i386 — VMAs below are its
`.text` VMAs), `mtx_parhelia.o` (Linux kernel HAL, x86-64 — VMAs are `.text`
offsets, symbols `mtxNNN`), `kernel/src/MtxParhlParms.c` (vendor source),
`libGL.so.1.3.0` / `mtx_drv.so`, and the live captures
`captures/tex_64x64_0.txt` (GL_RGBA), `captures/tex_64x64_1.txt` (GL_RGB),
`captures/tex_32x16_0.txt`, `captures/glsweep-0528-raw.txt`.

---

## 1. Is texture memory linear, tiled or twiddled?

**The layout the vendor Linux GL stack actually uses is LINEAR, byte for byte.
CONFIRMED on hardware.** A from-scratch driver must use tiling code 0 and
`addr = base + y*pitchBytes + x*bpt`, `pitchBytes = align(width,32) * bpt`.

The chip *does* have tiled ("textiled", Matrox's term) surface modes — six of
them, selected by `0x11e0[18:16]` — but the intra-tile byte ordering was **not**
recovered, and nothing in the open driver needs it.

### 1.1 The live proof that the sampled bytes are linear  **CONFIRMED**

The three `captures/tex_*.txt` runs upload a texture whose generator is
*per byte channel*: channel `c` of texel `i` is `(c0 + i*0x94) mod 256`. Since
`gcd(0x94,256) = 4`, the pattern **repeats every 64 texels** — verified directly
in the dumps (`tex_32x16_0.txt` offset 0x000 = 0x100 = `6f00254a`, and the
inter-dword delta is `0x94` in every byte lane).

That period is what discriminates the layouts for the 64x64 case:

| hypothesis | dword at byte offset `o` corresponds to | value depends on | period |
|---|---|---|---|
| linear, pitch 64 texels | source index `i = o/4` | `i mod 64` | **256 bytes** |
| 32x32 tiles, row-major in tile | `x = (o/4) mod 32`, `y = (o/4)/32` → `i = 64y+x` | `x` | 128 bytes |
| 64x64 tile (== whole texture) | same as linear | — | 256 bytes |

Measured in `tex_64x64_0.txt` region 6:

```
000000 6f00254a   000080 ef80a5ca   000100 6f00254a   000200 6f00254a
001000 6f00254a   001080 ef80a5ca   002000 6f00254a   003000 6f00254a
```

`0x000 == 0x100 == 0x200 == 0x1000 == 0x2000 == 0x3000` and `0x080 != 0x000`:
**period 256 bytes throughout the whole 16 KB, including across the 0x1000
boundary where a 32x32 tiling would have changed phase.** The 32x32-tile
hypothesis is excluded. The layout is `offset = 4 * (y*64 + x)`.

Supporting facts from the same captures:

- 64x64 RGBA and 64x64 RGB both change **exactly 16384 bytes, span 0x0-0x3ffc,
  fully dense** — `64*64*4`, no padding rows, no holes.
- 32x16 changes **exactly 2048 bytes, span 0x0-0x7fc, fully dense** —
  `32*16*4`. A 32x32-tiled surface would have allocated 4096 and, for any
  ordering other than plain rows, written a non-contiguous set.
- Both sizes give `pitchBytes = align(width,32) * 4` exactly
  (32 -> 128 B/row * 16 rows = 2048; 64 -> 256 B/row * 64 rows = 16384),
  which is the linear pitch rule of §3.

**Caveat, stated explicitly:** region 6 is the 4 MB texture heap
(`mmap` cookie `0x0d0d3000`, docs/parhelia-kernel-abi.md §"seven mappings"), it is
GPU-visible memory handed out by the create-buffer ioctl, the texture-descriptor
record in the region 2 command stream carries a pointer into it, and no blit
packet touches it. So these bytes *are* the sampled bytes, not a staging copy.
The only way this conclusion could be wrong is if a copy engine re-tiled into
unmapped VRAM behind the ioctl, for which there is no evidence in the stream.

### 1.2 Static corroboration  **CONFIRMED**

- MTXPARD's own textured-blit sets `0x11e0 = pitch | 0x32000000`
  (fragment-isa §11.10) — **tiling field = 0**.
- MTXPARD's surface copy `0x2f260` copies **row by row with `rep movs` at
  `pitchBytes`** for tiling codes 1, 2, 3 and 4, and only special-cases code 5
  (block-compressed, §2.4). No software swizzle exists anywhere in that path.
- MTXPARD `0x111150` (the per-stage texture re-basing loop) computes
  `addr = base + (Y & ~(2*tileH-1)) * bpt * align(width,32) + (X & ~0x3f) * bpt`
  — **linear address arithmetic even for a tiled surface**, with the origin
  merely constrained to a tile boundary (X a multiple of 64 texels, Y a multiple
  of `2*tileHeight`), and the fractional part folded into the texture matrix.

The consistent reading (**INFERRED**, but it is the only one that fits both the
row-copy and the re-basing code) is that "textiled" is an **address remap
performed by the memory subsystem** given the surface's base/pitch/tiling code —
not a layout the CPU has to produce. Software therefore addresses tiled surfaces
with the same linear arithmetic and only has to respect the alignment quanta of
§2. This also explains why `reg3DUseTextiled` can be flipped at run time without
changing any upload code.

### 1.3 That tiled modes exist at all  **CONFIRMED**

`kernel/src/MtxParhlParms.c:101`:

```c
/* Disables the use of textiled textures */
MTX_MODULE_PARM_DECL(reg3DUseTextiled, TRUE);
```

plus the registry keys `UseTextiled` (MTXPARD `.text` 0xf8ecc, read at
`0x109edb` into settings `[+0x58]`, default 1), and
`Force3DTextureTextiledToABSundog` / `Force3DTextureTextiledToCDSundog` in
`mtx_parhelia.o` `.rodata.str1.8+0x3e0/+0x408`. The "AB"/"CD" pair matches the
way the geometry helpers group the codes: `{2,3}` behave identically and
`{4,5}` behave identically.

**Not recovered:** the byte order *inside* a tile, for any of codes 1-5. Do not
guess it; use code 0.

---

## 2. The surface-geometry object and the tiling codes

MTXPARD and the Linux kernel HAL both carry the same 5-dword surface-geometry
object (MTXPARD `0xa3b10`-`0xa4230`; kernel HAL `mtx969` @0x5b2e0,
`mtx2981` @0x5b520 — byte-for-byte the same switches). It is embedded at
`+0x90` of the bigger surface record, and `0x35f95` copies exactly 5 dwords out
of it, which pins its size. **CONFIRMED.**

| dword | contents |
|---|---|
| +0x00 | VRAM byte base; **bits [1:0] are software flags**, `flags & 3 != 0` = block-compressed (`0xa3f18`, `0xa40b0`). `0x35f87` masks them off with `and eax,0xfffffffc` before use — so a real base is 4-byte aligned and the two spare bits are free |
| +0x04 | `ctrl` — the value of register **`0x11e0`**: width[13:0], tiling[18:16], format[29:24], unknown[31:30] |
| +0x08 | `dim2` — the value of register **`0x11e4`**: height[13:0], unknown[15:14], **depth[27:16]**, unknown[31:28] |
| +0x0c | linear byte size (used only when tiling == 0) |
| +0x10 | a 9-bit field at [18:10]; when non-zero the tile height and tile byte size halve (`0xa3e85`, `0xa3f4b`). Purpose **UNRESOLVED** — it behaves like a multisample/volume sub-mode |

### 2.1 Correction to fragment-isa §11.5

`0x11e4` is **not** "[16:14] extra field, bit16 forced set". `0xa4091` and
`0xa41f4` both read it as `(dim2 >> 16) & 0xfff`, and `0xa4180` multiplies the
surface size by it: **`0x11e4[27:16] = depth / slice count, minimum 1**. The
`or eax,0x10000` seen in the fillers is simply *depth = 1*. Bits [15:14] and
[31:28] remain unidentified (both are preserved by the write mask
`0xf001c000` at `0x35f5c` / `0x10eb47`).

Also: `docs/parhelia-gl-driver.md`'s "`[31:30]` = tiling (0 = linear)" is
**wrong**; tiling is `[18:16]`, and `[31:30]` is a separate unidentified field
that the render-target path preserves (mask `0xc0fbc000` at `0x35f4e`). Its
"bit17 set" note describes a *render target* (which that path forces to tiling
code 3, `0x35f63`), not a texture.

### 2.2 Tile geometry per tiling code  **CONFIRMED (values), INFERRED (naming)**

`0xa3f10` returns the tile height in lines; `0xa3d80` (uncompressed) and
`0xa3c90` (compressed) return a byte quantum. The quantum function takes one
argument: **arg = 0 gives the tile size in bytes, arg = 1 gives the allocation
granularity** (every surface-creation call site passes 1 — `0x1c036`,
`0x34de0`, `0x34ea8`, `0x34f70`, `0x35f9e`, `0x360e5`; the single arg-0 call
`0x1ce87` is a tile-alignment test). `0xa3c90` ignores the argument entirely.

Tile height, uncompressed (`0xa3f10`):

| tiling | tile height in lines |
|---|---|
| 0 | 1 |
| 1 | 2 |
| 2, 3 | 32 — but **16** if `obj[+0x10] & 0x7fc00` |
| 4, 5 | 32 if `obj[+0x10] & 0x7fc00`; else 32 bpt: `height > 16 ? 32 : 16`; other bpt: `height > 32 ? 64 : 32` |
| any, block-compressed (`flags & 3`) | 16 (early return at `0xa3f1d`) |

Tile size in bytes, uncompressed, arg = 0 (`0xa3d80`; halved when
`obj[+0x10] & 0x7fc00`):

| tiling | 1 bpt | 8 bpt | 16 bpt | 32 bpt |
|---|---|---|---|---|
| 0 | 0x40 | 0x40 | 0x40 | 0x40 |
| 1 | 0x40 | 0x40 | 0x40 | 0x100 |
| 2, 3 | — | 0x400 | 0x800 | 0x1000 |
| 4 | — | 0x400 | 0x800 | 0x800 |
| 5 | 4 bpt: 0x200 | 0x400 | — | — |

With arg = 1 the same function returns the larger allocation granule
(tiling 0: 0x200; tiling 1: 0x40/0x80/0x100; tiling 2/3: 0x2000/0x1000/0x2000
for 8/16/32 bpt; tiling 4/5: 0x1000/0x2000/0x2000, 4 bpt 0x800).

For tiling 2/3 with arg = 0 the numbers are exactly `32*32*bpt/8` for every
bit depth (0x400 / 0x800 / 0x1000), i.e. a **32x32-texel tile** — which is
precisely the hypothesis the 64x64 capture rules out for the *linear* path, and
is therefore the shape a driver would have to reproduce *if* it ever enabled
code 2 or 3. **INFERRED** from tile bytes / tile height only; the intra-tile
ordering is still unknown.

Allocation granule, block-compressed (`0xa3c90`, argument unused):
tiling 0 -> 0x20; tiling 1 -> 0x20 / 0x40 / 0x80 for 1-or-8 / 16 / 32 bpt;
tiling 2/3/4 -> 0x200 / 0x400 / 0x800 for 8 / 16 / 32 bpt; tiling 5 -> 0x100
for 4 bpt, 0x200 for 8 bpt.

### 2.3 Total surface size

`0xa4180` (`GetSurfaceSizeBytes`) — **CONFIRMED**:

```
tiling 0      : align(obj[+0x0c], 32)
tiling 1      : pitchBytes * align_up(height, 2)
tiling 2..5   : depth > 1 ? pitchBytes * depth * next_pow2(align_up(height,tileH))
                          : pitchBytes * align_up(height, tileH)
```

`0xa3fe0` (allocation size, aligned to the granule of §2.2) — **CONFIRMED**:

```
tiling 0      : align_up(obj[+0x0c], granule)
tiling 1..5   : align_up( bpt * align(width,32) * align_up(height,tileH) * depth , granule )
```

Note `0xa3fe0` uses `align(width,32)` where `0xa4180` uses the power-of-two
pitch of §3 — the allocator is deliberately the more generous of the two.

### 2.4 Tiling code 5 == block-compressed  **CONFIRMED**

`0x2f260` (surface copy) dispatches on `byte[obj+0x6] & 7`, i.e. on
`ctrl[18:16]`:

- `== 0` — a single flat `memcpy` of `obj[+0x0c]` bytes.
- `== 5` — the compressed path: `bytesPerUnit = bpt * 16 / 8`,
  `width >>= 2`, `height >>= 2` (both then masked to 12 bits, not 14),
  `pitchBytes <<= 2`. That is exactly **4x4 blocks**: dimensions in blocks,
  block size = 16 texels' worth of bits.
- anything else — row-by-row `rep movs` of `srcRowBytes`, destination advanced
  by `pitchBytes`, repeated `height * depth` times.

So a DXT surface stores `ceil(w/4) x ceil(h/4)` blocks of `2*bpt` bytes,
row-major, at a block pitch of `4*pitchBytes/4` — i.e. the ordinary S3TC
layout with the standard 8-byte (DXT1) / 16-byte (DXT2-5) blocks.

---

## 3. Pitch / stride rules  **CONFIRMED**

`0xa40f0` is the single source of truth (`GetPitchBytes`):

```
w = ctrl & 0x3fff
if (w >= 0x2000)              return bpt * 0x2000;      /* width clamped at 8192 */
if (tiling == 0 || tiling==1) return bpt * ((w + 31) & ~31);
else /* tiling 2..5 */        return bpt * next_pow2(max(w, 32));
```

- **The register field `0x11e0[13:0]` is a width in TEXELS, not bytes**, and it
  is the width already rounded up to a multiple of 32 (`0xa3b10` returns the
  constant 32 as "the pitch alignment"; `0x1111ef`, `0x1c006`, `0xc57a1`,
  `0x10d979` all use it that way). The hardware derives the byte pitch itself as
  `field * bpt`.
- **32-texel alignment is the only rule for linear surfaces.** A 40-texel-wide
  RGBA texture gets `align(40,32) = 64` texels = 256 bytes per row; the trailing
  24 texels of every row are padding the driver never writes.
- **Non-power-of-two and rectangular textures are fine in linear mode** — the
  32x16 capture is a live example, and its footprint is exactly `32*16*4` with
  no vertical padding at all. Height is *not* padded for tiling 0.
- For tiled surfaces (codes 2-5) the pitch is forced to a **power of two**,
  at least 32 texels — this is what makes non-power-of-two textures expensive
  there and is a further reason a first driver should stay linear.
- Maximum addressable width is **8192 texels** in the pitch computation, while
  GL advertises `GL_MAX_TEXTURE_SIZE` 2048 and the field itself is 14 bits.

---

## 4. Format table

### 4.1 What is already recovered elsewhere — do not redo

`docs/parhelia-fragment-isa.md` §11.7 already carries, all **CONFIRMED**:

- D3DFMT -> internal format id (MTXPARD `0x83af0`, 62-entry jump table),
- FOURCC -> internal id (`0x83f80`: DXT1..DXT5 = 1..5, YV12 = 0x27,
  NV12 = 0x28, UYVY = 0x29, YUY2 = 0x2a, MTXY/MTX0/ICD0 = 0x2b/0x2c/0x2d),
- hardware format code (`0x11e0[29:24]`) -> bits per texel (`0xa3b40`).

A **second** D3DFMT/FOURCC -> internal-id decoder exists at MTXPARD `0x838b0`
and agrees with `0x83af0` on every value they share; it adds the private codes
`0x102b0001 -> 0x2e`, `0x102b0002 -> 0x10`, `0x102b0003 -> 0x2c`.

The Linux kernel HAL contains the identical bits-per-texel switch
(`mtx969` @0x5b2e0, same code constants `0x04/0x08/0x09/0x0a/0x11/0x20/0x21/
0x22/0x24/0x26/0x27/0x28/0x2a/0x2d/0x30/0x32/0x33/0x36/0x38/0x3a/0x3b`), so the
code space is shared between the Windows and Linux stacks.

### 4.2 Bits per texel by hardware code (restated, it anchors everything)

| bpt | codes |
|---|---|
| 1 | 0x00, 0x02, 0x04, 0x05 |
| **4** | **0x08** |
| **8** | **0x09, 0x0a, 0x0b, 0x0c**, 0x10, 0x11, 0x12 |
| 16 | 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e |
| 32 | 0x30, 0x32, 0x33, 0x34, 0x36, 0x38, 0x3a, 0x3b |
| invalid | everything else, notably **0x29** |

Confirmed hardware codes with a known meaning:

| code | bpt | meaning | evidence |
|---|---|---|---|
| 0x11 | 8 | 8-bit paletted / indexed colour buffer | `0x1dceb` selects it for a 1-byte-per-pixel surface |
| 0x26 | 16 | 1:5:5:5 | docs/parhelia-gl-driver.md, unchanged here |
| 0x28 | 16 | 5:6:5 and 4:4:4:4 | `0x1dce1` selects it for a 2-byte-per-pixel surface |
| **0x32** | **32** | **8:8:8:8 ARGB** | `0x1dcbf` (MTXPARD default 32-bpp), `0x10eb71`, and Linux kernel HAL `0x3dcf5` / `0x3e005` / `0x3e995` (`and r12d,0xc0ffffff; or r12d,0x32000000`) |
| 0x34 | 32 | 2:10:10:10 | `0x1dcd7` |
| 0x30/0x33/0x36/0x38/0x3a/0x3b | 32 | further 32-bpp variants; `0x3b` is selected by `0x35f1b` for one particular surface class | `0xa3b40`, `0x35f1b` |

The Linux GL ICD stores **32 bits per texel for every requested internal
format** (fragment-isa/gl-feature-map ground truth; `tex_64x64_0` and
`tex_64x64_1` produce byte-identical descriptors and identical 16384-byte
footprints for GL_RGBA and GL_RGB). It therefore only ever needs code **0x32**.
`GL_INTENSITY` NULL-dereferences the vendor kernel module and is unsupported.

### 4.3 Compressed formats  **INFERRED (strong)**

`0xa3b40` gives code `0x08` a width of **4 bits per texel** and codes
`0x09, 0x0a, 0x0b, 0x0c` **8 bits per texel**. The compressed helpers
(`0xa3c90`, and the tiling-4/5 arms of `0xa3d80` / `0xa3f10`) handle **only**
4-bpt and 8-bpt surfaces, and the compressed copy path is tiling code 5.
DXT1 is 4 bits/texel and DXT2/3/4/5 are 8 bits/texel; their internal ids are
1 and 2,3,4,5. The only assignment consistent with all of that is

```
DXT1 -> hw code 0x08      (8 bytes per 4x4 block)
DXT2 -> 0x09, DXT3 -> 0x0a, DXT4 -> 0x0b, DXT5 -> 0x0c   (16 bytes per block)
```

i.e. `hwCode = internalId + 7`. **INFERRED** — the id -> code join table itself
was still not located (fragment-isa §11.7 says the same). Do not ship this
without a hardware check.

This also **refutes** `docs/parhelia-gl-driver.md`'s "DXT/S3TC = 0x2e":
`0xa3b40` classifies `0x2e` as 16 bits per texel, which no DXT format is.

Block layout: standard S3TC — `ceil(w/4) x ceil(h/4)` blocks stored row-major
(§2.4), width/height fields in the descriptor are the *block* counts masked to
12 bits, and the pitch field is scaled by 4.

### 4.4 Depth formats

Internal ids `0x11` (D16), `0x12` (D15S1), `0x13` (D24S8), `0x14` (D24X8),
`0x15` (D24X4S4) exist and are accepted by the D3DFMT decoder (fragment-isa
§11.7). **They are not sampleable as textures in this driver**: the texture
border-colour packer `0x84060` maps internal ids `0x11`-`0x1c` to its
"unsupported" case (§4.5). The depth *buffer* path is separate
(regs `0x1230`/`0x1234`, S8Z24; see docs/parhelia-gl-driver.md). No
depth-texture / shadow-compare sampler state was found anywhere.

### 4.5 Which internal formats the texture path accepts  **CONFIRMED (new)**

MTXPARD `0x84060` packs the D3D border colour into the texture's own layout and
is indexed by `internalId - 6`, range 0..0x1a — i.e. **internal ids 6 .. 0x20,
27 entries**, byte table at `0x843f0`, jump table at `0x843bc`:

```
843f0: 00 01 02 03 04 05 06 0c 0c 07 07 0c 0c 0c 0c 0c 0c 0c 0c 0c 0c 0c 08 09 0a 0b
        ^id 6                                                                    ^id 0x20
843bc: 8409e 840f0 8413b 8418c 841d7 84246 8429e 842f8 84346 84359 8437d 8438c 843b6
        case0 ..                                                              case12 = reject
```

| internal id | D3DFMT | case | packer behaviour |
|---|---|---|---|
| 0x06 | A4R4G4B4 | 0 | nibbles replicated to 8-bit fields, mask `0xffffffff` |
| 0x07 | X4R4G4B4 | 1 | same, mask `0x00ffffff` |
| 0x08 | A1R5G5B5 | 2 | 5-bit fields replicated to 10-bit, mask `0xffffffff` |
| 0x09 | X1R5G5B5 | 3 | same, mask `0x3fffffff` |
| 0x0a | R5G6B5 | 4 | optional R/B swap then 5->10-bit replicate, mask `0x3fffffff` |
| 0x0b | A8R8G8B8 | 5 | optional sign-flip of all four bytes, optional byte0<->byte2 swap, mask `0xffffffff` |
| 0x0c | X8R8G8B8 | 6 | as case 5 without alpha, mask `0x00ffffff` |
| 0x0d, 0x0e | A2B10G10R10, A8B8G8R8 | 12 | **rejected** |
| 0x0f, 0x10 | (10:10:10 class) | 7 | optional sign-flip of four 10-bit fields, optional swap of `[9:0]` with `[29:20]` |
| 0x11 .. 0x1c | D16 .. depth/bump | 12 | **rejected** |
| 0x1d | L8 | 8 | `L` replicated into all three low bytes, mask `0x00ffffff` |
| 0x1e | (16-bit single channel) | 9 | value placed at `[31:16]`, mask `0xffff0000` |
| 0x1f | A8 | 10 | value placed at `[31:24]`, mask `0xff000000` |
| 0x20 | A8L8 | 11 | `L` replicated into the three low bytes, alpha kept, mask `0xffffffff` |

The three tail rows independently re-confirm fragment-isa §11.7's id
assignments (L8 = 0x1d, A8 = 0x1f, A8L8 = 0x20).

**Channel order.** Cases 5/6/7 apply an optional swap of the outer two
channels, driven by a per-format-descriptor boolean (`desc[+0x10]`); a second
boolean (`desc[+0x14]`) XORs the sign bit of every field for signed
(bump-map) formats. Because D3D hands the border colour in **ARGB** order and
the swap exchanges byte 0 with byte 2, the hardware's 32-bit order for those
formats is **A[31:24] B[23:16] G[15:8] R[7:0]** when the swap flag is set.
**INFERRED** for the *texel* order (the border register need not share the
texel layout), **CONFIRMED** for the border register.

---

## 5. Mipmap chain layout

**There are no mip registers.** `0x11dc`/`0x11e0`/`0x11e4` describe a single
2D image; the driver picks the level on the CPU and reprograms the base per
draw (fragment-isa §11.9, `0x1110b0`-`0x111385` and `0x1559a0`-`0x155c52`).
Levels are packed back to back in one allocation:

```
levelAddr(N) = base + sum_{l < N}  bpt * align(width_l, 32) * height_l
```

i.e. each level's contribution is exactly `pitchBytes(l) * height_l` — the
value `0xa4180` returns for a tiling-0 surface, with **no per-level alignment
padding** beyond the 32-texel pitch rounding. **INFERRED (strongly)** — taken
from fragment-isa §11.9, which this document does not re-derive; the pitch term
matches `0xa40f0` exactly, which is the part that was re-checked here.

For a tiled surface each level would instead consume `0xa4180`'s tiled size
(pitch rounded to a power of two, height rounded to the tile height), and
`0x111150` shows that the re-based pointer must land on a tile boundary:
X a multiple of **64 texels**, Y a multiple of **2 * tileHeight**.

Practical consequence for an open driver: allocate one buffer, lay the levels
out with the formula above, keep an array of level base offsets, and write
`0x11dc` (plus `0x11e0`/`0x11e4` for the level's width/height) when the chosen
level changes. Nothing else is needed; the filter word `0x11c4[8:6]` still
selects mip-linear vs no-mip (fragment-isa §11.4) but the *level* is a CPU
decision.

No LOD-bias or LOD-clamp register exists in any traced path. D3D's
`MIPMAPLODBIAS` and `MAXMIPLEVEL` are stored in the driver's per-stage block
(`+0x18`, `+0x1c`, fragment-isa §11.8) and are consumed by that CPU-side
selection, never emitted.

---

## 6. Cube maps and 3D / volume textures

**Slices — CONFIRMED.** `0x11e4[27:16]` is a 12-bit depth / slice count with a
minimum of 1 (§2.1). `0xa4180` and `0xa3fe0` both multiply the surface size by
it, so slices are **consecutive in one allocation**, each slice occupying

```
tiling 0/1 : pitchBytes * height                (no extra padding)
tiling 2-5 : pitchBytes * next_pow2(align_up(height, tileHeight))
```

The `next_pow2` rounding of the slice height for tiled volume textures is an
explicit step at `0xa41ff`-`0xa4219`. `GL_EXT_texture3D` is advertised.

The mysterious `obj[+0x10] & 0x7fc00` field, which halves both the tile height
and the tile byte size (§2), is the only other depth-correlated knob found; if
it is the volume sub-mode then a tiled 3D texture uses a 32x16x2 tile rather
than 32x32x1. **UNRESOLVED.**

**Cube maps — INFERRED.** No six-face constant, no per-face register and no
face-ordering table was found in either binary. Cube faces therefore go through
the same mechanism as mip levels: six independent images in one allocation with
the base reprogrammed per face/draw. The existence of
`MTX_MODULE_PARM_DECL(reg3DHqCubemaps, FALSE)` ("use high quality mode
(borders) with cubemaps") says the seam handling is a driver-side decision, not
a hardware mode — consistent with faces being ordinary 2D surfaces.
`0x1200[18:16]` values 6 and 7 (fragment-isa §11.2) are the leading candidate
for a cube/3D texture-type selector but were not tied to anything.

---

## 7. The upload path

### 7.1 Linux GL  **CONFIRMED**

- The client mmaps seven regions; region 6 is the **4 MB texture heap**
  (`mmap` file offset `0x0d0d3000`, a cookie returned by the create-buffer
  ioctl `0xc0804618`; see docs/parhelia-kernel-abi.md).
- `glTexImage2D` (`tex_upload`) changes **nothing** in any mapping.
- The first draw that samples the texture (`tex_tri_nearest`) writes the whole
  image into region 6 with plain **CPU stores** — 16384 bytes for 64x64,
  2048 for 32x16, dense, linear (§1.1). There is no blit packet and no DMA
  descriptor for the pixels.
- No cache/flush ioctl exists. The only ioctl issued around a draw is the
  doorbell `_IO('F',33)` (`0x4621`, zero-sized). The heap is device memory
  mapped by the kernel driver, so the ordering guarantee comes from the mapping
  attributes plus the doorbell write — a driver must simply finish its stores
  before ringing the doorbell.
- The command stream carries a small texture record (region 2, offset 0x08 of
  the 1928-byte block), identical in all three captures except for size:

  | offset | 64x64 | 32x16 | reading |
  |---|---|---|---|
  | +0x00/+0x04 | `ffffc200_0cb4d004` | `ffffc200_0149a004` | 64-bit kernel pointer to the pixels |
  | +0x10 | 4 | 4 | constant 4 — bytes per texel (32-bpp always) |
  | +0x24 | 0x40 | 0x20 | **width**, plain dword |
  | +0x28 | 0x40 | 0x10 | **height**, plain dword |
  | +0x2c | 1 | 1 | level count / depth |

  (offsets relative to the record start at 0x08; the absolute offsets quoted in
  gl-feature-map §8 are +0x2c / +0x30 / +0x34.) The record carries **no format
  field** and **no pitch field** — pitch is derived as `align(width,32)*4`,
  matching the measured footprints exactly.

### 7.2 Windows D3D, for comparison  **CONFIRMED**

MTXPARD `0x2f260` is a CPU copy into the surface: flat `memcpy` for tiling 0,
row-by-row `rep movs` at `pitchBytes` for codes 1-4, and the 4x4-block variant
for code 5 (§2.4). No swizzle, no engine blit.

---

## 8. Sampler state not covered by fragment-isa §11

### 8.1 Per-unit enable  **CONFIRMED**

`0x1200 + unit*4`, field `[18:16]`: **1 = disabled**, 4-7 = enabled. When a
unit is disabled the emitter writes only `0x1200 + unit*4` and skips the whole
`0x11c0`-`0x11e4` window (MTXPARD `0x16fe5d`, `cmp ecx,0x10000`; kernel HAL
`0x69880`, `or eax,0x21212100`). Idle value `0x00010000`, active `0x00040101`.

### 8.2 LOD bias and LOD clamp  **CONFIRMED absent**

No register. See §5. A driver implementing `GL_TEXTURE_LOD_BIAS` /
`GL_TEXTURE_MAX_LEVEL` must fold them into its own level selection.

### 8.3 Border colour  **CONFIRMED**

Two registers, both written from the per-unit stage struct
(`+0x14 -> 0x11cc`, `+0x18 -> 0x11d0`), packed by MTXPARD `0x84060` (§4.5):

- **`0x11cc`** — the border colour expanded into the texture's own channel
  layout. Two families were decoded exactly:
  - 5-bit-channel formats (1555, 555, 565): each channel bit-replicated to
    **10 bits**, laid out **R[29:20] G[19:10] B[9:0]**. Verified by hand:
    input `0x7C00` -> `0x3FF00000`, `0x07E0` -> `0x000FFC00`,
    `0x001F` -> `0x000003FF`.
  - 4-bit and 8-bit channel formats: 8-bit fields,
    **A[31:24] .. [23:16] G[15:8] .. [7:0]**, nibbles replicated for 4444.
    Verified: 4444 input `0xF000` -> `0xFF000000`.
  - single-channel: A8 -> `[31:24]`; L8 -> replicated into `[23:0]`;
    A8L8 -> alpha kept at `[31:24]`, luminance replicated into `[23:0]`.
- **`0x11d0`** — the matching mask, ANDed into the colour before the write:
  `0xffffffff` (4444, 1555, 8888, A8L8), `0x00ffffff` (X4R4G4B4, X8R8G8B8, L8),
  `0x3fffffff` (X1R5G5B5, R5G6B5), `0xffff0000` (16-bit single channel),
  `0xff000000` (A8).
- The default when no border colour is set is `0x11cc = 0xffffffff`,
  `0x11d0 = 0x00000000` (MTXPARD `0x105381`).
- The border *enable* bits `0x11c4[10:9]` are cleared at `0x105378` on the
  border path, but the filler already writes the constant `0x49` (bits [10:9]
  zero) on both paths, so the AND is a no-op and the polarity of those two bits
  stays **UNRESOLVED**.

### 8.4 Wrap modes  **unchanged**

`0x11c0` as in fragment-isa §11.3: `0x1000 | U | (V<<3) | (W<<6)`, 2 bits per
axis. Which of the two non-zero encodings is clamp and which is mirror is still
undetermined.

---

## 9. What a from-scratch driver should do

1. Allocate textures in the 4 MB heap (region 6) obtained from the
   create-buffer ioctl. Base 4-byte aligned at minimum; 32-byte alignment
   matches `0xa4180`'s tiling-0 size rounding and costs nothing.
2. `tiling = 0`. Never set `0x11e0[18:16]` to anything else until the intra-tile
   order is proven on hardware.
3. `0x11e0 = align(width,32) | (formatCode << 24)`, `formatCode = 0x32` for
   the always-32-bpp path.
4. `0x11e4 = height | (depth << 16)` with `depth >= 1`.
5. `0x11dc = byteOffsetOfLevel0` (or of the currently selected level).
6. Store texels with the CPU at `base + y*align(width,32)*4 + x*4`, then ring
   the doorbell. No flush, no blit.
7. Mip levels, cube faces and volume slices: pack them consecutively, keep a
   CPU-side offset array, reprogram `0x11dc`/`0x11e0`/`0x11e4` per draw.

## 10. Open items

- The byte ordering inside a tile, for every tiling code 1-5. Not needed for a
  linear driver; needed before `reg3DUseTextiled`-style tiling can be enabled.
- The internal-format-id -> 6-bit hardware-code join table (still not located in
  either binary; §4.3's DXT assignment is the only inference offered).
- The meaning of `surfaceObj[+0x10][18:10]` (halves the tile height).
- `0x11e0[31:30]`, `0x11e4[15:14]` and `0x11e4[31:28]`.
- Cube-map face ordering, and whether `0x1200[18:16]` = 6/7 selects cube/3D.
- Whether the 5-bit-format border layout (10:10:10) and the 8-bit-format border
  layout (8:8:8:8) really are two different register interpretations, or whether
  one of them is a driver bug.

---

Copyright (C) 2026 René Rebe <rene@exactco.de>

## Independent verification of the linear layout  [HW]

The tiling answer is load-bearing, so it was re-checked by predicting exact
texel values rather than by comparing periods.

The `tex_tri_nearest` capture uploaded a 64x64 **GL_RGB** image whose source
bytes are `tx[i] = (i*37) & 0xff`. If storage is linear, row-major, ARGB8888
with an opaque alpha, then texel *t* must land at byte offset `4t` as:

```
A = 0xff,  R = 37*(3t) & 0xff,  G = 37*(3t+1) & 0xff,  B = 37*(3t+2) & 0xff
```

Checked against every captured texel in region 6: **1024 of 1024 match, zero
mismatches.** For example texel 64 (offset 0x100) predicts `0xffc0e50a` and the
capture reads `0xffc0e50a`.

This simultaneously establishes four things: the layout is **linear row-major**
with no tiling or swizzle, the pitch is dense (`4 * width`, no padding), the
channel order in memory is **ARGB8888** (dword `A<<24 | R<<16 | G<<8 | B`), and
`GL_RGB` is expanded to 32 bits with alpha forced to 0xff — which is the same
conclusion the byte-count evidence reached from a different direction.

**Caveat kept:** this proves the layout of the path the vendor driver uses
(tiling code 0). Tiled modes exist in the hardware and are selectable
(`reg3DUseTextiled` defaults TRUE), but their intra-tile byte order is **not**
recovered. An open driver should use tiling code 0 until that is decoded.

A note on method: a first check of this same data returned "not linear", because
it assumed the source pattern had a 256-byte period — true for a 4-byte-per-texel
source but not for the 3-byte `GL_RGB` source actually used, where the period is
256 *texels*. The period test was the wrong instrument; predicting exact values
is the right one.

## Non-power-of-two: three separate questions, three different answers

Added 2026-09-05.  "Does this hardware do NPOT" was previously graded as one
item; it is three, and they do not agree.

1. **NPOT/rectangular storage — SOLVED [HW].** The captured 32x16 texture
   occupies exactly `32*16*4` bytes, dense, with no vertical padding.  A
   non-square, non-power-of-two image is stored without waste.

2. **NPOT sampling capability — DECODED, static only.** `TextureCaps = 0x1EC4D`
   (`MTXPARD .data`) has **`D3DPTEXTURECAPS_POW2` clear**, and `CUBEMAP_POW2`,
   `VOLUMEMAP_POW2` and `SQUAREONLY` are clear as well.  Read literally: the
   silicon samples non-power-of-two textures in 2D, cube and volume, mipmapped.
   This is a headline claim resting on one `.data` bit and it has **not** been
   tested on hardware.

3. **The GL stack forbids it — SOLVED (static, enforcement read).**
   `glTexImage2D`/`glTexImage3D` validate `(dim-1) & dim == 0` for every
   dimension at `0x14a28d` / `0x14b078` and return `GL_INVALID_VALUE` otherwise.
   That is a GL-1.3 specification artefact in the ICD, not a hardware limit.

**Unreconciled:** a registry switch `np2Conditional` defaults to 0 and in that
state *withholds* `NONPOW2CONDITIONAL`.  That is not obviously consistent with
`POW2` being clear.  Resolve this before exposing
`ARB_texture_non_power_of_two` in a new driver -- the conservative reading is
that unconditional NPOT may carry restrictions (wrap modes, mipmapping) that the
conditional cap is there to express.
