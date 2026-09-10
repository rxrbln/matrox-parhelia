/* SPDX-License-Identifier: MIT */
/* Gallium screen and GEM resources for the kernel-owned Parhelia render path. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include "compiler/nir/nir.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/drm_mode.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_screen.h"
#include "draw/draw_context.h"
#include "parhelia_batch.h"
#include "parhelia_internal.h"
#include "parhelia_public.h"

static void ph_resource_destroy(struct pipe_screen *pscreen,
                                struct pipe_resource *pres);

static bool
ph_bo_layout_native(const struct parhelia_screen *screen, uint32_t layout)
{
   return (screen->tile_layout == PARHELIA_TILE_64X16_4K &&
           layout == PARHELIA_BO_LAYOUT_NATIVE_64X16) ||
          (screen->tile_layout == PARHELIA_TILE_64X32_8K &&
           layout == PARHELIA_BO_LAYOUT_NATIVE_64X32);
}

static bool
ph_query_bo(struct parhelia_screen *screen, uint32_t handle,
            struct drm_parhelia_get_bo_info *info)
{
   memset(info, 0, sizeof(*info));
   info->handle = handle;
   return !ioctl(screen->fd, DRM_IOCTL_PARHELIA_GET_BO_INFO, info);
}

/* Fragment NIR is inspected by the Parhelia state translator and executed by
 * draw's software fragment path; vertex NIR is executed by draw as well.  Use
 * the established no-integer lowering contract for that combination. */
static const nir_shader_compiler_options ph_fs_nir_options = {
   .fdot_replicates = true,
   .float_mul_add32 =
      nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
   .lower_bitops = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_fdiv = true,
   .lower_fdph = true,
   .lower_flrp32 = true,
   .lower_fmod = true,
   .lower_sincos = true,
   .lower_uniforms_to_ubo = true,
   .force_indirect_unrolling = nir_var_all,
   .force_indirect_unrolling_sampler = true,
   .max_unroll_iterations = 32,
   .no_integers = true,
   .has_fused_comp_and_csel = true,
};

static const nir_shader_compiler_options ph_vs_nir_options = {
   .lower_bitops = true,
   .lower_scmp = true,
   .lower_flrp32 = true,
   .lower_flrp64 = true,
   .lower_fsat = true,
   .lower_bitfield_insert = true,
   .lower_bitfield_extract = true,
   .lower_fdph = true,
   .lower_fmod = true,
   .lower_hadd = true,
   .lower_uadd_sat = true,
   .lower_usub_sat = true,
   .lower_iadd_sat = true,
   .lower_pack_snorm_2x16 = true,
   .lower_pack_snorm_4x8 = true,
   .lower_pack_unorm_2x16 = true,
   .lower_pack_unorm_4x8 = true,
   .lower_pack_half_2x16 = true,
   .lower_pack_split = true,
   .lower_unpack_snorm_2x16 = true,
   .lower_unpack_snorm_4x8 = true,
   .lower_unpack_unorm_2x16 = true,
   .lower_unpack_unorm_4x8 = true,
   .lower_unpack_half_2x16 = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_uadd_carry = true,
   .lower_usub_borrow = true,
   .lower_mul_2x32_64 = true,
   .lower_ifind_msb = true,
   .max_unroll_iterations = 32,
   .lower_uniforms_to_ubo = true,
   .lower_device_index_to_zero = true,
   .support_indirect_inputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES),
   .support_indirect_outputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES),
   .no_integers = true,
   .has_fused_comp_and_csel = true,
};

static const char *
ph_name(struct pipe_screen *screen)
{
   switch (ph_screen(screen)->chip_id) {
   case 0x0527: return "Matrox Parhelia-512";
   case 0x0528: return "Matrox Parhelia-8X";
   case 0x2537: return "Matrox Parhelia-LX (Sundog)";
   case 0x2538: return "Matrox Parhelia-LX (Sunex)";
   case 0x2539: return "Matrox P690 (SUN90)";
   default: return "Matrox Parhelia (unsupported silicon)";
   }
}
static const char *ph_vendor(struct pipe_screen *s) { return "Mesa"; }
static const char *ph_device_vendor(struct pipe_screen *s) { return "Matrox"; }
static int ph_fd(struct pipe_screen *s) { return ph_screen(s)->fd; }

