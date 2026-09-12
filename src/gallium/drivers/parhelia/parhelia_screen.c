/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
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

/* Native sampler codes for 0x11e0[29:24], recovered from the vendor format
 * record table (Linux mtx1646 .text+0x7c050, sampler variants 2/3; the
 * channel masks that name each internal id come from mtx3404 .text+0x79c60
 * and agree with the Windows MTXPARD.dll 0x83d50 pixel-format classifier).
 *
 * The P690 texture unit never delivers a sampled alpha channel: a texel with
 * A=0 still paints RGB, on 32-bit 0x32 and on 16-bit 0x26 alike (measured,
 * docs/parhelia-p690-sampler-alpha-emission-re.md and the RGB5_A1 alpha
 * probe).  That limits every format equally, so it is not a reason to leave a
 * colour format out; it is the reason A8 (code 0x12) and A8L8 (0x22), whose
 * only information is alpha, are not listed. */
bool
ph_sampler_format(enum pipe_format format, struct ph_sampler_format *out)
{
   struct ph_sampler_format native;

   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      native = (struct ph_sampler_format){ 0x32, 4 };
      break;
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      native = (struct ph_sampler_format){ 0x33, 4 };
      break;
   case PIPE_FORMAT_B5G5R5A1_UNORM:
      native = (struct ph_sampler_format){ 0x26, 2 };
      break;
   case PIPE_FORMAT_B5G6R5_UNORM:
      native = (struct ph_sampler_format){ 0x28, 2 };
      break;
   case PIPE_FORMAT_B4G4R4A4_UNORM:
      native = (struct ph_sampler_format){ 0x24, 2 };
      break;
   case PIPE_FORMAT_L8_UNORM:
      native = (struct ph_sampler_format){ 0x11, 1 };
      break;
   /* S3TC: DXT1 RGB and RGBA share code 0x08, the endpoint order inside each
    * BC1 block selects punch-through alpha (MTXPARD.dll 0x83f80 maps the
    * FOURCCs, mtx1646 builds the records at .text+0x7e813 and following). */
   case PIPE_FORMAT_DXT1_RGB:
   case PIPE_FORMAT_DXT1_RGBA:
      native = (struct ph_sampler_format){ 0x08, 0, 8 };
      break;
   case PIPE_FORMAT_DXT3_RGBA:
      native = (struct ph_sampler_format){ 0x0a, 0, 16 };
      break;
   case PIPE_FORMAT_DXT5_RGBA:
      native = (struct ph_sampler_format){ 0x0c, 0, 16 };
      break;
   default:
      return false;
   }
   if (out)
      *out = native;
   return true;
}

/* Render-target and depth surface descriptors.  The 16-bit codes and the
 * 128x16 storage order they select are measured in
 * docs/parhelia-p690-16-bit-render-targets.md; every other renderable format
 * goes through the 32-bit A8R8G8B8 surface. */
bool
ph_narrow_target_format(enum pipe_format format)
{
   return format == PIPE_FORMAT_B5G6R5_UNORM ||
          format == PIPE_FORMAT_Z16_UNORM;
}

