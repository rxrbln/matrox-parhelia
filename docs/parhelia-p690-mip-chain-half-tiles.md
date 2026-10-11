# P690 mip chain packing: the 32x16 half tile

Copyright 2026 René Rebe <rene@exactco.de>

How the P690 texture address unit (PCI 102b:2539) places the levels of a
32-bit 2-D mip chain, measured on hardware with
`parhelia-drm-test --mip-address-probe W H`.

The descriptor carries **one** base address (`0x11dc`) and a maximum level
(`0x11e4[31:28]`).  Every further level's address is derived by the hardware,
so this layout is measured, not chosen: the driver has to reproduce it exactly
or the sampler reads another level's texels.

## 1. The unit of the chain is a half tile, not a tile

A 64x16-texel 4 KiB tile is two 32-column halves, and the two halves
**interleave every 512 bytes**: dword-index bit 7 - the `x5 ^ tile_row_parity`
bit that every Parhelia surface order carries - selects between them.  So a
32x16-texel half tile is not contiguous, and the only index that advances
linearly across a chain is the half-tile index `H`:

```c
byte_offset_of_half_tile(H) = (H >> 1) * 4096 + (H & 1) * 512
```

A level's texel address is then

```c
H     = H_base + 2 * (((y / 16) * pitch + x) / 64) + (((x >> 5) ^ (y / 16)) & 1)
addr  = byte_offset_of_half_tile(H) + intra(x % 32, y % 16) * 4
```

with `intra` the measured texture bit order (x0..x2 -> 0..2, y1 -> 3,
x3,x4 -> 4,5, y0 -> 6, y2 -> 8, y3 -> 9; bit 7 is the half selector above).
`H_base` is recovered from the level's byte offset as
`(offset >> 12) * 2 + ((offset >> 9) & 1)`.

This is `p3d_texture_tile_offset_64x16_at()` in the Gallium driver.  The older
form that took no base and was added to `level->offset` in bytes is exact only
for a base that is a whole tile; adding a mid-tile base to an offset that
carries its own half bit carries into the tile field and aliases two texels.

## 2. Level size and pitch

* **Pitch is `align(width, 32)` texels**, not the next power of two.  Tile rows
  of a 96- and a 129-wide image land 96/64 and 160/64 texels apart.  Every
  earlier measurement used a power-of-two width, where the two rules agree.
* **Every level quantises to 16 rows**, at a 32-texel pitch too.  The 24x10
  level of a 96x40 chain is followed 0x800 bytes on, one 32x16 block, not the
  0x1000 a 32-row quantum would reserve.
* A level occupies `(align(height, 16) / 16) * (pitch / 32)` half tiles, and
  **the next level begins at the half tile where it ends** - a whole tile only
  when that count happens to be even.  Levels are not 4 KiB aligned.

A level's last row band therefore spills into the half tile the next level
starts in: at pitch 96 the base level of 96x40 reaches 0x45FC while level 1
begins at 0x4200.  There is no conflict, because the two use opposite halves
of that tile - level 0's last band has even parity there and level 1's base
carries the odd half.  The allocation must still cover whole tiles.

## 3. Measured next-level bases

`hw` is the byte the texture unit fetched for texel (0,0) of level 1, read off
the probe's `hw (0,0) byte`.  `bands = align(height, 16) / 16`.

| pitch | bands 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| 64 | 0x1000 | 0x2000 | 0x3000 | 0x4000 |
| 96 | 0x1200 | 0x3000 | 0x4200 | 0x6000 |
| 128 | 0x2000 | 0x4000 | 0x6000 | 0x8000 |
| 160 | 0x2200 | 0x5000 | 0x7200 | 0xa000 |
| 224 | 0x3200 | 0x7000 | 0xa200 | 0xe000 |

Every 0x?200 is a level that begins in the odd half of a tile, which only
happens when `bands * pitch` is not a multiple of 64 - that is, for an odd
number of bands at a pitch that is an odd multiple of 32.  129x65 (pitch 160,
5 bands) lands at 0xc200 and fits the same rule.

`mip_npot_chain_selftest()` in `parhelia_drm_test.c` pins all 26 of these.

## 4. The tail

Levels no larger than 32x16 share one 64x16 tail block, which is simply the
next two half tiles.  The slot inside it is a **plain running count of the
levels that entered the tail** - never a function of the level's own
dimensions.  The vendor chain builder computes exactly that:
`kernel/mtx_parhelia.o` `mtx325`, `.text+0x28400` enters the tail at
`w <= 16 && h <= 16 && d <= 31`, `.text+0x28c43` records the first such
level's index, and `.text+0x28a6d..0x28aad` indexes the three `.rodata`
tables `mtx3067` (x), `mtx516` (y) and `mtx1834` (bit offset) with
`level - firstTailLevel`.  The M9xx/P690 HAL carries the same tables verbatim
(`m9x_drv.so` `.rodata 0xe92e80/0xe92ea0/0xe92ec0`).

x is `{0,16,24,16,24}`, y is `{0,0,0,8,8}`, shifted by x+32 once a level wider
than 16 has taken the left half of the block.

## 5. A probe artefact worth knowing

`--mip-address-probe` draws each level over a 1:1 quad of that level's size
with `MIP_NEAREST`, so the minification ratio is `max(W/lw, H/lh)` and the
unit rounds `log2` of it to the nearest level.  A level whose ratio is
`1.5 * 2^k` samples the **next** level and reports its address: level 5 of
96x48 (48/1 = 1.5*32) fetches level 6's 0x7880, level 4 of 192x24
(24/1 = 1.5*16) fetches level 5's 0x9080.  96x40, with identical level widths
but a 1.25*32 ratio, is exact at every level.  `PROBE_PIN_LEVEL=1` caps the
descriptor's maximum level so the LOD clamp forces the level under test.

This cost a round of investigation: the substitution looks exactly like a tail
slot the hardware assigns differently.

## 6. State on hardware

With sections 1-4 implemented, every level of 96x64, 96x40, 96x16, 96x48,
160x40, 224x48, 129x65, 32x512, 64x64, 48x48, 192x24, 256x256, 128x80, 37x53,
100x100 and 255x255 reads back byte exact - 121 of 121 probed levels, against
68 before.  Host selftests: 51 PASS, 0 FAIL.

The kernel's `CREATE_RENDER` now describes the same `align(width, 32)` texture
pitch, and sizes such an object by the tiles its row bands reach, since
`pitch * height` stops covering the last tile once the pitch is not a multiple
of 64.