static bool
ph_format_supported(struct pipe_screen *s, enum pipe_format format,
                    enum pipe_texture_target target, unsigned samples,
                    unsigned storage_samples, unsigned bind)
{
   bool supported = false;

   /* P690 has no sampler sRGB decode or render-target sRGB encode path.
    * Accepting these formats makes state_tracker advertise the sRGB texture
    * and framebuffer extensions, whose contract requires linear-domain
    * sampling/blending and an sRGB conversion at the documented boundaries.
    * The GL 1 fixed-function path must instead keep legacy RGB(A) texels and
    * the XRGB scanout buffer as raw normalized values; display calibration is
    * handled by the separate CRTC LUT. */
   if (util_format_is_srgb(format))
      return false;

   if (samples <= 1 && storage_samples <= 1) {
      if (target == PIPE_BUFFER) {
         supported = true;
      } else if (target == PIPE_TEXTURE_2D || target == PIPE_TEXTURE_RECT) {
         switch (format) {
		 case PIPE_FORMAT_B5G5R5A1_UNORM:
		    /* A1R5G5B5: vendor format id 0x08 -> sampler code 0x26.  It
		     * is sampler-only until a 16-bpp render-target route is known. */
		    supported = !(bind & (PIPE_BIND_DEPTH_STENCIL |
		                          PIPE_BIND_SCANOUT |
		                          PIPE_BIND_DISPLAY_TARGET |
		                          PIPE_BIND_SHARED));
		    break;
         case PIPE_FORMAT_B8G8R8A8_UNORM:
         case PIPE_FORMAT_B8G8R8X8_UNORM:
            supported = !(bind & PIPE_BIND_DEPTH_STENCIL);
            break;
         case PIPE_FORMAT_Z24_UNORM_S8_UINT:
         case PIPE_FORMAT_S8_UINT_Z24_UNORM:
            supported = !(bind & PIPE_BIND_RENDER_TARGET);
            break;
         default:
            /* The texture unit consumes a 32-bit texel.  Gallium's format
             * conversion path supplies the matching channel order for legacy
             * non-sRGB OpenGL formats. */
            supported = util_format_get_blocksize(format) == 4 &&
                        !(bind & PIPE_BIND_DEPTH_STENCIL);
            break;
         }
      }
   }
   if (getenv("PARHELIA_DEBUG"))
      fprintf(stderr,
              "parhelia: format query format=%u target=%u samples=%u/%u bind=%#x -> %u\n",
              format, target, samples, storage_samples, bind, supported);
   return supported;
}

void
ph_resource_resolve_linear(struct parhelia_resource *r)
{
   struct parhelia_batch batch;
   uint32_t fence;
   int ret = -EINVAL;

   if (!r || !r->tiled_dirty || !r->map || !r->linear_map)
      return;
   if (!getenv("PARHELIA_CPU_BLIT") &&
       r->handle && r->linear_handle && !(r->linear_stride & 3) &&
       r->linear_stride / 4 <= 0x3fff) {
      ret = parhelia_batch_blit(&batch,
                                r->linear_handle, r->linear_stride / 4,
                                1, 0, 0,
                                r->handle, r->stride, 2, 0, 0,
                                r->base.width0, r->base.height0);
      if (!ret)
         ret = parhelia_batch_submit(ph_screen(r->base.screen)->fd, &batch,
                                     NULL, 0, &fence);
      if (!ret) {
         r->tiled_dirty = false;
         r->linear_dirty = false;
         if (getenv("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: GPU resolve tiled=%u linear=%u %ux%u\n",
                    r->handle, r->linear_handle,
                    r->base.width0, r->base.height0);
         return;
      }
   }
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: CPU resolve resource=%p tiled=%u linear=%u %ux%u gpu_error=%d\n",
              (void *)r, r->handle, r->linear_handle,
              r->base.width0, r->base.height0, ret);
   for (unsigned y = 0; y < r->base.height0; y++)
      for (unsigned x = 0; x < r->base.width0; x++)
         memcpy((uint8_t *)r->linear_map + y * r->linear_stride + x * 4,
                (uint8_t *)r->map +
                   p3d_tile_offset_layout(x, y, r->stride, r->tile_layout), 4);
   r->tiled_dirty = false;
   r->linear_dirty = false;
}

