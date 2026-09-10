/*
 * parhelia3d - colour target address mapping.
 *
 * The 3D colour surface is stored in 64x32 pixel tiles of 8192 bytes.  Within
 * a tile the dword index is bit-swizzled:
 *
 *   x = b0 | b1<<1 | b2<<2 | b4<<3 | b5<<4 | ((b7 ^ tile_row_parity) << 5)
 *   y = b6 | b3<<1 | b8<<2 | b9<<3 | b10<<4
 *
 * and TILES PER ROW = pitch / 64.  The parity term is not optional: without it
 * every second band of tiles lands 32 px off, which reads as a regular 32x32
 * checkerboard of misplaced blocks and is easy to mistake for a rasterisation
 * defect - it was, for a while.
 *
 * Verified against a known-geometry 3D draw rather than by eye: a 500x300
 * rectangle rendered as two triangles maps back to exactly 500x300 at its true
 * origin, with 99.3% of the pixels present.  Note the 2D fill CANNOT serve as
 * this control - it writes linearly.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "parhelia3d.h"
#include "../parhelia_drm.h"

#define P3D_TILE_W	64u
#define P3D_TILE_H	32u
#define P3D_TILE_BYTES	8192u

uint32_t
p3d_tile_offset(uint32_t x, uint32_t y, uint32_t pitch)
{
	uint32_t tpr = pitch / P3D_TILE_W;
	uint32_t tcol = x / P3D_TILE_W, trow = y / P3D_TILE_H;
	uint32_t ix = x % P3D_TILE_W, iy = y % P3D_TILE_H;
	uint32_t x5 = ((ix >> 5) & 1) ^ (trow & 1);
	uint32_t i;

	i =  ( ix        & 1)       |
	     ((ix >> 1)  & 1) << 1  |
	     ((ix >> 2)  & 1) << 2  |
	     ((iy >> 1)  & 1) << 3  |
	     ((ix >> 3)  & 1) << 4  |
	     ((ix >> 4)  & 1) << 5  |
	     ( iy        & 1) << 6  |
	     x5               << 7  |
	     ((iy >> 2)  & 1) << 8  |
	     ((iy >> 3)  & 1) << 9  |
	     ((iy >> 4)  & 1) << 10;

	return (trow * tpr + tcol) * P3D_TILE_BYTES + i * 4;
}

void
p3d_tile_position(uint32_t offset, uint32_t pitch, uint32_t *x, uint32_t *y)
{
	uint32_t tpr = pitch / P3D_TILE_W;
	uint32_t t = offset / P3D_TILE_BYTES;
	uint32_t i = (offset % P3D_TILE_BYTES) / 4;
	uint32_t trow = t / tpr, tcol = t % tpr;
	uint32_t b[11], k;

	for (k = 0; k < 11; k++)
		b[k] = (i >> k) & 1;

	*x = tcol * P3D_TILE_W +
	     (b[0] | b[1] << 1 | b[2] << 2 | b[4] << 3 | b[5] << 4 |
	      ((b[7] ^ (trow & 1)) << 5));
	*y = trow * P3D_TILE_H +
	     (b[6] | b[3] << 1 | b[8] << 2 | b[9] << 3 | b[10] << 4);
}

/* The 128-bit LX/P690 path halves a tile vertically: 64x16 pixels and
 * 4096 bytes.  The low-bit interleave is unchanged and x5 still XORs the
 * tile-row parity.  A full P690 coordinate-ramp capture verifies all 65,536
 * dwords as a bijection with at most the rasterizer's two-LSB dither error. */
uint32_t
p3d_tile_offset_layout(uint32_t x, uint32_t y, uint32_t pitch,
		       uint32_t layout)
{
	uint32_t tpr, tcol, trow, ix, iy, x5, i;

	if (layout == PARHELIA_TILE_64X32_8K)
		return p3d_tile_offset(x, y, pitch);
	if (layout != PARHELIA_TILE_64X16_4K)
		return UINT32_MAX;

	tpr = pitch / P3D_TILE_W;
	tcol = x / P3D_TILE_W;
	trow = y / 16;
	ix = x % P3D_TILE_W;
	iy = y % 16;
	x5 = ((ix >> 5) & 1) ^ (trow & 1);
	i =  ( ix        & 1)       |
	     ((ix >> 1)  & 1) << 1  |
	     ((ix >> 2)  & 1) << 2  |
	     ((iy >> 1)  & 1) << 3  |
	     ((ix >> 3)  & 1) << 4  |
	     ((ix >> 4)  & 1) << 5  |
	     ( iy        & 1) << 6  |
	     x5               << 7  |
	     ((iy >> 2)  & 1) << 8  |
	     ((iy >> 3)  & 1) << 9;
	return (trow * tpr + tcol) * 4096 + i * 4;
}

void
p3d_tile_position_layout(uint32_t offset, uint32_t pitch, uint32_t layout,
			 uint32_t *x, uint32_t *y)
{
	uint32_t tpr, t, i, trow, tcol, b[10], k;

	if (layout == PARHELIA_TILE_64X32_8K) {
		p3d_tile_position(offset, pitch, x, y);
		return;
	}
	if (layout != PARHELIA_TILE_64X16_4K) {
		*x = *y = UINT32_MAX;
		return;
	}
	tpr = pitch / P3D_TILE_W;
	t = offset / 4096;
	i = (offset % 4096) / 4;
	trow = t / tpr;
	tcol = t % tpr;
	for (k = 0; k < 10; k++)
		b[k] = (i >> k) & 1;
	*x = tcol * P3D_TILE_W +
	     (b[0] | b[1] << 1 | b[2] << 2 | b[4] << 3 | b[5] << 4 |
	      ((b[7] ^ (trow & 1)) << 5));
	*y = trow * 16 + (b[6] | b[3] << 1 | b[8] << 2 | b[9] << 3);
}

uint32_t
p3d_texture_tile_offset(uint32_t x, uint32_t y, uint32_t pitch)
{
	uint32_t block = ((y >> 4) * pitch + x) >> 5;
	uint32_t in_block = ((y & 0xcu) << 7) |
			    ((y & 3u) << 2) |
			    ((x & 0x1fu) << 4);

	return block * 2048u + in_block;
}

uint32_t
p3d_texture_tile_offset_cpp(uint32_t x, uint32_t y, uint32_t pitch,
                            unsigned cpp)
{
	uint32_t block, in_block;

	if (cpp == 4)
		return p3d_texture_tile_offset(x, y, pitch);
	/* Vendor mtx2146: all three byte sizes use 32x16-texel blocks.
	 * The 8-bpp path at 0x2145f uses 512-byte blocks and 4x4-byte
	 * microtiles; the 16-bpp path at 0x21592 uses 1024-byte blocks and
	 * 2x4-texel 16-byte microtiles. */
	if (cpp == 1) {
		block = ((y >> 4) * pitch + x) >> 5;
		in_block = ((y & 0xcu) << 5) | ((y & 3u) << 2) |
			   (x & 3u) | ((x & 0x1cu) << 2);
		return block * 512u + in_block;
	}
	if (cpp != 2)
		return UINT32_MAX;
	block = ((y >> 4) * pitch + x) >> 5;
	in_block = ((y & 0xcu) << 6) | ((y & 3u) << 2) |
		   ((x & 1u) << 1) | ((x & 0x1eu) << 3);
	return block * 1024u + in_block;
}
