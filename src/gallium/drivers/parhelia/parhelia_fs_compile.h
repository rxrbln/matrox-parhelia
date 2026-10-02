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
   const char *reject;     /* why it did not compile */
};

/* Compile @nir into at most @max_instructions combiner slots. */
bool ph_fs_compile(nir_shader *nir, unsigned max_instructions,
                   struct ph_fs_program *out);

/* Copy the runtime constants into the program's constant registers.
 * @constants is the mapped fragment constant buffer, @size its bytes. */
void ph_fs_update_constants(struct ph_fs_program *program,
                            const void *constants, unsigned size);

/* Run @prog on one fragment: logical texture values @tex, colours @colour.
 * Every intermediate is clamped to [-@range, @range]. */
void ph_fs_eval(const struct p3d_comb_program *prog, const float tex[4][4],
                const float colour[2][4], float range, float out[4]);

void ph_fs_dump(const struct ph_fs_program *program, FILE *f);

#endif