void
ph_resource_upload_tiled(struct parhelia_resource *r)
{
   struct parhelia_batch batch;
   uint32_t fence;
   int ret = -EINVAL;

   if (!r || !r->linear_dirty || r->tiled_dirty ||
       !r->map || !r->linear_map)
      return;
   if (!getenv("PARHELIA_CPU_BLIT") &&
       r->handle && r->linear_handle && !(r->linear_stride & 3) &&
       r->linear_stride / 4 <= 0x3fff) {
      ret = parhelia_batch_blit(&batch, r->handle, r->stride, 2, 0, 0,
                                r->linear_handle, r->linear_stride / 4,
                                1, 0, 0, r->base.width0, r->base.height0);
      if (!ret)
         ret = parhelia_batch_submit(ph_screen(r->base.screen)->fd, &batch,
                                     NULL, 0, &fence);
      if (!ret) {
         r->linear_dirty = false;
         if (getenv("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: GPU upload linear=%u tiled=%u %ux%u\n",
                    r->linear_handle, r->handle,
                    r->base.width0, r->base.height0);
         return;
      }
   }
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: CPU upload resource=%p linear=%u tiled=%u %ux%u layout=%u gpu_error=%d\n",
              (void *)r, r->linear_handle, r->handle,
              r->base.width0, r->base.height0, r->tile_layout, ret);
   for (unsigned y = 0; y < r->base.height0; y++)
      for (unsigned x = 0; x < r->base.width0; x++)
         memcpy((uint8_t *)r->map +
                   (r->upload_texture ?
                    p3d_texture_tile_offset(x, y, r->mip.level[0].pitch) :
                    p3d_tile_offset_layout(x, y, r->stride, r->tile_layout)),
                (uint8_t *)r->linear_map +
                   (size_t)y * r->linear_stride + x * 4, 4);
   r->upload_dirty |= r->upload_texture;
   r->linear_dirty = false;
   r->rgb5a1_packed = false;
}

int
ph_resource_upload_texture(struct parhelia_resource *r)
{
   struct drm_parhelia_upload_render upload;

   if (!r || !r->upload_texture || !r->upload_dirty)
      return 0;
   if (!r->handle || !r->map || !r->size)
      return -EINVAL;
   upload = (struct drm_parhelia_upload_render) {
      .data = (uintptr_t)r->map,
      .size = r->size,
      .handle = r->handle,
   };
   if (ioctl(ph_screen(r->base.screen)->fd, DRM_IOCTL_PARHELIA_UPLOAD_RENDER,
             &upload)) {
      int ret = -errno;

      fprintf(stderr,
              "parhelia: texture upload handle=%u size=%llu failed: %s\n",
              r->handle, (unsigned long long)r->size, strerror(errno));
      return ret;
   }
   r->upload_dirty = false;
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr, "parhelia: texture upload handle=%u size=%llu\n",
              r->handle, (unsigned long long)r->size);
   return 0;
}