bool
ph_target_format(enum pipe_format format, uint32_t *code, unsigned *cpp)
{
   uint32_t c = 0x32;
   unsigned b = 4;
   bool native = true;

   switch (format) {
   case PIPE_FORMAT_B5G6R5_UNORM:
      c = 0x28; b = 2;
      break;
   case PIPE_FORMAT_Z16_UNORM:
      c = 0x2a; b = 2;
      break;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      c = 0x36;
      break;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      break;
   default:
      native = util_format_get_blocksize(format) == 4 &&
               !util_format_is_depth_or_stencil(format);
      break;
   }
   if (code)
      *code = c;
   if (cpp)
      *cpp = b;
   return native;
}

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
         case PIPE_FORMAT_B5G6R5_UNORM:
            /* Sampler code 0x28, and the same code as a render target: a
             * 3-D draw into one writes exact 5:6:5 and the 2-D fill clears
             * it, measured by --narrow-target-probe.  One resource cannot
             * hold both the sampler's and the render target's storage order,
             * and display or dma-buf sharing still needs the 32-bit linear
             * companion path. */
            if (bind & PIPE_BIND_RENDER_TARGET)
               supported = ph_screen(s)->narrow_targets &&
                           !(bind & (PIPE_BIND_SAMPLER_VIEW |
                                     PIPE_BIND_DEPTH_STENCIL |
                                     PIPE_BIND_SCANOUT |
                                     PIPE_BIND_DISPLAY_TARGET |
                                     PIPE_BIND_SHARED));
            else
               supported = ph_screen(s)->narrow_formats &&
                           (bind & PIPE_BIND_SAMPLER_VIEW) &&
                           !(bind & (PIPE_BIND_DEPTH_STENCIL |
                                     PIPE_BIND_SCANOUT |
                                     PIPE_BIND_DISPLAY_TARGET |
                                     PIPE_BIND_SHARED));
            break;
         case PIPE_FORMAT_Z16_UNORM:
            /* Native D16, code 0x2a: the depth test, rejection and write
             * were measured exact, and the stored value is round(d*65536). */
            supported = ph_screen(s)->narrow_targets &&
                        (bind & PIPE_BIND_DEPTH_STENCIL) &&
                        !(bind & (PIPE_BIND_RENDER_TARGET |
                                  PIPE_BIND_SAMPLER_VIEW |
                                  PIPE_BIND_SCANOUT |
                                  PIPE_BIND_DISPLAY_TARGET |
                                  PIPE_BIND_SHARED));
            break;
         case PIPE_FORMAT_B5G5R5A1_UNORM:
         case PIPE_FORMAT_B4G4R4A4_UNORM:
         case PIPE_FORMAT_L8_UNORM:
            /* Native 8- and 16-bit sampler storage (codes 0x26, 0x24, 0x11).
             * There is no recovered render-target, depth or display
             * descriptor at these texel sizes, so they stay sampler-only.
             * Their alpha is no worse off than the 32-bit formats': this
             * texture unit emits none for any of them. */
            supported = ph_screen(s)->narrow_formats &&
                        (bind & PIPE_BIND_SAMPLER_VIEW) &&
                        !(bind & (PIPE_BIND_RENDER_TARGET |
                                  PIPE_BIND_DEPTH_STENCIL |
                                  PIPE_BIND_SCANOUT |
                                  PIPE_BIND_DISPLAY_TARGET |
                                  PIPE_BIND_SHARED));
            break;
         case PIPE_FORMAT_B8G8R8A8_UNORM:
         case PIPE_FORMAT_B8G8R8X8_UNORM:
            supported = !(bind & PIPE_BIND_DEPTH_STENCIL);
            break;
         case PIPE_FORMAT_DXT1_RGB:
         case PIPE_FORMAT_DXT1_RGBA:
         case PIPE_FORMAT_DXT3_RGBA:
         case PIPE_FORMAT_DXT5_RGBA:
            /* Native S3TC, sampler-only.  The recovered uploader rounds the
             * macrotile row to align(width,32) while the descriptor pitch
             * helper uses the next power of two; those agree only for a
             * power-of-two width, so non-power-of-two compressed storage
             * stays off until it is measured. */
            supported = ph_screen(s)->s3tc &&
                        target == PIPE_TEXTURE_2D &&
                        (bind & PIPE_BIND_SAMPLER_VIEW) &&
                        !(bind & (PIPE_BIND_RENDER_TARGET |
                                  PIPE_BIND_DEPTH_STENCIL |
                                  PIPE_BIND_SCANOUT |
                                  PIPE_BIND_DISPLAY_TARGET |
                                  PIPE_BIND_SHARED));
            break;
         case PIPE_FORMAT_Z24_UNORM_S8_UINT:
            supported = !(bind & PIPE_BIND_RENDER_TARGET);
            break;
         case PIPE_FORMAT_S8_UINT_Z24_UNORM:
            /* Native 0x36 depth is the low 24 bits; this order has no
             * depth/stencil surface descriptor. */
            supported = !(bind & (PIPE_BIND_RENDER_TARGET |
                                  PIPE_BIND_DEPTH_STENCIL));
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
   if (ph_env("PARHELIA_DEBUG"))
      fprintf(stderr,
              "parhelia: format query format=%u target=%u samples=%u/%u bind=%#x -> %u\n",
              format, target, samples, storage_samples, bind, supported);
   return supported;
}

void
ph_resource_resolve_linear(struct parhelia_resource *r)
{
   struct parhelia_batch *batch = NULL;
   uint32_t fence;
   int ret = -EINVAL;

   if (!r || !r->tiled_dirty || !r->linear_map)
      return;
   if (!ph_env("PARHELIA_CPU_BLIT") &&
       r->handle && r->linear_handle && !(r->linear_stride % r->cpp) &&
       r->linear_stride / r->cpp <= 0x3fff) {
      batch = calloc(1, sizeof(*batch));
      if (batch)
         ret = parhelia_batch_blit_format(batch,
                                r->linear_handle, r->linear_stride / r->cpp,
                                r->surf_fmt, 1, 0, 0,
                                r->handle, r->stride, r->surf_fmt, 2, 0, 0,
                                r->base.width0, r->base.height0);
      if (!ret)
         ret = parhelia_batch_submit(ph_screen(r->base.screen)->fd, batch,
                                     NULL, 0, &fence);
      if (!ret) {
         free(batch);
         r->tiled_dirty = false;
         r->linear_dirty = false;
         if (ph_env("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: GPU resolve tiled=%u linear=%u %ux%u\n",
                    r->handle, r->linear_handle,
                    r->base.width0, r->base.height0);
         return;
      }
      free(batch);
   }
   if (!r->map || (r->transfer_shadow && ph_resource_download_shadow(r)))
      return;
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: CPU resolve resource=%p tiled=%u linear=%u %ux%u gpu_error=%d\n",
              (void *)r, r->handle, r->linear_handle,
              r->base.width0, r->base.height0, ret);
   for (unsigned y = 0; y < r->base.height0; y++)
      for (unsigned x = 0; x < r->base.width0; x++)
         memcpy((uint8_t *)r->linear_map + y * r->linear_stride + x * r->cpp,
                (uint8_t *)r->map +
                   p3d_tile_offset_layout_cpp(x, y, r->stride, r->tile_layout,
                                              r->cpp), r->cpp);
   r->tiled_dirty = false;
   r->linear_dirty = false;
}

