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
#define P3D_R_EOBTAG		0x12b8	/* end-of-batch, constant 0x00070601 */
#define P3D_R_FENCE		0x12bc	/* batch fence COUNTER, steps by 8 */
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
 * A draw packet is a byte range of consecutive vertices.  It can contain more
 * than one primitive (the vendor emits 24-vertex / eight-triangle packets),
 * but overlapping primitives must be split: depth interlock is absent within
 * one packet.  The submitter decides the safe range boundaries.
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
	uint32_t fmt;		/* 0x32 = A8R8G8B8, 0x36 = S8Z24 */
};

/* 0x11c0 FILTER: MIN [1:0], MAG [4:3], MIP [7:6], bit 12 always set,
 * LOD bias [24:16] signed 4.4 biased -4.0.  0x01c01000 = nearest, no mip.
 * 0x11c4 WRAP: S [2:0], T [5:3], R [8:6], LOD min [23:16], max [31:24].
 * 0xf0000000 = repeat, LOD 0..15.  Codes measured from the capture:
 * texlin gives filter 0x01c01012 (MIN=MAG=2) and texclamp wrap 0xf0000036
 * (S=T=6). */
enum p3d_filter { P3D_FILTER_NEAREST = 0, P3D_FILTER_LINEAR = 2,
		  P3D_FILTER_ANISO = 3 };
enum p3d_mip	{ P3D_MIP_NONE = 0, P3D_MIP_NEAREST = 1, P3D_MIP_LINEAR = 2 };
enum p3d_wrap	{ P3D_WRAP_REPEAT = 0, P3D_WRAP_CLAMP = 6 };

/* GL_TEXTURE_ENV_MODE.  MODULATE is MUL(TEX, PREV); REPLACE is MOV(TEX), which
 * on this hardware means the alpha combiner slot 0x1260 goes to zero, 0x1268
 * stays at its resting 0x01400400, and the vertex colour attribute is not
 * fetched at all - the capture's texrepl has 0x1110 = 0x101 where MODULATE has
 * 0x103. */
enum p3d_texenv { P3D_TEXENV_MODULATE = 0, P3D_TEXENV_REPLACE = 1,
		  P3D_TEXENV_ADD = 2, P3D_TEXENV_DECAL = 3,
		  P3D_TEXENV_BLEND = 4 };

/* A combiner constant register holds two signed Q1.10 values in bits [10:0]
 * and [26:16].  GL_TEXTURE_ENV_COLOR for P3D_TEXENV_BLEND is c0, packed as
 * RG and BA.  Mesa converts/clamps floats to [-1024, 1023] before using this
 * macro; keeping the builder integer-only makes it usable in a kernel too. */
#define P3D_COMBINER_CONST(lo, hi) \
	(((uint32_t)(lo) & 0x7ffu) | (((uint32_t)(hi) & 0x7ffu) << 16))

struct p3d_texture {
	uint32_t base, width, height;
	uint32_t fmt;		/* 0x32 A8R8G8B8, 0x33 X8R8G8B8, 0x11 L8 */
	uint32_t texelcode;	/* 0x11e0[18:16]: log2(16 / bytes-per-texel) */
	uint32_t filter, wrap;		/* raw override; 0 = build from below */
	enum p3d_filter min_filter, mag_filter;
	enum p3d_mip	mip_filter;
	enum p3d_wrap	wrap_s, wrap_t;
	unsigned	max_level;	/* 0x11e4[31:28] */
	enum p3d_texenv	env;
	uint32_t	env_colour_rg, env_colour_ba;	/* P3D_COMBINER_CONST pairs */
};

uint32_t p3d_tex_filter(const struct p3d_texture *t);
uint32_t p3d_tex_wrap(const struct p3d_texture *t);

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
#define P3D_Z_LESS	P3D_CMP_LESS
#define P3D_Z_GEQUAL	P3D_CMP_GEQUAL
typedef enum p3d_cmp p3d_zfunc_t;

