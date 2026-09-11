/*
 * NIR -> Parhelia WARP vertex-program compiler.
 *
 * The WARP engine is a dual-issue vector machine with 16 temporaries, 16
 * latches and a constant file that doubles as the engine's block-state file.
 * This compiler lowers the expressions feeding a vertex shader's outputs into
 * WARP bundles, then appends the same perspective-divide and viewport tail the
 * hardware-proven kernel transform program uses, because the setup unit wants
 * a window-space position in o0.
 *
 * Unlike the fixed-function shape matcher next door, nothing here is pattern
 * matching: an output is compiled if its expression tree is made of operations
 * the engine has.  Everything else is refused so the caller keeps Gallium's
 * draw module rather than rendering the wrong algebra.
 *
 * Encoding and evidence: docs/parhelia-p690-warp-isa-re.md.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include <string.h>

#include "parhelia_vs_compile.h"

/* r0..r13 are free; the vendor epilogue owns r14/r15.  T0 latches the fetched
 * position, T1 the viewport offset and T2 is the temporary every draw depends
 * on, so none of the three may be allocated. */
#define PH_VS_TEMPS 14u

struct ph_vs_value {
   uint8_t file;        /* enum p3d_warp_src_file */
   uint8_t index;
   uint8_t neg;
   uint16_t swizzle;
   bool is_temp;        /* index names an allocated temporary */
};

struct ph_vs_ctx {
   struct ph_vs_program *program;
   const struct ph_vs_inputs *inputs;
   unsigned constant_first;
   unsigned constant_limit;     /* highest constant-buffer vec4 read, + 1 */
   uint16_t live;               /* allocated temporaries */
   bool failed;
};

static void
ph_vs_fail(struct ph_vs_ctx *ctx)
{
   ctx->failed = true;
}

static bool
ph_vs_alloc(struct ph_vs_ctx *ctx, uint8_t *out)
{
   for (unsigned i = 0; i < PH_VS_TEMPS; i++) {
      if (!(ctx->live & (1u << i))) {
         ctx->live |= 1u << i;
         *out = i;
         return true;
      }
   }
   ph_vs_fail(ctx);
   return false;
}

static void
ph_vs_release(struct ph_vs_ctx *ctx, const struct ph_vs_value *value)
{
   if (value->is_temp)
      ctx->live &= ~(1u << value->index);
}

static void
ph_vs_emit(struct ph_vs_ctx *ctx, const struct p3d_warp_inst *in)
{
   struct ph_vs_program *program = ctx->program;

   /* Two instructions of headroom for END and its padding word. */
   if (program->count + 2 >= PH_VS_MAX_INSTRUCTIONS) {
      ph_vs_fail(ctx);
      return;
   }
   p3d_warp_encode(in, program->code + program->count * 3);
   program->count++;
}

/* A source with an all-zero or all-one swizzle is the engine's way of
 * materialising a literal without spending a constant slot. */
static struct ph_vs_value
ph_vs_literal(float value)
{
   struct ph_vs_value out = {
      .file = P3D_WARP_FILE_R, .index = 1,
      .swizzle = value == 0.0f ? P3D_WARP_SWZ_ZERO : P3D_WARP_SWZ_ONE,
   };

   return out;
}

static struct p3d_warp_src
ph_vs_src(const struct ph_vs_value *value)
{
   struct p3d_warp_src src = {
      .file = value->file, .index = value->index, .neg = value->neg,
      .swizzle = value->swizzle,
   };

   return src;
}

/* ---- leaves ---------------------------------------------------------- */

