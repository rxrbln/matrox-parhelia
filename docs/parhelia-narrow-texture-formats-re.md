# 8- and 16-bit sampler formats on the Parhelia texture unit

Copyright 2026 René Rebe <rene@exactco.de>

This note records the vendor format-record table as it was recovered from the
binaries in `/srv/t2/xf86-video-parhelia`, and what the P690 actually does with
it.  Addresses in `mtx_parhelia.o` are `.text+` offsets; addresses in
`MTXPARD.dll` are RVAs as `objdump` prints them.

It corrects two earlier claims in `parhelia-gl-driver.md`: 4:4:4:4 does **not**
share code `0x28` with 5:6:5, and `0x11` is not "8bpp in the texture
descriptor" but specifically the L8 decoder.

## The format record table

`mtx1179` at `.text+0x7bf60` writes a 0x24-byte record; `mtx1646` at
`.text+0x7c050` builds 224 of them into `mtx2688` (the count `0xe0` is stored
at `0x8061e`); `mtx3921` at `.text+0x80630` looks one up by internal format id
(`+0x04`) and surface role, called *variant* here (`+0x08`).

| record offset | meaning |
|---|---|
| `+0x04` | internal format id |
| `+0x08` | variant (surface role 0..8) |
| `+0x0c` | hardware format code |
| `+0x10` | BGR channel-swap bit |
| `+0x11` | signed/bump bit |
| `+0x20` | YUV byte-order bit |
| `+0x1d` | format has an alpha channel |

`mtx3424` at `.text+0x1eab0` assembles the descriptor word from those fields:

```
0x1ec67  0x11e0[29:24] = record+0x0c & 0x3f
0x1ec82  0x11e0[30]    = record+0x10        /* BGR swap */
0x1ec99  0x11e0[31]    = record+0x11        /* signed / bump */
0x1ec4e  0x11e0[23]    = record+0x20        /* UYVY vs YUY2 */
```

Variants 2 and 3 are the 3-D texture sampler roles: they are the only variants
whose codes reproduce the three live captures in `parhelia-texture-sampling.md`
(`GL_RGBA` -> `0x32`, `GL_RGB` -> `0x33`, `GL_LUMINANCE` -> `0x11`).  Variant 0
is a raw size class (`0x10` for every 8-bit format, `0x20` for every 16-bit
one, `0x30` for 32-bit), which is why the older note read `0x10` as "the 8bpp
code".

The internal format ids are named by the channel-mask switch `mtx3404` at
`.text+0x79c60` (jump table `.rodata+0x2600`), and the same naming is visible
in the Windows `MTXPARD.dll` pixel-format classifier at `0x83d50`.

## Sampler format codes

| format | id | sampler code | record site (variant 2) |
|---|---:|---:|---|
| A4R4G4B4 | 0x06 | `0x24` | `0x7d086` |
| X4R4G4B4 | 0x07 | `0x25` | `0x7d6c9` |
| A1R5G5B5 | 0x08 | `0x26` | `0x7cc2c` |
| X1R5G5B5 | 0x09 | `0x27` | `0x7ce59` |
| R5G6B5   | 0x0a | `0x28` | `0x7c9af` |
| A8R8G8B8 | 0x0b | `0x32` | — |
| X8R8G8B8 | 0x0c | `0x33` | — |
| L8       | 0x1d | `0x11` | `0x7da33` |
| L16      | 0x1e | `0x21` | — |
| A8       | 0x1f | `0x12` | `0x7d8a6` |
| A8L8     | 0x20 | `0x22` | — |
| DXT1..5  | 0x01..0x05 | `0x08`..`0x0c` | — |

The low bit of each code distinguishes "alpha present" from "alpha forced to
one": `0x24`/`0x25`, `0x26`/`0x27`, `0x32`/`0x33`.  `0x29` and `0x23` are not
valid codes at all — `mtx969` at `.text+0x5b2e0` and `MTXPARD 0xa3b40` both
return zero bits per texel for them.

Windows corroborates the 8-bit identities independently.  `MTXPARD.dll`
`0x1549b5` rewrites a unit's format when it builds an alpha companion:

```
1549b5  cmp ecx,0x11000000   ; L8
1549c6  or  eax,0x12000000   ; -> A8
1549d7  cmp edx,0x28000000   ; R5G6B5
1549e4  or  eax,0x22000000   ; -> A8L8
```

There is no intensity format: `mtx3246` at `.text+0x79c00` puts exactly
L8, L16, A8 and A8L8 in its luminance class, and no id has matching luminance
and alpha masks.  There is no palettized format and no palette control field
anywhere in the descriptor.

## Channel order

Taken from the `mtx3404` masks; all 16-bit formats are little-endian ARGB
words and none of them sets the BGR-swap bit.

| code | A | R | G | B |
|---|---|---|---|---|
| `0x24` | [15:12] | [11:8] | [7:4] | [3:0] |
| `0x26` | [15] | [14:10] | [9:5] | [4:0] |
| `0x28` | — | [15:11] | [10:5] | [4:0] |

