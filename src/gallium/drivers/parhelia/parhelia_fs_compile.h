/*
 * NIR -> Parhelia fragment-combiner compiler.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef PARHELIA_FS_COMPILE_H
#define PARHELIA_FS_COMPILE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "compiler/nir/nir.h"
#include "reference/parhelia3d.h"

/* A constant register filled from the fragment constant buffer before each
 * draw: component c of constant @slot of @instruction is scalar @offset[c] of
 * the buffer, for every c in @mask. */
struct ph_fs_const_ref {
   uint8_t instruction;
   uint8_t slot;
   uint8_t mask;
   uint8_t e[4];           /* the register holds the value * 2^-e */
   uint16_t offset[4];
};

/* A runtime constant the program was compiled to read scaled by 2^-e: the
 * combiner holds [-1, 1], so a uniform beyond it selects a variant. */
struct ph_fs_uexp {
   uint16_t offset;
   uint8_t e;
};

/* One preshader scalar: @op over up to three sources, each a constant
 * buffer scalar, an earlier result or a literal. */
#define PH_FS_PRE_BASE 0x8000u

enum ph_fs_pre_kind { PH_FS_PRE_BUFFER, PH_FS_PRE_RESULT, PH_FS_PRE_LITERAL };

struct ph_fs_pre_src {
   uint8_t kind;
   uint32_t index;
   nir_const_value literal;
};

struct ph_fs_pre_node {
   nir_op op;
   uint8_t bit_size;
   uint8_t nsrc;
   uint8_t ncomp[3];       /* components of each source (reductions take 2-4) */
   struct ph_fs_pre_src src[3][4];
};

/* The fixed-function algebra a program computes, decided by evaluating it,
 * for the draw routes that emit their own combiner words.  Units are logical:
 * t1 is the second bound texture. */
enum ph_fs_class {
   PH_FS_CLASS_OTHER = 0,
   PH_FS_CLASS_COLOUR,     /* v0 */
   PH_FS_CLASS_REPLACE,    /* t0 */
   PH_FS_CLASS_MODULATE,   /* t0 * v0 */
   PH_FS_CLASS_DECAL,      /* rgb mix(v0, t0, t0.a), alpha v0 */
   PH_FS_CLASS_ADD,        /* rgb t0 + v0, alpha t0 * v0 */
   PH_FS_CLASS_MODULATE2,  /* t0 * v0 * t1 */
   PH_FS_CLASS_ADD2,       /* rgb t0 * v0 + t1, alpha t0 * v0 * t1 */
   PH_FS_CLASS_REPLACE1,   /* t1 */
   PH_FS_CLASS_MODULATE2_TEX, /* t0 * t1, no vertex colour */
};

/* One step of a fog-factor expression: operands are earlier steps. */
enum ph_fs_fog_kind {
   PH_FOG_COORD, PH_FOG_UNIFORM, PH_FOG_LITERAL, PH_FOG_NEG, PH_FOG_ABS,
   PH_FOG_ADD, PH_FOG_MUL, PH_FOG_FMA, PH_FOG_EXP2, PH_FOG_MIN, PH_FOG_MAX,
   PH_FOG_SAT,
};

struct ph_fs_fog_op {
   uint8_t kind;
   uint16_t a, b, c;
   uint32_t offset;        /* PH_FOG_UNIFORM: scalar index into the buffer */
   float value;            /* PH_FOG_LITERAL */
};

/* A floor of a varying, evaluated by a nearest-sampled ramp texture on a
 * free unit: texel j of dimension d holds code j * step, the floor is
 * code * 255 / step + k * lo[d], and the unit reads the varying's
 * components comp[] staged as (x - lo) / span. */
struct ph_fs_ramp {
   uint8_t unit;
   uint8_t location;       /* VARYING_SLOT_* */
   uint8_t dims;           /* floored components, 1..3 */
   uint8_t comp[3];
   uint16_t size[3];       /* texels per dimension */
   float lo[3], span[3];   /* coordinate domain [lo, lo + span) */
};