static bool
ph_vs_leaf(struct ph_vs_ctx *ctx, nir_def *def, struct ph_vs_value *out)
{
   nir_instr *instr = nir_def_instr(def);
   const nir_intrinsic_instr *load;
   unsigned base, location;

   memset(out, 0, sizeof(*out));
   out->swizzle = P3D_WARP_SWZ_IDENTITY;

   if (instr->type == nir_instr_type_load_const) {
      const nir_load_const_instr *value = nir_instr_as_load_const(instr);
      float first;

      if (value->def.bit_size != 32)
         return false;
      first = nir_const_value_as_float(value->value[0], 32);
      for (unsigned i = 1; i < value->def.num_components; i++)
         if (nir_const_value_as_float(value->value[i], 32) != first)
            return false;
      if (first != 0.0f && first != 1.0f)
         return false;          /* other literals need a constant slot */
      *out = ph_vs_literal(first);
      return true;
   }
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   load = nir_instr_as_intrinsic(instr);
   switch (load->intrinsic) {
   case nir_intrinsic_load_input:
      location = nir_intrinsic_base(load);
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]))
         return false;
      if (location >= ctx->inputs->count || ctx->inputs->slot[location] < 0)
         return false;
      out->file = P3D_WARP_FILE_V;
      out->index = (uint8_t)ctx->inputs->slot[location];
      ctx->program->input_mask |= 1u << out->index;
      return true;
   case nir_intrinsic_load_uniform:
      if (!nir_src_is_const(load->src[0]))
         return false;
      base = nir_intrinsic_base(load) + nir_src_as_uint(load->src[0]);
      break;
   case nir_intrinsic_load_ubo:
      if (!nir_src_is_const(load->src[0]) ||
          nir_src_as_uint(load->src[0]) != 0 ||
          !nir_src_is_const(load->src[1]))
         return false;
      base = nir_src_as_uint(load->src[1]);
      break;
   default:
      return false;
   }
   if (base % 16u)
      return false;
   base /= 16u;
   if (ctx->constant_first + base > 127u)
      return false;             /* beyond the direct constant index */
   if (base + 1u > ctx->constant_limit)
      ctx->constant_limit = base + 1u;
   out->file = P3D_WARP_FILE_C;
   out->index = (uint8_t)(ctx->constant_first + base);
   return true;
}

/* ---- expressions ------------------------------------------------------ */

static bool ph_vs_value(struct ph_vs_ctx *ctx, nir_def *def,
                        struct ph_vs_value *out);

/* The engine has no general swizzle: a source is read naturally, with one
 * component broadcast, or with the 0.0/1.0 literal codes. */
static bool
ph_vs_operand(struct ph_vs_ctx *ctx, const nir_alu_instr *alu, unsigned index,
              struct ph_vs_value *out)
{
   const nir_alu_src *src = &alu->src[index];
   unsigned components = nir_ssa_alu_instr_src_components(alu, index);
   unsigned channel = src->swizzle[0];
   bool identity = true;

   if (!ph_vs_value(ctx, src->src.ssa, out))
      return false;
   for (unsigned i = 0; i < components && i < 4; i++)
      if (src->swizzle[i] != i)
         identity = false;
   if (identity)
      return true;
   for (unsigned i = 1; i < components && i < 4; i++)
      if (src->swizzle[i] != channel)
         return false;          /* neither identity nor a broadcast */
   if (out->swizzle != P3D_WARP_SWZ_IDENTITY)
      return false;
   out->swizzle = P3D_WARP_SWZ(channel, channel, channel, channel);
   return true;
}

static bool
ph_vs_binary(struct ph_vs_ctx *ctx, uint8_t op, struct ph_vs_value *src,
             unsigned count, struct ph_vs_value *out)
{
   struct p3d_warp_inst in = { .op = op, .nsrc = count };
   uint8_t dst = 0xff;

   for (unsigned i = 0; i < count; i++)
      if (src[i].is_temp && dst == 0xff)
         dst = src[i].index;
   for (unsigned i = 0; i < count; i++)
      if (src[i].is_temp && src[i].index != dst)
         ph_vs_release(ctx, &src[i]);
   if (dst == 0xff && !ph_vs_alloc(ctx, &dst))
      return false;

   in.dst_file = P3D_WARP_FILE_DST_R;
   in.dst_index = dst;
   in.dst_mask = 0xf;
   for (unsigned i = 0; i < count; i++)
      in.src[i] = ph_vs_src(&src[i]);
   ph_vs_emit(ctx, &in);
   if (ctx->failed)
      return false;

   memset(out, 0, sizeof(*out));
   out->file = P3D_WARP_FILE_R;
   out->index = dst;
   out->swizzle = P3D_WARP_SWZ_IDENTITY;
   out->is_temp = true;
   return true;
}

