/*
 * NIR -> Parhelia fragment-combiner compiler.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef PARHELIA_FS_COMPILE_H
#define PARHELIA_FS_COMPILE_H

#include <stdbool.h>
#include "compiler/nir/nir.h"
#include "reference/parhelia3d.h"

/* A constant the program reads out of the fragment constant buffer.  The
 * compiler cannot know its value, so it records where to copy it from and the
 * driver fills it in before each draw. */
struct ph_fs_const_ref {
   uint8_t instruction;
   uint8_t slot;        /* 0 = c0, 1 = c1 */
   uint16_t vec4_index; /* offset in the fragment constant buffer, in vec4s */
};

struct ph_fs_program {
   struct p3d_comb_program comb;
   unsigned sampler_mask;
   unsigned const_ref_count;
   struct ph_fs_const_ref const_ref[2 * P3D_COMB_MAX_INSTRUCTIONS];
   /* Set when the shader reads an interpolated colour, so the draw path knows
    * it must fetch and pass the vertex colour attribute. */
   bool uses_color;
   bool valid;
};

/* Compile @nir into @out using at most @max_instructions combiner slots.
 * Returns false and leaves out->valid clear when the shader is outside the
 * combiner's expression set; the caller then falls back to the draw module. */
bool ph_fs_compile(nir_shader *nir, unsigned max_instructions,
                   struct ph_fs_program *out);

/* Copy the runtime constants the program refers to into its instructions.
 * @constants is the mapped fragment constant buffer, @size its bytes. */
void ph_fs_update_constants(struct ph_fs_program *program,
                            const void *constants, unsigned size);

#endif
