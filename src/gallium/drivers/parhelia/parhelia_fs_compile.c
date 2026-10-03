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

#include "compiler/nir/nir_builder.h"
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
   int16_t part;        /* >= 0: a preshaded constant, this part */
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
   const nir_tex_instr *tex_first[4];  /* the lookup each unit returns */
   bool tex_end_cross[4][2];
   unsigned sampler_mask;
   bool uses_color, uses_color1, uses_fogc, uses_col1, uses_color1_alpha;
   bool uses_color0_alpha;
   int route[2];           /* generic varying slot carried in v0/v1, or -1 */
   unsigned scaled_key, carriers;
   const struct ph_fs_ramp *ramps;
   unsigned ramp_count;
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

/* A constant index or offset.  nir_lower_int_to_float turns every integer
 * constant, address sources included, into a float of the same value, so
 * 1.0f reads as index 1; no real index reaches 0x3f800000. */
static uint64_t
ph_fs_index(nir_src src)
{
   const uint64_t u = nir_src_as_uint(src);

   if (nir_src_bit_size(src) == 32 && u >= 0x3f800000u) {
      const float f = nir_src_as_float(src);

      if (f >= 1.0f && f < 16777216.0f && f == floorf(f))
         return (uint64_t)f;
   }
   return u;
}

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
/* @def has one user: a single source, or the channels of one vecN that
 * assembles it (gl_FragColor = vec4(x.x, x.y, x.z, a)). */
static bool
ph_fs_one_user(const nir_def *def)
{
   const nir_instr *parent = NULL;

   if (list_is_singular(&def->uses))
      return true;
   nir_foreach_use_including_if(use, def) {
      const nir_instr *user = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);

      if (!user || user->type != nir_instr_type_alu ||
          !nir_op_is_vec(nir_instr_as_alu(user)->op) ||
          (parent && parent != user))
         return false;
      parent = user;
   }
   return true;
}

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
   return !node->memo || (node->def && ph_fs_one_user(node->def));
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
      ctx->atom[ctx->atoms++] = (struct ph_fs_atom){ def, lane, sel, -1 };
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
       (!root && !ph_fs_one_user(def)))
      goto leaf;
   alu = nir_instr_as_alu(instr);
   /* A uniform expression is one preshaded atom, but a negated or moved
    * uniform stays the uniform's own atom so terms in it cancel. */
   if (ph_fs_uniform_only(def, 0) && alu->op != nir_op_fneg &&
       alu->op != nir_op_mov && alu->op != nir_op_fcanonicalize)
      goto leaf;
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

   if (atom.part >= 0) {
      *out = (struct ph_fs_src){ .kind = PH_SRC_CONST, .channel = -1,
                                 .index = atom.part };
      return true;
   }

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

/* A polynomial a source reads without an instruction: a constant, or one
 * atom as x, -x or 1 - x. */
static bool
ph_fs_poly_source(const struct ph_fs_poly *p)
{
   float c0 = 0.0f;
   int lin = -1;

   for (unsigned i = 0; i < p->n; i++) {
      if (!p->term[i].n)
         c0 = p->term[i].coef;
      else if (p->term[i].n == 1 && lin < 0)
         lin = i;
      else
         return false;
   }
   if (lin < 0)
      return true;
   if (ph_fs_near(p->term[lin].coef, 1.0f) || ph_fs_near(p->term[lin].coef, -1.0f))
      return ph_fs_near(c0, 0.0f) ||
             (ph_fs_near(p->term[lin].coef, -1.0f) && ph_fs_near(c0, 1.0f));
   return false;
}

/* a * t + b * (1 - t) = a*t + b - b*t for an atom t, or t * x + y as one
 * MAD when x and y are sources. */
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
            /* t alone contributes its coefficient to a */
            if (term->n != 2 && term->n != 1) {
               usable = false;
               continue;
            }
            memset(&rest, 0, sizeof(rest));
            rest.coef = term->coef;
            rest.n = term->n - 1;
            rest.atom[0] = term->n == 2 ? term->atom[1 - at] : 0;
            usable = ph_fs_poly_add(&x, &rest);
         }
         if (!usable || !y.n)
            continue;
         a = x;
         if (!ph_fs_poly_sum(&a, &y, 1.0f))
            continue;
         if (ph_fs_poly_source(&x) && ph_fs_poly_source(&y) &&
             !ph_fs_poly_source(&a)) {
            if (!ph_fs_lower_atom_id(ctx, t, &sa) ||
                !ph_fs_select(ctx, lane, &y, &sb) ||
                !ph_fs_select(ctx, lane, &x, &sc))
               return false;
            return ph_fs_node(ctx, lane, P3D_COMB_MAD, &sa, &sb, &sc, out);
         }
         if (!ph_fs_poly_source(&a) || !ph_fs_poly_source(&y))
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

/* The coefficient of the lone atom @a in @p, 0 when absent. */
static float
ph_fs_poly_linear_coef(const struct ph_fs_poly *p, uint16_t a)
{
   for (unsigned i = 0; i < p->n; i++)
      if (p->term[i].n == 1 && p->term[i].atom[0] == a)
         return p->term[i].coef;
   return 0.0f;
}

/* c * x * y + bx * x + by * y + ... = (s1 (x + by/c)) * (s2 (y + bx/c)) + r
 * with s1 s2 = c = +-1: one MAD when both factors and r are sources, as in
 * GL_MODULATE_ADD of inverted operands, (1 - t.a) * (1 - t) + k. */