These are exactly Gallium's `PIPE_FORMAT_B4G4R4A4_UNORM`,
`PIPE_FORMAT_B5G5R5A1_UNORM` and `PIPE_FORMAT_B5G6R5_UNORM`, so no channel
conversion is needed on upload.

`0x11e0[30]` is the sampler's own BGR swap, used by the 32-bit A8B8G8R8 and
2:10:10:10 records (`0x7c501`, `0x7c754`, `0x7d24b`, `0x7d47e`).  It would let
`PIPE_FORMAT_R8G8B8A8_UNORM` be sampled natively as `0x32 | 1<<30` instead of
being byte-swapped on upload; that is untested here.

## The texel-layout field 0x11e0[18:16]

The field selects the storage order, not the texel size.  `mtx263` at
`.text+0x519a0` turns the "textiled" class off for 32 bits per texel and for
packed YUV:

```
51af5  cmp DWORD PTR [rax+0x14],0x20   ; bits per texel == 32 ?
51af9  je  51aa5                       ;   -> not textiled
51afd  cmp eax,0x29 ; je 51aa5         ; UYVY
51b02  cmp eax,0x2a ; je 51aa5         ; YUY2
51b07  mov DWORD PTR [rbx+0x10],0xa    ; textiled
```

`mtx3100` at `.text+0x26f70` turns that into surface class 2, and `mtx2029` at
`.text+0x1e9a0` writes the layout: 5 for compressed, then 4 for the tiled
sampler class and 2 for the untiled one.  So:

| texel size | layout |
|---|---:|
| 32-bit | 2 |
| 16-bit and 8-bit | **4** |
| DXT | 5 |
| linear staging | 0 |

The 0528 `GL_LUMINANCE` capture `0x11e0 = 0x11040002` and the 32-bit captures
`0x32020002` both agree.  The earlier `log2(16 / bytes per texel)` reading of
this field happens to give the right value for 8-bit and the wrong one (3) for
16-bit.

## Allocation constants

`mtx1219` at `.text+0x5b610` (argument 0 selects the per-level alignment `A`,
argument 1 the whole-allocation granularity `G`):

| layout | bits/texel | A | G |
|---:|---:|---:|---:|
| 0 | any | `0x40` | `0x200` |
| 2, 3 | 8 | `0x400` | `0x2000` |
| 2, 3 | 16 | `0x800` | `0x1000` |
| 2, 3 | 30/32 | `0x1000` | `0x2000` |
| 4 | 8 | `0x400` | `0x1000` |
| 4 | 16/30/32 | `0x800` | `0x2000` |
| 5 | 4 | `0x200` | `0x800` |
| 5 | 8 | `0x400` | `0x1000` |

`mtx625` at `.text+0x5b7f0` gives the row granularity.  Its two arms are
`0x5b8c0` for 4, 8 and 16 bits per texel (32 rows when the height is at most
32, otherwise 64) and `0x5b8a6` for 30 and 32 bits (16 rows when the height is
at most 16, otherwise 32).  `parhelia-texture-sampling.md` section 5.3 has
those two rows the wrong way round.

## Texel address

`mtx2146` at `.text+0x21120` is the vendor's scatter for every uncompressed
texel size.  It branches only on bytes per texel, never on the chip or the
layout:

```
212e5  pitch = align(width, 32)            /* texels */
213a0  shift = 9, 10, 11 for 1, 2, 4 bytes per texel
       block = (((y >> 4) * pitch) + x) >> 5
       addr  = (block << shift) + in_block(x, y)
```

so every texel size uses a 32x16-texel block, of 512, 1024 or 2048 bytes.  The
in-block offsets are

```
bpp 32 (0x21578):  ((y & 0xc) << 7) | ((y & 3) << 2) | ((x & 0x1f) << 4)
bpp 16 (0x21592):  ((y & 0xc) << 6) | ((y & 3) << 2) | ((x & 1) << 1) |
                   ((x & 0x1e) << 3)
bpp  8 (0x2145f):  ((y & 0xc) << 5) | ((y & 3) << 2) | (x & 3) |
                   ((x & 0x1c) << 2)
```

The primitive is a 16-byte micro-tile: 4x4 texels at 8 bits, 2x4 at 16 bits,
1x4 at 32 bits.  This is what `reference/p3d_tile.c`'s
`p3d_texture_tile_offset_cpp()` already implements.

**The P690 does not have to follow it.**  For 32-bit textures this chip reads a
64x16 / 4096-byte order instead (`p3d_texture_tile_offset_64x16()`), and 8- and
16-bit textures land on a different layout field (4 rather than 2) again.
`mtx2146` has no chip branch, so the deviation cannot be found statically.
`parhelia-drm-test --texture-format-probe` measures it: it fills the whole
storage span with its own byte index, samples the image 1:1 through the format
under test and reports which address model each fetched texel matches.

## Measured on the P690

`parhelia-drm-test --texture-format-probe` fills a texture BO with its own
byte index, samples it 1:1 through the format under test and reports which
byte the unit fetched.  Every format below reproduced **every** texel:

