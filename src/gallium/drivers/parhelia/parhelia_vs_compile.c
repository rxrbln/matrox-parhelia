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
#include <stdlib.h>
#include <string.h>

#include "util/list.h"
#include "util/macros.h"
#include "util/u_math.h"

#include "parhelia_vs_compile.h"

/* Why a shader was refused, when PARHELIA_TRACE_TCL is set.  A rejection is
 * silent otherwise: the draw simply keeps the software vertex pipeline. */
#define PH_VS_REJECT(reason) do { \
   if (getenv("PARHELIA_TRACE_TCL")) \
      fprintf(stderr, "parhelia: vs refused at %s:%d: %s\n", \
              __func__, __LINE__, (reason)); \
} while (0)


/* r0..r13 less r10/r11 are free; the vendor epilogue owns r14/r15.  T0 latches the fetched
 * position, T1 the viewport offset and T2 is the temporary every draw depends
 * on, so none of the three may be allocated. */
#define PH_VS_TEMPS 14u
/* r10 and r11 are where the light accumulator commits (P3D_WARP_ACC); a
 * value written there by an ordinary instruction does not read back. */
#define PH_VS_RESERVED_TEMPS ((1u << 10) | (1u << 11))

/* Highest constant slot an instruction can name: four index bits plus the one
 * three-bit extension field a bundle carries. */
#define PH_VS_MAX_CONSTANT 127u

struct ph_vs_value {
   uint8_t file;        /* enum p3d_warp_src_file */
   uint8_t index;
   uint8_t neg;
   uint16_t swizzle;
   bool is_temp;        /* index names an allocated temporary */
};

/* One compiled NIR def.  A value stays in its temporary until its last use has
 * been consumed, which is what lets a fourteen-register machine run a shader
 * with more intermediate results than that. */
struct ph_vs_slot {
   struct ph_vs_value value;
   unsigned refs;
   bool valid;
};

struct ph_vs_ctx {
   struct ph_vs_program *program;
   const struct ph_vs_inputs *inputs;
   unsigned constant_first;
   unsigned constant_vec4s;     /* constant-buffer vec4s, scanned up front */
   unsigned max_instructions;
   struct ph_vs_slot *cache;
   unsigned cache_size;
   uint16_t live;               /* allocated temporaries */
   /* Temporaries whose last use has been consumed.  Their registers stay
    * allocated until the statement is complete: an enclosing expression may
    * still be holding the value in a local, so handing the register to a
    * materialising move in between would clobber it. */
   uint8_t pending[PH_VS_TEMPS];
   unsigned pendings;
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
      if (!((ctx->live | PH_VS_RESERVED_TEMPS) & (1u << i))) {
         ctx->live |= 1u << i;
         *out = i;
         return true;
      }
   }
   ph_vs_fail(ctx);
   return false;
}

static void
ph_vs_free(struct ph_vs_ctx *ctx, const struct ph_vs_value *value)
{
   if (value->is_temp)
      ctx->live &= ~(1u << value->index);
}

static void
ph_vs_defer_free(struct ph_vs_ctx *ctx, const struct ph_vs_value *value)
{
   if (!value->is_temp)
      return;
   if (ctx->pendings >= ARRAY_SIZE(ctx->pending)) {
      ph_vs_fail(ctx);
      return;
   }
   ctx->pending[ctx->pendings++] = value->index;
}

/* Keep a value the caller still needs, even though its last NIR use has been
 * consumed - the compiled position is read again by the viewport tail. */
static void
ph_vs_retain(struct ph_vs_ctx *ctx, const struct ph_vs_value *value)
{
   for (unsigned i = 0; i < ctx->pendings; i++)
      if (ctx->pending[i] == value->index) {
         ctx->pending[i] = ctx->pending[--ctx->pendings];
         return;
      }
}

/* Statement boundary: nothing holds the dead values any more. */
static void
ph_vs_flush_free(struct ph_vs_ctx *ctx)
{
   while (ctx->pendings)
      ctx->live &= ~(1u << ctx->pending[--ctx->pendings]);
}