static bool
ph_fs_try_affine_mad(struct ph_fs_ctx *ctx, int lane,
                     const struct ph_fs_poly *p, struct ph_fs_src *out)
{
   for (unsigned i = 0; i < p->n; i++) {
      const struct ph_fs_term *t = &p->term[i];
      const float c = t->coef;

      if (t->n != 2 || t->atom[0] == t->atom[1] ||
          !(ph_fs_near(c, 1.0f) || ph_fs_near(c, -1.0f)))
         continue;
      for (int s1 = -1; s1 <= 1; s1 += 2) {
         const float s2 = c / (float)s1;
         struct ph_fs_poly f1, f2, prod, r;
         struct ph_fs_term term;
         struct ph_fs_src sa, sb, sc;
         const float alpha = ph_fs_poly_linear_coef(p, t->atom[1]) / c;
         const float beta = ph_fs_poly_linear_coef(p, t->atom[0]) / c;

         memset(&f1, 0, sizeof(f1));
         memset(&f2, 0, sizeof(f2));
         memset(&term, 0, sizeof(term));
         term.n = 1;
         term.coef = (float)s1;
         term.atom[0] = t->atom[0];
         ph_fs_poly_add(&f1, &term);
         term.coef = s2;
         term.atom[0] = t->atom[1];
         ph_fs_poly_add(&f2, &term);
         memset(&term, 0, sizeof(term));
         term.coef = s1 * alpha;
         ph_fs_poly_add(&f1, &term);
         term.coef = s2 * beta;
         ph_fs_poly_add(&f2, &term);
         if (!ph_fs_poly_source(&f1) || !ph_fs_poly_source(&f2) ||
             !ph_fs_poly_mul(&prod, &f1, &f2))
            continue;
         memset(&r, 0, sizeof(r));
         if (!ph_fs_poly_sum(&r, p, 1.0f) || !ph_fs_poly_sum(&r, &prod, -1.0f) ||
             !ph_fs_poly_source(&r))
            continue;
         if (!ph_fs_select(ctx, lane, &f1, &sa) ||
             !ph_fs_select(ctx, lane, &f2, &sc))
            return false;
         if (!r.n)
            return ph_fs_node(ctx, lane, P3D_COMB_MUL, &sa, NULL, &sc, out);
         return ph_fs_select(ctx, lane, &r, &sb) &&
                ph_fs_node(ctx, lane, P3D_COMB_MAD, &sa, &sb, &sc, out);
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
      /* The constant term divided by g must stay a source's [-1, 1]. */
      bool common = g > 0.0f && !ph_fs_near(g, 1.0f) &&
                    ph_fs_shift_of(g, &shift) && fabsf(c0 / g) <= 1.0f;

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
   if (ph_fs_try_affine_mad(ctx, lane, p, out))
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
      /* Constant buffer 1: the driver constants. */
      if (nir_src_is_const(load->src[0]) && nir_src_is_const(load->src[1]) &&
          nir_src_as_uint(load->src[0]) == PH_FS_DRV_UBO) {
         *offset = PH_FS_DRV_BASE +
                   (nir_intrinsic_base(load) + ph_fs_index(load->src[1])) * 4u +
                   nir_intrinsic_component(load);
         return true;
      }
      if (!nir_src_is_const(load->src[0]) || ph_fs_index(load->src[0]) ||
          !nir_src_is_const(load->src[1]))
         return false;
      *offset = (nir_intrinsic_base(load) + ph_fs_index(load->src[1])) * 4u +
                nir_intrinsic_component(load);
      return true;
   case nir_intrinsic_load_ubo:
      if (!nir_src_is_const(load->src[0]) || ph_fs_index(load->src[0]) ||
          !nir_src_is_const(load->src[1]) || ph_fs_index(load->src[1]) % 4u)
         return false;
      *offset = ph_fs_index(load->src[1]) / 4u;
      return true;
   case nir_intrinsic_load_uniform:
      if (!nir_src_is_const(load->src[0]) ||
          (nir_intrinsic_base(load) + ph_fs_index(load->src[0])) % 4u)
         return false;
      *offset = (nir_intrinsic_base(load) + ph_fs_index(load->src[0])) / 4u;
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

   /* a scalar read naturally replicates; any other channel must exist */
   if (def->num_components == 1)
      comp = 0;
   if (comp >= def->num_components)
      return ph_fs_fail(ctx, "a preshader channel past its value");
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

/* The unit and channel carrying component @comp of varying @location. */
static bool
ph_fs_pass_slot(const struct ph_fs_ctx *ctx, unsigned location, unsigned comp,
                unsigned *unit, unsigned *channel)
{
   for (unsigned r = 0; r < ctx->ramp_count; r++) {
      const struct ph_fs_ramp *rp = &ctx->ramps[r];

      if (rp->kind != PH_FS_RAMP_PASS || rp->location != location)
         continue;
      for (unsigned d = 0; d < rp->dims; d++)
         if (rp->comp[d] == comp) {
            *unit = rp->unit;
            *channel = d;
            return true;
         }
   }
   return false;
}

/* A varying on a coordinate unit: the unit's t register, channel d holding
 * component comp[d], scaled back from the carrier's domain. */
static bool
ph_fs_pass_varying(struct ph_fs_ctx *ctx, unsigned location, unsigned first,
                   unsigned width, int lane, int sel, struct ph_fs_src *out)
{
   unsigned unit = 0, channel[3], k;
   const unsigned n = ph_fs_lane_channels(lane);
   struct ph_fs_src t;

   for (unsigned c = 0; c < n; c++) {
      const unsigned comp = first + (sel >= 0 ? (unsigned)sel :
                                     lane == PH_LANE_A ? 3 : c);
      unsigned u;

      if (!ph_fs_pass_slot(ctx, location, comp, &u, &channel[c])) {
         /* A narrower load read naturally in the RGB lane: the channels
          * past its width are never used. */
         if (sel < 0 && lane == PH_LANE_RGB && c >= width && c) {
            channel[c] = c;
            continue;
         }
         return ph_fs_fail(ctx, "a varying other than a colour");
      }
      if (c && u != unit)
         return ph_fs_fail(ctx, "a varying split over coordinate units");
      unit = u;
   }
   /* Natural channels map to themselves, or one channel replicates. */
   if (n == 3 && !(channel[0] == 0 && channel[1] == 1 && channel[2] == 2) &&
       !(channel[0] == channel[1] && channel[1] == channel[2]))
      return ph_fs_fail(ctx, "a varying swizzle a coordinate unit cannot express");
   {
      static const uint8_t identity[4] = { 0, 1, 2, 3 };
      const int usel = n == 3 && channel[0] == 0 && channel[1] == 1 ? -1 :
                       (int)channel[0];

      t = (struct ph_fs_src){ .kind = PH_SRC_TEX, .index = unit,
                              .channel = usel };
      ctx->sampler_mask |= 1u << unit;
      k = 2 + unit;
      ctx->carriers |= 1u << k;
      (void)identity;
      *out = t;
      return true;
   }
}

/* Varying @location read from its component @first on (the load of a
 * packed varying starts there). */
static bool
ph_fs_varying(struct ph_fs_ctx *ctx, unsigned location, unsigned first,
              unsigned width, int lane, int sel, struct ph_fs_src *out)
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
      ctx->carriers |= 1u << out->index;
   } else {
      return ph_fs_pass_varying(ctx, location, first, width, lane, sel, out);
   }
   /* A colour interpolator carries the whole slot: a packed load's natural
    * read has to start at component 0. */
   if (first && sel < 0)
      return ph_fs_fail(ctx, "a packed varying read across its slot");
   if (first)
      sel += first;
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
      return ph_fs_varying(ctx, deref->var->data.location,
                           deref->var->data.location_frac,
                           load->def.num_components, lane, sel, out);
   }
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
      return ph_fs_varying(ctx, nir_intrinsic_io_semantics(load).location,
                           nir_intrinsic_component(load),
                           load->def.num_components, lane, sel, out);
   case nir_intrinsic_load_ubo_vec4: {
      /* The starting channel is the intrinsic's component index; buffer 1
       * holds the driver constants. */
      unsigned offset;

      if (!ph_fs_uniform_offset(load, &offset))
         break;
      return ph_fs_uniform(ctx, offset, lane, sel, out);
   }
   case nir_intrinsic_load_ubo:
      if (!nir_src_is_const(load->src[0]) || ph_fs_index(load->src[0]) ||
          !nir_src_is_const(load->src[1]) ||
          ph_fs_index(load->src[1]) % 4u)
         break;
      return ph_fs_uniform(ctx, ph_fs_index(load->src[1]) / 4u, lane,
                           sel, out);
   case nir_intrinsic_load_uniform:
      if (!nir_src_is_const(load->src[0]) ||
          (nir_intrinsic_base(load) + ph_fs_index(load->src[0])) % 4u)
         break;
      return ph_fs_uniform(ctx, (nir_intrinsic_base(load) +
                                 ph_fs_index(load->src[0])) / 4u,
                           lane, sel, out);
   default:
      break;
   }
   return ph_fs_fail(ctx, nir_intrinsic_infos[load->intrinsic].name);
}

