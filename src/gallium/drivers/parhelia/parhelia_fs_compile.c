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

#include "compiler/nir/nir_constant_expressions.h"
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

struct ph_fs_pre_memo {
   nir_def *def;
   unsigned comp;
   struct ph_fs_pre_src src;
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
   struct ph_fs_pre_node *pre;
   unsigned pres, pre_cap;
   struct ph_fs_pre_memo *pre_memo;
   unsigned pre_memos, pre_memo_cap;
   int tex_end[4][2];
   bool tex_end_cross[4][2];
   unsigned sampler_mask;
   bool uses_color, uses_color1, uses_fogc, uses_col1, uses_color1_alpha;
   bool uses_color0_alpha;
   int route[2];           /* generic varying slot carried in v0/v1, or -1 */
   const struct ph_fs_uexp *uexp;
   unsigned uexp_count;
   float tex_hi[4][4];     /* the largest value each unit channel samples */
   /* The fog factor read as v1.x: its root and its per-vertex expression. */
   nir_def *fog_root;
   struct ph_fs_fog_op *fog;
   unsigned fog_ops, fog_cap;
   const char *reject;
};

static bool ph_fs_lower(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
                        struct ph_fs_src *out);
static bool ph_fs_uniform_only(nir_def *def, unsigned depth);

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
   /* An instruction reads two constant registers; move any further constant
    * through a temporary first. */
   for (unsigned i = 0; constants > 2 && i < 3; i++) {
      if (copy[i].kind != PH_SRC_CONST || copy[i].index == seen[0] ||
          copy[i].index == seen[1])
         continue;
      if (!ph_fs_node(ctx, lane, P3D_COMB_MOV, &copy[i], NULL, NULL, &copy[i]))
         return false;
   }
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
      /* Channels past a narrower result are never read. */
      const unsigned n = nir_op_infos[alu->op].input_sizes[i] == 3 ? 3 :
                         MIN2(alu->def.num_components, 3);
      bool same = true, identity = true;

      for (unsigned c = 0; c < n; c++) {
         same &= sw[c] == sw[0];
         identity &= sw[c] == c;
      }
      if (identity)
         *out = -1;
      else if (same)
         *out = sw[0];
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
       (!root && !list_is_singular(&def->uses)) ||
       ph_fs_uniform_only(def, 0))
      goto leaf;
   alu = nir_instr_as_alu(instr);
   memset(p, 0, sizeof(*p));
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fcanonicalize:
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
   /* (x + c0 / k) * k needs c0 / k in a source's [-1, 1]; else multiply-add
    * with k and c0 as constants. */
   if (!ph_fs_shift_of(k, &shift) ||
       (!ph_fs_near(c0, 0.0f) && fabsf(c0 / k) > 1.0f)) {
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

/* ---- preshader ----------------------------------------------------------
 * A sub-expression that reads only uniforms and literals has one value per
 * draw.  It is evaluated on the CPU before the draw, with NIR's own constant
 * evaluator, and reaches the combiner as one more runtime constant, so the
 * ALU work the combiner lacks (floor, exp2, rsq, comparisons, ...) costs no
 * combiner slot.  Every fragment is still shaded by the combiner. */

/* The scalar constant-buffer offset of component 0 of a uniform load. */
static bool
ph_fs_uniform_offset(const nir_intrinsic_instr *load, unsigned *offset)
{
   switch (load->intrinsic) {
   case nir_intrinsic_load_ubo_vec4:
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
          !nir_src_is_const(load->src[1]))
         return false;
      *offset = (nir_intrinsic_base(load) + nir_src_as_uint(load->src[1])) * 4u +
                nir_intrinsic_component(load);
      return true;
   case nir_intrinsic_load_ubo:
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
          !nir_src_is_const(load->src[1]) || nir_src_as_uint(load->src[1]) % 4u)
         return false;
      *offset = nir_src_as_uint(load->src[1]) / 4u;
      return true;
   case nir_intrinsic_load_uniform:
      if (!nir_src_is_const(load->src[0]) ||
          (nir_intrinsic_base(load) + nir_src_as_uint(load->src[0])) % 4u)
         return false;
      *offset = (nir_intrinsic_base(load) + nir_src_as_uint(load->src[0])) / 4u;
      return true;
   default:
      return false;
   }
}

static bool
ph_fs_uniform_only(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   unsigned offset;

   if (depth > 64)
      return false;
   switch (instr->type) {
   case nir_instr_type_load_const:
      return true;
   case nir_instr_type_intrinsic:
      return ph_fs_uniform_offset(nir_instr_as_intrinsic(instr), &offset);
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         if (!ph_fs_uniform_only(alu->src[i].src.ssa, depth + 1))
            return false;
      return true;
   }
   default:
      return false;
   }
}

static bool
ph_fs_pre_node_vec(struct ph_fs_ctx *ctx, nir_op op, unsigned bit_size,
                   unsigned nsrc, const unsigned *ncomp,
                   const struct ph_fs_pre_src (*src)[4],
                   struct ph_fs_pre_src *out)
{
   struct ph_fs_pre_node *node;

   if (!PH_FS_GROW(ctx, pre, pres, pre_cap))
      return ph_fs_fail(ctx, "out of memory");
   node = &ctx->pre[ctx->pres];
   memset(node, 0, sizeof(*node));
   node->op = op;
   node->bit_size = bit_size;
   node->nsrc = nsrc;
   for (unsigned i = 0; i < nsrc; i++) {
      node->ncomp[i] = ncomp[i];
      memcpy(node->src[i], src[i], ncomp[i] * sizeof(src[i][0]));
   }
   *out = (struct ph_fs_pre_src){ .kind = PH_FS_PRE_RESULT,
                                  .index = ctx->pres++ };
   return true;
}

static bool
ph_fs_pre_node(struct ph_fs_ctx *ctx, nir_op op, unsigned bit_size,
               unsigned nsrc, const struct ph_fs_pre_src *src,
               struct ph_fs_pre_src *out)
{
   const unsigned ones[3] = { 1, 1, 1 };
   struct ph_fs_pre_src vec[3][4];

   for (unsigned i = 0; i < nsrc; i++)
      vec[i][0] = src[i];
   return ph_fs_pre_node_vec(ctx, op, bit_size, nsrc, ones,
                             (const struct ph_fs_pre_src (*)[4])vec, out);
}

/* Component @comp of uniform-only @def as a preshader source. */
static bool
ph_fs_pre_scalar(struct ph_fs_ctx *ctx, nir_def *def, unsigned comp,
                 struct ph_fs_pre_src *out)
{
   nir_instr *instr = nir_def_instr(def);
   struct ph_fs_pre_src src[3];
   unsigned offset;

   for (unsigned i = 0; i < ctx->pre_memos; i++)
      if (ctx->pre_memo[i].def == def && ctx->pre_memo[i].comp == comp) {
         *out = ctx->pre_memo[i].src;
         return true;
      }
   memset(out, 0, sizeof(*out));
   switch (instr->type) {
   case nir_instr_type_load_const:
      out->kind = PH_FS_PRE_LITERAL;
      out->literal = nir_instr_as_load_const(instr)->value[comp];
      return true;
   case nir_instr_type_intrinsic:
      if (!ph_fs_uniform_offset(nir_instr_as_intrinsic(instr), &offset))
         return ph_fs_fail(ctx, "a uniform the preshader cannot address");
      out->kind = PH_FS_PRE_BUFFER;
      out->index = offset + comp;
      return true;
   case nir_instr_type_alu:
      break;
   default:
      return ph_fs_fail(ctx, "a preshader input that is no value");
   }

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   const nir_op_info *info = &nir_op_infos[alu->op];
   unsigned bit_size = 0;

