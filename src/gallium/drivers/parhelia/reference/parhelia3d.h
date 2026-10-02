/*
 * parhelia3d - the reverse-engineered Parhelia 3D engine as a reusable C core.
 *
 * This is the consolidation of the RE work that lived in
 * tools/gen-minimal-draw.py (stream construction) and tools/parhelia-3dtri.c
 * (init and submission), in a form a DRM driver and a Mesa/Gallium winsys can
 * both link against.
 *
 * Design constraints, so it drops into either:
 *   - no allocation: every buffer is supplied by the caller
 *   - no libc beyond <string.h>, no floating point in the builder path
 *     (vertex data is handed over as raw dwords; the caller converts)
 *   - no MMIO in the builder: submission goes through struct p3d_ops, so the
 *     kernel provides writel/readl and userspace provides its own
 *
 * Everything encoded here is cited to the bring-up documentation; where a
 * value is a measured constant rather than a decoded field it says so.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef PARHELIA3D_H
#define PARHELIA3D_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ registers
 *
 * The 3D block is an indexed file: reg = 0x1000 + index * 4, and a register
 * list packet names registers by that index.  Only the registers the engine
 * actually needs for a draw are listed; the full map is in docs/.
 */
#define P3D_REG_BASE		0x1000

#define P3D_R_STREAM0		0x1000	/* memory class of the vertex stream */
#define P3D_R_STRIDE0		0x1040	/* vertex fetch stride selector */
#define P3D_R_VTXCAP		0x1060	/* vertices addressable = free / stride */
#define P3D_R_VTXCAP_HI		0x1064
#define P3D_R_PRIMWORD		0x10b0	/* primitive + topology */
#define P3D_R_DESC_INDEX	0x10b4	/* attribute descriptor write index */
#define P3D_R_DESC		0x10b8	/* attribute descriptor port */
#define P3D_R_STAGE0		0x10c0	/* setup-stage entries 0x10c0..0x10d8 */
#define P3D_R_STAGE2		0x10c8
#define P3D_R_WARP_MODE		0x10f0	/* WARP mode latch */
#define P3D_R_WARP_PAGE		0x10f8
#define P3D_R_TEX_UNK10FC	0x10fc	/* textured draws only (section 19) */
#define P3D_R_WARP_ENTRY	0x1100
#define P3D_R_WARP_ENTRY2	0x1104
#define P3D_R_LINE_STIPPLE	0x1028	/* (repeat << 16) | pattern */
#define P3D_R_ATTR_PRESENT	0x1110	/* bitmask over declared attribute kinds */
#define P3D_R_ATTR_MAP0		0x1114
#define P3D_R_ATTR_MAP1		0x1118
#define P3D_R_PIPECTL		0x1130	/* [25:24] primitive class, b12 flat, b0 cull */
#define P3D_R_TEX_FILTER	0x11c0
#define P3D_R_TEX_WRAP		0x11c4
#define P3D_R_TEX_SURF_BASE	0x11dc
#define P3D_R_TEX_SURF_W	0x11e0	/* width | texelcode<<16 | fmt<<24 */
#define P3D_R_TEX_SURF_H	0x11e4	/* height | slices<<16 */
#define P3D_R_TEX_CACHE		0x11e8
#define P3D_R_DEPTH_STATE	0x11f4
#define P3D_R_ZFUNC		0x12c8	/* [0] test enable, [3:1] compare func */
#define P3D_R_STENCIL_REF	0x12cc	/* compare mask [7:0], reference [15:8] */
#define P3D_R_ZWRITE		0x12d0	/* bit 8 = depth write enable */
#define P3D_R_ALPHATEST		0x12e4	/* [0] enable, [3:1] compare, [15:8] ref */
#define P3D_R_COLMASK		0x1324	/* colour write mask, byte per channel */
#define P3D_R_BLEND		0x12e8

/* 0x12e8 BLENDCTL, layout proven from both vendor binaries in
 * docs/parhelia-blend-alpha-encoding.md:
 *   [0] enable, [4:1] srcRGB, [8:5] dstRGB, [12:9] srcAlpha, [16:13] dstAlpha,
 *   [19:17] and [22:20] the blend equation (written twice; the hardware has one
 *   equation for both pipes), [23] dither engaged, [24] dither off.
 * Blending is only honoured when DWGCTL's ROP nibble [7:4] is 0xc (GXcopy).
 * The factor nibbles differ between the two pipes for the colour factors: a
 * colour factor on the RGB pipe becomes the matching alpha factor on the alpha
 * pipe, so each GL factor carries both codes. */
#define P3D_BLEND_DITHER	0x00800000u
#define P3D_BLEND_DITHER_DISABLE 0x01000000u

/* MODE for the kind-0 constant-file loader bind */
#define P3D_WARP_MODE_LOADER	0x20000028u
#define P3D_R_TEX_ENABLE	0x1200	/* +4*unit; low nibble = pipe ownership */
#define P3D_R_SURF0_BASE	0x1220	/* slot n at 0x1220 + 8*n, cfg at +4 */
#define P3D_R_SURF1_BASE	0x1228	/* depth */
#define P3D_R_COMB_ALPHA	0x1260	/* combiner: RGB 0x1264/0x126c, A 0x1260/0x1268 */
#define P3D_R_COMB_RGB		0x1264
#define P3D_R_WATERMARK		0x12b0
#define P3D_R_ZINIT		0x12b4	/* Z-init clear value, packed like FCOL */
#define P3D_R_EOBTAG		0x12b8	/* end-of-batch, constant 0x00070601 */
#define P3D_R_FENCE		0x12bc	/* batch fence COUNTER, steps by 8 */
#define P3D_R_FOG		0x12d4	/* enable, mode and colour */
#define P3D_R_FOG_START		0x12d8
#define P3D_R_FOG_END		0x12dc
#define P3D_R_FOG_TABLE		0x12e0	/* 64-dword table port */
#define P3D_R_GUARDBAND		0x12f4
#define P3D_R_RASTERMODE	0x12fc
#define P3D_R_DWGCTL		0x1300	/* [3:0] primitive class nibble */
#define P3D_R_FCOL		0x1314
#define P3D_R_SCISSOR_MIN	0x1328
#define P3D_R_SCISSOR_MAX	0x132c	/* (ymax<<16) | xmax */
#define P3D_R_TEX_UNK1330	0x1330	/* textured draws only (section 19) */
#define P3D_R_TEX_UNK1334	0x1334
#define P3D_R_STROBE		0x1374	/* payload ignored by the hardware */

/* MMIO registers outside the indexed file */
#define P3D_MMIO_RING_START	0x0404
#define P3D_MMIO_RING_END	0x0408	/* end | 3: the low bits select DMA mode */
#define P3D_MMIO_FENCE_DONE	0x041c	/* reads back the last RETIRED fence tag */
#define P3D_MMIO_BM_STATUS	0x08f0

/* ------------------------------------------------------------------ primitives
 *
 * A draw packet is a byte range of consecutive vertices and may carry a whole
 * run of primitives; the vendor emits 24-vertex / eight-triangle packets.
 * Measured on the P690 (--depth-interlock-probe): overlapping primitives in
 * one packet keep both the depth interlock and their rasterisation order -
 * triangle lists, strips, fans and quads at up to 20 primitives per packet
 * are pixel-identical to one packet per primitive, with the depth test, with
 * depth writes and under a non-commutative blend.  The one exception is a
 * packet of exactly nine vertices, which renders wrong pixels and leaves the
 * engine unable to retire the next draw.
 */
enum p3d_prim {
	P3D_POINTS = 0,
	P3D_LINES,
	P3D_LINESTRIP,
	P3D_TRIANGLES,
	P3D_TRISTRIP,
	P3D_TRIFAN,
	P3D_QUADS,
	P3D_PRIM_COUNT
};

/* ------------------------------------------------------------------ builder */

struct p3d_reloc {
	uint32_t ring_off;	/* dword index of this call in the ring */
	uint32_t sec_off;	/* dword index of the packet in the section blob */
	uint32_t sec_len;
	int	 is_draw;	/* draw packets need the vertex range patched in */
};