/* Two lookups that fetch the same texel: one op over the same sources. */
static bool
ph_fs_same_lookup(const nir_tex_instr *a, const nir_tex_instr *b)
{
   if (a == b)
      return true;
   if (a->op != b->op || a->num_srcs != b->num_srcs ||
       a->sampler_dim != b->sampler_dim)
      return false;
   for (unsigned i = 0; i < a->num_srcs; i++) {
      const int j = nir_tex_instr_src_index(b, a->src[i].src_type);

      if (j < 0 || b->src[j].src.ssa != a->src[i].src.ssa)
         return false;
   }
   return true;
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
   /* A unit returns one texel per fragment: a second lookup of it at
    * another coordinate (a filter's taps) is not the same t register. */
   if (ctx->tex_first[tex->sampler_index] &&
       !ph_fs_same_lookup(ctx->tex_first[tex->sampler_index], tex))
      return ph_fs_fail(ctx, "two lookups of one texture unit at different "
                             "coordinates");
   ctx->tex_first[tex->sampler_index] = tex;
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
         if (!nir_src_is_const(load->src[0]) || ph_fs_index(load->src[0]) ||
             !nir_src_is_const(load->src[1]))
            return -1;
         op.kind = PH_FOG_UNIFORM;
         op.offset = (nir_intrinsic_base(load) +
                      ph_fs_index(load->src[1])) * 4u +
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

static bool ph_fs_poly_preshade(struct ph_fs_ctx *ctx, int lane,
                                struct ph_fs_poly *p);

/* Channel @c of an atom that is a constant part, as a preshader source. */
static struct ph_fs_pre_src
ph_fs_part_pre_src(const struct ph_fs_ctx *ctx, unsigned part, unsigned c)
{
   const struct ph_fs_part *pt = &ctx->part[part];
   struct ph_fs_pre_src v = { .kind = PH_FS_PRE_LITERAL };

   if (pt->literal) {
      v.literal.f32 = pt->value[c];
   } else if (pt->offset[c] >= PH_FS_PRE_BASE &&
              pt->offset[c] < PH_FS_DRV_BASE) {
      v.kind = PH_FS_PRE_RESULT;
      v.index = pt->offset[c] - PH_FS_PRE_BASE;
   } else {
      v.kind = PH_FS_PRE_BUFFER;
      v.index = pt->offset[c];
   }
   return v;
}

/* *@acc += @coef * @v as preshader nodes; @acc starts as a literal. */
static bool
ph_fs_pre_accumulate(struct ph_fs_ctx *ctx, struct ph_fs_pre_src *acc,
                     float coef, const struct ph_fs_pre_src *v)
{
   struct ph_fs_pre_src srcs[2], term;

   if (v->kind == PH_FS_PRE_LITERAL) {
      if (acc->kind == PH_FS_PRE_LITERAL) {
         acc->literal.f32 += coef * v->literal.f32;
         return true;
      }
      term = *v;
      term.literal.f32 *= coef;
   } else {
      srcs[0] = (struct ph_fs_pre_src){ .kind = PH_FS_PRE_LITERAL };
      srcs[0].literal.f32 = coef;
      srcs[1] = *v;
      if (!ph_fs_pre_node(ctx, nir_op_fmul, 32, 2, srcs, &term))
         return false;
   }
   srcs[0] = *acc;
   srcs[1] = term;
   return ph_fs_pre_node(ctx, nir_op_fadd, 32, 2, srcs, acc);
}

/* A constant RGB vector from preshader sources: a literal when every
 * channel is one, so a program without uniforms needs no runtime update. */
static bool
ph_fs_pre_vector(struct ph_fs_ctx *ctx, int lane, struct ph_fs_pre_src v[3],
                 struct ph_fs_src *out)
{
   struct ph_fs_part part;
   float value[3];
   bool literal = true;

   for (unsigned i = 0; i < 3; i++) {
      literal &= v[i].kind == PH_FS_PRE_LITERAL;
      value[i] = v[i].literal.f32;
   }
   if (literal)
      return ph_fs_literal(ctx, lane, value, out);
   memset(&part, 0, sizeof(part));
   part.lane = lane;
   for (unsigned i = 0; i < 3; i++) {
      if (v[i].kind != PH_FS_PRE_RESULT &&
          !ph_fs_pre_node(ctx, nir_op_mov, 32, 1, &v[i], &v[i]))
         return false;
      part.offset[i] = PH_FS_PRE_BASE + v[i].index;
   }
   return ph_fs_part(ctx, &part, out);
}

/* Group value @poly as x * k + a with one non-constant atom x and constant
 * k, a per channel; *@x is -1 when the value has no such form. */
static bool
ph_fs_affine_group(struct ph_fs_ctx *ctx, const struct ph_fs_poly *poly,
                   const float mask[3], struct ph_fs_pre_src k[3],
                   struct ph_fs_pre_src a[3], int *x)
{
   *x = -1;
   for (unsigned t = 0; t < poly->n; t++) {
      const struct ph_fs_term *term = &poly->term[t];
      int tx = -1;

      for (unsigned m = 0; m < term->n; m++) {
         const struct ph_fs_atom *at = &ctx->atom[term->atom[m]];

         if (at->part >= 0 || (at->def && ph_fs_uniform_only(at->def, 0)))
            continue;
         if (tx >= 0) {
            *x = -1;
            return true;
         }
         tx = term->atom[m];
      }
      if (tx >= 0) {
         if (*x >= 0 && *x != tx) {
            *x = -1;
            return true;
         }
         *x = tx;
      }
   }
   if (*x < 0)
      return true;
   for (unsigned c = 0; c < 3; c++) {
      k[c] = (struct ph_fs_pre_src){ .kind = PH_FS_PRE_LITERAL };
      a[c] = (struct ph_fs_pre_src){ .kind = PH_FS_PRE_LITERAL };
   }
   for (unsigned t = 0; t < poly->n; t++) {
      const struct ph_fs_term *term = &poly->term[t];
      bool has_x = false;

      for (unsigned c = 0; c < 3; c++) {
         struct ph_fs_pre_src v = { .kind = PH_FS_PRE_LITERAL };

         if (!mask[c])
            continue;
         v.literal.f32 = 1.0f;
         for (unsigned m = 0; m < term->n; m++) {
            const struct ph_fs_atom *at = &ctx->atom[term->atom[m]];
            struct ph_fs_pre_src f, srcs[2];

            if (at->part >= 0) {
               f = ph_fs_part_pre_src(ctx, at->part, c);
            } else if (at->def && ph_fs_uniform_only(at->def, 0)) {
               if (!ph_fs_pre_scalar(ctx, at->def,
                                     at->sel >= 0 ? (unsigned)at->sel : c, &f))
                  return false;
            } else {
               has_x = true;
               continue;
            }
            if (v.kind == PH_FS_PRE_LITERAL && v.literal.f32 == 1.0f) {
               v = f;
               continue;
            }
            srcs[0] = v;
            srcs[1] = f;
            if (!ph_fs_pre_node(ctx, nir_op_fmul, 32, 2, srcs, &v))
               return false;
         }
         if (!ph_fs_pre_accumulate(ctx, has_x ? &k[c] : &a[c], term->coef, &v))
            return false;
      }
   }
   return true;
}

/* RGB channels taken from different values: each value, replicated or in
 * its natural channels, is multiplied by a 0/1 mask of the channels it fills
 * and accumulated with multiply-adds onto the constant channels - literals
 * and uniform expressions, preshaded into one constant. */
static bool
ph_fs_assemble(struct ph_fs_ctx *ctx, nir_def *const defs[3],
               const uint8_t comps[3], int lane, float *literal,
               struct ph_fs_src *out)
{
   struct { nir_def *def; int sel; float mask[3]; } group[3];
   unsigned groups = 0;
   bool have_acc = false, have_literal = false, have_uniform = false;
   nir_def *uniform_def[3] = { NULL };
   unsigned uniform_comp[3] = { 0 };
   struct ph_fs_src acc;

   if (lane != PH_LANE_RGB)
      return ph_fs_fail(ctx, "RGB assembled from separate values");
   for (unsigned i = 0; i < 3; i++) {
      nir_def *def = defs[i];
      nir_instr *instr = def ? nir_def_instr(def) : NULL;
      unsigned comp = comps[i];
      int sel;
      unsigned g;

      /* A channel picked out by a move reads its source's channel, so the
       * channels of one value share a group. */
      while (instr && instr->type == nir_instr_type_alu &&
             (nir_instr_as_alu(instr)->op == nir_op_mov ||
              nir_instr_as_alu(instr)->op == nir_op_fcanonicalize) &&
             nir_instr_as_alu(instr)->src[0].src.ssa->num_components <= 4) {
         const nir_alu_instr *mv = nir_instr_as_alu(instr);

         comp = mv->src[0].swizzle[comp];
         def = mv->src[0].src.ssa;
         instr = nir_def_instr(def);
      }
      sel = comp == i ? -1 : (int)comp;

      literal[i] = 0.0f;
      if (!def)
         continue;
      if (instr->type == nir_instr_type_load_const) {
         literal[i] = nir_const_value_as_float(
            nir_instr_as_load_const(instr)->value[comp], 32);
         have_literal |= literal[i] != 0.0f;
         continue;
      }
      if (ph_fs_uniform_only(def, 0)) {
         uniform_def[i] = def;
         uniform_comp[i] = comp;
         have_uniform = true;
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
   /* A group affine in one value with constant factors, x * k + a, is
    * that value's multiply-add onto the constant channels. */
   struct ph_fs_pre_src gk[3][3], ga[3][3];
   int gx[3] = { -1, -1, -1 };
   bool affine = false;

   for (unsigned g = 0; g < groups; g++) {
      struct ph_fs_poly poly;

      if (!ph_fs_expand(ctx, group[g].def, lane, group[g].sel, true, &poly)) {
         if (ctx->reject)
            return false;
         continue;
      }
      if (!ph_fs_poly_preshade(ctx, lane, &poly) ||
          !ph_fs_affine_group(ctx, &poly, group[g].mask, gk[g], ga[g], &gx[g]))
         return false;
      affine |= gx[g] >= 0;
   }
   /* Literal factors beyond a source's range keep the masked form, whose
    * literals the range pass can still rescale. */
   for (unsigned i = 0; affine && i < 3; i++) {
      bool wide = fabsf(literal[i]) > 1.0f;

      for (unsigned g = 0; g < groups; g++)
         if (gx[g] >= 0 && group[g].mask[i])
            wide |= (gk[g][i].kind == PH_FS_PRE_LITERAL &&
                     fabsf(gk[g][i].literal.f32) > 1.0f) ||
                    (ga[g][i].kind == PH_FS_PRE_LITERAL &&
                     fabsf(ga[g][i].literal.f32 + literal[i]) > 1.0f);
      if (wide) {
         for (unsigned g = 0; g < groups; g++)
            gx[g] = -1;
         affine = false;
      }
   }
   if (have_uniform || affine) {
      /* the constant channels as one preshaded constant */
      struct ph_fs_pre_src v[3];

      for (unsigned i = 0; i < 3; i++) {
         v[i] = (struct ph_fs_pre_src){ .kind = PH_FS_PRE_LITERAL };
         v[i].literal.f32 = literal[i];
         if (uniform_def[i] &&
             !ph_fs_pre_scalar(ctx, uniform_def[i], uniform_comp[i], &v[i]))
            return false;
         for (unsigned g = 0; g < groups; g++)
            if (gx[g] >= 0 && group[g].mask[i] &&
                !ph_fs_pre_accumulate(ctx, &v[i], 1.0f, &ga[g][i]))
               return false;
      }
      if (!ph_fs_pre_vector(ctx, lane, v, &acc))
         return false;
      have_acc = true;
      have_literal = false;
   }
   if (!groups)
      return have_acc ? (*out = acc, true) :
                        ph_fs_literal(ctx, lane, literal, out);
   for (unsigned g = 0; g < groups; g++) {
      struct ph_fs_src term, mask;

      if (gx[g] >= 0) {
         if (!ph_fs_lower_atom_id(ctx, gx[g], &term) ||
             !ph_fs_pre_vector(ctx, lane, gk[g], &mask) ||
             !ph_fs_node(ctx, lane, P3D_COMB_MAD, &term, &acc, &mask, &acc))
            return false;
         continue;
      }
      if (!ph_fs_lower(ctx, group[g].def, lane, group[g].sel, &term))
         return false;
      if (group[g].mask[0] && group[g].mask[1] && group[g].mask[2] &&
          !have_acc) {
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

/* @cond ? *b : *c with the condition read as (@lane, @sel).  Without native
 * integers a boolean is 0.0 or 1.0, and CMP is a >= 0 ? b : c. */
static bool
ph_fs_cond(struct ph_fs_ctx *ctx, nir_def *cond, int lane, int sel,
           const struct ph_fs_src *b, const struct ph_fs_src *c,
           struct ph_fs_src *out)
{
   nir_instr *instr = nir_def_instr(cond);
   struct ph_fs_src a, d, t, bb = *b, cc = *c;
   nir_alu_instr *op;
   int req[2];

   if (ph_fs_uniform_only(cond, 0)) {
      /* A condition of uniforms only: the preshader turns it into a 0/1
       * float per channel. */
      struct ph_fs_part part;

      memset(&part, 0, sizeof(part));
      part.lane = lane;
      for (unsigned ch = 0; ch < ph_fs_lane_channels(lane); ch++) {
         struct ph_fs_pre_src cs, f;

         if (!ph_fs_pre_scalar(ctx, cond, sel < 0 ? ch : (unsigned)sel, &cs) ||
             !ph_fs_pre_node(ctx, nir_op_b2f32, cond->bit_size, 1, &cs, &f))
            return false;
         part.offset[ch] = PH_FS_PRE_BASE + f.index;
      }
      return ph_fs_part(ctx, &part, &a) &&
             ph_fs_apply(ctx, lane, &a, P3D_COMB_MOD_NEG) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &cc, &bb, out);
   }
   if (instr->type != nir_instr_type_alu)
      return ph_fs_fail(ctx, "a select on a condition that is no boolean value");
   op = nir_instr_as_alu(instr);
   for (unsigned i = 0; i < 2 && i < nir_op_infos[op->op].num_inputs; i++)
      if (!ph_fs_translate(op, i, lane, sel, &req[i]))
         return ph_fs_fail(ctx, "a swizzle the replicates cannot express");
   switch (op->op) {
   case nir_op_flt:
   case nir_op_fge:
      /* x < y ? b : c is x - y >= 0 ? c : b; x >= y ? b : c directly. */
      if (!ph_fs_operand(ctx, op, 0, lane, sel, &a) ||
          !ph_fs_operand(ctx, op, 1, lane, sel, &d) ||
          !ph_fs_apply(ctx, lane, &d, P3D_COMB_MOD_NEG) ||
          !ph_fs_node(ctx, lane, P3D_COMB_ADD, &a, &d, NULL, &a))
         return false;
      return op->op == nir_op_fge ?
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &bb, &cc, out) :
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &cc, &bb, out);
   case nir_op_feq:
   case nir_op_fneu: {
      /* x == y holds where both x - y >= 0 and y - x >= 0. */
      const bool eq = op->op == nir_op_feq;

      if (!ph_fs_operand(ctx, op, 0, lane, sel, &a) ||
          !ph_fs_operand(ctx, op, 1, lane, sel, &d) ||
          !ph_fs_apply(ctx, lane, &d, P3D_COMB_MOD_NEG) ||
          !ph_fs_node(ctx, lane, P3D_COMB_ADD, &a, &d, NULL, &a) ||
          !ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, eq ? &bb : &cc,
                      eq ? &cc : &bb, &t))
         return false;
      d = a;
      return ph_fs_apply(ctx, lane, &d, P3D_COMB_MOD_NEG) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &d, &t, eq ? &cc : &bb, out);
   }
   case nir_op_b2b1:
      return ph_fs_operand(ctx, op, 0, lane, sel, &a) &&
             ph_fs_apply(ctx, lane, &a, P3D_COMB_MOD_NEG) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &cc, &bb, out);
   case nir_op_inot:
      return ph_fs_cond(ctx, op->src[0].src.ssa, lane, req[0], &cc, &bb, out);
   case nir_op_iand:
      /* a && b ? x : y is a ? (b ? x : y) : y */
      return ph_fs_cond(ctx, op->src[1].src.ssa, lane, req[1], &bb, &cc, &t) &&
             ph_fs_cond(ctx, op->src[0].src.ssa, lane, req[0], &t, &cc, out);
   case nir_op_ior:
      return ph_fs_cond(ctx, op->src[1].src.ssa, lane, req[1], &bb, &cc, &t) &&
             ph_fs_cond(ctx, op->src[0].src.ssa, lane, req[0], &bb, &t, out);
   case nir_op_ixor:
   case nir_op_ine:
   case nir_op_ieq: {
      /* a != b ? x : y is a ? (b ? y : x) : (b ? x : y) */
      const bool same = op->op == nir_op_ieq;
      struct ph_fs_src u;

      return ph_fs_cond(ctx, op->src[1].src.ssa, lane, req[1],
                        same ? &bb : &cc, same ? &cc : &bb, &t) &&
             ph_fs_cond(ctx, op->src[1].src.ssa, lane, req[1],
                        same ? &cc : &bb, same ? &bb : &cc, &u) &&
             ph_fs_cond(ctx, op->src[0].src.ssa, lane, req[0], &t, &u, out);
   }
   default:
      return ph_fs_fail(ctx, "a select on a condition that is no boolean value");
   }
}

