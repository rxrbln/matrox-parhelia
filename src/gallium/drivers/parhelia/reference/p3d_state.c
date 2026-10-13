/*
 * parhelia3d - the per-batch 3D state and the per-primitive draw.
 *
 * The emission ORDER matters and is the capture's own: raster and ROP, vertex
 * format, the WARP bind, vertex fetch, the surface, the batch fence, then the
 * combiner.  Everything here is cited to docs/parhelia-engine-context-bringup.md;
 * values marked "measured" are hardware constants we reproduce rather than
 * fields we have decoded.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include <string.h>

#include "parhelia3d.h"

#define ARRAY(x) ((x)), (sizeof (x) / sizeof (x)[0])

/* 0x10b0 = mtx734[prim] | topology << 12 | 0x60000.  Bit 17 is always set and
 * bit 18 is the "legacy per-vertex-register path" the ICD selects. */
static const struct { uint8_t code, topo, cls; } p3d_prim_tab[P3D_PRIM_COUNT] = {
	[P3D_POINTS]	= { 0x20, 0, 0 },
	[P3D_LINES]	= { 0x3d, 1, 1 },
	[P3D_LINESTRIP]	= { 0x3d, 2, 1 },
	[P3D_TRIANGLES]	= { 0x5c, 3, 2 },
	[P3D_TRISTRIP]	= { 0x5c, 4, 2 },
	[P3D_TRIFAN]	= { 0x5c, 5, 2 },
	[P3D_QUADS]	= { 0x5c, 6, 2 },
};

/* 0x1300 low nibble by primitive class, measured across the gl-matrix
 * captures: 0 point, 1 line, 2 triangle, 3 wide point. */
static const uint8_t p3d_dwg_nibble[4] = { 0xd, 0x5, 0x4, 0xc };

uint32_t
p3d_primword(enum p3d_prim prim)
{
	return p3d_prim_tab[prim].code |
	       ((uint32_t)p3d_prim_tab[prim].topo << 12) | 0x60000u;
}

/* A fog colour component: the ICD packs (int)(c * 255 + 0.5). */
static uint32_t
p3d_fog_byte(float c)
{
	float v = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;

	return (uint32_t)(v * 255.0f + 0.5f) & 0xffu;
}

/* Fog start and end are 24-bit depths: value * 16777215. */
static uint32_t
p3d_fog_depth(float v)
{
	float c = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;

	return (uint32_t)(c * 16777215.0f + 0.5f) & 0x00ffffffu;
}

/* DWGCTL for a 3D primitive: the captured base with the class nibble. */
uint32_t
p3d_dwgctl(const struct p3d_state *st)
{
	const uint32_t rop = st->logicop_enable ? st->logicop & 0xfu : 0xcu;
	const uint32_t cls = st->prim == P3D_POINTS && st->point_class ?
			     st->point_class : p3d_prim_class(st->prim);
	const uint32_t nibble = st->prim == P3D_POINTS && st->point_nibble ?
				st->point_nibble : p3d_dwg_nibble[cls];

	/* [7:4] is the raster op; 0xC is GXcopy, the engine's default and the
	 * only code that leaves blending enabled. */
	return (0x00a60ec4u & ~0xffu) | (rop << 4) | nibble;
}

uint32_t
p3d_prim_class(enum p3d_prim prim)
{
	return p3d_prim_tab[prim].cls;
}

unsigned
p3d_vertices_per_primitive(enum p3d_prim prim)
{
	switch (prim) {
	case P3D_POINTS:
		return 1;
	case P3D_LINES:
		return 2;
	case P3D_TRIANGLES:
		return 3;
	default:
		/* Strip, fan and quad packets consume a variable vertex count and
		 * are deliberately outside the one-primitive-per-packet builder. */
		return 0;
	}
}

unsigned
p3d_minimum_vertices(enum p3d_prim prim)
{
	switch (prim) {
	case P3D_POINTS:
		return 1;
	case P3D_LINES:
	case P3D_LINESTRIP:
		return 2;
	case P3D_QUADS:
		return 4;
	default:
		return 3;
	}
}

/* The vendor turns the blender off when src=ONE, dst=ZERO on both pipes, which
 * is a no-op; that is an optimisation, so it is reproduced here only in that it
 * gives byte-identity with the capture's disabled state. */
uint32_t
p3d_blendctl(const struct p3d_state *st)
{
	struct p3d_blend_factor s = st->blend ? st->blend_src : (struct p3d_blend_factor){ 1, 1 };
	struct p3d_blend_factor d = st->blend ? st->blend_dst : (struct p3d_blend_factor){ 0, 0 };
	uint32_t eq = (uint32_t)st->blend_eq & 7u;
	uint32_t eq_a = st->blend_eq_alpha_separate ?
		(uint32_t)st->blend_eq_alpha & 7u : eq;

	/* The blend-factor fields are ignored by MIN and MAX, but the hardware
	 * requires their canonical ONE encoding, per pipe. */
	if (st->blend && (eq == P3D_BEQ_MIN || eq == P3D_BEQ_MAX))
		s.rgb = d.rgb = P3D_BF_ONE.rgb;
	if (st->blend && (eq_a == P3D_BEQ_MIN || eq_a == P3D_BEQ_MAX))
		s.a = d.a = P3D_BF_ONE.a;

	/* A target with more than eight bits per component takes BOTH dither bits
	 * low, which is a third state distinct from "dither off".  The vendor
	 * gate is mtx2480 .text+0x12a67: internal render-target formats 0x0f
	 * (A2B10G10R10), 0x10 (A2R10G10B10) and 0x1e (L16) jump to 0x137a0,
	 * which clears bit 23 and falls into the bit-24 clear; every other format
	 * sets bit 23 and lets the request choose bit 24. */
	if (st->colour.fmt == 0x34)
		return (st->blend ? 1u : 0u) |
		       ((uint32_t)(s.rgb & 0xf) << 1) |
		       ((uint32_t)(d.rgb & 0xf) << 5) |
		       ((uint32_t)(s.a   & 0xf) << 9) |
		       ((uint32_t)(d.a   & 0xf) << 13) |
		       (eq << 17) | (eq_a << 20);

	return (st->blend ? 1u : 0u) |
	       ((uint32_t)(s.rgb & 0xf) << 1) | ((uint32_t)(d.rgb & 0xf) << 5) |
	       ((uint32_t)(s.a   & 0xf) << 9) | ((uint32_t)(d.a   & 0xf) << 13) |
	       (eq << 17) | (eq_a << 20) | P3D_BLEND_DITHER |
	       (st->dither ? 0u : P3D_BLEND_DITHER_DISABLE);
}

/* Append one block load to a scratch buffer rather than opening its own ring
 * call; the caller emits the whole group with p3d_raw. */
static size_t
blockload_into(uint32_t *out, size_t off, size_t cap, uint32_t first,
	       const float *vec4s, size_t nvec)
{
	size_t i, n = 5 + nvec * 4;

	if (off + n > cap)
		return off;
	out[off + 0] = 0x41000000u | (((uint32_t)nvec << 17) & 0x007e0000u)
				   | (((uint32_t)nvec * 4 + 3) & 0xffffu);
	out[off + 1] = (uint32_t)nvec;
	out[off + 2] = first;
	out[off + 3] = 1;
	out[off + 4] = 0;
	for (i = 0; i < nvec * 4; i++)
		memcpy(&out[off + 5 + i], &vec4s[i], 4);
	return off + n;
}

void
p3d_emit_constants(struct p3d_cs *cs, uint32_t first_slot, const float *vec4s,
		   size_t nvec)
{
	uint32_t buf[128];
	size_t n = blockload_into(buf, 0, sizeof buf / sizeof buf[0],
				  first_slot, vec4s, nvec);

	p3d_raw(cs, buf, n);
}

/* All the constant loads in ONE ring call, the way the vendor emits them. */
void
p3d_emit_constant_group(struct p3d_cs *cs, const struct p3d_state *st,
			const float *vp)
{
	/* Three loads: the program's own block, the viewport pair and the
	 * raster block's copy of it. */
	uint32_t buf[4 * 5 + (64 + 4 + 64) * 4];
	unsigned m = st->const_mask ? st->const_mask : 7u;
	size_t n = 0, cap = sizeof buf / sizeof buf[0];

	if ((m & 1u) && st->vs_const && st->vs_const_vec4s &&
	    st->vs_const_vec4s <= 63) {
		/* A compiled vertex program's constants: the shader's own
		 * uniform vec4s followed by the literals the compiler spelled
		 * out, all at c0. */
		n = blockload_into(buf, n, cap, 0, st->vs_const,
				   st->vs_const_vec4s);
	} else if ((m & 1u) && st->mvp) {
		/* One block: c0..c3 the MVP rows, and when lighting is on c4/c5
		 * carry the light direction and material.  They must ride in the
		 * SAME load as the matrix - a separate load at a higher slot is
		 * accepted by the loader but the values do not read back. */
		float blk[28];		/* c0..c3 matrix, c4 light, c5/c6 material */
		size_t k, nv = 4;

		for (k = 0; k < 16; k++) blk[k] = st->mvp[k];
		if (st->light) {
			for (k = 0; k < 4; k++) {
				blk[16 + k] = st->light_dir[k];
				blk[20 + k] = st->material[k];
				blk[24 + k] = st->ambient[k];
			}
			nv = 7;
		}
		n = blockload_into(buf, n, cap, 0, blk, nv);
	}
	if (m & 2u)
		n = blockload_into(buf, n, cap, st->vp_slot ? st->vp_slot : 8,
				   vp, 2);
	if (m & 4u)
		n = blockload_into(buf, n, cap, 0x160, vp, 2);
	if ((m & 8u) && st->aux_const && st->aux_const_vec4s)
		n = blockload_into(buf, n, cap, st->aux_const_slot,
				   st->aux_const, st->aux_const_vec4s);
	if (n)
		p3d_raw(cs, buf, n);
}