void
ph_resource_upload_tiled(struct parhelia_resource *r)
{
   struct parhelia_batch *batch = NULL;
   const unsigned cpp = r && r->mip.cpp ? r->mip.cpp : 4;
   uint32_t fence;
   int ret = -EINVAL;

   if (!r || !r->linear_dirty || r->tiled_dirty || !r->linear_map)
      return;
   /* A sampler image with a narrower texel has its own storage order and
    * needs the CPU; a 16 bpp render target does not. */
   if (cpp == r->cpp && !ph_env("PARHELIA_CPU_BLIT") &&
       r->handle && r->linear_handle && !(r->linear_stride % r->cpp) &&
       r->linear_stride / r->cpp <= 0x3fff) {
      batch = calloc(1, sizeof(*batch));
      if (batch)
         ret = parhelia_batch_blit_format(batch, r->handle, r->stride,
                                r->surf_fmt, 2, 0, 0,
                                r->linear_handle, r->linear_stride / r->cpp,
                                r->surf_fmt, 1, 0, 0,
                                r->base.width0, r->base.height0);
      if (!ret)
         ret = parhelia_batch_submit(ph_screen(r->base.screen)->fd, batch,
                                     NULL, 0, &fence);
      if (!ret) {
         free(batch);
         r->linear_dirty = false;
         if (ph_env("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: GPU upload linear=%u tiled=%u %ux%u\n",
                    r->linear_handle, r->handle,
                    r->base.width0, r->base.height0);
         return;
      }
      free(batch);
   }
   if (!r->map)
      return;
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: CPU upload resource=%p linear=%u tiled=%u %ux%u layout=%u gpu_error=%d\n",
              (void *)r, r->linear_handle, r->handle,
              r->base.width0, r->base.height0, r->tile_layout, ret);
   /* A sampler image moves texels of its own size; a render target moves
    * the surface's. */
   const unsigned texel = r->upload_texture ? cpp : r->cpp;

   for (unsigned y = 0; y < r->base.height0; y++)
      for (unsigned x = 0; x < r->base.width0; x++)
         memcpy((uint8_t *)r->map +
                   (r->upload_texture ?
                    (r->mip.native_64x16_32 ?
                     p3d_texture_tile_offset_64x16(x, y,
                                                   r->mip.level[0].pitch) :
                     p3d_texture_tile_offset_cpp(x, y,
                                                 r->mip.level[0].pitch, cpp)) :
                    p3d_tile_offset_layout_cpp(x, y, r->stride,
                                               r->tile_layout, r->cpp)),
                (uint8_t *)r->linear_map +
                   (size_t)y * r->linear_stride + x * texel, texel);
   if (r->transfer_shadow && ph_resource_upload_shadow(r))
      return;
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
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr, "parhelia: texture upload handle=%u size=%llu\n",
              r->handle, (unsigned long long)r->size);
   /* PARHELIA_VERIFY_UPLOAD=1 reads every texture upload back. */
   if (ph_env("PARHELIA_VERIFY_UPLOAD")) {
      uint32_t *back = malloc(r->size);
      struct drm_parhelia_upload_render download = upload;
      const uint32_t *want = r->map;
      size_t first = SIZE_MAX, bad = 0;

      download.data = (uintptr_t)back;
      if (back && !ioctl(ph_screen(r->base.screen)->fd,
                         DRM_IOCTL_PARHELIA_DOWNLOAD_RENDER, &download)) {
         for (size_t i = 0; i < r->size / 4; i++)
            if (back[i] != want[i]) {
               first = MIN2(first, i);
               bad++;
            }
         if (bad)
            fprintf(stderr, "parhelia: texture %ux%u upload handle=%u: %zu/%llu dwords differ from byte %#zx\n",
                    r->base.width0, r->base.height0, r->handle, bad,
                    (unsigned long long)r->size / 4, first * 4);
      }
      free(back);
   }
   return 0;
}

