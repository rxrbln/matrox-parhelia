/* SPDX-License-Identifier: MIT */
#ifndef PARHELIA_INTERNAL_H
#define PARHELIA_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "pipe/p_screen.h"
#include "parhelia_mipmap.h"

struct parhelia_screen {
   struct pipe_screen base;
   int fd;
   uint32_t chip_id;
   uint32_t tile_layout;
   bool single_texture_pipe;
   bool vram_vertex;
   bool bound_range;
   bool async_submit;
   bool stateful_submit;
   bool native_share;
   bool texture_upload;
   unsigned max_vertex_dwords;
   uint64_t vram_size;
};

/* Render targets are natively tiled.  A resource which has to cross the KMS
 * or dma-buf boundary gets a distinct linear image; rendering never treats
 * that image as a native 3D surface.
 */
struct parhelia_resource {
   struct pipe_resource base;
   uint32_t handle;
   bool handle_owned;
   uint64_t size, mmap_offset;
   unsigned stride;
   uint32_t tile_layout;
   void *map;
   void *malloc_data;
   bool native_shared;
   /* Native sampler BOs cannot be raw-mapped on P690.  Their CPU image is a
    * software-swizzled malloc shadow copied through UPLOAD_RENDER. */
   bool upload_texture;
   bool upload_dirty;

   uint32_t linear_handle;
   uint64_t linear_size;
   unsigned linear_stride;
   void *linear_map;
   int linear_fd;
   bool linear_handle_owned;
   bool linear_is_dumb;
   bool tiled_dirty;
   bool linear_dirty;
   /* The original RGBA image remains in linear_map while a binary-alpha
    * source-over draw temporarily uses the native A1R5G5B5 tiled image. */
   bool rgb5a1_packed;
   bool rgb5a1_checked;
   bool rgb5a1_eligible;
   uint64_t rgb5a1_checked_generation;
   uint64_t rgb5a1_packed_generation;
   /* Increment after any CPU-visible image write.  Predicate caches use this
    * instead of dirty flags because uploads may update both representations. */
   uint64_t content_generation;
   /* The texture unit walks this contiguous chain itself.  `mip_defined`
    * records levels initialized by either transfer upload or rendering, so
    * incomplete textures cannot select garbage and read maps do not replace
    * a rendered level zero with a synthetic black image. */
   struct ph_mip_layout mip;
   struct ph_mip_layout linear_mip;
   uint16_t mip_defined;
};

static inline struct parhelia_screen *
ph_screen(struct pipe_screen *s)
{
   return (struct parhelia_screen *)s;
}

static inline struct parhelia_resource *
ph_resource(struct pipe_resource *r)
{
   return (struct parhelia_resource *)r;
}

void ph_resource_resolve_linear(struct parhelia_resource *r);
void ph_resource_upload_tiled(struct parhelia_resource *r);
int ph_resource_upload_texture(struct parhelia_resource *r);
struct pipe_context *ph_context_create(struct pipe_screen *screen, void *priv,
                                       unsigned flags);

#endif