static void
ph_vs_emit(struct ph_vs_ctx *ctx, const struct p3d_warp_inst *in)
{
   struct ph_vs_program *program = ctx->program;

   /* Two instructions of headroom for END and its padding word. */
   if (program->count + 2 >= ctx->max_instructions) {
      ph_vs_fail(ctx);
      return;
   }
   p3d_warp_encode(in, program->code + program->count * 3);
   program->count++;
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

/* ---- swizzles ---------------------------------------------------------- */

static unsigned
ph_vs_swz_comp(uint16_t swizzle, unsigned component)
{
   return (swizzle >> (3u * component)) & 7u;
}

static bool
ph_vs_swz_uniform(uint16_t swizzle)
{
   for (unsigned i = 1; i < 4; i++)
      if (ph_vs_swz_comp(swizzle, i) != ph_vs_swz_comp(swizzle, 0))
         return false;
   return true;
}

/* Read @value through a component selection, composing with the swizzle it
 * already carries.  @select names, per destination component, which logical
 * component of the value to read. */
static void
ph_vs_swizzle(struct ph_vs_value *value, const unsigned select[4])
{
   unsigned code[4];

   for (unsigned i = 0; i < 4; i++)
      code[i] = ph_vs_swz_comp(value->swizzle, select[i] & 3u);
   value->swizzle = P3D_WARP_SWZ(code[0], code[1], code[2], code[3]);
}

/* ---- leaves ---------------------------------------------------------- */

/* A source with the 0.0/1.0 swizzle codes is the engine's way of materialising
 * a literal without spending a constant slot; the register read is irrelevant. */
static struct ph_vs_value
ph_vs_swizzled_literal(const float value[4])
{
   struct ph_vs_value out = { .file = P3D_WARP_FILE_R, .index = 1 };
   unsigned code[4];

   for (unsigned i = 0; i < 4; i++)
      code[i] = value[i] != 0.0f ? 5u : 4u;
   out.swizzle = P3D_WARP_SWZ(code[0], code[1], code[2], code[3]);
   return out;
}

/* Anything the swizzle cannot spell needs a constant slot, loaded behind the
 * shader's own constant buffer. */
static bool
ph_vs_literal(struct ph_vs_ctx *ctx, const float value[4],
              struct ph_vs_value *out)
{
   struct ph_vs_program *program = ctx->program;
   unsigned i, slot;

   for (i = 0; i < program->literals; i++)
      if (!memcmp(program->literal[i], value, sizeof program->literal[i]))
         break;
   slot = ctx->constant_first + ctx->constant_vec4s + i;
   if (i >= PH_VS_MAX_LITERALS || slot > PH_VS_MAX_CONSTANT) {
      ph_vs_fail(ctx);
      return false;
   }
   if (i == program->literals) {
      memcpy(program->literal[i], value, sizeof program->literal[i]);
      program->literals++;
   }
   memset(out, 0, sizeof(*out));
   out->file = P3D_WARP_FILE_C;
   out->index = (uint8_t)slot;
   out->swizzle = P3D_WARP_SWZ_IDENTITY;
   return true;
}

static bool
ph_vs_leaf_const(struct ph_vs_ctx *ctx, const nir_load_const_instr *load,
                 struct ph_vs_value *out)
{
   float value[4];
   bool swizzled = true;

   if (load->def.bit_size != 32 || !load->def.num_components)
      return false;
   for (unsigned i = 0; i < 4; i++) {
      unsigned c = i < load->def.num_components ? i :
                   load->def.num_components - 1u;

      value[i] = nir_const_value_as_float(load->value[c], 32);
      if (value[i] != 0.0f && value[i] != 1.0f)
         swizzled = false;
   }
   if (swizzled) {
      *out = ph_vs_swizzled_literal(value);
      return true;
   }
   return ph_vs_literal(ctx, value, out);
}

/* The constant-buffer vec4 and first component a uniform load addresses. */
static bool
ph_vs_uniform_slot(const nir_intrinsic_instr *load, unsigned *vec4,
                   unsigned *component)
{
   unsigned offset;

   switch (load->intrinsic) {
   case nir_intrinsic_load_ubo:
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
          !nir_src_is_const(load->src[1]))
         return false;
      offset = nir_src_as_uint(load->src[1]);
      break;
   case nir_intrinsic_load_uniform:
      if (!nir_src_is_const(load->src[0]))
         return false;
      offset = nir_intrinsic_base(load) + nir_src_as_uint(load->src[0]) * 16u;
      break;
   case nir_intrinsic_load_ubo_vec4:
      /* The vec4 index is the sum of the base and the offset source: GLSL
       * puts it in the source with base zero, so reading only the base
       * refuses every real shader. */
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]) ||
          !nir_src_is_const(load->src[1]))
         return false;
      offset = (nir_intrinsic_base(load) + nir_src_as_uint(load->src[1])) *
                  16u + nir_intrinsic_component(load) * 4u;
      break;
   default:
      return false;
   }
   if (offset % 4u || offset / 16u > PH_VS_MAX_CONSTANT)
      return false;
   *vec4 = offset / 16u;
   *component = (offset % 16u) / 4u;
   return *component + load->def.num_components <= 4u;
}

static bool
ph_vs_leaf_intrinsic(struct ph_vs_ctx *ctx, const nir_intrinsic_instr *load,
                     struct ph_vs_value *out)
{
   unsigned vec4, component, select[4];
   unsigned location;

   memset(out, 0, sizeof(*out));
   out->swizzle = P3D_WARP_SWZ_IDENTITY;

   switch (load->intrinsic) {
   case nir_intrinsic_load_deref: {
      const nir_deref_instr *deref = nir_src_as_deref(load->src[0]);

      if (!deref || deref->deref_type != nir_deref_type_var ||
          deref->var->data.mode != nir_var_shader_in)
         return false;
      location = deref->var->data.driver_location;
      break;
   }
   case nir_intrinsic_load_input:
      if (!nir_src_is_const(load->src[0]) || nir_src_as_uint(load->src[0]))
         return false;
      location = nir_intrinsic_base(load);
      break;
   default:
      if (!ph_vs_uniform_slot(load, &vec4, &component))
         return false;
      if (vec4 >= ctx->constant_vec4s)
         return false;
      out->file = P3D_WARP_FILE_C;
      out->index = (uint8_t)(ctx->constant_first + vec4);
      for (unsigned i = 0; i < 4; i++)
         select[i] = component + i < 4u ? component + i : 3u;
      ph_vs_swizzle(out, select);
      return true;
   }
   if (location >= ctx->inputs->count || ctx->inputs->slot[location] < 0)
      return false;
   out->file = P3D_WARP_FILE_V;
   out->index = (uint8_t)ctx->inputs->slot[location];
   ctx->program->input_mask |= 1u << out->index;
   return true;
}

/* ---- instruction issue ------------------------------------------------- */

/* A one-source operation on the secondary port, which reads src2 and writes
 * port A; the primary half stays the arithmetic no-op. */
static bool
ph_vs_secondary(struct ph_vs_ctx *ctx, uint8_t sop, struct ph_vs_value *value)
{
   struct p3d_warp_inst in = {0};
   uint8_t dst;

   if (!ph_vs_alloc(ctx, &dst))
      return false;
   in.op = P3D_WARP_WRA;
   in.sop = sop;
   in.sdst_file = P3D_WARP_FILE_DST_R;
   in.sdst_index = dst;
   in.sdst_mask = 0xf;
   in.ssrc = ph_vs_src(value);
   ph_vs_emit(ctx, &in);
   if (ctx->failed)
      return false;
   memset(value, 0, sizeof(*value));
   value->file = P3D_WARP_FILE_R;
   value->index = dst;
   value->swizzle = P3D_WARP_SWZ_IDENTITY;
   value->is_temp = true;
   return true;
}

/* "mov dst, src", so a source the bundle cannot encode in place becomes a
 * plain temporary read. */
static bool
ph_vs_materialise(struct ph_vs_ctx *ctx, struct ph_vs_value *value)
{
   return ph_vs_secondary(ctx, P3D_WARP_SOP_MOV, value);
}

/* src1 and src2 share one swizzle field and a bundle carries a single
 * index-extension field, so a second offender goes through a temporary.
 * @spare collects what that created; the caller frees it after the issue. */
static bool
ph_vs_legalise(struct ph_vs_ctx *ctx, struct ph_vs_value *src, unsigned count,
               struct ph_vs_value *spare, unsigned *spares)
{
   unsigned shared = 0, extended = 0;

