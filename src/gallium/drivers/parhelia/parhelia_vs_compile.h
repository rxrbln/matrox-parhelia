/*
 * NIR -> Parhelia WARP vertex-program compiler.
 *
 * Copyright 2026 René Rebe <rene@exactco.de>
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef PARHELIA_VS_COMPILE_H
#define PARHELIA_VS_COMPILE_H

#include <stdbool.h>
#include "pipe/p_state.h"
#include "compiler/nir/nir.h"
#include "reference/parhelia3d.h"

/* A WARP program slot holds 64 instructions; leave room for the viewport tail
 * and the END pair. */
#define PH_VS_MAX_INSTRUCTIONS 64u

/* The constant file range the program reads.  The state layer must block-load
 * the shader's constant buffer into [first, first + vec4s) before the draw;
 * the loader addresses the same file the instructions read. */
struct ph_vs_constants {
   unsigned first;      /* WARP constant slot of constant-buffer vec4 0 */
   unsigned vec4s;      /* how many vec4s the program actually reads */
};

struct ph_vs_program {
   uint32_t code[PH_VS_MAX_INSTRUCTIONS * 3];
   unsigned count;              /* instructions, including END and its pad */
   struct ph_vs_constants constants;
   /* Vertex-fetch slots the program reads, as a mask over v0..v15. */
   unsigned input_mask;
   /* Output slots written, as a mask over o0..o11. */
   unsigned output_mask;
   bool writes_t2;
   bool valid;
};

/* Map a NIR vertex input driver_location to a WARP vertex-fetch slot.  The
 * attribute descriptors the state layer emits use the classic fixed-function
 * numbering, so the caller supplies the mapping it programmed. */
struct ph_vs_inputs {
   int8_t slot[PIPE_MAX_ATTRIBS];       /* -1 when the attribute is unused */
   unsigned count;
};

/* Compile @nir into @out.  @constant_first is the WARP constant slot the
 * shader's constant buffer will be loaded at (0 for the fixed-function
 * matrix), @viewport_scale/@viewport_offset the slots holding the viewport
 * pair.  @allow_t2 must be false whenever the draw samples a texture: writing
 * WARP temporary T2 locks the engine on the first vertex-alpha draw.
 *
 * Returns false for anything outside the expression set, and the caller then
 * keeps Gallium's draw module.
 */
bool ph_vs_compile(nir_shader *nir, const struct ph_vs_inputs *inputs,
                   unsigned constant_first, unsigned viewport_scale,
                   unsigned viewport_offset, bool allow_t2,
                   struct ph_vs_program *out);

#endif