static bool
ph_fs_operation(struct ph_fs_ctx *ctx, nir_alu_instr *alu, int lane, int sel,
                struct ph_fs_src *out)
{
   struct ph_fs_src a, b, c, d;
   int req;

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
   case nir_op_fdot2:
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
      if (alu->op == nir_op_fdot2) {
         /* dot2 is dot3 with the third channel of one operand masked. */
         static const float mask[3] = { 1.0f, 1.0f, 0.0f };
         struct ph_fs_src m;
         int req[2];

         for (unsigned i = 0; i < 2; i++) {
            const uint8_t *sw = alu->src[i].swizzle;

            if (sw[0] == 0 && sw[1] == 1)
               req[i] = -1;
            else if (sw[0] == sw[1])
               req[i] = sw[0];
            else
               return ph_fs_fail(ctx, "a swizzle the replicates cannot express");
         }
         if (!ph_fs_lower(ctx, alu->src[0].src.ssa, PH_LANE_RGB, req[0], &a) ||
             !ph_fs_lower(ctx, alu->src[1].src.ssa, PH_LANE_RGB, req[1], &c) ||
             !ph_fs_literal(ctx, PH_LANE_RGB, mask, &m) ||
             !ph_fs_node(ctx, PH_LANE_RGB, P3D_COMB_MUL, &a, NULL, &m, &a))
            return false;
      } else if (!ph_fs_operand(ctx, alu, 0, PH_LANE_RGB, -1, &a) ||
                 !ph_fs_operand(ctx, alu, 1, PH_LANE_RGB, -1, &c)) {
         return false;
      }
      if (!ph_fs_node(ctx, PH_LANE_BOTH, P3D_COMB_DP3, &a, NULL, &c, out))
         return false;
      ctx->node[out->index].def = &alu->def;
      return true;
   case nir_op_fdot4: {
      /* dot3 of xyz, then a.w * b.w added in each lane: the DP4 opcode
       * is proven from the tables but not yet measured. */
      struct ph_fs_src dp = { 0 }, aw, bw;
      int req[2];
      int found = -1;

      for (unsigned i = 0; i < 2; i++) {
         const uint8_t *sw = alu->src[i].swizzle;

         if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2)
            req[i] = -1;
         else if (sw[0] == sw[1] && sw[1] == sw[2])
            req[i] = sw[0];
         else
            return ph_fs_fail(ctx, "a swizzle the replicates cannot express");
      }
      for (unsigned i = 0; i < ctx->nodes; i++)
         if (ctx->node[i].def == &alu->def &&
             ctx->node[i].lane == PH_LANE_BOTH &&
             ctx->node[i].op == P3D_COMB_DP3)
            found = (int)i;
      if (found >= 0) {
         ctx->node[found].refs++;
         dp = (struct ph_fs_src){ .kind = PH_SRC_NODE, .channel = -1,
                                  .index = (unsigned)found };
      } else {
         if (!ph_fs_lower(ctx, alu->src[0].src.ssa, PH_LANE_RGB, req[0], &a) ||
             !ph_fs_lower(ctx, alu->src[1].src.ssa, PH_LANE_RGB, req[1], &c) ||
             !ph_fs_node(ctx, PH_LANE_BOTH, P3D_COMB_DP3, &a, NULL, &c, &dp))
            return false;
         ctx->node[dp.index].def = &alu->def;
      }
      return ph_fs_lower(ctx, alu->src[0].src.ssa, lane,
                         alu->src[0].swizzle[3], &aw) &&
             ph_fs_lower(ctx, alu->src[1].src.ssa, lane,
                         alu->src[1].swizzle[3], &bw) &&
             ph_fs_node(ctx, lane, P3D_COMB_MAD, &aw, &dp, &bw, out);
   }
   case nir_op_fsign: {
      /* x >= 0 ? 0 : -1, then -x >= 0 ? that : 1 */
      struct ph_fs_src zero, one, minus, t, nx;

      if (!ph_fs_operand(ctx, alu, 0, lane, sel, &a) ||
          !ph_fs_literal_scalar(ctx, lane, 0.0f, &zero) ||
          !ph_fs_literal_scalar(ctx, lane, 1.0f, &one) ||
          !ph_fs_literal_scalar(ctx, lane, -1.0f, &minus) ||
          !ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &zero, &minus, &t))
         return false;
      nx = a;
      return ph_fs_apply(ctx, lane, &nx, P3D_COMB_MOD_NEG) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &nx, &t, &one, out);
   }
   case nir_op_fcsel_ge:
      return ph_fs_operand(ctx, alu, 0, lane, sel, &a) &&
             ph_fs_operand(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_operand(ctx, alu, 2, lane, sel, &c) &&
             ph_fs_node(ctx, lane, P3D_COMB_CMP, &a, &b, &c, out);
   case nir_op_bcsel:
      if (!ph_fs_translate(alu, 0, lane, sel, &req))
         return ph_fs_fail(ctx, "a swizzle the replicates cannot express");
      return ph_fs_operand(ctx, alu, 1, lane, sel, &b) &&
             ph_fs_operand(ctx, alu, 2, lane, sel, &c) &&
             ph_fs_cond(ctx, alu->src[0].src.ssa, lane, req, &b, &c, out);
   case nir_op_b2f32:
      if (!ph_fs_translate(alu, 0, lane, sel, &req))
         return ph_fs_fail(ctx, "a swizzle the replicates cannot express");
      return ph_fs_literal_scalar(ctx, lane, 1.0f, &b) &&
             ph_fs_literal_scalar(ctx, lane, 0.0f, &c) &&
             ph_fs_cond(ctx, alu->src[0].src.ssa, lane, req, &b, &c, out);
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
         /* A vec2 read naturally fills its third channel with zero, which
          * nothing reads. */
         const unsigned n = nir_op_infos[alu->op].num_inputs;
         nir_def *defs[3] = { NULL, NULL, NULL };
         uint8_t comps[3] = { 0, 0, 0 };

         for (unsigned c = 0; c < MIN2(n, 3); c++) {
            defs[c] = alu->src[c].src.ssa;
            comps[c] = alu->src[c].swizzle[0];
         }
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

/* Fold the uniform-only factors of every term into preshaded constants:
 * k0 * u0 * x + k1 * u1 * x + k2 * u2 becomes P * x + Q with P and Q
 * evaluated per draw, so no combiner slot computes uniform arithmetic. */
static bool
ph_fs_poly_preshade(struct ph_fs_ctx *ctx, int lane, struct ph_fs_poly *p)
{
   struct ph_fs_poly out;
   bool done[PH_FS_POLY_TERMS] = { false }, any = false;

   for (unsigned i = 0; i < p->n; i++)
      for (unsigned a = 0; a < p->term[i].n; a++)
         any |= ctx->atom[p->term[i].atom[a]].part < 0 &&
                ph_fs_uniform_only(ctx->atom[p->term[i].atom[a]].def, 0);
   if (!any)
      return true;
   memset(&out, 0, sizeof(out));
   for (unsigned i = 0; i < p->n; i++) {
      uint16_t rest[PH_FS_TERM_ATOMS];
      unsigned nrest = 0;
      struct ph_fs_pre_src sum[3];
      bool has_sum = false, has_uniform = false;
      struct ph_fs_term t;

      if (done[i])
         continue;
      for (unsigned a = 0; a < p->term[i].n; a++) {
         const struct ph_fs_atom *at = &ctx->atom[p->term[i].atom[a]];

         if (at->part < 0 && ph_fs_uniform_only(at->def, 0))
            has_uniform = true;
         else
            rest[nrest++] = p->term[i].atom[a];
      }
      if (has_uniform) {
         /* Worth it only when terms merge or the factor is a product or
          * scaled: a lone u * x or u is what LRP and MAD take as is. */
         unsigned group = 0, uatoms = 0;

         for (unsigned j = i; j < p->n; j++) {
            uint16_t rj[PH_FS_TERM_ATOMS];
            unsigned nj = 0;

            for (unsigned a = 0; a < p->term[j].n; a++) {
               const struct ph_fs_atom *at = &ctx->atom[p->term[j].atom[a]];

               if (!(at->part < 0 && ph_fs_uniform_only(at->def, 0)))
                  rj[nj++] = p->term[j].atom[a];
               else if (j == i)
                  uatoms++;
            }
            group += nj == nrest && !memcmp(rj, rest, nj * sizeof(rj[0]));
         }
         has_uniform = group > 1 || uatoms > 1 ||
                       !ph_fs_near(fabsf(p->term[i].coef), 1.0f);
      }
      if (!has_uniform) {
         done[i] = true;
         if (!ph_fs_poly_add(&out, &p->term[i]))
            return ph_fs_fail(ctx, "a polynomial beyond its terms");
         continue;
      }
      /* Every term with the same non-uniform atoms joins this one. */
      for (unsigned j = i; j < p->n; j++) {
         uint16_t rj[PH_FS_TERM_ATOMS];
         unsigned nj = 0;
         bool uj = false;

         if (done[j])
            continue;
         for (unsigned a = 0; a < p->term[j].n; a++) {
            const struct ph_fs_atom *at = &ctx->atom[p->term[j].atom[a]];

            if (at->part < 0 && ph_fs_uniform_only(at->def, 0))
               uj = true;
            else
               rj[nj++] = p->term[j].atom[a];
         }
         if (nj != nrest || memcmp(rj, rest, nj * sizeof(rj[0])))
            continue;
         /* This term's factor, per lane channel. */
         for (unsigned c = 0; c < ph_fs_lane_channels(lane); c++) {
            struct ph_fs_pre_src f = { .kind = PH_FS_PRE_LITERAL };

            f.literal.f32 = p->term[j].coef;
            for (unsigned a = 0; uj && a < p->term[j].n; a++) {
               const struct ph_fs_atom *at = &ctx->atom[p->term[j].atom[a]];
               struct ph_fs_pre_src v, srcs[2];

               if (at->part >= 0 || !ph_fs_uniform_only(at->def, 0))
                  continue;
               const unsigned comp = at->sel >= 0 ? (unsigned)at->sel :
                                     lane == PH_LANE_A ? 3 : c;

               /* a natural read of a narrower value never uses the
                * channels past its width */
               if (at->sel < 0 && at->def->num_components > 1 &&
                   comp >= at->def->num_components) {
                  v = (struct ph_fs_pre_src){ .kind = PH_FS_PRE_LITERAL };
               } else if (!ph_fs_pre_scalar(ctx, at->def, comp, &v)) {
                  return false;
               }
               srcs[0] = f;
               srcs[1] = v;
               if (!ph_fs_pre_node(ctx, nir_op_fmul, 32, 2, srcs, &f))
                  return false;
            }
            if (has_sum) {
               struct ph_fs_pre_src srcs[2] = { sum[c], f };

               if (!ph_fs_pre_node(ctx, nir_op_fadd, 32, 2, srcs, &sum[c]))
                  return false;
            } else {
               sum[c] = f;
            }
         }
         has_sum = true;
         done[j] = true;
      }
      {
         struct ph_fs_part part;
         struct ph_fs_src src;
         uint16_t atom;

         memset(&part, 0, sizeof(part));
         part.lane = lane;
         for (unsigned c = 0; c < ph_fs_lane_channels(lane); c++) {
            if (sum[c].kind != PH_FS_PRE_RESULT) {
               struct ph_fs_pre_src one = { .kind = PH_FS_PRE_LITERAL };

               one.literal.f32 = 1.0f;
               if (!ph_fs_pre_node(ctx, nir_op_fmul, 32, 2,
                                   (struct ph_fs_pre_src[2]){ sum[c], one },
                                   &sum[c]))
                  return false;
            }
            part.offset[c] = PH_FS_PRE_BASE + sum[c].index;
         }
         if (!ph_fs_part(ctx, &part, &src))
            return false;
         if (!PH_FS_GROW(ctx, atom, atoms, atom_cap))
            return ph_fs_fail(ctx, "out of memory");
         atom = ctx->atoms;
         ctx->atom[ctx->atoms++] = (struct ph_fs_atom){ NULL, lane, -1,
                                                        (int16_t)src.index };
         memset(&t, 0, sizeof(t));
         t.coef = 1.0f;
         memcpy(t.atom, rest, nrest * sizeof(rest[0]));
         t.n = nrest;
         t.atom[t.n++] = atom;
         if (!ph_fs_poly_add(&out, &t))
            return ph_fs_fail(ctx, "a polynomial beyond its terms");
      }
   }
   *p = out;
   return true;
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
   if (polynomial && !ph_fs_poly_preshade(ctx, lane, &poly))
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
         /* an interpolant in [0, 1] keeps the result between A and B */
         {
            bool convex = true;

            for (unsigned c = 0; c < 4; c++)
               convex &= r[2].lo[c] >= 0.0f && r[2].hi[c] <= 1.0f;
            if (convex)
               f = ph_fs_range_hull(r[0], r[1]);
         }
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
         /* A replicate of one channel of the value, or its alpha read
          * from the RGB half. */
         channel = s->channel >= 0 ? s->channel :
                   half == PH_LANE_RGB && dep->lane == PH_LANE_A ? 3 : -1;
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
         static const char *const shift[8] = {
            "", "_x2", "_x4", "_x8", "_s4", "_s5", "_d4", "_d2",
         };

         fprintf(f, "  fs %u %-3s %s%s%s %s", i, h ? "a" : "rgb",
                 op[x->op & 0x3f] ? op[x->op & 0x3f] : "??",
                 x->sat ? "_sat" : "", shift[x->scale & 7],
                 reg[MIN2(x->dst, 0x11u)]);
         /* ~n: source modifier n (P3D_COMB_MOD_*) */
         for (unsigned s = 0; s < 3; s++) {
            if (x->src[s].mod)
               fprintf(f, " ~%u%s.%c", x->src[s].mod,
                       reg[MIN2(x->src[s].reg, 0x11u)], rep[x->src[s].rep & 7 ?
                       MIN2(x->src[s].rep, 4u) : 0]);
            else
               fprintf(f, " %s.%c", reg[MIN2(x->src[s].reg, 0x11u)],
                       rep[x->src[s].rep & 7 ? MIN2(x->src[s].rep, 4u) : 0]);
         }
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

/* A def every use of which ends in a texture coordinate, directly or through
 * arithmetic that only feeds coordinates: the combiner never reads it as a
 * value, so it needs no interpolator of its own. */
static bool
ph_fs_coordinate_only(nir_def *def, unsigned depth)
{
   if (depth > 8 || list_is_empty(&def->uses))
      return false;
   nir_foreach_use_including_if(src, def) {
      nir_instr *user;

      if (nir_src_is_if(src))
         return false;
      user = nir_src_use_instr(src);
      if (user->type == nir_instr_type_tex) {
         const nir_tex_instr *tex = nir_instr_as_tex(user);
         const int c = nir_tex_instr_src_index(tex, nir_tex_src_coord);

         if (c < 0 || &tex->src[c].src != src)
            return false;
         continue;
      }
      if (user->type != nir_instr_type_alu ||
          !ph_fs_coordinate_only(&nir_instr_as_alu(user)->def, depth + 1))
         return false;
   }
   return true;
}

/* Generic varyings the program reads as values, assigned to the colour
 * interpolators it does not read itself: v0 first, then v1.  Texture
 * coordinate varyings only feed the texture units and are not routed. */
/* A varying the combiner reads as a value through a colour interpolator or
 * a coordinate unit: generics, the point coordinate, and texture
 * coordinates read as values (ARB_fragment_program's fragment.texcoord). */
static bool
ph_fs_value_varying(int location)
{
   return location >= VARYING_SLOT_VAR0 || location == VARYING_SLOT_PNTC ||
          (location >= VARYING_SLOT_TEX0 && location <= VARYING_SLOT_TEX7);
}

static bool
ph_fs_route_varyings(nir_shader *nir, int route[2],
                     const struct ph_fs_ramp *ramps, unsigned ramp_count)
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
            if (location >= 0 && ph_fs_coordinate_only(&in->def, 0))
               continue;
            if (location == VARYING_SLOT_COL0)
               used_col0 = true;
            else if (location == VARYING_SLOT_COL1 ||
                     location == VARYING_SLOT_FOGC)
               used_col1 = true;
            else if (ph_fs_value_varying(location)) {
               bool seen = false;

               /* Carried by a coordinate unit. */
               for (unsigned r = 0; r < ramp_count; r++)
                  seen |= ramps[r].kind == PH_FS_RAMP_PASS && ramps[r].location == location;
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

/* The carrier and channel of component @comp of varying @location, or
 * false when the colour route and coordinate units do not carry it. */
static bool
ph_fs_carrier_of(const int route[2], const struct ph_fs_ramp *ramps,
                 unsigned ramp_count, unsigned location, unsigned comp,
                 unsigned *carrier, unsigned *channel)
{
   for (unsigned k = 0; k < 2; k++)
      if (route[k] == (int)location) {
         *carrier = k;
         *channel = comp;
         return true;
      }
   for (unsigned r = 0; r < ramp_count; r++)
      for (unsigned d = 0; ramps[r].kind == PH_FS_RAMP_PASS && d < ramps[r].dims; d++)
         if (ramps[r].location == location && ramps[r].comp[d] == comp) {
            *carrier = 2 + ramps[r].unit;
            *channel = d;
            return true;
         }
   return false;
}

/* A carrier off the identity domain stores (x - lo) / span: every read of
 * its varyings becomes x * span + lo with span and lo the driver constants
 * of constant buffer 1, so the ordinary algebra and preshader fold them
 * into what the program does with the value. */
static void
ph_fs_scale_carriers(nir_shader *nir, const int route[2],
                     const struct ph_fs_ramp *ramps, unsigned ramp_count,
                     unsigned scaled_key)
{
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            nir_intrinsic_instr *in;
            unsigned location, first;
            nir_def *comp[4];
            bool any = false;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_load_deref) {
               nir_deref_instr *deref = nir_src_as_deref(in->src[0]);

               if (!deref || deref->deref_type != nir_deref_type_var ||
                   !(deref->var->data.mode & nir_var_shader_in))
                  continue;
               location = deref->var->data.location;
               first = deref->var->data.location_frac;
            } else if (in->intrinsic == nir_intrinsic_load_input ||
                       in->intrinsic == nir_intrinsic_load_interpolated_input) {
               location = nir_intrinsic_io_semantics(in).location;
               first = nir_intrinsic_component(in);
            } else {
               continue;
            }
            b.cursor = nir_after_instr(instr);
            {
               const unsigned n = MIN2(in->def.num_components, 4);
               unsigned k[4], ch[4], swz[4];
               bool scaled[4], one = true;
               nir_def *v;

               for (unsigned c = 0; c < n; c++) {
                  scaled[c] = ph_fs_carrier_of(route, ramps, ramp_count,
                                               location, first + c, &k[c],
                                               &ch[c]) &&
                              (scaled_key & (1u << k[c]));
                  any |= scaled[c];
                  swz[c] = scaled[c] ? ch[c] : 0;
                  one &= scaled[c] && k[c] == k[0];
               }
               if (!any)
                  continue;
               if (one) {
                  /* One carrier: a single vector multiply-add, which the
                   * algebra folds into what the program does next. */
                  nir_def *span = nir_load_ubo_vec4(&b, 4, 32,
                     nir_imm_int(&b, PH_FS_DRV_UBO), nir_imm_int(&b, k[0] * 2));
                  nir_def *lo = nir_load_ubo_vec4(&b, 4, 32,
                     nir_imm_int(&b, PH_FS_DRV_UBO), nir_imm_int(&b, k[0] * 2 + 1));

                  v = nir_ffma(&b, &in->def, nir_swizzle(&b, span, swz, n),
                               nir_swizzle(&b, lo, swz, n));
               } else {
                  for (unsigned c = 0; c < n; c++) {
                     comp[c] = nir_channel(&b, &in->def, c);
                     if (!scaled[c])
                        continue;
                     comp[c] = nir_ffma(&b, comp[c],
                        nir_channel(&b, nir_load_ubo_vec4(&b, 4, 32,
                           nir_imm_int(&b, PH_FS_DRV_UBO), nir_imm_int(&b, k[c] * 2)), ch[c]),
                        nir_channel(&b, nir_load_ubo_vec4(&b, 4, 32,
                           nir_imm_int(&b, PH_FS_DRV_UBO), nir_imm_int(&b, k[c] * 2 + 1)), ch[c]));
                  }
                  v = nir_vec(&b, comp, n);
               }
               nir_def_rewrite_uses_after(&in->def, v);
            }
         }
      }
      nir_progress(true, impl, nir_metadata_none);
   }
}