   for (unsigned i = 0; i < count; i++) {
      if (i >= 1 && src[i].swizzle != P3D_WARP_SWZ_IDENTITY)
         shared++;
      if (src[i].index > 15)
         extended++;
   }
   for (unsigned i = count; i-- > 0 && (shared > 1 || extended > 1);) {
      bool swizzled = i >= 1 && src[i].swizzle != P3D_WARP_SWZ_IDENTITY;
      bool wide = src[i].index > 15;

      if (!((swizzled && shared > 1) || (wide && extended > 1)))
         continue;
      if (!ph_vs_materialise(ctx, &src[i]))
         return false;
      shared -= swizzled;
      extended -= wide;
      spare[(*spares)++] = src[i];
   }
   return shared <= 1 && extended <= 1;
}

/* Issue one primary operation.  With @out the destination is a freshly
 * allocated temporary, otherwise it is the named register under @mask. */
static bool
ph_vs_issue(struct ph_vs_ctx *ctx, uint8_t op, struct ph_vs_value *src,
            unsigned count, uint8_t dst_file, uint8_t dst_index, uint8_t mask,
            struct ph_vs_value *out)
{
   struct ph_vs_value spare[3];
   struct p3d_warp_inst in = {0};
   unsigned spares = 0;

   if (count > 3 || !ph_vs_legalise(ctx, src, count, spare, &spares))
      return false;
   if (out) {
      if (!ph_vs_alloc(ctx, &dst_index))
         return false;
      dst_file = P3D_WARP_FILE_DST_R;
      mask = 0xf;
   }
   in.op = op;
   in.nsrc = count;
   in.dst_file = dst_file;
   in.dst_index = dst_index;
   in.dst_mask = mask;
   for (unsigned i = 0; i < count; i++)
      in.src[i] = ph_vs_src(&src[i]);
   ph_vs_emit(ctx, &in);
   while (spares)
      ph_vs_free(ctx, &spare[--spares]);
   if (ctx->failed)
      return false;
   if (out) {
      memset(out, 0, sizeof(*out));
      out->file = P3D_WARP_FILE_R;
      out->index = dst_index;
      out->swizzle = P3D_WARP_SWZ_IDENTITY;
      out->is_temp = true;
   }
   return true;
}

/* ---- expressions ------------------------------------------------------ */

static bool ph_vs_value(struct ph_vs_ctx *ctx, nir_def *def,
                        struct ph_vs_value *out);

/* Read one ALU operand.  A scalar result must hold the same value in all four
 * lanes, because a later consumer may read any of them, so a scalar operand is
 * always broadcast rather than read naturally. */
static bool
ph_vs_operand(struct ph_vs_ctx *ctx, const nir_alu_instr *alu, unsigned index,
              struct ph_vs_value *out)
{
   const nir_alu_src *src = &alu->src[index];
   unsigned components = nir_ssa_alu_instr_src_components(alu, index);
   unsigned select[4];

   if (!components || components > 4)
      return false;
   if (!ph_vs_value(ctx, src->src.ssa, out))
      return false;
   for (unsigned i = 0; i < 4; i++)
      select[i] = src->swizzle[i < components ? i : components - 1u];
   ph_vs_swizzle(out, select);
   return true;
}

static bool
ph_vs_identity_src(const nir_alu_instr *alu, unsigned index)
{
   unsigned components = nir_ssa_alu_instr_src_components(alu, index);

   for (unsigned i = 0; i < components; i++)
      if (alu->src[index].swizzle[i] != i)
         return false;
   return true;
}

/* Which operand of an add is a multiply the engine's mad can absorb.  GLSL's
 * matrix product is a chain of these, and the vertex NIR options do not fuse
 * them, so folding here is what keeps a transform at four bundles. */
static unsigned
ph_vs_fold_multiply(const nir_alu_instr *alu)
{
   for (unsigned i = 0; i < 2; i++) {
      nir_instr *instr = nir_def_instr(alu->src[i].src.ssa);
      const nir_alu_instr *mul;

      if (instr->type != nir_instr_type_alu)
         continue;
      mul = nir_instr_as_alu(instr);
      if (mul->op == nir_op_fmul && list_is_singular(&mul->def.uses) &&
          mul->def.num_components == alu->def.num_components &&
          ph_vs_identity_src(alu, i))
         return i;
   }
   return 2;
}

