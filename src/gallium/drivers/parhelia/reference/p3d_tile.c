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
 * 4096 bytes, x5 still XORs the tile-row parity.  Unlike the 64x32 formula
 * above, y bit 0 is dword-index bit 3 and y bit 1 is bit 6: a model-free
 * P690 probe shows 3-D rectangles and 2D fills writing identical bytes, and
 * GL points read back in place only with this order.  The earlier
 * coordinate-ramp check tolerated two LSBs and could not see a one-row swap. */
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
	     ( iy        & 1) << 3  |
	     ((ix >> 3)  & 1) << 4  |
	     ((ix >> 4)  & 1) << 5  |
	     ((iy >> 1)  & 1) << 6  |
	     x5               << 7  |
	     ((iy >> 2)  & 1) << 8  |
	     ((iy >> 3)  & 1) << 9;
	return (trow * tpr + tcol) * 4096 + i * 4;
}

/* A 16 bpp render target on the LX/P690 keeps the same 4096-byte, 16-row tile
 * and doubles its width: 128x16 pixels.  Measured exhaustively over two tile
 * columns and four tile rows with parhelia-drm-test --narrow-target-probe;
 * x bit 5 still carries the tile-row parity, as every Parhelia surface order
 * does.  The bit assignment is not the 32 bpp one shifted down. */
uint32_t
p3d_tile_offset_16bpp(uint32_t x, uint32_t y, uint32_t pitch)
{
	uint32_t tpr = pitch / 128u, tcol = x / 128u, trow = y / 16u;
	uint32_t ix = x % 128u, iy = y % 16u;
	uint32_t x5 = ((ix >> 5) & 1) ^ (trow & 1);

	return (trow * tpr + tcol) * 4096u |
	       ( ix        & 1) << 1  |
	       ((ix >> 1)  & 1) << 2  |
	       ((ix >> 2)  & 1) << 3  |
	       ((ix >> 3)  & 1) << 4  |
	       ( iy        & 1) << 5  |
	       ((ix >> 4)  & 1) << 6  |
	       ((iy >> 1)  & 1) << 7  |
	       ((iy >> 2)  & 1) << 8  |
	       x5               << 9  |
	       ((iy >> 3)  & 1) << 10 |
	       ((ix >> 6)  & 1) << 11;
}

void
p3d_tile_position_16bpp(uint32_t offset, uint32_t pitch, uint32_t *x,
			uint32_t *y)
{
	uint32_t tpr = pitch / 128u;
	uint32_t t = offset / 4096u, i = offset % 4096u;
	uint32_t trow = t / tpr, tcol = t % tpr;
	uint32_t b[12], k;

	for (k = 0; k < 12; k++)
		b[k] = (i >> k) & 1;
	*x = tcol * 128u + (b[1] | b[2] << 1 | b[3] << 2 | b[4] << 3 |
			    b[6] << 4 | (b[9] ^ (trow & 1)) << 5 | b[11] << 6);
	*y = trow * 16u + (b[5] | b[7] << 1 | b[8] << 2 | b[10] << 3);
}

/* Byte offset of a pixel in a native render target of @cpp bytes per pixel. */
uint32_t
p3d_tile_offset_layout_cpp(uint32_t x, uint32_t y, uint32_t pitch,
			   uint32_t layout, unsigned cpp)
{
	if (cpp == 4)
		return p3d_tile_offset_layout(x, y, pitch, layout);
	if (cpp == 2 && layout == PARHELIA_TILE_64X16_4K)
		return p3d_tile_offset_16bpp(x, y, pitch);
	return UINT32_MAX;
}

