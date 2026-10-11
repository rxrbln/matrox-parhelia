/*
 * Fragment programs beyond the combiner, as lookup textures of their input.
 *
 * The combiner has no reciprocal square root, power or exponential.  A
 * program whose colour depends on a single varying is a function of that
 * varying: the CPU evaluates the program itself (NIR's constant evaluator,
 * the current uniforms) into a texture, and the unit samples it with the
 * varying as its coordinate.  A colour that depends only on the direction of
 * a three-component varying - normalize() and everything computed from it,
 * the per-pixel lighting of a normal - is a cube map, which the unit indexes
 * by direction with no normalisation of its own.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_constant_expressions.h"
#include "util/u_atomic.h"
#include "util/u_math.h"

#include "parhelia_fs_lookup.h"

/* Scaling degree of a value under varying -> s * varying, in half units;
 * PH_DEG_NONE when the value does not scale as a power of s. */
#define PH_DEG_NONE INT8_MIN

static uint32_t ph_fs_lookup_uid;

static unsigned
ph_fs_lookup_env_size(const char *name, unsigned def, unsigned max)
{
   const char *e = getenv(name);
   const int v = e ? atoi(e) : 0;

   return v >= 8 && v <= (int)max && util_is_power_of_two_nonzero(v) ? v : def;
}

unsigned
ph_fs_lookup_size(unsigned kind)
{
   static unsigned size[3];

   if (!size[0]) {
      size[0] = ph_fs_lookup_env_size("PARHELIA_LOOKUP_CUBE", 128, 256);
      size[1] = ph_fs_lookup_env_size("PARHELIA_LOOKUP_LINE", 2048, 2048);
      size[2] = ph_fs_lookup_env_size("PARHELIA_LOOKUP_PLANE", 256, 512);
   }
   return kind == PH_FS_RAMP_DIRECTION ? size[0] :
          kind == PH_FS_RAMP_SCALAR ? size[1] : size[2];
}

/* The varying load @instr reads: its slot and first component. */
static bool
ph_fs_lookup_input(nir_instr *instr, unsigned *location, unsigned *first)
{
   nir_intrinsic_instr *in;

   if (instr->type != nir_instr_type_intrinsic)
      return false;
   in = nir_instr_as_intrinsic(instr);
   switch (in->intrinsic) {
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_input:
      if (!nir_src_is_const(in->src[in->intrinsic ==
                                    nir_intrinsic_load_interpolated_input]) ||
          nir_src_as_uint(in->src[in->intrinsic ==
                                  nir_intrinsic_load_interpolated_input]))
         return false;
      *location = nir_intrinsic_io_semantics(in).location;
      *first = nir_intrinsic_component(in);
      return true;
   case nir_intrinsic_load_deref: {
      nir_deref_instr *deref = nir_src_as_deref(in->src[0]);
      nir_variable *var = nir_deref_instr_get_variable(deref);

      if (!var || var->data.mode != nir_var_shader_in ||
          deref->deref_type != nir_deref_type_var)
         return false;
      *location = var->data.location;
      *first = var->data.location_frac;
      return true;
   }
   default:
      return false;
   }
}

static bool
ph_fs_lookup_uniform_load(const nir_intrinsic_instr *in)
{
   return in->intrinsic == nir_intrinsic_load_ubo ||
          in->intrinsic == nir_intrinsic_load_ubo_vec4 ||
          in->intrinsic == nir_intrinsic_load_uniform;
}

/* The colour output a store writes; false for any other store. */
static bool
ph_fs_lookup_colour_store(nir_intrinsic_instr *in, unsigned *first,
                          unsigned *mask)
{
   unsigned location;

   if (in->intrinsic == nir_intrinsic_store_output) {
      if (!nir_src_is_const(in->src[1]) || nir_src_as_uint(in->src[1]))
         return false;
      location = nir_intrinsic_io_semantics(in).location;
      *first = nir_intrinsic_component(in);
      *mask = nir_intrinsic_write_mask(in) << *first;
   } else if (in->intrinsic == nir_intrinsic_store_deref) {
      nir_deref_instr *deref = nir_src_as_deref(in->src[0]);
      nir_variable *var = nir_deref_instr_get_variable(deref);

      if (!var || var->data.mode != nir_var_shader_out ||
          deref->deref_type != nir_deref_type_var)
         return false;
      location = var->data.location;
      *first = var->data.location_frac;
      *mask = nir_intrinsic_write_mask(in) << *first;
   } else {
      return false;
   }
   return location == FRAG_RESULT_COLOR || location == FRAG_RESULT_DATA0;
}

