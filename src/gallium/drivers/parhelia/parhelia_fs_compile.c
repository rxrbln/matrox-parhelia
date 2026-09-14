/*
 * NIR -> Parhelia fragment-combiner compiler.
 *
 * The combiner is a three-operand ALU.  Each slot co-issues an RGB and an
 * alpha operation over four texture pipes, two temporaries (r0 is the
 * fragment colour), two interpolated colours and two constants private to the
 * slot.  A register's RGB and alpha halves are separate storage, so the
 * program is built as two lanes.
 *
 * Every lane of the colour output is expanded into a polynomial over the
 * values that are no sums or products - samples, colours, constants and other
 * operations - stopping at values with more than one use, so those are
 * computed once.  Instructions are selected from the polynomial, where LRP,
 * MAD, the source modifiers and the result scale absorb the forms GL's fixed
 * function, mix() and Mesa's flrp lowering take.  The nodes of both lanes are
 * then zipped into slots and registers assigned from their live ranges.
 *
 * Encoding and evidence: docs/parhelia-fragment-isa.md,
 * docs/parhelia-p690-combiner-isa-re.md.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parhelia_fs_compile.h"

#define PH_FS_POLY_TERMS 8u
#define PH_FS_TERM_ATOMS 3u
#define PH_FS_LIVE_TO_END 0x7fffffff

enum { PH_LANE_RGB = 0, PH_LANE_A = 1, PH_LANE_BOTH = 2 };
enum { PH_SRC_NONE, PH_SRC_NODE, PH_SRC_TEX, PH_SRC_COLOUR, PH_SRC_CONST };

/* One operand.  @channel is -1 for a natural read, otherwise the channel read
 * (replicated across RGB in the RGB lane). */
struct ph_fs_src {
   uint8_t kind;
   uint8_t mod;
   int8_t channel;
   uint16_t index;
};

/* A constant operand: the three RGB channels or the alpha channel, each a
 * literal or a scalar of the fragment constant buffer. */
struct ph_fs_part {
   uint8_t lane;
   uint8_t literal;
   uint16_t offset[3];
   float value[3];
};

struct ph_fs_node {
   nir_def *def;
   uint8_t lane, op, scale, sat;
   uint8_t output;      /* lanes the fragment colour takes from it */
   bool memo;
   struct ph_fs_src src[3];
   unsigned uses, refs;
   int slot;
   uint8_t reg;
   int end[2];          /* last slot reading each storage lane */
   bool end_cross[2];   /* that read comes from the other half */
};

struct ph_fs_atom {
   nir_def *def;
   int8_t lane, sel;
};

struct ph_fs_term {
   float coef;
   unsigned n;
   uint16_t atom[PH_FS_TERM_ATOMS];
};

struct ph_fs_poly {
   unsigned n;
   struct ph_fs_term term[PH_FS_POLY_TERMS];
};

struct ph_fs_memo {
   nir_def *def;
   int8_t lane, sel;
   struct ph_fs_src src;
};

struct ph_fs_ctx {
   struct ph_fs_node *node;
   unsigned nodes, node_cap;
   struct ph_fs_part *part;
   unsigned parts, part_cap;
   struct ph_fs_atom *atom;
   unsigned atoms, atom_cap;
   struct ph_fs_memo *memo;
   unsigned memos, memo_cap;
   int tex_end[4][2];
   bool tex_end_cross[4][2];
   unsigned sampler_mask;
   bool uses_color, uses_color1;
   const char *reject;
};

static bool ph_fs_lower(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
                        struct ph_fs_src *out);

static bool
ph_fs_grow(void **array, unsigned count, unsigned *cap, size_t size)
{
   unsigned want;
   void *grown;

   if (count < *cap)
      return true;
   want = *cap ? *cap * 2 : 16;
   grown = realloc(*array, want * size);
   if (!grown)
      return false;
   *array = grown;
   *cap = want;
   return true;
}

#define PH_FS_GROW(ctx, field, count, cap) \
   ph_fs_grow((void **)&(ctx)->field, (ctx)->count, &(ctx)->cap, \
              sizeof(*(ctx)->field))

static bool
ph_fs_fail(struct ph_fs_ctx *ctx, const char *why)
{
   if (!ctx->reject)
      ctx->reject = why;
   return false;
}

static bool
ph_fs_near(float a, float b)
{
   return fabsf(a - b) < 1e-6f;
}

/* ---- constants and modifiers ----------------------------------------- */

static bool
ph_fs_part(struct ph_fs_ctx *ctx, const struct ph_fs_part *want,
           struct ph_fs_src *out)
{
   unsigned i;

   for (i = 0; i < ctx->parts; i++)
      if (!memcmp(&ctx->part[i], want, sizeof(*want)))
         break;
   if (i == ctx->parts) {
      if (!PH_FS_GROW(ctx, part, parts, part_cap))
         return ph_fs_fail(ctx, "out of memory");
      ctx->part[ctx->parts++] = *want;
   }
   *out = (struct ph_fs_src){ .kind = PH_SRC_CONST, .channel = -1,
                              .index = i };
   return true;
}

static unsigned
ph_fs_lane_channels(int lane)
{
   return lane == PH_LANE_A ? 1 : 3;
}

static bool
ph_fs_literal(struct ph_fs_ctx *ctx, int lane, const float *value,
              struct ph_fs_src *out)
{
   struct ph_fs_part part;

   memset(&part, 0, sizeof(part));
   part.lane = lane;
   part.literal = 1;
   for (unsigned c = 0; c < ph_fs_lane_channels(lane); c++)
      part.value[c] = value[c];
   return ph_fs_part(ctx, &part, out);
}

static bool
ph_fs_literal_scalar(struct ph_fs_ctx *ctx, int lane, float v,
                     struct ph_fs_src *out)
{
   const float value[3] = { v, v, v };

   return ph_fs_literal(ctx, lane, value, out);
}

static float
ph_fs_modify(float x, unsigned mod)
{
   switch (mod) {
   case P3D_COMB_MOD_NEG:        return -x;
   case P3D_COMB_MOD_BIAS:       return x - 0.5f;
   case P3D_COMB_MOD_BIAS_NEG:   return 0.5f - x;
   case P3D_COMB_MOD_SIGNED:     return 2.0f * x - 1.0f;
   case P3D_COMB_MOD_SIGNED_NEG: return 1.0f - 2.0f * x;
   case P3D_COMB_MOD_INVERT:     return 1.0f - x;
   default:                      return x;
   }
}

/* @outer after @inner as one modifier, or -1. */
static int
ph_fs_compose(unsigned inner, unsigned outer)
{
   if (inner == P3D_COMB_MOD_NONE)
      return outer;
   if (outer == P3D_COMB_MOD_NEG) {
      switch (inner) {
      case P3D_COMB_MOD_NEG:        return P3D_COMB_MOD_NONE;
      case P3D_COMB_MOD_BIAS:       return P3D_COMB_MOD_BIAS_NEG;
      case P3D_COMB_MOD_BIAS_NEG:   return P3D_COMB_MOD_BIAS;
      case P3D_COMB_MOD_SIGNED:     return P3D_COMB_MOD_SIGNED_NEG;
      case P3D_COMB_MOD_SIGNED_NEG: return P3D_COMB_MOD_SIGNED;
      default:                      return -1;
      }
   }
   if (outer == P3D_COMB_MOD_INVERT && inner == P3D_COMB_MOD_INVERT)
      return P3D_COMB_MOD_NONE;
   return -1;
}

/* ---- nodes ------------------------------------------------------------ */

static bool
ph_fs_node(struct ph_fs_ctx *ctx, int lane, unsigned op,
           const struct ph_fs_src *a, const struct ph_fs_src *b,
           const struct ph_fs_src *c, struct ph_fs_src *out)
{
   const struct ph_fs_src *src[3] = { a, b, c };
   struct ph_fs_src copy[3];
   uint16_t seen[3];
   unsigned constants = 0;
   struct ph_fs_node *node;

   for (unsigned i = 0; i < 3; i++) {
      unsigned k;

      memset(&copy[i], 0, sizeof(copy[i]));
      if (!src[i])
         continue;
      copy[i] = *src[i];
      if (copy[i].kind != PH_SRC_CONST)
         continue;
      for (k = 0; k < constants && seen[k] != copy[i].index; k++)
         ;
      if (k == constants)
         seen[constants++] = copy[i].index;
   }
   if (constants > 2)
      return ph_fs_fail(ctx, "three constants in one instruction");
   if (!PH_FS_GROW(ctx, node, nodes, node_cap))
      return ph_fs_fail(ctx, "out of memory");
   node = &ctx->node[ctx->nodes];
   memset(node, 0, sizeof(*node));
   node->lane = lane;
   node->op = op;
   node->refs = 1;
   node->slot = -1;
   node->end[0] = node->end[1] = -1;
   for (unsigned i = 0; i < 3; i++) {
      node->src[i] = copy[i];
      if (copy[i].kind == PH_SRC_NODE)
         ctx->node[copy[i].index].uses++;
   }
   *out = (struct ph_fs_src){ .kind = PH_SRC_NODE, .channel = -1,
                              .index = ctx->nodes++ };
   return true;
}