static bool
ph_fs_compile_key(nir_shader *nir, unsigned max_instructions,
                  const struct ph_fs_uexp *uexp, unsigned uexp_count,
                  const struct ph_fs_ramp *ramps, unsigned ramp_count,
                  unsigned scaled_key, struct ph_fs_program *out)
{
   struct ph_fs_ctx ctx;
   nir_shader *scaled_nir = NULL;
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
   ctx.scaled_key = scaled_key;
   if (getenv("PARHELIA_TRACE_FS_NIR"))
      nir_print_shader(nir, stderr);
   for (unsigned u = 0; u < 4; u++)
      for (unsigned c = 0; c < 4; c++)
         ctx.tex_hi[u][c] = 1.0f;
   /* A ramp texel holds only its own components; the rest are zero. */
   for (unsigned r = 0; r < ramp_count; r++) {
      if (ramps[r].unit >= 4 || ramps[r].kind != PH_FS_RAMP_FLOOR)
         continue;
      for (unsigned c = 0; c < 4; c++)
         ctx.tex_hi[ramps[r].unit][c] = 0.0f;
      for (unsigned d = 0; d < ramps[r].dims; d++)
         ctx.tex_hi[ramps[r].unit][ramps[r].comp[d]] = 1.0f;
   }
   ctx.ramps = ramps;
   ctx.ramp_count = ramp_count;
   if (!ph_fs_route_varyings(nir, ctx.route, ramps, ramp_count)) {
      ph_fs_fail(&ctx, "more value varyings than free colour interpolators");
      goto done;
   }
   if (scaled_key) {
      scaled_nir = nir_shader_clone(NULL, nir);
      ph_fs_scale_carriers(scaled_nir, ctx.route, ramps, ramp_count,
                           scaled_key);
      NIR_PASS(_, scaled_nir, nir_opt_copy_prop);
      NIR_PASS(_, scaled_nir, nir_opt_algebraic);
      NIR_PASS(_, scaled_nir, nir_opt_constant_folding);
      NIR_PASS(_, scaled_nir, nir_opt_cse);
      NIR_PASS(_, scaled_nir, nir_opt_dce);
      nir = scaled_nir;
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
   out->scaled_key = scaled_key;
   out->carriers = ctx.carriers;
   for (unsigned k = 0; k < PH_FS_CARRIERS; k++)
      for (unsigned c = 0; c < 4; c++)
         out->drv[PH_FS_DRV_SPAN(k, c)] = 1.0f;
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
   ralloc_free(scaled_nir);
   return ok;
}

/* ---- floor through a ramp texture -------------------------------------- */

/* The varying a value reads, with its slot and the component each channel
 * reads; false when it is anything else. */
static bool
ph_fs_ramp_varying(nir_def *def, unsigned *location, uint8_t comp[4])
{
   nir_instr *instr = nir_def_instr(def);
   nir_intrinsic_instr *in;
   unsigned first = 0;

   if (instr->type != nir_instr_type_intrinsic)
      return false;
   in = nir_instr_as_intrinsic(instr);
   switch (in->intrinsic) {
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_input:
      *location = nir_intrinsic_io_semantics(in).location;
      first = nir_intrinsic_component(in);
      break;
   case nir_intrinsic_load_deref: {
      nir_variable *var =
         nir_deref_instr_get_variable(nir_src_as_deref(in->src[0]));

      if (!var || var->data.mode != nir_var_shader_in)
         return false;
      *location = var->data.location;
      first = var->data.location_frac;
      break;
   }
   default:
      return false;
   }
   if (*location == VARYING_SLOT_COL0 || *location == VARYING_SLOT_COL1)
      return false;
   for (unsigned c = 0; c < 4; c++)
      comp[c] = first + c;
   return true;
}

/* floor(x * k) per channel of @floor: the varying component and k each
 * channel takes; false for any other operand. */
static bool
ph_fs_ramp_match(nir_alu_instr *floor, unsigned *location,
                 uint8_t comp[4], float k[4])
{
   nir_def *src = floor->src[0].src.ssa;
   nir_alu_instr *mul;
   uint8_t vc[4];
   int xi;

   if (ph_fs_ramp_varying(src, location, vc)) {
      for (unsigned c = 0; c < floor->def.num_components; c++) {
         comp[c] = vc[floor->src[0].swizzle[c]];
         k[c] = 1.0f;
      }
      return true;
   }
   if (nir_def_instr(src)->type != nir_instr_type_alu)
      return false;
   mul = nir_def_as_alu(src);
   if (mul->op != nir_op_fmul)
      return false;
   xi = nir_src_is_const(mul->src[1].src) ? 0 :
        nir_src_is_const(mul->src[0].src) ? 1 : -1;
   if (xi < 0 || !ph_fs_ramp_varying(mul->src[xi].src.ssa, location, vc))
      return false;
   for (unsigned c = 0; c < floor->def.num_components; c++) {
      const unsigned m = floor->src[0].swizzle[c];

      comp[c] = vc[mul->src[xi].swizzle[m]];
      k[c] = nir_src_comp_as_float(mul->src[1 - xi].src,
                                   mul->src[1 - xi].swizzle[m]);
   }
   return true;
}

/* Texels per dimension and coordinate span for scale @k: the texel count is
 * one whose code step divides 255 exactly (2, 4, 16, 256), so the ramp's
 * value times 255/step is the integer floor without rounding. */
static bool
ph_fs_ramp_size(float k, uint16_t *size, float *lo, float *span)
{
   static const unsigned sizes[] = { 16, 256, 4, 2 };
   const int ik = (int)k;

   if (k != (float)ik || ik < 1 || ik > 256 || (ik & (ik - 1)))
      return false;
   for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
      if (sizes[i] < (unsigned)ik || sizes[i] % ik)
         continue;
      *size = sizes[i];
      *span = (float)(sizes[i] / ik);
      *lo = *span >= 2.0f ? -0.5f * *span : 0.0f;
      return true;
   }
   return false;
}

