/*
 * NIR -> Parhelia fragment-combiner compiler.
 *
 * The combiner is a small three-source ALU with ten slots, two writable
 * registers (r0 is also the fragment result) and two per-slot constants.  A
 * slot co-issues an RGB and an alpha operation over the same register file,
 * so the RGB and alpha halves of a shader are compiled as two independent
 * instruction lists and then zipped slot by slot.
 *
 * This replaces the previous instruction-counting matcher, which recognised
 * four fixed shapes.  Anything the expression set below does not cover is
 * reported as uncompilable and the caller falls back to Gallium's draw
 * module, so the driver never silently renders the wrong algebra.
 *
 * Encoding and evidence: docs/parhelia-fragment-isa.md,
 * docs/parhelia-p690-ganged-pipe-ta-re.md.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include <string.h>

#include "parhelia_fs_compile.h"

#define PH_FS_MAX_VALUES 256u

/* One compiled operand: either a leaf (texture, varying, constant) or a
 * register holding a previously computed subexpression. */
struct ph_fs_value {
   uint8_t reg;
   uint8_t mod;
   uint8_t rep;
   bool is_register;    /* reg is r0/r1 and holds a computed value */
   bool is_constant;
   bool constant_is_literal;
   float literal[4];
   uint16_t vec4_index; /* constant buffer offset when not a literal */
};

struct ph_fs_half {
   struct p3d_comb_half inst[P3D_COMB_MAX_INSTRUCTIONS];
   /* Per instruction, the constants its sources refer to. */
   struct ph_fs_value constant[P3D_COMB_MAX_INSTRUCTIONS][2];
   uint8_t constants[P3D_COMB_MAX_INSTRUCTIONS];
   unsigned count;
};

struct ph_fs_ctx {
   nir_shader *nir;
   unsigned max_instructions;
   unsigned sampler_mask;
   bool uses_color;
   bool failed;
   /* Register liveness: bit 0 = r0, bit 1 = r1. */
   uint8_t live;
   struct ph_fs_half *half;
};

static void
ph_fs_fail(struct ph_fs_ctx *ctx)
{
   ctx->failed = true;
}

/* NIR channel index -> combiner replicate code. */
static uint8_t
ph_fs_replicate(unsigned channel)
{
   static const uint8_t code[4] = {
      P3D_COMB_REP_R, P3D_COMB_REP_G, P3D_COMB_REP_B, P3D_COMB_REP_A,
   };
   return code[channel & 3];
}

static bool
ph_fs_alloc(struct ph_fs_ctx *ctx, uint8_t *out)
{
   for (unsigned i = 0; i < 2; i++) {
      if (!(ctx->live & (1u << i))) {
         ctx->live |= 1u << i;
         *out = P3D_COMB_R0 + i;
         return true;
      }
   }
   ph_fs_fail(ctx);
   return false;
}

static void
ph_fs_release(struct ph_fs_ctx *ctx, const struct ph_fs_value *value)
{
   if (value->is_register)
      ctx->live &= ~(1u << (value->reg - P3D_COMB_R0));
}

/* Apply a modifier to a value.  Two modifiers on one operand cannot be
 * encoded except for the negate pairs the table already provides. */
static bool
ph_fs_apply_mod(struct ph_fs_value *value, uint8_t mod)
{
   if (value->mod == P3D_COMB_MOD_NONE) {
      value->mod = mod;
      return true;
   }
   if (mod == P3D_COMB_MOD_NEG) {
      switch (value->mod) {
      case P3D_COMB_MOD_BIAS:   value->mod = P3D_COMB_MOD_BIAS_NEG; return true;
      case P3D_COMB_MOD_SIGNED: value->mod = P3D_COMB_MOD_SIGNED_NEG; return true;
      case P3D_COMB_MOD_NEG:    value->mod = P3D_COMB_MOD_NONE; return true;
      default: break;
      }
   }
   return false;
}

/* ---- leaves ---------------------------------------------------------- */