static struct pipe_resource *
ph_resource_create(struct pipe_screen *pscreen, const struct pipe_resource *templ)
{
   struct parhelia_screen *screen = ph_screen(pscreen);
   struct parhelia_resource *r = CALLOC_STRUCT(parhelia_resource);
   const bool share_native = screen->native_share &&
      (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                      PIPE_BIND_SHARED));
   const bool sampler_storage = !share_native &&
      (templ->bind & PIPE_BIND_SAMPLER_VIEW) &&
      templ->width0 <= 512 && templ->height0 <= 512 &&
      !(templ->bind & PIPE_BIND_DEPTH_STENCIL);
   const unsigned max_width = share_native ? 4096 : 1024;
   const unsigned max_height = share_native ? 4096 : 768;
   unsigned mip_levels;
   if (!r)
      return NULL;
   r->linear_fd = -1;
   r->base = *templ;
   r->base.screen = pscreen;
   r->tile_layout = screen->tile_layout;
   r->native_shared = share_native;
   pipe_reference_init(&r->base.reference, 1);
   if (templ->target == PIPE_BUFFER) {
      r->size = templ->width0;
      r->malloc_data = align_malloc(r->size, 64);
      if (!r->malloc_data)
         goto fail;
      return &r->base;
   }
   if (templ->width0 > max_width || templ->height0 > max_height ||
       templ->depth0 != 1 ||
       templ->array_size != 1) {
      fprintf(stderr,
              "parhelia: reject resource target=%u format=%u %ux%ux%u array=%u levels=%u bind=%#x\n",
              templ->target, templ->format, templ->width0, templ->height0,
              templ->depth0, templ->array_size, templ->last_level + 1,
              templ->bind);
      goto fail;
   }
   /* P690 rejects the otherwise vendor-correct code-2 mip descriptor
    * (MIP_NEAREST/max_level=1) with a non-retiring engine.  Preserve the
    * historical base-level allocation until its sampler-specific chain mode
    * is recovered; lower-level transfer maps remain staging-only. */
   mip_levels = 1;
   if (!ph_mip_layout_init(&r->mip, templ->width0, templ->height0,
                           mip_levels, true))
      goto fail;
   /* CREATE_RENDER has a bounded height.  Retain the historical level-zero
    * allocation for an unusually large chain rather than rejecting an
    * otherwise usable texture; transfer maps for its lower levels become
    * harmless staging-only fallbacks. */
   if (mip_levels > 1 &&
       (r->mip.size + (size_t)ALIGN_POT(templ->width0, 64) * 4 - 1) /
       ((size_t)ALIGN_POT(templ->width0, 64) * 4) > 768) {
      mip_levels = 1;
      if (!ph_mip_layout_init(&r->mip, templ->width0, templ->height0,
                              mip_levels, true))
         goto fail;
   }
   struct drm_parhelia_create_render create = {
      .width = templ->width0,
      .height = mip_levels > 1 ?
         (r->mip.size + (size_t)ALIGN_POT(templ->width0, 64) * 4 - 1) /
         ((size_t)ALIGN_POT(templ->width0, 64) * 4) :
         templ->height0,
   };
   /* A tiled sampler uses the texture-unit address path.  Keeping the 32 MiB
    * SUN90 placement rule for it leaves only four possible origins in the
    * P690's 128 MiB VRAM and makes ordinary Q3 textures unpinnable.  Any
    * object that can be rendered to retains the conservative target class. */
   /* ioquake3 advertises static sampler images as render-target capable as
    * well (bind=0xa), although it never binds its 16..512px atlases as a
    * target.  Treat this measured sampler extent as texture storage; the
    * actual 640x480 colour/depth images remain in the bank-separated class. */
   if (sampler_storage && screen->chip_id == 0x2539 &&
       !screen->texture_upload) {
      fprintf(stderr,
              "parhelia: safe P690 texture upload ABI is unavailable\n");
      goto fail;
   }
   if (sampler_storage)
      create.flags = PARHELIA_CREATE_RENDER_TEXTURE;
   if (ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &create)) {
      fprintf(stderr, "parhelia: create render %ux%u failed: %s\n",
              templ->width0, templ->height0, strerror(errno));
      goto fail;
   }
   r->handle = create.handle;
   r->handle_owned = true;
   r->size = create.size;
   /* The DRM allocation ABI reports byte pitch, while the recovered surface
    * descriptor and p3d_tile_offset_layout() both consume pitch in pixels. */
   if ((create.pitch & 3) || create.pitch < templ->width0 * 4)
      goto fail;
   /* Layout 2 uses a power-of-two texel pitch, which can exceed the current
    * allocation ABI's 64-pixel pitch for NPOT images.  Reject any object
    * whose reported storage cannot contain the software-swizzled image. */
   if ((create.flags & PARHELIA_CREATE_RENDER_TEXTURE) &&
       create.size < r->mip.size)
      goto fail;
   r->stride = create.pitch / 4;
   r->mmap_offset = create.mmap_offset;
   r->upload_texture = screen->texture_upload &&
                       create.flags == PARHELIA_CREATE_RENDER_TEXTURE;
   if (r->upload_texture) {
      if (create.mmap_offset)
         goto fail;
      r->map = align_malloc(r->size, 64);
      if (!r->map)
         goto fail;
      memset(r->map, 0, r->size);
      r->upload_dirty = true;
   } else {
      r->map = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    ph_screen(pscreen)->fd, r->mmap_offset);
      if (r->map == MAP_FAILED) {
         r->map = NULL;
         goto fail;
      }
   }
   if (!share_native &&
       (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                       PIPE_BIND_SHARED | PIPE_BIND_SAMPLER_VIEW))) {
      /* CREATE_DUMB is a modeset-only ioctl and is unavailable through a
       * render node.  A second render allocation is valid linear sampler
       * storage when its descriptor selects texture layout 0; it also gives
       * the conservative non-aliasing placement required by this hardware. */
      if (!ph_mip_layout_init(&r->linear_mip, templ->width0, templ->height0,
                              mip_levels, false))
         goto fail;
      struct drm_parhelia_create_render linear = {
         .width = ALIGN_POT(templ->width0, 32),
         .height = mip_levels > 1 ?
            (r->linear_mip.size +
             (size_t)ALIGN_POT(templ->width0, 32) * 4 - 1) /
            ((size_t)ALIGN_POT(templ->width0, 32) * 4) :
            templ->height0,
         .flags = PARHELIA_CREATE_RENDER_LINEAR,
      };
      if (r->upload_texture) {
         r->linear_size = r->linear_mip.size;
         r->linear_stride = r->linear_mip.level[0].pitch * 4;
         r->linear_map = align_malloc(r->linear_size, 64);
         if (!r->linear_map)
            goto fail;
         memset(r->linear_map, 0, r->linear_size);
      } else {
         if (ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER,
                   &linear)) {
            fprintf(stderr,
                    "parhelia: create linear shadow %ux%u failed: %s\n",
                    linear.width, linear.height, strerror(errno));
            goto fail;
         }
         r->linear_handle = linear.handle;
         r->linear_size = linear.size;
         r->linear_stride = linear.pitch;
         if (r->linear_stride < ALIGN_POT(templ->width0, 32) * 4 ||
             (r->linear_stride & 3))
            goto fail;
         if (mip_levels > 1 && linear.size < r->linear_mip.size)
            goto fail;
         r->linear_handle_owned = true;
         r->linear_map = mmap(NULL, r->linear_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, ph_screen(pscreen)->fd,
                              linear.mmap_offset);
         if (r->linear_map == MAP_FAILED) {
            r->linear_map = NULL;
            goto fail;
         }
      }
   }
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: create resource=%p tiled=%u linear=%u %ux%u format=%u/%s bind=%#x create_flags=%#x tiled_pitch_px=%u linear_pitch_B=%u layout=%u\n",
              (void *)r, r->handle, r->linear_handle,
              r->base.width0, r->base.height0, r->base.format,
              util_format_description(r->base.format)->name, r->base.bind, create.flags,
              r->stride,
              r->linear_stride, r->tile_layout);
   return &r->base;