static bool
ph_vs_alu(struct ph_vs_ctx *ctx, const nir_alu_instr *alu,
          struct ph_vs_value *out)
{
   struct ph_vs_value src[3];
   uint8_t op;
   unsigned count;

   switch (alu->op) {
   case nir_op_mov:
      return ph_vs_operand(ctx, alu, 0, out);
   case nir_op_fneg:
      if (!ph_vs_operand(ctx, alu, 0, out))
         return false;
      if (out->neg)
         return false;          /* one negate per operand */
      out->neg = 1;
      return true;
   case nir_op_fmul: op = P3D_WARP_MUL; count = 2; break;
   case nir_op_fadd: op = P3D_WARP_ADD; count = 2; break;
   case nir_op_fmax: op = P3D_WARP_MAX; count = 2; break;
   case nir_op_ffma: op = P3D_WARP_MAD; count = 3; break;
   case nir_op_fdot3: op = P3D_WARP_DP3; count = 2; break;
   case nir_op_fdot4: op = P3D_WARP_DP4; count = 2; break;
   default:
      return false;
   }
   for (unsigned i = 0; i < count; i++)
      if (!ph_vs_operand(ctx, alu, i, &src[i]))
         return false;
   return ph_vs_binary(ctx, op, src, count, out);
}

static bool
ph_vs_value(struct ph_vs_ctx *ctx, nir_def *def, struct ph_vs_value *out)
{
   if (ctx->failed)
      return false;
   if (ph_vs_leaf(ctx, def, out))
      return true;
   if (nir_def_instr(def)->type != nir_instr_type_alu)
      return false;
   return ph_vs_alu(ctx, nir_instr_as_alu(nir_def_instr(def)), out);
}

/* A vec4 built from four scalar dot products is the ordinary matrix product;
 * compile each component into one masked instruction instead of four moves. */
static bool
ph_vs_vector(struct ph_vs_ctx *ctx, nir_def *def, struct ph_vs_value *out)
{
   const nir_alu_instr *alu;
   uint8_t dst;

   if (nir_def_instr(def)->type != nir_instr_type_alu)
      return ph_vs_value(ctx, def, out);
   alu = nir_instr_as_alu(nir_def_instr(def));
   if (alu->op != nir_op_vec4)
      return ph_vs_value(ctx, def, out);

   if (!ph_vs_alloc(ctx, &dst))
      return false;
   for (unsigned i = 0; i < 4; i++) {
      struct ph_vs_value component, source[3];
      const nir_alu_instr *term;
      struct p3d_warp_inst in = {0};
      uint8_t op;
      unsigned count;

      if (nir_def_instr(alu->src[i].src.ssa)->type != nir_instr_type_alu)
         return false;
      term = nir_instr_as_alu(nir_def_instr(alu->src[i].src.ssa));
      switch (term->op) {
      case nir_op_fdot4: op = P3D_WARP_DP4; count = 2; break;
      case nir_op_fdot3: op = P3D_WARP_DP3; count = 2; break;
      case nir_op_fmul:  op = P3D_WARP_MUL; count = 2; break;
      case nir_op_fadd:  op = P3D_WARP_ADD; count = 2; break;
      default:
         return false;
      }
      for (unsigned k = 0; k < count; k++)
         if (!ph_vs_operand(ctx, term, k, &source[k]))
            return false;
      for (unsigned k = 0; k < count; k++) {
         if (source[k].is_temp)
            return false;       /* a component term must read leaves only */
         in.src[k] = ph_vs_src(&source[k]);
      }
      in.op = op;
      in.nsrc = count;
      in.dst_file = P3D_WARP_FILE_DST_R;
      in.dst_index = dst;
      in.dst_mask = 1u << i;
      ph_vs_emit(ctx, &in);
      if (ctx->failed)
         return false;
      (void)component;
   }
   memset(out, 0, sizeof(*out));
   out->file = P3D_WARP_FILE_R;
   out->index = dst;
   out->swizzle = P3D_WARP_SWZ_IDENTITY;
   out->is_temp = true;
   return true;
}

