/*
 * WARP vertex-program encoder and fixed-shape program builder.
 *
 * The 96-bit bundle is a primary operation writing destination port B plus an
 * optional secondary operation that reads src2 and writes port A.  The field
 * assignment and this encoder's steps are the vendor D3D assembler's, and
 * every instruction the kernel runs on hardware re-encodes bit-exactly from
 * this code (comb of docs/parhelia-p690-warp-isa-re.md section 2 and
 * tools/parhelia-warp-roundtrip.py).
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include "parhelia3d.h"

/* Every instruction starts from the "wra" template: primary opcode 0x60 with
 * an empty write mask, both file codes 2, identity swizzles. */
static const uint32_t p3d_warp_template[3] = {
	0x00080200u, 0x00440000u, 0xb01a21a2u,
};

static void
p3d_warp_swz_shared(uint32_t *w, uint16_t swizzle, unsigned owner_is_src2)
{
	if (swizzle != P3D_WARP_SWZ_IDENTITY) {
		w[1] = (w[1] & 0x3fffffffu) | ((uint32_t)(swizzle & 3u) << 30);
		w[2] = (w[2] & (owner_is_src2 ? 0xfffffc00u : 0xffbffc00u)) |
		       (uint32_t)(swizzle >> 2) |
		       (owner_is_src2 ? 0x400000u : 0u);
	} else if (((w[2] >> 22) & 1u) == owner_is_src2) {
		/* Release the shared field back to its identity encoding. */
		w[1] &= 0x3fffffffu;
		w[2] = (w[2] & 0xfffffda2u) | 0x1a2u;
	}
}

/* One operand index above 15 per instruction, through exthi/extsel. */
static void
p3d_warp_extend(uint32_t *w, unsigned index, unsigned operand)
{
	if (index < 16)
		return;
	w[1] = (w[1] & 0xfff03fffu) | ((uint32_t)(index & 0x70u) << 10) |
	       ((uint32_t)operand << 17);
}

void
p3d_warp_encode(const struct p3d_warp_inst *in, uint32_t out[3])
{
	uint32_t w[3] = { p3d_warp_template[0], p3d_warp_template[1],
			  p3d_warp_template[2] };

	w[2] = (w[2] & ~0x7f800000u) | ((uint32_t)(in->op & 0xffu) << 23);
	if (in->dst_mask)
		w[0] = (w[0] & 0xfff003ffu) |
		       ((uint32_t)(in->dst_file & 3u) << 18) |
		       ((uint32_t)(in->dst_index & 0xfu) << 14) |
		       ((uint32_t)(in->dst_mask & 0xfu) << 10);
	if (in->sop) {
		w[1] = (w[1] & ~0x1e00u) | ((uint32_t)(in->sop & 0xfu) << 9);
		w[0] = (w[0] & 0xfffffc00u) |
		       ((uint32_t)(in->sdst_file & 3u) << 8) |
		       ((uint32_t)(in->sdst_index & 0xfu) << 4) |
		       (uint32_t)(in->sdst_mask & 0xfu);
	}

	for (unsigned i = 0; i < in->nsrc && i < 3; i++) {
		const struct p3d_warp_src *s = &in->src[i];

		switch (i) {
		case 0:
			w[1] = (w[1] & 0xfffffe03u) |
			       ((uint32_t)(s->file & 3u) << 7) |
			       ((uint32_t)(s->neg ? 1u : 0u) << 6) |
			       ((uint32_t)(s->index & 0xfu) << 2);
			w[2] = (w[2] & ~0x3ffc00u) |
			       ((uint32_t)(s->swizzle & 0xfffu) << 10);
			p3d_warp_extend(w, s->index, 0);
			break;
		case 1:
			w[1] = (w[1] & ~3u) | (uint32_t)(s->file & 3u);
			w[0] = (w[0] & 0x07ffffffu) |
			       ((uint32_t)(s->neg ? 1u : 0u) << 31) |
			       ((uint32_t)(s->index & 0xfu) << 27);
			p3d_warp_swz_shared(w, s->swizzle, 0);
			p3d_warp_extend(w, s->index, 1);
			break;
		default:
			w[0] = (w[0] & 0xf80fffffu) |
			       ((uint32_t)(s->file & 3u) << 25) |
			       ((uint32_t)(s->neg ? 1u : 0u) << 24) |
			       ((uint32_t)(s->index & 0xfu) << 20);
			p3d_warp_swz_shared(w, s->swizzle, 1);
			p3d_warp_extend(w, s->index, 2);
			break;
		}
	}
	/* The secondary operation reads src2, so a bundle that has one and no
	 * third primary source still has to encode that operand. */
	if (in->sop && in->nsrc < 3) {
		const struct p3d_warp_src *s = &in->ssrc;

		w[0] = (w[0] & 0xf80fffffu) |
		       ((uint32_t)(s->file & 3u) << 25) |
		       ((uint32_t)(s->neg ? 1u : 0u) << 24) |
		       ((uint32_t)(s->index & 0xfu) << 20);
		p3d_warp_swz_shared(w, s->swizzle, 1);
		p3d_warp_extend(w, s->index, 2);
	}
	out[0] = w[0];
	out[1] = w[1];
	out[2] = w[2];
}