   switch (alu->op) {
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      return ph_fs_pre_scalar(ctx, alu->src[comp].src.ssa,
                              alu->src[comp].swizzle[0], out);
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
   case nir_op_fdot2_replicated:
   case nir_op_fdot3_replicated:
   case nir_op_fdot4_replicated: {
      const unsigned n = nir_ssa_alu_instr_src_components(alu, 0);
      struct ph_fs_pre_src sum, product;

      for (unsigned c = 0; c < n; c++) {
         if (!ph_fs_pre_scalar(ctx, alu->src[0].src.ssa,
                               alu->src[0].swizzle[c], &src[0]) ||
             !ph_fs_pre_scalar(ctx, alu->src[1].src.ssa,
                               alu->src[1].swizzle[c], &src[1]) ||
             !ph_fs_pre_node(ctx, nir_op_fmul, 32, 2, src, &product))
            return false;
         if (!c) {
            sum = product;
            continue;
         }
         src[0] = sum;
         src[1] = product;
         if (!ph_fs_pre_node(ctx, nir_op_fadd, 32, 2, src, &sum))
            return false;
      }
      *out = sum;
      break;
   }
   default:
      if (info->num_inputs > 3 || info->output_size > 1)
         return ph_fs_fail(ctx, "a preshader operation that is not per component");
      if (info->output_size == 1) {
         /* A reduction (all, any, a vector compare): its sources keep their
          * width and the one result is every component. */
         struct ph_fs_pre_src vec[3][4];
         unsigned ncomp[3];

         for (unsigned i = 0; i < info->num_inputs; i++) {
            ncomp[i] = info->input_sizes[i] ? info->input_sizes[i] :
                       nir_ssa_alu_instr_src_components(alu, i);
            if (ncomp[i] > 4)
               return ph_fs_fail(ctx, "a preshader operation that is not per component");
            if (!bit_size && !nir_alu_type_get_type_size(info->input_types[i]))
               bit_size = alu->src[i].src.ssa->bit_size;
            for (unsigned c = 0; c < ncomp[i]; c++)
               if (!ph_fs_pre_scalar(ctx, alu->src[i].src.ssa,
                                     alu->src[i].swizzle[c], &vec[i][c]))
                  return false;
         }
         if (!nir_alu_type_get_type_size(info->output_type) && !bit_size)
            bit_size = alu->def.bit_size;
         if (!ph_fs_pre_node_vec(ctx, alu->op, bit_size ? bit_size : 32,
                                 info->num_inputs, ncomp,
                                 (const struct ph_fs_pre_src (*)[4])vec, out))
            return false;
         break;
      }
      /* Unsized operations take the bit size of their unsized sources, as
       * nir_opt_constant_folding does. */
      if (!nir_alu_type_get_type_size(info->output_type))
         bit_size = alu->def.bit_size;
      for (unsigned i = 0; i < info->num_inputs; i++) {
         if (info->input_sizes[i])
            return ph_fs_fail(ctx, "a preshader operation that is not per component");
         if (!bit_size && !nir_alu_type_get_type_size(info->input_types[i]))
            bit_size = alu->src[i].src.ssa->bit_size;
         if (!ph_fs_pre_scalar(ctx, alu->src[i].src.ssa,
                               alu->src[i].swizzle[comp], &src[i]))
            return false;
      }
      if (!ph_fs_pre_node(ctx, alu->op, bit_size ? bit_size : 32,
                          info->num_inputs, src, out))
         return false;
      break;
   }
   if (!PH_FS_GROW(ctx, pre_memo, pre_memos, pre_memo_cap))
      return ph_fs_fail(ctx, "out of memory");
   ctx->pre_memo[ctx->pre_memos++] = (struct ph_fs_pre_memo){ def, comp, *out };
   return true;
}

/* Uniform-only @def as a runtime constant filled by the preshader. */
static bool
ph_fs_preshade(struct ph_fs_ctx *ctx, nir_def *def, int lane, int sel,
               struct ph_fs_src *out)
{
   struct ph_fs_part part;

   if (def->bit_size != 32)
      return ph_fs_fail(ctx, "a preshader value that is no 32-bit float");
   memset(&part, 0, sizeof(part));
   part.lane = lane;
   for (unsigned c = 0; c < ph_fs_lane_channels(lane); c++) {
      unsigned comp = sel < 0 ? c : (unsigned)sel;
      struct ph_fs_pre_src src;

      if (comp >= def->num_components)
         comp = def->num_components - 1;
      if (!ph_fs_pre_scalar(ctx, def, comp, &src))
         return false;
      /* A bare buffer scalar or literal still becomes a result, so every
       * preshaded constant has one address space. */
      if (src.kind != PH_FS_PRE_RESULT &&
          !ph_fs_pre_node(ctx, nir_op_mov, 32, 1, &src, &src))
         return false;
      if (PH_FS_PRE_BASE + src.index > UINT16_MAX)
         return ph_fs_fail(ctx, "a preshader past the addressable constants");
      part.offset[c] = PH_FS_PRE_BASE + src.index;
   }
   return ph_fs_part(ctx, &part, out);
}

static bool
ph_fs_varying(struct ph_fs_ctx *ctx, unsigned location, int lane, int sel,
              struct ph_fs_src *out)
{
   if (location == VARYING_SLOT_COL0) {
      out->index = 0;
   } else if (location == VARYING_SLOT_COL1 ||
              location == VARYING_SLOT_FOGC) {
      /* The vertex side carries the secondary colour or the fog
       * coordinate in colour 1, never both. */
      if ((location == VARYING_SLOT_FOGC ? ctx->uses_col1 : ctx->uses_fogc) ||
          ctx->fog_root)
         return ph_fs_fail(ctx, "fog and the secondary colour both need v1");
      ctx->uses_fogc |= location == VARYING_SLOT_FOGC;
      ctx->uses_col1 |= location == VARYING_SLOT_COL1;
      out->index = 1;
      ctx->uses_color1 = true;
   } else if (ctx->route[0] == (int)location || ctx->route[1] == (int)location) {
      out->index = ctx->route[0] == (int)location ? 0 : 1;
      ctx->uses_color1 |= out->index == 1;
   } else {
      return ph_fs_fail(ctx, "a varying other than a colour");
   }
   out->kind = PH_SRC_COLOUR;
   out->channel = sel;
   ctx->uses_color = true;
   /* An alpha read needs that colour's four-component carrier. */
   if (sel == 3 || (sel < 0 && lane != PH_LANE_RGB)) {
      if (out->index == 1)
         ctx->uses_color1_alpha = true;
      else
         ctx->uses_color0_alpha = true;
   }
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
      return ph_fs_varying(ctx, deref->var->data.location, lane, sel, out);
   }
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
      return ph_fs_varying(ctx, nir_intrinsic_io_semantics(load).location,
                           lane, sel, out);
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

/* ---- the fog factor ------------------------------------------------- */

static int
ph_fs_fog_push(struct ph_fs_ctx *ctx, struct ph_fs_fog_op op)
{
   if (ctx->fog_ops == ctx->fog_cap) {
      unsigned cap = ctx->fog_cap ? 2 * ctx->fog_cap : 8;
      struct ph_fs_fog_op *grown = realloc(ctx->fog, cap * sizeof(*grown));

      if (!grown)
         return -1;
      ctx->fog = grown;
      ctx->fog_cap = cap;
   }
   ctx->fog[ctx->fog_ops] = op;
   return ctx->fog_ops++;
}

/* Channel @comp of @def as a scalar expression over the fog coordinate,
 * uniforms and literals; -1 when it reads anything else.  *@coord is set
 * once the fog coordinate is reached. */