/* ---- program assembly ------------------------------------------------- */

struct ph_vs_output {
   nir_def *value;
   unsigned slot;       /* WARP output index */
   bool present;
};

static bool
ph_vs_collect(nir_shader *nir, struct ph_vs_output *position,
              struct ph_vs_output *varying, unsigned varyings)
{
   nir_foreach_function_impl(impl, nir) {
      if (exec_list_length(&impl->body) != 1)
         return false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            const nir_intrinsic_instr *store;
            unsigned location, slot;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            store = nir_instr_as_intrinsic(instr);
            if (store->intrinsic != nir_intrinsic_store_output)
               continue;
            if (nir_intrinsic_write_mask(store) != 0xf)
               return false;
            location = nir_intrinsic_io_semantics(store).location;
            if (location == VARYING_SLOT_POS) {
               if (position->present)
                  return false;
               position->value = store->src[0].ssa;
               position->present = true;
               continue;
            }
            if (location == VARYING_SLOT_COL0)
               slot = 1;
            else if (location == VARYING_SLOT_COL1)
               slot = 2;
            else if (location >= VARYING_SLOT_TEX0 &&
                     location <= VARYING_SLOT_TEX7)
               slot = 3 + (location - VARYING_SLOT_TEX0);
            else
               return false;    /* no output slot for this varying */
            for (unsigned i = 0; i < varyings; i++) {
               if (varying[i].present && varying[i].slot == slot)
                  return false;
               if (!varying[i].present) {
                  varying[i].value = store->src[0].ssa;
                  varying[i].slot = slot;
                  varying[i].present = true;
                  break;
               }
            }
         }
      }
   }
   return position->present;
}

/* Copy a value into an output slot: a plain source rides on the secondary
 * port, a computed one through an add with a zero operand. */
static void
ph_vs_write_output(struct ph_vs_ctx *ctx, unsigned slot,
                   const struct ph_vs_value *value)
{
   struct p3d_warp_inst in = {0};
   struct ph_vs_value zero = ph_vs_literal(0.0f);

   in.op = P3D_WARP_ADD;
   in.dst_file = P3D_WARP_FILE_DST_O;
   in.dst_index = slot;
   in.dst_mask = 0xf;
   in.nsrc = 2;
   in.src[0] = ph_vs_src(value);
   in.src[1] = ph_vs_src(&zero);
   ph_vs_emit(ctx, &in);
   ctx->program->output_mask |= 1u << slot;
}

bool
ph_vs_compile(nir_shader *nir, const struct ph_vs_inputs *inputs,
              unsigned constant_first, unsigned viewport_scale,
              unsigned viewport_offset, bool allow_t2,
              struct ph_vs_program *out)
{
   struct ph_vs_ctx ctx = {
      .program = out, .inputs = inputs, .constant_first = constant_first,
   };
   struct ph_vs_output position = {0};
   struct ph_vs_output varying[8] = {0};
   struct ph_vs_value clip, value;
   struct p3d_warp_inst in;

   memset(out, 0, sizeof(*out));
   if (!ph_vs_collect(nir, &position, varying, 8))
      return false;

   /* The clip-space position first: the viewport tail consumes it, and the
    * remaining outputs may then reuse its temporaries. */
   if (!ph_vs_vector(&ctx, position.value, &clip) || ctx.failed)
      return false;
   if (!clip.is_temp || clip.neg ||
       clip.swizzle != P3D_WARP_SWZ_IDENTITY)
      return false;

   for (unsigned i = 0; i < 8 && varying[i].present; i++) {
      if (!ph_vs_value(&ctx, varying[i].value, &value) || ctx.failed)
         return false;
      ph_vs_write_output(&ctx, varying[i].slot, &value);
      ph_vs_release(&ctx, &value);
      if (ctx.failed)
         return false;
   }