static nir_def *
ph_fs_ramp_sample(nir_builder *b, const struct ph_fs_ramp *ramp,
                  nir_def *coord)
{
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 1);

   tex->op = nir_texop_tex;
   tex->sampler_dim = ramp->dims == 3 ? GLSL_SAMPLER_DIM_3D :
                                        GLSL_SAMPLER_DIM_2D;
   tex->dest_type = nir_type_float32;
   tex->texture_index = tex->sampler_index = ramp->unit;
   tex->coord_components = ramp->dims == 3 ? 3 : 2;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                     nir_trim_vector(b, coord,
                                                     tex->coord_components));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(b, &tex->instr);
   return &tex->def;
}

unsigned
ph_fs_ramp_step(unsigned size)
{
   return size > 1 ? 255u / (size - 1u) : 255u;
}

bool
ph_fs_assign_pass_units(nir_shader *nir, unsigned used_units,
                        unsigned max_units, struct ph_fs_ramp *ramps,
                        unsigned *count, const char **reject)
{
   unsigned location[VARYING_SLOT_MAX], read[VARYING_SLOT_MAX];
   unsigned generics = 0, colours = 2, u = 0;

   memset(read, 0, sizeof(read));
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;
            unsigned loc = ~0u, first = 0, i;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_load_deref) {
               nir_deref_instr *deref = nir_src_as_deref(in->src[0]);

               if (!deref || deref->deref_type != nir_deref_type_var ||
                   !(deref->var->data.mode & nir_var_shader_in))
                  continue;
               loc = deref->var->data.location;
               first = deref->var->data.location_frac;
            } else if (in->intrinsic == nir_intrinsic_load_input ||
                       in->intrinsic == nir_intrinsic_load_interpolated_input) {
               loc = nir_intrinsic_io_semantics(in).location;
               first = nir_intrinsic_component(in);
            } else {
               continue;
            }
            if (ph_fs_coordinate_only(&in->def, 0))
               continue;
            if (loc == VARYING_SLOT_COL0 || loc == VARYING_SLOT_COL1 ||
                loc == VARYING_SLOT_FOGC) {
               read[loc] |= 1;
               continue;
            }
            if (loc >= VARYING_SLOT_MAX || !ph_fs_value_varying(loc))
               continue;
            for (i = 0; i < generics && location[i] != loc; i++)
               ;
            if (i == generics)
               location[generics++] = loc;
            read[loc] |= nir_def_components_read(&in->def) << first;
         }
      }
   }
   colours = !read[VARYING_SLOT_COL0] +
             !(read[VARYING_SLOT_COL1] || read[VARYING_SLOT_FOGC]);
   /* Coordinate units interpolate perspective-correctly, the colour
    * interpolators affinely (MEASURED on the P690: a routed varying read
    * the same with w = 1 and w = 2 on one edge), so a varying of up to two
    * read components takes a unit while units last; the colours take the
    * rest, wider ones first. */
   bool on_unit[VARYING_SLOT_MAX] = { false };
   unsigned left = generics, free_units = 0;

   for (unsigned v = 0; v < max_units; v++)
      free_units += !(used_units & (1u << v));
   for (unsigned g = 0; g < generics && free_units; g++)
      if (util_bitcount(read[location[g]]) <= 2) {
         on_unit[location[g]] = true;
         free_units--;
         left--;
      }
   for (unsigned g = 0; g < generics && left > colours; g++)
      if (!on_unit[location[g]] && util_bitcount(read[location[g]]) <= 4 &&
          free_units >= DIV_ROUND_UP(util_bitcount(read[location[g]]), 2)) {
         on_unit[location[g]] = true;
         free_units -= DIV_ROUND_UP(util_bitcount(read[location[g]]), 2);
         left--;
      }
   if (left > colours) {
      *reject = "more value varyings than colour interpolators and "
                "coordinate units";
      return false;
   }
   for (unsigned g = 0; g < generics; g++) {
      struct ph_fs_ramp *ramp = NULL;

      if (!on_unit[location[g]])
         continue;

      for (unsigned c = 0; c < 4; c++) {
         if (!(read[location[g]] & (1u << c)))
            continue;
         if (!ramp || ramp->dims == 2) {
            while (u < max_units && (used_units & (1u << u)))
               u++;
            if (u >= max_units) {
               *reject = "more value varyings than colour interpolators "
                         "and coordinate units";
               return false;
            }
            ramp = &ramps[(*count)++];
            memset(ramp, 0, sizeof(*ramp));
            ramp->unit = u++;
            ramp->location = location[g];
            ramp->kind = PH_FS_RAMP_PASS;
         }
         ramp->comp[ramp->dims] = c;
         ramp->size[ramp->dims] = 1;
         ramp->span[ramp->dims] = 1.0f;
         ramp->dims++;
      }
   }
   return true;
}