fail:
   if (r->upload_texture)
      align_free(r->linear_map);
   else if (r->linear_map)
      munmap(r->linear_map, r->linear_size);
   if (r->linear_handle_owned && r->linear_handle) {
      if (r->linear_is_dumb) {
         struct drm_mode_destroy_dumb dumb = {.handle = r->linear_handle};
         ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb);
      } else {
         struct drm_gem_close close = {.handle = r->linear_handle};
         ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
      }
   }
   if (r->handle) {
      struct drm_gem_close close = {.handle = r->handle};
      ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
   if (r->upload_texture)
      align_free(r->map);
   align_free(r->malloc_data);
   FREE(r);
   return NULL;
}

static struct pipe_resource *
ph_resource_from_handle(struct pipe_screen *pscreen,
                        const struct pipe_resource *templ,
                        struct winsys_handle *whandle, unsigned usage)
{
   struct parhelia_screen *screen = ph_screen(pscreen);
   struct drm_parhelia_get_bo_info info;
   struct pipe_resource render_templ = *templ;
   struct pipe_resource *pres;
   struct parhelia_resource *r;
   uint32_t imported_handle = 0;
   bool imported_owned = false;
   (void)usage;

   if (screen->native_share && !whandle->offset) {
      if (whandle->type == WINSYS_HANDLE_TYPE_FD) {
         if (drmPrimeFDToHandle(screen->fd, whandle->handle,
                                &imported_handle))
            return NULL;
         imported_owned = true;
      } else if (whandle->type == WINSYS_HANDLE_TYPE_SHARED) {
         struct drm_gem_open open = {.name = whandle->handle};
         if (ioctl(screen->fd, DRM_IOCTL_GEM_OPEN, &open))
            return NULL;
         imported_handle = open.handle;
         imported_owned = true;
      } else if (whandle->type == WINSYS_HANDLE_TYPE_KMS) {
         imported_handle = whandle->handle;
      }
      if (imported_handle && ph_query_bo(screen, imported_handle, &info) &&
          ph_bo_layout_native(screen, info.layout)) {
         if (!info.pitch || (info.pitch & 255) ||
             info.pitch < (uint64_t)ALIGN_POT(templ->width0, 64) * 4 ||
             info.width < templ->width0 || info.height < templ->height0) {
            if (imported_owned) {
               struct drm_gem_close close = {.handle = imported_handle};
               ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
            }
            return NULL;
         }
         r = CALLOC_STRUCT(parhelia_resource);
         if (!r) {
            if (imported_owned) {
               struct drm_gem_close close = {.handle = imported_handle};
               ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
            }
            return NULL;
         }
         r->linear_fd = -1;
         r->base = *templ;
         r->base.screen = pscreen;
         r->handle = imported_handle;
         r->handle_owned = imported_owned;
         r->size = info.size;
         r->stride = info.pitch / 4;
         r->tile_layout = screen->tile_layout;
         r->native_shared = true;
         pipe_reference_init(&r->base.reference, 1);
         if (getenv("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: import native resource=%p handle=%u %ux%u bind=%#x pitch_B=%u layout=%u\n",
                    (void *)r, r->handle, templ->width0, templ->height0,
                    templ->bind, info.pitch, info.layout);
         return &r->base;
      }
      if (imported_owned) {
         struct drm_gem_close close = {.handle = imported_handle};
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
      }
   }

   /* The imported image is linear presentation storage.  Allocate a native
    * tiled shadow for every renderable import and resolve explicitly.
    */
   /* Imported images need a renderable native backing regardless of whether
    * callers will later sample them.  They cannot use the sampler-only
    * malloc/upload class because the imported mmap replaces linear_map. */
   render_templ.bind &= ~(PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                          PIPE_BIND_SHARED | PIPE_BIND_SAMPLER_VIEW);
   pres = ph_resource_create(pscreen, &render_templ);
   if (!pres) {
      fprintf(stderr,
              "parhelia: import backing allocation failed type=%u format=%u %ux%u bind=%#x stride=%u\n",
              whandle->type, templ->format, templ->width0, templ->height0,
              templ->bind, whandle->stride);
      return NULL;
   }
   r = ph_resource(pres);
   r->base.bind = templ->bind;
   if (whandle->offset ||
       (whandle->modifier != DRM_FORMAT_MOD_INVALID &&
        whandle->modifier != DRM_FORMAT_MOD_LINEAR) ||
       whandle->stride < (uint64_t)templ->width0 * 4 ||
       (whandle->stride & 3)) {
      fprintf(stderr,
              "parhelia: reject non-linear/invalid import offset=%u modifier=%#llx stride=%u width=%u\n",
              whandle->offset, (unsigned long long)whandle->modifier,
              whandle->stride, templ->width0);
      goto fail;
   }
   r->linear_stride = whandle->stride;
   r->linear_size = (uint64_t)r->linear_stride * templ->height0;

   if (whandle->type == WINSYS_HANDLE_TYPE_FD) {
      r->linear_fd = fcntl(whandle->handle, F_DUPFD_CLOEXEC, 3);
      if (r->linear_fd < 0 ||
          drmPrimeFDToHandle(ph_screen(pscreen)->fd, whandle->handle,
                             &r->linear_handle))
         goto fail;
      r->linear_handle_owned = true;
      r->linear_map = mmap(NULL, r->linear_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, r->linear_fd, 0);
   } else if (whandle->type == WINSYS_HANDLE_TYPE_KMS) {
      struct drm_mode_map_dumb map = {.handle = whandle->handle};
      r->linear_handle = whandle->handle;
      if (ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
         goto fail;
      r->linear_map = mmap(NULL, r->linear_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, ph_screen(pscreen)->fd, map.offset);
   } else {
      fprintf(stderr, "parhelia: unsupported winsys handle type %u\n",
              whandle->type);
      goto fail;
   }
   if (r->linear_map == MAP_FAILED) {
      r->linear_map = NULL;
      goto fail;
   }
   r->linear_dirty = true;
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: import resource=%p tiled=%u linear=%u %ux%u bind=%#x type=%u tiled_pitch_px=%u linear_pitch_B=%u layout=%u\n",
              (void *)r, r->handle, r->linear_handle,
              r->base.width0, r->base.height0, r->base.bind, whandle->type,
              r->stride, r->linear_stride, r->tile_layout);
   return pres;
fail:
   ph_resource_destroy(pscreen, pres);
   return NULL;
}

static void
ph_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *pres)
{
   struct parhelia_resource *r = ph_resource(pres);
   if (r->upload_texture)
      align_free(r->linear_map);
   else if (r->linear_map)
      munmap(r->linear_map, r->linear_size);
   if (r->linear_fd >= 0)
      close(r->linear_fd);
   if (r->linear_handle_owned && r->linear_handle) {
      if (r->linear_is_dumb) {
         struct drm_mode_destroy_dumb dumb = {.handle = r->linear_handle};
         ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb);
      } else {
         struct drm_gem_close close = {.handle = r->linear_handle};
         ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
      }
   }
   if (r->upload_texture)
      align_free(r->map);
   else if (r->map)
      munmap(r->map, r->size);
   if (r->handle_owned && r->handle) {
      struct drm_gem_close close = {.handle = r->handle};
      ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
   align_free(r->malloc_data);
   FREE(r);
}