static bool
ph_fs_leaf(struct ph_fs_ctx *ctx, nir_def *def, struct ph_fs_value *out)
{
   nir_instr *instr = nir_def_instr(def);

   memset(out, 0, sizeof(*out));
   switch (instr->type) {
   case nir_instr_type_tex: {
      const nir_tex_instr *tex = nir_instr_as_tex(instr);

      if (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
          tex->op != nir_texop_txl)
         return false;
      if (tex->sampler_index > 3)
         return false;
      ctx->sampler_mask |= 1u << tex->sampler_index;
      out->reg = P3D_COMB_T0 + tex->sampler_index;
      return true;
   }
   case nir_instr_type_load_const: {
      const nir_load_const_instr *load = nir_instr_as_load_const(instr);

      if (load->def.bit_size != 32)
         return false;
      out->is_constant = true;
      out->constant_is_literal = true;
      for (unsigned i = 0; i < 4; i++)
         out->literal[i] = nir_const_value_as_float(
            load->value[i < load->def.num_components ? i : 0], 32);
      out->reg = P3D_COMB_C0;
      return true;
   }
   case nir_instr_type_intrinsic: {
      const nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
      unsigned base;

      switch (load->intrinsic) {
      case nir_intrinsic_load_input:
      case nir_intrinsic_load_interpolated_input: {
         unsigned location = nir_intrinsic_io_semantics(load).location;

         if (location == VARYING_SLOT_COL0)
            out->reg = P3D_COMB_V0;
         else if (location == VARYING_SLOT_COL1)
            out->reg = P3D_COMB_V1;
         else
            return false;
         ctx->uses_color = true;
         return true;
      }
      case nir_intrinsic_load_uniform:
         if (!nir_src_is_const(load->src[0]))
            return false;
         base = nir_intrinsic_base(load) +
                nir_src_as_uint(load->src[0]);
         if (base % 16u)
            return false;
         out->is_constant = true;
         out->vec4_index = base / 16u;
         out->reg = P3D_COMB_C0;
         return true;
      case nir_intrinsic_load_ubo:
         if (!nir_src_is_const(load->src[0]) ||
             nir_src_as_uint(load->src[0]) != 0 ||
             !nir_src_is_const(load->src[1]))
            return false;
         base = nir_src_as_uint(load->src[1]);
         if (base % 16u)
            return false;
         out->is_constant = true;
         out->vec4_index = base / 16u;
         out->reg = P3D_COMB_C0;
         return true;
      default:
         return false;
      }
   }
   default:
      return false;
   }
}

/* ---- instruction emission -------------------------------------------- */

static bool
ph_fs_same_constant(const struct ph_fs_value *a, const struct ph_fs_value *b)
{
   if (a->constant_is_literal != b->constant_is_literal)
      return false;
   if (!a->constant_is_literal)
      return a->vec4_index == b->vec4_index;
   return !memcmp(a->literal, b->literal, sizeof(a->literal));
}

/* Assign the constant registers of one instruction.  Only two exist. */
static bool
ph_fs_place_constants(struct ph_fs_ctx *ctx, unsigned index,
                      struct ph_fs_value *src, unsigned count)
{
   struct ph_fs_half *half = ctx->half;

   for (unsigned i = 0; i < count; i++) {
      unsigned slot;

      if (!src[i].is_constant)
         continue;
      for (slot = 0; slot < half->constants[index]; slot++)
         if (ph_fs_same_constant(&half->constant[index][slot], &src[i]))
            break;
      if (slot == half->constants[index]) {
         if (slot >= 2)
            return false;
         half->constant[index][slot] = src[i];
         half->constants[index]++;
      }
      src[i].reg = slot ? P3D_COMB_C1 : P3D_COMB_C0;
   }
   return true;
}