static int
ph_fs_fog_expr(struct ph_fs_ctx *ctx, nir_def *def, unsigned comp, bool *coord)
{
   nir_instr *instr = nir_def_instr(def);
   struct ph_fs_fog_op op = {0};

   switch (instr->type) {
   case nir_instr_type_load_const: {
      const nir_load_const_instr *load = nir_instr_as_load_const(instr);

      if (load->def.bit_size != 32 || comp >= load->def.num_components)
         return -1;
      op.kind = PH_FOG_LITERAL;
      op.value = nir_const_value_as_float(load->value[comp], 32);
      return ph_fs_fog_push(ctx, op);
   }
   case nir_instr_type_intrinsic: {
      const nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
      unsigned location;

      switch (load->intrinsic) {
      case nir_intrinsic_load_input:
      case nir_intrinsic_load_interpolated_input:
         location = nir_intrinsic_io_semantics(load).location;
         break;
      case nir_intrinsic_load_deref: {
         nir_deref_instr *deref = nir_src_as_deref(load->src[0]);

         if (!deref || deref->deref_type != nir_deref_type_var ||
             !(deref->var->data.mode & nir_var_shader_in))
            return -1;
         location = deref->var->data.location;
         break;
      }
      case nir_intrinsic_load_ubo_vec4:
         if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
             !nir_src_is_const(load->src[1]))
            return -1;
         op.kind = PH_FOG_UNIFORM;
         op.offset = (nir_intrinsic_base(load) +
                      nir_src_as_uint(load->src[1])) * 4u +
                     nir_intrinsic_component(load) + comp;
         return ph_fs_fog_push(ctx, op);
      default:
         return -1;
      }
      if (location != VARYING_SLOT_FOGC || comp)
         return -1;
      *coord = true;
      op.kind = PH_FOG_COORD;
      return ph_fs_fog_push(ctx, op);
   }
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      unsigned n = nir_op_infos[alu->op].num_inputs;
      int src[3] = { -1, -1, -1 };

      switch (alu->op) {
      case nir_op_mov:
         return ph_fs_fog_expr(ctx, alu->src[0].src.ssa,
                               alu->src[0].swizzle[comp], coord);
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
         return comp < n ? ph_fs_fog_expr(ctx, alu->src[comp].src.ssa,
                                          alu->src[comp].swizzle[0], coord) :
                           -1;
      case nir_op_fneg: op.kind = PH_FOG_NEG; break;
      case nir_op_fabs: op.kind = PH_FOG_ABS; break;
      case nir_op_fsat: op.kind = PH_FOG_SAT; break;
      case nir_op_fexp2: op.kind = PH_FOG_EXP2; break;
      case nir_op_fadd: op.kind = PH_FOG_ADD; break;
      case nir_op_fmul: op.kind = PH_FOG_MUL; break;
      case nir_op_fmin: op.kind = PH_FOG_MIN; break;
      case nir_op_fmax: op.kind = PH_FOG_MAX; break;
      case nir_op_ffma:
      case nir_op_ffma_weak:
      case nir_op_fmad:
         op.kind = PH_FOG_FMA;
         break;
      default:
         return -1;
      }
      for (unsigned i = 0; i < n; i++) {
         const unsigned c = nir_op_infos[alu->op].input_sizes[i] ? 0 :
                            alu->src[i].swizzle[comp];

         src[i] = ph_fs_fog_expr(ctx, alu->src[i].src.ssa, c, coord);
         if (src[i] < 0)
            return -1;
      }
      op.a = src[0];
      op.b = src[1] < 0 ? 0 : src[1];
      op.c = src[2] < 0 ? 0 : src[2];
      return ph_fs_fog_push(ctx, op);
   }
   default:
      return -1;
   }
}

/* A scalar of the fog coordinate - GL's fog factor as the state tracker
 * lowers it - becomes v1.x, which the vertex side fills with the
 * factor evaluated at each vertex. */
static bool
ph_fs_fog_factor(struct ph_fs_ctx *ctx, nir_alu_instr *sat, int sel,
                 struct ph_fs_src *out)
{
   const unsigned comp = sel < 0 ? 0 : (unsigned)sel;
   const unsigned mark = ctx->fog_ops;
   bool coord = false;

   if (ctx->fog_root && ctx->fog_root != &sat->def)
      return false;
   if (!ctx->fog_root) {
      if (sat->def.num_components != 1 || ctx->uses_fogc || ctx->uses_col1 ||
          ph_fs_fog_expr(ctx, &sat->def, comp, &coord) < 0 || !coord) {
         ctx->fog_ops = mark;
         return false;
      }
      ctx->fog_root = &sat->def;
   }
   *out = (struct ph_fs_src){ .kind = PH_SRC_COLOUR, .channel = 0,
                              .index = 1 };
   ctx->uses_color = ctx->uses_color1 = true;
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

/* RGB channels taken from different values: each value, replicated or in
 * its natural channels, is multiplied by a 0/1 mask of the channels it fills
 * and accumulated with multiply-adds; the literal channels are added last. */
static bool
ph_fs_assemble(struct ph_fs_ctx *ctx, nir_def *const defs[3],
               const uint8_t comps[3], int lane, float *literal,
               struct ph_fs_src *out)
{
   struct { nir_def *def; int sel; float mask[3]; } group[3];
   unsigned groups = 0;
   bool have_acc = false, have_literal = false;
   struct ph_fs_src acc;

   if (lane != PH_LANE_RGB)
      return ph_fs_fail(ctx, "RGB assembled from separate values");
   for (unsigned i = 0; i < 3; i++) {
      nir_def *def = defs[i];
      nir_instr *instr = nir_def_instr(def);
      int sel = comps[i] == i ? -1 : comps[i];
      unsigned g;

      literal[i] = 0.0f;
      if (instr->type == nir_instr_type_load_const) {
         literal[i] = nir_const_value_as_float(
            nir_instr_as_load_const(instr)->value[comps[i]], 32);
         have_literal |= literal[i] != 0.0f;
         continue;
      }
      for (g = 0; g < groups; g++)
         if (group[g].def == def && group[g].sel == sel)
            break;
      if (g == groups) {
         memset(&group[g], 0, sizeof(group[g]));
         group[g].def = def;
         group[g].sel = sel;
         groups++;
      }
      group[g].mask[i] = 1.0f;
   }
   if (!groups)
      return ph_fs_literal(ctx, lane, literal, out);
   for (unsigned g = 0; g < groups; g++) {
      struct ph_fs_src term, mask;

      if (!ph_fs_lower(ctx, group[g].def, lane, group[g].sel, &term))
         return false;
      if (group[g].mask[0] && group[g].mask[1] && group[g].mask[2]) {
         acc = term;
         have_acc = true;
         continue;
      }
      if (!ph_fs_literal(ctx, lane, group[g].mask, &mask))
         return false;
      if (!(have_acc ? ph_fs_node(ctx, lane, P3D_COMB_MAD, &term, &acc, &mask, &acc) :
                       ph_fs_node(ctx, lane, P3D_COMB_MUL, &term, NULL, &mask, &acc)))
         return false;
      have_acc = true;
   }
   if (have_literal) {
      struct ph_fs_src lit;

      if (!ph_fs_literal(ctx, lane, literal, &lit) ||
          !ph_fs_node(ctx, lane, P3D_COMB_ADD, &acc, &lit, NULL, &acc))
         return false;
   }
   *out = acc;
   return true;
}

static bool
ph_fs_operation(struct ph_fs_ctx *ctx, nir_alu_instr *alu, int lane, int sel,
                struct ph_fs_src *out)
{
   struct ph_fs_src a, b, c, d;