/* ---- program builder --------------------------------------------------- */

struct p3d_warp_emit {
	uint32_t *out;
	unsigned count, max;
};

static void
emit(struct p3d_warp_emit *e, const struct p3d_warp_inst *in)
{
	if (e->count >= e->max) {
		e->count = e->max + 1;	/* mark overflow */
		return;
	}
	p3d_warp_encode(in, e->out + e->count * 3);
	e->count++;
}

#define SRC(f, i) { .file = (f), .index = (i), .swizzle = P3D_WARP_SWZ_IDENTITY }
#define SRC_SWZ(f, i, s) { .file = (f), .index = (i), .swizzle = (s) }

/* "mov dst, src" through the secondary port; the primary half stays a no-op. */
static void
emit_mov(struct p3d_warp_emit *e, unsigned file, unsigned index,
	 const struct p3d_warp_src *src)
{
	struct p3d_warp_inst in = {
		.op = P3D_WARP_WRA, .sop = P3D_WARP_SOP_MOV,
		.sdst_file = file, .sdst_index = index, .sdst_mask = 0xf,
		.ssrc = *src,
	};

	emit(e, &in);
}

/* "add dst, a, b" with an optional co-issued secondary move. */
static void
emit_add(struct p3d_warp_emit *e, unsigned op, unsigned file, unsigned index,
	 unsigned mask, const struct p3d_warp_src *a,
	 const struct p3d_warp_src *b, unsigned sop, unsigned sfile,
	 unsigned sindex, const struct p3d_warp_src *ssrc)
{
	struct p3d_warp_inst in = {
		.op = op, .dst_file = file, .dst_index = index,
		.dst_mask = mask, .nsrc = 2, .src = { *a, *b },
		.sop = sop, .sdst_file = sfile, .sdst_index = sindex,
		.sdst_mask = sop ? 0xf : 0,
	};

	if (sop)
		in.ssrc = *ssrc;
	emit(e, &in);
}