void
p3d_tile_position_layout_cpp(uint32_t offset, uint32_t pitch, uint32_t layout,
			     unsigned cpp, uint32_t *x, uint32_t *y)
{
	if (cpp == 4)
		p3d_tile_position_layout(offset, pitch, layout, x, y);
	else if (cpp == 2 && layout == PARHELIA_TILE_64X16_4K)
		p3d_tile_position_16bpp(offset, pitch, x, y);
	else
		*x = *y = UINT32_MAX;
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
	*y = trow * 16 + (b[3] | b[6] << 1 | b[8] << 2 | b[9] << 3);
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

/* The P690 texture unit reads 32-bit 64x16 textures with y bit 0 in
 * dword-index bit 6 and y bit 1 in bit 3: the render-target order with
 * those two bits exchanged.  Sampling tests validated this order; the
 * 3-D and 2D engines write the one in p3d_tile_offset_layout(). */
/* A 64x16 tile is two 32-column halves that interleave every 512 bytes, and
 * the x5 parity bit selects between them, so the natural unit of the chain is
 * the 32x16-texel half tile: level N+1 begins at the half tile where level N
 * ends, which need not be a whole tile.  @base is the level's byte offset and
 * carries its half index in bit 9; adding it to a tile offset that has its own
 * half bit instead carries into the tile field and collides two texels.
 * Measured with --mip-address-probe: the level 1 of 96x40 lands at 0x4200 and
 * its texels (32,0) and (0,16) at 0x5000 and 0x6000, three half tiles on. */
uint32_t
p3d_texture_tile_offset_64x16_at(uint32_t base, uint32_t x, uint32_t y,
				 uint32_t pitch)
{
	uint32_t trow = y / 16;
	uint32_t half = ((base >> 12) << 1) + ((base >> 9) & 1) +
			(((trow * pitch + x) >> 6) << 1) +
			(((x >> 5) ^ trow) & 1);
	uint32_t ix = x % 32u, iy = y % 16;
	uint32_t i;

	i =  ( ix        & 1)       |
	     ((ix >> 1)  & 1) << 1  |
	     ((ix >> 2)  & 1) << 2  |
	     ((iy >> 1)  & 1) << 3  |
	     ((ix >> 3)  & 1) << 4  |
	     ((ix >> 4)  & 1) << 5  |
	     ( iy        & 1) << 6  |
	     ((iy >> 2)  & 1) << 8  |
	     ((iy >> 3)  & 1) << 9;
	return (half >> 1) * 4096 + (half & 1) * 512 + i * 4;
}

uint32_t
p3d_texture_tile_offset_64x16(uint32_t x, uint32_t y, uint32_t pitch)
{
	uint32_t trow = y / 16;
	/* The tile index is the linear texel index over the descriptor's pitch
	 * divided by the 64-texel tile width, the same shape the vendor's
	 * 32x16-block order uses - not (trow * pitch / 64) + x / 64, which
	 * aliases two tile rows onto one tile whenever the pitch is not a
	 * multiple of 64.  Measured with --mip-address-probe 96 64 and 129 65:
	 * tile rows land 96/64 and 160/64 texels on, and this form is the only
	 * one of the two that also places every texel of the rows between. */
	uint32_t tile = (trow * pitch + x) / P3D_TILE_W;
	uint32_t ix = x % P3D_TILE_W, iy = y % 16;
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
	     ((iy >> 3)  & 1) << 9;
	return tile * 4096 + i * 4;
}

/* The P690 texture unit reads 8- and 16-bit textures in 64x32-texel tiles,
 * 2048 bytes at one byte per texel and 4096 at two, with the tile-row parity
 * on x bit 5 that every other Parhelia surface order also carries.  Measured
 * exhaustively with parhelia-drm-test --texture-format-probe over two tile
 * rows and two tile columns; it is neither the vendor's generic 32x16-block
 * order nor this chip's own 32-bit 64x16 order, and the two texel sizes do
 * not share one order either. */
uint32_t
p3d_texture_tile_offset_p690_narrow(uint32_t x, uint32_t y, uint32_t pitch,
				    unsigned cpp)
{
	/* A tile always spans 64x32 texels of storage, so at a 32-texel pitch
	 * two consecutive 32-row bands share one tile and are told apart by
	 * the parity bit alone; the linear texel index over the pitch, divided
	 * by the tile width, expresses that and keeps a pitch that is not a
	 * multiple of 64 collision-free, as measured for the 32-bit order. */
	uint32_t trow = y / 32;
	uint32_t tile = (trow * pitch + x) / P3D_TILE_W;
	uint32_t ix = x % P3D_TILE_W, iy = y % 32;
	uint32_t x5 = ((ix >> 5) & 1) ^ (trow & 1);
	uint32_t i;

	if (cpp == 2)
		i =  ((ix       & 1) << 1) |
		     (((ix >> 1) & 1) << 2) |
		     (( iy       & 1) << 3) |
		     (((ix >> 2) & 1) << 4) |
		     (((iy >> 1) & 1) << 5) |
		     (((ix >> 3) & 1) << 6) |
		     (((ix >> 4) & 1) << 7) |
		     (((iy >> 2) & 1) << 8) |
		     ( x5             << 9) |
		     (((iy >> 3) & 1) << 10) |
		     (((iy >> 4) & 1) << 11);
	else if (cpp == 1)
		i =  ( ix       & 1)       |
		     (((ix >> 1) & 1) << 1) |
		     (((ix >> 2) & 1) << 2) |
		     (( iy       & 1) << 3) |
		     (((ix >> 3) & 1) << 4) |
		     (((iy >> 1) & 1) << 5) |
		     (((ix >> 4) & 1) << 6) |
		     (((iy >> 2) & 1) << 7) |
		     (((iy >> 3) & 1) << 8) |
		     ( x5             << 9) |
		     (((iy >> 4) & 1) << 10);
	else
		return UINT32_MAX;
	return tile * (2048u * cpp) + i;
}

bool
p3d_texture_uses_native_64x16(uint32_t chip_id, uint32_t tile_layout,
			      unsigned cpp)
{
	return chip_id == 0x2539 && tile_layout == PARHELIA_TILE_64X16_4K &&
	       cpp == 4;
}

bool
p3d_texture_uses_p690_narrow(uint32_t chip_id, uint32_t tile_layout,
			     unsigned cpp)
{
	return chip_id == 0x2539 && tile_layout == PARHELIA_TILE_64X16_4K &&
	       (cpp == 1 || cpp == 2);
}

uint32_t
p3d_texture_tile_offset_chip(uint32_t x, uint32_t y, uint32_t pitch,
			     unsigned cpp, uint32_t chip_id,
			     uint32_t tile_layout)
{
	if (p3d_texture_uses_native_64x16(chip_id, tile_layout, cpp))
		return p3d_texture_tile_offset_64x16(x, y, pitch);
	if (p3d_texture_uses_p690_narrow(chip_id, tile_layout, cpp))
		return p3d_texture_tile_offset_p690_narrow(x, y, pitch, cpp);
	return p3d_texture_tile_offset_cpp(x, y, pitch, cpp);
}