/* A node no other operand can observe, so its scale or saturate may change. */
static bool
ph_fs_owned(const struct ph_fs_ctx *ctx, const struct ph_fs_src *src)
{
   const struct ph_fs_node *node;

   if (src->kind != PH_SRC_NODE || src->mod != P3D_COMB_MOD_NONE)
      return false;
   node = &ctx->node[src->index];
   if (node->uses || node->refs != 1 || node->output ||
       node->lane == PH_LANE_BOTH)
      return false;
   return !node->memo || (node->def && list_is_singular(&node->def->uses));
}

static bool
ph_fs_mov(struct ph_fs_ctx *ctx, int lane, struct ph_fs_src *src)
{
   struct ph_fs_src in = *src;

   return ph_fs_node(ctx, lane, P3D_COMB_MOV, &in, NULL, NULL, src);
}

static bool
ph_fs_apply(struct ph_fs_ctx *ctx, int lane, struct ph_fs_src *src,
            unsigned mod)
{
   int composed;

   if (mod == P3D_COMB_MOD_NONE)
      return true;
   if (src->kind == PH_SRC_CONST && ctx->part[src->index].literal) {
      struct ph_fs_part part = ctx->part[src->index];

      for (unsigned c = 0; c < ph_fs_lane_channels(part.lane); c++)
         part.value[c] = ph_fs_modify(part.value[c], mod);
      return ph_fs_part(ctx, &part, src);
   }
   /* Bias and sign on a texture operand are OPEN: the vendor moves those
    * into the sampler state instead of the ALU. */
   composed = ph_fs_compose(src->mod, mod);
   if (composed >= 0 &&
       !(src->kind == PH_SRC_TEX && composed >= P3D_COMB_MOD_BIAS &&
         composed <= P3D_COMB_MOD_SIGNED_NEG)) {
      src->mod = composed;
      return true;
   }
   if (!ph_fs_mov(ctx, lane, src))
      return false;
   src->mod = mod;
   return true;
}

/* Scale a value by 2^@shift, -2..3, through 0x126c[19:17]. */
static bool
ph_fs_scaled(struct ph_fs_ctx *ctx, int lane, struct ph_fs_src *src, int shift)
{
   if (!shift)
      return true;
   if (shift < -2 || shift > 3)
      return ph_fs_fail(ctx, "a scale beyond the result shift");
   if (!ph_fs_owned(ctx, src) || ctx->node[src->index].scale ||
       ctx->node[src->index].sat) {
      if (!ph_fs_mov(ctx, lane, src))
         return false;
   }
   ctx->node[src->index].scale = shift >= 0 ? shift : 8 + shift;
   return true;
}

static bool
ph_fs_saturated(struct ph_fs_ctx *ctx, int lane, struct ph_fs_src *src)
{
   if (!ph_fs_owned(ctx, src) && !ph_fs_mov(ctx, lane, src))
      return false;
   ctx->node[src->index].sat = 1;
   return true;
}

/* ---- polynomials ------------------------------------------------------ */

static bool
ph_fs_atom(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
           uint16_t *out)
{
   unsigned i;

   for (i = 0; i < ctx->atoms; i++)
      if (ctx->atom[i].def == def && ctx->atom[i].lane == lane &&
          ctx->atom[i].sel == sel)
         break;
   if (i == ctx->atoms) {
      if (!PH_FS_GROW(ctx, atom, atoms, atom_cap))
         return ph_fs_fail(ctx, "out of memory");
      ctx->atom[ctx->atoms++] = (struct ph_fs_atom){ def, lane, sel };
   }
   *out = i;
   return true;
}

static void
ph_fs_poly_const(struct ph_fs_poly *p, float v)
{
   memset(p, 0, sizeof(*p));
   p->n = v != 0.0f;
   p->term[0].coef = v;
}

static bool
ph_fs_poly_add(struct ph_fs_poly *p, const struct ph_fs_term *t)
{
   if (t->coef == 0.0f)
      return true;
   for (unsigned i = 0; i < p->n; i++) {
      if (p->term[i].n != t->n ||
          memcmp(p->term[i].atom, t->atom, t->n * sizeof(t->atom[0])))
         continue;
      p->term[i].coef += t->coef;
      if (ph_fs_near(p->term[i].coef, 0.0f))
         p->term[i] = p->term[--p->n];
      return true;
   }
   if (p->n == PH_FS_POLY_TERMS)
      return false;
   p->term[p->n++] = *t;
   return true;
}

static bool
ph_fs_poly_sum(struct ph_fs_poly *p, const struct ph_fs_poly *a, float scale)
{
   for (unsigned i = 0; i < a->n; i++) {
      struct ph_fs_term t = a->term[i];

      t.coef *= scale;
      if (!ph_fs_poly_add(p, &t))
         return false;
   }
   return true;
}

static bool
ph_fs_poly_mul(struct ph_fs_poly *out, const struct ph_fs_poly *a,
               const struct ph_fs_poly *b)
{
   struct ph_fs_poly r;

   memset(&r, 0, sizeof(r));
   for (unsigned i = 0; i < a->n; i++) {
      for (unsigned j = 0; j < b->n; j++) {
         struct ph_fs_term t;

         memset(&t, 0, sizeof(t));
         t.n = a->term[i].n + b->term[j].n;
         if (t.n > PH_FS_TERM_ATOMS)
            return false;
         t.coef = a->term[i].coef * b->term[j].coef;
         memcpy(t.atom, a->term[i].atom, a->term[i].n * sizeof(t.atom[0]));
         memcpy(t.atom + a->term[i].n, b->term[j].atom,
                b->term[j].n * sizeof(t.atom[0]));
         for (unsigned x = 1; x < t.n; x++)
            for (unsigned y = x; y > 0 && t.atom[y - 1] > t.atom[y]; y--) {
               uint16_t swap = t.atom[y];

               t.atom[y] = t.atom[y - 1];
               t.atom[y - 1] = swap;
            }
         if (!ph_fs_poly_add(&r, &t))
            return false;
      }
   }
   *out = r;
   return true;
}

/* The request (lane, sel) on @alu's result makes on operand @i. */
static bool
ph_fs_translate(const nir_alu_instr *alu, unsigned i, int lane, int sel,
                int *out)
{
   const uint8_t *sw = alu->src[i].swizzle;

   if (nir_op_infos[alu->op].input_sizes[i] == 3 ||
       (lane == PH_LANE_RGB && sel < 0)) {
      if (sw[0] == sw[1] && sw[1] == sw[2])
         *out = sw[0];
      else if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2)
         *out = -1;
      else
         return false;
      return true;
   }
   if (nir_op_infos[alu->op].input_sizes[i])
      return false;
   *out = sw[sel];
   return true;
}

/* A literal whose requested channels all hold one value. */
static bool
ph_fs_uniform_literal(const nir_load_const_instr *load, int lane, int sel,
                      float *v)
{
   unsigned first = sel < 0 ? 0 : sel, last = sel < 0 ? 2 : sel;

   if (load->def.bit_size != 32 || last >= load->def.num_components)
      return false;
   *v = nir_const_value_as_float(load->value[first], 32);
   for (unsigned c = first + 1; c <= last; c++)
      if (nir_const_value_as_float(load->value[c], 32) != *v)
         return false;
   return true;
}

static bool
ph_fs_expand(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
             bool root, struct ph_fs_poly *p);

static bool
ph_fs_expand_src(struct ph_fs_ctx *ctx, const nir_alu_instr *alu, unsigned i,
                 int lane, int sel, struct ph_fs_poly *p)
{
   int req;

   return ph_fs_translate(alu, i, lane, sel, &req) &&
          ph_fs_expand(ctx, alu->src[i].src.ssa, lane, req, false, p);
}