struct ph_fs_program {
   struct p3d_comb_program comb;
   unsigned sampler_mask;
   unsigned const_ref_count;
   struct ph_fs_const_ref const_ref[2 * P3D_COMB_MAX_INSTRUCTIONS];
   bool uses_color;
   bool uses_color1;       /* v1: the secondary colour or the fog coordinate */
   bool uses_fogc;         /* v1 is the fog coordinate */
   bool uses_color1_alpha; /* the program reads v1's alpha */
   bool uses_color0_alpha; /* the program reads v0's alpha */
   /* A generic varying the program reads as a value rides in a colour
    * interpolator the shader leaves free: route[k] is its varying slot for
    * v0/v1, or -1. */
   int8_t route[2];
   /* v1.x is GL's fog factor, evaluated per vertex by this expression over
    * the fog coordinate (GL allows the factor at each vertex); NULL when
    * the program reads the raw coordinate or no fog at all. */
   struct ph_fs_fog_op *fog;
   unsigned fog_ops;
   bool valid;
   enum ph_fs_class cls;
   /* Units whose texture alpha the classified two-unit algebra takes as
    * 1.0: the fixed-function program of an alpha-less format leaves that
    * factor out, so the route is exact only for such a bound format. */
   uint8_t alpha_one_mask;
   /* The preshader: uniform-only scalars the CPU evaluates before each draw;
    * a constant reference at PH_FS_PRE_BASE + i reads result i. */
   struct ph_fs_pre_node *pre;
   nir_const_value *pre_value;
   unsigned pre_count;
   const char *reject;     /* why it did not compile */
   /* What a recompile for larger runtime constants starts from. */
   nir_shader *nir;
   unsigned budget;
   struct ph_fs_uexp *uexp;
   unsigned uexp_count;
   /* Units the program samples as floor ramps. */
   struct ph_fs_ramp ramps[P3D_MAX_TEXTURE_UNITS];
   unsigned ramp_count;
};

/* The code step of a ramp dimension of @size texels. */
unsigned ph_fs_ramp_step(unsigned size);

/* Replace floor(varying * k) by ramp samples on units outside @used_units;
 * false with @reject when a floor has no ramp form. */
bool ph_fs_lower_floor_ramps(nir_shader *nir, unsigned used_units,
                             unsigned max_units, struct ph_fs_ramp *ramps,
                             unsigned *count, const char **reject);

/* Compile @nir into at most @max_instructions combiner slots. */
bool ph_fs_compile(nir_shader *nir, unsigned max_instructions,
                   struct ph_fs_program *out);

/* ph_fs_compile() for a program whose units @ramps hold floor ramps. */
bool ph_fs_compile_ramps(nir_shader *nir, unsigned max_instructions,
                         const struct ph_fs_ramp *ramps, unsigned ramp_count,
                         struct ph_fs_program *out);

/* Free what ph_fs_compile allocated for @program. */
void ph_fs_program_fini(struct ph_fs_program *program);

/* Copy the runtime constants into the program's constant registers.
 * @constants is the mapped fragment constant buffer, @size its bytes.  A
 * constant beyond [-1, 1] recompiles the program for its magnitude; false
 * when no variant can hold the constants. */
bool ph_fs_update_constants(struct ph_fs_program *program,
                            const void *constants, unsigned size);

/* Run @prog on one fragment: logical texture values @tex, colours @colour.
 * Every intermediate is clamped to [-@range, @range]. */
void ph_fs_eval(const struct p3d_comb_program *prog, const float tex[4][4],
                const float colour[2][4], float range, float out[4]);

/* Step @step of @program's fog expression for fog coordinate @fogc. */
float ph_fs_fog_eval_step(const struct ph_fs_program *program, unsigned step,
                          float fogc, const void *constants, unsigned size);

/* The fog factor for fog coordinate @fogc, from @program's expression. */
float ph_fs_fog_eval(const struct ph_fs_program *program, float fogc,
                     const void *constants, unsigned size);


void ph_fs_dump(const struct ph_fs_program *program, FILE *f);

/* True when draws with @nir must be dropped rather than rendered: an
 * application's assembly fragment program (Mesa names those "ARB<n>") that
 * @program could not express.  Fixed-function and internal shaders keep
 * their environment-matcher fallback. */
bool ph_fs_program_must_drop(const nir_shader *nir,
                             const struct ph_fs_program *program);

#endif