static int
ph_resource_shadow_transfer(struct parhelia_resource *r, unsigned long request,
                            const char *operation, uint64_t offset,
                            uint64_t size)
{
   struct drm_parhelia_upload_render transfer;

   if (!r || !r->transfer_shadow || !r->handle || !r->map || !r->size ||
       offset > r->size || size > r->size - offset)
      return -EINVAL;
   if (!size)
      return 0;
   transfer = (struct drm_parhelia_upload_render) {
      .data = (uintptr_t)r->map + offset,
      .offset = offset,
      .size = size,
      .handle = r->handle,
   };
   if (ioctl(ph_screen(r->base.screen)->fd, request, &transfer)) {
      int ret = -errno;

      fprintf(stderr, "parhelia: %s handle=%u offset=%llu size=%llu failed: %s\n",
              operation, r->handle, (unsigned long long)offset,
              (unsigned long long)size, strerror(errno));
      return ret;
   }
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr, "parhelia: %s handle=%u offset=%llu size=%llu\n",
              operation, r->handle, (unsigned long long)offset,
              (unsigned long long)size);
   return 0;
}

/* Image rows [y0, y1) of a render-target BO live in one contiguous byte
 * range: their whole tile rows. */
static void
ph_shadow_row_range(const struct parhelia_resource *r, unsigned y0,
                    unsigned y1, uint64_t *offset, uint64_t *size)
{
   const unsigned th = r->tile_layout == PARHELIA_TILE_64X16_4K ? 16 : 32;
   const uint64_t tile_row = (uint64_t)r->stride * th * MAX2(r->cpp, 1u);
   uint64_t end = MIN2(((uint64_t)y1 + th - 1) / th * tile_row, r->size);

   *offset = MIN2((uint64_t)(y0 / th) * tile_row, end);
   *size = end - *offset;
}

int
ph_resource_upload_shadow(struct parhelia_resource *r)
{
   return ph_resource_shadow_transfer(r, DRM_IOCTL_PARHELIA_UPLOAD_RENDER,
                                      "render-shadow upload", 0,
                                      r ? r->size : 0);
}

int
ph_resource_download_shadow(struct parhelia_resource *r)
{
   return ph_resource_shadow_transfer(r, DRM_IOCTL_PARHELIA_DOWNLOAD_RENDER,
                                      "render-shadow download", 0,
                                      r ? r->size : 0);
}

int
ph_resource_upload_shadow_rows(struct parhelia_resource *r, unsigned y0,
                               unsigned y1)
{
   uint64_t offset, size;

   ph_shadow_row_range(r, y0, y1, &offset, &size);
   return ph_resource_shadow_transfer(r, DRM_IOCTL_PARHELIA_UPLOAD_RENDER,
                                      "render-shadow upload", offset, size);
}

int
ph_resource_download_shadow_rows(struct parhelia_resource *r, unsigned y0,
                                 unsigned y1)
{
   uint64_t offset, size;

   ph_shadow_row_range(r, y0, y1, &offset, &size);
   return ph_resource_shadow_transfer(r, DRM_IOCTL_PARHELIA_DOWNLOAD_RENDER,
                                      "render-shadow download", offset, size);
}