struct p3d_cs {
	uint32_t	 *sec;		/* secondary command buffer */
	size_t		  sec_cap, sec_len;
	uint32_t	 *ring;		/* primary ring: one CALL per packet */
	size_t		  ring_cap, ring_len;
	struct p3d_reloc *rel;
	size_t		  rel_cap, rel_len;
	int		  error;	/* sticky: set on overflow */
};

/* A register write, as the builder takes them. */
struct p3d_rw { uint32_t reg, val; };

/* P3D_BREAK ends the current tag word early.  The vendor's per-vertex bind
 * puts the 0x1374 strobe in a tag word of its own, which a naive
 * four-at-a-time packer cannot reproduce. */
#define P3D_BREAK 0xffffffffu

void	 p3d_cs_init(struct p3d_cs *cs,
		     uint32_t *sec, size_t sec_cap,
		     uint32_t *ring, size_t ring_cap,
		     struct p3d_reloc *rel, size_t rel_cap);

/* Packet builders.  Each appends one packet and one ring CALL. */
void	 p3d_reglist(struct p3d_cs *cs, const struct p3d_rw *rw, size_t n);
void	 p3d_blockload(struct p3d_cs *cs, uint32_t first,
		       const uint32_t *vec4s, size_t nvec);
void	 p3d_drawpacket(struct p3d_cs *cs, unsigned dw_per_vertex);
/* Mode 3: draw [first,last) from the already-bound vertex streams. */
void	 p3d_rangepacket(struct p3d_cs *cs, uint32_t first, uint32_t last);
void	 p3d_rangepacket2(struct p3d_cs *cs,
			 uint32_t first0, uint32_t last0,
			 uint32_t first1, uint32_t last1);
/* P690 mode-7 GL_UNSIGNED_INT indexed packet.  Its address operands stay
 * zero until the guarded kernel INDEX_VRAM relocation supplies a local-VRAM
 * range; callers cannot encode a raw address through this builder. */
void	 p3d_indexpacket_u32(struct p3d_cs *cs);
void	 p3d_bindpacket(struct p3d_cs *cs, uint32_t mode,
			const struct p3d_rw *rw, size_t n, int strobe_alone);

/* ------------------------------------------------------------------ state */

struct p3d_surface {
	uint32_t base;		/* surface register value; a POOL OFFSET under a
				 * live vendor context, a raw address cold */
	uint32_t pitch;		/* pixels */
	uint32_t fmt;		/* 0x32 = A8R8G8B8, 0x36 = S8Z24,
				 * 0x28 = R5G6B5, 0x2a = D16,
				 * 0x34 = A2R10G10B10 (GigaColor) */
};

/* Bytes per pixel of a surface format code, zero when the code is not one
 * this driver drives.  The vendor's depth codes are in
 * docs/parhelia-p690-zcompression-clear-banks-re.md §6. */
static inline unsigned p3d_surface_format_bytes(uint32_t fmt)
{
	switch (fmt) {
	case 0x32: return 4;	/* A8R8G8B8 colour */
	case 0x34: return 4;	/* A2R10G10B10 colour, GigaColor */
	case 0x36: return 4;	/* S8Z24 depth */
	case 0x28: return 2;	/* R5G6B5 colour */
	case 0x2a: return 2;	/* D16 depth */
	default:   return 0;
	}
}

/* Stencil bit count for 0x12c8[20:17]; only D16 has none.  The baseline S8Z24
 * value stands for every other code, including an unbound depth surface. */
static inline uint32_t p3d_depth_stencil_bits(uint32_t fmt)
{
	return fmt == 0x2a ? 0u : 8u;
}

/* 0x11c0 FILTER: MIN [1:0], MAG [4:3], MIP [7:6], bit 12 always set,
 * LOD bias [24:16] signed 4.4 biased -4.0.  0x01c01000 = nearest, no mip.
 * 0x11c4 WRAP: S [2:0], T [5:3], R [8:6], LOD min [23:16], max [31:24].
 * 0xf0000000 = repeat, LOD 0..15.  Codes measured from the capture:
 * texlin gives filter 0x01c01012 (MIN=MAG=2) and texclamp wrap 0xf0000036
 * (S=T=6). */
enum p3d_filter { P3D_FILTER_NEAREST = 0, P3D_FILTER_LINEAR = 2,
		  P3D_FILTER_ANISO = 3 };
enum p3d_mip	{ P3D_MIP_NONE = 0, P3D_MIP_NEAREST = 1, P3D_MIP_LINEAR = 2 };
/* 0x11c4 address modes, a 3-bit enum per axis, not a bit field.  Both vendor
 * stacks share one table ({0,0,4,1,7,5,6}, MTXPARD .data 0x1d45d0 and kernel
 * mtx3018 .data 0x19330); codes 2 and 3 are emitted by no path.  Codes from 6
 * up sample the 0x11c8 border colour outside [0,1], 6 mirroring nothing and 7
 * being the same clamp with the vendor's own border semantics.  Every code
 * here is re-measured on the P690 by parhelia-drm-test --wrap-probe; see
 * docs/parhelia-p690-texture-wrap-border.md. */
enum p3d_wrap	{ P3D_WRAP_REPEAT = 0, P3D_WRAP_CLAMP_EDGE = 1,
		  P3D_WRAP_MIRROR = 4, P3D_WRAP_MIRROR_ONCE = 5,
		  P3D_WRAP_CLAMP = 6, P3D_WRAP_BORDER = 7 };

/* GL_TEXTURE_ENV_MODE.  MODULATE is MUL(TEX, PREV); REPLACE is MOV(TEX), which
 * on this hardware means the alpha combiner slot 0x1260 goes to zero, 0x1268
 * stays at its resting 0x01400400, and the vertex colour attribute is not
 * fetched at all - the capture's texrepl has 0x1110 = 0x101 where MODULATE has
 * 0x103. */
enum p3d_texenv { P3D_TEXENV_MODULATE = 0, P3D_TEXENV_REPLACE = 1,
		  P3D_TEXENV_ADD = 2, P3D_TEXENV_DECAL = 3,
		  P3D_TEXENV_BLEND = 4 };

/* A combiner constant register holds two signed Q1.10 values in bits [10:0]
 * and [26:16].  The components are NOT (r, g) then (b, a): the first register
 * carries red and ALPHA, the second blue and GREEN.  Packing them the obvious
 * way made a constant's green read back as its alpha on the P690, with red and
 * blue exact - see docs/parhelia-p690-combiner-isa-re.md section 7.1.  Mesa
 * converts/clamps floats to [-1024, 1023] before using this macro; keeping the
 * builder integer-only makes it usable in a kernel too. */
#define P3D_COMBINER_CONST(lo, hi) \
	(((uint32_t)(lo) & 0x7ffu) | (((uint32_t)(hi) & 0x7ffu) << 16))

/* ---- WARP vertex programs ----------------------------------------------
 *
 * A 96-bit bundle is a primary operation writing destination port B plus an
 * optional secondary operation that reads src2 and writes port A.  Encoding
 * and evidence: docs/parhelia-p690-warp-isa-re.md.
 */
enum p3d_warp_src_file {
	P3D_WARP_FILE_V = 0,	/* vertex-fetch input */
	P3D_WARP_FILE_R = 1,	/* temporary */
	P3D_WARP_FILE_T = 2,	/* latch */
	P3D_WARP_FILE_C = 3,	/* constant / engine block state */
};

enum p3d_warp_dst_file {
	P3D_WARP_FILE_DST_T = 0,	/* latch; T1 is the address register */
	P3D_WARP_FILE_DST_R = 1,
	P3D_WARP_FILE_DST_O = 2,	/* output slot */
	P3D_WARP_FILE_DST_C = 3,	/* constant file - never from userspace */
};

/* Three bits per component: 0..3 select x y z w, 4 is the literal 0.0 and 5
 * the literal 1.0, which is how a program gets constants without a slot. */
#define P3D_WARP_SWZ(x, y, z, w) \
	((uint16_t)((x) | ((y) << 3) | ((z) << 6) | ((w) << 9)))
#define P3D_WARP_SWZ_IDENTITY	P3D_WARP_SWZ(0, 1, 2, 3)	/* 0x688 */
#define P3D_WARP_SWZ_ZERO	P3D_WARP_SWZ(4, 4, 4, 4)	/* 0x924 */
#define P3D_WARP_SWZ_ONE	P3D_WARP_SWZ(5, 5, 5, 5)	/* 0xb6d */
#define P3D_WARP_SWZ_XYZ1	P3D_WARP_SWZ(0, 1, 2, 5)
#define P3D_WARP_SWZ_XYZ0	P3D_WARP_SWZ(0, 1, 2, 4)