static bool
ph_fs_emit(struct ph_fs_ctx *ctx, uint8_t op, struct ph_fs_value *src,
           unsigned count, struct ph_fs_value *out)
{
   struct ph_fs_half *half = ctx->half;
   struct p3d_comb_half *inst;
   unsigned index = half->count;
   uint8_t dst = 0xff;

   if (index >= ctx->max_instructions) {
      ph_fs_fail(ctx);
      return false;
   }
   if (!ph_fs_place_constants(ctx, index, src, count)) {
      ph_fs_fail(ctx);
      return false;
   }
   /* Reuse one operand's register as the destination where possible. */
   for (unsigned i = 0; i < count; i++)
      if (src[i].is_register && dst == 0xff)
         dst = src[i].reg;
   for (unsigned i = 0; i < count; i++)
      if (src[i].is_register && src[i].reg != dst)
         ph_fs_release(ctx, &src[i]);
   if (dst == 0xff && !ph_fs_alloc(ctx, &dst))
      return false;

   inst = &half->inst[index];
   memset(inst, 0, sizeof(*inst));
   inst->used = 1;
   inst->op = op;
   inst->dst = dst;
   inst->sat = 1;
   inst->scale = P3D_COMB_SCALE_1;
   for (unsigned i = 0; i < count; i++) {
      inst->src[i].reg = src[i].reg;
      inst->src[i].mod = src[i].mod;
      inst->src[i].rep = src[i].rep;
   }
   /* An operand the opcode does not read is left at register 0, as every
    * vendor emitter leaves it. */
   for (unsigned i = count; i < 3; i++)
      inst->src[i].reg = P3D_COMB_T0;
   half->count++;

   memset(out, 0, sizeof(*out));
   out->reg = dst;
   out->is_register = true;
   return true;
}

/* ---- expression lowering --------------------------------------------- */

static bool ph_fs_value(struct ph_fs_ctx *ctx, nir_def *def, bool alpha,
                        struct ph_fs_value *out);

/* Read one ALU operand, folding its swizzle into a replicate code.  The
 * hardware has no general swizzle: a source is read either naturally or with
 * one channel replicated across all four. */
static bool
ph_fs_operand(struct ph_fs_ctx *ctx, const nir_alu_instr *alu, unsigned index,
              bool alpha, struct ph_fs_value *out)
{
   const nir_alu_src *src = &alu->src[index];
   unsigned components = nir_ssa_alu_instr_src_components(alu, index);
   unsigned channel;

   if (!ph_fs_value(ctx, src->src.ssa, alpha, out))
      return false;

   if (alpha) {
      /* The alpha pipe reads component 3, or the only component of a scalar
       * alpha expression. */
      channel = src->swizzle[components >= 4 ? 3 : components - 1];
      if (channel == 3)
         return true;
      if (out->rep != P3D_COMB_REP_NONE)
         return false;
      out->rep = ph_fs_replicate(channel);
      return true;
   }

   if (components >= 3 && src->swizzle[0] == 0 && src->swizzle[1] == 1 &&
       src->swizzle[2] == 2)
      return true;                       /* natural rgb read */
   channel = src->swizzle[0];
   for (unsigned i = 1; i < components && i < 3; i++)
      if (src->swizzle[i] != channel)
         return false;
   if (out->rep != P3D_COMB_REP_NONE)
      return false;
   out->rep = ph_fs_replicate(channel);
   return true;
}

/* fadd(1.0, fneg(x)) and its commuted form are the invert modifier. */
static bool
ph_fs_try_invert(struct ph_fs_ctx *ctx, const nir_alu_instr *alu, bool alpha,
                 struct ph_fs_value *out)
{
   for (unsigned i = 0; i < 2; i++) {
      const nir_alu_instr *neg;

      if (!nir_src_is_const(alu->src[i].src) ||
          nir_src_comp_as_float(alu->src[i].src, alu->src[i].swizzle[0]) != 1.0f)
         continue;
      if (nir_def_instr(alu->src[1 - i].src.ssa)->type != nir_instr_type_alu)
         continue;
      neg = nir_instr_as_alu(nir_def_instr(alu->src[1 - i].src.ssa));
      if (neg->op != nir_op_fneg)
         continue;
      if (!ph_fs_operand(ctx, alu, 1 - i, alpha, out))
         return false;
      /* ph_fs_operand already folded the fneg into MOD_NEG. */
      if (out->mod != P3D_COMB_MOD_NEG)
         return false;
      out->mod = P3D_COMB_MOD_INVERT;
      return true;
   }
   return false;
}