void
p3d_viewport(struct p3d_state *st, float x0, float y0, float w, float h,
	     float zn, float zf)
{
	st->vp_scale[0]  = w / 2.0f;
	st->vp_scale[1]  = -h / 2.0f;
	st->vp_scale[2]  = (zf - zn) / 2.0f;
	st->vp_scale[3]  = 1.0f;
	st->vp_offset[0] = x0 + w / 2.0f;
	st->vp_offset[1] = y0 + h / 2.0f;
	st->vp_offset[2] = (zn + zf) / 2.0f;
	st->vp_offset[3] = h;
}

uint32_t
p3d_tex_filter(const struct p3d_texture *t)
{
	if (t->filter)
		return t->filter;
	return 0x01c01000u | ((uint32_t)t->min_filter & 3u) |
	       (((uint32_t)t->mag_filter & 3u) << 3) |
	       (((uint32_t)t->mip_filter & 3u) << 6);
}

/* 0x11c8 is packed for the sampled format's class, not for its exact layout:
 * the vendor converter (MTXPARD 0x84560 jump table, kernel mtx678
 * .text+0x7bc50) passes A8R8G8B8 through for the 8888, 4444, 8-bit and
 * compressed classes and expands the 5-bit-channel 16 bpp classes to
 * A2R10G10B10.  Scaling is the vendor's truncating v*1023/255 and a*3/255. */
uint32_t
p3d_border_colour(uint32_t fmt, uint32_t argb)
{
	if (fmt != 0x26 && fmt != 0x27 && fmt != 0x28)
		return argb;
	return ((((argb >> 24) & 0xffu) * 3u / 255u) << 30) |
	       ((((argb >> 16) & 0xffu) * 1023u / 255u) << 20) |
	       ((((argb >> 8) & 0xffu) * 1023u / 255u) << 10) |
	       ((argb & 0xffu) * 1023u / 255u);
}

uint32_t
p3d_tex_wrap(const struct p3d_texture *t)
{
	if (t->wrap)
		return t->wrap;
	/* LOD max 15 in [31:24] is the capture's resting value */
	return 0xf0000000u | ((uint32_t)t->wrap_s & 7u) |
	       (((uint32_t)t->wrap_t & 7u) << 3) |
	       (((uint32_t)t->wrap_r & 7u) << 6);
}

/* One byte per channel, the 5:6:5 fields of a 16 bpp target replicated into
 * both halves of the masked word, or the 2:10:10:10 fields of a GigaColor
 * target; a disabled channel is zero.  PLNWT masks the stored word, so the
 * field boundaries are the surface format's, not the 8-bit ones: the vendor's
 * own replicate helper (MTXPARD 0x9b320, Linux mtx3347 @0xffc0) passes a
 * four-byte surface's mask through verbatim and leaves the channel split to
 * the caller. */
static uint32_t
p3d_colour_mask(const struct p3d_state *st)
{
	uint32_t mask;

	if (!st->mask_explicit &&
	    !st->mask_r && !st->mask_g && !st->mask_b && !st->mask_a)
		return 0xffffffffu;	/* nothing set = all channels on */
	if (st->colour.fmt == 0x34)
		return (st->mask_a ? 0xc0000000u : 0u) |
		       (st->mask_r ? 0x3ff00000u : 0u) |
		       (st->mask_g ? 0x000ffc00u : 0u) |
		       (st->mask_b ? 0x000003ffu : 0u);
	if (st->colour.fmt == 0x28) {
		mask = (st->mask_b ? 0x001fu : 0u) |
		       (st->mask_g ? 0x07e0u : 0u) |
		       (st->mask_r ? 0xf800u : 0u);
		return mask | mask << 16;
	}
	return (st->mask_b ? 0x000000ffu : 0u) |
	       (st->mask_g ? 0x0000ff00u : 0u) |
	       (st->mask_r ? 0x00ff0000u : 0u) |
	       (st->mask_a ? 0xff000000u : 0u);
}

uint32_t
p3d_surface_cfg(const struct p3d_surface *s)
{
	return (s->pitch & 0x3fff) | (2u << 16) | ((s->fmt & 0x3f) << 24);
}

/* 0x1060 is a vertex COUNT - floor(free bytes / stride) over one 0x321000
 * buffer (mtx1035 @0xb9ee).  Reproduces the captured 0x280cc, 0x1c9b6 and
 * 0x19080 for widths 5, 7 and 8 exactly. */
uint32_t
p3d_vtxcap(unsigned dw_per_vertex)
{
	return 0x321000u / (dw_per_vertex * 4);
}

/* A position carrying 1/w: the vendor's pre-transformed vertex is
 * [x][y][z][1/w] with descriptor mask xyzw, so the position widens to four
 * components and every later attribute moves one dword on (offset [11:4]). */
static void
p3d_descriptors_add_w(struct p3d_rw *desc, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (!desc[i].val)
			continue;
		if (i == 0)
			desc[i].val |= 0x03c00000u;
		else
			desc[i].val += 1u << 4;
	}
}

/* A projective coordinate set 0: widen its component mask to s,t,r,q and
 * move every later attribute the two added dwords on. */
static void
p3d_descriptors_add_q(struct p3d_rw *desc, size_t n)
{
	int found = 0;

	for (size_t i = 0; i < n; i++) {
		if (!desc[i].val)
			continue;
		if (found) {
			desc[i].val += 2u << 4;
		} else if (((desc[i].val >> 16) & 0xf) == 7) {
			desc[i].val |= 0x03c00000u;
			found = 1;
		}
	}
}

/* A packed colour at dword @colour: its descriptor (kind 5), if declared,
 * fetches one dword in format 6, and every attribute after it moves on by
 * the dwords it no longer takes, also when GL_REPLACE leaves the colour
 * undeclared.  Explicit descriptor overrides are left as they are. */
static void
p3d_descriptors_pack_colour(struct p3d_rw *desc, size_t n, unsigned colour,
			    int alpha)
{
	const unsigned saved = alpha ? 3u : 2u;

	for (size_t i = 0; i < n; i++) {
		if (!desc[i].val)
			continue;
		if (((desc[i].val >> 16) & 0xf) == 5)
			desc[i].val = (desc[i].val & ~(0xfu << 22)) |
				      (1u << 22) | (6u << 12);
		else if (((desc[i].val >> 4) & 0xff) > colour)
			desc[i].val -= saved << 4;
	}
}

/* The attribute descriptors, by vertex layout.  Bit fields: [2:0] stream,
 * [11:4] dword offset, [19:16] attribute KIND, [25:22] component mask,
 * [31] last.  Every capture terminates the burst with a zero descriptor. */
/* The untextured two-colour stream: colour at dword @first (kind 5), then
 * the second colour (kind 6).  @flags bit 0 = present, bit 1 = the second
 * colour carries alpha, bit 2 = the first does.  The component mask sits in
 * [25:22] (7 = RGB, 0xf = RGBA, the encoding the measured kind-5 words
 * 0x21c5/0x23c5 use) and the dword offset in [11:4]; flags 1 reproduces the
 * measured 0x21c50020/0xa1c60050 and 0x21c50030/0xa1c60060 exactly. */
static size_t
p3d_colour1_descriptors(struct p3d_rw *out, size_t n, unsigned first,
			int flags)
{
	const unsigned c0 = flags & 4 ? 4u : 3u;

	out[n].reg = P3D_R_DESC;
	out[n].val = 0x20000000u | ((flags & 4 ? 0xfu : 7u) << 22) |
		     (5u << 16) | (first << 4);
	n++;
	out[n].reg = P3D_R_DESC;
	out[n].val = 0xa0000000u | ((flags & 2 ? 0xfu : 7u) << 22) |
		     (6u << 16) | ((first + c0) << 4);
	n++;
	return n;
}