static int
ph_deg_same(int a, int b)
{
   return a == b ? a : PH_DEG_NONE;
}

/* The scaling degree of channel @c of @alu from its sources' degrees. */
static int
ph_fs_lookup_alu_degree(const nir_alu_instr *alu, int8_t (*deg)[16], unsigned c)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   int s[NIR_ALU_MAX_INPUTS];

   if (info->output_size) {
      /* Reductions: every product term must scale alike. */
      int d = PH_DEG_NONE;

      if (alu->op != nir_op_fdot2 && alu->op != nir_op_fdot3 &&
          alu->op != nir_op_fdot4)
         goto generic;
      for (unsigned k = 0; k < info->input_sizes[0]; k++) {
         const int a = deg[alu->src[0].src.ssa->index][alu->src[0].swizzle[k]];
         const int b = deg[alu->src[1].src.ssa->index][alu->src[1].swizzle[k]];
         const int t = a == PH_DEG_NONE || b == PH_DEG_NONE ?
                       PH_DEG_NONE : a + b;

         d = k ? ph_deg_same(d, t) : t;
      }
      return d;
   }
   for (unsigned i = 0; i < info->num_inputs; i++) {
      if (info->input_sizes[i])
         goto generic;
      s[i] = deg[alu->src[i].src.ssa->index][alu->src[i].swizzle[c]];
      if (s[i] == PH_DEG_NONE)
         return PH_DEG_NONE;
   }
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_fneg:
   case nir_op_fabs:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      if (alu->op >= nir_op_vec2 && alu->op <= nir_op_vec4)
         return deg[alu->src[c].src.ssa->index][alu->src[c].swizzle[0]];
      return s[0];
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmin:
   case nir_op_fmax:
      return ph_deg_same(s[0], s[1]);
   case nir_op_fmul:
      return s[0] + s[1];
   case nir_op_fdiv:
      return s[0] - s[1];
   case nir_op_frcp:
      return -s[0];
   case nir_op_fsqrt:
      return s[0] % 2 ? PH_DEG_NONE : s[0] / 2;
   case nir_op_frsq:
      return s[0] % 2 ? PH_DEG_NONE : -s[0] / 2;
   case nir_op_ffma:
      return ph_deg_same(s[0] + s[1], s[2]);
   case nir_op_flrp:
      return s[2] ? PH_DEG_NONE : ph_deg_same(s[0], s[1]);
   case nir_op_fsign:
      return 0;
   case nir_op_flt:
   case nir_op_fge:
   case nir_op_feq:
   case nir_op_fneu:
   case nir_op_slt:
   case nir_op_sge:
   case nir_op_seq:
   case nir_op_sne:
      /* A positive scale keeps the order of two equally scaling values. */
      return ph_deg_same(s[0], s[1]) == PH_DEG_NONE ? PH_DEG_NONE : 0;
   case nir_op_bcsel:
   case nir_op_fcsel:
   case nir_op_fcsel_gt:
   case nir_op_fcsel_ge:
      return s[0] ? PH_DEG_NONE : ph_deg_same(s[1], s[2]);
   default:
      break;
   }
generic:
   /* Anything else is exact only on values that do not scale at all. */
   for (unsigned i = 0; i < info->num_inputs; i++) {
      const unsigned n = nir_ssa_alu_instr_src_components(alu, i);

      for (unsigned k = 0; k < n; k++)
         if (deg[alu->src[i].src.ssa->index][alu->src[i].swizzle[k]])
            return PH_DEG_NONE;
   }
   return 0;
}

bool
ph_fs_saturate_colour(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);

      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;
            unsigned first, mask, si;
            nir_def *value;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (!ph_fs_lookup_colour_store(in, &first, &mask))
               continue;
            si = in->intrinsic == nir_intrinsic_store_deref;
            value = in->src[si].ssa;
            if (nir_def_instr(value)->type == nir_instr_type_alu &&
                nir_def_as_alu(value)->op == nir_op_fsat)
               continue;
            b.cursor = nir_before_instr(instr);
            if (nir_def_is_const(value) && value->bit_size == 32) {
               /* a literal converts as GL converts to fixed point, NaN to 0 */
               nir_const_value c[NIR_MAX_VEC_COMPONENTS];
               bool changed = false;

               for (unsigned k = 0; k < value->num_components; k++) {
                  const nir_const_value v = nir_def_as_load_const(value)->value[k];
                  const float f = v.f32 > 0.0f ? MIN2(v.f32, 1.0f) : 0.0f;

                  c[k] = nir_const_value_for_float(f, 32);
                  changed |= c[k].u32 != v.u32;
               }
               if (!changed)
                  continue;
               nir_src_rewrite(&in->src[si],
                               nir_build_imm(&b, value->num_components, 32, c));
            } else {
               nir_src_rewrite(&in->src[si], nir_fsat(&b, value));
            }
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   return progress;
}