struct p3d_state {
	struct p3d_surface colour;
	struct p3d_surface depth;	/* .base 0 = none */
	int		   has_depth;	/* bind the Z surface */
	int		   depth_test;	/* run the compare */
	enum p3d_cmp	   depth_func;
	uint32_t	   depth_raw;	/* whole 0x12c8 value; overrides the above */
	uint32_t	   comb_op;	/* probe: opcode base OR'd into 0x126c/0x1268 */
	int		   depth_write;	/* 0x12d0 bit 8 */
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
	/* 0x1324 colour write mask, one byte per channel in the render
	 * target's own order: B [7:0], G [15:8], R [23:16], A [31:24].
	 * glColorMask(1,0,1,1) was captured as 0xffff00ff. */
	int		   mask_r, mask_g, mask_b, mask_a;
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
	unsigned	   depth_slot;	/* surface slot for Z; 1 = 0x1228 */
	uint32_t	   screen_w, screen_h;
	enum p3d_prim	   prim;
	int		   flat;
	int		   cull;	/* 0x1130[1:0]: 0 none, 1 cull CW, 2 cull CCW */
	uint32_t	   warp_entry;
	unsigned	   dw_per_vertex;
	const struct p3d_texture *tex;	/* unit 0 */
	const struct p3d_texture *tex1;	/* unit 1; NULL = single unit */	/* NULL = untextured */
	/* P690 workaround proven by the RGB-companion cutout probe: preserve
	 * unit-0 RGB and replicate unit-1 red into fragment alpha. */
	int		   sampled_alpha_companion;
	/* Diagnostic only: fetch a second two-component coordinate into v8 and
	 * advertise sets 0/1 while keeping a single sampler/combiner stage.  This
	 * separates the multitexture WARP program's o4 output from unit-1 texture
	 * state.  Gallium never sets this field. */
	int		   second_texcoord_probe;
	/* 1 selects the P690-proven single live pipe.  Value 2 is diagnostic
	 * only: it additionally retains the vendor's disabled unit-1 pipe mask
	 * (0x0001000c), while issuing no second sampler descriptor. */
	int		   single_texture_pipe;
	/* Select the P690 mode-3 frontend.  It retains one packet per triangle;
	 * hardware testing proved that the packet boundary supplies the depth
	 * interlock which a second range in the same packet does not. */
	int		   bound_ranges;
	uint32_t	   fence;	/* first fence tag; steps by 8 per draw */
};

/* Emit the whole per-batch state.  Call once, then p3d_emit_draw per
 * primitive. */
/* the 2D solid fill; must be emitted BEFORE p3d_emit_state, which rebinds the
 * surface slots the fill overwrites.  Z24 is unsigned normalised: far = 0x00ffffff */
#define P3D_ZCLEAR_FAR	0x00ffffffu
#define P3D_ZCLEAR_NEAR	0x00000000u
void	 p3d_emit_clear(struct p3d_cs *cs, const struct p3d_surface *s,
			uint32_t value, uint32_t x, uint32_t y,
			uint32_t w, uint32_t h);
void	 p3d_emit_blit(struct p3d_cs *cs, const struct p3d_surface *dst,
		       unsigned dst_type, uint32_t dst_x, uint32_t dst_y,
		       const struct p3d_surface *src, unsigned src_type,
		       uint32_t src_x, uint32_t src_y, uint32_t w, uint32_t h);
void	 p3d_emit_clear_frame(struct p3d_cs *cs, const struct p3d_state *st,
			      uint32_t colour, uint32_t depth);
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
uint32_t p3d_prim_class(enum p3d_prim prim);
unsigned p3d_vertices_per_primitive(enum p3d_prim prim);
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
/* Texture layout code 2: 32x16 texels per 2 KiB block at 32 bpp.  This is
 * deliberately not p3d_tile_offset(): texture and render-target swizzles are
 * different on Parhelia. @pitch is the padded texture width in texels.
 */
uint32_t p3d_texture_tile_offset(uint32_t x, uint32_t y, uint32_t pitch);
/* Texture layouts 2 and 3 share the 32x16 block order, but their texel
 * widths differ.  The 16-bit A1R5G5B5 route uses code 3. */
uint32_t p3d_texture_tile_offset_cpp(uint32_t x, uint32_t y, uint32_t pitch,
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