enum p3d_warp_op {
	P3D_WARP_WRA = 0x60,	/* arithmetic no-op, carries a secondary op */
	P3D_WARP_MIN = 0x61,
	P3D_WARP_MAX = 0x62,
	P3D_WARP_ADD = 0x71,
	P3D_WARP_MUL = 0x75,
	P3D_WARP_MAD = 0x76,	/* s0 * s1 + s2 */
	P3D_WARP_DP4 = 0x77,
	P3D_WARP_DP3 = 0x78,
	P3D_WARP_SMUL = 0x89,	/* dst.c = s1.c * (1 / s0.w): perspective divide */
	P3D_WARP_RSQ = 0x8a,	/* dst.c = s1.c * rsq(s0.w) */
	/* The fixed-function light accumulator.  Neither ACC opcode encodes a
	 * destination: they commit into r10 (diffuse) and r11 (specular). */
	P3D_WARP_ACC_START = 0x82,
	P3D_WARP_ACC = 0x81,
	P3D_WARP_ACC_READ = 0x84,	/* diffuse total -> a write port */
	/* User clip planes: a dp4 against a plane, accumulating a clip code
	 * that 0x6d publishes to o10.x. */
	P3D_WARP_CLIP_FIRST = 0x7a,
	P3D_WARP_CLIP_NEXT = 0x79,
	P3D_WARP_CLIP_EMIT = 0x6d,
};

#define P3D_WARP_SOP_MOV 1
#define P3D_WARP_SOP_FRC 4
#define P3D_WARP_SOP_FLOOR 5

/* Fixed-function vertex-fetch slots, in the classic D3D numbering the
 * attribute descriptors use. */
#define P3D_WARP_V_POSITION	0
#define P3D_WARP_V_COLOUR	5
#define P3D_WARP_V_COLOUR1	6	/* specular, and the fog carrier */
#define P3D_WARP_V_TEXCOORD0	7

struct p3d_warp_src {
	uint8_t  file;		/* enum p3d_warp_src_file */
	uint8_t  index;		/* 0..127; above 15 uses the extension field */
	uint8_t  neg;
	uint16_t swizzle;
};

struct p3d_warp_inst {
	uint8_t op;		/* primary opcode */
	uint8_t dst_file, dst_index, dst_mask;
	uint8_t nsrc;
	struct p3d_warp_src src[3];
	uint8_t sop;		/* secondary opcode, 0 = none */
	uint8_t sdst_file, sdst_index, sdst_mask;
	struct p3d_warp_src ssrc;	/* the secondary operand, i.e. src2 */
};

void p3d_warp_encode(const struct p3d_warp_inst *in, uint32_t out[3]);

/* The shapes a fixed-function vertex pipeline needs.  A transforming program
 * reads the matrix from @matrix_slot..+3 and the viewport pair from
 * @viewport_scale_slot / @viewport_offset_slot, does the perspective divide
 * itself and hands the setup unit a window-space position.
 *
 * @write_t2 primes WARP temporary T2, which every draw depends on.  A program
 * that samples a texture must NOT write it: doing so locked the engine on the
 * first vertex-alpha draw.  See docs/parhelia-p690-warp-isa-re.md section 3.3.
 */
struct p3d_warp_shape {
	int	 transform;
	unsigned matrix_slot;
	unsigned viewport_scale_slot, viewport_offset_slot;
	int	 light;
	unsigned light_slot, material_slot, ambient_slot;
	/* Lights beyond the first.  Light i takes its direction from
	 * @light_slot + 2*i and its colour from @material_slot + 2*i, so one
	 * pair of constants per light, and the diffuse terms accumulate.
	 * Zero or one keeps the single-light program byte for byte. */
	unsigned lights;
	/* Emit the accumulating light terms as a separate multiply and add
	 * instead of one MAD, which is under measurement. */
	int	 split_mad;
	/* Write the N.L dot product to one component and replicate it, which
	 * is the only shape the vendor library ever emits.  A full-mask dp3
	 * broadcasts, but its semantics are unproven. */
	int	 masked_dp3;
	/* Use the hardware light accumulator instead of summing in registers:
	 * 0x82 starts the walk, 0x81 adds each further light, and the diffuse
	 * total is read out of r10.  This is how the vendor library does it -
	 * it never chains MADs - and it is the only shape that accumulates
	 * correctly (parhelia-fog-points-implementation-leads.md). */
	int	 accumulator;
	/* Positional lights with distance attenuation.  Light i then reads a
	 * position rather than a direction, and its three coefficients
	 * (constant, linear, quadratic) from @atten_slot + i.  The term
	 * becomes max(0, N.L) / (k0 + k1*d + k2*d*d). */
	int	 attenuation;
	unsigned atten_slot;
	/* GL_SPOT_CUTOFF: light i reads its spot direction from
	 * @spot_slot + i, with the cosine of the cutoff in .w.  The term is
	 * gated off outside the cone.  Needs @attenuation, which is what
	 * computes the direction to the light. */
	int	 spot;
	unsigned spot_slot;
	/* GL_CLIP_PLANE.  @clip_planes equations, each already transformed by
	 * transpose(inverse(view * projection)) host-side and compacted so the
	 * enabled ones are consecutive from @clip_slot.  The program dp4s the
	 * clip-space position against each and publishes the accumulated code
	 * to o10.x, which is the only thing that writes that output. */
	unsigned clip_planes;
	unsigned clip_slot;
	/* GL_LIGHT_MODEL_TWO_SIDE.  The whole light walk runs a second time
	 * with the normal reversed and the back material, and its result goes
	 * to o7 instead of o1 - the hardware picks between them by facing, so
	 * one draw covers both.  The back colours come from
	 * @back_material_slot + 2*i, matching the front layout. */
	int	 two_sided;
	unsigned back_material_slot;
	/* Vertex blending.  @bones matrices of four rows each from
	 * @bone_slot, blended by a weight the vertex carries in its colour x,
	 * with the last weight the implicit 1 - sum.  The blended position
	 * then feeds the ordinary model-view-projection transform.  Absolute
	 * constants, so no index register and no base-relative addressing. */
	unsigned bones;
	unsigned bone_slot;
	/* glPolygonOffset.  The depth bias is applied by the vertex program,
	 * not by setup - which is why loading c[0x178] alone measured inert.
	 * @offset_slot holds (0, 0, units, 0) in window-depth units and is
	 * added to the viewport-transformed z.  The slope-scaled term needs a
	 * per-primitive gradient a vertex program cannot see, so only the
	 * constant term is generated here. */
	int	 polygon_offset;
	unsigned offset_slot;
	/* Specular.  Light i reads its halfway vector from @specular_slot + 2*i
	 * and its specular colour from @specular_slot + 2*i + 1, and the term
	 * is max(0, N.H) raised to 2^@shininess_log2 by repeated squaring -
	 * this ISA has no POW, so the exponent is a power of two. */
	int	 specular;
	unsigned specular_slot;
	unsigned shininess_log2;
	int	 colour_white;	/* GL_REPLACE: force the vertex colour to 1 */
	int	 write_t2;
	int	 clip_copy;	/* o11 <- the pre-viewport position */
	unsigned texcoords;	/* o3 <- v7, o4 <- v8, ... */
	/* GL_TEXTURE_GEN: coordinate sets whose bit is set here take their s
	 * and t from a plane equation against the vertex position instead of
	 * from the fetched attribute, which is GL_OBJECT_LINEAR.  Set i reads
	 * its two planes from @texgen_slot + 2*i.  Eye-linear is the same
	 * program with the planes already transformed host-side. */
	unsigned texgen_mask;
	unsigned texgen_slot;
};

/* Returns the instruction count including END and its padding word, or 0 when
 * the program does not fit in @max instructions. */
unsigned p3d_warp_build(const struct p3d_warp_shape *shape, uint32_t *out,
			unsigned max);