void
ph_fs_lookup_free(struct ph_fs_lookup *fn)
{
   if (!fn)
      return;
   ralloc_free(fn->nir);
   free(fn->varies);
   free(fn->steps);
   free(fn);
}

bool
ph_fs_lower_lookup(nir_shader *nir, unsigned used_units, unsigned max_units,
                   unsigned kinds, struct ph_fs_ramp *ramp, struct ph_fs_lookup **fn_out,
                   const char **reject)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_intrinsic_instr **stores = NULL;
   unsigned store_count = 0, location = ~0u, out_mask = 0;
   uint8_t comp_read = 0;
   int8_t (*deg)[16] = NULL;
   bool *varies = NULL, homogeneous = true;
   struct ph_fs_lookup *fn = NULL;
   unsigned kind, comps;
   bool ok = false;

   *fn_out = NULL;
   if (used_units || !max_units) {
      *reject = "a lookup texture needs unit 0 free";
      return false;
   }
   if (exec_list_length(&impl->body) != 1) {
      *reject = "control flow";
      return false;
   }
   nir_index_ssa_defs(impl);
   deg = calloc(impl->ssa_alloc, sizeof(*deg));
   varies = calloc(impl->ssa_alloc, sizeof(*varies));
   if (!deg || !varies) {
      *reject = "out of memory";
      goto done;
   }
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         unsigned loc, first, mask;

         switch (instr->type) {
         case nir_instr_type_load_const:
         case nir_instr_type_undef:
         case nir_instr_type_deref:
            continue;
         case nir_instr_type_alu: {
            nir_alu_instr *alu = nir_instr_as_alu(instr);

            for (unsigned c = 0; c < alu->def.num_components; c++)
               deg[alu->def.index][c] = ph_fs_lookup_alu_degree(alu, deg, c);
            for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
               varies[alu->def.index] |= varies[alu->src[i].src.ssa->index];
            continue;
         }
         case nir_instr_type_intrinsic:
            break;
         default:
            *reject = "a texture lookup beside a lookup texture";
            goto done;
         }
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

         if (ph_fs_lookup_input(instr, &loc, &first)) {
            if (location != ~0u && loc != location) {
               *reject = "a colour of more than one varying";
               goto done;
            }
            location = loc;
            varies[in->def.index] = true;
            for (unsigned c = 0; c < in->def.num_components; c++) {
               const unsigned vc = first + c;

               deg[in->def.index][c] = 2;
               if (vc < 4 && (nir_def_components_read(&in->def) & (1u << c)))
                  comp_read |= 1u << vc;
            }
         } else if (ph_fs_lookup_uniform_load(in)) {
            for (unsigned i = 0; i < nir_intrinsic_infos[in->intrinsic].num_srcs; i++)
               if (varies[in->src[i].ssa->index]) {
                  *reject = "a uniform indexed by a varying";
                  goto done;
               }
         } else if (ph_fs_lookup_colour_store(in, &first, &mask)) {
            const unsigned n = in->src[in->intrinsic == nir_intrinsic_store_deref].ssa->num_components;
            nir_def *value = in->src[in->intrinsic == nir_intrinsic_store_deref].ssa;
            nir_intrinsic_instr **grown;

            for (unsigned c = 0; c < n; c++)
               if (((mask >> first) & (1u << c)) && deg[value->index][c] != 0)
                  homogeneous = false;
            grown = realloc(stores, (store_count + 1) * sizeof(*stores));
            if (!grown) {
               *reject = "out of memory";
               goto done;
            }
            stores = grown;
            stores[store_count++] = in;
            out_mask |= mask;
         } else if (in->intrinsic != nir_intrinsic_load_barycentric_pixel &&
                    in->intrinsic != nir_intrinsic_load_barycentric_centroid &&
                    in->intrinsic != nir_intrinsic_load_barycentric_sample) {
            *reject = nir_intrinsic_infos[in->intrinsic].name;
            goto done;
         }
      }
   }
   if (location == ~0u || !store_count || (out_mask & ~0xfu)) {
      *reject = "a lookup of no varying";
      goto done;
   }
   /* One or two components tabulate over the draw's range of them; three
    * only through their direction. */
   comps = util_bitcount(comp_read);
   kind = comps == 1 ? PH_FS_RAMP_SCALAR : comps == 2 ? PH_FS_RAMP_PLANE :
          comps == 3 && homogeneous ? PH_FS_RAMP_DIRECTION : ~0u;
   if (kind != ~0u && !(kinds & (1u << kind))) {
      *reject = "a colour of one or two components needs the draw's range";
      goto done;
   }
   if (location == VARYING_SLOT_COL0 || location == VARYING_SLOT_COL1 ||
       kind == ~0u) {
      *reject = comps == 3 ? "a colour of three components beyond their direction" :
                "a colour of more than three components";
      goto done;
   }
   fn = calloc(1, sizeof(*fn));
   if (!fn || !(fn->nir = nir_shader_clone(NULL, nir))) {
      *reject = "out of memory";
      goto done;
   }
   fn->uid = p_atomic_inc_return(&ph_fs_lookup_uid);
   fn->kind = kind;
   fn->location = location;
   fn->out_mask = out_mask;
   fn->pixel = location == VARYING_SLOT_POS && !(comp_read & ~3u);
   fn->origin_lower_left = !nir->info.fs.origin_upper_left;
   fn->pixel_center_integer = nir->info.fs.pixel_center_integer;
   for (unsigned c = 0; c < 4; c++)
      if (comp_read & (1u << c))
         fn->comp[fn->comps++] = c;
   nir_index_ssa_defs(nir_shader_get_entrypoint(fn->nir));
   fn->num_defs = nir_shader_get_entrypoint(fn->nir)->ssa_alloc;
   fn->varies = varies;
   varies = NULL;
   nir_foreach_block(block, nir_shader_get_entrypoint(fn->nir)) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         unsigned first, mask;
         nir_instr **grown;

         if (def ? !fn->varies[def->index] :
             instr->type != nir_instr_type_intrinsic ||
             !ph_fs_lookup_colour_store(nir_instr_as_intrinsic(instr),
                                        &first, &mask))
            continue;
         grown = realloc(fn->steps, (fn->step_count + 1) * sizeof(*grown));
         if (!grown) {
            *reject = "out of memory";
            goto done;
         }
         fn->steps = grown;
         fn->steps[fn->step_count++] = instr;
      }
   }

   {
      /* The colour stores move to the end and take the cube map's texel. */
      nir_builder b = nir_builder_create(impl);
      nir_tex_instr *tex;

      for (unsigned i = 0; i < store_count; i++)
         nir_instr_move(nir_after_impl(impl), &stores[i]->instr);
      b.cursor = nir_before_instr(&stores[0]->instr);
      tex = nir_tex_instr_create(nir, 1);
      tex->op = nir_texop_tex;
      tex->sampler_dim = kind == PH_FS_RAMP_DIRECTION ? GLSL_SAMPLER_DIM_CUBE :
                                                        GLSL_SAMPLER_DIM_2D;
      tex->dest_type = nir_type_float32;
      tex->texture_index = tex->sampler_index = 0;
      tex->coord_components = kind == PH_FS_RAMP_DIRECTION ? 3 : 2;
      /* The unit reads the staged varying (ph_fs_lookup_stage), so the
       * program keeps no load that would route the varying as a value. */
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                        nir_imm_zero(&b, tex->coord_components, 32));
      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);
      for (unsigned i = 0; i < store_count; i++) {
         nir_intrinsic_instr *in = stores[i];
         const unsigned si = in->intrinsic == nir_intrinsic_store_deref;
         const unsigned n = in->src[si].ssa->num_components;
         unsigned first, mask, swz[4];

         ph_fs_lookup_colour_store(in, &first, &mask);
         for (unsigned c = 0; c < n; c++)
            swz[c] = MIN2(first + c, 3);
         b.cursor = nir_before_instr(&in->instr);
         nir_src_rewrite(&in->src[si], nir_swizzle(&b, &tex->def, swz, n));
      }
      nir_progress(true, impl, nir_metadata_none);
      NIR_PASS(_, nir, nir_opt_dce);
   }
   memset(ramp, 0, sizeof(*ramp));
   ramp->kind = fn->kind;
   ramp->fn = fn;
   ramp->unit = 0;
   ramp->location = location;
   ramp->dims = comps;
   for (unsigned i = 0; i < comps; i++) {
      ramp->comp[i] = fn->comp[i];
      ramp->size[i] = ph_fs_lookup_size(kind);
      ramp->span[i] = 1.0f;
   }
   if (kind == PH_FS_RAMP_SCALAR)
      ramp->size[1] = 1;
   *fn_out = fn;
   fn = NULL;
   ok = true;