   /* The fog factor's outermost step: the saturate, or the exponential
    * when range analysis has removed a saturate it cannot need. */
   if ((alu->op == nir_op_fsat || alu->op == nir_op_fexp2) &&
       ph_fs_fog_factor(ctx, alu, sel, out))
      return true;
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fcanonicalize:
      /* A channel permutation the replicates cannot express (.abgr) is
       * rebuilt from replicated channels and masks. */
      if (lane == PH_LANE_RGB && sel < 0 &&
          !ph_fs_translate(alu, 0, lane, sel, &(int){0})) {
         nir_def *defs[3] = { alu->src[0].src.ssa, alu->src[0].src.ssa,
                              alu->src[0].src.ssa };
         float literal[3];

         return ph_fs_assemble(ctx, defs, alu->src[0].swizzle, lane, literal,
                               out);
      }
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
   case nir_op_bcsel: {
      /* Without native integers a boolean is 0.0 or 1.0 (b2b1 of a float),
       * so b ? x : y is -b >= 0 ? y : x. */
      nir_instr *cond = nir_def_instr(alu->src[0].src.ssa);
      nir_alu_instr *b2b;
      int req;

      if (ph_fs_uniform_only(alu->src[0].src.ssa, 0)) {
         /* A condition of uniforms only: the preshader turns it into a 0/1
          * float per channel. */
         struct ph_fs_part part;

         memset(&part, 0, sizeof(part));
         part.lane = lane;
         for (unsigned ch = 0; ch < ph_fs_lane_channels(lane); ch++) {
            struct ph_fs_pre_src cs, f;
            unsigned comp = alu->src[0].swizzle[sel < 0 ? ch : (unsigned)sel];

            if (!ph_fs_pre_scalar(ctx, alu->src[0].src.ssa, comp, &cs) ||
                !ph_fs_pre_node(ctx, nir_op_b2f32,
                                alu->src[0].src.ssa->bit_size, 1, &cs, &f))
               return false;
            part.offset[ch] = PH_FS_PRE_BASE + f.index;
         }
         return ph_fs_part(ctx, &part, &a) &&
                ph_fs_operand(ctx, alu, 1, lane, sel, &b) &&
                ph_fs_operand(ctx, alu, 2, lane, sel, &c) &&
                ph_fs_apply(ctx, lane, &a, P3D_COMB_MOD_NEG) &&
                ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &c, &b, out);
      }
      if (cond->type != nir_instr_type_alu)
         return ph_fs_fail(ctx, "a select on a condition that is no boolean value");
      b2b = nir_instr_as_alu(cond);
      if (!ph_fs_translate(alu, 0, lane, sel, &req))
         return ph_fs_fail(ctx, "a swizzle the replicates cannot express");
      if (b2b->op == nir_op_flt || b2b->op == nir_op_fge) {
         /* x < y ? b : c is x - y >= 0 ? c : b; x >= y ? b : c directly. */
         if (!ph_fs_operand(ctx, b2b, 0, lane, req, &a) ||
             !ph_fs_operand(ctx, b2b, 1, lane, req, &d) ||
             !ph_fs_apply(ctx, lane, &d, P3D_COMB_MOD_NEG) ||
             !ph_fs_node(ctx, lane, P3D_COMB_ADD, &a, &d, NULL, &a) ||
             !ph_fs_operand(ctx, alu, 1, lane, sel, &b) ||
             !ph_fs_operand(ctx, alu, 2, lane, sel, &c))
            return false;
         return b2b->op == nir_op_fge ?
                ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &b, &c, out) :
                ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &c, &b, out);
      }
      if (b2b->op == nir_op_feq || b2b->op == nir_op_fneu) {
         /* x == y holds where both x - y >= 0 and y - x >= 0. */
         struct ph_fs_src t, n;
         const bool eq = b2b->op == nir_op_feq;

         if (!ph_fs_operand(ctx, b2b, 0, lane, req, &a) ||
             !ph_fs_operand(ctx, b2b, 1, lane, req, &d) ||
             !ph_fs_apply(ctx, lane, &d, P3D_COMB_MOD_NEG) ||
             !ph_fs_node(ctx, lane, P3D_COMB_ADD, &a, &d, NULL, &a) ||
             !ph_fs_operand(ctx, alu, eq ? 1 : 2, lane, sel, &b) ||
             !ph_fs_operand(ctx, alu, eq ? 2 : 1, lane, sel, &c) ||
             !ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &b, &c, &t))
            return false;
         n = a;
         return ph_fs_apply(ctx, lane, &n, P3D_COMB_MOD_NEG) &&
                ph_fs_node(ctx, lane, P3D_COMB_CMP, &n, &t, &c, out);
      }
      if (b2b->op != nir_op_b2b1)
         return ph_fs_fail(ctx, "a select on a condition that is no boolean value");
      return ph_fs_operand(ctx, b2b, 0, lane, req, &a) &&
             ph_fs_operand(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_operand(ctx, alu, 2, lane, sel, &c) &&
             ph_fs_apply(ctx, lane, &a, P3D_COMB_MOD_NEG) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &c, &b, out);
   }
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
      {
         nir_def *defs[3] = { alu->src[0].src.ssa, alu->src[1].src.ssa,
                              alu->src[2].src.ssa };
         const uint8_t comps[3] = { alu->src[0].swizzle[0],
                                    alu->src[1].swizzle[0],
                                    alu->src[2].swizzle[0] };

         return ph_fs_assemble(ctx, defs, comps, lane, value, out);
      }
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
   case nir_instr_type_undef: {
      /* An undefined value may be anything; zero is as good as any. */
      static const float zero[3] = { 0 };

      return ph_fs_literal(ctx, lane, zero, out);
   }
   case nir_instr_type_intrinsic:
      return ph_fs_intrinsic(ctx, nir_instr_as_intrinsic(instr), lane, sel,
                             out);
   case nir_instr_type_alu:
      if (ph_fs_uniform_only(def, 0))
         return ph_fs_preshade(ctx, def, lane, sel, out);
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

/* ---- register range --------------------------------------------------- */

static unsigned
ph_fs_uexp_of(const struct ph_fs_uexp *uexp, unsigned count, unsigned offset)
{
   for (unsigned i = 0; i < count; i++)
      if (uexp[i].offset == offset)
         return uexp[i].e;
   return 0;
}

/* The exponent a runtime constant part is read with: one per register
 * half, the largest of its channels'. */
static unsigned
ph_fs_part_exp(const struct ph_fs_ctx *ctx, const struct ph_fs_part *p)
{
   unsigned e = 0;

   for (unsigned c = 0; !p->literal && c < ph_fs_lane_channels(p->lane); c++)
      e = MAX2(e, ph_fs_uexp_of(ctx->uexp, ctx->uexp_count, p->offset[c]));
   return e;
}

/* MEASURED on the P690: every source reads, and every register write
 * clamps to, [-1, 1] - intermediates of 1.5, 2.5 and 7 read back as 1.0 -
 * while the ALU computes past 1 before the result shift ((1 + 0.75) * 0.5
 * gave 0.875).  Each node therefore stores its value scaled by 2^-e, with
 * e picked from interval analysis; consumers align exponents, and the shift
 * folds the scale back in at the saturated colour write. */
/* Per storage channel: RGB 0..2, alpha 3. */
struct ph_fs_range {
   float lo[4], hi[4];
   int e;
};

static struct ph_fs_range
ph_fs_range_mod(struct ph_fs_range r, unsigned mod)
{
   for (unsigned c = 0; c < 4; c++) {
      const float a = ph_fs_modify(r.lo[c], mod), b = ph_fs_modify(r.hi[c], mod);

      r.lo[c] = MIN2(a, b);
      r.hi[c] = MAX2(a, b);
   }
   return r;
}