static bool
ph_fs_expand(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
             bool root, struct ph_fs_poly *p)
{
   nir_instr *instr = nir_def_instr(def);
   struct ph_fs_poly a, b, c;
   nir_alu_instr *alu;
   uint16_t atom;
   float v;

   if (instr->type == nir_instr_type_load_const &&
       ph_fs_uniform_literal(nir_instr_as_load_const(instr), lane, sel, &v)) {
      ph_fs_poly_const(p, v);
      return true;
   }
   if (instr->type != nir_instr_type_alu ||
       (!root && !list_is_singular(&def->uses)))
      goto leaf;
   alu = nir_instr_as_alu(instr);
   memset(p, 0, sizeof(*p));
   switch (alu->op) {
   case nir_op_mov:
      return ph_fs_expand_src(ctx, alu, 0, lane, sel, p);
   case nir_op_fneg:
      return ph_fs_expand_src(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_poly_sum(p, &a, -1.0f);
   case nir_op_fadd:
   case nir_op_fsub:
      return ph_fs_expand_src(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_expand_src(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_poly_sum(p, &a, 1.0f) &&
             ph_fs_poly_sum(p, &b, alu->op == nir_op_fsub ? -1.0f : 1.0f);
   case nir_op_fmul:
      return ph_fs_expand_src(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_expand_src(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_poly_mul(p, &a, &b);
   case nir_op_ffma:
   case nir_op_ffma_weak:
   case nir_op_fmad:
   case nir_op_ffmaz:
   case nir_op_fmadz:
      return ph_fs_expand_src(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_expand_src(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_expand_src(ctx, alu, 2, lane, sel, &c) &&
             ph_fs_poly_mul(p, &a, &b) && ph_fs_poly_sum(p, &c, 1.0f);
   case nir_op_flrp: {
      struct ph_fs_poly one, keep;

      if (!ph_fs_expand_src(ctx, alu, 0, lane, sel, &a) ||
          !ph_fs_expand_src(ctx, alu, 1, lane, sel, &b) ||
          !ph_fs_expand_src(ctx, alu, 2, lane, sel, &c))
         return false;
      ph_fs_poly_const(&one, 1.0f);
      if (!ph_fs_poly_sum(&one, &c, -1.0f) ||
          !ph_fs_poly_mul(&keep, &a, &one) || !ph_fs_poly_mul(p, &b, &c))
         return false;
      return ph_fs_poly_sum(p, &keep, 1.0f);
   }
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      if (sel >= 0) {
         if ((unsigned)sel >= nir_op_infos[alu->op].num_inputs)
            return false;
         return ph_fs_expand(ctx, alu->src[sel].src.ssa, lane,
                             alu->src[sel].swizzle[0], false, p);
      }
      if (alu->src[0].src.ssa == alu->src[1].src.ssa &&
          alu->src[1].src.ssa == alu->src[2].src.ssa) {
         const uint8_t c0 = alu->src[0].swizzle[0];
         const uint8_t c1 = alu->src[1].swizzle[0];
         const uint8_t c2 = alu->src[2].swizzle[0];

         if (c0 == 0 && c1 == 1 && c2 == 2)
            return ph_fs_expand(ctx, alu->src[0].src.ssa, lane, -1, false, p);
         if (c0 == c1 && c1 == c2)
            return ph_fs_expand(ctx, alu->src[0].src.ssa, lane, c0, false, p);
      }
      break;
   default:
      break;
   }
leaf:
   if (!ph_fs_atom(ctx, def, lane, sel, &atom))
      return false;
   memset(p, 0, sizeof(*p));
   p->n = 1;
   p->term[0].coef = 1.0f;
   p->term[0].n = 1;
   p->term[0].atom[0] = atom;
   return true;
}

/* ---- instruction selection -------------------------------------------- */

static bool
ph_fs_lower_atom_id(struct ph_fs_ctx *ctx, uint16_t id, struct ph_fs_src *out)
{
   const struct ph_fs_atom atom = ctx->atom[id];

   return ph_fs_lower(ctx, atom.def, atom.lane, atom.sel, out);
}

/* The exponent of @k when |k| is a power of two the result shift reaches. */
static bool
ph_fs_shift_of(float k, int *shift)
{
   for (int s = -2; s <= 3; s++)
      if (ph_fs_near(fabsf(k), (float)ldexp(1.0, s))) {
         *shift = s;
         return true;
      }
   return false;
}

/* k * x + c0. */
static bool
ph_fs_linear(struct ph_fs_ctx *ctx, int lane, uint16_t atom, float k, float c0,
             struct ph_fs_src *out)
{
   struct ph_fs_src x, lit, lit2;
   int shift;

   if (!ph_fs_lower_atom_id(ctx, atom, &x))
      return false;
   if (x.kind == PH_SRC_CONST && ctx->part[x.index].literal) {
      struct ph_fs_part part = ctx->part[x.index];

      for (unsigned c = 0; c < ph_fs_lane_channels(lane); c++)
         part.value[c] = part.value[c] * k + c0;
      return ph_fs_part(ctx, &part, out);
   }
   *out = x;
   if (ph_fs_near(k, 1.0f) && ph_fs_near(c0, 0.0f))
      return true;
   if (ph_fs_near(k, -1.0f) && ph_fs_near(c0, 0.0f))
      return ph_fs_apply(ctx, lane, out, P3D_COMB_MOD_NEG);
   if (ph_fs_near(k, -1.0f) && ph_fs_near(c0, 1.0f))
      return ph_fs_apply(ctx, lane, out, P3D_COMB_MOD_INVERT);
   if (x.kind != PH_SRC_TEX && x.mod == P3D_COMB_MOD_NONE) {
      if (ph_fs_near(k, 1.0f) && ph_fs_near(c0, -0.5f))
         return ph_fs_apply(ctx, lane, out, P3D_COMB_MOD_BIAS);
      if (ph_fs_near(k, 2.0f) && ph_fs_near(c0, -1.0f))
         return ph_fs_apply(ctx, lane, out, P3D_COMB_MOD_SIGNED);
      if (ph_fs_near(k, -2.0f) && ph_fs_near(c0, 1.0f))
         return ph_fs_apply(ctx, lane, out, P3D_COMB_MOD_SIGNED_NEG);
   }
   if (!ph_fs_shift_of(k, &shift)) {
      /* |k| > 1 goes into the shift, the rest into a constant in [-1, 1]. */
      shift = fabsf(k) > 1.0f ? (int)ceil(log2(fabsf(k))) : 0;
      if (shift > 3)
         return ph_fs_fail(ctx, "a coefficient beyond the result shift");
      if (!ph_fs_literal_scalar(ctx, lane, (float)ldexp(k, -shift), &lit))
         return false;
      if (ph_fs_near(c0, 0.0f)) {
         if (!ph_fs_node(ctx, lane, P3D_COMB_MUL, &x, NULL, &lit, out))
            return false;
      } else {
         if (!ph_fs_literal_scalar(ctx, lane, (float)ldexp(c0, -shift), &lit2) ||
             !ph_fs_node(ctx, lane, P3D_COMB_MAD, &x, &lit2, &lit, out))
            return false;
      }
      return ph_fs_scaled(ctx, lane, out, shift);
   }
   if (k < 0.0f && !ph_fs_apply(ctx, lane, out, P3D_COMB_MOD_NEG))
      return false;
   if (!ph_fs_near(c0, 0.0f)) {
      /* (x + c0 / k) * k: the shift scales the sum. */
      if (!ph_fs_literal_scalar(ctx, lane, c0 / fabsf(k), &lit))
         return false;
      x = *out;
      if (!ph_fs_node(ctx, lane, P3D_COMB_ADD, &x, &lit, NULL, out))
         return false;
   }
   return ph_fs_scaled(ctx, lane, out, shift);
}

static bool ph_fs_select(struct ph_fs_ctx *ctx, int lane,
                         const struct ph_fs_poly *p, struct ph_fs_src *out);

/* A polynomial that is one atom with coefficient one, or a constant. */
static bool
ph_fs_poly_simple(const struct ph_fs_poly *p)
{
   return !p->n || (p->n == 1 && (!p->term[0].n ||
                   (p->term[0].n == 1 && ph_fs_near(p->term[0].coef, 1.0f))));
}

/* a * t + b * (1 - t) = a*t + b - b*t for an atom t. */
static bool
ph_fs_try_lrp(struct ph_fs_ctx *ctx, int lane, const struct ph_fs_poly *p,
              struct ph_fs_src *out)
{
   for (unsigned i = 0; i < p->n; i++) {
      if (p->term[i].n != 2)
         continue;
      for (unsigned j = 0; j < 2; j++) {
         const uint16_t t = p->term[i].atom[j];
         struct ph_fs_poly x, y, a;
         struct ph_fs_src sa, sb, sc;
         bool usable = true;

         memset(&x, 0, sizeof(x));
         memset(&y, 0, sizeof(y));
         for (unsigned k = 0; k < p->n && usable; k++) {
            const struct ph_fs_term *term = &p->term[k];
            struct ph_fs_term rest;
            int at = -1;

            for (unsigned m = 0; m < term->n; m++)
               if (term->atom[m] == t)
                  at = m;
            if (at < 0) {
               usable = ph_fs_poly_add(&y, term);
               continue;
            }
            if (term->n != 2) {
               usable = false;
               continue;
            }
            memset(&rest, 0, sizeof(rest));
            rest.coef = term->coef;
            rest.n = 1;
            rest.atom[0] = term->atom[1 - at];
            usable = ph_fs_poly_add(&x, &rest);
         }
         if (!usable || !y.n)
            continue;
         a = x;
         if (!ph_fs_poly_sum(&a, &y, 1.0f))
            continue;
         if (!ph_fs_poly_simple(&a) || !ph_fs_poly_simple(&y))
            continue;
         if (!ph_fs_select(ctx, lane, &a, &sa) ||
             !ph_fs_select(ctx, lane, &y, &sb) ||
             !ph_fs_lower_atom_id(ctx, t, &sc))
            return false;
         return ph_fs_node(ctx, lane, P3D_COMB_LRP, &sa, &sb, &sc, out);
      }
   }
   return false;
}

/* The two multiplier operands of a product term, its coefficient folded in. */
static bool
ph_fs_factor(struct ph_fs_ctx *ctx, int lane, const struct ph_fs_term *t,
             struct ph_fs_src *a, struct ph_fs_src *c)
{
   struct ph_fs_src x, lit;
   int shift;

   if (!ph_fs_lower_atom_id(ctx, t->atom[0], a) ||
       !ph_fs_lower_atom_id(ctx, t->atom[1], c))
      return false;
   if (t->n == 3) {
      struct ph_fs_src last;

      x = *a;
      if (!ph_fs_lower_atom_id(ctx, t->atom[2], &last) ||
          !ph_fs_node(ctx, lane, P3D_COMB_MUL, &x, NULL, c, a))
         return false;
      *c = last;
   }
   if (ph_fs_near(t->coef, 1.0f))
      return true;
   if (ph_fs_near(t->coef, -1.0f))
      return ph_fs_apply(ctx, lane, a, P3D_COMB_MOD_NEG);
   if (a->kind == PH_SRC_CONST && ctx->part[a->index].literal) {
      x = *a;
      *a = *c;
      *c = x;
   }
   if (c->kind == PH_SRC_CONST && ctx->part[c->index].literal) {
      struct ph_fs_part part = ctx->part[c->index];

      for (unsigned k = 0; k < ph_fs_lane_channels(lane); k++)
         part.value[k] *= t->coef;
      return ph_fs_part(ctx, &part, c);
   }
   if (t->coef < 0.0f && !ph_fs_apply(ctx, lane, c, P3D_COMB_MOD_NEG))
      return false;
   if (ph_fs_shift_of(t->coef, &shift))
      return ph_fs_scaled(ctx, lane, c, shift);
   if (fabsf(t->coef) > 1.0f)
      return ph_fs_fail(ctx, "a product coefficient beyond [-1, 1]");
   x = *c;
   return ph_fs_literal_scalar(ctx, lane, fabsf(t->coef), &lit) &&
          ph_fs_node(ctx, lane, P3D_COMB_MUL, &x, NULL, &lit, c);
}

static void
ph_fs_poly_without(struct ph_fs_poly *out, const struct ph_fs_poly *p,
                   unsigned skip)
{
   memset(out, 0, sizeof(*out));
   for (unsigned i = 0; i < p->n; i++)
      if (i != skip)
         out->term[out->n++] = p->term[i];
}

static bool
ph_fs_select(struct ph_fs_ctx *ctx, int lane, const struct ph_fs_poly *p,
             struct ph_fs_src *out)
{
   struct ph_fs_poly rest;
   struct ph_fs_src a, b, c;
   int lin = -1, prod = -1, shift = 0;
   unsigned terms = 0;
   float c0 = 0.0f;

   for (unsigned i = 0; i < p->n; i++) {
      const struct ph_fs_term *t = &p->term[i];

      if (!t->n) {
         c0 = t->coef;
         continue;
      }
      terms++;
      if (t->n == 1 && lin < 0)
         lin = i;
      if (t->n >= 2 && (prod < 0 || t->n < p->term[prod].n))
         prod = i;
   }
   if (!terms)
      return ph_fs_literal_scalar(ctx, lane, c0, out);
   if (terms == 1 && lin >= 0)
      return ph_fs_linear(ctx, lane, p->term[lin].atom[0], p->term[lin].coef,
                          c0, out);
   /* One positive power-of-two coefficient on every term: the sum at unit
    * weight, then the result shift. */
   {
      const float g = p->term[lin >= 0 ? lin : prod].coef;
      bool common = g > 0.0f && !ph_fs_near(g, 1.0f) && ph_fs_shift_of(g, &shift);

      for (unsigned i = 0; i < p->n && common; i++)
         if (p->term[i].n && !ph_fs_near(p->term[i].coef, g))
            common = false;
      if (common) {
         struct ph_fs_poly unit;

         memset(&unit, 0, sizeof(unit));
         return ph_fs_poly_sum(&unit, p, 1.0f / g) &&
                ph_fs_select(ctx, lane, &unit, out) &&
                ph_fs_scaled(ctx, lane, out, shift);
      }
   }
   if (ph_fs_try_lrp(ctx, lane, p, out))
      return true;
   if (ctx->reject)
      return false;
   if (prod >= 0) {
      ph_fs_poly_without(&rest, p, prod);
      if (!ph_fs_factor(ctx, lane, &p->term[prod], &a, &c))
         return false;
      if (!rest.n)
         return ph_fs_node(ctx, lane, P3D_COMB_MUL, &a, NULL, &c, out);
      return ph_fs_select(ctx, lane, &rest, &b) &&
             ph_fs_node(ctx, lane, P3D_COMB_MAD, &a, &b, &c, out);
   }
   /* A weighted term is a multiply-add by its literal weight. */
   for (unsigned i = 0; i < p->n; i++) {
      const struct ph_fs_term *t = &p->term[i];
      struct ph_fs_src lit;

      if (t->n != 1 || ph_fs_near(fabsf(t->coef), 1.0f) ||
          fabsf(t->coef) > 1.0f)
         continue;
      if (!ph_fs_lower_atom_id(ctx, t->atom[0], &a))
         return false;
      if (a.kind == PH_SRC_CONST)
         continue;
      ph_fs_poly_without(&rest, p, i);
      return ph_fs_literal_scalar(ctx, lane, t->coef, &lit) &&
             ph_fs_select(ctx, lane, &rest, &b) &&
             ph_fs_node(ctx, lane, P3D_COMB_MAD, &a, &b, &lit, out);
   }
   /* A sum of linear terms: one term plus the rest, constant included. */
   ph_fs_poly_without(&rest, p, lin);
   return ph_fs_linear(ctx, lane, p->term[lin].atom[0], p->term[lin].coef,
                       0.0f, &a) &&
          ph_fs_select(ctx, lane, &rest, &b) &&
          ph_fs_node(ctx, lane, P3D_COMB_ADD, &a, &b, NULL, out);
}

/* ---- leaves and operations that are no polynomial --------------------- */

static bool
ph_fs_uniform(struct ph_fs_ctx *ctx, unsigned base, int lane, int sel,
              struct ph_fs_src *out)
{
   struct ph_fs_part part;

   memset(&part, 0, sizeof(part));
   part.lane = lane;
   for (unsigned c = 0; c < ph_fs_lane_channels(lane); c++) {
      unsigned offset = base + (sel < 0 ? c : (unsigned)sel);

      if (offset > UINT16_MAX)
         return ph_fs_fail(ctx, "a constant past the addressable buffer");
      part.offset[c] = offset;
   }
   return ph_fs_part(ctx, &part, out);
}

static bool
ph_fs_varying(struct ph_fs_ctx *ctx, unsigned location, int sel,
              struct ph_fs_src *out)
{
   if (location == VARYING_SLOT_COL0) {
      out->index = 0;
   } else if (location == VARYING_SLOT_COL1 ||
              location == VARYING_SLOT_FOGC) {
      /* The vertex side carries the fog coordinate in colour 1. */
      out->index = 1;
      ctx->uses_color1 = true;
   } else {
      return ph_fs_fail(ctx, "a varying other than a colour");
   }
   out->kind = PH_SRC_COLOUR;
   out->channel = sel;
   ctx->uses_color = true;
   return true;
}

static bool
ph_fs_intrinsic(struct ph_fs_ctx *ctx, const nir_intrinsic_instr *load,
                int lane, int sel, struct ph_fs_src *out)
{
   switch (load->intrinsic) {
   case nir_intrinsic_load_deref: {
      nir_deref_instr *deref = nir_src_as_deref(load->src[0]);

      if (!deref || deref->deref_type != nir_deref_type_var ||
          !(deref->var->data.mode & nir_var_shader_in))
         return ph_fs_fail(ctx, "a dereference that is no input variable");
      return ph_fs_varying(ctx, deref->var->data.location, sel, out);
   }
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
      return ph_fs_varying(ctx, nir_intrinsic_io_semantics(load).location,
                           sel, out);
   case nir_intrinsic_load_ubo_vec4:
      /* The starting channel is the intrinsic's component index. */
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
          !nir_src_is_const(load->src[1]))
         break;
      return ph_fs_uniform(ctx, (nir_intrinsic_base(load) +
                                 nir_src_as_uint(load->src[1])) * 4u +
                                nir_intrinsic_component(load),
                           lane, sel, out);
   case nir_intrinsic_load_ubo:
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
          !nir_src_is_const(load->src[1]) ||
          nir_src_as_uint(load->src[1]) % 4u)
         break;
      return ph_fs_uniform(ctx, nir_src_as_uint(load->src[1]) / 4u, lane,
                           sel, out);
   case nir_intrinsic_load_uniform:
      if (!nir_src_is_const(load->src[0]) ||
          (nir_intrinsic_base(load) + nir_src_as_uint(load->src[0])) % 4u)
         break;
      return ph_fs_uniform(ctx, (nir_intrinsic_base(load) +
                                 nir_src_as_uint(load->src[0])) / 4u,
                           lane, sel, out);
   default:
      break;
   }
   return ph_fs_fail(ctx, nir_intrinsic_infos[load->intrinsic].name);
}

static bool
ph_fs_texture(struct ph_fs_ctx *ctx, const nir_tex_instr *tex, int sel,
              struct ph_fs_src *out)
{
   int bias = nir_tex_instr_src_index(tex, nir_tex_src_bias);

   if (tex->op != nir_texop_tex && tex->op != nir_texop_txb)
      return ph_fs_fail(ctx, "a texture lookup with an explicit level");
   if (bias >= 0 && (!nir_src_is_const(tex->src[bias].src) ||
                     nir_src_as_float(tex->src[bias].src) != 0.0f))
      return ph_fs_fail(ctx, "a per-fragment level bias");
   /* Fixed-function lookups always carry the coordinate's q as projector;
    * the vertex stream takes s and t only, so q other than 1 is not honoured
    * by any path. */
   if (nir_tex_instr_src_index(tex, nir_tex_src_comparator) >= 0)
      return ph_fs_fail(ctx, "a shadow comparison");
   if (nir_tex_instr_src_index(tex, nir_tex_src_offset) >= 0)
      return ph_fs_fail(ctx, "a texel offset");
   if (tex->sampler_index > 3)
      return ph_fs_fail(ctx, "a sampler beyond the four pipes");
   ctx->sampler_mask |= 1u << tex->sampler_index;
   *out = (struct ph_fs_src){ .kind = PH_SRC_TEX, .channel = sel,
                              .index = tex->sampler_index };
   return true;
}

static bool
ph_fs_operand(struct ph_fs_ctx *ctx, const nir_alu_instr *alu, unsigned i,
              int lane, int sel, struct ph_fs_src *out)
{
   int req;

   if (!ph_fs_translate(alu, i, lane, sel, &req))
      return ph_fs_fail(ctx, "a swizzle the replicates cannot express");
   return ph_fs_lower(ctx, alu->src[i].src.ssa, lane, req, out);
}

static bool
ph_fs_operation(struct ph_fs_ctx *ctx, nir_alu_instr *alu, int lane, int sel,
                struct ph_fs_src *out)
{
   struct ph_fs_src a, b, c, d;

   switch (alu->op) {
   case nir_op_mov:
      return ph_fs_operand(ctx, alu, 0, lane, sel, out);
   case nir_op_fsat:
      return ph_fs_operand(ctx, alu, 0, lane, sel, out) &&
             ph_fs_saturated(ctx, lane, out);
   case nir_op_fneg:
      return ph_fs_operand(ctx, alu, 0, lane, sel, out) &&
             ph_fs_apply(ctx, lane, out, P3D_COMB_MOD_NEG);
   case nir_op_fadd:
   case nir_op_fsub:
      if (!ph_fs_operand(ctx, alu, 0, lane, sel, &a) ||
          !ph_fs_operand(ctx, alu, 1, lane, sel, &b) ||
          (alu->op == nir_op_fsub &&
           !ph_fs_apply(ctx, lane, &b, P3D_COMB_MOD_NEG)))
         return false;
      return ph_fs_node(ctx, lane, P3D_COMB_ADD, &a, &b, NULL, out);
   case nir_op_fmul:
      return ph_fs_operand(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_operand(ctx, alu, 1, lane, sel, &c) &&
             ph_fs_node(ctx, lane, P3D_COMB_MUL, &a, NULL, &c, out);
   case nir_op_ffma:
   case nir_op_ffma_weak:
   case nir_op_fmad:
   case nir_op_ffmaz:
   case nir_op_fmadz:
      return ph_fs_operand(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_operand(ctx, alu, 1, lane, sel, &c) &&
             ph_fs_operand(ctx, alu, 2, lane, sel, &b) &&
             ph_fs_node(ctx, lane, P3D_COMB_MAD, &a, &b, &c, out);
   case nir_op_flrp:
      return ph_fs_operand(ctx, alu, 0, lane, sel, &b) &&
             ph_fs_operand(ctx, alu, 1, lane, sel, &a) &&
             ph_fs_operand(ctx, alu, 2, lane, sel, &c) &&
             ph_fs_node(ctx, lane, P3D_COMB_LRP, &a, &b, &c, out);
   case nir_op_fdot3:
      /* One node writes the dot product to both halves. */
      for (unsigned i = 0; i < ctx->nodes; i++)
         if (ctx->node[i].def == &alu->def &&
             ctx->node[i].lane == PH_LANE_BOTH) {
            ctx->node[i].refs++;
            *out = (struct ph_fs_src){ .kind = PH_SRC_NODE, .channel = -1,
                                       .index = i };
            return true;
         }
      if (!ph_fs_operand(ctx, alu, 0, PH_LANE_RGB, -1, &a) ||
          !ph_fs_operand(ctx, alu, 1, PH_LANE_RGB, -1, &c) ||
          !ph_fs_node(ctx, PH_LANE_BOTH, P3D_COMB_DP3, &a, NULL, &c, out))
         return false;
      ctx->node[out->index].def = &alu->def;
      return true;
   case nir_op_fcsel_ge:
      return ph_fs_operand(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_operand(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_operand(ctx, alu, 2, lane, sel, &c) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &b, &c, out);
   case nir_op_fcsel_gt:
      /* a > 0 ? b : c is -a >= 0 ? c : b. */
      return ph_fs_operand(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_operand(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_operand(ctx, alu, 2, lane, sel, &c) &&
             ph_fs_apply(ctx, lane, &a, P3D_COMB_MOD_NEG) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &c, &b, out);
   case nir_op_fmax:
   case nir_op_fmin:
      if (!ph_fs_operand(ctx, alu, 0, lane, sel, &a) ||
          !ph_fs_operand(ctx, alu, 1, lane, sel, &b))
         return false;
      d = b;
      if (!ph_fs_apply(ctx, lane, &d, P3D_COMB_MOD_NEG) ||
          !ph_fs_node(ctx, lane, P3D_COMB_ADD, &a, &d, NULL, &d))
         return false;
      return alu->op == nir_op_fmax ?
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &d, &a, &b, out) :
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &d, &b, &a, out);
   case nir_op_fabs:
      if (!ph_fs_operand(ctx, alu, 0, lane, sel, &a))
         return false;
      b = a;
      return ph_fs_apply(ctx, lane, &b, P3D_COMB_MOD_NEG) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &a, &b, out);
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4: {
      float value[3];

      if (sel >= 0) {
         if ((unsigned)sel >= nir_op_infos[alu->op].num_inputs)
            return ph_fs_fail(ctx, "a channel past the vector");
         return ph_fs_lower(ctx, alu->src[sel].src.ssa, lane,
                            alu->src[sel].swizzle[0], out);
      }
      /* Three RGB channels from different values: only literals combine. */
      for (unsigned i = 0; i < 3; i++) {
         nir_instr *instr = nir_def_instr(alu->src[i].src.ssa);

         if (instr->type != nir_instr_type_load_const)
            return ph_fs_fail(ctx, "RGB assembled from separate values");
         value[i] = nir_const_value_as_float(
            nir_instr_as_load_const(instr)->value[alu->src[i].swizzle[0]], 32);
      }
      return ph_fs_literal(ctx, lane, value, out);
   }
   default:
      return ph_fs_fail(ctx, nir_op_infos[alu->op].name);
   }
}

static bool
ph_fs_lower_leaf(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
                 struct ph_fs_src *out)
{
   nir_instr *instr = nir_def_instr(def);

   memset(out, 0, sizeof(*out));
   switch (instr->type) {
   case nir_instr_type_load_const: {
      const nir_load_const_instr *load = nir_instr_as_load_const(instr);
      float value[3];

      if (load->def.bit_size != 32)
         return ph_fs_fail(ctx, "a literal that is no 32-bit float");
      for (unsigned c = 0; c < ph_fs_lane_channels(lane); c++) {
         unsigned channel = sel < 0 ? c : (unsigned)sel;

         value[c] = nir_const_value_as_float(
            load->value[MIN2(channel, load->def.num_components - 1u)], 32);
      }
      return ph_fs_literal(ctx, lane, value, out);
   }
   case nir_instr_type_tex:
      return ph_fs_texture(ctx, nir_instr_as_tex(instr), sel, out);
   case nir_instr_type_intrinsic:
      return ph_fs_intrinsic(ctx, nir_instr_as_intrinsic(instr), lane, sel,
                             out);
   case nir_instr_type_alu:
      return ph_fs_operation(ctx, nir_instr_as_alu(instr), lane, sel, out);
   default:
      return ph_fs_fail(ctx, "an instruction that is no value");
   }
}

static bool
ph_fs_lower(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
            struct ph_fs_src *out)
{
   struct ph_fs_poly poly;
   const struct ph_fs_atom *self;
   bool polynomial;

   for (unsigned i = 0; i < ctx->memos; i++) {
      if (ctx->memo[i].def != def || ctx->memo[i].lane != lane ||
          ctx->memo[i].sel != sel)
         continue;
      *out = ctx->memo[i].src;
      if (out->kind == PH_SRC_NODE)
         ctx->node[out->index].refs++;
      return true;
   }
   polynomial = ph_fs_expand(ctx, def, lane, sel, true, &poly);
   if (ctx->reject)
      return false;
   self = polynomial && poly.n == 1 && poly.term[0].n == 1 &&
          ph_fs_near(poly.term[0].coef, 1.0f) ?
          &ctx->atom[poly.term[0].atom[0]] : NULL;
   if (polynomial && !(self && self->def == def && self->lane == lane &&
                       self->sel == sel)) {
      if (!ph_fs_select(ctx, lane, &poly, out))
         return false;
   } else if (!ph_fs_lower_leaf(ctx, def, lane, sel, out)) {
      return false;
   }
   if (!PH_FS_GROW(ctx, memo, memos, memo_cap))
      return ph_fs_fail(ctx, "out of memory");
   ctx->memo[ctx->memos++] = (struct ph_fs_memo){ def, lane, sel, *out };
   if (out->kind == PH_SRC_NODE && out->mod == P3D_COMB_MOD_NONE) {
      struct ph_fs_node *node = &ctx->node[out->index];

      node->memo = true;
      if (!node->def)
         node->def = def;
   }
   return true;
}

/* Put a lane of the fragment colour into r0, saturated. */
static bool
ph_fs_result(struct ph_fs_ctx *ctx, int lane, struct ph_fs_src *src)
{
   struct ph_fs_node *node;

   if (src->kind == PH_SRC_NODE && src->mod == P3D_COMB_MOD_NONE) {
      node = &ctx->node[src->index];
      if ((node->lane == lane || node->lane == PH_LANE_BOTH) && !node->uses) {
         node->output |= 1u << lane;
         node->sat = 1;
         return true;
      }
   }
   if (!ph_fs_mov(ctx, lane, src))
      return false;
   node = &ctx->node[src->index];
   node->output |= 1u << lane;
   node->sat = 1;
   return true;
}

/* ---- slots and registers ---------------------------------------------- */

static bool
ph_fs_in_lane(const struct ph_fs_node *node, int lane)
{
   return node->lane == lane || node->lane == PH_LANE_BOTH;
}

static bool
ph_fs_ready(const struct ph_fs_ctx *ctx, const struct ph_fs_node *node,
            int slot)
{
   for (unsigned i = 0; i < 3; i++) {
      const struct ph_fs_src *s = &node->src[i];

      if (s->kind == PH_SRC_NODE &&
          (ctx->node[s->index].slot < 0 || ctx->node[s->index].slot >= slot))
         return false;
   }
   return true;
}

static bool
ph_fs_schedule(struct ph_fs_ctx *ctx, unsigned max, unsigned *count)
{
   unsigned head[2] = { 0, 0 };
   int slot = 0;

   for (;;) {
      int cand[2];
      bool picked = false;

      for (int lane = 0; lane < 2; lane++) {
         while (head[lane] < ctx->nodes &&
                (!ph_fs_in_lane(&ctx->node[head[lane]], lane) ||
                 ctx->node[head[lane]].slot >= 0))
            head[lane]++;
         cand[lane] = head[lane] < ctx->nodes ? (int)head[lane] : -1;
      }
      if (cand[0] < 0 && cand[1] < 0)
         break;
      if ((unsigned)slot >= max)
         return ph_fs_fail(ctx, "more instructions than the free slots");
      if (cand[0] >= 0 && cand[0] == cand[1]) {
         if (ph_fs_ready(ctx, &ctx->node[cand[0]], slot)) {
            ctx->node[cand[0]].slot = slot;
            picked = true;
         }
      } else {
         for (int lane = 0; lane < 2; lane++) {
            struct ph_fs_node *node;

            if (cand[lane] < 0)
               continue;
            node = &ctx->node[cand[lane]];
            if (node->lane != PH_LANE_BOTH && ph_fs_ready(ctx, node, slot)) {
               node->slot = slot;
               picked = true;
            }
         }
      }
      if (!picked)
         return ph_fs_fail(ctx, "no slot order satisfies both halves");
      slot++;
   }
   *count = slot;
   return true;
}

/* The storage lane an operand in half @half reads. */
static int
ph_fs_storage(const struct ph_fs_ctx *ctx, int half,
              const struct ph_fs_src *s)
{
   if (s->kind == PH_SRC_NODE) {
      const struct ph_fs_node *dep = &ctx->node[s->index];

      return dep->lane == PH_LANE_BOTH ? half : dep->lane;
   }
   return s->channel == 3 ? PH_LANE_A :
          half == PH_LANE_A && s->channel < 0 ? PH_LANE_A : PH_LANE_RGB;
}

static void
ph_fs_note_read(int *end, bool *cross, int slot, bool other_half)
{
   if (slot > *end) {
      *end = slot;
      *cross = other_half;
   } else if (slot == *end && other_half) {
      *cross = true;
   }
}

static void
ph_fs_liveness(struct ph_fs_ctx *ctx)
{
   for (unsigned u = 0; u < 4; u++)
      for (unsigned l = 0; l < 2; l++) {
         ctx->tex_end[u][l] = -1;
         ctx->tex_end_cross[u][l] = false;
      }
   for (unsigned n = 0; n < ctx->nodes; n++) {
      const struct ph_fs_node *reader = &ctx->node[n];
      const int half = reader->lane == PH_LANE_A ? PH_LANE_A : PH_LANE_RGB;

      for (unsigned i = 0; i < 3; i++) {
         const struct ph_fs_src *s = &reader->src[i];
         const int storage = ph_fs_storage(ctx, half, s);

         if (s->kind == PH_SRC_NODE)
            ph_fs_note_read(&ctx->node[s->index].end[storage],
                            &ctx->node[s->index].end_cross[storage],
                            reader->slot, storage != half);
         else if (s->kind == PH_SRC_TEX)
            ph_fs_note_read(&ctx->tex_end[s->index][storage],
                            &ctx->tex_end_cross[s->index][storage],
                            reader->slot, storage != half);
      }
   }
   for (unsigned n = 0; n < ctx->nodes; n++)
      for (unsigned l = 0; l < 2; l++)
         if (ctx->node[n].output & (1u << l))
            ctx->node[n].end[l] = PH_FS_LIVE_TO_END;
}

/* Whether @later writing @storage of @p's register destroys a value still
 * read. */
static bool
ph_fs_clobbers(const struct ph_fs_node *p, const struct ph_fs_node *later,
               int storage)
{
   if (!ph_fs_in_lane(p, storage) || later->slot <= p->slot)
      return false;
   return later->slot < p->end[storage] ||
          (later->slot == p->end[storage] && p->end_cross[storage]);
}

static bool
ph_fs_allocate(struct ph_fs_ctx *ctx, unsigned count)
{
   static const uint8_t order[] = {
      P3D_COMB_R1, P3D_COMB_R0, P3D_COMB_T3, P3D_COMB_T2, P3D_COMB_T1,
      P3D_COMB_T0,
   };

   ph_fs_liveness(ctx);
   for (unsigned slot = 0; slot < count; slot++) {
      for (unsigned n = 0; n < ctx->nodes; n++) {
         struct ph_fs_node *node = &ctx->node[n];
         bool placed = false;

         if (node->slot != (int)slot)
            continue;
         for (unsigned r = 0; r < sizeof(order) && !placed; r++) {
            const uint8_t reg = node->output ? P3D_COMB_R0 : order[r];
            bool usable = true;

            for (int l = 0; l < 2 && usable; l++) {
               if (!ph_fs_in_lane(node, l))
                  continue;
               if (reg <= P3D_COMB_T3 &&
                   (ctx->tex_end[reg][l] > (int)slot ||
                    (ctx->tex_end[reg][l] == (int)slot &&
                     ctx->tex_end_cross[reg][l])))
                  usable = false;
               for (unsigned o = 0; o < ctx->nodes && usable; o++) {
                  const struct ph_fs_node *other = &ctx->node[o];

                  if (o == n || other->slot < 0)
                     continue;
                  /* An earlier value in this register must be dead, and a
                   * later output that needs r0 must find this one dead. */
                  if (other->slot < (int)slot && other->reg == reg &&
                      ph_fs_clobbers(other, node, l))
                     usable = false;
                  if (reg == P3D_COMB_R0 && other->output &&
                      other->slot > (int)slot && ph_fs_in_lane(other, l) &&
                      ph_fs_clobbers(node, other, l))
                     usable = false;
               }
            }
            if (usable) {
               node->reg = reg;
               placed = true;
            }
            if (node->output)
               break;
         }
         if (!placed)
            return ph_fs_fail(ctx, "no free register for an intermediate");
      }
   }
   return true;
}

/* ---- emission --------------------------------------------------------- */

static uint8_t
ph_fs_rep(int half, int channel)
{
   static const uint8_t code[4] = {
      P3D_COMB_REP_R, P3D_COMB_REP_G, P3D_COMB_REP_B, P3D_COMB_REP_A,
   };

   if (channel < 0 || (half == PH_LANE_A && channel == 3))
      return P3D_COMB_REP_NONE;
   return code[channel];
}

/* A stored constant and the read modifier that give @value exactly, or else
 * within the Q1.10 range. */
static bool
ph_fs_encode(const float *value, unsigned n, uint8_t *mod, float *stored)
{
   static const uint8_t mods[] = {
      P3D_COMB_MOD_NONE, P3D_COMB_MOD_INVERT, P3D_COMB_MOD_NEG,
      P3D_COMB_MOD_SIGNED, P3D_COMB_MOD_BIAS,
   };

   for (unsigned pass = 0; pass < 2; pass++) {
      for (unsigned m = 0; m < sizeof(mods); m++) {
         bool fits = true;

         for (unsigned c = 0; c < n && fits; c++) {
            float s;

            switch (mods[m]) {
            case P3D_COMB_MOD_INVERT: s = 1.0f - value[c]; break;
            case P3D_COMB_MOD_NEG:    s = -value[c]; break;
            case P3D_COMB_MOD_SIGNED: s = (value[c] + 1.0f) * 0.5f; break;
            case P3D_COMB_MOD_BIAS:   s = value[c] + 0.5f; break;
            default:                  s = value[c]; break;
            }
            if (pass == 0)
               fits = s >= -1.0f && s <= 1023.0f / 1024.0f &&
                      fabsf(s * 1024.0f - roundf(s * 1024.0f)) < 1e-3f;
            else
               fits = s >= -1.0f && s <= 1.0f;
            stored[c] = s;
         }
         if (fits) {
            *mod = mods[m];
            return true;
         }
      }
   }
   return false;
}

static void
ph_fs_emit_half(const struct ph_fs_ctx *ctx, const struct ph_fs_node *node,
                int half, const int8_t *creg, const uint8_t *cmod,
                struct p3d_comb_half *out)
{
   memset(out, 0, sizeof(*out));
   out->used = 1;
   out->op = node->op;
   out->dst = node->reg;
   out->scale = node->scale;
   out->sat = node->sat;
   for (unsigned i = 0; i < 3; i++) {
      const struct ph_fs_src *s = &node->src[i];
      struct p3d_comb_src *d = &out->src[i];
      int channel = s->channel;

      d->mod = s->mod;
      switch (s->kind) {
      case PH_SRC_NODE: {
         const struct ph_fs_node *dep = &ctx->node[s->index];

         d->reg = dep->reg;
         channel = half == PH_LANE_RGB && dep->lane == PH_LANE_A ? 3 : -1;
         break;
      }
      case PH_SRC_TEX:
         d->reg = P3D_COMB_T0 + s->index;
         break;
      case PH_SRC_COLOUR:
         d->reg = P3D_COMB_V0 + s->index;
         break;
      case PH_SRC_CONST:
         d->reg = P3D_COMB_C0 + creg[i];
         d->mod = cmod[i] == P3D_COMB_MOD_NONE ? s->mod : cmod[i];
         channel = -1;
         break;
      default:
         d->reg = P3D_COMB_T0;
         continue;
      }
      d->rep = ph_fs_rep(half, channel);
   }
}

static bool
ph_fs_emit_constants(struct ph_fs_ctx *ctx, const struct ph_fs_node *node,
                     unsigned instruction, int lane,
                     struct ph_fs_program *prog, int8_t *creg, uint8_t *cmod)
{
   struct p3d_comb_inst *inst = &prog->comb.inst[instruction];
   uint16_t part[2];
   unsigned parts = 0;

   for (unsigned i = 0; i < 3; i++) {
      const struct ph_fs_src *s = &node->src[i];
      const struct ph_fs_part *p;
      unsigned k;

      creg[i] = 0;
      cmod[i] = P3D_COMB_MOD_NONE;
      if (s->kind != PH_SRC_CONST)
         continue;
      for (k = 0; k < parts && part[k] != s->index; k++)
         ;
      if (k == parts)
         part[parts++] = s->index;
      creg[i] = k;
      p = &ctx->part[s->index];
      inst->has_const[k] = 1;
      if (p->literal) {
         float stored[3];
         uint8_t mod;

         if (!ph_fs_encode(p->value, ph_fs_lane_channels(lane), &mod, stored))
            return ph_fs_fail(ctx, "a literal outside the constant range");
         if (s->mod != P3D_COMB_MOD_NONE)
            return ph_fs_fail(ctx, "a modifier on a folded literal");
         cmod[i] = mod;
         if (lane == PH_LANE_A)
            inst->constant[k][3] = stored[0];
         else
            memcpy(inst->constant[k], stored, 3 * sizeof(float));
      } else {
         struct ph_fs_const_ref *ref = NULL;

         for (unsigned r = 0; r < prog->const_ref_count; r++)
            if (prog->const_ref[r].instruction == instruction &&
                prog->const_ref[r].slot == k)
               ref = &prog->const_ref[r];
         if (!ref) {
            ref = &prog->const_ref[prog->const_ref_count++];
            memset(ref, 0, sizeof(*ref));
            ref->instruction = instruction;
            ref->slot = k;
         }
         if (lane == PH_LANE_A) {
            ref->mask |= 1u << 3;
            ref->offset[3] = p->offset[0];
         } else {
            ref->mask |= 7u;
            memcpy(ref->offset, p->offset, 3 * sizeof(p->offset[0]));
         }
      }
   }
   return true;
}

static bool
ph_fs_emit(struct ph_fs_ctx *ctx, unsigned count, struct ph_fs_program *prog)
{
   prog->comb.count = count;
   for (unsigned n = 0; n < ctx->nodes; n++) {
      const struct ph_fs_node *node = &ctx->node[n];
      struct p3d_comb_inst *inst = &prog->comb.inst[node->slot];
      int8_t creg[3];
      uint8_t cmod[3];

      if (node->lane == PH_LANE_BOTH) {
         /* A dot product broadcasts: both halves carry the same operation. */
         if (!ph_fs_emit_constants(ctx, node, node->slot, PH_LANE_RGB, prog,
                                   creg, cmod))
            return false;
         ph_fs_emit_half(ctx, node, PH_LANE_RGB, creg, cmod, &inst->rgb);
         ph_fs_emit_half(ctx, node, PH_LANE_A, creg, cmod, &inst->alpha);
         continue;
      }
      if (!ph_fs_emit_constants(ctx, node, node->slot, node->lane, prog,
                                creg, cmod))
         return false;
      ph_fs_emit_half(ctx, node, node->lane, creg, cmod,
                      node->lane == PH_LANE_A ? &inst->alpha : &inst->rgb);
   }
   return true;
}

/* ---- evaluation and classification ------------------------------------ */

static float
ph_fs_quantise(float f)
{
   int v = (int)(f * 1024.0f);

   return (float)CLAMP(v, -1024, 1023) / 1024.0f;
}

void
ph_fs_eval(const struct p3d_comb_program *prog, const float tex[4][4],
           const float colour[2][4], float range, float out[4])
{
   float reg[P3D_COMB_V1 + 1][4];

   memset(reg, 0, sizeof(reg));
   for (unsigned u = 0; u < 4; u++)
      memcpy(reg[P3D_COMB_T0 + u], tex[u], sizeof(reg[0]));
   memcpy(reg[P3D_COMB_V0], colour[0], sizeof(reg[0]));
   memcpy(reg[P3D_COMB_V1], colour[1], sizeof(reg[0]));

   for (unsigned i = 0; i < prog->count; i++) {
      const struct p3d_comb_inst *in = &prog->inst[i];
      const struct p3d_comb_half *half[2] = { &in->rgb, &in->alpha };
      float result[2][4];

      for (unsigned k = 0; k < 2; k++)
         for (unsigned c = 0; c < 4; c++)
            reg[P3D_COMB_C0 + k][c] =
               in->has_const[k] ? ph_fs_quantise(in->constant[k][c]) : 0.0f;
      for (unsigned h = 0; h < 2; h++) {
         const struct p3d_comb_half *op = half[h];
         float v[3][4];

         if (!op->used)
            continue;
         for (unsigned s = 0; s < 3; s++) {
            const float *r = reg[MIN2(op->src[s].reg, P3D_COMB_V1)];
            const unsigned rep = op->src[s].rep;

            for (unsigned c = 0; c < 4; c++) {
               unsigned ch = c;

               if (h == 0 && rep)
                  ch = rep == P3D_COMB_REP_A ? 3 : 3 - rep;
               else if (h == 1)
                  ch = (rep & 3) ? 3 - (rep & 3) : 3;
               v[s][c] = ph_fs_modify(r[ch], op->src[s].mod);
            }
         }
         for (unsigned c = 0; c < 4; c++) {
            const float a = v[0][c], b = v[1][c], k = v[2][c];
            float x;

            switch (op->op) {
            case P3D_COMB_MUL: x = a * k; break;
            case P3D_COMB_MOV: x = a; break;
            case P3D_COMB_SUB: x = a - b; break;
            case P3D_COMB_MAD: x = a * k + b; break;
            case P3D_COMB_LRP: x = a * k + b * (1.0f - k); break;
            case P3D_COMB_ADD: x = a + b; break;
            case P3D_COMB_DP3:
               x = v[0][0] * v[2][0] + v[0][1] * v[2][1] + v[0][2] * v[2][2];
               break;
            case P3D_COMB_CMP: x = a >= 0.0f ? b : k; break;
            case P3D_COMB_CND: x = a > 0.5f ? b : k; break;
            default: x = 0.0f; break;
            }
            x = (float)ldexp(x, op->scale < 4 ? op->scale : (int)op->scale - 8);
            x = CLAMP(x, -range, range);
            result[h][c] = op->sat ? CLAMP(x, 0.0f, 1.0f) : x;
         }
      }
      /* Both halves read before either writes; a dot product broadcasts. */
      if (in->rgb.used) {
         memcpy(reg[in->rgb.dst & 0xf], result[0], 3 * sizeof(float));
         if (in->rgb.op == P3D_COMB_DP3)
            reg[in->alpha.used ? in->alpha.dst & 0xf : in->rgb.dst & 0xf][3] =
               result[0][0];
      }
      if (in->alpha.used && in->rgb.op != P3D_COMB_DP3)
         reg[in->alpha.dst & 0xf][3] = result[1][3];
   }
   memcpy(out, reg[P3D_COMB_R0], 4 * sizeof(float));
}

static enum ph_fs_class
ph_fs_classify(const struct ph_fs_program *prog)
{
   static const enum ph_fs_class classes[] = {
      PH_FS_CLASS_COLOUR, PH_FS_CLASS_REPLACE, PH_FS_CLASS_MODULATE,
      PH_FS_CLASS_DECAL, PH_FS_CLASS_ADD, PH_FS_CLASS_MODULATE2,
      PH_FS_CLASS_ADD2, PH_FS_CLASS_REPLACE1,
   };
   uint32_t seed = 0x2545f491;

   if (prog->const_ref_count)
      return PH_FS_CLASS_OTHER;
   for (unsigned k = 0; k < sizeof(classes) / sizeof(classes[0]); k++) {
      const enum ph_fs_class cls = classes[k];
      bool match = true;

      for (unsigned trial = 0; trial < 16 && match; trial++) {
         float tex[4][4], colour[2][4], got[4], want[4];
         const float *t0 = tex[0], *t1 = tex[1], *v = colour[0];

         for (unsigned i = 0; i < 24; i++) {
            seed = seed * 1103515245u + 12345u;
            (i < 16 ? tex[i / 4] : colour[(i - 16) / 4])[i % 4] =
               ((seed >> 16) & 0xff) / 255.0f;
         }
         for (unsigned c = 0; c < 4; c++) {
            switch (cls) {
            case PH_FS_CLASS_COLOUR:    want[c] = v[c]; break;
            case PH_FS_CLASS_REPLACE:   want[c] = t0[c]; break;
            case PH_FS_CLASS_MODULATE:  want[c] = t0[c] * v[c]; break;
            case PH_FS_CLASS_DECAL:
               want[c] = c == 3 ? v[3] : v[c] + (t0[c] - v[c]) * t0[3];
               break;
            case PH_FS_CLASS_ADD:
               want[c] = c == 3 ? t0[3] * v[3] : t0[c] + v[c];
               break;
            case PH_FS_CLASS_MODULATE2: want[c] = t0[c] * v[c] * t1[c]; break;
            case PH_FS_CLASS_ADD2:
               want[c] = c == 3 ? t0[3] * v[3] * t1[3] : t0[c] * v[c] + t1[c];
               break;
            default:                    want[c] = t1[c]; break;
            }
            want[c] = CLAMP(want[c], 0.0f, 1.0f);
         }
         ph_fs_eval(&prog->comb, (const float (*)[4])tex,
                    (const float (*)[4])colour, 8.0f, got);
         for (unsigned c = 0; c < 4; c++)
            if (fabsf(got[c] - want[c]) > 1e-3f)
               match = false;
      }
      if (match)
         return cls;
   }
   return PH_FS_CLASS_OTHER;
}

/* ---- entry points ----------------------------------------------------- */

void
ph_fs_dump(const struct ph_fs_program *program, FILE *f)
{
   static const char *const reg[0x12] = {
      "t0", "t1", "t2", "t3", "r0", "r1", "?6", "?7", "?8", "?9",
      "?a", "?b", "?c", "?d", "c0", "c1", "v0", "v1",
   };
   static const char *const op[0x40] = {
      [P3D_COMB_MUL] = "mul", [P3D_COMB_MOV] = "mov", [P3D_COMB_SUB] = "sub",
      [P3D_COMB_MAD] = "mad", [P3D_COMB_LRP] = "lrp", [P3D_COMB_ADD] = "add",
      [P3D_COMB_DP3] = "dp3", [P3D_COMB_DP4] = "dp4", [P3D_COMB_CND] = "cnd",
      [P3D_COMB_CMP] = "cmp",
   };
   static const char *const rep = "_bgra";

   for (unsigned i = 0; i < program->comb.count; i++) {
      const struct p3d_comb_inst *in = &program->comb.inst[i];
      const struct p3d_comb_half *half[2] = { &in->rgb, &in->alpha };

      for (unsigned h = 0; h < 2; h++) {
         const struct p3d_comb_half *x = half[h];

         if (!x->used)
            continue;
         fprintf(f, "  fs %u %-3s %s%s%s %s", i, h ? "a" : "rgb",
                 op[x->op & 0x3f] ? op[x->op & 0x3f] : "??",
                 x->sat ? "_sat" : "", x->scale ? "_x" : "",
                 reg[MIN2(x->dst, 0x11u)]);
         for (unsigned s = 0; s < 3; s++)
            fprintf(f, " %s%s.%c", x->src[s].mod ? "~" : "",
                    reg[MIN2(x->src[s].reg, 0x11u)], rep[x->src[s].rep & 7 ?
                    MIN2(x->src[s].rep, 4u) : 0]);
         fputc('\n', f);
      }
      for (unsigned k = 0; k < 2; k++)
         if (in->has_const[k])
            fprintf(f, "  fs %u c%u = %g %g %g %g\n", i, k, in->constant[k][0],
                    in->constant[k][1], in->constant[k][2], in->constant[k][3]);
   }
   for (unsigned r = 0; r < program->const_ref_count; r++)
      fprintf(f, "  fs %u c%u from constants %u %u %u %u mask %#x\n",
              program->const_ref[r].instruction, program->const_ref[r].slot,
              program->const_ref[r].offset[0], program->const_ref[r].offset[1],
              program->const_ref[r].offset[2], program->const_ref[r].offset[3],
              program->const_ref[r].mask);
}

bool
ph_fs_compile(nir_shader *nir, unsigned max_instructions,
              struct ph_fs_program *out)
{
   struct ph_fs_ctx ctx;
   nir_def *colour[4] = { NULL };
   uint8_t channel[4] = { 0 };
   struct ph_fs_src rgb, alpha;
   unsigned count = 0;
   int rgb_sel;
   bool ok = false;

   memset(&ctx, 0, sizeof(ctx));
   memset(out, 0, sizeof(*out));
   nir_foreach_function_impl(impl, nir) {
      if (exec_list_length(&impl->body) != 1) {
         ph_fs_fail(&ctx, "control flow");
         goto done;
      }
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            const nir_intrinsic_instr *in;
            nir_def *value = NULL;
            unsigned mask = 0, first = 0;
            int location = -1;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            switch (in->intrinsic) {
            case nir_intrinsic_terminate:
            case nir_intrinsic_terminate_if:
            case nir_intrinsic_demote:
            case nir_intrinsic_demote_if:
               ph_fs_fail(&ctx, "discard");
               goto done;
            case nir_intrinsic_store_deref: {
               nir_deref_instr *deref = nir_src_as_deref(in->src[0]);

               if (!deref || deref->deref_type != nir_deref_type_var ||
                   !(deref->var->data.mode & nir_var_shader_out))
                  continue;
               location = deref->var->data.location;
               value = in->src[1].ssa;
               mask = nir_intrinsic_write_mask(in);
               break;
            }
            case nir_intrinsic_store_output:
               location = nir_intrinsic_io_semantics(in).location;
               value = in->src[0].ssa;
               mask = nir_intrinsic_write_mask(in);
               first = nir_intrinsic_component(in);
               break;
            default:
               continue;
            }
            if (location != FRAG_RESULT_COLOR &&
                location != FRAG_RESULT_DATA0) {
               ph_fs_fail(&ctx, "an output other than the colour");
               goto done;
            }
            for (unsigned c = 0; c < 4; c++) {
               if (!(mask & (1u << c)) || first + c >= 4)
                  continue;
               colour[first + c] = value;
               channel[first + c] = c;
            }
         }
      }
   }
   if (!colour[0] || !colour[1] || !colour[2] || !colour[3]) {
      ph_fs_fail(&ctx, "no complete colour write");
      goto done;
   }
   if (colour[0] != colour[1] || colour[1] != colour[2]) {
      ph_fs_fail(&ctx, "RGB written from separate values");
      goto done;
   }
   if (channel[0] == 0 && channel[1] == 1 && channel[2] == 2)
      rgb_sel = -1;
   else if (channel[0] == channel[1] && channel[1] == channel[2])
      rgb_sel = channel[0];
   else {
      ph_fs_fail(&ctx, "an RGB write the replicates cannot express");
      goto done;
   }
   if (!ph_fs_lower(&ctx, colour[0], PH_LANE_RGB, rgb_sel, &rgb) ||
       !ph_fs_lower(&ctx, colour[3], PH_LANE_A, channel[3], &alpha) ||
       !ph_fs_result(&ctx, PH_LANE_RGB, &rgb) ||
       !ph_fs_result(&ctx, PH_LANE_A, &alpha) ||
       !ph_fs_schedule(&ctx, MIN2(max_instructions, P3D_COMB_MAX_INSTRUCTIONS),
                       &count) ||
       !ph_fs_allocate(&ctx, count) || !ph_fs_emit(&ctx, count, out))
      goto done;
   out->sampler_mask = ctx.sampler_mask;
   out->uses_color = ctx.uses_color;
   out->uses_color1 = ctx.uses_color1;
   out->valid = true;
   out->cls = ph_fs_classify(out);
   ok = true;
done:
   if (!ok) {
      struct ph_fs_program failed = { .reject = ctx.reject ? ctx.reject :
                                                "not expressible" };

      *out = failed;
   }
   if (getenv("PARHELIA_TRACE_FS")) {
      if (ok) {
         fprintf(stderr, "parhelia: fs compiled, %u instructions, class %u\n",
                 out->comb.count, out->cls);
         ph_fs_dump(out, stderr);
      } else {
         fprintf(stderr, "parhelia: fs not compiled: %s\n", out->reject);
      }
   }
   free(ctx.node);
   free(ctx.part);
   free(ctx.atom);
   free(ctx.memo);
   return ok;
}

void
ph_fs_update_constants(struct ph_fs_program *program, const void *constants,
                       unsigned size)
{
   const float *values = constants;

   for (unsigned i = 0; i < program->const_ref_count; i++) {
      const struct ph_fs_const_ref *ref = &program->const_ref[i];
      float *target = program->comb.inst[ref->instruction].constant[ref->slot];

      for (unsigned c = 0; c < 4; c++) {
         if (!(ref->mask & (1u << c)))
            continue;
         target[c] = values && (ref->offset[c] + 1u) * sizeof(float) <= size ?
                     values[ref->offset[c]] : 0.0f;
      }
   }
}