done:
   ph_fs_lookup_free(fn);
   free(stores);
   free(deg);
   free(varies);
   return ok;
}

/* ---- the CPU evaluator ------------------------------------------------- */

struct ph_fs_lookup_eval {
   const struct ph_fs_lookup *fn;
   const uint8_t *constants;
   unsigned size;
   const float *drv;       /* the driver constants, constant buffer 1 */
   nir_const_value *val;   /* 16 per SSA def */
   float out[4];
};

static bool
ph_fs_lookup_read_uniform(struct ph_fs_lookup_eval *e, nir_intrinsic_instr *in,
                          nir_const_value *dst)
{
   uint64_t base;

   switch (in->intrinsic) {
   case nir_intrinsic_load_ubo:
      if (e->val[in->src[0].ssa->index * 16].u32)
         return false;
      base = e->val[in->src[1].ssa->index * 16].u32;
      break;
   case nir_intrinsic_load_ubo_vec4:
      if (e->val[in->src[0].ssa->index * 16].u32 == PH_FS_DRV_UBO && e->drv) {
         base = ((uint64_t)nir_intrinsic_base(in) +
                 e->val[in->src[1].ssa->index * 16].u32) * 4 +
                nir_intrinsic_component(in);
         for (unsigned c = 0; c < in->def.num_components; c++) {
            if (in->def.bit_size != 32 || base + c >= PH_FS_DRV_COUNT)
               return false;
            dst[c].f32 = e->drv[base + c];
         }
         return true;
      }
      if (e->val[in->src[0].ssa->index * 16].u32)
         return false;
      base = ((uint64_t)nir_intrinsic_base(in) +
              e->val[in->src[1].ssa->index * 16].u32) * 16 +
             nir_intrinsic_component(in) * 4;
      break;
   default:
      base = (uint64_t)nir_intrinsic_base(in) +
             e->val[in->src[0].ssa->index * 16].u32;
      break;
   }
   for (unsigned c = 0; c < in->def.num_components; c++) {
      const uint64_t at = base + 4 * c;

      if (in->def.bit_size != 32 || at + 4 > e->size)
         return false;
      memcpy(&dst[c].u32, e->constants + at, 4);
   }
   return true;
}