/* 1/x and 1/sqrt(x) through the scalar divide opcodes: dst.c = s1.c * f(s0.w). */
static bool
ph_vs_reciprocal(struct ph_vs_ctx *ctx, uint8_t op,
                 const struct ph_vs_value *value, struct ph_vs_value *out)
{
   static const float ones[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
   struct ph_vs_value src[2];

   if (!ph_vs_swz_uniform(value->swizzle))
      return false;              /* the scalar has to reach .w */
   src[0] = *value;
   src[1] = ph_vs_swizzled_literal(ones);
   return ph_vs_issue(ctx, op, src, 2, 0, 0, 0, out);
}

static bool
ph_vs_alu(struct ph_vs_ctx *ctx, const nir_alu_instr *alu,
          struct ph_vs_value *out)
{
   struct ph_vs_value src[3];
   uint8_t op;
   unsigned count;

   if (alu->def.bit_size != 32)
      return false;

   switch (alu->op) {
   case nir_op_mov:
      return ph_vs_operand(ctx, alu, 0, out);
   case nir_op_fneg:
      if (!ph_vs_operand(ctx, alu, 0, out))
         return false;
      out->neg = !out->neg;
      return true;
   case nir_op_fabs:
      /* max(x, -x): one bundle, and the engine has no absolute modifier. */
      if (!ph_vs_operand(ctx, alu, 0, &src[0]))
         return false;
      src[1] = src[0];
      src[1].neg = !src[1].neg;
      return ph_vs_issue(ctx, P3D_WARP_MAX, src, 2, 0, 0, 0, out);
   case nir_op_fsqrt: {
      /* rcp(rsq(x)) rather than x * rsq(x), so that sqrt(0) is 0. */
      struct ph_vs_value inv;

      if (!ph_vs_operand(ctx, alu, 0, &src[0]) ||
          !ph_vs_reciprocal(ctx, P3D_WARP_RSQ, &src[0], &inv))
         return false;
      /* Every component of the reciprocal root holds the same scalar. */
      ph_vs_swizzle(&inv, (const unsigned[4]){ 3, 3, 3, 3 });
      if (!ph_vs_reciprocal(ctx, P3D_WARP_SMUL, &inv, out))
         return false;
      ph_vs_defer_free(ctx, &inv);
      return true;
   }
   case nir_op_frcp:
   case nir_op_frsq:
      if (!ph_vs_operand(ctx, alu, 0, &src[0]))
         return false;
      return ph_vs_reciprocal(ctx, alu->op == nir_op_frcp ? P3D_WARP_SMUL :
                                                            P3D_WARP_RSQ,
                              &src[0], out);
   case nir_op_fexp2:
   case nir_op_flog2:
      /* 0x91/0x92 read the scalar from .w and, MEASURED on the P690, write
       * the result to every component: exp2/log2 over [-1.7, 1.3] and
       * [0.25, 3.3] matched the draw module to 8 bits in each channel. */
      if (!ph_vs_operand(ctx, alu, 0, &src[0]) ||
          !ph_vs_swz_uniform(src[0].swizzle))
         return false;
      return ph_vs_issue(ctx, alu->op == nir_op_fexp2 ? P3D_WARP_EXP :
                                                        P3D_WARP_LOG,
                         src, 1, 0, 0, 0, out);
   case nir_op_ffloor:
   case nir_op_ffract:
      if (!ph_vs_operand(ctx, alu, 0, &src[0]))
         return false;
      if (!ph_vs_secondary(ctx, alu->op == nir_op_ffloor ?
                                   P3D_WARP_SOP_FLOOR : P3D_WARP_SOP_FRC,
                           &src[0]))
         return false;
      *out = src[0];
      return true;
   case nir_op_fdiv:
      /* The divisor and its reciprocal first: the dividend must not be held
       * across an instruction that could reuse its register. */
      if (!ph_vs_operand(ctx, alu, 1, &src[1]) ||
          !ph_vs_reciprocal(ctx, P3D_WARP_SMUL, &src[1], &src[1]) ||
          !ph_vs_operand(ctx, alu, 0, &src[0]))
         return false;
      if (!ph_vs_issue(ctx, P3D_WARP_MUL, src, 2, 0, 0, 0, out))
         return false;
      ph_vs_defer_free(ctx, &src[1]);
      return true;
   /* GL's fixed-function lighting gates the specular term on "is the light
    * in front of the surface", which the state tracker spells as
    * b2f32(flt(0.0, N.L)).  The WARP unit has no comparison, no saturating
    * modifier and no component-wise minimum (0x61 is hardware-refuted), so
    * the step is built from the three operations it does have:
    *
    *    d = b - a              the comparison's difference, positive when true
    *    s = max(d * 2^60, 0)   the negative half becomes exactly zero
    *    step = s / (s + 1)     the positive half is pulled down to one
    *
    * That is exactly 0.0 for d <= 0, and within a part in a million of 1.0
    * for any d above 1e-12 - far below the smallest N.L a light contributes.
    * The scale stays at 2^60 so d = 1 cannot overflow the addition. */
   case nir_op_b2f32: {
      static const float scale[4] = {
         1152921504606846976.0f, 1152921504606846976.0f,
         1152921504606846976.0f, 1152921504606846976.0f,
      };
      static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      static const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
      static const unsigned broadcast_x[4] = { 0, 0, 0, 0 };
      const nir_instr *cmp_instr = nir_def_instr(alu->src[0].src.ssa);
      const nir_alu_instr *cmp;
      struct ph_vs_value d, run, sum;

      if (cmp_instr->type != nir_instr_type_alu)
         return false;
      cmp = nir_instr_as_alu(cmp_instr);
      /* Only the strict comparison: a step built this way answers 0 at
       * d == 0, which is what flt means and is not what fge means. */
      if (cmp->op != nir_op_flt || cmp->def.num_components != 1)
         return false;
      if (!ph_vs_operand(ctx, cmp, 1, &src[0]) ||
          !ph_vs_operand(ctx, cmp, 0, &src[1]))
         return false;
      src[1].neg = !src[1].neg;
      if (!ph_vs_issue(ctx, P3D_WARP_ADD, src, 2, 0, 0, 0, &d))
         return false;
      src[0] = d;
      if (!ph_vs_literal(ctx, scale, &src[1]) ||
          !ph_vs_issue(ctx, P3D_WARP_MUL, src, 2, 0, 0, 0, &run))
         return false;
      ph_vs_defer_free(ctx, &d);
      src[0] = run;
      src[1] = ph_vs_swizzled_literal(zero);
      if (!ph_vs_issue(ctx, P3D_WARP_MAX, src, 2, 0, 0, 0, &run))
         return false;
      src[0] = run;
      src[1] = ph_vs_swizzled_literal(one);
      if (!ph_vs_issue(ctx, P3D_WARP_ADD, src, 2, 0, 0, 0, &sum))
         return false;
      /* SMUL divides by source 0's .w, and every value in this chain carries
       * the same scalar in each component, so read x into w. */
      ph_vs_swizzle(&sum, broadcast_x);
      src[0] = sum;
      src[1] = run;
      if (!ph_vs_issue(ctx, P3D_WARP_SMUL, src, 2, 0, 0, 0, out))
         return false;
      ph_vs_defer_free(ctx, &sum);
      ph_vs_defer_free(ctx, &run);
      return true;
   }
   case nir_op_fmul: op = P3D_WARP_MUL; count = 2; break;
   case nir_op_fadd: {
      unsigned fold = ph_vs_fold_multiply(alu);

      if (fold < 2) {
         const nir_alu_instr *mul =
            nir_instr_as_alu(nir_def_instr(alu->src[fold].src.ssa));

         if (!ph_vs_operand(ctx, mul, 0, &src[0]) ||
             !ph_vs_operand(ctx, mul, 1, &src[1]) ||
             !ph_vs_operand(ctx, alu, fold ^ 1u, &src[2]))
            return false;
         return ph_vs_issue(ctx, P3D_WARP_MAD, src, 3, 0, 0, 0, out);
      }
      op = P3D_WARP_ADD;
      count = 2;
      break;
   }
   /* Hardware refutes 0x61 as a componentwise minimum (scene 2 of
    * parhelia-gl-vs-test differed on every pixel while scene 3 proves 0x62
    * is a real maximum), so min(a, b) is -max(-a, -b). */
   case nir_op_fmin:
      if (!ph_vs_operand(ctx, alu, 0, &src[0]) ||
          !ph_vs_operand(ctx, alu, 1, &src[1]))
         return false;
      src[0].neg = !src[0].neg;
      src[1].neg = !src[1].neg;
      if (!ph_vs_issue(ctx, P3D_WARP_MAX, src, 2, 0, 0, 0, out))
         return false;
      out->neg = !out->neg;
      return true;
   case nir_op_fmax: op = P3D_WARP_MAX; count = 2; break;
   case nir_op_ffma: op = P3D_WARP_MAD; count = 3; break;
   case nir_op_fdot2:
   case nir_op_fdot3: op = P3D_WARP_DP3; count = 2; break;
   case nir_op_fdot4: op = P3D_WARP_DP4; count = 2; break;
   default:
      return false;
   }
   for (unsigned i = 0; i < count; i++)
      if (!ph_vs_operand(ctx, alu, i, &src[i]))
         return false;
   if (alu->op == nir_op_fdot2) {
      /* dp3 with both third components forced to the literal zero. */
      for (unsigned i = 0; i < 2; i++)
         src[i].swizzle = P3D_WARP_SWZ(ph_vs_swz_comp(src[i].swizzle, 0),
                                       ph_vs_swz_comp(src[i].swizzle, 1), 4, 4);
   }
   return ph_vs_issue(ctx, op, src, count, 0, 0, 0, out);
}

/* A constructed vector's component compiles into one masked instruction when
 * it is a single operation over leaves, which is what turns a vec4 of four dot
 * products back into the ordinary matrix product the reference program emits. */
static bool
ph_vs_component_op(const nir_def *def, uint8_t *op, unsigned *count)
{
   const nir_alu_instr *term;

   if (nir_def_instr(def)->type != nir_instr_type_alu ||
       !list_is_singular(&def->uses))
      return false;
   term = nir_instr_as_alu(nir_def_instr(def));
   switch (term->op) {
   case nir_op_fdot4: *op = P3D_WARP_DP4; *count = 2; break;
   case nir_op_fdot3: *op = P3D_WARP_DP3; *count = 2; break;
   case nir_op_fmul:  *op = P3D_WARP_MUL; *count = 2; break;
   case nir_op_fadd:  *op = P3D_WARP_ADD; *count = 2; break;
   case nir_op_fmax:  *op = P3D_WARP_MAX; *count = 2; break;
   default:
      return false;
   }
   if (term->def.bit_size != 32 || term->def.num_components != 1)
      return false;
   for (unsigned i = 0; i < *count; i++)
      if (nir_def_instr(term->src[i].src.ssa)->type == nir_instr_type_alu)
         return false;           /* not a leaf: take the general path */
   return true;
}

/* vec2/vec3/vec4: one temporary, one masked write per component. */
static bool
ph_vs_vector(struct ph_vs_ctx *ctx, const nir_alu_instr *alu,
             struct ph_vs_value *out)
{
   unsigned count = nir_op_infos[alu->op].num_inputs;
   uint8_t dst;

   if (count > 4 || !ph_vs_alloc(ctx, &dst))
      return false;
   for (unsigned i = 0; i < count; i++) {
      static const float zeros[4] = {0};
      struct ph_vs_value src[3];
      unsigned terms;
      uint8_t op;

      if (ph_vs_component_op(alu->src[i].src.ssa, &op, &terms)) {
         const nir_alu_instr *term =
            nir_instr_as_alu(nir_def_instr(alu->src[i].src.ssa));

         for (unsigned k = 0; k < terms; k++)
            if (!ph_vs_operand(ctx, term, k, &src[k]))
               return false;
         if (!ph_vs_issue(ctx, op, src, terms, P3D_WARP_FILE_DST_R, dst,
                          1u << i, NULL))
            return false;
         continue;
      }
      if (!ph_vs_operand(ctx, alu, i, &src[0]))
         return false;
      src[1] = ph_vs_swizzled_literal(zeros);
      if (!ph_vs_issue(ctx, P3D_WARP_ADD, src, 2, P3D_WARP_FILE_DST_R, dst,
                       1u << i, NULL))
         return false;
   }
   memset(out, 0, sizeof(*out));
   out->file = P3D_WARP_FILE_R;
   out->index = dst;
   out->swizzle = P3D_WARP_SWZ_IDENTITY;
   out->is_temp = true;
   return true;
}

static bool
ph_vs_value(struct ph_vs_ctx *ctx, nir_def *def, struct ph_vs_value *out)
{
   struct ph_vs_slot *slot;
   bool ok;

   if (ctx->failed || def->index >= ctx->cache_size)
      return false;
   slot = &ctx->cache[def->index];
   if (!slot->valid) {
      nir_instr *instr = nir_def_instr(def);

      switch (instr->type) {
      case nir_instr_type_load_const:
         ok = ph_vs_leaf_const(ctx, nir_instr_as_load_const(instr), out);
         break;
      case nir_instr_type_undef: {
         /* An undefined value may be anything; zero is as good as any. */
         static const float zero[4] = { 0 };

         *out = ph_vs_swizzled_literal(zero);
         ok = true;
         break;
      }
      case nir_instr_type_intrinsic:
         ok = ph_vs_leaf_intrinsic(ctx, nir_instr_as_intrinsic(instr), out);
         break;
      case nir_instr_type_alu: {
         const nir_alu_instr *alu = nir_instr_as_alu(instr);

         ok = alu->op == nir_op_vec2 || alu->op == nir_op_vec3 ||
              alu->op == nir_op_vec4 ? ph_vs_vector(ctx, alu, out) :
                                       ph_vs_alu(ctx, alu, out);
         break;
      }
      default:
         return false;
      }
      if (!ok || ctx->failed)
         return false;
      slot->value = *out;
      slot->refs = list_length(&def->uses);
      slot->valid = true;
   }
   *out = slot->value;
   if (slot->refs && !--slot->refs) {
      ph_vs_defer_free(ctx, &slot->value);
      slot->valid = false;
   }
   return !ctx->failed;
}

/* ---- program assembly ------------------------------------------------- */

/* An output slot, and the stores that fill it.  A shader may write one slot
 * in several partial stores - the fixed-function lighting shader writes the
 * colour's alpha from the scene colour and its rgb from the light sum - so
 * every store is kept with the components it covers, and the components must
 * together cover the slot exactly once. */
struct ph_vs_output {
   nir_def *value;      /* the single full-mask store, when there is one */
   struct {
      nir_def *value;
      unsigned mask;
   } part[4];
   unsigned parts;
   unsigned covered;    /* components written so far */
   unsigned slot;       /* WARP output index */
   bool present;
};

/* An output deref: a variable, or a constant element of a varying array such
 * as GLSL's gl_TexCoord[]. */
static bool
ph_vs_deref_location(const nir_deref_instr *deref, unsigned *location)
{
   unsigned element = 0;

   if (!deref)
      return false;
   if (deref->deref_type == nir_deref_type_array) {
      if (!nir_src_is_const(deref->arr.index))
         return false;
      element = nir_src_as_uint(deref->arr.index);
      deref = nir_deref_instr_parent(deref);
      if (!deref)
         return false;
   }
   if (deref->deref_type != nir_deref_type_var ||
       deref->var->data.mode != nir_var_shader_out)
      return false;
   *location = deref->var->data.location + element;
   return true;
}

static bool
ph_vs_output_slot(unsigned location, unsigned *slot)
{
   if (location == VARYING_SLOT_COL0)
      *slot = 1;
   else if (location == VARYING_SLOT_COL1 || location == VARYING_SLOT_FOGC)
      /* The fixed-function fog coordinate rides in colour 1: the combiner's
       * only interpolated inputs are the two colours, and the fragment stage
       * reads this one back as v1.  A shader that wants both a specular
       * colour and fog would collide, and ph_vs_collect refuses that because
       * the slot is already taken. */
      *slot = 2;
   else if (location >= VARYING_SLOT_TEX0 && location <= VARYING_SLOT_TEX7)
      *slot = 3 + (location - VARYING_SLOT_TEX0);
   else
      return false;
   return true;
}

/* Walk the one basic block, refusing anything that is not an expression this
 * compiler can read, and record the value feeding every output. */
static bool
ph_vs_collect(nir_shader *nir, struct ph_vs_output *position,
              struct ph_vs_output *varying, unsigned varyings)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   if (!impl || exec_list_length(&impl->body) != 1)
      { PH_VS_REJECT("collect"); return false; }
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         const nir_intrinsic_instr *store;
         struct ph_vs_output *out = NULL;
         unsigned location, slot;
         nir_def *value;

         switch (instr->type) {
         case nir_instr_type_load_const:
         case nir_instr_type_undef:
         case nir_instr_type_alu:
         case nir_instr_type_jump:
            continue;
         case nir_instr_type_deref: {
            nir_deref_type type = nir_instr_as_deref(instr)->deref_type;

            if (type != nir_deref_type_var && type != nir_deref_type_array)
               { PH_VS_REJECT("collect"); return false; }
            continue;
         }
         case nir_instr_type_intrinsic:
            break;
         default:
            { PH_VS_REJECT("collect"); return false; }
         }
         store = nir_instr_as_intrinsic(instr);
         switch (store->intrinsic) {
         case nir_intrinsic_load_deref:
         case nir_intrinsic_load_input:
         case nir_intrinsic_load_ubo:
         case nir_intrinsic_load_ubo_vec4:
         case nir_intrinsic_load_uniform:
            continue;
         case nir_intrinsic_store_deref:
            if (!ph_vs_deref_location(nir_src_as_deref(store->src[0]),
                                      &location))
               { PH_VS_REJECT("collect"); return false; }
            value = store->src[1].ssa;
            break;
         case nir_intrinsic_store_output:
            location = nir_intrinsic_io_semantics(store).location;
            value = store->src[0].ssa;
            break;
         default:
            { PH_VS_REJECT("collect"); return false; }
         }
         if (value->num_components != 4)
            { PH_VS_REJECT("collect"); return false; }
         if (location == VARYING_SLOT_POS) {
            out = position;
            slot = 0;
         } else {
            if (!ph_vs_output_slot(location, &slot))
               { PH_VS_REJECT("collect"); return false; }
            for (unsigned i = 0; i < varyings; i++) {
               if (varying[i].present && varying[i].slot == slot) {
                  out = &varying[i];   /* another part of the same slot */
                  break;
               }
               if (!varying[i].present) {
                  out = &varying[i];
                  break;
               }
            }
            if (!out)
               { PH_VS_REJECT("collect"); return false; }
         }
         {
            const unsigned mask = nir_intrinsic_write_mask(store) & 0xf;

            /* A component written twice would need the stores ordered, which
             * this compiler does not model. */
            if (!mask || (out->covered & mask) ||
                out->parts >= ARRAY_SIZE(out->part))
               { PH_VS_REJECT("collect"); return false; }
            out->part[out->parts].value = value;
            out->part[out->parts].mask = mask;
            out->parts++;
            out->covered |= mask;
            out->value = mask == 0xf ? value : NULL;
            out->slot = slot;
            out->present = true;
         }
      }
   }
   /* The position has to be completely written.  A varying component the
    * shader leaves out (the linker drops one the fragment shader does not
    * read) is undefined and gets zero at emission, so it never reads what
    * the previous vertex left there. */
   if (position->present && position->covered != 0xf)
      { PH_VS_REJECT("collect"); return false; }
   return position->present && position->value;
}