static bool
ph_resource_get_handle(struct pipe_screen *pscreen, struct pipe_context *ctx,
                       struct pipe_resource *pres, struct winsys_handle *whandle,
                       unsigned usage)
{
   struct parhelia_resource *r = ph_resource(pres);
   if (r->native_shared) {
      int prime_fd;

      if (getenv("PARHELIA_TRACE"))
         fprintf(stderr,
                 "parhelia: export native resource=%p handle=%u type=%u pitch_B=%u\n",
                 (void *)r, r->handle, whandle->type, r->stride * 4);
      whandle->stride = r->stride * 4;
      whandle->offset = 0;
      /* The matching KMS and Xorg driver query the GEM layout explicitly.
       * INVALID must never be inferred as LINEAR for a Parhelia dma-buf. */
      whandle->modifier = DRM_FORMAT_MOD_INVALID;
      if (whandle->type == WINSYS_HANDLE_TYPE_FD) {
         if (drmPrimeHandleToFD(ph_screen(pscreen)->fd, r->handle,
                                DRM_CLOEXEC | DRM_RDWR, &prime_fd))
            return false;
         whandle->handle = prime_fd;
         return true;
      }
      if (whandle->type == WINSYS_HANDLE_TYPE_KMS) {
         whandle->handle = r->handle;
         return true;
      }
      if (whandle->type == WINSYS_HANDLE_TYPE_SHARED) {
         struct drm_gem_flink flink = {.handle = r->handle};
         if (ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_FLINK, &flink))
            return false;
         whandle->handle = flink.name;
         return true;
      }
      return false;
   }
   if (!r->linear_handle)
      return false;
   ph_resource_resolve_linear(r);
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: export resource=%p tiled=%u linear=%u type=%u\n",
              (void *)r, r->handle, r->linear_handle, whandle->type);
   whandle->stride = r->linear_stride;
   whandle->offset = 0;
   whandle->modifier = DRM_FORMAT_MOD_INVALID;
   if (whandle->type == WINSYS_HANDLE_TYPE_FD)
   {
      int prime_fd;
      if (drmPrimeHandleToFD(ph_screen(pscreen)->fd, r->linear_handle,
                             DRM_CLOEXEC | DRM_RDWR, &prime_fd))
         return false;
      whandle->handle = prime_fd;
      return true;
   }
   if (whandle->type == WINSYS_HANDLE_TYPE_KMS) {
      whandle->handle = r->linear_handle;
      return true;
   }
   return false;
}