/* The common 32-bit float operations, as NIR's constant evaluator computes
 * them, without its per-call generality; false for anything else. */
static bool
ph_fs_lookup_fast_alu(const nir_alu_instr *alu, nir_const_value *val,
                      nir_const_value *dst)
{
   const unsigned n = alu->def.num_components;
   const nir_const_value *s[3] = { NULL };
   float d = 0.0f;

   if (alu->def.bit_size != 32)
      return false;
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      if (alu->src[i].src.ssa->bit_size != 32 || i >= 3)
         return false;
      s[i] = &val[alu->src[i].src.ssa->index * 16];
   }
#define A(c) s[0][alu->src[0].swizzle[c]].f32
#define B(c) s[1][alu->src[1].swizzle[c]].f32
#define C(c) s[2][alu->src[2].swizzle[c]].f32
   switch (alu->op) {
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
      for (unsigned k = 0; k < nir_op_infos[alu->op].input_sizes[0]; k++)
         d += A(k) * B(k);
      for (unsigned c = 0; c < n; c++)
         dst[c].f32 = d;
      return true;
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      for (unsigned c = 0; c < n; c++)
         dst[c] = val[alu->src[c].src.ssa->index * 16 + alu->src[c].swizzle[0]];
      return true;
   default:
      break;
   }
   for (unsigned c = 0; c < n; c++) {
      float r;

      switch (alu->op) {
      case nir_op_mov:  dst[c] = s[0][alu->src[0].swizzle[c]]; continue;
      case nir_op_fneg: r = -A(c); break;
      case nir_op_fabs: r = fabsf(A(c)); break;
      case nir_op_fadd: r = A(c) + B(c); break;
      case nir_op_fsub: r = A(c) - B(c); break;
      case nir_op_fmul: r = A(c) * B(c); break;
      case nir_op_ffma: r = fmaf(A(c), B(c), C(c)); break;
      case nir_op_fmax: r = fmaxf(A(c), B(c)); break;
      case nir_op_fmin: r = fminf(A(c), B(c)); break;
      case nir_op_fsat: r = A(c) > 1.0f ? 1.0f : A(c) <= 0.0f ? 0.0f : A(c); break;
      case nir_op_frsq: r = 1.0f / sqrtf(A(c)); break;
      case nir_op_fsqrt: r = sqrtf(A(c)); break;
      case nir_op_frcp: r = 1.0f / A(c); break;
      case nir_op_fdiv: r = A(c) / B(c); break;
      case nir_op_fpow: r = powf(A(c), B(c)); break;
      case nir_op_fexp2: r = exp2f(A(c)); break;
      case nir_op_flog2: r = log2f(A(c)); break;
      case nir_op_flrp: r = A(c) * (1.0f - C(c)) + B(c) * C(c); break;
      default:
         return false;
      }
      dst[c].f32 = r;
   }