static size_t
p3d_descriptors(struct p3d_rw *out, int textured, int with_z,
		uint32_t pos_override, uint32_t col_override, int no_colour,
		unsigned ntex, int vertex_alpha, int texcoord3,
		int vertex_colour1)
{
	const int two_tex = ntex >= 2;
	size_t n = 0;

	/* Three and four coordinate sets.  A set is one descriptor: kind 7 +
	 * set (WARP input v7 + set), component mask 3, float type 4, at the
	 * dword the earlier attributes end on, the last carrying bit 31 (KRN
	 * mtx2877 0xa9130).  As in the measured two-set words, the colour
	 * dwords stay in the stream when GL_REPLACE leaves them undeclared.
	 * One and two sets keep their measured words below verbatim. */
	if (textured && ntex > 2 && !texcoord3 && !pos_override) {
		const unsigned base = (with_z ? 3u : 2u) +
			(vertex_alpha ? 4u : 3u);
		unsigned set;

		out[n].reg = P3D_R_DESC;
		out[n].val = with_z ? 0x21c00000u : 0x20c00000u;
		n++;
		if (!no_colour) {
			const unsigned off = with_z ? 3u : 2u;

			out[n].reg = P3D_R_DESC;
			out[n].val = (vertex_alpha ? 0x23c50000u : 0x21c50000u) |
				     (off << 4);
			n++;
		}
		for (set = 0; set < ntex; set++) {
			const unsigned last = set == ntex - 1u;

			out[n].reg = P3D_R_DESC;
			out[n].val = (last ? 0x80000000u : 0u) | 0x20c00000u |
				     ((7u + set) << 16) |
				     ((base + 2u * set) << 4);
			n++;
		}
		out[n].reg = P3D_R_DESC; out[n].val = 0; n++;
		return n;
	}

	if (pos_override) {
		out[n].reg = P3D_R_DESC; out[n].val = pos_override; n++;
		out[n].reg = P3D_R_DESC; out[n].val = col_override; n++;
		out[n].reg = P3D_R_DESC; out[n].val = 0; n++;
		return n;
	}
	/* One coordinate set after both colours (original family, separate
	 * specular): x,y[,z],rgb[a],rgb[a]1,st; derived from the measured
	 * two-colour and textured words. */
	if (textured && vertex_colour1 && ntex == 1 && !texcoord3) {
		const unsigned first = with_z ? 3u : 2u;
		const unsigned st = first + (vertex_colour1 & 4 ? 4u : 3u) +
				    (vertex_colour1 & 2 ? 4u : 3u);

		out[n].reg = P3D_R_DESC;
		out[n].val = with_z ? 0x21c00000u : 0x20c00000u;
		n++;
		n = p3d_colour1_descriptors(out, n, first, vertex_colour1);
		out[n - 1].val &= ~0x80000000u;
		out[n].reg = P3D_R_DESC;
		out[n].val = 0xa0c70000u | (st << 4);
		n++;
		out[n].reg = P3D_R_DESC; out[n].val = 0; n++;
		return n;
	}
	/* A three-component coordinate set takes the dwords the colour would
	 * occupy, so the stream is position then S,T,R and nothing else.  That
	 * keeps dw_per_vertex at the two-component figure: 7 without a depth or
	 * transformed position, 8 with one. */
	if (texcoord3) {
		out[n].reg = P3D_R_DESC;
		out[n].val = with_z ? 0x21c00000u : 0x20c00000u;
		n++;
		out[n].reg = P3D_R_DESC;
		out[n].val = with_z ? 0xa1c70030u : 0xa1c70020u;
		n++;
		out[n].reg = P3D_R_DESC; out[n].val = 0; n++;
		return n;
	}
	if (with_z) {
		/* a THREE-component position: mask 7 in [25:22] instead of 3,
		 * so z reaches the setup unit, and the colour moves to dword 3.
		 * When the isolated depth-RGBA probe is selected, colour expands
		 * from dwords 3..5 to 3..6 and ST moves from 6 to 7.  The former
		 * early return ignored vertex_alpha entirely, which made a nine-
		 * dword stream be fetched as RGB plus ST at 6 (A/S became S/T). */
		out[n].reg = P3D_R_DESC; out[n].val = 0x21c00000u; n++;
		if (textured) {
			out[n].reg = P3D_R_DESC;
			out[n].val = vertex_alpha ? 0x23c50030u : 0x21c50030u;
			n++;
			if (two_tex) {
				out[n].reg = P3D_R_DESC;
				out[n].val = vertex_alpha ? 0x20c70070u : 0x20c70060u;
				n++;
				out[n].reg = P3D_R_DESC;
				out[n].val = vertex_alpha ? 0xa0c80090u : 0xa0c80080u;
				n++;
			} else {
				out[n].reg = P3D_R_DESC;
				out[n].val = vertex_alpha ? 0xa0c70070u : 0xa0c70060u;
				n++;
			}
		} else if (vertex_colour1) {
			n = p3d_colour1_descriptors(out, n, 3, vertex_colour1);
		} else {
			out[n].reg = P3D_R_DESC;
			out[n].val = vertex_alpha ? 0xa3c50030u : 0xa1c50030u;
			n++;
		}
		out[n].reg = P3D_R_DESC; out[n].val = 0; n++;
		return n;
	}
	out[n].reg = P3D_R_DESC; out[n].val = 0x20c00000u; n++;	/* pos, kind 0 */
	if (textured) {
		if (!no_colour) {
			/* The 8-dword non-Z probe is x,y,r,g,b,a,s,t.  The vendor
			 * descriptor's component mask is 0x23, and S/T therefore
			 * starts at dword six rather than five. */
			out[n].reg = P3D_R_DESC;
			out[n].val = vertex_alpha ? 0x23c50020u : 0x21c50020u;
			n++;
		}
		if (two_tex) {
			/* set 0 loses its last flag and set 1 gains it */
			out[n].reg = P3D_R_DESC; out[n].val = 0x20c70050u; n++;
			out[n].reg = P3D_R_DESC; out[n].val = 0xa0c80070u; n++;
		} else {
			out[n].reg = P3D_R_DESC;
			out[n].val = vertex_alpha ? 0xa0c70060u : 0xa0c70050u;
			n++;
		}
	} else if (vertex_colour1) {
		/* A second colour: attribute kind 6, the specular slot the
		 * fixed-function numbering gives WARP input v6, three dwords
		 * after the first colour.  It is what carries the fog
		 * coordinate to the combiner's v1. */
		n = p3d_colour1_descriptors(out, n, 2, vertex_colour1);
	} else {
		out[n].reg = P3D_R_DESC; out[n].val = 0xa1c50020u; n++;
	}
	out[n].reg = P3D_R_DESC; out[n].val = 0; n++;
	return n;
}

/* 0x1110 is a presence bitmask over the attribute KINDS the descriptors
 * declare, not a literal: kind 0 -> bit 0, kind 5 -> bit 1, kind 7 -> bit 8. */
static uint32_t
p3d_attr_present(const struct p3d_rw *desc, size_t n)
{
	static const uint16_t kindbit[11] = {
		[0] = 0x0001, [5] = 0x0002, [6] = 0x0004, [7] = 0x0100,
		[8] = 0x0200, [9] = 0x0400, [10] = 0x0800,
	};
	uint32_t mask = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		uint32_t kind = (desc[i].val >> 16) & 0xf;
		if (desc[i].val && kind < 11)
			mask |= kindbit[kind];
	}
	return mask ? mask : 3;
}

/* The batch fence.  0x12bc is a COUNTER: the tag is armed with bit 0 set and
 * immediately disarmed, 0x41c reads back the last tag that RETIRED, and the
 * capture steps it by 8 every sync.  0x1374 is the strobe the vendor writes
 * between the opcode and the pair. */
static void
p3d_fence_block(struct p3d_cs *cs, uint32_t tag)
{
	struct p3d_rw rw[3];

	rw[0].reg = P3D_R_STROBE; rw[0].val = 0;
	rw[1].reg = P3D_R_FENCE;  rw[1].val = tag | 1;
	rw[2].reg = P3D_R_FENCE;  rw[2].val = tag;
	p3d_reglist(cs, ARRAY(rw));
}

/* Instruction index -> hardware slot address.  Five ALUs are addressed in two
 * passes (slot = 2 * pair + pass) with the pair 3 skipped; every vendor
 * emitter walks exactly this order. */
static const uint8_t p3d_comb_slot[P3D_COMB_MAX_INSTRUCTIONS] = {
	0, 2, 4, 8, 10, 1, 3, 5, 9, 11,
};

/* Signed Q1.10 with round-toward-zero and the vendor's asymmetric clamp. */
static uint32_t
p3d_comb_const_word(float lo, float hi)
{
	int32_t a = (int32_t)(lo * 1024.0f), b = (int32_t)(hi * 1024.0f);

	if (a > 0x3ff) a = 0x3ff;
	if (a <= -1024) a = -1024;
	if (b > 0x3ff) b = 0x3ff;
	if (b <= -1024) b = -1024;
	return P3D_COMBINER_CONST(a, b);
}

static uint32_t
p3d_comb_sources(const struct p3d_comb_half *h, uint32_t slot)
{
	if (!h->used)		/* MOV r0, r0 keeps the half inert */
		return slot | (P3D_COMB_R0 << 5);
	return slot | ((h->src[0].reg & 0x1fu) << 5) |
	       ((h->src[0].mod & 0xfu) << 10) |
	       ((h->src[1].reg & 0x1fu) << 14) |
	       ((h->src[1].mod & 0xfu) << 19) |
	       ((h->src[2].reg & 0x1fu) << 23) |
	       ((uint32_t)(h->src[2].mod & 0xfu) << 28);
}

/* @alpha_half clears the replicate bit that selects alpha: the alpha pipe
 * reads alpha anyway and the vendor packers never set it there. */
static uint32_t
p3d_comb_opcode(const struct p3d_comb_half *h, int alpha_half)
{
	uint32_t word;

	if (!h->used)
		return ((uint32_t)P3D_COMB_MOV << 9) | (P3D_COMB_R0 << 20);
	word = ((uint32_t)(h->op & 0x3fu) << 9) |
	       ((uint32_t)(h->src[0].rep & 7u)) |
	       ((uint32_t)(h->src[1].rep & 7u) << 3) |
	       ((uint32_t)(h->src[2].rep & 7u) << 6) |
	       ((uint32_t)(h->scale & 7u) << 17) |
	       ((uint32_t)(h->dst & 0xfu) << 20) |
	       ((uint32_t)(h->sat ? 1u : 0u) << 24);
	return alpha_half ? word & ~0x124u : word;
}