   /* The proven tail: load the viewport offset, divide by w, scale and bias,
    * and hand o0 a window-space position.  o11 keeps the clip copy every
    * kernel program writes. */
   {
      struct ph_vs_value offset = {
         .file = P3D_WARP_FILE_C, .index = (uint8_t)viewport_offset,
         .swizzle = P3D_WARP_SWZ_IDENTITY,
      };
      struct ph_vs_value scale = {
         .file = P3D_WARP_FILE_C, .index = (uint8_t)viewport_scale,
         .swizzle = P3D_WARP_SWZ_XYZ1,
      };
      struct ph_vs_value clip_xyz1 = clip, latch_t1 = {
         .file = P3D_WARP_FILE_T, .index = 1,
         .swizzle = P3D_WARP_SWZ_IDENTITY,
      };
      struct ph_vs_value latch_t1_xyz0 = latch_t1;
      struct ph_vs_value ndc;
      struct ph_vs_value zero = ph_vs_literal(0.0f);

      clip_xyz1.swizzle = P3D_WARP_SWZ_XYZ1;
      latch_t1_xyz0.swizzle = P3D_WARP_SWZ_XYZ0;

      memset(&in, 0, sizeof(in));
      in.op = P3D_WARP_WRA;
      in.sop = P3D_WARP_SOP_MOV;
      in.sdst_file = P3D_WARP_FILE_DST_T;
      in.sdst_index = 1;
      in.sdst_mask = 0xf;
      in.ssrc = ph_vs_src(&offset);
      ph_vs_emit(&ctx, &in);

      if (!ph_vs_alloc(&ctx, &ndc.index))
         return false;
      ndc.file = P3D_WARP_FILE_R;
      ndc.neg = 0;
      ndc.swizzle = P3D_WARP_SWZ_IDENTITY;
      ndc.is_temp = true;

      memset(&in, 0, sizeof(in));
      in.op = P3D_WARP_SMUL;
      in.dst_file = P3D_WARP_FILE_DST_R;
      in.dst_index = ndc.index;
      in.dst_mask = 0xf;
      in.nsrc = 2;
      in.src[0] = ph_vs_src(&clip);
      in.src[1] = ph_vs_src(&clip_xyz1);
      in.sop = P3D_WARP_SOP_MOV;
      in.sdst_file = P3D_WARP_FILE_DST_O;
      in.sdst_index = 11;
      in.sdst_mask = 0xf;
      in.ssrc = ph_vs_src(&clip);
      ph_vs_emit(&ctx, &in);
      out->output_mask |= 1u << 11;

      memset(&in, 0, sizeof(in));
      in.op = P3D_WARP_MAD;
      in.dst_file = P3D_WARP_FILE_DST_T;
      in.dst_index = 1;
      in.dst_mask = 7;
      in.nsrc = 3;
      in.src[0] = ph_vs_src(&scale);
      in.src[1] = ph_vs_src(&ndc);
      in.src[2] = ph_vs_src(&latch_t1_xyz0);
      ph_vs_emit(&ctx, &in);

      ph_vs_write_output(&ctx, 0, &latch_t1);
      (void)zero;
   }

   /* Prime the temporary every draw depends on, unless this draw samples a
    * texture - then writing it locks the engine. */
   if (allow_t2) {
      struct ph_vs_value zero = ph_vs_literal(0.0f);

      memset(&in, 0, sizeof(in));
      in.op = P3D_WARP_MUL;
      in.dst_file = P3D_WARP_FILE_DST_T;
      in.dst_index = 2;
      in.dst_mask = 0xf;
      in.nsrc = 2;
      in.src[0] = ph_vs_src(&zero);
      in.src[1] = ph_vs_src(&zero);
      ph_vs_emit(&ctx, &in);
      out->writes_t2 = true;
   }

   if (ctx.failed || out->count + 2 > PH_VS_MAX_INSTRUCTIONS)
      return false;
   {
      static const uint32_t end[3] = { 0x00000200u, 0x00040000u, 0x14c00000u };
      static const uint32_t tail[3] = { 0x00080200u, 0x00440000u, 0x301a21a2u };

      memcpy(out->code + out->count * 3, end, sizeof(end));
      out->count++;
      memcpy(out->code + out->count * 3, tail, sizeof(tail));
      out->count++;
   }
   out->constants.first = constant_first;
   out->constants.vec4s = ctx.constant_limit;
   out->valid = true;
   return true;
}