#undef A
#undef B
#undef C
   return true;
}

/* Evaluate the program's instructions that vary (@varying) or not. */
static bool
ph_fs_lookup_run(struct ph_fs_lookup_eval *e, const float *v, bool varying)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(e->fn->nir);
   const unsigned mode = e->fn->nir->info.float_controls_execution_mode;
   nir_block *block = nir_start_block(impl);
   unsigned step = 0;

   /* The varying part walks its step list, the rest the whole program. */
   for (nir_instr *instr = varying ? NULL : nir_block_first_instr(block);
        varying ? step < e->fn->step_count : instr != NULL;
        instr = varying ? NULL : nir_instr_next(instr)) {
      {
         nir_def *def;
         nir_const_value *dst;

         if (varying)
            instr = e->fn->steps[step++];
         def = nir_instr_def(instr);
         if (def && e->fn->varies[def->index] != varying)
            continue;
         dst = def ? &e->val[def->index * 16] : NULL;
         switch (instr->type) {
         case nir_instr_type_load_const:
            memcpy(dst, nir_instr_as_load_const(instr)->value,
                   def->num_components * sizeof(*dst));
            break;
         case nir_instr_type_undef:
            memset(dst, 0, def->num_components * sizeof(*dst));
            break;
         case nir_instr_type_alu: {
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            const nir_op_info *info = &nir_op_infos[alu->op];
            nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
            nir_const_value *srcs[NIR_ALU_MAX_INPUTS];
            unsigned bit_size = 0;
            uint16_t poison = 0;

            if (ph_fs_lookup_fast_alu(alu, e->val, dst))
               break;
            if (!nir_alu_type_get_type_size(info->output_type))
               bit_size = alu->def.bit_size;
            for (unsigned i = 0; i < info->num_inputs; i++) {
               const nir_const_value *s = &e->val[alu->src[i].src.ssa->index * 16];

               if (!bit_size && !nir_alu_type_get_type_size(info->input_types[i]))
                  bit_size = alu->src[i].src.ssa->bit_size;
               for (unsigned c = 0; c < nir_ssa_alu_instr_src_components(alu, i); c++)
                  src[i][c] = s[alu->src[i].swizzle[c]];
               srcs[i] = src[i];
            }
            nir_eval_const_opcode(alu->op, dst, &poison, alu->def.num_components,
                                  bit_size ? bit_size : 32, srcs, mode);
            break;
         }
         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            unsigned loc, first, mask;

            if (ph_fs_lookup_input(instr, &loc, &first)) {
               for (unsigned c = 0; c < def->num_components; c++) {
                  dst[c].f32 = 0.0f;
                  for (unsigned i = 0; i < e->fn->comps; i++)
                     if (e->fn->comp[i] == first + c)
                        dst[c].f32 = v[i];
               }
            } else if (ph_fs_lookup_uniform_load(in)) {
               if (!ph_fs_lookup_read_uniform(e, in, dst))
                  return false;
            } else if (varying && ph_fs_lookup_colour_store(in, &first, &mask)) {
               const unsigned si = in->intrinsic == nir_intrinsic_store_deref;
               const nir_const_value *s = &e->val[in->src[si].ssa->index * 16];

               for (unsigned c = 0; c < 4; c++)
                  if (mask & (1u << c))
                     e->out[c] = s[c - first].f32;
            }
            break;
         }
         default:
            break;
         }
      }
   }
   return true;
}

static bool
ph_fs_lookup_begin(struct ph_fs_lookup_eval *e, const struct ph_fs_lookup *fn,
                   const void *constants, unsigned size, const float *drv)
{
   e->fn = fn;
   e->constants = constants;
   e->size = size;
   e->drv = drv;
   e->val = calloc((size_t)fn->num_defs * 16, sizeof(*e->val));
   if (!e->val)
      return false;
   if (!ph_fs_lookup_run(e, NULL, false)) {
      free(e->val);
      return false;
   }
   return true;
}