static struct pipe_resource *
ph_resource_create(struct pipe_screen *pscreen, const struct pipe_resource *templ)
{
   struct parhelia_screen *screen = ph_screen(pscreen);
   struct parhelia_resource *r = CALLOC_STRUCT(parhelia_resource);
   const bool share_native = screen->native_share &&
      (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                      PIPE_BIND_SHARED));
   /* Bytes per pixel of the render/depth surface this format would use. */
   const bool narrow_target = ph_narrow_target_format(templ->format);
   /* Mesa puts PIPE_BIND_SAMPLER_VIEW on every renderbuffer.  A two-byte
    * surface has different sampler and render-target storage orders, so a
    * render target has to take the render one; ph_format_supported refuses
    * the ambiguous sampler+target combination outright. */
   const bool sampler_storage = !share_native &&
      (templ->bind & PIPE_BIND_SAMPLER_VIEW) &&
      templ->width0 <= 512 && templ->height0 <= 512 &&
      !(templ->bind & PIPE_BIND_DEPTH_STENCIL) &&
      !(narrow_target && (templ->bind & PIPE_BIND_RENDER_TARGET));
   struct ph_sampler_format native_format = { 0x32, 4, 0 };
   const bool native_storage = sampler_storage &&
      ph_sampler_format(templ->format, &native_format);
   const unsigned block_bytes = native_storage ? native_format.block_bytes : 0;
   const unsigned cpp = native_storage && !block_bytes ? native_format.cpp : 4;
   const bool p690_native_texture32 = sampler_storage && !block_bytes &&
      p3d_texture_uses_native_64x16(screen->chip_id, screen->tile_layout, cpp);
   const bool p690_narrow = sampler_storage && !block_bytes &&
      p3d_texture_uses_p690_narrow(screen->chip_id, screen->tile_layout, cpp);
   const bool native_depth = screen->native_rt_classes &&
      (templ->bind & PIPE_BIND_DEPTH_STENCIL);
   const bool native_color = screen->native_rt_classes && !sampler_storage &&
      (templ->bind & PIPE_BIND_RENDER_TARGET);
   /* Bytes per pixel of the render/depth surface.  A sampler-only image keeps
    * its texel size; everything else follows the surface descriptor. */
   uint32_t target_code = 0x32;
   unsigned target_cpp = 4;
   /* CREATE_RENDER and the surface/texture descriptors all accept 2048 in
    * either dimension.  Keep resource creation consistent with the advertised
    * GL/DRI limit; the old 1024x768 userspace guard was a bring-up limit and
    * rejected otherwise valid large textures and render targets. */
   const unsigned max_width = 2048;
   const unsigned max_height = 2048;
   unsigned mip_levels;
   if (!r)
      return NULL;
   r->linear_fd = -1;
   r->base = *templ;
   r->base.screen = pscreen;
   r->tile_layout = screen->tile_layout;
   r->native_shared = share_native;
   if (!sampler_storage && !share_native)
      ph_target_format(templ->format, &target_code, &target_cpp);
   /* Sampler storage keeps its own 32-bit row ABI and mip.cpp; only a
    * render or depth surface changes the meaning of r->stride. */
   r->cpp = sampler_storage ? 4 : target_cpp;
   r->surf_fmt = target_code;
   pipe_reference_init(&r->base.reference, 1);
   if (templ->target == PIPE_BUFFER) {
      r->size = templ->width0;
      r->malloc_data = align_malloc(r->size, 64);
      if (!r->malloc_data)
         goto fail;
      return &r->base;
   }
   /* A compressed image only has a native block layout; there is no 32-bit
    * fallback storage for one. */
   if (templ->width0 > max_width || templ->height0 > max_height ||
       templ->depth0 != 1 ||
       (util_format_is_compressed(templ->format) && !block_bytes) ||
       templ->array_size != 1) {
      fprintf(stderr,
              "parhelia: reject resource target=%u format=%u %ux%ux%u array=%u levels=%u bind=%#x\n",
              templ->target, templ->format, templ->width0, templ->height0,
              templ->depth0, templ->array_size, templ->last_level + 1,
              templ->bind);
      goto fail;
   }
   /* P690 uses the common vendor chain geometry with a native 64x16 base,
    * compact 32x32 child, and a shared <=16 native 64x16 tail.  The
    * texture-only upload BO is required: an ordinary mapped render BO
    * selects the wrong storage class for mip sampling. */
   mip_levels = p690_native_texture32 &&
                !(templ->width0 & (templ->width0 - 1)) &&
                !(templ->height0 & (templ->height0 - 1)) ?
                   templ->last_level + 1 : 1;
   if (!ph_mip_layout_init_cpp(&r->mip, templ->width0, templ->height0,
                               mip_levels, true, p690_native_texture32, cpp))
      goto fail;
   /* A compressed image is stored as 4x4 blocks in 32x16-texel macrotiles.
    * Its level pitch stays a texel count; only the byte size differs. */
   if (block_bytes) {
      /* The vendor uploader rounds the macrotile row to align(width,32)
       * while the descriptor pitch is the next power of two; they agree only
       * for a power-of-two width. */
      if ((templ->width0 & (templ->width0 - 1)) ||
          (templ->height0 & (templ->height0 - 1)))
         goto fail;
      r->mip.block_bytes = block_bytes;
      r->mip.level[0].pitch = MAX2(32u, ph_mip_next_pot(templ->width0));
      r->mip.size = ph_mip_align(ph_dxt_level_size(templ->width0,
                                                   templ->height0,
                                                   block_bytes),
                                 block_bytes == 8 ? 0x800u : 0x1000u);
   }
   /* The 8/16-bit order addresses 64x32-texel tiles but takes its pitch from
    * the descriptor width, so a 32-texel image keeps a 32-texel pitch and
    * shares each tile between two row bands. */
   if (p690_narrow) {
      r->mip.p690_narrow = true;
      r->mip.size = ph_mip_align(r->mip.level[0].pitch *
                                 ph_mip_align(templ->height0, 32) * cpp,
                                 0x2000);
   }
   if (p690_native_texture32) {
      /* Live spatial sampling proves the hybrid address permutations used
       * for this vendor chain; ph_mip_physical_coords() selects each one. */
      r->mip.native_64x16_32 = true;
      /* A base up to 32 texels wide uses the compact 32-pitch permutation
       * of a 32x32 child (--mip-address-probe 32 32, 16 32, 32 64). */
      if (templ->width0 > 32)
         r->mip.level[0].pitch = MAX2(r->mip.level[0].pitch, 64u);
      if (mip_levels == 1)
         r->mip.size = ph_mip_align(r->mip.level[0].pitch *
                                   ph_mip_align(templ->height0, 32) * 4,
                                   0x2000);
   }
   /* CREATE_RENDER has a bounded height.  Retain the historical level-zero
    * allocation for an unusually large chain rather than rejecting an
    * otherwise usable texture; transfer maps for its lower levels become
    * harmless staging-only fallbacks. */
   if (mip_levels > 1 &&
       (r->mip.size + (size_t)r->mip.level[0].pitch * 4 - 1) /
       ((size_t)r->mip.level[0].pitch * 4) > 768) {
      mip_levels = 1;
      if (!ph_mip_layout_init_chip(&r->mip, templ->width0, templ->height0,
                                   mip_levels, true, p690_native_texture32))
         goto fail;
      if (p690_native_texture32) {
         r->mip.native_64x16_32 = true;
         if (templ->width0 > 32)
            r->mip.level[0].pitch = MAX2(r->mip.level[0].pitch, 64u);
         r->mip.size = ph_mip_align(r->mip.level[0].pitch *
                                   ph_mip_align(templ->height0, 32) * 4,
                                   0x2000);
      }
   }
   /* The allocation ABI only describes 32-bit texture rows.  A chain, or an
    * 8/16-bit image whose byte size no longer follows width0*height0*4, asks
    * for the rows of that wider shape which cover its storage. */
   const unsigned create_pitch = MAX2(r->mip.level[0].pitch, 64u);
   struct drm_parhelia_create_render create = {
      .width = p690_native_texture32 ? MAX2(templ->width0, 64u) :
               cpp != 4 || block_bytes ? create_pitch : templ->width0,
      /* Rows of the (possibly wider) allocation that hold the chain. */
      .height = mip_levels > 1 || cpp != 4 || block_bytes ?
         (r->mip.size + (size_t)create_pitch * 4 - 1) /
         ((size_t)create_pitch * 4) :
         templ->height0,
      /* The sampler classes keep their own 32-bit row ABI; only a native
       * render or depth surface allocates at two bytes per pixel.  Zero is
       * the 32-bit default, which an older kernel also accepts. */
      .cpp = !sampler_storage && target_cpp == 2 ? 2 : 0,
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
   else if (native_depth)
      create.flags = PARHELIA_CREATE_RENDER_NATIVE_DEPTH;
   else if (native_color)
      create.flags = PARHELIA_CREATE_RENDER_NATIVE_COLOR;
   /* The X front buffer is the one front-rendered scanout image: its
    * display refresh belongs in bank B, away from colour and textures. */
   if (native_color &&
       (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_USE_FRONT_RENDERING)) ==
       (PIPE_BIND_SCANOUT | PIPE_BIND_USE_FRONT_RENDERING)) {
      create.flags = PARHELIA_CREATE_RENDER_NATIVE_SCANOUT;
      if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &create)) {
         create.flags = PARHELIA_CREATE_RENDER_NATIVE_COLOR;
         create.handle = create.pitch = 0;
         create.size = create.mmap_offset = 0;
      }
   }
   if (!create.handle &&
       ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &create)) {
      fprintf(stderr, "parhelia: create render %ux%u failed: %s\n",
              templ->width0, templ->height0, strerror(errno));
      goto fail;
   }
   r->handle = create.handle;
   r->handle_owned = true;
   r->size = create.size;
   /* The DRM allocation ABI reports byte pitch, while the recovered surface
    * descriptor and p3d_tile_offset_layout() both consume pitch in pixels. */
   if ((create.pitch & 3) || create.pitch < templ->width0 * r->cpp ||
       create.pitch % r->cpp)
      goto fail;
   /* Layout 2 uses a power-of-two texel pitch, which can exceed the current
    * allocation ABI's 64-pixel pitch for NPOT images.  Reject any object
    * whose reported storage cannot contain the software-swizzled image. */
   if ((create.flags & PARHELIA_CREATE_RENDER_TEXTURE) &&
       create.size < r->mip.size)
      goto fail;
   r->stride = create.pitch / r->cpp;
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
   } else if (native_depth || native_color) {
      /* Compact P690 target classes live above the raw 16-MiB host window.
       * Their zero mmap offset is intentional.  Imported/present resources
       * which already have a CPU-visible linear companion use GPU BITBLT;
       * direct CPU transfers on a private class target remain unavailable. */
      if (create.mmap_offset)
         goto fail;
      if (screen->render_transfer) {
         r->map = align_malloc(r->size, 64);
         if (!r->map)
            goto fail;
         memset(r->map, 0, r->size);
         r->transfer_shadow = true;
      }
   } else {
      if (!r->mmap_offset)
         goto fail;
      r->map = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    ph_screen(pscreen)->fd, r->mmap_offset);
      if (r->map == MAP_FAILED) {
         r->map = NULL;
         goto fail;
      }
   }
   /* A compressed image has no linear companion: layout 5 is the only
    * storage the sampler decodes, and nothing detiles blocks.  Neither has a
    * 16 bpp render or depth surface: it is never shared or sampled, and its
    * CPU image is the byte-exact native shadow. */
   if (!share_native && !block_bytes && r->cpp == 4 &&
       (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                       PIPE_BIND_SHARED | PIPE_BIND_SAMPLER_VIEW))) {
      /* CREATE_DUMB is a modeset-only ioctl and is unavailable through a
       * render node.  A second render allocation is valid linear sampler
       * storage when its descriptor selects texture layout 0; it also gives
       * the conservative non-aliasing placement required by this hardware. */
      if (!ph_mip_layout_init_cpp(&r->linear_mip, templ->width0,
                                  templ->height0, mip_levels, false, false,
                                  cpp))
         goto fail;
      struct drm_parhelia_create_render linear = {
         .width = ALIGN_POT(templ->width0, 32),
         .height = mip_levels > 1 || cpp != 4 ?
            (r->linear_mip.size +
             (size_t)r->linear_mip.level[0].pitch * 4 - 1) /
            ((size_t)r->linear_mip.level[0].pitch * 4) :
            templ->height0,
         .flags = PARHELIA_CREATE_RENDER_LINEAR,
      };
      if (r->upload_texture) {
         r->linear_size = r->linear_mip.size;
         r->linear_stride = r->linear_mip.level[0].pitch * cpp;
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
         if ((mip_levels > 1 || cpp != 4) &&
             linear.size < r->linear_mip.size)
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
   if (ph_env("PARHELIA_TRACE"))
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
   if (r->upload_texture || r->transfer_shadow)
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
         /* GET_BO_INFO reports no pixel size; every shared native surface
          * is 32-bit, which ph_format_supported also enforces. */
         r->cpp = 4;
         r->surf_fmt = 0x32;
         r->stride = info.pitch / 4;
         r->tile_layout = screen->tile_layout;
         r->native_shared = true;
         pipe_reference_init(&r->base.reference, 1);
         if (ph_env("PARHELIA_TRACE"))
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
   if (ph_env("PARHELIA_TRACE"))
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
   if (r->premul_rgb_handle) {
      struct drm_gem_close close = {.handle = r->premul_rgb_handle};
      ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
   if (r->alpha_rgb_handle) {
      struct drm_gem_close close = {.handle = r->alpha_rgb_handle};
      ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
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
   if (r->upload_texture || r->transfer_shadow)
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

      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr,
                 "parhelia: export native resource=%p handle=%u type=%u pitch_B=%u\n",
                 (void *)r, r->handle, whandle->type, r->stride * 4);
      whandle->stride = r->stride * r->cpp;
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
   if (ph_env("PARHELIA_TRACE"))
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
   struct drm_parhelia_get_param async_slots = {
      .param = PARHELIA_PARAM_ASYNC_SLOTS,
   };
   struct drm_parhelia_get_param pack_adjacent = {
      .param = PARHELIA_PARAM_PACK_ADJACENT_BOUNDARIES,
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
   struct drm_parhelia_get_param native_rt_classes = {
      .param = PARHELIA_PARAM_NATIVE_RT_CLASSES,
   };
   struct drm_parhelia_get_param render_transfer = {
      .param = PARHELIA_PARAM_RENDER_TRANSFER,
   };
   struct drm_parhelia_get_param max_packets = {
      .param = PARHELIA_PARAM_MAX_PACKETS,
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
    * older kernels. The P690-only CREATE_VERTEX path is never inferred from
    * a PCI id alone. */
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
   if (s->async_submit &&
       !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &async_slots) &&
       async_slots.value > 0 && async_slots.value <= PH_VRAM_VERTEX_SLOTS_MAX)
      s->async_slots = async_slots.value;
   s->pack_adjacent_packets = s->async_submit &&
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &pack_adjacent) &&
      pack_adjacent.value == 1;
   s->stateful_submit = s->async_submit &&
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &stateful) &&
      stateful.value == 1;
   s->native_share =
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &native_share) &&
      native_share.value == 1;
   s->texture_upload =
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &texture_upload) &&
      texture_upload.value == 1;
   s->native_rt_classes =
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &native_rt_classes) &&
      native_rt_classes.value == 1;
   s->render_transfer =
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &render_transfer) &&
      render_transfer.value == 1;
   /* Native 8- and 16-bit sampler storage is on where its texel order has
    * been measured: parhelia-drm-test --texture-format-probe reproduces every
    * texel of L8, A1R5G5B5, R5G6B5 and A4R4G4B4 over two tile rows and two
    * tile columns on the P690.  PARHELIA_NO_NARROW_FORMATS=1 opts out.
    * Native S3TC is implemented but has not retired on hardware. */
   s->narrow_formats = s->texture_upload &&
      p3d_texture_uses_p690_narrow(s->chip_id, s->tile_layout, 2) &&
      !ph_env("PARHELIA_NO_NARROW_FORMATS");
   s->s3tc = s->texture_upload && ph_env("PARHELIA_S3TC");
   /* 16-bit render targets: ask the kernel for a two-byte-per-pixel native
    * colour allocation.  Only a kernel that knows the 128-pixel tile row can
    * place one, and only the P690's measured order is implemented.
    * PARHELIA_NO_NARROW_TARGETS=1 opts out. */
   if (s->native_rt_classes && s->render_transfer &&
       s->tile_layout == PARHELIA_TILE_64X16_4K &&
       !ph_env("PARHELIA_NO_NARROW_TARGETS")) {
      struct drm_parhelia_create_render probe = {
         .flags = PARHELIA_CREATE_RENDER_NATIVE_COLOR,
         .width = 128, .height = 16, .cpp = 2,
      };

      if (!ioctl(s->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &probe) &&
          probe.pitch == 256) {
         struct drm_gem_close close_bo = {.handle = probe.handle};

         s->narrow_targets = true;
         ioctl(s->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      }
   }
   if (!ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &max_packets) &&
       max_packets.value && max_packets.value <= PH_BATCH_PACKETS)
      s->kernel_max_packets = max_packets.value;
   /* u_init_pipe_screen_caps() queries DRM_CAP_PRIME through this callback.
    * Install it first so GBM uses the DRI image path for ARGB scanout rather
    * than its restricted dumb-buffer fallback. */
   s->base.get_screen_fd = ph_fd;
   u_init_pipe_screen_caps(&s->base, 1);
   s->base.nir_options[MESA_SHADER_VERTEX] = &ph_vs_nir_options;
   s->base.nir_options[MESA_SHADER_FRAGMENT] = &ph_fs_nir_options;
   struct pipe_caps *caps = (struct pipe_caps *)&s->base.caps;
   /* Both GEM handle import/export callbacks are implemented and the kernel
    * reports the two PRIME directions.  Keep the DRI/GBM contract explicit:
    * without this capability GBM falls back to CREATE_DUMB, which cannot
    * create the ARGB rendering+scanout front buffer requested by glamor. */
   caps->dmabuf = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
   caps->max_render_targets = 1;
   /* Vendor ranges: aliased 1..2048, smooth 0.25..10 in 0.25 steps.  Sized
    * points and wide or smooth lines are expanded to triangles, so smooth
    * sizes below one pixel have no coverage path and start at 1.0. */
   caps->min_point_size = caps->min_point_size_aa = 1.0f;
   caps->min_line_width = caps->min_line_width_aa = 1.0f;
   caps->max_point_size = caps->max_line_width = 2048.0f;
   caps->max_point_size_aa = caps->max_line_width_aa = 10.0f;
   caps->point_size_granularity = caps->line_width_granularity = 0.25f;
   /* The PLNWT solid fill takes an arbitrary rectangle and bit-plane mask;
    * partial-tile fills are hardware-verified only on the P690 64x16 layout. */
   caps->clear_scissored = caps->clear_masked =
      s->tile_layout == PARHELIA_TILE_64X16_4K;
   /* Native shared render/scanout images support the chip's full 2048-pixel
    * surface extent.  DRI rejects a GBM image before resource_create() when
    * either dimension exceeds this cap, so reporting 1024 prevented glamor
    * from creating a 1920x1200 front buffer despite that path being valid. */
   caps->max_texture_2d_size = 2048;
   /* There is no pipe fence object yet.  Native window swaps are ordered by
    * the following Present blit, so the DRI frontend must not request a NULL
    * fence solely to throttle every swap in the client. */
   caps->throttle = false;
   /* The fragment pipeline evaluates only what the combiner can express, and
    * there is no software fragment fallback: a shader the driver cannot lower
    * is still rendered by the hardware, with the wrong algebra.  Advertising
    * GLSL 1.20 is therefore only defensible once the data-flow compiler is
    * on; with the instruction-shape matcher alone, 24 of the 27 checks in
    * tools/parhelia-gl-combiner-test render the wrong colour.  Report 1.10
    * by default so applications that can choose a fixed-function path do. */
   caps->glsl_feature_level = caps->glsl_feature_level_compatibility =
      ph_env("PARHELIA_FS_COMPILER") ? 120 : 110;
   caps->essl_feature_level = 100;
   caps->npot_textures = true;
   caps->anisotropic_filter = true;
   caps->max_texture_anisotropy = 2.0f;
   /* Match the vendor OpenGL driver's advertised limit; the register field
    * itself remains safely clamped over its wider recovered +/-4 range. */
   caps->max_texture_lod_bias = 2.0f;
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