/* ---- programmable fragment combiner -----------------------------------
 *
 * The five fixed-function texenv shapes above are special cases of a small
 * programmable ALU.  One slot co-issues an RGB operation (sources 0x1264,
 * opcode 0x126c) and an alpha operation (sources 0x1260, opcode 0x1268)
 * over three sources A, B, C, each with a register, a modifier and a
 * channel replicate.  Encoding and evidence:
 * docs/parhelia-fragment-isa.md, docs/parhelia-p690-ganged-pipe-ta-re.md.
 */
enum p3d_comb_reg {
	P3D_COMB_T0 = 0, P3D_COMB_T1 = 1, P3D_COMB_T2 = 2, P3D_COMB_T3 = 3,
	P3D_COMB_R0 = 4,	/* also the fragment result register */
	P3D_COMB_R1 = 5,
	P3D_COMB_C0 = 0xe,	/* this instruction's first constant */
	P3D_COMB_C1 = 0xf,	/* this instruction's second constant */
	P3D_COMB_V0 = 0x10,	/* interpolated colour 0 */
	P3D_COMB_V1 = 0x11,	/* interpolated colour 1 */
};

enum p3d_comb_mod {
	P3D_COMB_MOD_NONE = 0,
	P3D_COMB_MOD_NEG = 1,		/* -x */
	P3D_COMB_MOD_BIAS = 2,		/* x - 0.5 */
	P3D_COMB_MOD_BIAS_NEG = 3,
	P3D_COMB_MOD_SIGNED = 4,	/* 2x - 1 */
	P3D_COMB_MOD_SIGNED_NEG = 5,
	P3D_COMB_MOD_INVERT = 0xf,	/* 1 - x */
};

/* Replicate one channel of a source.  The alpha half has only two bits, so
 * P3D_COMB_REP_A degenerates to the natural alpha read there - which is the
 * same value, so a .aaaa swizzle needs no special case. */
enum p3d_comb_rep {
	P3D_COMB_REP_NONE = 0, P3D_COMB_REP_B = 1, P3D_COMB_REP_G = 2,
	P3D_COMB_REP_R = 3, P3D_COMB_REP_A = 4,
};

/* dst = the expression in the comment.  Values are 0x126c[14:9]. */
enum p3d_comb_op {
	P3D_COMB_MUL = 0x00,	/* A * C */
	P3D_COMB_MOV = 0x02,	/* A */
	P3D_COMB_SUB = 0x03,	/* A - B */
	P3D_COMB_MAD = 0x04,	/* A * C + B */
	P3D_COMB_LRP = 0x05,	/* A * C + B * (1 - C) */
	P3D_COMB_ADD = 0x06,	/* A + B */
	P3D_COMB_DP3 = 0x08,	/* dot3(A, C), broadcast */
	P3D_COMB_DP4 = 0x10,	/* dot4(A, C), broadcast */
	P3D_COMB_CND = 0x26,	/* A > 0.5 ? B : C */
	P3D_COMB_CMV = 0x2e,
	P3D_COMB_CMP = 0x3e,	/* A >= 0 ? B : C */
};

/* 0x126c[19:17], a signed shift count: 0..3 multiply, 6 and 7 divide. */
enum p3d_comb_scale {
	P3D_COMB_SCALE_1 = 0, P3D_COMB_SCALE_2 = 1, P3D_COMB_SCALE_4 = 2,
	P3D_COMB_SCALE_8 = 3, P3D_COMB_SCALE_D4 = 6, P3D_COMB_SCALE_D2 = 7,
};

struct p3d_comb_src {
	uint8_t reg;	/* enum p3d_comb_reg */
	uint8_t mod;	/* enum p3d_comb_mod */
	uint8_t rep;	/* enum p3d_comb_rep */
};

/* One half of a co-issued slot.  @used = 0 leaves that half a MOV r0, r0. */
struct p3d_comb_half {
	uint8_t used;
	uint8_t op;	/* enum p3d_comb_op */
	uint8_t dst;	/* enum p3d_comb_reg, only t0..t3/r0/r1 are writable */
	uint8_t scale;	/* enum p3d_comb_scale */
	uint8_t sat;
	struct p3d_comb_src src[3];	/* A, B, C */
};

struct p3d_comb_inst {
	struct p3d_comb_half rgb, alpha;
	/* Up to two constants, shared by both halves of this slot.  Values are
	 * clamped to [-1, 1) by the Q1.10 register format. */
	uint8_t has_const[2];
	float constant[2][4];
};

/* Ten slots exist and the count is padded to five or ten.  The driver only
 * emits five: more sets the 0x1300 half-rate bit, which is untested. */
#define P3D_COMB_MAX_INSTRUCTIONS 10u
#define P3D_COMB_QUANTUM 5u

struct p3d_comb_program {
	unsigned count;
	struct p3d_comb_inst inst[P3D_COMB_MAX_INSTRUCTIONS];
};

/* Sampler target.  0x11e0[20:19] carries the surface type: the Linux HAL's
 * descriptor writer mtx3424 (.text+0x1eab0) ORs (type & 3) << 3 into the byte
 * that is 0x11e0[23:16], and its three surface-class builders pass 0 for 2-D
 * (mtx1747 .text+0x290ed), 1 for a cube map (mtx3887 .text+0x279b1) and 2 for
 * a volume (mtx325 .text+0x28677).  There is no 1-D class: GL_TEXTURE_1D is a
 * height-1 2-D surface.  0x1200+4u[18:16] is the coordinate mode, 4 for the
 * two-coordinate targets and 6 for the three-coordinate ones.  See
 * docs/parhelia-p690-volume-cube-targets.md. */
enum p3d_tex_type { P3D_TEX_TYPE_2D = 0, P3D_TEX_TYPE_CUBE = 1,
		    P3D_TEX_TYPE_VOLUME = 2 };
#define P3D_TEX_COORD_2D	4u
/* Bit 16 divides the coordinate by its fourth component per fragment: a
 * 4-component set (mask 0xf) with mode 5 samples s/q,t/q, measured by
 * parhelia-drm-test --texture-projective-probe on the P690. */
#define P3D_TEX_COORD_2D_PROJ	5u
#define P3D_TEX_COORD_3D	6u

struct p3d_texture {
	uint32_t base, width, height;
	uint32_t fmt;		/* 0x32 A8R8G8B8, 0x33 X8R8G8B8, 0x11 L8 */
	uint32_t texelcode;	/* 0x11e0[18:16]: log2(16 / bytes-per-texel) */
	uint32_t filter, wrap;		/* raw override; 0 = build from below */
	enum p3d_filter min_filter, mag_filter;
	enum p3d_mip	mip_filter;
	enum p3d_wrap	wrap_s, wrap_t, wrap_r;
	/* Volume and cube-map descriptor.  0x11e4[27:16] is the slice count with
	 * a minimum of one, 0x11e0[20:19] the surface type and 0x1200+4u[18:16]
	 * the coordinate mode; zero in either keeps the plain 2-D descriptor. */
	uint32_t	depth;
	unsigned	type;
	unsigned	coord_mode;
	unsigned	max_level;	/* 0x11e4[31:28] */
	enum p3d_texenv	env;
	/* GL_TEXTURE_ENV_COLOR for P3D_TEXENV_BLEND, in the hardware's own
	 * component pairing: (red, alpha) and (blue, green). */
	uint32_t	env_colour_ra, env_colour_bg;	/* P3D_COMBINER_CONST pairs */
	/* GL_TEXTURE_BORDER_COLOR in 0x11c8, the colour the address modes from
	 * code 6 up return outside [0,1].  GL's default is transparent black,
	 * which is this field's zero. */
	uint32_t	border_colour;
	uint32_t	reloc_offset;	/* base level byte offset, relocated */
	/* Diagnostic override of the per-unit auxiliary window
	 * 0x11cc, 0x11d0, 0x11c8, 0x11d4, 0x11d8; NULL keeps the defaults. */
	const uint32_t	*aux;
};

uint32_t p3d_tex_filter(const struct p3d_texture *t);
uint32_t p3d_tex_wrap(const struct p3d_texture *t);
uint32_t p3d_border_colour(uint32_t fmt, uint32_t argb);

/* The address unit reads 0x11c8 only for codes 6 and 7, and the vendor gates
 * the register write on exactly that, on all three axes. */
static inline int p3d_tex_border_used(const struct p3d_texture *t)
{
	return t->wrap_s >= P3D_WRAP_CLAMP || t->wrap_t >= P3D_WRAP_CLAMP ||
	       t->wrap_r >= P3D_WRAP_CLAMP;
}

