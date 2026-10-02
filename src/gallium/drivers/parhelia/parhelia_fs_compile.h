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
   uint16_t offset[4];
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
   struct ph_fs_pre_src src[3];
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

struct ph_fs_program {
   struct p3d_comb_program comb;
   unsigned sampler_mask;
   unsigned const_ref_count;
   struct ph_fs_const_ref const_ref[2 * P3D_COMB_MAX_INSTRUCTIONS];
   bool uses_color;
   bool uses_color1;       /* v1: the secondary colour or the fog coordinate */
   bool uses_fogc;         /* v1 is the fog coordinate */
   bool valid;
   enum ph_fs_class cls;
   /* The preshader: uniform-only scalars the CPU evaluates before each draw;
    * a constant reference at PH_FS_PRE_BASE + i reads result i. */
   struct ph_fs_pre_node *pre;
   nir_const_value *pre_value;
   unsigned pre_count;
   const char *reject;     /* why it did not compile */
};

/* Compile @nir into at most @max_instructions combiner slots. */
bool ph_fs_compile(nir_shader *nir, unsigned max_instructions,
                   struct ph_fs_program *out);

/* Free what ph_fs_compile allocated for @program. */
void ph_fs_program_fini(struct ph_fs_program *program);

/* Copy the runtime constants into the program's constant registers.
 * @constants is the mapped fragment constant buffer, @size its bytes. */
void ph_fs_update_constants(struct ph_fs_program *program,
                            const void *constants, unsigned size);

/* Run @prog on one fragment: logical texture values @tex, colours @colour.
 * Every intermediate is clamped to [-@range, @range]. */
void ph_fs_eval(const struct p3d_comb_program *prog, const float tex[4][4],
                const float colour[2][4], float range, float out[4]);

void ph_fs_dump(const struct ph_fs_program *program, FILE *f);

/* True when draws with @nir must be dropped rather than rendered: an
 * application's assembly fragment program (Mesa names those "ARB<n>") that
 * @program could not express.  Fixed-function and internal shaders keep
 * their environment-matcher fallback. */
bool ph_fs_program_must_drop(const nir_shader *nir,
                             const struct ph_fs_program *program);

#endif
