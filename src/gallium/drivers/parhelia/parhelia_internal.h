/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef PARHELIA_INTERNAL_H
#define PARHELIA_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <stdlib.h>

#include "pipe/p_screen.h"
#include "parhelia_mipmap.h"

/* Presence of a debug environment variable, looked up once per call site:
 * several are tested for every staged triangle. */
#define ph_env(name) ({ \
   static int ph_env_cached_ = -1; \
   if (ph_env_cached_ < 0) \
      ph_env_cached_ = getenv(name) != NULL; \
   ph_env_cached_ != 0; })

/* Texture units a draw can actually drive today.  The state emitter banks all
 * four of the original Parhelia's units, and the host selftest covers three
 * and four, but a draw past two still needs a WARP program writing o3..o6, a
 * vertex layout with that many coordinate sets and the matching combiner
 * stages.  Raise this when those land. */
#define PH_TEXTURE_UNITS_DRIVEN 2u

#define PH_VRAM_VERTEX_LEGACY_SLOTS 3
#define PH_VRAM_VERTEX_SLOTS_MAX 16

struct parhelia_screen {
   struct pipe_screen base;
   int fd;
   uint32_t chip_id;
   unsigned texture_units;	/* what the chip has, not what a draw drives */
   uint32_t tile_layout;
   bool single_texture_pipe;
   bool vram_vertex;
   bool bound_range;
   bool async_submit;
   uint32_t async_slots;
   bool pack_adjacent_packets;
   bool stateful_submit;
   bool native_share;
   bool texture_upload;
   bool native_rt_classes;
   bool render_transfer;
   /* Native 8- and 16-bit sampler storage, and native S3TC.  Both stay
    * gated until they retire on this silicon. */
   bool narrow_formats;
   /* Native R5G6B5 colour and D16 depth render targets: the measured 128x16
    * tile order plus a kernel that allocates two bytes per pixel. */
   bool narrow_targets;
   bool s3tc;
   /* Volume and cube-map sampler targets: the texture-address unit's
    * three-coordinate mode plus the measured slice addressing.  The two are
    * separate flags because their storage is: a volume interleaves its
    * slices inside the storage tile and a cube map stacks whole face
    * images. */
   bool volume_targets;
   bool cube_targets;
   /* Zero means GET_PARAM was unavailable or returned an invalid value. */
   uint32_t kernel_max_packets;
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
   /* Pitch in PIXELS, as the surface descriptor and the tile-offset helpers
    * consume it, with @cpp bytes each.  R5G6B5 colour and Z16 depth targets
    * are two bytes; everything else is four. */
   unsigned stride;
   unsigned cpp;
   /* Surface descriptor format code: 0x32 A8R8G8B8, 0x28 R5G6B5, 0x36 S8Z24,
    * 0x2a D16.  Meaningless for sampler-only storage. */
   uint32_t surf_fmt;
   uint32_t tile_layout;
   void *map;
   void *malloc_data;
   bool native_shared;
   /* Native sampler BOs cannot be raw-mapped on P690.  Their CPU image is a
    * software-swizzled malloc shadow copied through UPLOAD_RENDER. */
   bool upload_texture;
   bool upload_dirty;
   /* Compact P690 colour/depth BOs are not raw-mappable.  This byte-exact
    * native-layout shadow crosses the transient host aperture only through
    * UPLOAD_RENDER/DOWNLOAD_RENDER. */
   bool transfer_shadow;

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
   /* P690 does not expose sampled alpha from its four-byte texture decoder.
    * These texture-layout BOs are generated lazily from level zero: one
    * carries A replicated into RGB for the native unit-1 alpha route, the
    * other carries RGB premultiplied by A for its older two-pass fallback. */
   uint32_t alpha_rgb_handle;
   uint32_t premul_rgb_handle;
   uint64_t alpha_companion_size;
   unsigned alpha_companion_stride;
   unsigned alpha_companion_width;
   unsigned alpha_companion_height;
   bool alpha_companion_repeat_s;
   bool alpha_companion_repeat_t;
   uint64_t alpha_companion_generation;
   /* The texture unit walks this contiguous chain itself.  `mip_defined`
    * records levels initialized by either transfer upload or rendering, so
    * incomplete textures cannot select garbage and read maps do not replace
    * a rendered level zero with a synthetic black image. */
   struct ph_mip_layout mip;
   struct ph_mip_layout linear_mip;
   uint16_t mip_defined;
   bool trace_mips;
   /* A P690 hardware depth clear is outstanding: the Z unit holds "every tile
    * reads as @zinit_value" and memory is still stale.  The 3-D pipeline sees
    * the cleared buffer; every other consumer needs ph_zinit_resolve() first.
    * @zinit_colour is the render target the resolve rectangle draws into with
    * its colour writes masked off. */
   bool zinit_pending;
   uint32_t zinit_value;
   struct pipe_resource *zinit_colour;
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

/* Native sampler storage of a Gallium format: the hardware format code for
 * 0x11e0[29:24] and its bytes per texel.  Formats outside this table are
 * converted to a 32-bit BGRA texel by the Gallium transfer path. */
struct ph_sampler_format {
   uint32_t code;
   unsigned cpp;
   /* Bytes per 4x4 S3TC block; zero for an uncompressed format. */
   unsigned block_bytes;
};

bool ph_sampler_format(enum pipe_format format, struct ph_sampler_format *out);

/* Native render-target or depth storage of a Gallium format: its surface
 * descriptor code and bytes per pixel.  False when the hardware has no such
 * surface and the format may only be sampled or converted. */
bool ph_target_format(enum pipe_format format, uint32_t *code, unsigned *cpp);
/* True for the two-byte render and depth surfaces, whose storage order is not
 * the sampler's. */
bool ph_narrow_target_format(enum pipe_format format);

void ph_resource_resolve_linear(struct parhelia_resource *r);
void ph_resource_upload_tiled(struct parhelia_resource *r);
int ph_resource_upload_texture(struct parhelia_resource *r);
int ph_resource_upload_shadow(struct parhelia_resource *r);
int ph_resource_download_shadow(struct parhelia_resource *r);
/* Transfer only the tile rows holding image rows [y0, y1). */
int ph_resource_upload_shadow_rows(struct parhelia_resource *r, unsigned y0,
                                   unsigned y1);
int ph_resource_download_shadow_rows(struct parhelia_resource *r, unsigned y0,
                                     unsigned y1);
struct pipe_context *ph_context_create(struct pipe_screen *screen, void *priv,
                                       unsigned flags);

#endif
