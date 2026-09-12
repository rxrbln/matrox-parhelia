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

/* Upper bound on a generated program.  The kernel's slot size is the real
 * limit and is passed to ph_vs_compile() at run time. */
#define PH_VS_MAX_INSTRUCTIONS 128u

/* Literal vec4s the compiler places behind the shader's own constants.  A
 * component that is only 0.0 or 1.0 costs nothing (the swizzle encodes both),
 * so these are the genuinely arbitrary values a shader spells out. */
#define PH_VS_MAX_LITERALS 16u

/* The constant file range the program reads.  The state layer block-loads
 * @vec4s vectors of the shader's constant buffer at @first, then the compiler's
 * own literals, then the viewport pair.  The constant file is the engine's
 * block-state file, so nothing here may be written by the program itself. */
struct ph_vs_constants {
   unsigned first;      /* WARP constant slot of constant-buffer vec4 0 */
   unsigned vec4s;      /* constant-buffer vec4s the program reads */
};

struct ph_vs_program {
   uint32_t code[PH_VS_MAX_INSTRUCTIONS * 3];
   unsigned count;              /* instructions, including END and its pad */
   struct ph_vs_constants constants;
   /* Literals the compiler allocated, loaded right behind the constant
    * buffer's vec4s. */
   float literal[PH_VS_MAX_LITERALS][4];
   unsigned literals;
   /* Viewport pair slots, chosen behind everything else. */
   unsigned vp_scale_slot, vp_offset_slot;
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
 * shader's constant buffer is loaded at, @max_instructions the kernel's
 * program-slot size.  @allow_t2 must be false whenever the draw may sample a
 * texture: writing WARP temporary T2 locks the engine on the first
 * vertex-alpha draw, and Mesa primes T2 at context creation instead.
 *
 * Returns false for anything outside the expression set, and the caller then
 * keeps Gallium's draw module.
 */
bool ph_vs_compile(nir_shader *nir, const struct ph_vs_inputs *inputs,
                   unsigned constant_first, unsigned max_instructions,
                   bool allow_t2, struct ph_vs_program *out);

#endif
