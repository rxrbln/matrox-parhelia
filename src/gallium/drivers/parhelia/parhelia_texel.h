/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * The one place that decides where a texel of a sampler image lives.
 *
 * The storage order is not a free choice: 0x11e0[18:16] tells the address unit
 * which order to read, so whatever writes the texels has to use the order that
 * descriptor selects - see ph_sampler_texelcode(), which picks 4 for an 8- or
 * 16-bit texel and 2 for a 32-bit one.  Every code path that disagreed with it
 * produced the same defect: the orders share only some of their address bits,
 * so a small image loses a half and a larger one comes out striped, which
 * reads as a rendering fault rather than as an upload fault.  Keeping the
 * decision in one function is what prevents another one.
 */
#ifndef PARHELIA_TEXEL_H
#define PARHELIA_TEXEL_H

#include <stdbool.h>
#include <stddef.h>

#include "parhelia_mipmap.h"
#include "reference/parhelia3d.h"

/* Byte offset of texel (x,y) of @level in a chain of @cpp-byte texels. */
static inline size_t
ph_mip_texel_offset_cpp(const struct ph_mip_layout *mip, unsigned level,
                        unsigned x, unsigned y, bool linear, unsigned cpp)
{
   const struct ph_mip_level *image = &mip->level[level];
   unsigned physical_pitch;
   bool native;

   /* A linear image is row-major in its own coordinates: the native level's
    * coordinate permutation belongs to the tiled orders alone. */
   if (linear)
      return image->offset + ((size_t)y * image->pitch + x) * cpp;
   native = ph_mip_physical_coords(mip, level, cpp, &x, &y, &physical_pitch);
   if (native)
      return p3d_texture_tile_offset_64x16_at(image->offset, x, y,
                                              physical_pitch);
   /* ph_mip_physical_coords() has already added the level's tail position. */
   if (mip->native_64x32)
      return image->offset + p3d_texture_tile_offset_64x32(x, y, image->pitch);
   if (mip->p690_narrow)
      return image->offset +
         p3d_texture_tile_offset_p690_narrow(x, y, image->pitch, cpp);
   return image->offset + p3d_texture_tile_offset_cpp(x, y, image->pitch, cpp);
}

static inline size_t
ph_mip_texel_offset(const struct ph_mip_layout *mip, unsigned level,
                    unsigned x, unsigned y, bool linear)
{
   return ph_mip_texel_offset_cpp(mip, level, x, y, linear, 4);
}

#endif