/* MOV r0, r0 in both halves of an unused combiner slot.  The alpha half
 * must be stated too: it keeps what the last client left, and after power-on
 * that dropped an untextured RGBA draw's second triangle (vertex-count) until
 * a compiled fragment program happened to rewrite it; X's conversion stream
 * needed the same. */
static void
p3d_emit_comb_pad(struct p3d_cs *cs, uint32_t slot)
{
	struct p3d_rw rw[4] = {
		{ P3D_R_COMB_RGB, 0x80u | slot },
		{ P3D_R_COMB_ALPHA, 0x80u },
		{ 0x126c, 0x00400400u },
		{ 0x1268, 0x00400400u },
	};

	p3d_reglist(cs, rw, 4);
}

/* Emit a compiled fragment program starting at instruction index @first,
 * padding the slot walk out to the five-instruction quantum. */
static void
p3d_emit_comb_program(struct p3d_cs *cs, const struct p3d_comb_program *prog,
		      unsigned first)
{
	struct p3d_rw rw[8];
	unsigned i, emitted = first;

	for (i = 0; i < prog->count && emitted < P3D_COMB_QUANTUM; i++, emitted++) {
		const struct p3d_comb_inst *in = &prog->inst[i];
		uint32_t slot = p3d_comb_slot[emitted];

		rw[0].reg = P3D_R_COMB_RGB;
		rw[0].val = p3d_comb_sources(&in->rgb, slot);
		rw[1].reg = P3D_R_COMB_ALPHA;
		rw[1].val = p3d_comb_sources(&in->alpha, 0);
		rw[2].reg = 0x126c; rw[2].val = p3d_comb_opcode(&in->rgb, 0);
		rw[3].reg = 0x1268; rw[3].val = p3d_comb_opcode(&in->alpha, 1);
		/* Component order, hardware-proven with the GL combiner test: the
		 * first register carries red and ALPHA, the second blue and green.
		 * Packing (r, g) and (b, a) made a constant's green read as its
		 * alpha in an otherwise exact MUL. */
		rw[4].reg = 0x1278;
		rw[4].val = in->has_const[0] ?
			p3d_comb_const_word(in->constant[0][0], in->constant[0][3]) : 0;
		rw[5].reg = 0x127c;
		rw[5].val = in->has_const[0] ?
			p3d_comb_const_word(in->constant[0][2], in->constant[0][1]) : 0;
		rw[6].reg = 0x1280;
		rw[6].val = in->has_const[1] ?
			p3d_comb_const_word(in->constant[1][0], in->constant[1][3]) : 0;
		rw[7].reg = 0x1284;
		rw[7].val = in->has_const[1] ?
			p3d_comb_const_word(in->constant[1][2], in->constant[1][1]) : 0;
		p3d_reglist(cs, rw, 8);
	}
	/* MOV r0, r0 in every unused slot: the padding is the terminator. */
	for (; emitted < P3D_COMB_QUANTUM; emitted++)
		p3d_emit_comb_pad(cs, p3d_comb_slot[emitted]);
}

/* Emit @prog with texture register t<u> renamed to the physical pipe that
 * holds unit u's sample. */
static void
p3d_emit_comb_program_units(struct p3d_cs *cs, const struct p3d_state *st,
			    unsigned nunit, unsigned first)
{
	struct p3d_comb_program p = *st->comb;
	uint8_t map[4];
	unsigned i, h, k;

	p3d_state_treg_map(st, nunit, map);
	for (i = 0; i < p.count && i < P3D_COMB_MAX_INSTRUCTIONS; i++) {
		struct p3d_comb_half *half[2] = { &p.inst[i].rgb, &p.inst[i].alpha };

		for (h = 0; h < 2; h++) {
			if (half[h]->dst <= P3D_COMB_T3)
				half[h]->dst = map[half[h]->dst];
			for (k = 0; k < 3; k++)
				if (half[h]->src[k].reg <= P3D_COMB_T3)
					half[h]->src[k].reg =
						map[half[h]->src[k].reg];
		}
	}
	p3d_emit_comb_program(cs, &p, first);
}

/* Emit one fixed-function texture-environment ALU stage.  The texture-address
 * stages occupy slots 0 (and 2 for unit 1), so the matching ALU stages are 2
 * or 4 for one unit and 4 / 8 for two.  The encodings are the measured table
 * in docs/parhelia-multitexture.md section 4.2. */
static void
p3d_emit_texenv(struct p3d_cs *cs, uint32_t slot, unsigned unit, int first,
		const struct p3d_texture *t, uint32_t comb_op)
{
	struct p3d_rw rw[8];
	uint32_t tex = unit << 5;
	uint32_t prev = first ? 0x10u : 4u;
	uint32_t rgb, alpha, rgbop, alphaop, c0ra = 0, c0bg = 0;

	switch (t->env) {
	case P3D_TEXENV_REPLACE:
		rgb = slot | tex;
		alpha = 0;
		rgbop = alphaop = 0x01400400u;
		break;
	case P3D_TEXENV_ADD:
		/* GL_ADD adds RGB but multiplies alpha. */
		rgb = slot | tex | (prev << 14);
		alpha = tex | (prev << 23);
		rgbop = 0x01400c00u;
		alphaop = 0x01400000u;
		break;
	case P3D_TEXENV_DECAL:
		/* LRP(TEX, PREV, TEX.a); alpha is passed through from PREV. */
		rgb = slot | tex | (prev << 14) | (unit << 23);
		alpha = prev << 5;
		rgbop = 0x01400b00u;
		alphaop = 0x01400400u;
		break;
	case P3D_TEXENV_BLEND:
		/* LRP(c0, PREV, TEX); c0 is GL_TEXTURE_ENV_COLOR. */
		rgb = slot | (0xeu << 5) | (prev << 14) | (unit << 23);
		alpha = tex | (prev << 23);
		rgbop = 0x01400a00u;
		alphaop = 0x01400000u;
		c0ra = t->env_colour_ra;
		c0bg = t->env_colour_bg;
		break;
	case P3D_TEXENV_MODULATE:
	default:
		rgb = slot | tex | (prev << 23);
		alpha = tex | (prev << 23);
		rgbop = alphaop = 0x01400000u;
		break;
	}

	rw[0].reg = P3D_R_COMB_RGB;   rw[0].val = rgb;
	rw[1].reg = P3D_R_COMB_ALPHA; rw[1].val = alpha;
	rw[2].reg = 0x126c;           rw[2].val = rgbop | comb_op;
	rw[3].reg = 0x1268;           rw[3].val = alphaop | comb_op;
	rw[4].reg = 0x1278;           rw[4].val = c0ra;
	rw[5].reg = 0x127c;           rw[5].val = c0bg;
	rw[6].reg = 0x1280;           rw[6].val = 0;
	rw[7].reg = 0x1284;           rw[7].val = 0;
	p3d_reglist(cs, rw, 8);
}

/* Whether the vertex fetch of @st delivers a colour (attribute kind 5) to
 * WARP input v5.  Without it v5 keeps the previous draw's value, from any
 * client, and a program reading it inherits that colour. */
int
p3d_state_fetches_colour(const struct p3d_state *st)
{
	struct p3d_rw desc[8];
	size_t nd = p3d_descriptors(desc, st->tex != NULL,
				    p3d_position_has_z(st), st->pos_desc,
				    st->col_desc,
				    st->tex && st->tex->env == P3D_TEXENV_REPLACE,
				    p3d_state_coord_sets(st), !!st->vertex_alpha,
				    !!st->texcoord3, !!st->vertex_colour1);

	for (size_t i = 0; i < nd; i++)
		if (desc[i].val && ((desc[i].val >> 16) & 0xf) == 5)
			return 1;
	return 0;
}

/* The vendor validator admits only sampling kinds (TEX, TEXBEM, TEXREG2AR/GB)
 * on a ganged mask (PSER 0x17b720, KRN mtx1414): any other stage of the
 * original family takes its pair's first pipe alone (non-LX n=1/2 tables). */
static uint32_t
p3d_orig_unit_pipes(const struct p3d_texture *tu, unsigned u)
{
	const unsigned op = tu->ta_op;
	const int gang = op == 0 || op == 4 || op == 0x10 || op == 0x11;

	return gang ? 0x3u << (2 * u) : 0x1u << (2 * u);
}