/* Compare functions are a three-bit MASK, shared by the depth test (0x12c8)
 * and the alpha test (0x12e4): bit 0 = EQUAL, bit 1 = LESS, bit 2 = GREATER,
 * with bit 0 of the register enabling the test.  Three independent captures
 * agree: GL_LESS -> 2, GL_GEQUAL -> 5 (GREATER|EQUAL), GL_GREATER -> 4, and
 * a disabled test carries 7 (ALWAYS). */
/* the .rgb code goes in the RGB-pipe field, .a in the alpha-pipe field */
struct p3d_blend_factor { uint8_t rgb, a; };
#define P3D_BF_ZERO		((struct p3d_blend_factor){ 0x0, 0x0 })
#define P3D_BF_ONE		((struct p3d_blend_factor){ 0x1, 0x1 })
#define P3D_BF_CONST_COLOR	((struct p3d_blend_factor){ 0x2, 0xe })
#define P3D_BF_ONE_MINUS_CONST_COLOR ((struct p3d_blend_factor){ 0x3, 0xf })
#define P3D_BF_SRC_COLOR	((struct p3d_blend_factor){ 0x4, 0x6 })
#define P3D_BF_ONE_MINUS_SRC_COLOR ((struct p3d_blend_factor){ 0x5, 0x7 })
#define P3D_BF_SRC_ALPHA	((struct p3d_blend_factor){ 0x6, 0x6 })
#define P3D_BF_ONE_MINUS_SRC_ALPHA ((struct p3d_blend_factor){ 0x7, 0x7 })
#define P3D_BF_DST_ALPHA	((struct p3d_blend_factor){ 0xa, 0xa })
#define P3D_BF_ONE_MINUS_DST_ALPHA ((struct p3d_blend_factor){ 0xb, 0xb })
#define P3D_BF_DST_COLOR	((struct p3d_blend_factor){ 0x8, 0xa })
#define P3D_BF_ONE_MINUS_DST_COLOR ((struct p3d_blend_factor){ 0x9, 0xb })
#define P3D_BF_SRC_ALPHA_SATURATE  ((struct p3d_blend_factor){ 0xc, 0x1 })
#define P3D_BF_CONST_ALPHA	((struct p3d_blend_factor){ 0xe, 0xe })
#define P3D_BF_ONE_MINUS_CONST_ALPHA ((struct p3d_blend_factor){ 0xf, 0xf })

enum p3d_blend_eq {
	P3D_BEQ_ADD = 0,
	P3D_BEQ_SUBTRACT = 1,
	P3D_BEQ_REVERSE_SUBTRACT = 2,
	P3D_BEQ_MIN = 5,
	P3D_BEQ_MAX = 6,
};

enum p3d_cmp {
	P3D_CMP_NEVER	 = 0,
	P3D_CMP_EQUAL	 = 1,
	P3D_CMP_LESS	 = 2,
	P3D_CMP_LEQUAL	 = 3,
	P3D_CMP_GREATER	 = 4,
	P3D_CMP_GEQUAL	 = 5,
	P3D_CMP_NOTEQUAL = 6,
	P3D_CMP_ALWAYS	 = 7,
};

/* Packed 0x12c8 stencil operations recovered from the Windows ICD. */
enum p3d_stencil_op {
	P3D_STENCIL_KEEP = 0,
	P3D_STENCIL_REPLACE = 1,
	P3D_STENCIL_INVERT = 2,
	P3D_STENCIL_ZERO = 3,
	P3D_STENCIL_INCR_WRAP = 4,
	P3D_STENCIL_DECR_WRAP = 5,
	P3D_STENCIL_INCR_SAT = 6,
	P3D_STENCIL_DECR_SAT = 7,
};
#define P3D_Z_LESS	P3D_CMP_LESS
#define P3D_Z_GEQUAL	P3D_CMP_GEQUAL
typedef enum p3d_cmp p3d_zfunc_t;

/* The original Parhelia has four texture units; the LX family has two. */
#define P3D_MAX_TEXTURE_UNITS 4u