bool
ph_fs_lookup_key(const struct ph_fs_lookup *fn, const void *constants,
                 unsigned size, const float *drv, uint32_t **key,
                 unsigned *count)
{
   struct ph_fs_lookup_eval e;
   uint32_t *k = NULL;
   unsigned n = 0;

   *key = NULL;
   *count = 0;
   if (!ph_fs_lookup_begin(&e, fn, constants, size, drv))
      return false;
   nir_foreach_block(block, nir_shader_get_entrypoint(fn->nir)) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *in;
         uint32_t *grown;

         if (instr->type != nir_instr_type_intrinsic ||
             !ph_fs_lookup_uniform_load(in = nir_instr_as_intrinsic(instr)))
            continue;
         grown = realloc(k, (n + in->def.num_components) * sizeof(*k));
         if (!grown) {
            free(k);
            free(e.val);
            return false;
         }
         k = grown;
         for (unsigned c = 0; c < in->def.num_components; c++)
            k[n++] = e.val[in->def.index * 16 + c].u32;
      }
   }
   free(e.val);
   *key = k;
   *count = n;
   return true;
}

bool
ph_fs_lookup_eval(const struct ph_fs_lookup *fn, const float *v,
                  const void *constants, unsigned size, const float *drv,
                  float out[4])
{
   struct ph_fs_lookup_eval e;
   bool ok;

   if (!ph_fs_lookup_begin(&e, fn, constants, size, drv))
      return false;
   memset(e.out, 0, sizeof(e.out));
   ok = ph_fs_lookup_run(&e, v, true);
   memcpy(out, e.out, sizeof(e.out));
   free(e.val);
   return ok;
}

static uint8_t
ph_fs_lookup_unorm8(float x)
{
   if (!(x > 0.0f))
      return 0;
   if (x >= 1.0f)
      return 255;
   return (uint8_t)lrintf(x * 255.0f);
}

/* The direction of texel (@s, @t) in [-1, 1] of GL cube face @face. */
static void
ph_fs_lookup_cube_dir(unsigned face, float sc, float tc, float d[3])
{
   switch (face) {
   case 0: d[0] = 1.0f;  d[1] = -tc;   d[2] = -sc;   break;
   case 1: d[0] = -1.0f; d[1] = -tc;   d[2] = sc;    break;
   case 2: d[0] = sc;    d[1] = 1.0f;  d[2] = tc;    break;
   case 3: d[0] = sc;    d[1] = -1.0f; d[2] = -tc;   break;
   case 4: d[0] = sc;    d[1] = -tc;   d[2] = 1.0f;  break;
   default: d[0] = -sc;  d[1] = -tc;   d[2] = -1.0f; break;
   }
}