void
p3d_emit_state(struct p3d_cs *cs, const struct p3d_state *st)
{
	/* The raster class and the WARP class are not the same thing for a wide
	 * point: 0x1130 and DWGCTL carry class 3, while the WARP mode latch and
	 * the bind packet stay at the primitive's own class.  Raising the WARP
	 * class with them drops the primitive outright (measured). */
	const uint32_t warp_cls = p3d_prim_class(st->prim);
	uint32_t cls = st->prim == P3D_POINTS && st->point_class ?
		       st->point_class : warp_cls;
	uint32_t pipectl, dwgctl;
	struct p3d_rw rw[10], desc[6];
	const int lx = st->lx_pipe_route && st->single_texture_pipe == 1;
	size_t nd;

	/* 0x1130: set only the decoded fields and keep the captured remainder */
	pipectl = (0x0200200cu & ~0x03001001u) | (cls << 24) |
		  (st->flat ? 0x1000u : 0u) | ((uint32_t)st->cull & 3u) |
		  st->pipectl_extra;
	dwgctl  = p3d_dwgctl(st);

	/* -- raster / ROP ------------------------------------------------- */
	rw[0].reg = P3D_R_SCISSOR_MIN;
	rw[0].val = st->scissor ? ((uint32_t)st->scissor_miny << 16) |
	                            st->scissor_minx : 0;
	rw[1].reg = P3D_R_SCISSOR_MAX;
	rw[1].val = st->scissor ? ((uint32_t)st->scissor_maxy << 16) |
	                            st->scissor_maxx :
	                            (st->screen_h << 16) | st->screen_w;
	rw[2].reg = P3D_R_RASTERMODE;
	/* The vendor's own unit-point draw uses the class-0 raster image.  The
	 * ordinary triangle/line surface image is different even though the
	 * decoded low control bits agree. */
	rw[2].val = st->point_rastermode ? st->point_rastermode :
		    st->prim == P3D_POINTS ? 0x04000024u : 0x0bb80024u;
	rw[3].reg = 0x12f8;            rw[3].val = 0x007fe000u;	/* AA threshold */
	p3d_reglist(cs, rw, 4);

	/* Line stipple.  Every state image states it, because a register this
	 * driver never wrote could be left holding another client's pattern and
	 * silently dash our lines.  GL's own stipple stays with the draw module
	 * for now: it applies to every line, including the wide and smooth ones
	 * this driver expands into triangles, which the hardware line unit would
	 * not stipple.  Enabling the hardware here therefore means disabling
	 * that stage at the same time, and only for lines that stay native. */
	{
		const unsigned repeat = st->line_stipple_enable &&
					st->line_stipple_repeat ?
					st->line_stipple_repeat : 1u;
		const unsigned pattern = st->line_stipple_enable ?
					 st->line_stipple_pattern & 0xffffu :
					 0xffffu;

		rw[0].reg = P3D_R_LINE_STIPPLE;
		rw[0].val = ((repeat & 0xffffu) << 16) | pattern;
		p3d_reglist(cs, rw, 1);
	}

	if (st->probe_reg) {
		rw[0].reg = st->probe_reg;
		rw[0].val = st->probe_val;
		p3d_reglist(cs, rw, 1);
	}

	/* The fog unit.  The vendor emits these three together, and a disabled
	 * fog still states them so a register another client left set cannot
	 * tint this draw. */
	{
		const unsigned r = p3d_fog_byte(st->fog_colour[0]);
		const unsigned g = p3d_fog_byte(st->fog_colour[1]);
		const unsigned b = p3d_fog_byte(st->fog_colour[2]);

		rw[0].reg = P3D_R_FOG;
		rw[0].val = st->fog ? 1u | ((st->fog_mode & 3u) << 1) |
				      (b << 6) | (g << 15) | (r << 24) : 0u;
		rw[1].reg = P3D_R_FOG_START;
		rw[1].val = st->fog ? p3d_fog_depth(st->fog_start) : 0u;
		rw[2].reg = P3D_R_FOG_END;
		rw[2].val = st->fog ? p3d_fog_depth(st->fog_end) : 0x00ffffffu;
		p3d_reglist(cs, rw, 3);
		if (st->fog && st->fog_table) {
			struct p3d_rw tab[64];
			size_t k;

			for (k = 0; k < 64; k++) {
				tab[k].reg = P3D_R_FOG_TABLE;
				tab[k].val = st->fog_table[k];
			}
			p3d_reglist(cs, tab, 64);
		}
	}

	rw[0].reg = P3D_R_BLEND; rw[0].val = p3d_blendctl(st);
	/* the alpha test shares the depth test's compare encoding; a disabled
	 * test carries ALWAYS, which is what the vendor's baseline holds */
	rw[1].reg = P3D_R_ALPHATEST;
	rw[1].val  = (st->alpha_test ? (st->alpha_ref & 0xffu) << 8 : 0u) |
		     ((uint32_t)(st->alpha_test ? st->alpha_func
					        : P3D_CMP_ALWAYS) << 1) |
		     (st->alpha_test ? 1u : 0u);
	rw[2].reg = 0x12f0; rw[2].val = st->blend_const_ba;
	rw[3].reg = 0x12ec; rw[3].val = st->blend_const_rg;
	p3d_reglist(cs, rw, 4);

	/* Depth and stencil share the 0x12c8 control word and S8Z24 surface.
	 * The stencil masks/reference occupy 0x12cc/0x12d0; depth write remains
	 * independent in 0x12d0 bit 8. */
	rw[0].reg = P3D_R_ZFUNC;
	rw[0].val  = st->depth_raw ? st->depth_raw :
		     p3d_depth_stencil_bits(st->depth.fmt) << 17 |
		     ((uint32_t)(st->depth_test ? st->depth_func
					        : P3D_CMP_LESS) << 1) |
		     (st->depth_test ? 1u : 0u) |
		     (st->stencil_test ? 1u << 4 : 0u) |
		     ((uint32_t)(st->stencil_test ? st->stencil_func :
		                                      P3D_CMP_ALWAYS) << 5) |
		     ((uint32_t)(st->stencil_test ? st->stencil_fail :
		                                      P3D_STENCIL_KEEP) << 8) |
		     ((uint32_t)(st->stencil_test ? st->stencil_zfail :
		                                      P3D_STENCIL_KEEP) << 11) |
		     ((uint32_t)(st->stencil_test ? st->stencil_zpass :
		                                      P3D_STENCIL_KEEP) << 14);
	rw[1].reg = P3D_R_STENCIL_REF;
	rw[1].val = (st->stencil_test ? st->stencil_value_mask : 0xffu) |
		    ((uint32_t)(st->stencil_test ? st->stencil_ref : 0u) << 8);
	rw[2].reg = P3D_R_ZWRITE;
	rw[2].val  = (st->stencil_test ? st->stencil_write_mask : 0xffu) |
		     (st->depth_write ? 0x100u : 0u);
	rw[3].reg = 0x12c0; rw[3].val = st->zclip_set ? st->zclip_min : 0x80000000u;
	rw[4].reg = 0x12c4; rw[4].val = st->zclip_set ? st->zclip_max : 0x7fffffffu;
	p3d_reglist(cs, rw, 5);

	rw[0].reg = 0x12d4; rw[0].val = 0;
	rw[1].reg = 0x12d8; rw[1].val = 0;
	rw[2].reg = 0x12dc; rw[2].val = 0x00ffffffu;
	p3d_reglist(cs, rw, 3);

	/* -- constants ----------------------------------------------------
	 *
	 * The 0x41 block loads go between the control binds and the vertex
	 * format group.  c8/c9 are free only while vertex blending is off; the
	 * clip/raster block reads its own copy at 0x160. */
	/* Transformed vertices always need the matrix and viewport blocks; a
	 * state that binds the loader and names the blocks it wants gets those
	 * without also claiming a transformed vertex layout, which is what a
	 * program reading a plain constant needs. */
	if (st->transform || (st->const_loader_entry && st->const_mask)) {
		float vp[8];
		size_t i;

		for (i = 0; i < 4; i++) {
			vp[i]     = st->vp_scale[i];
			vp[4 + i] = st->vp_offset[i];
		}
		/* The loader is fed as a VERTEX program: it consumes one vec4 per
		 * iteration through the attribute port, so the fetch has to be
		 * pointed at a single four-component attribute first.  Our draw
		 * descriptors describe position+colour and are emitted later, so
		 * without this the loader waits for data that never arrives -
		 * the engine stops retiring even with no draw in the stream. */
		rw[0].reg = P3D_R_STREAM0;    rw[0].val = 0;
		rw[1].reg = P3D_R_STRIDE0;    rw[1].val = 4;
		rw[2].reg = P3D_R_PRIMWORD;   rw[2].val = 0x0002305cu;
		p3d_reglist(cs, rw, 3);
		rw[0].reg = P3D_R_DESC_INDEX; rw[0].val = 0;
		rw[1].reg = P3D_R_DESC;       rw[1].val = 0x83c00000u;
		/* close the list: every draw descriptor list ends with a zero
		 * terminator, and leaving the loader's one-attribute list open
		 * leaves descriptor state behind for the next submission */
		rw[2].reg = P3D_R_DESC;       rw[2].val = 0;
		p3d_reglist(cs, rw, 3);

		if (st->base_reg_entry) {
			/* the vendor's kind-1 packet, verbatim apart from the
			 * entries: v0 = base registers, v1 = a0 = (0,1,2,3),
			 * v2.y = the 0x1130 control word */
			static const uint32_t k1[] = {
				0x4106000f,
				0x000000e0, 0x00000160, 0x00000080, 0x00000800,
				0x00000000, 0x00000001, 0x00000002, 0x00000003,
				0x02003004, 0x0200300c, 0x00000000, 0x00000000,
				0x00000000, 0xffffffff, 0xffffffff, 0xffffffff,
			};
			uint32_t pkt[sizeof k1 / 4];
			size_t k;

			for (k = 0; k < sizeof k1 / 4; k++) pkt[k] = k1[k];
			pkt[13] = st->clipper_entry;

			rw[0].reg = P3D_R_PIPECTL;     rw[0].val = 0x0700200cu;
			p3d_reglist(cs, rw, 1);
			rw[0].reg = P3D_R_WARP_ENTRY;  rw[0].val = st->base_reg_entry;
			p3d_bindpacket(cs, P3D_WARP_MODE_LOADER, rw, 1, 0);
			p3d_raw(cs, pkt, sizeof k1 / 4);
		}
		if (st->const_loader_entry) {
			rw[0].reg = P3D_R_WARP_ENTRY;
			rw[0].val = st->const_loader_entry;
			p3d_bindpacket(cs, P3D_WARP_MODE_LOADER, rw, 1, 0);
		}
		p3d_emit_constant_group(cs, st, vp);


		/* The vendor brackets the loads: enter the control class, stream
		 * the blocks, then a bind-shaped packet whose payload is 0x1130
		 * rather than an entry.  Without that trailing write the engine
		 * never leaves the load - it stops retiring with no draw even
		 * when the stream carries no draw packet at all. */
		rw[0].reg = P3D_R_PIPECTL; rw[0].val = 0x0200200cu;
		p3d_bindpacket(cs, P3D_WARP_MODE_LOADER, rw, 1, 0);
	}

	/* -- vertex format ------------------------------------------------ */
	rw[0].reg = 0x1120; rw[0].val = 0x45ffc5ffu;
	rw[1].reg = 0x1124; rw[1].val = 0xc5ff45ffu;
	rw[2].reg = 0x1128; rw[2].val = 0;
	rw[3].reg = 0x112c; rw[3].val = 0x3f800000u;
	p3d_reglist(cs, rw, 4);

	nd = p3d_descriptors(desc, st->tex != NULL, p3d_position_has_z(st),
			     st->pos_desc, st->col_desc,
			     st->tex && st->tex->env == P3D_TEXENV_REPLACE,
			     st->second_texcoord_probe && !st->tex1 ? 2u :
			     p3d_state_coord_sets(st),
			     st->vertex_alpha, st->texcoord3,
			     st->vertex_colour1);
	if (st->oow)
		p3d_descriptors_add_w(desc, nd);
	if (st->texcoord_q)
		p3d_descriptors_add_q(desc, nd);
	if (st->packed_colour && !st->pos_desc)
		p3d_descriptors_pack_colour(desc, nd,
					    (p3d_position_has_z(st) ? 3u : 2u) +
					    !!st->oow, !!st->vertex_alpha);

	/* Interpolant 5 is the fog factor and 0x1114's nibble 5 already sources
	 * it from o2, so the fog unit needs only its presence bit here. */
	rw[0].reg = P3D_R_ATTR_PRESENT;
	rw[0].val = p3d_attr_present(desc, nd) | (st->fog ? 0x20u : 0u) |
		    st->attr_present_extra;
	rw[1].reg = P3D_R_ATTR_MAP0;    rw[1].val = 0x00287210u;
	rw[2].reg = P3D_R_ATTR_MAP1;
	rw[2].val = st->coord_map ? st->coord_map : 0x00006543u;
	p3d_reglist(cs, rw, 3);

	/* -- the per-vertex WARP bind --------------------------------------
	 *
	 * 0x1104 alongside 0x1100 comes from mtx676, the kernel's own WARP
	 * self-test, which writes the entry to both; the captured GL draw path
	 * writes only 0x1100.  We emit both because the self-test is the one
	 * place the entry is provably consumed. */
	rw[0].reg = P3D_R_WARP_ENTRY;  rw[0].val = st->warp_entry;
	rw[1].reg = P3D_R_WARP_ENTRY2; rw[1].val = st->warp_entry;
	rw[2].reg = P3D_R_WARP_PAGE;   rw[2].val = 2;
	p3d_bindpacket(cs, 0x20000000u | warp_cls, rw, 3, 1);

	rw[0].reg = P3D_R_STROBE;    rw[0].val = 0;
	rw[1].reg = P3D_R_WARP_MODE; rw[1].val = 0x20000000u | warp_cls;
	rw[2].reg = P3D_R_WARP_MODE; rw[2].val = 0x20000000u | warp_cls;
	rw[3].reg = P3D_R_WARP_MODE; rw[3].val = warp_cls;
	rw[4].reg = P3D_R_PIPECTL;   rw[4].val = pipectl;
	rw[5].reg = 0x111c;          rw[5].val = 0x00000fdbu;
	rw[6].reg = 0x1144;          rw[6].val = st->vertex_out_extra;
	p3d_reglist(cs, rw, 7);

	/* -- vertex fetch -------------------------------------------------- */
	rw[0].reg = P3D_R_VTXCAP_HI;  rw[0].val = 0;
	rw[1].reg = P3D_R_VTXCAP;     rw[1].val = p3d_vtxcap(st->dw_per_vertex);
	rw[2].reg = P3D_R_DESC_INDEX; rw[2].val = 0;
	p3d_reglist(cs, rw, 3);
	p3d_reglist(cs, desc, nd);

	/* -- surfaces ------------------------------------------------------ */
	rw[0].reg = P3D_R_SURF0_BASE;     rw[0].val = st->colour.base;
	rw[1].reg = P3D_R_SURF0_BASE + 4; rw[1].val = p3d_surface_cfg(&st->colour);
	rw[2].reg = 0x1230;               rw[2].val = 0;
	rw[3].reg = 0x1234;               rw[3].val = 0x32020000u;
	p3d_reglist(cs, rw, 4);

	if (st->has_depth) {
		unsigned slot = st->depth_slot ? st->depth_slot : 1;
		rw[0].reg = P3D_R_SURF0_BASE + 8 * slot;
		rw[0].val = st->depth.base;
		rw[1].reg = P3D_R_SURF0_BASE + 8 * slot + 4;
		rw[1].val = p3d_surface_cfg(&st->depth);
		p3d_reglist(cs, rw, 2);
	}

	rw[0].reg = 0x1318; rw[0].val = 0;
	rw[1].reg = P3D_R_FCOL; rw[1].val = 0;
	rw[2].reg = P3D_R_COLMASK; rw[2].val = p3d_colour_mask(st);
	p3d_reglist(cs, rw, 3);

	/* -- the batch fence, then the draw arm ---------------------------- */
	if (st->fence)
		p3d_fence_block(cs, st->fence);

	/* Both vendor drivers raise MemLevels for targets above 250,000 pixels.
	 * Q3's 640x480 buffer crosses that exact threshold. */
	rw[0].reg = P3D_R_WATERMARK;
	rw[0].val = (uint64_t)st->screen_w * st->screen_h > 250000u ?
		    0x3fff9084u : 0x3fff8842u;
	p3d_reglist(cs, rw, 1);
	rw[0].reg = P3D_R_DWGCTL; rw[0].val = dwgctl;
	p3d_reglist(cs, rw, 1);
	/* 32.0f, or the LX family's 31.0f when the caller asks for it: the
	 * vendor clamps this shadow one unit down on exactly that family
	 * (KRN mtx2572, emitted by mtx2393). */
	rw[0].reg = P3D_R_GUARDBAND;
	/* 16.0f with three or more P690 units (KRN mtx958). */
	rw[0].val = st->guard_band_bits ? st->guard_band_bits :
		    st->single_texture_pipe && p3d_state_units(st) > 2 ?
		    0x41800000u :
		    st->lx_family ? 0x41f80000u : 0x42000000u;
	p3d_reglist(cs, rw, 1);

	rw[0].reg = 0x121c;             rw[0].val = 0;
	rw[1].reg = P3D_R_TEX_ENABLE;   rw[1].val = 0x0001000fu;
	rw[2].reg = P3D_R_TEX_ENABLE+4; rw[2].val = 0;
	rw[3].reg = P3D_R_TEX_ENABLE+8; rw[3].val = 0;
	rw[4].reg = P3D_R_TEX_ENABLE+12;rw[4].val = 0;
	p3d_reglist(cs, rw, 5);

	/* 0x11e8 is the texture-cache partition latch.  Diagnostic and
	 * vendor-style clients can program 0x00500000, while the hardware-proven
	 * production GL path uses zero, so a complete state image writes the
	 * requested value including zero and a later context cannot inherit
	 * another client's cache mode.  Only the LX family has the latch: the
	 * original Parhelia's texture engine stops retiring once it is written,
	 * and no vendor capture on that chip writes it at all. */
	if ((st->tex_cache_latch || st->texture_cache_probe) &&
	    !(lx && st->tex)) {
		rw[0].reg = P3D_R_TEX_CACHE;
		rw[0].val = st->texture_cache_probe;
		p3d_reglist(cs, rw, 1);
	}

	/* -- texture ------------------------------------------------------- */
	if (st->tex) {
		const struct p3d_texture *t = st->tex;
		unsigned nunit = p3d_state_units(st), u;
		uint32_t orig_union = 0;
		/* P690 units of a multi-unit draw sit alone on their pipes: the
		 * vendor emits no merges and the ALU reads each unit's first pipe
		 * (an unowned pipe reads alpha 1). */
		const int pair = nunit > 1 && st->single_texture_pipe &&
				 !st->sampled_alpha_companion;
		/* original family, three or four units: one merge for a gang on
		 * pipes 0+1, then one stage per unit */
		const int orig_multi = p3d_orig_multi(st, nunit);
		const unsigned merges = orig_multi &&
			p3d_orig_multi_mask(st, nunit, 0) == 0x3u ? 1u : 0u;
		/* PSER 0x1dbe58 / KRN mtx2422 LX masks for three and four units */
		static const uint8_t lx_pipes[2][4] = {
			{ 0x1, 0x2, 0x8 }, { 0x1, 0x2, 0x4, 0x8 },
		};
		/* 0x11e8 per unit bank (PSER 0x1d63f0, KRN mtx1783) */
		static const uint32_t lx_cache[2][4] = {
			{ 0x01600000u, 0x01600000u, 0x00400000u },
			{ 0x01600000u, 0x01600000u, 0x01600000u, 0x01600000u },
		};

		rw[0].reg = 0x121c; rw[0].val = 0;
		p3d_reglist(cs, rw, 1);

		/* the capture packs the enable and the surface triple into one
		 * tag word, which makes "enable before surface" structural */
		/* 0x11c0..0x11e4 is ONE windowed register file shared by all four
		 * units; 0x1200 + unit*4 with the low nibble forced to 1<<unit
		 * selects which bank the following writes land in.  There is no
		 * 0x11c0 + unit*stride. */
		for (u = 0; u < nunit; u++) {
			const struct p3d_texture *tu = p3d_state_unit(st, u);
			/* The live draw assigns a complete texture-address pipe pair,
			 * not the one-hot context-prime values: unit 0 owns pipes 0+1
			 * and unit 1 owns pipes 2+3.  0x40101/02 only primes private
			 * banks during context creation and samples black in a draw.
			 *
			 * Three or four units cannot pair up: each takes one pipe,
			 * 1 << u, which is what all three vendor sources agree on for
			 * the four-unit case (the Linux ICD, KRN mtx398 and the PARD
			 * table - parhelia-multitexture.md section 1.3).
			 */
			static const uint32_t pipe_pair[2] = { 0x3u, 0xcu };
			const uint32_t pipes = orig_multi ?
					       p3d_orig_multi_mask(st, nunit, u) :
					       nunit > 2 ? 1u << u :
					       st->single_texture_pipe ? pipe_pair[u] :
					       p3d_orig_unit_pipes(tu, u);

			orig_union |= pipes;
			rw[0].reg = P3D_R_TEX_ENABLE + 4 * u;
			rw[0].val = 0x00040100u | pipes;
			if (st->single_texture_pipe && nunit > 2)
				rw[0].val = 0x00040100u | lx_pipes[nunit - 3][u];
			else if (st->single_texture_pipe && u == 0)
				/* A TEXCOORD unit is not ganged: both pipes would
				 * pass the coordinate and the merge would add them. */
				rw[0].val = lx && nunit == 1 && !tu->ta_op ? 0x00040105u :
					    0x00040101u;
			else if (st->single_texture_pipe && u == 1)
				/* SUN90/P690 allocates units 0/1 to pipes 0/2. */
				rw[0].val = 0x00040104u;
			/* [18:16] is the coordinate mode: the default 4 feeds
			 * the address unit two coordinates, 6 three. */
			if (tu->coord_mode)
				rw[0].val = (rw[0].val & ~0x00070000u) |
					    ((tu->coord_mode & 7u) << 16);
			rw[0].val |= (tu->ta_op & 0x1fu) << 19;
			rw[1].reg = P3D_R_TEX_SURF_BASE; rw[1].val = tu->base;
			rw[2].reg = P3D_R_TEX_SURF_W;
			rw[2].val = (tu->width & 0x3fff) |
				    ((tu->texelcode & 7) << 16) |
				    ((tu->type & 3u) << 19) | (tu->fmt << 24);
			rw[3].reg = P3D_R_TEX_SURF_H;
			rw[3].val = (tu->height & 0x3fff) |
				    ((tu->depth ? tu->depth & 0xfffu : 1u) << 16) |
				    ((tu->max_level & 0xfu) << 28);
			p3d_reglist(cs, rw, 4);

			rw[0].reg = P3D_R_TEX_FILTER; rw[0].val = p3d_tex_filter(tu);
			rw[1].reg = P3D_R_TEX_WRAP;   rw[1].val = p3d_tex_wrap(tu);
			rw[2].reg = 0x11d4;           rw[2].val = tu->aux ? tu->aux[3] : 0;
			rw[3].reg = 0x11d8;           rw[3].val = tu->aux ? tu->aux[4] : 0;
			p3d_reglist(cs, rw, 4);

			rw[0].reg = 0x11cc;
			rw[0].val = tu->aux ? tu->aux[0] : 0xffffffffu;
			rw[1].reg = 0x11d0; rw[1].val = tu->aux ? tu->aux[1] : 0;
			/* 0x11c8 is the border colour the modes from code 6 up
			 * sample outside [0,1]; the vendor loads it only when
			 * an axis asks for one and writes zero otherwise. */
			rw[2].reg = 0x11c8;
			rw[2].val = tu->aux ? tu->aux[2] :
				    p3d_tex_border_used(tu) ?
				    p3d_border_colour(tu->fmt,
						      tu->border_colour) : 0;
			/* the vendor LX table value for one or two stages */
			rw[3].reg = P3D_R_TEX_CACHE;
			rw[3].val = st->single_texture_pipe && nunit > 2 ?
				    lx_cache[nunit - 3][u] : 0x00500000u;
			p3d_reglist(cs, rw, lx || (st->single_texture_pipe &&
						    nunit > 2) ? 4 : 3);
		}

		if (st->single_texture_pipe && nunit > 2) {
			/* three units park the leftover pipe 2 on unit 3
			 * (PSER 0xcbac8, KRN 0x3f878); four leave none */
			if (nunit == 3) {
				rw[0].reg = P3D_R_TEX_ENABLE + 12;
				rw[0].val = 0x00010004u;
				rw[1].reg = P3D_R_TEX_CACHE; rw[1].val = 0;
				p3d_reglist(cs, rw, 2);
			}
		} else if (lx) {
			/* park the pipes no stage owns on the first free unit */
			for (u = nunit; u < 4; u++) {
				rw[0].reg = P3D_R_TEX_ENABLE + 4 * u;
				rw[0].val = u == nunit ? 0x0001000au : 0;
				rw[1].reg = P3D_R_TEX_CACHE; rw[1].val = 0;
				p3d_reglist(cs, rw, 2);
			}
		} else if (nunit >= 3) {
			/* The vendor's table-driven tail is all pads at four
			 * units and only 0x120c at three, which parks the pipes no
			 * unit owns (KRN mtx1381 0x3f86b: [1,2,8] parks 0x00010004). */
			if (nunit == 3) {
				rw[0].reg = P3D_R_TEX_ENABLE + 12;
				rw[0].val = orig_union != 0xfu ?
					    0x00010000u | (~orig_union & 0xfu) : 0;
				p3d_reglist(cs, rw, 1);
			}
		} else if (nunit == 1) {
			rw[0].reg = P3D_R_TEX_ENABLE + 4;
			/* The normal P690 one-pipe path leaves unit 1 clear.  The
			 * vendor capture instead retains a disabled unit-1 descriptor
			 * claiming pipes 2+3.  Keep that state an explicit diagnostic
			 * variant: it must never widen a production packet without a
			 * texture-alpha result. */
			rw[0].val = st->single_texture_pipe == 2 ? 0x0001000cu :
				    st->single_texture_pipe ? 0 : 0x00010000u | (~orig_union & 0xfu);
			rw[1].reg = P3D_R_TEX_ENABLE + 8;  rw[1].val = 0;
			rw[2].reg = P3D_R_TEX_ENABLE + 12; rw[2].val = 0;
			p3d_reglist(cs, rw, 3);
		} else {
			rw[0].reg = P3D_R_TEX_ENABLE + 8;  rw[0].val =
				!st->single_texture_pipe && orig_union != 0xfu ?
				0x00010000u | (~orig_union & 0xfu) : 0;
			rw[1].reg = P3D_R_TEX_ENABLE + 12; rw[1].val = 0;
			p3d_reglist(cs, rw, 2);
		}

		/* the three registers only a same-boot diff could reveal: they
		 * are UNSET in an untextured draw, so a last-value diff across
		 * frames hides them, and without them the vendor's own
		 * 0x1200/0x1204 pair stalls the draw outright */
		rw[0].reg = P3D_R_TEX_UNK10FC; rw[0].val = 0x3e4ccccdu;
		p3d_reglist(cs, rw, 1);
		rw[0].reg = P3D_R_TEX_UNK1330; rw[0].val = 0x3e4ccccdu;
		rw[1].reg = P3D_R_TEX_UNK1334; rw[1].val = 0x3f800000u;
		p3d_reglist(cs, rw, 2);

		/* Slot 0 is ADD t0, t0, t1: it merges the partial samples of a
		 * stage ganged over two pipes (0528: pipes 0+1).  The P690 gang
		 * is pipes 0+2, merged with ADD t0, t0, t2; each pipe returns
		 * zero for the fragments it does not own. */
		rw[0].reg = P3D_R_COMB_RGB;
		rw[0].val = lx && nunit == 1 && !t->ta_op ? 0x00008000u :
							    0x00004000u;
		rw[1].reg = P3D_R_COMB_ALPHA; rw[1].val = rw[0].val;
		rw[2].reg = 0x126c;           rw[2].val = 0x00000c00u;
		rw[3].reg = 0x1268;           rw[3].val = 0x00000c00u;
		rw[4].reg = 0x1278; rw[4].val = 0;
		rw[5].reg = 0x127c; rw[5].val = 0;
		rw[6].reg = 0x1280; rw[6].val = 0;
		rw[7].reg = 0x1284; rw[7].val = 0;
		if (!pair && (!orig_multi || merges))
			p3d_reglist(cs, rw, 8);

		/* MODULATE = MUL(TEX, PREV).  REPLACE drops the alpha slot and
		 * leaves 0x1268 at its resting value, exactly as the capture's
		 * texrepl differs from tex. */
		if (nunit > 1 && !pair && !orig_multi) {
			/* slot 2: the second unit's texture-address stage.  On
			 * PARD's P690 two-unit route, logical unit 1 occupies physical
			 * pipe 2 (0x1204 = 0x40104), and live hardware proved that the
			 * TA Areg field must select that physical pipe as well. */
			/* The source names physical pipes.  The P690 route puts unit 1
			 * alone on pipe 2 (pipe 1 is idle and reads zero); the original
			 * board gangs it over pipes 2+3, merged as the vendor's mask-0xc
			 * ADD t2, t2, t3.  Either result lands in t1. */
			uint32_t ta_sources = st->single_texture_pipe ?
					      0x00004040u : 0x0000c040u;
			rw[0].reg = P3D_R_COMB_RGB;
			rw[0].val = ta_sources | 2u;
			rw[1].reg = P3D_R_COMB_ALPHA;
			rw[1].val = ta_sources;
			rw[2].reg = 0x126c;           rw[2].val = 0x00100c00u;
			rw[3].reg = 0x1268;           rw[3].val = 0x00100c00u;
			rw[4].reg = 0x1278; rw[4].val = 0;
			rw[5].reg = 0x127c; rw[5].val = 0;
			rw[6].reg = 0x1280; rw[6].val = 0;
			rw[7].reg = 0x1284; rw[7].val = 0;
			p3d_reglist(cs, rw, 8);
		}
		if (st->comb && (pair || orig_multi)) {
			p3d_emit_comb_program_units(cs, st, nunit,
						    st->comb_slot_base ?
						    st->comb_slot_base : merges);
			return;
		}
		if (st->comb && !st->sampled_alpha_companion) {
			p3d_emit_comb_program(cs, st->comb,
					      st->comb_slot_base ?
					      st->comb_slot_base :
					      (nunit > 1 ? 2u : 1u));
			return;
		}
		if (pair || orig_multi) {
			/* one stage per unit in slots 0, 2, 4, 8 after the merge,
			 * then pads */
			static const uint32_t slot[] = { 0, 2, 4, 8, 10 };

			p3d_emit_texenv(cs, slot[merges], 0, 1, t, st->comb_op);
			for (u = 1; u < nunit; u++)
				p3d_emit_texenv(cs, slot[u + merges],
						p3d_state_unit_treg(st, nunit, u),
						0, p3d_state_unit(st, u),
						st->comb_op);
			for (u = nunit + merges; u < 5; u++)
				p3d_emit_comb_pad(cs, slot[u]);
			return;
		}
		p3d_emit_texenv(cs, nunit > 1 ? 4u : 2u, 0, 1, t, st->comb_op);

		if (nunit > 1) {
			if (st->sampled_alpha_companion) {
				/* Co-issued MOVs: rgb = r0.rgb, alpha = t1.r.  The
				 * alpha opcode's scalar-select value 3 replicates red;
				 * live P690 testing proved this feeds both alpha test
				 * and SRC_ALPHA blending. */
				rw[0].reg = P3D_R_COMB_RGB;
				rw[0].val = 0x00000088u;
				rw[1].reg = P3D_R_COMB_ALPHA;
				rw[1].val = 0x00000020u;
				rw[2].reg = 0x126c;
				rw[2].val = 0x01400400u;
				rw[3].reg = 0x1268;
				rw[3].val = 0x01400403u;
				p3d_reglist(cs, rw, 4);
			} else {
				p3d_emit_texenv(cs, 8, 1, 0, st->tex1,
						st->comb_op);
			}
			/* one pad to reach the five-instruction quantum */
			p3d_emit_comb_pad(cs, 10);
		} else {
			static const uint32_t pad[] = { 4, 8, 10 };
			size_t i;

			for (i = 0; i < 3; i++)
				p3d_emit_comb_pad(cs, pad[i]);
		}
	} else if (st->comb) {
		p3d_emit_comb_program(cs, st->comb, 0);
	} else {
		static const uint32_t pad[] = { 2, 4, 8, 10 };
		size_t i;

		/* MOV_sat r0, v0 in slot 0 with identity pads in 2/4/8/10 */
		rw[0].reg = P3D_R_COMB_RGB;   rw[0].val = 0x00000200u;
		rw[1].reg = P3D_R_COMB_ALPHA; rw[1].val = 0x00000200u;
		rw[2].reg = 0x126c;           rw[2].val = 0x01400400u;
		rw[3].reg = 0x1268;           rw[3].val = 0x01400400u;
		rw[4].reg = 0x1278; rw[4].val = 0;
		rw[5].reg = 0x127c; rw[5].val = 0;
		rw[6].reg = 0x1280; rw[6].val = 0;
		rw[7].reg = 0x1284; rw[7].val = 0;
		p3d_reglist(cs, rw, 8);

		for (i = 0; i < 4; i++)
			p3d_emit_comb_pad(cs, pad[i]);
	}
}

