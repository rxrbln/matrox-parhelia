/*
 * parhelia3d - surface clear.
 *
 * The clear is the 2D solid fill, emitted into the same command stream as the
 * 3D state so a frame is one submission: bind the surface into BOTH slot 0 and
 * slot 1, set PLNWT, DWGCTL 0xc0 (opaque solid), the fill colour, and write LEN
 * last - 0x138c is what fires the blit.
 *
 * Because the fill overwrites slots 0 and 1, a clear must be emitted BEFORE
 * p3d_emit_state(), which rebinds the colour and depth surfaces.  Clearing
 * after the state would leave the 3D pipeline pointing at the cleared surface.
 *
 * A depth surface clears exactly like a colour one: the fill is byte-level over
 * the same tiling, so the value written is the raw 32-bit S8Z24 word.  Z24 is
 * unsigned normalised, so "far" is 0x00ffffff and "near" is 0x00000000.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "parhelia3d.h"

#define P3D_DWGCTL_SOLID	0xc0u
#define P3D_DWGCTL_BITBLT_2_TO_0 0x2c2u

static uint32_t
p3d_surface_cfg_type(const struct p3d_surface *s, unsigned type)
{
	return (s->pitch & 0x3fffu) | ((type & 7u) << 16) |
	       ((s->fmt & 0x3fu) << 24);
}

/* PLNWT (0x1324) is the fill's bit-plane mask, as in the vendor XAA solid
 * fill.  Write it every time: 3D draws leave their colour mask in it. */
void
p3d_emit_clear(struct p3d_cs *cs, const struct p3d_surface *s, uint32_t value,
	       uint32_t write_mask, uint32_t x, uint32_t y, uint32_t w,
	       uint32_t h)
{
	struct p3d_rw rw[11];
	uint32_t cfg = p3d_surface_cfg(s);
	size_t n = 0;

	rw[n].reg = P3D_R_SURF0_BASE;     rw[n].val = s->base; n++;
	rw[n].reg = P3D_R_SURF0_BASE + 4; rw[n].val = cfg;     n++;
	rw[n].reg = P3D_R_SURF1_BASE;     rw[n].val = s->base; n++;
	rw[n].reg = P3D_R_SURF1_BASE + 4; rw[n].val = cfg;     n++;
	rw[n].reg = 0x12c0;               rw[n].val = 0xffffffffu; n++;
	rw[n].reg = P3D_R_COLMASK;        rw[n].val = write_mask; n++;
	rw[n].reg = P3D_R_DWGCTL;         rw[n].val = P3D_DWGCTL_SOLID; n++;
	rw[n].reg = P3D_R_FCOL;           rw[n].val = value;   n++;
	rw[n].reg = 0x1304;               rw[n].val = 0;       n++;
	rw[n].reg = 0x1308;               rw[n].val = (y << 16) | (x & 0xffffu); n++;
	rw[n].reg = 0x138c;               rw[n].val = (h << 16) | (w & 0xffffu); n++;
	p3d_reglist(cs, rw, n);
}

/* Vendor mtx3388's native surface-to-linear readback packet.  Slot 2 is the
 * source, slot 0 is the destination, and LEN is the firing write. */
void
p3d_emit_blit(struct p3d_cs *cs, const struct p3d_surface *dst,
	      unsigned dst_type, uint32_t dst_x, uint32_t dst_y,
	      const struct p3d_surface *src, unsigned src_type,
	      uint32_t src_x, uint32_t src_y, uint32_t w, uint32_t h)
{
	struct p3d_rw rw[9];
	size_t n = 0;

	rw[n].reg = P3D_R_SURF0_BASE; rw[n].val = dst->base; n++;
	rw[n].reg = P3D_R_SURF0_BASE + 4;
	rw[n].val = p3d_surface_cfg_type(dst, dst_type); n++;
	rw[n].reg = P3D_R_SURF0_BASE + 16; rw[n].val = src->base; n++;
	rw[n].reg = P3D_R_SURF0_BASE + 20;
	rw[n].val = p3d_surface_cfg_type(src, src_type); n++;
	rw[n].reg = 0x1324; rw[n].val = 0xffffffffu; n++;
	rw[n].reg = P3D_R_DWGCTL; rw[n].val = P3D_DWGCTL_BITBLT_2_TO_0; n++;
	rw[n].reg = 0x1304; rw[n].val = (src_y << 16) | (src_x & 0xffffu); n++;
	rw[n].reg = 0x1308; rw[n].val = (dst_y << 16) | (dst_x & 0xffffu); n++;
	rw[n].reg = 0x138c; rw[n].val = (h << 16) | (w & 0xffffu); n++;
	p3d_reglist(cs, rw, n);
}

void
p3d_emit_clear_frame(struct p3d_cs *cs, const struct p3d_state *st,
		     uint32_t colour, uint32_t depth)
{
	/* a tiled target only takes whole 32-row tile rows */
	uint32_t h = (st->screen_h + 31) & ~31u;

	p3d_emit_clear(cs, &st->colour, colour, 0xffffffffu, 0, 0,
		       st->screen_w, h);
	if (st->has_depth)
		p3d_emit_clear(cs, &st->depth, depth, 0xffffffffu, 0, 0,
			       st->screen_w, h);
}