static bool
ph_fs_alu(struct ph_fs_ctx *ctx, const nir_alu_instr *alu, bool alpha,
          struct ph_fs_value *out)
{
   struct ph_fs_value src[3];
   uint8_t op;
   unsigned count;

   switch (alu->op) {
   case nir_op_mov:
      return ph_fs_operand(ctx, alu, 0, alpha, out);
   case nir_op_fneg:
      if (!ph_fs_operand(ctx, alu, 0, alpha, out))
         return false;
      return ph_fs_apply_mod(out, P3D_COMB_MOD_NEG);
   case nir_op_fsat:
      /* Every instruction this compiler emits already saturates. */
      if (!ph_fs_operand(ctx, alu, 0, alpha, out))
         return false;
      if (out->is_register)
         return true;
      src[0] = *out;
      src[1] = src[2] = (struct ph_fs_value){ .reg = P3D_COMB_T0 };
      return ph_fs_emit(ctx, P3D_COMB_MOV, src, 1, out);
   case nir_op_fmul:
      op = P3D_COMB_MUL;
      count = 2;
      break;
   case nir_op_fadd:
      if (ph_fs_try_invert(ctx, alu, alpha, out))
         return true;
      if (ctx->failed)
         return false;
      op = P3D_COMB_ADD;
      count = 2;
      break;
   case nir_op_ffma:
      op = P3D_COMB_MAD;
      count = 3;
      break;
   case nir_op_flrp:
      op = P3D_COMB_LRP;
      count = 3;
      break;
   case nir_op_fdot3:
      op = P3D_COMB_DP3;
      count = 2;
      break;
   case nir_op_fdot4:
      op = P3D_COMB_DP4;
      count = 2;
      break;
   default:
      return false;
   }

   for (unsigned i = 0; i < count; i++)
      if (!ph_fs_operand(ctx, alu, i, alpha, &src[i]))
         return false;

   /* Map the source order onto the hardware's A/B/C operands:
    *   MUL  dst = A * C          ADD  dst = A + B
    *   MAD  dst = A * C + B      LRP  dst = A * C + B * (1 - C)
    *   DP3/DP4 dst = dot(A, C) */
   switch (op) {
   case P3D_COMB_MUL:
   case P3D_COMB_DP3:
   case P3D_COMB_DP4: {
      /* B is not read by these opcodes and stays at register 0. */
      struct ph_fs_value ordered[3] = { src[0], { .reg = P3D_COMB_T0 }, src[1] };

      return ph_fs_emit(ctx, op, ordered, 3, out);
   }
   case P3D_COMB_ADD:
      return ph_fs_emit(ctx, op, src, 2, out);
   case P3D_COMB_MAD: {
      /* NIR ffma(a, b, c) = a * b + c */
      struct ph_fs_value ordered[3] = { src[0], src[2], src[1] };

      return ph_fs_emit(ctx, op, ordered, 3, out);
   }
   case P3D_COMB_LRP: {
      /* NIR flrp(a, b, t) = a * (1 - t) + b * t; the hardware computes
       * A * C + B * (1 - C), so A = b, B = a, C = t. */
      struct ph_fs_value ordered[3] = { src[1], src[0], src[2] };

      return ph_fs_emit(ctx, op, ordered, 3, out);
   }
   default:
      return false;
   }
}

static bool
ph_fs_value(struct ph_fs_ctx *ctx, nir_def *def, bool alpha,
            struct ph_fs_value *out)
{
   if (ctx->failed)
      return false;
   if (ph_fs_leaf(ctx, def, out))
      return true;
   if (nir_def_instr(def)->type != nir_instr_type_alu)
      return false;
   return ph_fs_alu(ctx, nir_instr_as_alu(nir_def_instr(def)), alpha, out);
}

