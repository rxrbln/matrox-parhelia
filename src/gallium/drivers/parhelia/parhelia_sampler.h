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

#endif