bool
ph_fs_lower_floor_ramps(nir_shader *nir, unsigned used_units,
                        unsigned max_units, struct ph_fs_ramp *ramps,
                        unsigned *count, const char **reject)
{
   /* The scale each varying component is floored at, 0 when it is not. */
   float need[VARYING_SLOT_MAX][4];
   unsigned free_units = 0, groups = 0, per = 2, n = 0;
   bool any = false;

   *count = 0;
   memset(need, 0, sizeof(need));
   /* Shaders without a floor of a varying stay untouched. */
   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block) {
            unsigned location;
            uint8_t comp[4];
            float k[4];

            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == nir_op_ffloor &&
                ph_fs_ramp_match(nir_instr_as_alu(instr), &location, comp, k))
               any = true;
         }
   if (!any)
      return true;
   any = false;
   /* Only the channels something reads need a ramp. */
   NIR_PASS(_, nir, nir_opt_shrink_vectors, true);
   NIR_PASS(_, nir, nir_opt_dce);
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_alu_instr *alu;
            unsigned location;
            uint8_t comp[4];
            float k[4];

            if (instr->type != nir_instr_type_alu ||
                (alu = nir_instr_as_alu(instr))->op != nir_op_ffloor)
               continue;
            if (!ph_fs_ramp_match(alu, &location, comp, k) ||
                location >= VARYING_SLOT_MAX)
               continue;
            const nir_component_mask_t read = nir_def_components_read(&alu->def);

            for (unsigned c = 0; c < alu->def.num_components; c++) {
               uint16_t size;
               float lo, span;

               if (!(read & (1u << c)))
                  continue;
               if (comp[c] > 3 || !ph_fs_ramp_size(k[c], &size, &lo, &span)) {
                  *reject = "a floor whose scale no ramp texture expresses";
                  return false;
               }
               if (need[location][comp[c]] != 0.0f &&
                   need[location][comp[c]] != k[c]) {
                  *reject = "floors of one coordinate at different scales";
                  return false;
               }
               need[location][comp[c]] = k[c];
               any = true;
            }
         }
      }
   }
   if (!any)
      return true;
   for (unsigned u = 0; u < max_units; u++)
      free_units += !(used_units & (1u << u));
   /* Two components per 2-D unit.  Three take one volume when it is the
    * only unit: the draw stream carries a third coordinate for unit 0 alone
    * (MEASURED on the P690: a volume ramp beside a 2-D texture read its third
    * component as 0), and a volume of up to 16^3 texels. */
   for (unsigned l = 0; l < VARYING_SLOT_MAX; l++) {
      unsigned comps = 0;
      bool small = true;

      for (unsigned c = 0; c < 4; c++) {
         uint16_t size = 0;
         float lo, span;

         if (need[l][c] == 0.0f)
            continue;
         comps++;
         ph_fs_ramp_size(need[l][c], &size, &lo, &span);
         small &= size <= 16;
      }
      groups += DIV_ROUND_UP(comps, 2);
      if (comps == 3 && small && !used_units)
         per = 3;
   }
   if (per == 3 && groups != 2)
      per = 2;
   else if (per == 3)
      groups = 1;
   if (groups > free_units) {
      *reject = "a floor with no free texture unit for its ramp";
      return false;
   }
   for (unsigned l = 0, u = 0; l < VARYING_SLOT_MAX; l++) {
      struct ph_fs_ramp *ramp = NULL;

      for (unsigned c = 0; c < 4; c++) {
         if (need[l][c] == 0.0f)
            continue;
         if (!ramp || ramp->dims == per) {
            while (used_units & (1u << u))
               u++;
            ramp = &ramps[n++];
            memset(ramp, 0, sizeof(*ramp));
            ramp->unit = u++;
            ramp->location = l;
         }
         ramp->comp[ramp->dims] = c;
         ph_fs_ramp_size(need[l][c], &ramp->size[ramp->dims],
                         &ramp->lo[ramp->dims], &ramp->span[ramp->dims]);
         ramp->dims++;
      }
   }
   /* A two-dimensional unit with one component keeps a 1-texel second row. */
   for (unsigned r = 0; r < n; r++)
      for (unsigned d = ramps[r].dims; d < (ramps[r].dims == 3 ? 3u : 2u); d++) {
         ramps[r].size[d] = 1;
         ramps[r].span[d] = 1.0f;
      }
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            nir_alu_instr *alu;
            nir_def *value[4];
            unsigned location;
            uint8_t comp[4];
            float k[4];

            if (instr->type != nir_instr_type_alu ||
                (alu = nir_instr_as_alu(instr))->op != nir_op_ffloor ||
                !ph_fs_ramp_match(alu, &location, comp, k))
               continue;
            b.cursor = nir_before_instr(instr);
            {
               /* Texel channel c holds the floor of varying component c and
                * every other channel is zero, so the ramps of one varying
                * add up to the whole floored vector. */
               nir_def *sum = NULL;
               nir_def *coord = nir_pad_vector_imm_int(&b, alu->src[0].src.ssa,
                                                       0, 4);

               for (unsigned r = 0; r < n; r++) {
                  nir_def *t;

                  if (ramps[r].location != location)
                     continue;
                  /* The coordinate is only traced; the unit reads the
                   * staged ramp coordinate. */
                  t = ph_fs_ramp_sample(&b, &ramps[r], coord);
                  sum = sum ? nir_fadd(&b, sum, t) : t;
               }
               {
                  unsigned swz[4];
                  float scale[4] = { 0 }, offset[4] = { 0 };

                  for (unsigned c = 0; c < alu->def.num_components; c++) {
                     swz[c] = comp[c] & 3;
                     for (unsigned r = 0; r < n; r++)
                        for (unsigned d = 0; d < ramps[r].dims; d++)
                           if (ramps[r].location == location &&
                               ramps[r].comp[d] == swz[c]) {
                              scale[c] = 255.0f /
                                 (float)ph_fs_ramp_step(ramps[r].size[d]);
                              offset[c] = k[c] * ramps[r].lo[d];
                           }
                  }
                  /* floor(k x) = texel index + k * lo, as one vector op */
                  value[0] = nir_fadd(&b, nir_fmul(&b,
                                nir_swizzle(&b, sum, swz, alu->def.num_components),
                                nir_build_imm(&b, alu->def.num_components, 32,
                                   (nir_const_value[4]){
                                      nir_const_value_for_float(scale[0], 32),
                                      nir_const_value_for_float(scale[1], 32),
                                      nir_const_value_for_float(scale[2], 32),
                                      nir_const_value_for_float(scale[3], 32) })),
                                nir_build_imm(&b, alu->def.num_components, 32,
                                   (nir_const_value[4]){
                                      nir_const_value_for_float(offset[0], 32),
                                      nir_const_value_for_float(offset[1], 32),
                                      nir_const_value_for_float(offset[2], 32),
                                      nir_const_value_for_float(offset[3], 32) }));
               }
            }
            nir_def_replace(&alu->def, value[0]);
         }
      }
      nir_progress(true, impl, nir_metadata_none);
   }
   *count = n;
   return true;
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
   return ph_fs_compile_ramps(nir, max_instructions, NULL, 0, out);
}