/* How many constant-buffer vec4s the shader reads, so the literals behind them
 * get stable slots.  Refuses an indirect or misaligned uniform address. */
static bool
ph_vs_scan_constants(nir_shader *nir, unsigned *vec4s)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   *vec4s = 0;
   if (!impl)
      return false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         const nir_intrinsic_instr *load;
         unsigned vec4, component;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         load = nir_instr_as_intrinsic(instr);
         switch (load->intrinsic) {
         case nir_intrinsic_load_ubo:
         case nir_intrinsic_load_ubo_vec4:
         case nir_intrinsic_load_uniform:
            if (!ph_vs_uniform_slot(load, &vec4, &component))
               return false;
            if (vec4 + 1 > *vec4s)
               *vec4s = vec4 + 1;
            break;
         default:
            break;
         }
      }
   }
   return true;
}

/* Write an output slot from the instruction that computes it, rather than
 * computing into a temporary and copying: one bundle less per varying, and
 * the varying expressions are exactly where per-vertex cost accumulates.
 * Returns false when the expression is not one operation wide, in which case
 * nothing has been emitted and the caller takes the general path. */
static bool
ph_vs_output_direct(struct ph_vs_ctx *ctx, nir_def *def, unsigned slot)
{
   const nir_alu_instr *alu;
   struct ph_vs_value src[3];
   unsigned count, fold = 2;
   uint8_t op;

   if (nir_def_instr(def)->type != nir_instr_type_alu ||
       !list_is_singular(&def->uses) || def->num_components != 4)
      return false;
   alu = nir_instr_as_alu(nir_def_instr(def));
   if (alu->def.bit_size != 32)
      return false;
   switch (alu->op) {
   case nir_op_fmul: op = P3D_WARP_MUL; count = 2; break;
   /* fmin is deliberately absent: hardware refutes 0x61 as a componentwise
    * minimum (scene 2 of parhelia-gl-vs-test differed on every pixel of the
    * quad while every other scene was bit-identical, and scene 3 proves 0x62
    * is a real maximum).  clamp() and min() therefore keep the draw module. */
   case nir_op_fmax: op = P3D_WARP_MAX; count = 2; break;
   case nir_op_ffma: op = P3D_WARP_MAD; count = 3; break;
   case nir_op_fadd:
      fold = ph_vs_fold_multiply(alu);
      op = fold < 2 ? P3D_WARP_MAD : P3D_WARP_ADD;
      count = fold < 2 ? 3 : 2;
      break;
   default:
      return false;
   }
   if (fold < 2) {
      const nir_alu_instr *mul =
         nir_instr_as_alu(nir_def_instr(alu->src[fold].src.ssa));

      if (!ph_vs_operand(ctx, mul, 0, &src[0]) ||
          !ph_vs_operand(ctx, mul, 1, &src[1]) ||
          !ph_vs_operand(ctx, alu, fold ^ 1u, &src[2])) {
         ph_vs_fail(ctx);
         return false;
      }
   } else {
      for (unsigned i = 0; i < count; i++)
         if (!ph_vs_operand(ctx, alu, i, &src[i])) {
            ph_vs_fail(ctx);
            return false;
         }
   }
   if (!ph_vs_issue(ctx, op, src, count, P3D_WARP_FILE_DST_O, (uint8_t)slot,
                    0xf, NULL))
      return false;
   ctx->program->output_mask |= 1u << slot;
   return true;
}

