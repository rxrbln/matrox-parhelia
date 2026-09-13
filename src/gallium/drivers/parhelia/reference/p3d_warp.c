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
		if (shape->bones >= 2) {
			/* Blend the position through the bone matrices first;
			 * the weight is the vertex colour's x and the second
			 * bone takes the rest. */
			const struct p3d_warp_src weight =
				SRC_SWZ(P3D_WARP_FILE_V, P3D_WARP_V_COLOUR,
					P3D_WARP_SWZ(0, 0, 0, 0));
			struct p3d_warp_src blended[2] = {
				SRC(P3D_WARP_FILE_R, 6),
				SRC(P3D_WARP_FILE_R, 7),
			};
			unsigned b, j;

			for (b = 0; b < 2; b++)
				for (j = 0; j < 4; j++) {
					struct p3d_warp_src row =
						SRC(P3D_WARP_FILE_C,
						    shape->bone_slot +
						    4 * b + j);
					struct p3d_warp_inst dp4 = {
						.op = P3D_WARP_DP4,
						.dst_file =
							P3D_WARP_FILE_DST_R,
						.dst_index = 6 + b,
						.dst_mask = 1u << j,
						.nsrc = 2,
						.src = { position, row },
					};

					emit(&e, &dp4);
				}
			/* T0 = w * bone0 + (1 - w) * bone1, as one lerp. */
			{
				struct p3d_warp_inst lerp = {
					.op = P3D_WARP_MUL,
					.dst_file = P3D_WARP_FILE_DST_R,
					.dst_index = 8, .dst_mask = 0xf,
					.nsrc = 2,
					.src = { blended[0], weight },
				};
				struct p3d_warp_src one_minus = weight;
				struct p3d_warp_inst mad = {
					.op = P3D_WARP_MAD,
					.dst_file = P3D_WARP_FILE_DST_T,
					.dst_index = 0, .dst_mask = 0xf,
					.nsrc = 3,
				};

				emit(&e, &lerp);
				one_minus.neg = 1;
				emit_add(&e, P3D_WARP_ADD,
					 P3D_WARP_FILE_DST_R, 9, 0xf,
					 &one, &one_minus, 0, 0, 0, NULL);
				{
					struct p3d_warp_src rest =
						SRC(P3D_WARP_FILE_R, 9);
					struct p3d_warp_src partial =
						SRC(P3D_WARP_FILE_R, 8);

					mad.src[0] = blended[1];
					mad.src[1] = rest;
					mad.src[2] = partial;
				}
				emit(&e, &mad);
			}
		} else {
			emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_T, 0, 0xf,
				 &position, &position_zero, 0, 0, 0, NULL);
		}
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
		/* User clip planes, while r2 holds the clip-space position and
		 * before the perspective divide reuses r5 for the NDC. */
		for (i = 0; i < shape->clip_planes && i < 6; i++) {
			struct p3d_warp_src plane =
				SRC(P3D_WARP_FILE_C, shape->clip_slot + i);
			struct p3d_warp_inst test = {
				.op = i ? P3D_WARP_CLIP_NEXT :
					  P3D_WARP_CLIP_FIRST,
				.dst_file = P3D_WARP_FILE_DST_R,
				.dst_index = 5, .dst_mask = 0xf,
				.nsrc = 2, .src = { clip, plane },
			};

			emit(&e, &test);
		}
		if (shape->clip_planes) {
			struct p3d_warp_src code = SRC(P3D_WARP_FILE_R, 5);
			struct p3d_warp_inst publish = {
				.op = P3D_WARP_CLIP_EMIT,
				.dst_file = P3D_WARP_FILE_DST_O,
				.dst_index = 10, .dst_mask = 0x1,
				.nsrc = 1, .src = { code },
			};

			emit(&e, &publish);
		}

		/* Load the viewport offset into T1 while the colour is copied. */
		emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 1, 0xf,
			 &colour, &zero, P3D_WARP_SOP_MOV,
			 P3D_WARP_FILE_DST_T, 1, &offset);
		if (shape->light) {

			struct p3d_warp_src ambient =
				SRC(P3D_WARP_FILE_C, shape->ambient_slot);
			struct p3d_warp_src diffuse =
				shape->masked_dp3 ?
				(struct p3d_warp_src)SRC_SWZ(P3D_WARP_FILE_R, 3,
						P3D_WARP_SWZ(1, 1, 1, 1)) :
				(struct p3d_warp_src)SRC(P3D_WARP_FILE_R, 3);
			const struct p3d_warp_src sum = SRC(P3D_WARP_FILE_R, 4);
			const struct p3d_warp_src term = SRC(P3D_WARP_FILE_R, 5);
			/* r3 holds N.L in y; the accumulator wants it as the
			 * second of four factors with the rest zeroed. */
			const struct p3d_warp_src factors =
				SRC_SWZ(P3D_WARP_FILE_R, 3,
					P3D_WARP_SWZ(4, 1, 4, 4));
			const struct p3d_warp_src accum = SRC(P3D_WARP_FILE_R, 10);
			const unsigned nl = shape->lights ? shape->lights : 1;
			const int split_mad = shape->split_mad;
			struct p3d_warp_inst mad_ambient = {
				.op = P3D_WARP_MAD,
				.dst_file = P3D_WARP_FILE_DST_T, .dst_index = 2,
				.dst_mask = 0xf, .nsrc = 3,
				.src = { ambient, one,
					 nl > 1 ? sum : latch_t2 },
				/* filled below when the MAD is split */
			};
			unsigned k, face;
			const unsigned nface = shape->two_sided ? 2u : 1u;

			for (face = 0; face < nface; face++) {
			/* One light per pair of constants.  A second light
			 * accumulated straight into T2 came back multiplied by
			 * about 1.66 while the term already in T2 passed
			 * through exactly, so the running sum lives in r4 and
			 * T2 is written once, at the end, as the ambient MAD.
			 * One light keeps the kernel program byte for byte. */
			for (k = 0; k < nl; k++) {
				const unsigned k_light = k;
				const struct p3d_warp_src to_light =
					SRC(P3D_WARP_FILE_R, 6);
				const struct p3d_warp_src d2 =
					SRC_SWZ(P3D_WARP_FILE_R, 7,
						P3D_WARP_SWZ(0, 0, 0, 0));
				const struct p3d_warp_src inv_d =
					SRC(P3D_WARP_FILE_R, 8);
				const struct p3d_warp_src poly =
					SRC(P3D_WARP_FILE_R, 9);
				const struct p3d_warp_src poly_w =
					SRC_SWZ(P3D_WARP_FILE_R, 9,
						P3D_WARP_SWZ(3, 3, 3, 3));
				const struct p3d_warp_src kx =
					SRC_SWZ(P3D_WARP_FILE_C,
						shape->atten_slot + k,
						P3D_WARP_SWZ(0, 0, 0, 0));
				const struct p3d_warp_src ky =
					SRC_SWZ(P3D_WARP_FILE_C,
						shape->atten_slot + k,
						P3D_WARP_SWZ(1, 1, 1, 1));
				const struct p3d_warp_src kz =
					SRC_SWZ(P3D_WARP_FILE_C,
						shape->atten_slot + k,
						P3D_WARP_SWZ(2, 2, 2, 2));
				struct p3d_warp_src light =
					SRC(P3D_WARP_FILE_C,
					    shape->light_slot + 2 * k);
				struct p3d_warp_src material =
					SRC(P3D_WARP_FILE_C,
					    (face ? shape->back_material_slot :
						    shape->material_slot) +
					    2 * k);
				/* GL reverses the normal for the back face. */
				struct p3d_warp_src normal = colour;
				struct p3d_warp_inst dp3 = {
					.op = P3D_WARP_DP3,
					.dst_file = P3D_WARP_FILE_DST_R,
					.dst_index = 3,
					.dst_mask = shape->masked_dp3 ? 0x2 : 0xf,
					.nsrc = 2,
					.src = { normal, light },
				};

				normal.neg = face ? 1 : 0;
				dp3.src[0] = normal;

				if (shape->attenuation) {
					/* The light constant is a position, so
					 * the direction is it minus the vertex
					 * and the distance comes out of the
					 * same dot product. */
					const struct p3d_warp_src d2w =
						SRC_SWZ(P3D_WARP_FILE_R, 7,
							P3D_WARP_SWZ(3, 3, 3, 3));
					struct p3d_warp_src neg_pos = position;
					struct p3d_warp_inst dot = {
						.op = P3D_WARP_DP3,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 7, .dst_mask = 0xf,
						.nsrc = 2,
						.src = { to_light, to_light },
					};
					struct p3d_warp_inst rsq = {
						.op = P3D_WARP_RSQ,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 8, .dst_mask = 0xf,
						.nsrc = 2,
						.src = { d2w, one },
					};

					neg_pos.neg = 1;
					/* to_light = lightpos - position */
					emit_add(&e, P3D_WARP_ADD,
						 P3D_WARP_FILE_DST_R, 6, 0xf,
						 &light, &neg_pos, 0, 0, 0,
						 NULL);
					emit(&e, &dot);		/* r7 = d*d */
					emit(&e, &rsq);		/* r8 = 1/d */
					/* The unit direction, and N.L from it. */
					emit_add(&e, P3D_WARP_MUL,
						 P3D_WARP_FILE_DST_R, 6, 0xf,
						 &to_light, &inv_d, 0, 0, 0,
						 NULL);
					dp3.src[1] = to_light;
				}
				struct p3d_warp_inst mad_diffuse = {
					.op = P3D_WARP_MAD,
					.dst_file = nl > 1 ?
						    P3D_WARP_FILE_DST_R :
						    P3D_WARP_FILE_DST_T,
					.dst_index = nl > 1 ? 4 : 2,
					.dst_mask = 0xf, .nsrc = 3,
					.src = { material, diffuse,
						 k ? sum : zero },
				};

				emit(&e, &dp3);
				emit_add(&e, P3D_WARP_MAX,
					 P3D_WARP_FILE_DST_R, 3,
					 shape->masked_dp3 || shape->accumulator ?
					 0x2 : 0xf,
					 &diffuse, &zero, 0, 0, 0, NULL);
				if (shape->attenuation) {
					struct p3d_warp_inst mad_lin = {
						.op = P3D_WARP_MAD,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 9, .dst_mask = 0xf,
						.nsrc = 3,
						.src = { ky, poly, kx },
					};
					struct p3d_warp_inst mad_quad = {
						.op = P3D_WARP_MAD,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 9, .dst_mask = 0xf,
						.nsrc = 3,
						.src = { kz, d2, poly },
					};
					struct p3d_warp_inst recip = {
						.op = P3D_WARP_SMUL,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 9, .dst_mask = 0xf,
						.nsrc = 2,
						.src = { poly_w, one },
					};

					/* d = d*d * (1/d), then the polynomial
					 * and its reciprocal, then scale the
					 * clamped N.L by it. */
					emit_add(&e, P3D_WARP_MUL,
						 P3D_WARP_FILE_DST_R, 9, 0xf,
						 &d2, &inv_d, 0, 0, 0, NULL);
					emit(&e, &mad_lin);
					emit(&e, &mad_quad);
					emit(&e, &recip);
					emit_add(&e, P3D_WARP_MUL,
						 P3D_WARP_FILE_DST_R, 3,
						 shape->masked_dp3 ? 0x2 : 0xf,
						 &diffuse, &poly, 0, 0, 0,
						 NULL);
				}
				if (shape->attenuation && shape->spot) {
					/* The cone test.  spot = dot(-L, axis)
					 * against cos(cutoff) in .w, turned
					 * into a 0/1 gate the same way the
					 * shader compiler lowers a comparison:
					 * scale the difference enormously,
					 * clamp at zero, then s/(s+1). */
					const struct p3d_warp_src axis =
						SRC(P3D_WARP_FILE_C,
						    shape->spot_slot + k);
					const struct p3d_warp_src cos_cut =
						SRC_SWZ(P3D_WARP_FILE_C,
							shape->spot_slot + k,
							P3D_WARP_SWZ(3, 3, 3, 3));
					const struct p3d_warp_src gate =
						SRC(P3D_WARP_FILE_R, 10);
					/* The spot slot is the axis in xyz and
					 * the cutoff cosine in w, so the gate's
					 * scale rides in the attenuation slot's
					 * unused w. */
					const struct p3d_warp_src huge =
						SRC_SWZ(P3D_WARP_FILE_C,
							shape->atten_slot + k,
							P3D_WARP_SWZ(3, 3, 3, 3));
					struct p3d_warp_src to_light_neg =
						to_light;
					struct p3d_warp_src cos_cut_neg = cos_cut;
					struct p3d_warp_src gate11 =
						SRC(P3D_WARP_FILE_R, 11);
					struct p3d_warp_inst spot_dot = {
						.op = P3D_WARP_DP3,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 10, .dst_mask = 0xf,
						.nsrc = 2,
					};
					struct p3d_warp_inst recip_gate = {
						.op = P3D_WARP_SMUL,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 11, .dst_mask = 0xf,
						.nsrc = 2,
					};

					to_light_neg.neg = 1;
					cos_cut_neg.neg = 1;
					spot_dot.src[0] = to_light_neg;
					spot_dot.src[1] = axis;
					emit(&e, &spot_dot);
					/* r10 = dot - cos(cutoff), clamped */
					emit_add(&e, P3D_WARP_ADD,
						 P3D_WARP_FILE_DST_R, 10, 0xf,
						 &gate, &cos_cut_neg,
						 0, 0, 0, NULL);
					emit_add(&e, P3D_WARP_MAX,
						 P3D_WARP_FILE_DST_R, 10, 0xf,
						 &gate, &zero, 0, 0, 0, NULL);
					emit_add(&e, P3D_WARP_MUL,
						 P3D_WARP_FILE_DST_R, 10, 0xf,
						 &gate, &huge, 0, 0, 0, NULL);
					/* r11 = s / (s + 1), which is 0 or 1 */
					emit_add(&e, P3D_WARP_ADD,
						 P3D_WARP_FILE_DST_R, 11, 0xf,
						 &gate, &one, 0, 0, 0, NULL);
					{
						struct p3d_warp_src sum_w =
							SRC_SWZ(P3D_WARP_FILE_R, 11,
								P3D_WARP_SWZ(3, 3, 3, 3));

						recip_gate.src[0] = sum_w;
						recip_gate.src[1] = gate;
					}
					emit(&e, &recip_gate);
					emit_add(&e, P3D_WARP_MUL,
						 P3D_WARP_FILE_DST_R, 3,
						 shape->masked_dp3 ? 0x2 : 0xf,
						 &diffuse, &gate11, 0, 0, 0,
						 NULL);
				}
				if (shape->specular) {
					/* N.H, clamped, raised by squaring,
					 * then added in with its own colour. */
					const struct p3d_warp_src half =
						SRC(P3D_WARP_FILE_C,
						    shape->specular_slot + 2 * k);
					const struct p3d_warp_src spec_col =
						SRC(P3D_WARP_FILE_C,
						    shape->specular_slot + 2 * k + 1);
					/* A full-mask dp3 does not broadcast the
					 * scalar - the vendor only ever writes
					 * one component - so write y and read
					 * it replicated.  Every earlier test
					 * here used N.L = 1, where the
					 * difference is invisible. */
					const struct p3d_warp_src sh =
						SRC_SWZ(P3D_WARP_FILE_R, 14,
							P3D_WARP_SWZ(1, 1, 1, 1));
					/* src1 encodes through the shared
					 * swizzle field, which does not carry
					 * a replicate, so the broadcast has to
					 * happen through src0 of its own
					 * instruction before the multiply. */
					const struct p3d_warp_src sh_all =
						SRC(P3D_WARP_FILE_R, 14);
					struct p3d_warp_inst ndoth = {
						.op = P3D_WARP_DP3,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 14, .dst_mask = 0x2,
						.nsrc = 2,
						.src = { normal, half },
					};
					/* Its own running sum in r13, seeded by
					 * the first light, so it does not race
					 * the diffuse accumulator. */
					const struct p3d_warp_src spec_sum =
						SRC(P3D_WARP_FILE_R, 15);
					struct p3d_warp_inst add_spec = {
						.op = P3D_WARP_MAD,
						.dst_file = P3D_WARP_FILE_DST_R,
						.dst_index = 15, .dst_mask = 0xf,
						.nsrc = 3,
						.src = { spec_col, sh_all,
							 k ? spec_sum : zero },
					};
					unsigned p;

					emit(&e, &ndoth);
					emit_add(&e, P3D_WARP_MAX,
						 P3D_WARP_FILE_DST_R, 14, 0x2,
						 &sh, &zero, 0, 0, 0, NULL);
					for (p = 0; p < shape->shininess_log2;
					     p++)
						emit_add(&e, P3D_WARP_MUL,
							 P3D_WARP_FILE_DST_R,
							 14, 0x2, &sh, &sh,
							 0, 0, 0, NULL);
					/* Broadcast y across r12, then scale
					 * the specular colour by it. */
					emit_add(&e, P3D_WARP_ADD,
						 P3D_WARP_FILE_DST_R, 14, 0xf,
						 &sh, &zero, 0, 0, 0, NULL);
					emit(&e, &add_spec);
				}
				if (shape->accumulator) {
					/* src0 states the per-light factors as
					 * (spot, N.L, N.H, attenuation); a
					 * plain directional light zeroes all
					 * but N.L.  The vendor hands the
					 * colours in registers, not straight
					 * from the constant file, so stage the
					 * diffuse colour into r5 and a zero
					 * specular into r6 first. */
					struct p3d_warp_inst acc = {
						.op = k ? P3D_WARP_ACC :
							  P3D_WARP_ACC_START,
						.dst_mask = 0,
						.nsrc = 3,
						.src = { factors, term,
							 SRC(P3D_WARP_FILE_R, 6) },
					};

					emit_add(&e, P3D_WARP_MUL,
						 P3D_WARP_FILE_DST_R, 5, 0xf,
						 &material, &one, 0, 0, 0,
						 NULL);
					emit_add(&e, P3D_WARP_MUL,
						 P3D_WARP_FILE_DST_R, 6, 0xf,
						 &zero, &zero, 0, 0, 0, NULL);
					emit(&e, &acc);
					continue;
				}
				if (!k || !split_mad) {
					emit(&e, &mad_diffuse);
					continue;
				}
				/* A MAD whose addend is a real operand scales
				 * its product; multiply and add separately. */
				emit_add(&e, P3D_WARP_MUL,
					 P3D_WARP_FILE_DST_R, 5, 0xf,
					 &material, &diffuse, 0, 0, 0, NULL);
				emit_add(&e, P3D_WARP_ADD,
					 P3D_WARP_FILE_DST_R, 4, 0xf,
					 &sum, &term, 0, 0, 0, NULL);
			}
			if (shape->accumulator) {
				/* 0x84 reads the diffuse total out to a write
				 * port with the material constant as its only
				 * operand; a plain read of r10 returns a
				 * constant, so the accumulator is not an
				 * ordinary register. */
				struct p3d_warp_inst read = {
					.op = P3D_WARP_ACC_READ,
					.dst_file = P3D_WARP_FILE_DST_O,
					.dst_index = 1, .dst_mask = 0xf,
					.nsrc = 3,
					.src = { zero, zero, one },
				};

				(void)accum;
				emit(&e, &read);
			} else {
				/* The ambient term, then this face's colour:
				 * the front goes to o1 and the back to o7,
				 * and the setup unit picks by facing. */
				mad_ambient.src[2] = nl > 1 ? sum : latch_t2;
				emit(&e, &mad_ambient);
				if (shape->specular) {
					struct p3d_warp_src spec_sum =
						SRC(P3D_WARP_FILE_R, 15);

					emit_add(&e, P3D_WARP_ADD,
						 P3D_WARP_FILE_DST_T, 2, 0xf,
						 &latch_t2, &spec_sum,
						 0, 0, 0, NULL);
				}
				emit_add(&e, P3D_WARP_ADD,
					 P3D_WARP_FILE_DST_O, face ? 7 : 1,
					 0xf, &latch_t2, &zero, 0, 0, 0, NULL);
			}
			}
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
		if (shape->polygon_offset) {
			/* The bias rides in z, so the window position leaves
			 * with it already applied. */
			struct p3d_warp_src bias =
				SRC(P3D_WARP_FILE_C, shape->offset_slot);

			emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_T, 1, 0xf,
				 &latch_t1, &bias, 0, 0, 0, NULL);
		}
		emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 0, 0xf,
			 &latch_t1, &zero, 0, 0, 0, NULL);
	} else {
		if (shape->polygon_offset) {
			/* Screen-space vertices: bias z on the way to o0. */
			struct p3d_warp_src bias =
				SRC(P3D_WARP_FILE_C, shape->offset_slot);

			emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 0, 0xf,
				 &position, &bias, 0, 0, 0, NULL);
		} else {
			emit_add(&e, P3D_WARP_ADD, P3D_WARP_FILE_DST_O, 0, 0xf,
				 &position, &position_zero, 0, 0, 0, NULL);
		}
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

		if (shape->texgen_mask & (1u << i)) {
			/* s and t are each a plane against the position, so
			 * one dp4 per component into its own mask.  The set's
			 * r and q are not generated, and the sampler reads
			 * all four, so state them: leaving them at whatever
			 * the register held sampled the wrong texel. */
			const struct p3d_warp_src rq =
				SRC_SWZ(P3D_WARP_FILE_V, 0,
					P3D_WARP_SWZ(4, 4, 4, 5));
			unsigned k;

			emit_mov(&e, P3D_WARP_FILE_DST_O, 3 + i, &rq);

			for (k = 0; k < 2; k++) {
				struct p3d_warp_src plane =
					SRC(P3D_WARP_FILE_C,
					    shape->texgen_slot + 2 * i + k);
				struct p3d_warp_inst dp4 = {
					.op = P3D_WARP_DP4,
					.dst_file = P3D_WARP_FILE_DST_O,
					.dst_index = 3 + i,
					.dst_mask = 1u << k,
					.nsrc = 2,
					.src = { position, plane },
				};

				emit(&e, &dp4);
			}
			continue;
		}
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