static void ph_destroy(struct pipe_screen *pscreen)
{
   close(ph_screen(pscreen)->fd);
   FREE(pscreen);
}

static void
ph_fence_reference(struct pipe_screen *screen, struct pipe_fence_handle **ptr,
                   struct pipe_fence_handle *fence)
{
   (void)screen;
   *ptr = fence;
}

static bool
ph_fence_finish(struct pipe_screen *screen, struct pipe_context *ctx,
                struct pipe_fence_handle *fence, uint64_t timeout)
{
   (void)screen;
   (void)ctx;
   (void)fence;
   (void)timeout;
   return true;
}

struct pipe_screen *
parhelia_screen_create(int fd, const struct pipe_screen_config *config)
{
   struct parhelia_screen *s = CALLOC_STRUCT(parhelia_screen);
   struct drm_parhelia_get_param chip = {.param = PARHELIA_PARAM_CHIP_ID};
   struct drm_parhelia_get_param tile = {.param = PARHELIA_PARAM_TILE_LAYOUT};
   struct drm_parhelia_get_param vram = {.param = PARHELIA_PARAM_VRAM_SIZE};
   struct drm_parhelia_get_param pipe_mode = {
      .param = PARHELIA_PARAM_TEXTURE_PIPE_MODE,
   };
   struct drm_parhelia_get_param vertex = {
      .param = PARHELIA_PARAM_VRAM_VERTEX,
   };
   struct drm_parhelia_get_param async = {
      .param = PARHELIA_PARAM_ASYNC_SUBMIT,
   };
   struct drm_parhelia_get_param bound_range = {
      .param = PARHELIA_PARAM_BOUND_RANGE,
   };
   struct drm_parhelia_get_param stateful = {
      .param = PARHELIA_PARAM_STATEFUL_SUBMIT,
   };
   struct drm_parhelia_get_param native_share = {
      .param = PARHELIA_PARAM_NATIVE_SHARE,
   };
   struct drm_parhelia_get_param texture_upload = {
      .param = PARHELIA_PARAM_TEXTURE_UPLOAD,
   };
   (void)config;
   if (!s)
      return NULL;
   s->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (s->fd < 0 || ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &chip) ||
       ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &tile) ||
       ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &vram) ||
       ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &pipe_mode))
      goto fail;
   switch (chip.value) {
   case 0x0527:
   case 0x0528:
      if (tile.value != PARHELIA_TILE_64X32_8K ||
          pipe_mode.value != PARHELIA_TEXTURE_PIPES_PAIRED)
         goto fail;
      break;
   case 0x2537:
   case 0x2538:
   case 0x2539:
      if (tile.value != PARHELIA_TILE_64X16_4K ||
          pipe_mode.value != PARHELIA_TEXTURE_PIPES_SINGLE)
         goto fail;
      break;
   default:
      goto fail;
   }
   s->chip_id = chip.value;
   s->tile_layout = tile.value;
   s->single_texture_pipe =
      pipe_mode.value == PARHELIA_TEXTURE_PIPES_SINGLE;
   s->vram_size = vram.value;
   /* This optional parameter lets new Mesa retain the coherent-DMA path on
    * older kernels. The P690-only class-0 path is never inferred from a PCI
    * id alone. */
   s->vram_vertex = !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &vertex) &&
                    vertex.value == 1;
   s->bound_range = s->vram_vertex &&
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &bound_range) &&
      bound_range.value == 1;
   /* Async submission is an optional queue contract.  An older staged module
    * returns EINVAL for this parameter, which deliberately retains the
    * complete synchronous path. */
   s->async_submit = !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &async) &&
                     async.value == 1;
   s->stateful_submit = s->async_submit &&
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &stateful) &&
      stateful.value == 1;
   s->native_share =
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &native_share) &&
      native_share.value == 1;
   s->texture_upload =
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &texture_upload) &&
      texture_upload.value == 1;
   /* u_init_pipe_screen_caps() queries DRM_CAP_PRIME through this callback.
    * Install it first so GBM uses the DRI image path for ARGB scanout rather
    * than its restricted dumb-buffer fallback. */
   s->base.get_screen_fd = ph_fd;
   u_init_pipe_screen_caps(&s->base, 1);
   s->base.nir_options[MESA_SHADER_VERTEX] = &ph_vs_nir_options;
   s->base.nir_options[MESA_SHADER_FRAGMENT] = &ph_fs_nir_options;
   struct pipe_caps *caps = (struct pipe_caps *)&s->base.caps;
   caps->max_render_targets = 1;
   caps->max_texture_2d_size = 1024;
   /* There is no pipe fence object yet.  Native window swaps are ordered by
    * the following Present blit, so the DRI frontend must not request a NULL
    * fence solely to throttle every swap in the client. */
   caps->throttle = false;
   caps->glsl_feature_level = caps->glsl_feature_level_compatibility = 120;
   caps->essl_feature_level = 100;
   caps->npot_textures = true;
   /* The back-end has separate RGB/alpha factors but one shared blend
    * equation, so Gallium must not expose the separate-equation extension. */
   caps->blend_equation_separate = false;
   caps->alpha_test = true;
   caps->user_vertex_buffers = true;
   caps->vertex_color_unclamped = true;
   caps->vertex_color_clamped = true;
   caps->fs_coord_origin_upper_left = caps->fs_coord_origin_lower_left = true;
   caps->fs_coord_pixel_center_half_integer = true;
   caps->constant_buffer_offset_alignment = 16;
   caps->min_map_buffer_alignment = 64;
   caps->vendor_id = 0x102b;
   caps->device_id = s->chip_id;
   caps->video_memory = MIN2(s->vram_size >> 20, UINT32_MAX);
   caps->endianness = PIPE_ENDIAN_NATIVE;
   caps->max_viewports = 1;
   caps->max_varyings = 8;
   caps->max_vertex_buffers = PIPE_MAX_ATTRIBS;
   caps->max_vertex_attrib_stride = 2048;
   draw_init_shader_caps((struct pipe_shader_caps *)&s->base.shader_caps[MESA_SHADER_VERTEX]);
   ((struct pipe_shader_caps *)&s->base.shader_caps[MESA_SHADER_VERTEX])->supported_irs =
      1 << PIPE_SHADER_IR_NIR;
   struct pipe_shader_caps *fs =
      (struct pipe_shader_caps *)&s->base.shader_caps[MESA_SHADER_FRAGMENT];
   fs->max_instructions = fs->max_alu_instructions = 64;
   fs->max_tex_instructions = fs->max_tex_indirections = 8;
   fs->max_inputs = 8;
   fs->max_outputs = 1;
   fs->max_const_buffer0_size = 16 * 4 * sizeof(float);
   fs->max_const_buffers = 1;
   fs->max_temps = 16;
   /* All supported chips expose at least two texture address units.  The
    * P690 executes them through one physical pipe, but still supports the
    * documented two-unit diffuse/lightmap state used by Quake III. */
   fs->max_texture_samplers = fs->max_sampler_views = 2;
   fs->supported_irs = 1 << PIPE_SHADER_IR_NIR;
   s->base.destroy = ph_destroy;
   s->base.context_create = ph_context_create;
   s->base.get_name = ph_name;
   s->base.get_vendor = ph_vendor;
   s->base.get_device_vendor = ph_device_vendor;
   s->base.is_format_supported = ph_format_supported;
   s->base.resource_create = ph_resource_create;
   s->base.resource_from_handle = ph_resource_from_handle;
   s->base.resource_get_handle = ph_resource_get_handle;
   s->base.resource_destroy = ph_resource_destroy;
   s->base.fence_reference = ph_fence_reference;
   s->base.fence_finish = ph_fence_finish;
   return &s->base;
fail:
   if (s->fd >= 0)
      close(s->fd);
   FREE(s);
   return NULL;
}