/* Copy a value into some components of an output slot. */
static bool
ph_vs_write_output_masked(struct ph_vs_ctx *ctx, unsigned slot,
                          const struct ph_vs_value *value, unsigned mask)
{
   static const float zeros[4] = {0};
   struct ph_vs_value src[2];

   src[0] = *value;
   src[1] = ph_vs_swizzled_literal(zeros);
   return ph_vs_issue(ctx, P3D_WARP_ADD, src, 2, P3D_WARP_FILE_DST_O,
                      (uint8_t)slot, (uint8_t)mask, NULL);
}

/* Copy a value into an output slot through an add with a zero operand. */
static bool
ph_vs_write_output(struct ph_vs_ctx *ctx, unsigned slot,
                   const struct ph_vs_value *value)
{
   static const float zeros[4] = {0};
   struct ph_vs_value src[2];

   src[0] = *value;
   src[1] = ph_vs_swizzled_literal(zeros);
   if (!ph_vs_issue(ctx, P3D_WARP_ADD, src, 2, P3D_WARP_FILE_DST_O,
                    (uint8_t)slot, 0xf, NULL))
      return false;
   ctx->program->output_mask |= 1u << slot;
   return true;
}

/* The proven tail: latch the viewport offset, divide by w, scale and bias, and
 * hand o0 a window-space position.  o11 keeps the clip copy every running
 * program writes. */