/* ---- program assembly ------------------------------------------------- */

/* Compile one half and make sure its result ends up in r0. */
static bool
ph_fs_compile_half(struct ph_fs_ctx *ctx, nir_def *def, bool alpha,
                   struct ph_fs_half *half)
{
   struct ph_fs_value value, move[3];

   memset(half, 0, sizeof(*half));
   ctx->half = half;
   ctx->live = 0;
   ctx->failed = false;
   if (!ph_fs_value(ctx, def, alpha, &value) || ctx->failed)
      return false;
   if (value.is_register && value.reg == P3D_COMB_R0 &&
       value.mod == P3D_COMB_MOD_NONE && value.rep == P3D_COMB_REP_NONE)
      return true;
   move[0] = value;
   move[1] = move[2] = (struct ph_fs_value){ .reg = P3D_COMB_T0 };
   if (!ph_fs_emit(ctx, P3D_COMB_MOV, move, 1, &value) || ctx->failed)
      return false;
   if (value.reg != P3D_COMB_R0) {
      /* The allocator reused r1; force the result register. */
      half->inst[half->count - 1].dst = P3D_COMB_R0;
   }
   return true;
}

/* Find the vec4 written to the colour output, and split it into the
 * expression feeding rgb and the one feeding alpha. */
static bool
ph_fs_output(nir_shader *nir, nir_def **rgb, nir_def **alpha)
{
   nir_def *value = NULL;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            const nir_intrinsic_instr *store;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            store = nir_instr_as_intrinsic(instr);
            if (store->intrinsic != nir_intrinsic_store_output)
               continue;
            if (nir_intrinsic_io_semantics(store).location !=
                   FRAG_RESULT_COLOR &&
                nir_intrinsic_io_semantics(store).location != FRAG_RESULT_DATA0)
               return false;
            if (value)
               return false;         /* more than one colour store */
            if (nir_intrinsic_write_mask(store) != 0xf)
               return false;
            value = store->src[0].ssa;
         }
      }
   }
   if (!value)
      return false;

   *rgb = *alpha = value;
   /* vec4(x, y, z, w) with a common rgb source and a separate alpha. */
   if (nir_def_instr(value)->type == nir_instr_type_alu) {
      const nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(value));

      if (alu->op == nir_op_vec4) {
         for (unsigned i = 1; i < 3; i++)
            if (alu->src[i].src.ssa != alu->src[0].src.ssa ||
                alu->src[i].swizzle[0] != alu->src[0].swizzle[0] + i)
               return false;
         *rgb = alu->src[0].src.ssa;
         *alpha = alu->src[3].src.ssa;
      }
   }
   return true;
}