struct p3d_state {
	struct p3d_surface colour;
	struct p3d_surface depth;	/* .base 0 = none */
	int		   has_depth;	/* bind the Z surface */
	int		   depth_test;	/* run the compare */
	enum p3d_cmp	   depth_func;
	uint32_t	   depth_raw;	/* whole 0x12c8 value; overrides the above */
	uint32_t	   comb_op;	/* probe: opcode base OR'd into 0x126c/0x1268 */
	int		   depth_write;	/* 0x12d0 bit 8 */
	int		   stencil_test;
	enum p3d_cmp	   stencil_func;
	enum p3d_stencil_op stencil_fail, stencil_zfail, stencil_zpass;
	uint8_t		   stencil_ref, stencil_value_mask, stencil_write_mask;
	/* alpha test, 0x12e4: [0] enable, [3:1] compare, [15:8] reference.
	 * GL's 0..1 reference scales to 0..255 (0.5 was captured as 0x80). */
	int		   alpha_test;
	enum p3d_cmp	   alpha_func;
	uint32_t	   alpha_ref;	/* 0..255 */
	int		   blend;	/* 0x12e8 bit 0 */
	struct p3d_blend_factor blend_src, blend_dst;
	enum p3d_blend_eq  blend_eq;
	/* Blend constants are two 10-bit unsigned components per register:
	 * RG in 0x12ec and BA in 0x12f0. */
	uint32_t	   blend_const_rg, blend_const_ba;
	int		   dither;
	/* GL_COLOR_LOGIC_OP.  The raster op is DWGCTL bits [7:4], a 4-bit truth
	 * table, and Gallium's pipe_logicop values already are that encoding:
	 * PIPE_LOGICOP_COPY is 12, the 0xC the engine defaults to.  Blending is
	 * honoured only at 0xC, which is also GL's rule that an enabled logic op
	 * replaces blending (parhelia-accel.md "ROP encoding",
	 * parhelia-blend-alpha-encoding.md section 198). */
	int		   logicop_enable;
	unsigned	   logicop;
	/* A point wider than one pixel.  The extent reaches the setup unit as
	 * WARP output o10.z (parhelia-point-size-re.md, confirmed here: a unit
	 * point disappears once o10.z exceeds one).  Which raster image states
	 * a wide point is still open, so the three registers that select it
	 * stay separate until measured: class 3 alone draws nothing.  Zero in
	 * any of them keeps the unit-point choice. */
	/* The hardware fog unit.  The vertex program states the fog factor in
	 * o2.w, interpolant 5 is sourced from o2 (0x1114 nibble 5, already 2 in
	 * the value this driver writes), 0x1110 bit 5 turns that interpolant
	 * on, and 0x12d4 carries the enable, the mode and the fog colour:
	 * [0] enable, [2:1] mode, [13:6] blue, [22:15] green, [31:24] red, each
	 * (int)(c * 255 + 0.5).  0x12d8 and 0x12dc are start and end as
	 * value * 16777215, and 0x00ffffff in 0x12dc is the off state.  Decoded
	 * byte-exact from the vendor's own GL_FOG capture
	 * (parhelia-pixel-pipeline-complete.md section 3.6, confirmed).
	 * No combiner slot is used: the unit blends after the combiner. */
	/* Extra bits ORed into 0x1110.  The vendor's own low byte is 0x27 and
	 * only bits [11:8] are decoded here, so an output a program adds
	 * beyond the fetched attributes may need advertising. */
	uint32_t	   attr_present_extra;
	/* Extra bits ORed into 0x1130.  The vendor's own control words carry
	 * 0x300c where this driver writes 0x200c, so bit 12 is set in paths
	 * where we clear it; our map calls it flat shading. */
	uint32_t	   pipectl_extra;
	/* 0x1144, which the vendor emits in the same packet as 0x111c and this
	 * driver hardcodes to zero.  A candidate for the word that says which
	 * vertex outputs the setup unit consumes. */
	uint32_t	   vertex_out_extra;
	int		   fog;
	unsigned	   fog_mode;		/* 0 linear, 1 and 2 exponential */
	float		   fog_colour[4];
	float		   fog_start, fog_end;
	/* 64 dwords streamed to the 0x12e0 table port.  A unit whose table was
	 * never written is one candidate for fog having no effect at all. */
	const uint32_t	  *fog_table;
	/* A second colour attribute (kind 6, WARP input v6).  The combiner
	 * reads it as v1, which is where a lowered GL fog expression expects
	 * its fog coordinate; it costs three more dwords per vertex. */
	int		   vertex_colour1;
	unsigned	   point_class;
	unsigned	   point_nibble;
	uint32_t	   point_rastermode;
	/* GL_LINE_STIPPLE.  0x1028 carries (repeat << 16) | pattern, where the
	 * repeat is GL's factor and the pattern its sixteen bits, bit 0 first
	 * (parhelia-fragment-ops.md section 2, CONFIRMED).  A disabled stipple
	 * is the solid pattern at repeat one, which is the reset state. */
	int		   line_stipple_enable;
	unsigned	   line_stipple_pattern;	/* 16 bits */
	unsigned	   line_stipple_repeat;		/* GL factor, 1..256 */
	/* Diagnostic: one extra register write emitted with the raster state,
	 * so a candidate register can be measured on hardware without a new
	 * builder.  Zero emits nothing.  Never set by Gallium. */
	uint32_t	   probe_reg, probe_val;
	/* 0x1324 colour write mask, one byte per channel in the render
	 * target's own order: B [7:0], G [15:8], R [23:16], A [31:24].
	 * glColorMask(1,0,1,1) was captured as 0xffff00ff.  All four clear
	 * means all channels on unless mask_explicit asks for no colour writes. */
	int		   mask_r, mask_g, mask_b, mask_a;
	int		   mask_explicit;
	/* Transform.  With a transforming WARP program the vertices arrive in
	 * clip space and the program reads the matrix from the constant file:
	 * c0..c3 are the MVP rows (c[r] = M[r], M[r+4], M[r+8], M[r+12]), c8 is
	 * the viewport scale (W/2, -H/2, (zf-zn)/2, 1) and c9 the offset
	 * (x0+W/2, y0+H/2, (zn+zf)/2, H).  The raster/clip block reads the same
	 * pair again at c[0x160]/c[0x161]. */
	int		   transform;
	/* The block loads are executed BY a WARP program: the vendor binds the
	 * kind-0 constant-file loader (mtx1079) with MODE 0x20000028 right
	 * before them, and that loop copies each block into c[] through
	 * destination file 3.  Without the bind the loads go nowhere and the
	 * engine stops retiring. */
	uint32_t	   const_loader_entry;
	/* The loader loop indexes through a0, and the kind-1 program (mtx251)
	 * is what latches a0 = (0,1,2,3) and the base registers b0..b3 from
	 * its own block load.  Without it a0 is whatever the last context left
	 * and the loader's loop need not terminate - which reads as an engine
	 * that stops retiring rather than as a dropped draw. */
	uint32_t	   base_reg_entry;
	uint32_t	   clipper_entry;	/* v3.x of the kind-1 load */
	unsigned	   const_mask;
	/* fixed-function directional light: c4 = light direction (normalised,
	 * in the same space as the vertex normals), c5 = material colour.
	 * The lighting program computes max(0, N.L) per vertex and the setup
	 * unit interpolates it, which is Gouraud lighting. */
	int		   light;
	float		   light_dir[4], material[4];
	/* c6: the ambient term, so the program computes GL's
	 * globalAmbient*C + C*max(0,N.L) rather than the diffuse alone */
	float		   ambient[4];	/* which loads to emit: 1 mvp, 2 c8/c9, 4 c[0x160]; 0 = all */
	const float	  *mvp;		/* c0..c3 as 16 floats, i.e. already
					 * transposed from a GL column-major
					 * matrix: c[r][k] = M[r + 4*k] */
	/* A compiled vertex program brings its own constant block instead of
	 * the fixed-function matrix: @vs_const_vec4s vectors loaded at c0, and
	 * the viewport pair at @vp_slot / @vp_slot + 1 rather than c8/c9. */
	const float	  *vs_const;
	unsigned	   vs_const_vec4s;
	/* A further block at an arbitrary slot, for constants the fixed setup
	 * reads rather than the program: the vendor keeps the point size at
	 * c[0x172].w and its clamp at c[0x173] (parhelia-point-size-re.md).
	 * Selected by const_mask bit 3. */
	const float	  *aux_const;
	unsigned	   aux_const_vec4s;
	uint32_t	   aux_const_slot;
	unsigned	   vp_slot;
	float		   vp_scale[4], vp_offset[4];
	/* descriptor overrides, for probing the attribute port: 0 = use the
	 * built-in encoding.  The component-count field is documented two ways
	 * ([25:22] as a mask and [28:24] as count-1) and only hardware settles
	 * which a three-component position needs. */
	uint32_t	   pos_desc, col_desc;
	/* The documented RGBA colour descriptor has only been observed in the
	 * isolated alpha-blend diagnostic.  Keep it opt-in until that diagnostic
	 * has demonstrated the required WARP/attribute routing on hardware. */
	int		   vertex_alpha;
	/* A depth-bound RGBA stream has a different descriptor and source stride.
	 * The batch validator accepts it only for the verified one-texture,
	 * MODULATE, source-alpha, depth-test shape. */
	int		   depth_vertex_alpha;
	/* Source-alpha ADD with destination ONE, selected only by the narrow
	 * depth/RGBA Q3 predicate.  This distinguishes vendor factor pair 5,2
	 * (SRC_ALPHA, ONE) from the alpha-over destination factor. */
	int		   depth_vertex_alpha_add;
	/* Test harness escape hatch for the isolated depth+RGBA+texture-alpha
	 * cutout experiment.  Mesa never sets it; the batch validator limits it to
	 * GREATER .5 alpha-test with depth enabled and no blending. */
	int		   depth_alpha_test_probe;
	/* Test-only texture-cache partition override.  Zero preserves the normal
	 * context state; the isolated RGBA cutout probe uses the vendor one-stage
	 * image (0x00500000) while tracing texture-alpha propagation. */
	uint32_t	   texture_cache_probe;
	/* The texture-cache partition latch 0x11e8 belongs to the LX family
	 * (0x2537/0x2538/0x2539) alone.  The original Parhelia's vendor drivers
	 * never write it on any path, and a write latches this chip's texture
	 * engine: the draw stops retiring and the engine has to be reset
	 * (parhelia-gl-features.md "zeroes the draw and latches the engine. No
	 * capture ever writes it; do not write it", engine-context-bringup 17.1).
	 * The 0528-proven emitter in xf86-video-parhelia/src/parhelia3d never
	 * writes it either.  texture_cache_probe still forces a write, so the
	 * latch stays reproducible on purpose. */
	int		   tex_cache_latch;
	unsigned	   depth_slot;	/* surface slot for Z; 1 = 0x1228 */
	uint32_t	   screen_w, screen_h;
	int		   scissor;
	uint16_t	   scissor_minx, scissor_miny;
	uint16_t	   scissor_maxx, scissor_maxy;
	enum p3d_prim	   prim;
	int		   flat;
	int		   cull;	/* 0x1130[1:0]: 0 none, 1 cull CW, 2 cull CCW */
	uint32_t	   warp_entry;
	unsigned	   dw_per_vertex;
	/* A compiled fragment program.  When set it replaces the fixed-function
	 * texenv ALU stages; the pipe-merge slots and every sampler descriptor
	 * are unchanged.  Ignored for the sampled-alpha companion and RGBA
	 * vertex-alpha shapes, which own their own combiner words. */
	const struct p3d_comb_program *comb;
	/* Diagnostic: first instruction index of the compiled program in the
	 * slot walk.  Zero uses the default, which is right after the pipe
	 * merges. */
	unsigned	   comb_slot_base;
	const struct p3d_texture *tex;	/* unit 0 */
	const struct p3d_texture *tex1;	/* unit 1; NULL = single unit */	/* NULL = untextured */
	/* Units 2 and 3 exist on the original Parhelia (0x0527/0x0528), which
	 * has four texture units; the LX family has two and leaves these NULL.
	 * The units must be contiguous: a unit is active only if every lower
	 * one is.  parhelia-multitexture.md section 1 has the register model -
	 * 0x11c0..0x11e4 is one window that 0x1200 + unit*4 banks. */
	const struct p3d_texture *tex2;	/* unit 2; original family only */
	const struct p3d_texture *tex3;	/* unit 3; original family only */
	/* P690 workaround proven by the RGB-companion cutout probe: preserve
	 * unit-0 RGB and replicate unit-1 red into fragment alpha. */
	int		   sampled_alpha_companion;
	/* Diagnostic only: fetch a second two-component coordinate into v8 and
	 * advertise sets 0/1 while keeping a single sampler/combiner stage.  This
	 * separates the multitexture WARP program's o4 output from unit-1 texture
	 * state.  Gallium never sets this field. */
	int		   second_texcoord_probe;
	/* The vertex carries a three-component coordinate set 0 (s, t, r), which
	 * a volume or cube-map sampler needs.  The attribute port has room for
	 * the third component only where the vertex colour would sit, so this
	 * shape drops the colour attribute; the combiner takes its primary
	 * colour from the WARP replace program's constant instead. */
	int		   texcoord3;
	/* Coordinate set 0 carries four components, s,t,r,q, for the projective
	 * coordinate mode; later attributes move two dwords on. */
	int		   texcoord_q;
	/* The position carries 1/w as a fourth component (descriptor mask
	 * xyzw), and every later attribute sits one dword further on. */
	int		   oow;
	/* 1 selects the P690-proven single live pipe.  Value 2 is diagnostic
	 * only: it additionally retains the vendor's disabled unit-1 pipe mask
	 * (0x0001000c), while issuing no second sampler descriptor. */
	int		   single_texture_pipe;
	/* Vendor LX-class pipe allocation (parhelia-p690-texture-pipes-re.md,
	 * parhelia-p690-ganged-pipe-ta-re.md): one stage ganged on pipes 0+2
	 * and merged in combiner slot 0, leftover pipes parked on the next
	 * unit, 0x11e8 written per unit bank.  Needs single_texture_pipe. */
	int		   lx_pipe_route;
	/* The LX family (0x2537, 0x2538, 0x2539).  The vendor clamps the raster
	 * guard band one unit below the value every other part uses on exactly
	 * this family (KRN mtx2572: 32.0f -> 31.0f, 16.0f -> 15.0f), which is
	 * the one proven LX-only value in the geometry path. */
	int		   lx_family;
	/* Select the P690 mode-3 frontend.  It retains one packet per triangle;
	 * hardware testing proved that the packet boundary supplies the depth
	 * interlock which a second range in the same packet does not. */
	int		   bound_ranges;
	/* Vertices consumed by one draw packet.  Zero keeps one primitive per
	 * packet (three vertices for a triangle list).  A larger count draws
	 * several primitives, or a strip, fan or quad run, from one packet. */
	unsigned	   vertices_per_packet;
	uint32_t	   fence;	/* first fence tag; steps by 8 per draw */
};