bool
ph_fs_lookup_fill(const struct ph_fs_lookup *fn, const struct ph_fs_ramp *r,
                  const void *constants, unsigned size, const float *drv,
                  uint8_t *texels)
{
   static const unsigned byte_of[4] = { 2, 1, 0, 3 };   /* B8G8R8A8 */
   const unsigned w = r->size[0], h = r->size[1];
   const unsigned faces = fn->kind == PH_FS_RAMP_DIRECTION ? 6 : 1;
   const size_t count = (size_t)faces * w * h;
   struct ph_fs_lookup_eval e;
   float *value = malloc(count * 4 * sizeof(*value));

   if (!value)
      return false;
   if (!ph_fs_lookup_begin(&e, fn, constants, size, drv)) {
      free(value);
      return false;
   }
   for (unsigned face = 0; face < faces; face++)
      for (unsigned y = 0; y < h; y++)
         for (unsigned x = 0; x < w; x++) {
            float d[3], len;

            /* a pixel table's texels beyond the framebuffer are never sampled */
            if (fn->pixel && ((r->extent[0] && x >= r->extent[0]) ||
                              (r->extent[1] && y >= r->extent[1]))) {
               memset(&value[4 * ((face * h + y) * w + x)], 0, 4 * sizeof(*value));
               continue;
            }
            if (fn->kind == PH_FS_RAMP_DIRECTION) {
               ph_fs_lookup_cube_dir(face, 2.0f * (x + 0.5f) / w - 1.0f,
                                     2.0f * (y + 0.5f) / h - 1.0f, d);
               len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
               for (unsigned c = 0; c < 3; c++)
                  d[c] /= len;
            } else if (fn->pixel) {
               /* the window position of pixel (x, y) as gl_FragCoord has it */
               const float c = fn->pixel_center_integer ? 0.0f : 0.5f;

               for (unsigned i = 0; i < fn->comps; i++) {
                  const unsigned p = i ? y : x;

                  d[i] = fn->comp[i] == 1 && fn->origin_lower_left ?
                         r->extent[i] - p - 1 + c : p + c;
               }
            } else {
               /* texel centres at the cell centres of the domain: the
                * domain's ends are often where a function jumps (x = -1
                * of atan(y, x)), and no fragment samples them exactly */
               d[0] = r->lo[0] + r->span[0] * (x + 0.5f) / w;
               d[1] = r->lo[1] + r->span[1] * (y + 0.5f) / h;
            }
            memset(e.out, 0, sizeof(e.out));
            if (!ph_fs_lookup_run(&e, d, true)) {
               free(e.val);
               free(value);
               return false;
            }
            memcpy(&value[4 * ((face * h + y) * w + x)], e.out, sizeof(e.out));
         }
   /* A table must represent its function: between texel centres, the
    * function has to stay close to what the unit interpolates.  A colour
    * that changes faster than the texels (fract(x * y) over the window)
    * is refused, not sampled wrong; isolated jumps (atan's branch cut, a
    * comparison) pass. */
   if (fn->kind != PH_FS_RAMP_DIRECTION && !fn->pixel) {
      const unsigned stride = h > 1 ? 3 : 1;
      unsigned checked = 0, bad = 0;

      for (unsigned y = 0; y + (h > 1) < h; y += stride)
         for (unsigned x = 0; x + 1 < w; x += stride) {
            const float *v00 = &value[4 * (y * w + x)];
            const float *v10 = v00 + 4;
            const float *v01 = h > 1 ? v00 + 4 * w : v00;
            const float *v11 = h > 1 ? v01 + 4 : v10;
            float d[2];
            bool wrong = false;

            d[0] = r->lo[0] + r->span[0] * (x + 1.0f) / w;
            d[1] = r->lo[1] + r->span[1] * (h > 1 ? y + 1.0f : 0.5f) / h;
            memset(e.out, 0, sizeof(e.out));
            if (!ph_fs_lookup_run(&e, d, true))
               continue;
            for (unsigned c = 0; c < 4; c++) {
               const float lerp = h > 1 ?
                  0.25f * (CLAMP(v00[c], 0, 1) + CLAMP(v10[c], 0, 1) +
                           CLAMP(v01[c], 0, 1) + CLAMP(v11[c], 0, 1)) :
                  0.5f * (CLAMP(v00[c], 0, 1) + CLAMP(v10[c], 0, 1));

               if (isnan(e.out[c]) || isnan(lerp))
                  continue;
               wrong |= fabsf(CLAMP(e.out[c], 0, 1) - lerp) > 4.0f / 255.0f;
            }
            checked++;
            bad += wrong;
         }
      if (bad * 50 > checked) {
         free(e.val);
         free(value);
         return false;
      }
   }
   free(e.val);
   /* An undefined result (NaN: asin beyond 1 between the last fragment and
    * the vertex) takes its nearest defined neighbour along x, then y, so a
    * texel no fragment needs does not bleed into those that do. */
   for (unsigned pass = 0; pass < 2 && fn->kind != PH_FS_RAMP_DIRECTION; pass++) {
      const unsigned lines = pass ? w : h, len = pass ? h : w;
      const size_t step = pass ? 4 * w : 4, line_step = pass ? 4 : 4 * w;

      for (unsigned l = 0; l < lines; l++)
         for (unsigned c = 0; c < 4; c++) {
            float *v = value + l * line_step + c;
            int last = -1;

            for (unsigned i = 0; i < len; i++) {
               if (isnan(v[i * step]))
                  continue;
               /* fill the gap before i from both ends */
               for (int j = last + 1; j < (int)i; j++)
                  v[j * step] = last >= 0 && j - last < (int)i - j ?
                                v[last * step] : v[i * step];
               last = i;
            }
            for (unsigned j = last + 1; last >= 0 && j < len; j++)
               v[j * step] = v[last * step];
         }
   }
   for (size_t i = 0; i < count; i++)
      for (unsigned c = 0; c < 4; c++)
         texels[4 * i + byte_of[c]] = ph_fs_lookup_unorm8(value[4 * i + c]);
   free(value);
   return true;
}

void
ph_fs_lookup_stage(const struct ph_fs_ramp *r, const float *lo,
                   const float *span, const float *src, float c[4])
{
   c[0] = c[1] = c[2] = 0.5f;
   c[3] = 1.0f;
   if (r->kind == PH_FS_RAMP_DIRECTION) {
      for (unsigned i = 0; i < 3; i++)
         c[i] = src[r->comp[i]];
      return;
   }
   for (unsigned d = 0; d < r->dims; d++)
      c[d] = (src[r->comp[d]] - lo[d]) / span[d];
}