void
p3d_emit_draws(struct p3d_cs *cs, const struct p3d_state *st, unsigned nprim)
{
	struct p3d_rw rw[2];
	unsigned k;

	rw[0].reg = P3D_R_PRIMWORD; rw[0].val = p3d_primword(st->prim);
	p3d_reglist(cs, rw, 1);

	rw[0].reg = P3D_R_STREAM0; rw[0].val = 3;	/* class 3: PCI bus master */
	rw[1].reg = P3D_R_STRIDE0; rw[1].val = 4;
	p3d_reglist(cs, rw, 2);

	/* One packet per group: the caller decides how many primitives share a
	 * packet and the batch builder relocates one vertex range per packet.
	 * Overlapping primitives may share one - the P690 keeps the depth
	 * interlock and the rasterisation order inside a packet (see the
	 * primitive notes in parhelia3d.h). */
	for (k = 0; k < (nprim ? nprim : 1); k++)
		p3d_drawpacket(cs, st->dw_per_vertex);

	/* End of batch, then the NEXT fence.  Without this the 3D pipeline
	 * never completes and the following 2D fill draws nothing even though
	 * the bus-master status reports idle. */
	if (st->fence) {
		struct p3d_rw fl[4];
		uint32_t tag = st->fence + 8;

		rw[0].reg = P3D_R_EOBTAG; rw[0].val = 0x00070601u;
		p3d_reglist(cs, rw, 1);

		/* Drain the pixel pipeline.  DWGCTL 0x307 is a DWGSYNC opcode,
		 * and the vendor wraps the batch fence around it in ONE
		 * register list - [0x1300][0x12bc][0x12bc][0x1300] - so the
		 * fence waits for the sync rather than merely ordering behind
		 * the draw.  Without it the last 32x32 pixel block of the
		 * primitive stays in the pipeline and never reaches memory:
		 * geometry is exact, but ~1024 pixels at the end of the raster
		 * order are simply missing. */
		fl[0].reg = P3D_R_DWGCTL; fl[0].val = P3D_DWGCTL_SYNC;
		fl[1].reg = P3D_R_FENCE;  fl[1].val = tag | 1;
		fl[2].reg = P3D_R_FENCE;  fl[2].val = tag;
		fl[3].reg = P3D_R_DWGCTL; fl[3].val = p3d_dwgctl(st);
		p3d_reglist(cs, fl, 4);
	}
}