static bool
ph_vs_viewport_tail(struct ph_vs_ctx *ctx, const struct ph_vs_value *clip)
{
   struct ph_vs_program *program = ctx->program;
   struct ph_vs_value offset = {
      .file = P3D_WARP_FILE_C, .index = (uint8_t)program->vp_offset_slot,
      .swizzle = P3D_WARP_SWZ_IDENTITY,
   };
   struct ph_vs_value scale = {
      .file = P3D_WARP_FILE_C, .index = (uint8_t)program->vp_scale_slot,
      .swizzle = P3D_WARP_SWZ_XYZ1,
   };
   struct ph_vs_value latch = {
      .file = P3D_WARP_FILE_T, .index = 1,
      .swizzle = P3D_WARP_SWZ_IDENTITY,
   };
   struct ph_vs_value clip_xyz1 = *clip, latch_xyz0 = latch;
   struct ph_vs_value src[3], ndc;
   struct p3d_warp_inst in = {0};

   clip_xyz1.swizzle = P3D_WARP_SWZ_XYZ1;
   latch_xyz0.swizzle = P3D_WARP_SWZ_XYZ0;

   in.op = P3D_WARP_WRA;
   in.sop = P3D_WARP_SOP_MOV;
   in.sdst_file = P3D_WARP_FILE_DST_T;
   in.sdst_index = 1;
   in.sdst_mask = 0xf;
   in.ssrc = ph_vs_src(&offset);
   ph_vs_emit(ctx, &in);
   if (ctx->failed)
      return false;

   if (!ph_vs_alloc(ctx, &ndc.index))
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
   in.src[0] = ph_vs_src(clip);
   in.src[1] = ph_vs_src(&clip_xyz1);
   in.sop = P3D_WARP_SOP_MOV;
   in.sdst_file = P3D_WARP_FILE_DST_O;
   in.sdst_index = 11;
   in.sdst_mask = 0xf;
   in.ssrc = ph_vs_src(clip);
   ph_vs_emit(ctx, &in);
   program->output_mask |= 1u << 11;
   if (ctx->failed)
      return false;

   src[0] = scale;
   src[1] = ndc;
   src[2] = latch_xyz0;
   if (!ph_vs_issue(ctx, P3D_WARP_MAD, src, 3, P3D_WARP_FILE_DST_T, 1, 7,
                    NULL))
      return false;
   ph_vs_free(ctx, &ndc);
   return ph_vs_write_output(ctx, 0, &latch);
}