unsigned
p3d_warp_build(const struct p3d_warp_shape *shape, uint32_t *out, unsigned max)
{
	static const uint32_t end[3] = { 0x00000200u, 0x00040000u, 0x14c00000u };
	static const uint32_t tail[3] = { 0x00080200u, 0x00440000u, 0x301a21a2u };
	struct p3d_warp_emit e = { .out = out, .max = max };
	const struct p3d_warp_src position = SRC(P3D_WARP_FILE_V, 0);
	const struct p3d_warp_src colour = SRC(P3D_WARP_FILE_V, P3D_WARP_V_COLOUR);
	const struct p3d_warp_src zero =
		SRC_SWZ(P3D_WARP_FILE_R, 1, P3D_WARP_SWZ_ZERO);
	const struct p3d_warp_src one =
		SRC_SWZ(P3D_WARP_FILE_R, 1, P3D_WARP_SWZ_ONE);
	const struct p3d_warp_src position_zero =
		SRC_SWZ(P3D_WARP_FILE_V, 0, P3D_WARP_SWZ_ZERO);
	const struct p3d_warp_src latch_t2 = SRC(P3D_WARP_FILE_T, 2);
	unsigned i;

	if (shape->transform) {
		const struct p3d_warp_src latch_t0 = SRC(P3D_WARP_FILE_T, 0);
		const struct p3d_warp_src latch_t1 = SRC(P3D_WARP_FILE_T, 1);
		const struct p3d_warp_src clip = SRC(P3D_WARP_FILE_R, 2);
		const struct p3d_warp_src clip_xyz1 =
			SRC_SWZ(P3D_WARP_FILE_R, 2, P3D_WARP_SWZ_XYZ1);
		const struct p3d_warp_src ndc = SRC(P3D_WARP_FILE_R, 5);
		const struct p3d_warp_src latch_t1_xyz0 =
			SRC_SWZ(P3D_WARP_FILE_T, 1, P3D_WARP_SWZ_XYZ0);
		struct p3d_warp_src scale =
			SRC_SWZ(P3D_WARP_FILE_C, shape->viewport_scale_slot,
				P3D_WARP_SWZ_XYZ1);
		struct p3d_warp_src offset =
			SRC(P3D_WARP_FILE_C, shape->viewport_offset_slot);

		/* Latch the fetched position, then one dp4 per clip component,
		 * highest first as the reference program does. */
		emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_T, 0, 0xf,
			 &position, &position_zero, 0, 0, 0, NULL);
		for (i = 0; i < 4; i++) {
			struct p3d_warp_src row =
				SRC(P3D_WARP_FILE_C, shape->matrix_slot + 3 - i);
			struct p3d_warp_inst in = {
				.op = P3D_WARP_DP4,
				.dst_file = P3D_WARP_FILE_DST_R, .dst_index = 2,
				.dst_mask = 8u >> i,
				.nsrc = 2, .src = { latch_t0, row },
			};

			emit(&e, &in);
		}
		/* Load the viewport offset into T1 while the colour is copied. */
		emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 1, 0xf,
			 &colour, &zero, P3D_WARP_SOP_MOV,
			 P3D_WARP_FILE_DST_T, 1, &offset);
		if (shape->light) {
			struct p3d_warp_src light =
				SRC(P3D_WARP_FILE_C, shape->light_slot);
			struct p3d_warp_src material =
				SRC(P3D_WARP_FILE_C, shape->material_slot);
			struct p3d_warp_src ambient =
				SRC(P3D_WARP_FILE_C, shape->ambient_slot);
			struct p3d_warp_src diffuse = SRC(P3D_WARP_FILE_R, 3);
			struct p3d_warp_inst dp3 = {
				.op = P3D_WARP_DP3,
				.dst_file = P3D_WARP_FILE_DST_R, .dst_index = 3,
				.dst_mask = 0xf, .nsrc = 2,
				.src = { colour, light },
			};
			struct p3d_warp_inst mad_diffuse = {
				.op = P3D_WARP_MAD,
				.dst_file = P3D_WARP_FILE_DST_T, .dst_index = 2,
				.dst_mask = 0xf, .nsrc = 3,
				.src = { material, diffuse, zero },
			};
			struct p3d_warp_inst mad_ambient = {
				.op = P3D_WARP_MAD,
				.dst_file = P3D_WARP_FILE_DST_T, .dst_index = 2,
				.dst_mask = 0xf, .nsrc = 3,
				.src = { ambient, one, latch_t2 },
			};

			emit(&e, &dp3);
			emit_add(&e, P3D_WARP_MAX, P3D_WARP_FILE_DST_R, 3, 0xf,
				 &diffuse, &zero, 0, 0, 0, NULL);
			emit(&e, &mad_diffuse);
			emit(&e, &mad_ambient);
			emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 1, 0xf,
				 &latch_t2, &zero, 0, 0, 0, NULL);
		}
		/* Perspective divide, then the viewport scale and offset. */
		emit_add(&e, P3D_WARP_SMUL, P3D_WARP_FILE_DST_R, 5, 0xf,
			 &clip, &clip_xyz1,
			 shape->clip_copy ? P3D_WARP_SOP_MOV : 0,
			 P3D_WARP_FILE_DST_O, 11, &clip);
		{
			struct p3d_warp_inst viewport = {
				.op = P3D_WARP_MAD,
				.dst_file = P3D_WARP_FILE_DST_T, .dst_index = 1,
				.dst_mask = 7, .nsrc = 3,
				.src = { scale, ndc, latch_t1_xyz0 },
			};

			emit(&e, &viewport);
		}
		emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 0, 0xf,
			 &latch_t1, &zero, 0, 0, 0, NULL);
	} else {
		emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 0, 0xf,
			 &position, &position_zero, 0, 0, 0, NULL);
		if (shape->write_t2) {
			/* Priming the WARP temporary needs its own bundle, so
			 * the clip copy rides on it and the colour follows. */
			emit_add(&e, P3D_WARP_MUL, P3D_WARP_FILE_DST_T, 2, 0xf,
				 &colour, &zero,
				 shape->clip_copy ? P3D_WARP_SOP_MOV : 0,
				 P3D_WARP_FILE_DST_O, 11, &position);
			if (shape->colour_white)
				emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O,
					 1, 0xf, &latch_t2, &one, 0, 0, 0, NULL);
			else
				emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O,
					 1, 0xf, &colour, &zero, 0, 0, 0, NULL);
		} else {
			emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 1, 0xf,
				 &colour, &zero,
				 shape->clip_copy ? P3D_WARP_SOP_MOV : 0,
				 P3D_WARP_FILE_DST_O, 11, &position);
		}
	}

	for (i = 0; i < shape->texcoords; i++) {
		struct p3d_warp_src set =
			SRC(P3D_WARP_FILE_V, P3D_WARP_V_TEXCOORD0 + i);

		emit_mov(&e, P3D_WARP_FILE_DST_O, 3 + i, &set);
	}

	if (e.count + 2 > e.max)
		return 0;
	out[e.count * 3 + 0] = end[0];
	out[e.count * 3 + 1] = end[1];
	out[e.count * 3 + 2] = end[2];
	e.count++;
	out[e.count * 3 + 0] = tail[0];
	out[e.count * 3 + 1] = tail[1];
	out[e.count * 3 + 2] = tail[2];
	e.count++;
	return e.count;
}