/* Emit the whole per-batch state.  Call once, then p3d_emit_draw per
 * primitive. */
/* the 2D solid fill; must be emitted BEFORE p3d_emit_state, which rebinds the
 * surface slots the fill overwrites.  Z24 is unsigned normalised: far = 0x00ffffff
 * write_mask is PLNWT: destination bits outside it keep their old value. */
#define P3D_ZCLEAR_FAR	0x00ffffffu
#define P3D_ZCLEAR_NEAR	0x00000000u
#define P3D_DWGCTL_SOLID	0xc0u	/* opaque 2-D solid fill; LEN fires it */
void	 p3d_emit_clear(struct p3d_cs *cs, const struct p3d_surface *s,
			uint32_t value, uint32_t write_mask, uint32_t x,
			uint32_t y, uint32_t w, uint32_t h);
void	 p3d_emit_blit(struct p3d_cs *cs, const struct p3d_surface *dst,
		       unsigned dst_type, uint32_t dst_x, uint32_t dst_y,
		       const struct p3d_surface *src, unsigned src_type,
		       uint32_t src_x, uint32_t src_y, uint32_t w, uint32_t h);
void	 p3d_emit_clear_frame(struct p3d_cs *cs, const struct p3d_state *st,
			      uint32_t colour, uint32_t depth);

/* Vendor Z-tracking / Z-init one-shot DWGCTL words, recovered in
 * docs/parhelia-p690-zcompression-clear-banks-re.md section 2.4.  Each is
 * written to DWGCTL and immediately followed by the running draw word, so the
 * command acts once. */
#define P3D_ZTRACK_OFF	0x0000000au
#define P3D_ZTRACK_ON	0x0000020au
#define P3D_ZINIT_LOAD	0x00000208u	/* set every tracked tile to 0x12b4 */
#define P3D_ZINIT_RESET	0x00000308u	/* discard the tracked tile state */
/* Bind @s as the depth surface in slot @slot and issue one Z control word,
 * optionally preceded by the Z-init value.  @resume is the draw word the
 * hardware returns to. */
void	 p3d_emit_depth_control(struct p3d_cs *cs, const struct p3d_surface *s,
				unsigned slot, uint32_t control, int load,
				uint32_t value, uint32_t resume);
void	 p3d_emit_state(struct p3d_cs *cs, const struct p3d_state *st);

/* Emit one draw.  index is the draw's ordinal in this batch and selects its
 * fence tag; the vertex range is left as zeros for the submitter to patch. */
void	 p3d_emit_draw(struct p3d_cs *cs, const struct p3d_state *st,
		       unsigned index);
/* nprim draw packets, one primitive each - the vendor's own cadence */
void	 p3d_emit_draws(struct p3d_cs *cs, const struct p3d_state *st,
			unsigned nprim);
void	 p3d_emit_bound_ranges(struct p3d_cs *cs, const struct p3d_state *st,
			       unsigned nprim);

/* Derived values, exposed because a driver wants them for validation. */
uint32_t p3d_primword(enum p3d_prim prim);

/* The texture bound to unit @u, or NULL.  Units are contiguous, so the first
 * NULL ends the active set. */
static inline const struct p3d_texture *
p3d_state_unit(const struct p3d_state *st, unsigned u)
{
	switch (u) {
	case 0:  return st->tex;
	case 1:  return st->tex1;
	case 2:  return st->tex2;
	case 3:  return st->tex3;
	default: return NULL;
	}
}

/* How many contiguous texture units this state drives, 0 when untextured. */
static inline unsigned
p3d_state_units(const struct p3d_state *st)
{
	unsigned u = 0;

	while (u < P3D_MAX_TEXTURE_UNITS && p3d_state_unit(st, u))
		u++;
	return u;
}
uint32_t p3d_prim_class(enum p3d_prim prim);
unsigned p3d_vertices_per_primitive(enum p3d_prim prim);
/* Smallest vertex count that draws one primitive of this kind. */
unsigned p3d_minimum_vertices(enum p3d_prim prim);
uint32_t p3d_surface_cfg(const struct p3d_surface *s);
uint32_t p3d_blendctl(const struct p3d_state *st);
uint32_t p3d_dwgctl(const struct p3d_state *st);
/* DWGCTL nibble 6/7 is the DWGSYNC family; 0x307 drains the pixel pipeline */
#define P3D_DWGCTL_SYNC	0x00000307u
void	 p3d_raw(struct p3d_cs *cs, const uint32_t *dw, size_t n);
void	 p3d_emit_constants(struct p3d_cs *cs, uint32_t first_slot,
			    const float *vec4s, size_t nvec);
void	 p3d_emit_constant_group(struct p3d_cs *cs, const struct p3d_state *st,
				 const float *vp);
void	 p3d_viewport(struct p3d_state *st, float x0, float y0,
		      float w, float h, float zn, float zf);
size_t	 p3d_warp_transform(uint32_t *out, size_t cap);
/* A transforming program reads an object-space position, and a position
 * carrying 1/w has z before it, so both carry z even when no depth buffer is
 * bound. */
static inline int p3d_position_has_z(const struct p3d_state *st)
{
	return st->has_depth || st->transform || st->oow;
}
size_t	 p3d_warp_lighting(uint32_t *out, size_t cap);
/* GL_REPLACE: writes a constant white colour so MUL(TEX, PREV) passes TEX */
size_t	 p3d_warp_replace(uint32_t *out, size_t cap);
uint32_t p3d_vtxcap(unsigned dw_per_vertex);

