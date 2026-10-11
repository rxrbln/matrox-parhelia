/* SPDX-License-Identifier: MIT */
#ifndef PARHELIA_SAMPLER_H
#define PARHELIA_SAMPLER_H

#include <stdbool.h>

#include "reference/parhelia3d.h"

/* Both saved vendor stacks expose a maximum anisotropy of two.  The sampler
 * has no separate ratio field: a request above one selects filter code 3 for
 * both image filters, while the mip-filter field remains independent. */
static inline enum p3d_filter
ph_sampler_image_filter(bool linear, unsigned max_anisotropy)
{
   if (max_anisotropy > 1)
      return P3D_FILTER_ANISO;
   return linear ? P3D_FILTER_LINEAR : P3D_FILTER_NEAREST;
}

/* 0x11e0[18:16] selects the storage order, not the texel size.  The vendor's
 * mtx263 (.text+0x51af5) turns its "textiled" class off for 32 bits per texel
 * and for packed YUV, and mtx2029 (.text+0x1e9a0) then writes 2 for the
 * untiled sampler class and 4 for the tiled one, so 8- and 16-bit textures
 * take layout 4 and 32-bit ones layout 2.  A 0528 GL_LUMINANCE capture
 * writes 0x11e0 = 0x11040002 (format 0x11, layout 4), and the live 32-bit
 * captures write 0x32020002. */
static inline unsigned
ph_sampler_texelcode(unsigned cpp)
{
   return cpp < 4 ? 4u : 2u;
}

#endif