/* Zip the two halves slot by slot, merging their constant references. */
static bool
ph_fs_merge(const struct ph_fs_half *rgb, const struct ph_fs_half *alpha,
            unsigned max_instructions, struct ph_fs_program *out)
{
   unsigned count = rgb->count > alpha->count ? rgb->count : alpha->count;

   if (!count || count > max_instructions)
      return false;
   memset(&out->comb, 0, sizeof(out->comb));
   out->const_ref_count = 0;
   out->comb.count = count;
   for (unsigned i = 0; i < count; i++) {
      struct p3d_comb_inst *inst = &out->comb.inst[i];
      const struct ph_fs_value *source[4];
      unsigned sources = 0;

      if (i < rgb->count)
         inst->rgb = rgb->inst[i];
      if (i < alpha->count)
         inst->alpha = alpha->inst[i];
      /* A dot product writes all four channels, so a slot carrying one must
       * carry the same opcode in both halves. */
      if ((inst->rgb.op == P3D_COMB_DP3 || inst->rgb.op == P3D_COMB_DP4 ||
           inst->alpha.op == P3D_COMB_DP3 || inst->alpha.op == P3D_COMB_DP4) &&
          (!inst->rgb.used || !inst->alpha.used ||
           inst->rgb.op != inst->alpha.op))
         return false;
      if (i < rgb->count)
         for (unsigned k = 0; k < rgb->constants[i]; k++)
            source[sources++] = &rgb->constant[i][k];
      if (i < alpha->count)
         for (unsigned k = 0; k < alpha->constants[i]; k++) {
            unsigned existing;

            for (existing = 0; existing < sources; existing++)
               if (ph_fs_same_constant(source[existing], &alpha->constant[i][k]))
                  break;
            if (existing < sources) {
               /* The alpha half must name the register the rgb half got. */
               for (unsigned s = 0; s < 3; s++)
                  if (inst->alpha.src[s].reg ==
                      (k ? P3D_COMB_C1 : P3D_COMB_C0))
                     inst->alpha.src[s].reg =
                        existing ? P3D_COMB_C1 : P3D_COMB_C0;
               continue;
            }
            if (sources >= 2)
               return false;
            for (unsigned s = 0; s < 3; s++)
               if (inst->alpha.src[s].reg == (k ? P3D_COMB_C1 : P3D_COMB_C0))
                  inst->alpha.src[s].reg =
                     sources ? P3D_COMB_C1 : P3D_COMB_C0;
            source[sources++] = &alpha->constant[i][k];
         }
      for (unsigned k = 0; k < sources; k++) {
         inst->has_const[k] = 1;
         if (source[k]->constant_is_literal) {
            memcpy(inst->constant[k], source[k]->literal,
                   sizeof(inst->constant[k]));
         } else {
            struct ph_fs_const_ref *ref =
               &out->const_ref[out->const_ref_count++];

            ref->instruction = i;
            ref->slot = k;
            ref->vec4_index = source[k]->vec4_index;
         }
      }
   }
   return true;
}

bool
ph_fs_compile(nir_shader *nir, unsigned max_instructions,
              struct ph_fs_program *out)
{
   struct ph_fs_ctx ctx = {
      .nir = nir,
      .max_instructions = max_instructions > P3D_COMB_MAX_INSTRUCTIONS ?
                          P3D_COMB_MAX_INSTRUCTIONS : max_instructions,
   };
   struct ph_fs_half rgb, alpha;
   nir_def *rgb_def, *alpha_def;

   memset(out, 0, sizeof(*out));
   if (!max_instructions || !ph_fs_output(nir, &rgb_def, &alpha_def))
      return false;
   /* One block and no control flow: the combiner cannot branch. */
   nir_foreach_function_impl(impl, nir) {
      if (exec_list_length(&impl->body) != 1)
         return false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               const nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

               if (in->intrinsic == nir_intrinsic_terminate ||
                   in->intrinsic == nir_intrinsic_terminate_if ||
                   in->intrinsic == nir_intrinsic_demote ||
                   in->intrinsic == nir_intrinsic_demote_if)
                  return false;
            }
         }
      }
   }

   if (!ph_fs_compile_half(&ctx, rgb_def, false, &rgb))
      return false;
   if (rgb_def == alpha_def) {
      /* Identical algebra: the alpha half is the same instruction list, so
       * the two halves align with no padding. */
      alpha = rgb;
   } else if (!ph_fs_compile_half(&ctx, alpha_def, true, &alpha)) {
      return false;
   }
   if (!ph_fs_merge(&rgb, &alpha, ctx.max_instructions, out))
      return false;
   out->sampler_mask = ctx.sampler_mask;
   out->uses_color = ctx.uses_color;
   out->valid = true;
   return true;
}

void
ph_fs_update_constants(struct ph_fs_program *program, const void *constants,
                       unsigned size)
{
   const float *values = constants;

   for (unsigned i = 0; i < program->const_ref_count; i++) {
      const struct ph_fs_const_ref *ref = &program->const_ref[i];
      float *target = program->comb.inst[ref->instruction].constant[ref->slot];

      if (!values || (ref->vec4_index + 1u) * 16u > size) {
         memset(target, 0, 4 * sizeof(float));
         continue;
      }
      memcpy(target, values + ref->vec4_index * 4u, 4 * sizeof(float));
   }
}