/* ----------------------------------------------------------- indexed draws
 *
 * The hardware has no index buffer in the ring: every draw packet is a byte
 * RANGE of consecutive vertices (opcode 0x97, operands start/end, memory class
 * in 0x1000).  That is not an assumption - the vendor's own captures settle it.
 * GL_QUADS and GL_QUAD_STRIP are decomposed into an INDEXED triangle list one
 * level up in the vendor ICD, and GL_LINE_LOOP appends its closing segment as a
 * packed 16-bit index pair, yet the quads, quadstrip, polygon and lineloop
 * captures contain 0x97 range draws and nothing else.  The indices are resolved
 * before the stream is built.
 *
 * So glDrawElements is served by expanding the indices into a linear vertex
 * stream, which is what p3d_deindex does.  out and verts may not overlap; the
 * return is the number of dwords written, or 0 if out_cap is too small or an
 * index is out of range.  index_bytes is 1, 2 or 4 and indices is read
 * unaligned, so a GL client's own array can be passed straight in.
 */
size_t	 p3d_deindex(uint32_t *out, size_t out_cap,
		     const uint32_t *verts, size_t nvert,
		     unsigned dw_per_vertex,
		     const void *indices, unsigned index_bytes, size_t count);

/* ------------------------------------------------------------------ tiling
 *
 * The colour target is stored in 64x32 pixel tiles of 8192 bytes with a
 * bit-swizzled in-tile index, and bit 7 of that index is XORed with the tile
 * ROW parity.  Tiles per row = pitch / 64.  Verified against a known-geometry
 * 3D draw: a 500x300 rectangle maps back to exactly 500x300 at its true
 * origin.
 */
uint32_t p3d_tile_offset(uint32_t x, uint32_t y, uint32_t pitch);
void	 p3d_tile_position(uint32_t offset, uint32_t pitch,
			   uint32_t *x, uint32_t *y);
uint32_t p3d_tile_offset_layout(uint32_t x, uint32_t y, uint32_t pitch,
				uint32_t layout);
void	 p3d_tile_position_layout(uint32_t offset, uint32_t pitch,
			  uint32_t layout, uint32_t *x, uint32_t *y);
/* The measured 16 bpp render-target order: 128x16 pixels per 4 KiB tile. */
uint32_t p3d_tile_offset_16bpp(uint32_t x, uint32_t y, uint32_t pitch);
void	 p3d_tile_position_16bpp(uint32_t offset, uint32_t pitch,
				 uint32_t *x, uint32_t *y);
uint32_t p3d_tile_offset_layout_cpp(uint32_t x, uint32_t y, uint32_t pitch,
				    uint32_t layout, unsigned cpp);
void	 p3d_tile_position_layout_cpp(uint32_t offset, uint32_t pitch,
				      uint32_t layout, unsigned cpp,
				      uint32_t *x, uint32_t *y);
/* Texture layout code 2: 32x16 texels per 2 KiB block at 32 bpp.  This is
 * deliberately not p3d_tile_offset(): texture and render-target swizzles are
 * different on Parhelia. @pitch is the padded texture width in texels.
 */
uint32_t p3d_texture_tile_offset(uint32_t x, uint32_t y, uint32_t pitch);
/* Texture layouts 2 and 3 share the 32x16 block order, but their texel
 * widths differ.  The 16-bit A1R5G5B5 route uses code 3. */
uint32_t p3d_texture_tile_offset_cpp(uint32_t x, uint32_t y, uint32_t pitch,
                                    unsigned cpp);
/* P690 32-bit texture sampling order; differs from the 64x16 render-target
 * order in y bits 0 and 1. */
uint32_t p3d_texture_tile_offset_64x16(uint32_t x, uint32_t y, uint32_t pitch);
uint32_t p3d_texture_tile_offset_64x16_at(uint32_t base, uint32_t x, uint32_t y,
					  uint32_t pitch);
/* The measured P690 order for 8- and 16-bit textures. */
uint32_t p3d_texture_tile_offset_p690_narrow(uint32_t x, uint32_t y,
					     uint32_t pitch, unsigned cpp);
bool p3d_texture_uses_p690_narrow(uint32_t chip_id, uint32_t tile_layout,
				  unsigned cpp);
uint32_t p3d_texture_tile_offset_64x32(uint32_t x, uint32_t y, uint32_t pitch);
bool p3d_texture_uses_native_64x32(uint32_t chip_id, uint32_t tile_layout,
				   unsigned cpp);
bool p3d_texture_uses_native_64x16(uint32_t chip_id, uint32_t tile_layout,
                                  unsigned cpp);
uint32_t p3d_texture_tile_offset_chip(uint32_t x, uint32_t y, uint32_t pitch,
                                     unsigned cpp, uint32_t chip_id,
                                     uint32_t tile_layout);

/* ------------------------------------------------------------------ WARP */

/* Encode the passthrough vertex program: window-space position through to the
 * setup unit, colour to o1, position copied to o11 for clipping, and with
 * texcoords when the vertex carries them.  Returns the instruction count;
 * each instruction is three dwords. */
size_t	 p3d_warp_passthrough(uint32_t *out, size_t cap, int textured);

/* ------------------------------------------------------------------ submit */

struct p3d_ops {
	void	 (*wr32)(void *dev, uint32_t reg, uint32_t val);
	uint32_t (*rd32)(void *dev, uint32_t reg);
	void	 (*barrier)(void *dev);
	void	  *dev;
};

/* ------------------------------------------------------------------ init
 *
 * The vendor HAL first-start sequence ([XDRV] mtx1373) brings a cold engine
 * up: on a genuine power cycle with no vendor code loaded, running this takes
 * the 2D engine to 120000/120000 and lets the whole 3D stream be consumed.
 *
 * Two of its steps are not plain register writes and need the VBIOS PInS
 * block, so the caller supplies an accessor: step 7 (0x1c50) and step 12 (the
 * memory controller and system clock).  A driver that already parses PInS for
 * modeset passes its own in; without it those two steps are skipped, which is
 * what every cold bring-up test used.
 *
 * Display registers are deliberately NOT here.  The PLL trio, the head enable
 * and the DC clock bits need their own programming protocol - copying values
 * from a running vendor driver takes the 2D instrument from 120000 px to 0 -
 * and the open driver's modeset path already implements them.
 */
struct p3d_init_env {
	uint32_t (*pins32)(void *priv, uint32_t offset);  /* VBIOS PInS, or NULL */
	void	 (*udelay)(void *priv, unsigned us);
	void	  *priv;

	/* HAL step 7 writes 0x1c50 "from PInS" ([XDRV] 0xddceb) and WHICH PInS
	 * offset it reads has not been identified, so this is a value the
	 * caller supplies rather than an offset we look up.  Leave has_1c50
	 * clear to skip the step, which is what every cold bring-up did. */
	uint32_t reg_1c50;
	int	 has_1c50;
};

#define P3D_INIT_MEMCLK		0x1u	/* run step 12; takes the display down */
#define P3D_INIT_APERTURE	0x2u	/* run steps 17-21; RESETS a machine
					 * whose console scanout runs through
					 * the apertures it disarms */

int	 p3d_engine_init(const struct p3d_ops *ops,
			 const struct p3d_init_env *env,
			 uint32_t devid, unsigned flags);
void	 p3d_apply_vid_reset(const struct p3d_ops *ops,
			     const struct p3d_init_env *env, int head);

/* 0x400 holds the bus address of the shared status page.  Cold it reads 0 and
 * the DWGSYNC drain and surface fences cannot retire without it. */
void	 p3d_arm_writeback(const struct p3d_ops *ops, uint32_t bus_page);

/* Ring the doorbell for [start,end) as bus addresses and wait for the fence to
 * reach `tag`.  0x408 carries the DMA mode in its low two bits; without them
 * the engine does not fetch.  Returns 0 on success, -1 on timeout. */
int	 p3d_submit(const struct p3d_ops *ops, uint32_t bus_start,
		    uint32_t bus_end, uint32_t tag, unsigned timeout_us);

/* Continue the fence from whatever last retired.  A submission that restarts
 * the counter at its stream's built-in base re-arms a tag the engine has
 * already passed, and the batch then never completes - which latched the
 * engine after a few draws until this was understood. */
uint32_t p3d_fence_next(const struct p3d_ops *ops);

#endif /* PARHELIA3D_H */