/* P690 mode 3 consumes ordinal ranges from a bound stream.  Live Z24 tests
 * establish that interlock occurs at the packet boundary, not between two
 * ranges carried by one packet, so keep exactly one triangle in every 0x8f.
 * Re-emit the complete direct-fetch snapshot and descriptors in the order
 * used by the passing isolated comparison. */
void
p3d_emit_bound_ranges(struct p3d_cs *cs, const struct p3d_state *st,
		      unsigned nprim)
{
	struct p3d_rw stream[12] = { };
	struct p3d_rw prim[1], bounds[2], reset[1], desc[6];
	size_t nd;
	unsigned i;

	for (i = 0; i < 8; i++)
		stream[i].reg = P3D_R_STREAM0 + i * 4;
	for (i = 0; i < 4; i++)
		stream[8 + i].reg = P3D_R_STRIDE0 + i * 4;
	stream[8].val = st->dw_per_vertex * sizeof(uint32_t);
	p3d_reglist(cs, ARRAY(stream));

	prim[0].reg = P3D_R_PRIMWORD;
	prim[0].val = p3d_primword(st->prim) & ~0x00040000u;
	p3d_reglist(cs, ARRAY(prim));

	bounds[0].reg = P3D_R_VTXCAP_HI;
	bounds[0].val = 0;
	bounds[1].reg = P3D_R_VTXCAP;
	bounds[1].val = nprim * 3;
	p3d_reglist(cs, ARRAY(bounds));

	reset[0].reg = P3D_R_DESC_INDEX;
	reset[0].val = 0;
	p3d_reglist(cs, ARRAY(reset));
	nd = p3d_descriptors(desc, !!st->tex, p3d_position_has_z(st),
			     st->pos_desc, st->col_desc,
			     st->tex && st->tex->env == P3D_TEXENV_REPLACE,
			     p3d_state_coord_sets(st),
			     !!st->vertex_alpha, !!st->texcoord3,
			     st->vertex_colour1);
	if (st->oow)
		p3d_descriptors_add_w(desc, nd);
	if (st->texcoord_q)
		p3d_descriptors_add_q(desc, nd);
	if (st->packed_colour && !st->pos_desc)
		p3d_descriptors_pack_colour(desc, nd,
					    (p3d_position_has_z(st) ? 3u : 2u) +
					    !!st->oow, !!st->vertex_alpha);
	p3d_reglist(cs, desc, nd);

	for (i = 0; i < nprim; i++)
		p3d_rangepacket(cs, i * 3, i * 3 + 3);
}

void
p3d_emit_draw(struct p3d_cs *cs, const struct p3d_state *st, unsigned index)
{
	(void)index;
	p3d_emit_draws(cs, st, 1);
}