static struct ph_fs_range
ph_fs_range_mul(struct ph_fs_range a, struct ph_fs_range b)
{
   struct ph_fs_range r = { .e = a.e + b.e };

   for (unsigned c = 0; c < 4; c++) {
      const float p[4] = { a.lo[c] * b.lo[c], a.lo[c] * b.hi[c],
                           a.hi[c] * b.lo[c], a.hi[c] * b.hi[c] };

      r.lo[c] = r.hi[c] = p[0];
      for (unsigned i = 1; i < 4; i++) {
         r.lo[c] = MIN2(r.lo[c], p[i]);
         r.hi[c] = MAX2(r.hi[c], p[i]);
      }
   }
   return r;
}

static struct ph_fs_range
ph_fs_range_hull(struct ph_fs_range a, struct ph_fs_range b)
{
   for (unsigned c = 0; c < 4; c++) {
      a.lo[c] = MIN2(a.lo[c], b.lo[c]);
      a.hi[c] = MAX2(a.hi[c], b.hi[c]);
   }
   return a;
}

static struct ph_fs_range
ph_fs_range_add(struct ph_fs_range a, struct ph_fs_range b)
{
   for (unsigned c = 0; c < 4; c++) {
      a.lo[c] += b.lo[c];
      a.hi[c] += b.hi[c];
   }
   return a;
}

static struct ph_fs_range
ph_fs_range_scale(struct ph_fs_range r, float k)
{
   for (unsigned c = 0; c < 4; c++) {
      r.lo[c] *= k;
      r.hi[c] *= k;
   }
   return r;
}

static void
ph_fs_range_fill(struct ph_fs_range *r, float lo, float hi)
{
   for (unsigned c = 0; c < 4; c++) {
      r->lo[c] = lo;
      r->hi[c] = hi;
   }
}

static bool
ph_fs_src_range(struct ph_fs_ctx *ctx, const struct ph_fs_range *nr,
                const struct ph_fs_src *s, struct ph_fs_range *out)
{
   struct ph_fs_range r = { .e = 0 };

   ph_fs_range_fill(&r, 0.0f, 1.0f);
   switch (s->kind) {
   case PH_SRC_NONE:
      ph_fs_range_fill(&r, 0.0f, 0.0f);
      break;
   case PH_SRC_NODE:
      r = nr[s->index];
      break;
   case PH_SRC_TEX:
      for (unsigned c = 0; s->index < 4 && c < 4; c++)
         r.hi[c] = ctx->tex_hi[s->index][c];
      break;
   case PH_SRC_CONST: {
      const struct ph_fs_part *p = &ctx->part[s->index];

      if (!p->literal) {
         ph_fs_range_fill(&r, -1.0f, 1.0f);
         r.e = ph_fs_part_exp(ctx, p);
         break;
      }
      for (unsigned c = 0; c < 4; c++)
         r.lo[c] = r.hi[c] = p->value[p->lane == PH_LANE_A ? 0 : MIN2(c, 2)];
      break;
   }
   default:
      break;
   }
   /* A replicate reads one channel everywhere. */
   if (s->channel >= 0 && s->channel < 4) {
      const float lo = r.lo[s->channel], hi = r.hi[s->channel];

      ph_fs_range_fill(&r, lo, hi);
   }
   if (r.e && s->mod != P3D_COMB_MOD_NONE && s->mod != P3D_COMB_MOD_NEG)
      return ph_fs_fail(ctx, "a modifier on an intermediate beyond the "
                             "register range");
   *out = ph_fs_range_mod(r, s->mod);
   return true;
}

struct ph_fs_rebuild {
   struct ph_fs_node *node;
   struct ph_fs_range *range;
   unsigned nodes, cap;
};

static bool
ph_fs_rebuild_push(struct ph_fs_ctx *ctx, struct ph_fs_rebuild *rb,
                   const struct ph_fs_node *node, struct ph_fs_range range)
{
   if (rb->nodes == rb->cap) {
      const unsigned cap = rb->cap ? rb->cap * 2 : 16;
      struct ph_fs_node *n = realloc(rb->node, cap * sizeof(*n));
      struct ph_fs_range *r;

      if (!n)
         return ph_fs_fail(ctx, "out of memory");
      rb->node = n;
      r = realloc(rb->range, cap * sizeof(*r));
      if (!r)
         return ph_fs_fail(ctx, "out of memory");
      rb->range = r;
      rb->cap = cap;
   }
   rb->node[rb->nodes] = *node;
   rb->range[rb->nodes++] = range;
   return true;
}

/* Raise operand @s's exponent by @d: a literal is rescaled in place, any
 * other value goes through a multiply by 2^-d placed before its consumer. */
static bool
ph_fs_range_lower(struct ph_fs_ctx *ctx, struct ph_fs_rebuild *rb, int lane,
                  struct ph_fs_src *s, struct ph_fs_range *r, int d)
{
   const float k = ldexpf(1.0f, -d);
   struct ph_fs_range scaled = ph_fs_range_scale(*r, k);

   if (d <= 0)
      return true;
   scaled.e = r->e + d;
   if (d > 10)
      return ph_fs_fail(ctx, "an intermediate beyond the register range");
   if (s->kind == PH_SRC_NONE) {
      r->e += d;
      return true;
   }
   /* A producer only this operand reads stores itself further scaled down
    * when its shift allows, which needs no instruction. */
   if (s->kind == PH_SRC_NODE && s->mod == P3D_COMB_MOD_NONE) {
      struct ph_fs_node *dep = &rb->node[s->index];
      const int sh = dep->scale < 4 ? dep->scale : (int)dep->scale - 8;

      if (dep->uses == 1 && !dep->sat && !dep->output && sh - d >= -2) {
         dep->scale = sh - d >= 0 ? sh - d : 8 + sh - d;
         rb->range[s->index] = scaled;
         *r = scaled;
         return true;
      }
   }
   if (s->kind == PH_SRC_CONST && ctx->part[s->index].literal) {
      struct ph_fs_part part = ctx->part[s->index];
      const int8_t channel = s->channel;

      /* The modifier is applied before the scale, so it folds in first. */
      for (unsigned c = 0; c < ph_fs_lane_channels(part.lane); c++) {
         part.value[c] = ph_fs_modify(part.value[c], s->mod) * k;
         if (fabsf(part.value[c]) > 1.0f)
            return ph_fs_fail(ctx, "a literal outside the constant range");
      }
      if (!ph_fs_part(ctx, &part, s))
         return false;
      s->channel = channel;
   } else {
      struct ph_fs_node mul;
      struct ph_fs_src lit;

      if (lane == PH_LANE_BOTH)
         return ph_fs_fail(ctx, "an intermediate beyond the register range");
      if (!ph_fs_literal_scalar(ctx, lane, k, &lit))
         return false;
      memset(&mul, 0, sizeof(mul));
      mul.lane = lane;
      mul.op = P3D_COMB_MUL;
      mul.refs = 1;
      mul.slot = -1;
      mul.end[0] = mul.end[1] = -1;
      mul.src[0] = *s;
      mul.src[2] = lit;
      if (!ph_fs_rebuild_push(ctx, rb, &mul, scaled))
         return false;
      *s = (struct ph_fs_src){ .kind = PH_SRC_NODE, .channel = -1,
                               .index = rb->nodes - 1 };
   }
   *r = scaled;
   return true;
}