bool
ph_fs_compile_ramps(nir_shader *nir, unsigned max_instructions,
                    const struct ph_fs_ramp *ramps, unsigned ramp_count,
                    struct ph_fs_program *out)
{
   if (ramp_count > ARRAY_SIZE(out->ramps))
      return false;
   if (!ph_fs_compile_key(nir, max_instructions, NULL, 0, ramps, ramp_count,
                          0, out))
      return false;
   memcpy(out->ramps, ramps, ramp_count * sizeof(*ramps));
   out->ramp_count = ramp_count;
   /* Only runtime constants and scaled carriers can ask for a variant. */
   if (out->const_ref_count || out->carriers) {
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

            if (in->kind == PH_FS_PRE_BUFFER && in->index >= PH_FS_DRV_BASE)
               src[s][c].f32 = in->index - PH_FS_DRV_BASE < PH_FS_DRV_COUNT ?
                               program->drv[in->index - PH_FS_DRV_BASE] : 0.0f;
            else if (in->kind == PH_FS_PRE_BUFFER)
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
      if (getenv("PARHELIA_TRACE_PRE"))
         fprintf(stderr, "pre %u: %s/%u size %u (%u:%u %g, %u:%u %g, %u:%u %g) -> %#llx %g\n", i,
                 nir_op_infos[node->op].name, node->bit_size, size,
                 node->src[0][0].kind, node->src[0][0].index, src[0][0].f32,
                 node->src[1][0].kind, node->src[1][0].index, src[1][0].f32,
                 node->src[2][0].kind, node->src[2][0].index, src[2][0].f32,
                 (unsigned long long)program->pre_value[i].u64,
                 program->pre_value[i].f32);
   }
}

static float
ph_fs_ref_value(const struct ph_fs_program *program,
                const struct ph_fs_const_ref *ref, unsigned c,
                const float *values, unsigned size)
{
   if (ref->offset[c] >= PH_FS_DRV_BASE)
      return ref->offset[c] - PH_FS_DRV_BASE < PH_FS_DRV_COUNT ?
             program->drv[ref->offset[c] - PH_FS_DRV_BASE] : 0.0f;
   if (ref->offset[c] >= PH_FS_PRE_BASE)
      return ref->offset[c] - PH_FS_PRE_BASE < program->pre_count ?
             program->pre_value[ref->offset[c] - PH_FS_PRE_BASE].f32 : 0.0f;
   return values && (ref->offset[c] + 1u) * sizeof(float) <= size ?
          values[ref->offset[c]] : 0.0f;
}

/* A constant the instruction only moves into a saturated result: clamping
 * it to the register range first changes nothing, at any magnitude. */
static bool
ph_fs_ref_saturated(const struct ph_fs_program *program,
                    const struct ph_fs_const_ref *ref, unsigned c)
{
   const struct p3d_comb_inst *in = &program->comb.inst[ref->instruction];
   const struct p3d_comb_half *h = c == 3 ? &in->alpha : &in->rgb;

   return h->used && h->op == P3D_COMB_MOV && h->sat && !h->scale &&
          h->src[0].reg == P3D_COMB_C0 + ref->slot &&
          h->src[0].mod == P3D_COMB_MOD_NONE;
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
   unsigned count = program->uexp_count, scaled = program->scaled_key;
   bool grown = false;

   if (!program->nir)
      return 0;
   /* A carrier off the identity domain needs the scaling variant. */
   for (unsigned k = 0; k < PH_FS_CARRIERS; k++)
      for (unsigned c = 0; c < 4; c++)
         if ((program->carriers & (1u << k)) &&
             (program->drv[PH_FS_DRV_SPAN(k, c)] != 1.0f ||
              program->drv[PH_FS_DRV_LO(k, c)] != 0.0f))
            scaled |= 1u << k;
   grown = scaled != program->scaled_key;
   uexp = malloc((count + 4 * program->const_ref_count + 1) * sizeof(*uexp));
   if (!uexp)
      return -1;
   memcpy(uexp, program->uexp, count * sizeof(*uexp));
   for (unsigned i = 0; i < program->const_ref_count; i++) {
      const struct ph_fs_const_ref *ref = &program->const_ref[i];

      for (unsigned c = 0; c < 4; c++) {
         float m;
         unsigned e = 0, k;

         if (!(ref->mask & (1u << c)) || ph_fs_ref_saturated(program, ref, c))
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
                          program->ramps, program->ramp_count, scaled,
                          &variant)) {
      /* The program stays as it was: only this draw is refused. */
      if (getenv("PARHELIA_TRACE_FS")) {
         fprintf(stderr, "parhelia: fs variant refused: %s; exponents",
                 variant.reject ? variant.reject : "?");
         for (unsigned k = 0; k < count; k++)
            fprintf(stderr, " %u:%u", uexp[k].offset, uexp[k].e);
         fprintf(stderr, "\n");
      }
      free(uexp);
      return -1;
   }
   memcpy(variant.ramps, program->ramps, sizeof(variant.ramps));
   variant.ramp_count = program->ramp_count;
   memcpy(variant.drv, program->drv, sizeof(variant.drv));
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

void
ph_fs_set_domain(struct ph_fs_program *program, unsigned carrier,
                 const float lo[4], const float span[4])
{
   if (carrier >= PH_FS_CARRIERS)
      return;
   for (unsigned c = 0; c < 4; c++) {
      program->drv[PH_FS_DRV_SPAN(carrier, c)] = span[c];
      program->drv[PH_FS_DRV_LO(carrier, c)] = lo[c];
   }
}

bool
ph_fs_update_constants(struct ph_fs_program *program, const void *constants,
                       unsigned size)
{
   const float *values = constants;
   int rescaled = 0;

   ph_fs_run_preshader(program, values, size);
   /* A scaling variant brings new constants whose exponents the next pass
    * sees; exponents only grow, so this settles. */
   for (unsigned pass = 0; pass < 4; pass++) {
      int r = ph_fs_rescale(program, values, size);

      if (r <= 0) {
         rescaled = r < 0 ? -1 : pass ? 1 : 0;
         break;
      }
      rescaled = 1;
      ph_fs_run_preshader(program, values, size);
   }
   for (unsigned i = 0; i < program->const_ref_count; i++) {
      const struct ph_fs_const_ref *ref = &program->const_ref[i];
      float *target = program->comb.inst[ref->instruction].constant[ref->slot];

      for (unsigned c = 0; c < 4; c++) {
         float v;

         if (!(ref->mask & (1u << c)))
            continue;
         /* The register holds [-1, 1]; an infinity or NaN (log2(0)) must
          * not reach the fixed-point conversion. */
         v = ldexpf(ph_fs_ref_value(program, ref, c, values, size),
                    -(int)ref->e[c]);
         target[c] = isnan(v) ? 0.0f : CLAMP(v, -1.0f, 1.0f);
      }
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