bool
ph_vs_compile(nir_shader *nir, const struct ph_vs_inputs *inputs,
              unsigned constant_first, unsigned max_instructions,
              bool allow_t2, struct ph_vs_program *out)
{
   struct ph_vs_ctx ctx = {
      .program = out, .inputs = inputs, .constant_first = constant_first,
      .max_instructions = MIN2(max_instructions, PH_VS_MAX_INSTRUCTIONS),
   };
   struct ph_vs_output position = {0};
   struct ph_vs_output varying[8] = {0};
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct ph_vs_value clip, value;
   unsigned behind;
   bool ok = false;

   memset(out, 0, sizeof(*out));
   if (!impl || ctx.max_instructions < 4)
      { PH_VS_REJECT("compile-early"); return false; }
   if (!ph_vs_collect(nir, &position, varying, ARRAY_SIZE(varying)) ||
       !ph_vs_scan_constants(nir, &ctx.constant_vec4s))
      { PH_VS_REJECT("compile-early"); return false; }
   if (constant_first + ctx.constant_vec4s > PH_VS_MAX_CONSTANT)
      { PH_VS_REJECT("compile-early"); return false; }

   nir_index_ssa_defs(impl);
   ctx.cache_size = impl->ssa_alloc;
   ctx.cache = calloc(ctx.cache_size, sizeof(*ctx.cache));
   if (!ctx.cache)
      { PH_VS_REJECT("compile-early"); return false; }

   /* The clip-space position first: the viewport tail consumes it, and the
    * remaining outputs may then reuse its temporaries. */
   if (!ph_vs_value(&ctx, position.value, &clip))
      do { PH_VS_REJECT("compile"); goto done; } while (0);
   if (!clip.is_temp || clip.neg || clip.swizzle != P3D_WARP_SWZ_IDENTITY) {
      /* A position that is a plain read still has to live in a temporary: the
       * tail reads it three times. */
      if (!ph_vs_materialise(&ctx, &clip))
         do { PH_VS_REJECT("compile"); goto done; } while (0);
   }
   ph_vs_retain(&ctx, &clip);
   ph_vs_flush_free(&ctx);

   for (unsigned i = 0; i < ARRAY_SIZE(varying) && varying[i].present; i++) {
      if (varying[i].parts > 1 || varying[i].covered != 0xf) {
         static const float zeros[4] = { 0 };
         struct ph_vs_value zero = ph_vs_swizzled_literal(zeros);

         /* Several partial stores: each one writes its own components. */
         for (unsigned p = 0; p < varying[i].parts; p++) {
            if (!ph_vs_value(&ctx, varying[i].part[p].value, &value) ||
                !ph_vs_write_output_masked(&ctx, varying[i].slot, &value,
                                           varying[i].part[p].mask))
               do { PH_VS_REJECT("compile"); goto done; } while (0);
            ph_vs_flush_free(&ctx);
         }
         if (varying[i].covered != 0xf &&
             !ph_vs_write_output_masked(&ctx, varying[i].slot, &zero,
                                        0xf & ~varying[i].covered))
            do { PH_VS_REJECT("compile"); goto done; } while (0);
         ctx.program->output_mask |= 1u << varying[i].slot;
         continue;
      }
      if (!ph_vs_output_direct(&ctx, varying[i].value, varying[i].slot)) {
         if (ctx.failed ||
             !ph_vs_value(&ctx, varying[i].value, &value) ||
             !ph_vs_write_output(&ctx, varying[i].slot, &value))
            do { PH_VS_REJECT("compile"); goto done; } while (0);
      }
      ph_vs_flush_free(&ctx);
   }
   /* The fragment path reads the interpolated colour whether or not the shader
    * wrote one, so give it white rather than whatever o1 held. */
   if (!(out->output_mask & (1u << 1))) {
      static const float ones[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
      struct ph_vs_value white = ph_vs_swizzled_literal(ones);

      if (!ph_vs_write_output(&ctx, 1, &white))
         do { PH_VS_REJECT("compile"); goto done; } while (0);
   }

   /* The viewport pair goes behind the shader's constants and its literals,
    * and keeps the proven c8/c9 slots whenever there is room. */
   behind = constant_first + ctx.constant_vec4s + out->literals;
   out->vp_scale_slot = MAX2(behind, 8u);
   out->vp_offset_slot = out->vp_scale_slot + 1;
   if (out->vp_offset_slot > PH_VS_MAX_CONSTANT ||
       !ph_vs_viewport_tail(&ctx, &clip))
      do { PH_VS_REJECT("compile"); goto done; } while (0);

   /* Prime the temporary every draw depends on, unless this draw samples a
    * texture - then writing it locks the engine. */
   if (allow_t2) {
      static const float zeros[4] = {0};
      struct ph_vs_value zero[2];

      zero[0] = zero[1] = ph_vs_swizzled_literal(zeros);
      if (!ph_vs_issue(&ctx, P3D_WARP_MUL, zero, 2, P3D_WARP_FILE_DST_T, 2,
                       0xf, NULL))
         do { PH_VS_REJECT("compile"); goto done; } while (0);
      out->writes_t2 = true;
   }

   if (ctx.failed || out->count + 2 > ctx.max_instructions)
      do { PH_VS_REJECT("compile"); goto done; } while (0);
   {
      static const uint32_t end[3] = { 0x00000200u, 0x00040000u, 0x14c00000u };
      static const uint32_t tail[3] = { 0x00080200u, 0x00440000u, 0x301a21a2u };

      memcpy(out->code + out->count * 3, end, sizeof(end));
      out->count++;
      memcpy(out->code + out->count * 3, tail, sizeof(tail));
      out->count++;
   }
   out->constants.first = constant_first;
   out->constants.vec4s = ctx.constant_vec4s;
   out->valid = true;
   ok = true;
done:
   free(ctx.cache);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok;
}