static bool
ph_fs_range_align(struct ph_fs_ctx *ctx, struct ph_fs_rebuild *rb, int lane,
                  struct ph_fs_src *a, struct ph_fs_range *ra,
                  struct ph_fs_src *b, struct ph_fs_range *rb_)
{
   return ra->e < rb_->e ?
          ph_fs_range_lower(ctx, rb, lane, a, ra, rb_->e - ra->e) :
          ph_fs_range_lower(ctx, rb, lane, b, rb_, ra->e - rb_->e);
}

static bool
ph_fs_range_pass(struct ph_fs_ctx *ctx)
{
   struct ph_fs_rebuild rb = { 0 };
   unsigned *map = calloc(MAX2(ctx->nodes, 1), sizeof(*map));
   bool ok = false;

   if (!map)
      return ph_fs_fail(ctx, "out of memory");
   for (unsigned i = 0; i < ctx->nodes; i++) {
      struct ph_fs_node n = ctx->node[i];
      struct ph_fs_range r[3], f;
      const int so = n.scale < 4 ? n.scale : (int)n.scale - 8;
      int sh, e = 0;

      for (unsigned s = 0; s < 3; s++) {
         if (n.src[s].kind == PH_SRC_NODE)
            n.src[s].index = map[n.src[s].index];
         if (!ph_fs_src_range(ctx, rb.range, &n.src[s], &r[s]))
            goto out;
      }
      switch (n.op) {
      case P3D_COMB_MOV:
         f = r[0];
         break;
      case P3D_COMB_MUL:
         f = ph_fs_range_mul(r[0], r[2]);
         break;
      case P3D_COMB_DP3:
      case P3D_COMB_DP4: {
         const struct ph_fs_range p = ph_fs_range_mul(r[0], r[2]);
         float lo = 0.0f, hi = 0.0f;

         for (unsigned c = 0; c < (n.op == P3D_COMB_DP3 ? 3u : 4u); c++) {
            lo += p.lo[c];
            hi += p.hi[c];
         }
         f = p;
         ph_fs_range_fill(&f, lo, hi);
         break;
      }
      case P3D_COMB_MAD: {
         struct ph_fs_range p = ph_fs_range_mul(r[0], r[2]);

         if (p.e < r[1].e) {
            /* Rescale the product through whichever factor is cheaper. */
            const unsigned which = n.src[2].kind == PH_SRC_CONST ? 2 : 0;

            if (!ph_fs_range_lower(ctx, &rb, n.lane, &n.src[which], &r[which],
                                   r[1].e - p.e))
               goto out;
            p = ph_fs_range_mul(r[0], r[2]);
         } else if (!ph_fs_range_lower(ctx, &rb, n.lane, &n.src[1], &r[1],
                                       p.e - r[1].e)) {
            goto out;
         }
         f = ph_fs_range_add(p, r[1]);
         break;
      }
      case P3D_COMB_ADD:
      case P3D_COMB_SUB:
         if (!ph_fs_range_align(ctx, &rb, n.lane, &n.src[0], &r[0],
                                &n.src[1], &r[1]))
            goto out;
         f = n.op == P3D_COMB_ADD ? ph_fs_range_add(r[0], r[1]) :
             ph_fs_range_add(r[0], ph_fs_range_mod(r[1], P3D_COMB_MOD_NEG));
         break;
      case P3D_COMB_LRP: {
         struct ph_fs_range inv = ph_fs_range_mod(r[2], P3D_COMB_MOD_INVERT);

         if (r[2].e) {
            ph_fs_fail(ctx, "an interpolant beyond the register range");
            goto out;
         }
         if (!ph_fs_range_align(ctx, &rb, n.lane, &n.src[0], &r[0],
                                &n.src[1], &r[1]))
            goto out;
         f = ph_fs_range_add(ph_fs_range_mul(r[0], r[2]),
                             ph_fs_range_mul(r[1], inv));
         f.e = r[0].e;
         break;
      }
      case P3D_COMB_CMP:
      case P3D_COMB_CND:
         if (n.op == P3D_COMB_CND && r[0].e) {
            ph_fs_fail(ctx, "a condition beyond the register range");
            goto out;
         }
         if (!ph_fs_range_align(ctx, &rb, n.lane, &n.src[1], &r[1],
                                &n.src[2], &r[2]))
            goto out;
         f = ph_fs_range_hull(r[1], r[2]);
         break;
      default:
         if (r[0].e || r[1].e || r[2].e) {
            ph_fs_fail(ctx, "an intermediate beyond the register range");
            goto out;
         }
         f.e = 0;
         ph_fs_range_fill(&f, -1.0f, 1.0f);
         break;
      }
      /* The true value is f * 2^(f.e + so); store it * 2^-e. */
      {
         const unsigned first = n.lane == PH_LANE_A ? 3 : 0;
         const unsigned last = n.lane == PH_LANE_RGB ? 2 : 3;
         float m = 0.0f;

         f = ph_fs_range_scale(f, ldexpf(1.0f, f.e + so));
         for (unsigned c = first; !n.sat && c <= last; c++)
            m = MAX3(m, fabsf(f.lo[c]), fabsf(f.hi[c]));
         while (m > 1.0f + 1e-6f) {
            m *= 0.5f;
            e++;
         }
         sh = f.e + so - e;
         if (getenv("PARHELIA_TRACE_RANGE"))
            fprintf(stderr, "range: node %u op %#x lane %u sat %u so %d fe %d "
                    "-> e %d sh %d max %g\n", i, n.op, n.lane, n.sat, so, f.e,
                    e, sh, ldexpf(m, e));
         if (sh < -2 || sh > 3) {
            ph_fs_fail(ctx, "a scale beyond the result shift");
            goto out;
         }
         n.scale = sh >= 0 ? sh : 8 + sh;
         f = ph_fs_range_scale(f, ldexpf(1.0f, -e));
         for (unsigned c = 0; c < 4; c++) {
            f.lo[c] = CLAMP(f.lo[c], n.sat ? 0.0f : -1.0f, 1.0f);
            f.hi[c] = CLAMP(f.hi[c], n.sat ? 0.0f : -1.0f, 1.0f);
         }
         f.e = e;
      }
      if (!ph_fs_rebuild_push(ctx, &rb, &n, f))
         goto out;
      map[i] = rb.nodes - 1;
   }
   free(ctx->node);
   ctx->node = rb.node;
   ctx->nodes = rb.nodes;
   ctx->node_cap = rb.cap;
   rb.node = NULL;
   ok = true;
out:
   free(rb.node);
   free(rb.range);
   free(map);
   return ok;
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
 * within the Q1.10 range.  A source reads at most [-1, 1]: MEASURED on the
 * P690, "1 - c" with c = -1 read as 1, not 2, so a literal outside that
 * range has no encoding and the caller has to build it differently. */
static bool
ph_fs_encode(const float *value, unsigned n, uint8_t *mod, float *stored)
{
   static const uint8_t mods[] = {
      P3D_COMB_MOD_NONE, P3D_COMB_MOD_INVERT, P3D_COMB_MOD_NEG,
      P3D_COMB_MOD_SIGNED, P3D_COMB_MOD_BIAS,
   };

   for (unsigned c = 0; c < n; c++)
      if (!(value[c] >= -1.0f && value[c] <= 1.0f))
         return false;

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
            ref->e[3] = ph_fs_part_exp(ctx, p);
         } else {
            ref->mask |= 7u;
            memcpy(ref->offset, p->offset, 3 * sizeof(p->offset[0]));
            ref->e[0] = ref->e[1] = ref->e[2] = ph_fs_part_exp(ctx, p);
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
ph_fs_classify(const struct ph_fs_program *prog, uint8_t *alpha_one_mask)
{
   static const enum ph_fs_class classes[] = {
      PH_FS_CLASS_COLOUR, PH_FS_CLASS_REPLACE, PH_FS_CLASS_MODULATE,
      PH_FS_CLASS_DECAL, PH_FS_CLASS_ADD, PH_FS_CLASS_MODULATE2,
      PH_FS_CLASS_ADD2, PH_FS_CLASS_REPLACE1, PH_FS_CLASS_MODULATE2_TEX,
   };
   uint32_t seed = 0x2545f491;

   *alpha_one_mask = 0;
   if (prog->const_ref_count)
      return PH_FS_CLASS_OTHER;
   /* Masks 1..3 pin a unit's texture alpha to 1.0; only the two-unit
    * classes take them, the one-unit ones keep their exact match. */
   for (unsigned m = 0; m < 4; m++)
   for (unsigned k = 0; k < sizeof(classes) / sizeof(classes[0]); k++) {
      const enum ph_fs_class cls = classes[k];
      bool match = true;

      if (m && cls != PH_FS_CLASS_MODULATE2 && cls != PH_FS_CLASS_ADD2 &&
          cls != PH_FS_CLASS_REPLACE1 && cls != PH_FS_CLASS_MODULATE2_TEX)
         continue;

      for (unsigned trial = 0; trial < 16 && match; trial++) {
         float tex[4][4], colour[2][4], got[4], want[4];
         const float *t0 = tex[0], *t1 = tex[1], *v = colour[0];

         for (unsigned i = 0; i < 24; i++) {
            seed = seed * 1103515245u + 12345u;
            (i < 16 ? tex[i / 4] : colour[(i - 16) / 4])[i % 4] =
               ((seed >> 16) & 0xff) / 255.0f;
         }
         for (unsigned u = 0; u < 2; u++)
            if (m & (1u << u))
               tex[u][3] = 1.0f;
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
            case PH_FS_CLASS_MODULATE2_TEX: want[c] = t0[c] * t1[c]; break;
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
      if (match) {
         *alpha_one_mask = m;
         return cls;
      }
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

/* Generic varyings the program reads as values, assigned to the colour
 * interpolators it does not read itself: v0 first, then v1.  Texture
 * coordinate varyings only feed the texture units and are not routed. */
static bool
ph_fs_route_varyings(nir_shader *nir, int route[2])
{
   bool used_col0 = false, used_col1 = false;
   int generic[2];
   unsigned generics = 0;
   bool too_many = false;

   route[0] = route[1] = -1;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;
            int location = -1;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_load_deref) {
               nir_deref_instr *deref = nir_src_as_deref(in->src[0]);

               if (deref && deref->deref_type == nir_deref_type_var &&
                   (deref->var->data.mode & nir_var_shader_in))
                  location = deref->var->data.location;
            } else if (in->intrinsic == nir_intrinsic_load_input ||
                       in->intrinsic == nir_intrinsic_load_interpolated_input) {
               location = nir_intrinsic_io_semantics(in).location;
            }
            if (location == VARYING_SLOT_COL0)
               used_col0 = true;
            else if (location == VARYING_SLOT_COL1 ||
                     location == VARYING_SLOT_FOGC)
               used_col1 = true;
            else if (location >= VARYING_SLOT_VAR0 ||
                     location == VARYING_SLOT_PNTC) {
               bool seen = false;

               for (unsigned i = 0; i < generics; i++)
                  seen |= generic[i] == location;
               if (!seen && generics < 2)
                  generic[generics++] = location;
               else if (!seen)
                  too_many = true;
            }
         }
      }
   }
   if (too_many)
      return false;
   for (unsigned i = 0; i < generics; i++) {
      if (!used_col0 && route[0] < 0)
         route[0] = generic[i];
      else if (!used_col1 && route[1] < 0)
         route[1] = generic[i];
      else
         return false;
   }
   return true;
}

static bool
ph_fs_compile_key(nir_shader *nir, unsigned max_instructions,
                  const struct ph_fs_uexp *uexp, unsigned uexp_count,
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
   ctx.uexp = uexp;
   ctx.uexp_count = uexp_count;
   for (unsigned u = 0; u < 4; u++)
      for (unsigned c = 0; c < 4; c++)
         ctx.tex_hi[u][c] = 1.0f;
   if (!ph_fs_route_varyings(nir, ctx.route)) {
      ph_fs_fail(&ctx, "more value varyings than free colour interpolators");
      goto done;
   }
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
   /* Channels the shader never writes are undefined; they read as zero. */
   static const float zero[3] = { 0 };
   const bool rgb_unwritten = !colour[0] && !colour[1] && !colour[2];

   if (!rgb_unwritten && (!colour[0] || !colour[1] || !colour[2])) {
      ph_fs_fail(&ctx, "an RGB write that leaves channels undefined");
      goto done;
   }
   if (!rgb_unwritten && (colour[0] != colour[1] || colour[1] != colour[2])) {
      ph_fs_fail(&ctx, "RGB written from separate values");
      goto done;
   }
   if (rgb_unwritten || (channel[0] == 0 && channel[1] == 1 && channel[2] == 2))
      rgb_sel = -1;
   else if (channel[0] == channel[1] && channel[1] == channel[2])
      rgb_sel = channel[0];
   else {
      ph_fs_fail(&ctx, "an RGB write the replicates cannot express");
      goto done;
   }
   if (!(rgb_unwritten ? ph_fs_literal(&ctx, PH_LANE_RGB, zero, &rgb) :
                         ph_fs_lower(&ctx, colour[0], PH_LANE_RGB, rgb_sel, &rgb)) ||
       !(colour[3] ? ph_fs_lower(&ctx, colour[3], PH_LANE_A, channel[3], &alpha) :
                     ph_fs_literal(&ctx, PH_LANE_A, zero, &alpha)) ||
       !ph_fs_result(&ctx, PH_LANE_RGB, &rgb) ||
       !ph_fs_result(&ctx, PH_LANE_A, &alpha) ||
       !ph_fs_range_pass(&ctx) ||
       !ph_fs_schedule(&ctx, MIN2(max_instructions, P3D_COMB_MAX_INSTRUCTIONS),
                       &count) ||
       !ph_fs_allocate(&ctx, count) || !ph_fs_emit(&ctx, count, out))
      goto done;
   out->sampler_mask = ctx.sampler_mask;
   out->uses_color = ctx.uses_color;
   out->uses_color1 = ctx.uses_color1;
   out->uses_fogc = ctx.uses_fogc || ctx.fog_root;
   out->uses_color1_alpha = ctx.uses_color1_alpha;
   out->uses_color0_alpha = ctx.uses_color0_alpha;
   out->route[0] = ctx.route[0];
   out->route[1] = ctx.route[1];
   if (ctx.fog_root) {
      out->fog = ctx.fog;
      out->fog_ops = ctx.fog_ops;
      ctx.fog = NULL;
   }
   if (ctx.pres) {
      out->pre_value = calloc(ctx.pres, sizeof(*out->pre_value));
      if (!out->pre_value) {
         ph_fs_fail(&ctx, "out of memory");
         goto done;
      }
      out->pre = ctx.pre;
      out->pre_count = ctx.pres;
      ctx.pre = NULL;
   }
   out->valid = true;
   out->cls = ph_fs_classify(out, &out->alpha_one_mask);
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
   free(ctx.pre);
   free(ctx.pre_memo);
   free(ctx.fog);
   return ok;
}

float
ph_fs_fog_eval_step(const struct ph_fs_program *program, unsigned step,
                    float fogc, const void *constants, unsigned size)
{
   if (step >= program->fog_ops)
      return 1.0f;

   float v[step + 1];

   for (unsigned i = 0; i <= step; i++) {
      const struct ph_fs_fog_op *op = &program->fog[i];

      switch (op->kind) {
      case PH_FOG_COORD: v[i] = fogc; break;
      case PH_FOG_UNIFORM:
         v[i] = (op->offset + 1u) * 4u <= size ?
                ((const float *)constants)[op->offset] : 0.0f;
         break;
      case PH_FOG_LITERAL: v[i] = op->value; break;
      case PH_FOG_NEG: v[i] = -v[op->a]; break;
      case PH_FOG_ABS: v[i] = fabsf(v[op->a]); break;
      case PH_FOG_ADD: v[i] = v[op->a] + v[op->b]; break;
      case PH_FOG_MUL: v[i] = v[op->a] * v[op->b]; break;
      case PH_FOG_FMA: v[i] = v[op->a] * v[op->b] + v[op->c]; break;
      case PH_FOG_EXP2: v[i] = exp2f(v[op->a]); break;
      case PH_FOG_MIN: v[i] = fminf(v[op->a], v[op->b]); break;
      case PH_FOG_MAX: v[i] = fmaxf(v[op->a], v[op->b]); break;
      case PH_FOG_SAT: v[i] = CLAMP(v[op->a], 0.0f, 1.0f); break;
      default: v[i] = 0.0f; break;
      }
   }
   return v[step];
}

float
ph_fs_fog_eval(const struct ph_fs_program *program, float fogc,
               const void *constants, unsigned size)
{
   return program->fog_ops ?
      ph_fs_fog_eval_step(program, program->fog_ops - 1, fogc, constants, size) :
      1.0f;
}

bool
ph_fs_compile(nir_shader *nir, unsigned max_instructions,
              struct ph_fs_program *out)
{
   if (!ph_fs_compile_key(nir, max_instructions, NULL, 0, out))
      return false;
   /* Only runtime constants can ask for a variant. */
   if (out->const_ref_count) {
      out->nir = nir_shader_clone(NULL, nir);
      out->budget = max_instructions;
   }
   return true;
}

void
ph_fs_program_fini(struct ph_fs_program *program)
{
   ralloc_free(program->nir);
   program->nir = NULL;
   free(program->uexp);
   program->uexp = NULL;
   program->uexp_count = 0;
   free(program->fog);
   program->fog = NULL;
   program->fog_ops = 0;
   free(program->pre);
   free(program->pre_value);
   program->pre = NULL;
   program->pre_value = NULL;
   program->pre_count = 0;
}

/* Run the preshader over the constant buffer. */
static void
ph_fs_run_preshader(struct ph_fs_program *program, const float *values,
                    unsigned size)
{
   for (unsigned i = 0; i < program->pre_count; i++) {
      const struct ph_fs_pre_node *node = &program->pre[i];
      nir_const_value src[3][NIR_MAX_VEC_COMPONENTS];
      nir_const_value *srcs[3];
      uint16_t poison = 0;

      memset(src, 0, sizeof(src));
      for (unsigned s = 0; s < node->nsrc; s++) {
         for (unsigned c = 0; c < node->ncomp[s]; c++) {
            const struct ph_fs_pre_src *in = &node->src[s][c];

            if (in->kind == PH_FS_PRE_BUFFER)
               src[s][c].f32 = values && (in->index + 1u) * sizeof(float) <= size ?
                               values[in->index] : 0.0f;
            else if (in->kind == PH_FS_PRE_RESULT)
               src[s][c] = program->pre_value[in->index];
            else
               src[s][c] = in->literal;
         }
         srcs[s] = src[s];
      }
      if (node->op == nir_op_mov)
         program->pre_value[i] = src[0][0];
      /* A reduction evaluates once over its sources' width. */
      else
         nir_eval_const_opcode(node->op, &program->pre_value[i], &poison, 1,
                               node->bit_size, srcs, 0);
   }
}

static float
ph_fs_ref_value(const struct ph_fs_program *program,
                const struct ph_fs_const_ref *ref, unsigned c,
                const float *values, unsigned size)
{
   if (ref->offset[c] >= PH_FS_PRE_BASE)
      return ref->offset[c] - PH_FS_PRE_BASE < program->pre_count ?
             program->pre_value[ref->offset[c] - PH_FS_PRE_BASE].f32 : 0.0f;
   return values && (ref->offset[c] + 1u) * sizeof(float) <= size ?
          values[ref->offset[c]] : 0.0f;
}

/* Recompile for the exponents the current constants need, which only grow,
 * so a program settles after a few variants rather than alternating.
 * Returns 1 when it recompiled, 0 when nothing changed, -1 on failure. */
static int
ph_fs_rescale(struct ph_fs_program *program, const float *values,
              unsigned size)
{
   struct ph_fs_uexp *uexp;
   struct ph_fs_program variant;
   unsigned count = program->uexp_count;
   bool grown = false;

   if (!program->nir)
      return 0;
   uexp = malloc((count + 4 * program->const_ref_count + 1) * sizeof(*uexp));
   if (!uexp)
      return -1;
   memcpy(uexp, program->uexp, count * sizeof(*uexp));
   for (unsigned i = 0; i < program->const_ref_count; i++) {
      const struct ph_fs_const_ref *ref = &program->const_ref[i];

      for (unsigned c = 0; c < 4; c++) {
         float m;
         unsigned e = 0, k;

         if (!(ref->mask & (1u << c)))
            continue;
         m = fabsf(ph_fs_ref_value(program, ref, c, values, size));
         while (m > 1.0f + 1e-6f && e < 16) {
            m *= 0.5f;
            e++;
         }
         if (e <= ref->e[c])
            continue;
         for (k = 0; k < count && uexp[k].offset != ref->offset[c]; k++)
            ;
         if (k == count)
            uexp[count++] = (struct ph_fs_uexp){ ref->offset[c], 0 };
         if (e > uexp[k].e) {
            uexp[k].e = e;
            grown = true;
         }
      }
   }
   if (!grown) {
      free(uexp);
      return 0;
   }
   if (!ph_fs_compile_key(program->nir, program->budget, uexp, count,
                          &variant)) {
      free(uexp);
      program->valid = false;
      program->reject = "a runtime constant beyond the combiner's range";
      return -1;
   }
   variant.nir = program->nir;
   variant.budget = program->budget;
   variant.uexp = uexp;
   variant.uexp_count = count;
   program->nir = NULL;
   ph_fs_program_fini(program);
   *program = variant;
   if (getenv("PARHELIA_TRACE_FS")) {
      fprintf(stderr, "parhelia: fs recompiled for runtime constants beyond "
                      "[-1, 1]\n");
      ph_fs_dump(program, stderr);
   }
   return 1;
}

bool
ph_fs_update_constants(struct ph_fs_program *program, const void *constants,
                       unsigned size)
{
   const float *values = constants;
   int rescaled;

   ph_fs_run_preshader(program, values, size);
   rescaled = ph_fs_rescale(program, values, size);
   if (rescaled > 0)
      ph_fs_run_preshader(program, values, size);
   for (unsigned i = 0; i < program->const_ref_count; i++) {
      const struct ph_fs_const_ref *ref = &program->const_ref[i];
      float *target = program->comb.inst[ref->instruction].constant[ref->slot];

      for (unsigned c = 0; c < 4; c++)
         if (ref->mask & (1u << c))
            target[c] = ldexpf(ph_fs_ref_value(program, ref, c, values, size),
                               -(int)ref->e[c]);
   }
   return rescaled >= 0;
}

bool
ph_fs_program_must_drop(const nir_shader *nir,
                        const struct ph_fs_program *program)
{
   return !program->valid && nir->info.name &&
          !strncmp(nir->info.name, "ARB", 3);
}