| descriptor | decode | sizes measured |
|---|---|---|
| `0x11` layout 4 | L8, alpha reads 1 | 64x64, 32x128, 128x64 |
| `0x24` layout 4 | A4R4G4B4, alpha reads 1 | 64x32, 128x64 |
| `0x26` layout 4 | A1R5G5B5, alpha reads 1 | 64x32, 64x64, 128x64, 32x32, 32x64, 32x128, 16x64, 256x256 |
| `0x28` layout 4 | R5G6B5 | 64x32, 128x64 |
| `0x12` layout 4 | A8 — **samples as zero** | 64x64 |

So texel layout 4 is confirmed for 8- and 16-bit, the channel decodes are
confirmed, and A8 is confirmed useless: its only information is alpha, which
this unit does not emit.  Alpha reads as one for every format.

### The texel order

It is neither the vendor's generic 32x16-block formula nor this chip's own
32-bit 64x16 order, and the two texel sizes do not share one order:

```
16-bit   byte bit 1, 2, 4, 6, 7, 9  = x0..x5     bit 3, 5, 8, 10, 11 = y0..y4
 8-bit   byte bit 0, 1, 2, 4, 6, 9  = x0..x5     bit 3, 5, 7, 8, 10  = y0..y4
```

A tile always spans 64x32 texels of storage: 4096 bytes at two bytes per
texel, 2048 at one.  x bit 5 carries the tile-row parity, as it does in every
other Parhelia surface order — the `y bit 32` fetch delta is `0x1200` at a
64-texel pitch, that is one tile plus a flip of bit 9.

The parity is also what lets a 32-texel pitch work: two consecutive 32-row
bands then share one tile and are told apart by that bit alone (`y bit 32`
delta `0x200`, `y bit 64` delta `0x1000`).  Both cases fall out of

```c
tile = ((y / 32) * pitch) / 64 + x / 64;      /* multiply before dividing */
```

`reference/p3d_tile.c:p3d_texture_tile_offset_p690_narrow()` implements this
and `p3d_texture_tile_offset_chip()` dispatches to it for chip 0x2539 at one
or two bytes per texel.

### At the GL level

`tools/parhelia-gl-texformat-test` reports 0 failing checks, and the stored
component sizes show the storage the driver really chose:

```
RGB5_A1     R5G5B5A1   native      RGB565   R5G6B5   native
LUMINANCE8  L8         native      RGBA8    R8G8B8A8 native
RGBA4       R8G8B8A8   wider fallback
```

`GL_RGBA4` cannot reach the native `0x24` storage: `st_format.c` adds
`PIPE_BIND_RENDER_TARGET` to the bindings for it (and for `GL_RGB4`,
`GL_RGBA2`, `GL_RGB`, `GL_RGBA`, `GL_RGB8`, `GL_RGBA8`) even for an ordinary
texture, and there is no 16-bit render-target descriptor to advertise.
`GL_RGB5_A1` and `GL_RGB565` are not on that list, which is why they do reach
it.  The format stays in the table because its encoding is measured; the
state tracker simply never asks for it.

Regressions after the change: GL clear, primitive and mipmap tests pass, the
texture test keeps only its documented FBO-readback failure, and the Q3
timedemo is unchanged at 30.9 fps with and without `PARHELIA_NO_NARROW_FORMATS=1`
(Q3's own textures are 32-bit).

### Getting there

Two earlier probe runs wedged the engine, and both were red herrings:

* **A fresh engine needs an untextured draw before any textured one.**  WARP
  temporary T2 holds garbage until a program writes it, which the handover
  records for Mesa; it applies to every non-Mesa client.  `probe_prime_warp()`
  issues one degenerate untextured passthrough draw, like `ph_prime_warp()`.
* **`reset_on_probe=1` must not be used on the P690.**  After a wedge a plain
  module reload fails in `warp_gate_ready()` (`parhelia_warp.c:193`) with
  `WARP store busy`, and there is no engine reset path.  Loading with
  `reset_on_probe=1` does clear that, but `pci_reset_function_locked()` also
  clears the DRAM timings POST established, and `parhelia_drv.c` deliberately
  never reprograms the memory controller.  Every VRAM access is corrupt from
  then on and stays corrupt across further reloads
  (`--native-transfer-probe`: mismatch; `--masked-clear-probe`: 65536/65536).
  Only a reboot, which re-POSTs the card, restores it.

Once a reference draw with the known-good 32-bit descriptor was added in
front of the sweep it passed 0/2048 on a healthy engine, and no run since has
wedged anything.

## Sampled alpha

The P690 texture unit does not deliver a sampled alpha channel.  A texel with
A=0 still paints RGB, with `0x32` and with `0x26` alike; only vertex alpha
reaches the comparator.  See `parhelia-p690-sampler-alpha-emission-re.md`,
`parhelia-p690-texture-alpha-context-re.md` and the measured
`--p690-rgb5a1-alpha-probe` line in `parhelia-p690-q3-rgb5a1-alpha-route.md`.

That is a property of the unit, not of a format, so it applies to `0x12` (A8),
`0x24` (A4R4G4B4) and `0x22` (A8L8) as well.  The Mesa driver therefore
advertises only the narrow formats whose alpha carries no information: L8,
R5G6B5, and A1R5G5B5 for its RGB.
