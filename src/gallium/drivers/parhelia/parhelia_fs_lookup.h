/*
 * Fragment programs beyond the combiner, as lookup textures of their input.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef PARHELIA_FS_LOOKUP_H
#define PARHELIA_FS_LOOKUP_H

#include <stdbool.h>
#include <stdint.h>
#include "compiler/nir/nir.h"
#include "parhelia_fs_compile.h"

/* A program whose colour is a function of one varying alone: the CPU
 * evaluates the program per texel of a lookup texture, the unit samples it
 * with the varying as coordinate and the combiner passes the texel on.
 * PH_FS_RAMP_DIRECTION: the colour depends only on the direction of a
 * three-component varying (proved by scaling degree), a cube map.
 * PH_FS_RAMP_SCALAR / _PLANE: one or two components, a 1-D or 2-D table
 * over the components' range in the draw (ramp lo/span, set per draw). */
struct ph_fs_lookup {
   nir_shader *nir;        /* the program as the CPU evaluates it */
   uint32_t uid;           /* distinguishes programs in texture caches */
   uint8_t kind;           /* enum ph_fs_ramp_kind */
   uint8_t location;       /* VARYING_SLOT_* */
   uint8_t comps;
   uint8_t comp[4];        /* the varying components, in coordinate order */
   uint8_t out_mask;       /* colour channels the program writes */
   unsigned num_defs;
   bool *varies;           /* per SSA def: depends on the varying */
   nir_instr **steps;      /* what runs per texel, in program order */
   unsigned step_count;
};

/* Texels per dimension of a lookup of @kind (a cube map's face edge). */
unsigned ph_fs_lookup_size(unsigned kind);

/* Rewrite @nir, a flattened program the combiner refused, into one sample
 * of a lookup texture on unit 0, which must be free (@used_units == 0),
 * of a kind in the mask @kinds (bit per enum ph_fs_ramp_kind).
 * On success @ramp describes the unit and *@fn the evaluator, which the
 * caller frees with ph_fs_lookup_free(); false with @reject otherwise,
 * leaving @nir untouched. */
bool ph_fs_lower_lookup(nir_shader *nir, unsigned used_units,
                        unsigned max_units, unsigned kinds,
                        struct ph_fs_ramp *ramp,
                        struct ph_fs_lookup **fn, const char **reject);

void ph_fs_lookup_free(struct ph_fs_lookup *fn);

/* Saturate every colour store of @nir: the colour targets are fixed-point,
 * where GL clamps the fragment colour anyway.  True when a store changed. */
bool ph_fs_saturate_colour(nir_shader *nir);

/* Fill the B8G8R8A8 texels of @fn's texture of @r's size and domain for
 * the fragment constants @constants (@size bytes): a cube map is six faces
 * in GL face order.  False when the program reads constants beyond @size
 * or a one- or two-component table cannot represent the colour (it changes
 * faster than the texels over that domain). */
bool ph_fs_lookup_fill(const struct ph_fs_lookup *fn,
                       const struct ph_fs_ramp *r,
                       const void *constants, unsigned size, uint8_t *texels);

/* The uniform values @fn's table depends on, as *@count dwords in *@key
 * (malloc'ed); false as for ph_fs_lookup_fill(). */
bool ph_fs_lookup_key(const struct ph_fs_lookup *fn, const void *constants,
                      unsigned size, uint32_t **key, unsigned *count);

/* The colour @fn computes for varying value @v (components in coordinate
 * order), unclamped; false as for ph_fs_lookup_fill(). */
bool ph_fs_lookup_eval(const struct ph_fs_lookup *fn, const float *v,
                       const void *constants, unsigned size, float out[4]);

/* The coordinate the unit of @r reads, from the varying's vertex value,
 * for the batch's domain [@lo, @lo + @span) of a one- or two-component
 * table. */
void ph_fs_lookup_stage(const struct ph_fs_ramp *r, const float *lo,
                        const float *span, const float *src, float c[4]);

#endif
