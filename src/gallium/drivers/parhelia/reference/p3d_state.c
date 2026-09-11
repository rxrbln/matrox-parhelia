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

/* DWGCTL for a 3D primitive: the captured base with the class nibble. */
uint32_t
p3d_dwgctl(const struct p3d_state *st)
{
	return (0x00a60ec4u & ~0xfu) | p3d_dwg_nibble[p3d_prim_class(st->prim)];
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

/* The vendor turns the blender off when src=ONE, dst=ZERO on both pipes, which
 * is a no-op; that is an optimisation, so it is reproduced here only in that it
 * gives byte-identity with the capture's disabled state. */
uint32_t
p3d_blendctl(const struct p3d_state *st)
{
	struct p3d_blend_factor s = st->blend ? st->blend_src : (struct p3d_blend_factor){ 1, 1 };
	struct p3d_blend_factor d = st->blend ? st->blend_dst : (struct p3d_blend_factor){ 0, 0 };
	uint32_t eq = (uint32_t)st->blend_eq & 7u;

	/* The blend-factor fields are ignored by MIN and MAX, but the hardware
	 * requires their canonical ONE encoding. */
	if (st->blend && (eq == P3D_BEQ_MIN || eq == P3D_BEQ_MAX))
		s = d = P3D_BF_ONE;

	return (st->blend ? 1u : 0u) |
	       ((uint32_t)(s.rgb & 0xf) << 1) | ((uint32_t)(d.rgb & 0xf) << 5) |
	       ((uint32_t)(s.a   & 0xf) << 9) | ((uint32_t)(d.a   & 0xf) << 13) |
	       (eq << 17) | (eq << 20) | P3D_BLEND_DITHER |
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
	uint32_t buf[256];
	unsigned m = st->const_mask ? st->const_mask : 7u;
	size_t n = 0, cap = sizeof buf / sizeof buf[0];

	if ((m & 1u) && st->mvp) {
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
		n = blockload_into(buf, n, cap, 8, vp, 2);
	if (m & 4u)
		n = blockload_into(buf, n, cap, 0x160, vp, 2);
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

uint32_t
p3d_tex_wrap(const struct p3d_texture *t)
{
	if (t->wrap)
		return t->wrap;
	/* LOD max 15 in [31:24] is the capture's resting value */
	return 0xf0000000u | ((uint32_t)t->wrap_s & 7u) |
	       (((uint32_t)t->wrap_t & 7u) << 3);
}

/* one byte per channel; a disabled channel is zero */
static uint32_t
p3d_colour_mask(const struct p3d_state *st)
{
	if (!st->mask_r && !st->mask_g && !st->mask_b && !st->mask_a)
		return 0xffffffffu;	/* nothing set = all channels on */
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

/* The attribute descriptors, by vertex layout.  Bit fields: [2:0] stream,
 * [11:4] dword offset, [19:16] attribute KIND, [25:22] component mask,
 * [31] last.  Every capture terminates the burst with a zero descriptor. */
static size_t
p3d_descriptors(struct p3d_rw *out, int textured, int with_z,
		uint32_t pos_override, uint32_t col_override, int no_colour,
		int two_tex, int vertex_alpha)
{
	size_t n = 0;

	if (pos_override) {
		out[n].reg = P3D_R_DESC; out[n].val = pos_override; n++;
		out[n].reg = P3D_R_DESC; out[n].val = col_override; n++;
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
	uint32_t rgb, alpha, rgbop, alphaop, c0rg = 0, c0ba = 0;

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
		c0rg = t->env_colour_rg;
		c0ba = t->env_colour_ba;
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
	rw[4].reg = 0x1278;           rw[4].val = c0rg;
	rw[5].reg = 0x127c;           rw[5].val = c0ba;
	rw[6].reg = 0x1280;           rw[6].val = 0;
	rw[7].reg = 0x1284;           rw[7].val = 0;
	p3d_reglist(cs, rw, 8);
}

void
p3d_emit_state(struct p3d_cs *cs, const struct p3d_state *st)
{
	uint32_t cls = p3d_prim_class(st->prim);
	uint32_t pipectl, dwgctl;
	struct p3d_rw rw[10], desc[6];
	size_t nd;

	/* 0x1130: set only the decoded fields and keep the captured remainder */
	pipectl = (0x0200200cu & ~0x03001001u) | (cls << 24) |
		  (st->flat ? 0x1000u : 0u) | ((uint32_t)st->cull & 3u);
	dwgctl  = p3d_dwgctl(st);

	/* -- raster / ROP ------------------------------------------------- */
	rw[0].reg = P3D_R_SCISSOR_MIN; rw[0].val = 0;
	rw[1].reg = P3D_R_SCISSOR_MAX; rw[1].val = (st->screen_h << 16) | st->screen_w;
	rw[2].reg = P3D_R_RASTERMODE;  rw[2].val = 0x0bb80024u;	/* measured */
	rw[3].reg = 0x12f8;            rw[3].val = 0x007fe000u;	/* AA threshold */
	p3d_reglist(cs, rw, 4);

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

	/* 0x12c8 [0] enables the depth compare and [3:1] selects it; 0x12d0
	 * bit 8 is the depth WRITE enable - the depthmask capture leaves that
	 * bit clear and changes nothing else. */
	rw[0].reg = P3D_R_ZFUNC;
	rw[0].val  = st->depth_raw ? st->depth_raw : 0x001000e0u |
		     ((uint32_t)(st->depth_test ? st->depth_func
					        : P3D_CMP_LESS) << 1) |
		     (st->depth_test ? 1u : 0u);
	rw[1].reg = 0x12cc; rw[1].val = 0x000000ffu;
	rw[2].reg = P3D_R_ZWRITE;
	rw[2].val  = 0x000000ffu | (st->depth_write ? 0x100u : 0u);
	rw[3].reg = 0x12c0; rw[3].val = 0x80000000u;
	rw[4].reg = 0x12c4; rw[4].val = 0x7fffffffu;
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
	if (st->transform) {
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

	nd = p3d_descriptors(desc, st->tex != NULL, st->has_depth,
			     st->pos_desc, st->col_desc,
			     st->tex && st->tex->env == P3D_TEXENV_REPLACE,
			     st->tex1 != NULL || st->second_texcoord_probe,
			     st->vertex_alpha);

	rw[0].reg = P3D_R_ATTR_PRESENT; rw[0].val = p3d_attr_present(desc, nd);
	rw[1].reg = P3D_R_ATTR_MAP0;    rw[1].val = 0x00287210u;
	rw[2].reg = P3D_R_ATTR_MAP1;    rw[2].val = 0x00006543u;
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
	p3d_bindpacket(cs, 0x20000000u | cls, rw, 3, 1);

	rw[0].reg = P3D_R_STROBE;    rw[0].val = 0;
	rw[1].reg = P3D_R_WARP_MODE; rw[1].val = 0x20000000u | cls;
	rw[2].reg = P3D_R_WARP_MODE; rw[2].val = 0x20000000u | cls;
	rw[3].reg = P3D_R_WARP_MODE; rw[3].val = cls;
	rw[4].reg = P3D_R_PIPECTL;   rw[4].val = pipectl;
	rw[5].reg = 0x111c;          rw[5].val = 0x00000fdbu;
	rw[6].reg = 0x1144;          rw[6].val = 0;
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
	rw[0].reg = P3D_R_GUARDBAND; rw[0].val = 0x42000000u;	/* 32.0f */
	p3d_reglist(cs, rw, 1);

	rw[0].reg = 0x121c;             rw[0].val = 0;
	rw[1].reg = P3D_R_TEX_ENABLE;   rw[1].val = 0x0001000fu;
	rw[2].reg = P3D_R_TEX_ENABLE+4; rw[2].val = 0;
	rw[3].reg = P3D_R_TEX_ENABLE+8; rw[3].val = 0;
	rw[4].reg = P3D_R_TEX_ENABLE+12;rw[4].val = 0;
	p3d_reglist(cs, rw, 5);

	/* -- texture ------------------------------------------------------- */
	if (st->tex) {
		const struct p3d_texture *t = st->tex;
		unsigned nunit = st->tex1 ? 2u : 1u, u;

		if (st->texture_cache_probe) {
			rw[0].reg = P3D_R_TEX_CACHE;
			rw[0].val = st->texture_cache_probe;
			p3d_reglist(cs, rw, 1);
		}
		rw[0].reg = 0x121c; rw[0].val = 0;
		p3d_reglist(cs, rw, 1);

		/* the capture packs the enable and the surface triple into one
		 * tag word, which makes "enable before surface" structural */
		/* 0x11c0..0x11e4 is ONE windowed register file shared by all four
		 * units; 0x1200 + unit*4 with the low nibble forced to 1<<unit
		 * selects which bank the following writes land in.  There is no
		 * 0x11c0 + unit*stride. */
		for (u = 0; u < nunit; u++) {
			const struct p3d_texture *tu = u ? st->tex1 : st->tex;
			/* The live draw assigns a complete texture-address pipe pair,
			 * not the one-hot context-prime values: unit 0 owns pipes 0+1
			 * and unit 1 owns pipes 2+3.  0x40101/02 only primes private
			 * banks during context creation and samples black in a draw.
			 */
			static const uint32_t pipe_mask[2] = { 0x3u, 0xcu };

			rw[0].reg = P3D_R_TEX_ENABLE + 4 * u;
			rw[0].val = 0x00040100u | pipe_mask[u];
			if (st->single_texture_pipe && u == 0)
				rw[0].val = 0x00040101u;
			else if (st->single_texture_pipe && u == 1)
				/* SUN90/P690 allocates units 0/1 to pipes 0/2. */
				rw[0].val = 0x00040104u;
			rw[1].reg = P3D_R_TEX_SURF_BASE; rw[1].val = tu->base;
			rw[2].reg = P3D_R_TEX_SURF_W;
			rw[2].val = (tu->width & 0x3fff) |
				    ((tu->texelcode & 7) << 16) | (tu->fmt << 24);
			rw[3].reg = P3D_R_TEX_SURF_H;
			rw[3].val = (tu->height & 0x3fff) | (1u << 16) |
				    ((tu->max_level & 0xfu) << 28);
			p3d_reglist(cs, rw, 4);

			rw[0].reg = P3D_R_TEX_FILTER; rw[0].val = p3d_tex_filter(tu);
			rw[1].reg = P3D_R_TEX_WRAP;   rw[1].val = p3d_tex_wrap(tu);
			rw[2].reg = 0x11d4;           rw[2].val = 0;
			rw[3].reg = 0x11d8;           rw[3].val = 0;
			p3d_reglist(cs, rw, 4);

			rw[0].reg = 0x11cc; rw[0].val = 0xffffffffu;
			rw[1].reg = 0x11d0; rw[1].val = 0;
			rw[2].reg = 0x11c8; rw[2].val = 0;
			p3d_reglist(cs, rw, 3);
		}

		if (nunit == 1) {
			rw[0].reg = P3D_R_TEX_ENABLE + 4;
			/* The normal P690 one-pipe path leaves unit 1 clear.  The
			 * vendor capture instead retains a disabled unit-1 descriptor
			 * claiming pipes 2+3.  Keep that state an explicit diagnostic
			 * variant: it must never widen a production packet without a
			 * texture-alpha result. */
			rw[0].val = st->single_texture_pipe == 2 ? 0x0001000cu :
				    st->single_texture_pipe ? 0 : 0x0001000cu;
			rw[1].reg = P3D_R_TEX_ENABLE + 8;  rw[1].val = 0;
			rw[2].reg = P3D_R_TEX_ENABLE + 12; rw[2].val = 0;
			p3d_reglist(cs, rw, 3);
		} else {
			rw[0].reg = P3D_R_TEX_ENABLE + 8;  rw[0].val = 0;
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

		/* MODULATE: the texture-address stage takes instruction 0, so
		 * the ALU is instruction 1 = hardware slot 2 */
		rw[0].reg = P3D_R_COMB_RGB;   rw[0].val = 0x00004000u;
		rw[1].reg = P3D_R_COMB_ALPHA; rw[1].val = 0x00004000u;
		rw[2].reg = 0x126c;           rw[2].val = 0x00000c00u;
		rw[3].reg = 0x1268;           rw[3].val = 0x00000c00u;
		rw[4].reg = 0x1278; rw[4].val = 0;
		rw[5].reg = 0x127c; rw[5].val = 0;
		rw[6].reg = 0x1280; rw[6].val = 0;
		rw[7].reg = 0x1284; rw[7].val = 0;
		p3d_reglist(cs, rw, 8);

		/* MODULATE = MUL(TEX, PREV).  REPLACE drops the alpha slot and
		 * leaves 0x1268 at its resting value, exactly as the capture's
		 * texrepl differs from tex. */
		if (nunit > 1) {
			/* slot 2: the second unit's texture-address stage.  On
			 * PARD's P690 two-unit route, logical unit 1 occupies physical
			 * pipe 2 (0x1204 = 0x40104), and live hardware proved that the
			 * TA Areg field must select that physical pipe as well. */
			uint32_t ta_source = st->single_texture_pipe ? 2u : 0u;
			rw[0].reg = P3D_R_COMB_RGB;
			rw[0].val = 0x00004002u | (ta_source << 5);
			rw[1].reg = P3D_R_COMB_ALPHA;
			rw[1].val = 0x00004000u | (ta_source << 5);
			rw[2].reg = 0x126c;           rw[2].val = 0x00100c00u;
			rw[3].reg = 0x1268;           rw[3].val = 0x00100c00u;
			rw[4].reg = 0x1278; rw[4].val = 0;
			rw[5].reg = 0x127c; rw[5].val = 0;
			rw[6].reg = 0x1280; rw[6].val = 0;
			rw[7].reg = 0x1284; rw[7].val = 0;
			p3d_reglist(cs, rw, 8);
		}
		p3d_emit_texenv(cs, nunit > 1 ? 4u : 2u, 0, 1, t,
				st->comb_op);

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
			rw[0].reg = P3D_R_COMB_RGB; rw[0].val = 0x8a;
			rw[1].reg = 0x126c;         rw[1].val = 0x00400400u;
			p3d_reglist(cs, rw, 2);
		} else {
			static const uint32_t pad[] = { 0x84, 0x88, 0x8a };
			size_t i;
			for (i = 0; i < 3; i++) {
				rw[0].reg = P3D_R_COMB_RGB; rw[0].val = pad[i];
				rw[1].reg = 0x126c;         rw[1].val = 0x00400400u;
				p3d_reglist(cs, rw, 2);
			}
		}
	} else {
		static const uint32_t pad[] = { 0x82, 0x84, 0x88, 0x8a };
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

		for (i = 0; i < 4; i++) {
			rw[0].reg = P3D_R_COMB_RGB; rw[0].val = pad[i];
			rw[1].reg = 0x126c;         rw[1].val = 0x00400400u;
			p3d_reglist(cs, rw, 2);
		}
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

	/* One draw packet per primitive by default, which is also what every
	 * one of the 1235 draw packets in the vendor capture does.  Batching
	 * several primitives into one packet is legal and much faster, but only
	 * for triangles that do not OVERLAP each other on screen: within a
	 * single packet there is no depth interlock between them, so a hidden
	 * back face can win against the front face it sits behind - measured at
	 * 11759 wrong pixels on one gear, falling to 29 when the same triangles
	 * were split.  The caller decides, by how it groups nprim. */
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
	nd = p3d_descriptors(desc, !!st->tex, !!st->has_depth,
			     st->pos_desc, st->col_desc,
			     st->tex && st->tex->env == P3D_TEXENV_REPLACE,
			     !!st->tex1,
			     !!st->vertex_alpha);
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
