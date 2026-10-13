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
#include "parhelia_texel.h"

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
   /* The combiner reads COL0/COL1 as fixed RGBA interpolators. */
   .io_options = nir_io_dont_compact_color_varyings,
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
   .lower_fpow = true,
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
   /* GigaColor, 2:10:10:10.  The record table gives internal formats 0x0f and
    * 0x10 the same hardware code 0x34 for the sampler variants (usage 2 and 3)
    * and for the render target (usage 1); the two differ only in the channel
    * order the vendor's converter produces, not in the descriptor.  See
    * docs/parhelia-p690-gigacolor-10bit.md. */
   case PIPE_FORMAT_B10G10R10A2_UNORM:
      native = (struct ph_sampler_format){ 0x34, 4 };
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
   case PIPE_FORMAT_B10G10R10A2_UNORM:
      c = 0x34;
      break;
   case PIPE_FORMAT_Z16_UNORM:
      c = 0x2a; b = 2;
      break;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_Z24X8_UNORM:
      c = 0x36;
      break;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_R8_UNORM:      /* widened to the 32-bit texel */
   case PIPE_FORMAT_R8G8_UNORM:
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

/* Native 2:10:10:10 sampling.  A ten-bit texel is four bytes, so the image
 * would share the measured 32-bit storage order and the kernel's ordinary
 * texture allocation; only the descriptor's format field differs, and the
 * vendor record table gives sampler and render target the same code 0x34
 * (docs/parhelia-p690-gigacolor-10bit.md §1).
 *
 * It stays OFF by default: the address unit's acceptance of code 0x34 has not
 * been read back from this silicon, and a descriptor it rejects stops the
 * engine retiring rather than failing the draw - storage order 0 with code
 * 0x34 wedged the P690 and cost a reboot.  PARHELIA_GIGACOLOR=1 opts in for
 * `parhelia-drm-test --gigacolor-texture-probe` follow-up work.  There is no
 * screen flag because nothing but the format field is negotiated with the
 * kernel. */
static bool
ph_gigacolor_sampler(const struct pipe_screen *s)
{
   const struct parhelia_screen *ps = ph_screen((struct pipe_screen *)s);

   return ps->texture_upload && ps->chip_id == 0x2539 &&
          p3d_texture_uses_native_64x16(ps->chip_id, ps->tile_layout, 4) &&
          ph_env("PARHELIA_GIGACOLOR");
}

static bool
ph_format_supported(struct pipe_screen *s, enum pipe_format format,
                    enum pipe_texture_target target, unsigned samples,
                    unsigned storage_samples, unsigned bind)
{
   bool supported = false;
   /* Volume slices and cube faces exist only in the 32-bit native order. */
   const bool layered = target == PIPE_TEXTURE_3D ||
                        target == PIPE_TEXTURE_CUBE;

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
      } else if (target == PIPE_TEXTURE_2D || target == PIPE_TEXTURE_RECT ||
                 target == PIPE_TEXTURE_1D ||
                 ((target == PIPE_TEXTURE_3D ? ph_screen(s)->volume_targets :
                   target == PIPE_TEXTURE_CUBE && ph_screen(s)->cube_targets) &&
                  (bind & PIPE_BIND_SAMPLER_VIEW) &&
                  !(bind & (PIPE_BIND_DEPTH_STENCIL |
                            PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                            PIPE_BIND_SHARED)))) {
         /* A 1-D image is a one-row 2-D surface: the address unit's own mip
          * chain for a height-1 base was measured level by level with
          * --mip-address-probe W 1 and needs no descriptor of its own.  A
          * volume or cube map is stored for the texture unit alone; the
          * state tracker asks for the render bind on the GL_RGBA/RGB packed
          * types it treats as required renderable formats and has no
          * fallback without it, so the bind is accepted and a draw into a
          * slice or face is refused at draw time. */
         switch (format) {
         case PIPE_FORMAT_B5G6R5_UNORM:
            /* Sampler code 0x28, and the same code as a render target: a
             * 3-D draw into one writes exact 5:6:5 and the 2-D fill clears
             * it, measured by --narrow-target-probe.  One resource cannot
             * hold both the sampler's and the render target's storage order,
             * and display or dma-buf sharing still needs the 32-bit linear
             * companion path. */
            if (bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT |
                        PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED))
               supported = ph_screen(s)->narrow_targets &&
                           !(bind & (PIPE_BIND_SAMPLER_VIEW |
                                     PIPE_BIND_DEPTH_STENCIL));
            else
               supported = ph_screen(s)->narrow_formats && !layered &&
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
         case PIPE_FORMAT_B10G10R10A2_UNORM:
            /* GigaColor as a sampler image, code 0x34 in the ordinary 32-bit
             * storage order; opt-in only, see ph_gigacolor_sampler().  It is
             * never a render target: the shared batch builder validates the
             * colour surface code against the two formats it drives, so a
             * ten-bit colour slot has no production path. */
            supported = ph_gigacolor_sampler(s) &&
                        (bind & PIPE_BIND_SAMPLER_VIEW) &&
                        !(bind & (PIPE_BIND_RENDER_TARGET |
                                  PIPE_BIND_DEPTH_STENCIL |
                                  PIPE_BIND_SCANOUT |
                                  PIPE_BIND_DISPLAY_TARGET |
                                  PIPE_BIND_SHARED));
            break;
         case PIPE_FORMAT_R10G10B10A2_UNORM:
         case PIPE_FORMAT_R10G10B10X2_UNORM:
         case PIPE_FORMAT_A2R10G10B10_UNORM:
         case PIPE_FORMAT_A2B10G10R10_UNORM:
            /* The other 10-bit packings are four bytes wide, so the default
             * arm below used to accept them, and both the sampler and the
             * colour output then read the word as 8:8:8:8 and delivered the
             * wrong channels.  The hardware has one 10-bit word,
             * A[31:30] R[29:20] G[19:10] B[9:0], which is
             * B10G10R10A2_UNORM; refusing the rest makes the state tracker
             * choose that one for GL_RGB10_A2 and GL_RGB10. */
            break;
         case PIPE_FORMAT_B5G5R5A1_UNORM:
         case PIPE_FORMAT_B4G4R4A4_UNORM:
         case PIPE_FORMAT_L8_UNORM:
            /* Native 8- and 16-bit sampler storage (codes 0x26, 0x24, 0x11).
             * There is no recovered render-target, depth or display
             * descriptor at these texel sizes, so they stay sampler-only.
             * Their alpha is no worse off than the 32-bit formats': this
             * texture unit emits none for any of them. */
            supported = ph_screen(s)->narrow_formats && !layered &&
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
            /* Native S3TC, sampler-only: block order, mip chain and NPOT
             * bases measured with parhelia-drm-test --texture-dxt-probe
             * (see ph_dxt_layout_init()).  Other targets sample a decoded
             * twin (parhelia_resource.decoded): Mesa takes S3TC for every
             * target once 2-D has it. */
            supported = ph_screen(s)->s3tc &&
                        (target == PIPE_TEXTURE_2D ||
                         ph_format_supported(s, PIPE_FORMAT_B8G8R8A8_UNORM,
                                             target, samples, storage_samples,
                                             PIPE_BIND_SAMPLER_VIEW)) &&
                        (bind & PIPE_BIND_SAMPLER_VIEW) &&
                        !(bind & (PIPE_BIND_RENDER_TARGET |
                                  PIPE_BIND_DEPTH_STENCIL |
                                  PIPE_BIND_SCANOUT |
                                  PIPE_BIND_DISPLAY_TARGET |
                                  PIPE_BIND_SHARED));
            break;
         case PIPE_FORMAT_Z24_UNORM_S8_UINT:
         /* The same 0x36 surface; its stencil byte is simply unused, which
          * gives GL a depth-only format with no stencil bits. */
         case PIPE_FORMAT_Z24X8_UNORM:
            supported = !(bind & PIPE_BIND_RENDER_TARGET);
            break;
         case PIPE_FORMAT_S8_UINT_Z24_UNORM:
            /* Native 0x36 depth is the low 24 bits; this order has no
             * depth/stencil surface descriptor. */
            supported = !(bind & (PIPE_BIND_RENDER_TARGET |
                                  PIPE_BIND_DEPTH_STENCIL));
            break;
         case PIPE_FORMAT_R8_UNORM:
         case PIPE_FORMAT_R8G8_UNORM:
            /* Stored as the 32-bit texel (r, g or 0, 0, 1), sampled and
             * rendered as such; the transfer paths widen and narrow it and
             * the write masks keep the missing channels (ph_format_widened).
             * Not shareable: other clients would see the 32-bit word. */
            supported = !(bind & (PIPE_BIND_DEPTH_STENCIL |
                                  PIPE_BIND_SCANOUT |
                                  PIPE_BIND_DISPLAY_TARGET |
                                  PIPE_BIND_SHARED));
            break;
         default: {
            /* The sampler and colour surfaces store 8-bit B,G,R,A; the
             * transfers reorder any other 8-bit UNORM byte order exactly.
             * Other four-byte formats (signed, 10- or 16-bit channels) would
             * be read as 8:8:8:8 and are left to the state tracker's
             * conversion into a supported format. */
            uint8_t order[4];

            supported = ph_rgba8_order(format, order) &&
                        !(bind & PIPE_BIND_DEPTH_STENCIL);
            break;
         }
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

   /* Sampler texels go through the one function that knows which order the
    * descriptor selects.  Choosing here instead wrote 8- and 16-bit images in
    * the generic order while 0x11e0 asked for the measured P690 one, and
    * 32-bit images without the coordinate permutation of a native level. */
   for (unsigned y = 0; y < r->base.height0; y++)
      for (unsigned x = 0; x < r->base.width0; x++)
         memcpy((uint8_t *)r->map +
                   (r->upload_texture ?
                    ph_mip_texel_offset_cpp(&r->mip, 0, x, y, !r->mip.tiled,
                                            cpp) :
                    p3d_tile_offset_layout_cpp(x, y, r->stride,
                                               r->tile_layout, r->cpp)),
                (uint8_t *)r->linear_map +
                   (size_t)y * r->linear_stride + x * texel, texel);
   if (r->transfer_shadow && ph_resource_upload_shadow(r))
      return;
   r->upload_dirty |= r->upload_texture;
   r->linear_dirty = false;
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

int
ph_resource_shadow_range(struct parhelia_resource *r, bool upload,
                         uint64_t offset, uint64_t size)
{
   return ph_resource_shadow_transfer(r, upload ?
                                         DRM_IOCTL_PARHELIA_UPLOAD_RENDER :
                                         DRM_IOCTL_PARHELIA_DOWNLOAD_RENDER,
                                      upload ? "render-shadow upload" :
                                               "render-shadow download",
                                      offset, size);
}

/* Each failure exit records where it left, for PARHELIA_TRACE. */
#define PH_CREATE_FAIL() do { fail_line = __LINE__; goto fail; } while (0)

/* @target_class: allocate in the render-target classes even where a small
 * sampler image would take the upload class. */
static struct pipe_resource *
ph_resource_create_class(struct pipe_screen *pscreen,
                         const struct pipe_resource *templ, bool target_class)
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
   const bool sampler_storage = !share_native && !target_class &&
      (templ->bind & PIPE_BIND_SAMPLER_VIEW) &&
      templ->width0 <= 2048 && templ->height0 <= 2048 &&
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
   /* Slices of a volume, or the six faces of a cube map.  The address unit
    * derives their addresses itself from 0x11e4[27:16]. */
   const bool cube_target = templ->target == PIPE_TEXTURE_CUBE;
   const unsigned slices = templ->target == PIPE_TEXTURE_3D ? templ->depth0 :
                           cube_target ? 6u : 1u;
   /* Every volume takes the volume order, a single slice too: a depth-1
    * volume still filters and wraps along r (--texture-volume-probe W H 1
    * matches the volume order). */
   const bool layered = templ->target == PIPE_TEXTURE_3D || cube_target;
   const bool native_64x32_texture = sampler_storage && !block_bytes &&
      p3d_texture_uses_native_64x32(screen->chip_id, screen->tile_layout,
                                    cpp);
   /* Original-family volumes and cube maps in the 64x32 orders. */
   const bool o528_layered = layered && native_64x32_texture &&
      (cube_target ? screen->cube_targets : screen->volume_targets);
   unsigned mip_levels;
   int fail_line = 0;
   if (!r)
      return NULL;
   r->linear_fd = -1;
   r->base = *templ;
   r->base.screen = pscreen;
   r->tile_layout = screen->tile_layout;
   r->native_shared = share_native;
   /* Other processes write shared images: their contents are always defined. */
   if (share_native)
      r->mip_defined = BITFIELD_BIT(0);
   if (!sampler_storage)
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
         PH_CREATE_FAIL();
      return &r->base;
   }
   /* A compressed image only has a native block layout; there is no 32-bit
    * fallback storage for one. */
   /* A volume or cube map needs the native sampler storage: its slices are
    * interleaved inside the storage tile, which only the tiled texture
    * classes have. */
   if (templ->width0 > max_width || templ->height0 > max_height ||
       (util_format_is_compressed(templ->format) && !block_bytes) ||
       (layered && (!(p690_native_texture32 || o528_layered) || block_bytes ||
                       slices > 256 ||
                       (cube_target && templ->width0 != templ->height0))) ||
       (templ->target != PIPE_TEXTURE_3D && templ->depth0 != 1) ||
       templ->array_size != (cube_target ? 6u : 1u)) {
      fprintf(stderr,
              "parhelia: reject resource target=%u format=%u %ux%ux%u array=%u levels=%u bind=%#x\n",
              templ->target, templ->format, templ->width0, templ->height0,
              templ->depth0, templ->array_size, templ->last_level + 1,
              templ->bind);
      PH_CREATE_FAIL();
   }
   /* P690 uses the common vendor chain geometry with a native 64x16 base,
    * compact 32x32 child, and a shared <=16 native 64x16 tail.  The
    * texture-only upload BO is required: an ordinary mapped render BO
    * selects the wrong storage class for mip sampling. */
   /* A volume or a cube map carries level 0 only: the address unit's own
    * chain geometry for those targets has not been measured, and a chain it
    * derives differently from the driver's samples the wrong storage.  A 1-D
    * chain needs no exception: --mip-address-probe W 1 walks every level of a
    * 64x1 and a 256x1 base, tail included, with no differing texel. */
   /* The original family's chain layout is measured for square, larger and
    * rectangular chains (ph-glprobe mipmap mode, every level exact). */
   const bool cube_probe = cube_target && p690_native_texture32 &&
                           ph_env("PARHELIA_CUBE_PROBE");
   const bool cube_chain = cube_target && p690_native_texture32;
   /* Original-family cube chains in the vendor order (ph_mip_layout_cube_64x32),
    * every level exact on the 0528 for faces 1..256 and NPOT 20..100. */
   const bool o528_cube_chain = cube_target && o528_layered &&
      (!(templ->width0 & (templ->width0 - 1)) ? templ->width0 <= 256 :
                                                templ->width0 <= 100);
   /* P690 NPOT chains, 2-D and cube, are exact for every texel of every
    * level up to 256 (fixfn cubeprobe); larger NPOT bases keep level 0 until
    * measured (a 600x20 probe met a concurrent lockup: inconclusive).  The original family's
    * NPOT chain is measured up to 255 and taken when no level loses a cell. */
   const bool pot = !(templ->width0 & (templ->width0 - 1)) &&
                    !(templ->height0 & (templ->height0 - 1));
   const bool p690_npot = templ->width0 <= 256 && templ->height0 <= 256;
   const bool o528_npot = p690_npot && native_64x32_texture && !layered &&
      ph_mip_native_64x32_chain_exact(templ->width0, templ->height0,
                                      templ->last_level + 1);
   mip_levels = ((p690_native_texture32 && (pot || p690_npot)) ||
                 (native_64x32_texture && (pot || o528_npot || o528_cube_chain))) &&
                (!layered || cube_chain || o528_cube_chain) ?
                   templ->last_level + 1 : 1;
   if (!ph_mip_layout_init_cpp(&r->mip, templ->width0, templ->height0,
                               mip_levels, true, p690_native_texture32, cpp))
      PH_CREATE_FAIL();
   /* A compressed image is stored as 4x4 blocks in 64x32-texel tiles.
    * Its level pitch stays a texel count; only the byte size differs. */
   if (block_bytes) {
      ph_dxt_layout_init(&r->mip, templ->width0, templ->height0,
                         MIN2(templ->last_level + 1, PH_MAX_MIP_LEVELS),
                         block_bytes);
      r->mip.size = ph_mip_align(r->mip.size,
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
   /* The original's sampler reads single-level 32-bit storage in 64x32 tiles
    * at a 32-aligned pitch; cover every tile its row bands reach. */
   if (native_64x32_texture && (!layered || o528_layered)) {
      if (layered) {
         r->mip.native_64x32 = true;
         r->mip.level[0].pitch = ph_mip_align(templ->width0, 32);
      } else if (!ph_mip_layout_native_64x32(&r->mip)) {
         PH_CREATE_FAIL();
      }
      if (ph_env("PARHELIA_TRACE"))
         for (unsigned l = 0; l < mip_levels; l++)
            fprintf(stderr,
                    "parhelia: 64x32 level %u %ux%u pitch %u offset %#zx tail %u,%u bytes %#zx (size %#zx)\n",
                    l, r->mip.level[l].width, r->mip.level[l].height,
                    r->mip.level[l].pitch, r->mip.level[l].offset,
                    r->mip.level[l].tail_x, r->mip.level[l].tail_y,
                    r->mip.level[l].bytes, r->mip.size);
   }
   /* One level image per cube face, or one per volume slice; the chain above
    * already describes a single image. */
   if (layered) {
      r->mip.slices = slices;
      r->mip.cube = cube_target;
      /* The vendor's own level size, which is what the address unit derives
       * a face stride from: the descriptor pitch by the tile-row-aligned
       * height (mtx1444, .text+0x5b972). */
      r->mip.level[0].bytes = (size_t)r->mip.level[0].pitch *
                              ph_mip_align(templ->height0,
                                           o528_layered ? 32 : 16) * 4;
      r->mip.size = ph_mip_align(r->mip.level[0].bytes * slices, 0x2000);
      /* the last 8 KiB tile the original family's volume order reaches */
      if (o528_layered && !cube_target) {
         const size_t cells = (size_t)ph_mip_align(templ->height0, 32) / 32 *
                              (r->mip.level[0].pitch / 32);

         r->mip.size = ((((cells - 1) | 1) * slices + slices - 1) / 2 + 1) *
                       0x2000;
      }
      if (o528_cube_chain && !ph_mip_layout_cube_64x32(&r->mip, slices))
         PH_CREATE_FAIL();
      /* The P690 cube chain (PARHELIA_CUBE_PROBE, every texel of 32..256
       * cubes): levels above 16x16 hold six whole face images, each the
       * vendor's level size apart (mtx3887); the 2-D tail block follows,
       * its levels at the 2-D tail positions with the faces interleaved as
       * volume slices. */
      if (cube_chain && !cube_probe) {
         size_t next = 0;
         unsigned tail = 0;

         for (unsigned l = 0; l < mip_levels; l++) {
            struct ph_mip_level *level = &r->mip.level[l];

            if (level->width <= 16) {
               if (!tail)
                  r->mip.tail_base = next;
               ph_mip_tail_position(tail++, &level->tail_x, &level->tail_y);
               level->offset = r->mip.tail_base;
               level->pitch = 32;
               level->face_tail = true;
               level->bytes = 0;
               continue;
            }
            level->pitch = ph_mip_align(level->width, 32);
            level->tail_x = level->tail_y = 0;
            level->offset = next;
            level->bytes = (size_t)level->pitch *
                           ph_mip_align(level->height, 16) * 4;
            next += level->bytes * slices;
         }
         if (tail)
            next = r->mip.tail_base +
                   ph_volume_texel_offset(31, 15, slices - 1, 32, slices, 4) + 4;
         r->mip.size = ph_mip_align(next, 0x2000);
      }
      if (cube_probe) {
         size_t next = 0, vendor = 0;

         r->mip.probe_linear = true;
         for (unsigned l = 0; l < mip_levels; l++) {
            struct ph_mip_level *level = &r->mip.level[l];

            level->pitch = level->width;
            level->tail_x = level->tail_y = 0;
            level->offset = next;
            level->bytes = (size_t)level->width * level->height * 4;
            next += level->bytes * slices;
            /* the vendor's chain (mtx3887): 6 x align32(w) x align16(h) */
            vendor += (size_t)slices * ph_mip_align(level->width, 32) *
                      ph_mip_align(level->height, 16) * 4;
         }
         r->mip.size = ph_mip_align(MAX2(next, vendor), 0x2000);
      }
   }
   /* CREATE_RENDER has a bounded height.  Retain the historical level-zero
    * allocation for an unusually large chain rather than rejecting an
    * otherwise usable texture; transfer maps for its lower levels become
    * harmless staging-only fallbacks. */
   /* Count the rows the allocation below really asks for: it uses a pitch of
    * at least 64 texels, so measuring a 32-texel chain against its own pitch
    * doubled the row count and silently dropped mipmapping for a tall narrow
    * base (32x512 sampled level 0 at every level). */
   /* The original family asks for wider rows of the same storage instead:
    * a 256 cube chain needs 2064 rows of 1 KiB (S10, MEASURED). */
   unsigned alloc_width = templ->width0;

   while (native_64x32_texture && alloc_width < 2048 &&
          (r->mip.size + (size_t)ph_mip_align(alloc_width, 32) * 4 - 1) /
          ((size_t)ph_mip_align(alloc_width, 32) * 4) > 2048)
      alloc_width = MIN2(ph_mip_align(alloc_width, 32) * 2, 2048u);
   if (mip_levels > 1 &&
       (native_64x32_texture ?
          (r->mip.size + (size_t)ph_mip_align(alloc_width, 32) * 4 - 1) /
          ((size_t)ph_mip_align(alloc_width, 32) * 4) :
          (r->mip.size + (size_t)MAX2(r->mip.level[0].pitch, 64u) * 4 - 1) /
          ((size_t)MAX2(r->mip.level[0].pitch, 64u) * 4)) > 2048) {
      mip_levels = 1;
      alloc_width = templ->width0;
      if (!ph_mip_layout_init_chip(&r->mip, templ->width0, templ->height0,
                                   mip_levels, true, p690_native_texture32))
         PH_CREATE_FAIL();
      if (p690_native_texture32) {
         r->mip.native_64x16_32 = true;
         if (templ->width0 > 32)
            r->mip.level[0].pitch = MAX2(r->mip.level[0].pitch, 64u);
         r->mip.size = ph_mip_align(r->mip.level[0].pitch *
                                   ph_mip_align(templ->height0, 32) * 4,
                                   0x2000);
      }
      /* the original family keeps its 64x32 order for the level left */
      if (native_64x32_texture) {
         if (layered)
            r->mip.level[0].pitch = ph_mip_align(templ->width0, 32);
         else if (!ph_mip_layout_native_64x32(&r->mip))
            PH_CREATE_FAIL();
         r->mip.native_64x32 = true;
      }
      /* A cube map too large for its chain keeps six level-0 faces. */
      if (layered) {
         r->mip.slices = slices;
         r->mip.cube = cube_target;
         r->mip.level[0].bytes = (size_t)r->mip.level[0].pitch *
                                 ph_mip_align(templ->height0, 16) * 4;
         r->mip.size = ph_mip_align(r->mip.level[0].bytes * slices, 0x2000);
      }
      if (layered && native_64x32_texture) {
         r->mip.level[0].bytes = (size_t)r->mip.level[0].pitch *
                                 ph_mip_align(templ->height0, 32) * 4;
         r->mip.size = ph_mip_align(r->mip.level[0].bytes * slices, 0x2000);
      }
   }
   /* The allocation ABI only describes 32-bit texture rows.  A chain, or an
    * 8/16-bit image whose byte size no longer follows width0*height0*4, asks
    * for the rows of that wider shape which cover its storage. */
   const unsigned create_pitch = MAX2(r->mip.level[0].pitch, 64u);
   /* The kernel allocates original-family texture rows at its own pitch,
    * max(32, align(width, 32)); asking for rows of a wider pitch halved the
    * storage of 32-wide chains (32x64 failed the size check below). */
   const unsigned row_pitch = native_64x32_texture ?
      MAX2(ph_mip_align(alloc_width, 32), 32u) : create_pitch;
   struct drm_parhelia_create_render create = {
      .width = p690_native_texture32 ? MAX2(templ->width0, 64u) :
               cpp != 4 || block_bytes ? create_pitch :
               native_64x32_texture ? alloc_width : templ->width0,
      /* Rows of the (possibly wider) allocation that hold the chain.  A
       * single-level native image also needs them whenever its layout
       * reserves more rows than it has, which a one-row 1-D image always
       * does: the P690 order quantises the base level to 32 rows. */
      .height = mip_levels > 1 || cpp != 4 || block_bytes || layered ||
                r->mip.size > (size_t)row_pitch * 4 * templ->height0 ?
         (r->mip.size + (size_t)row_pitch * 4 - 1) /
         ((size_t)row_pitch * 4) :
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
   if (sampler_storage && !screen->texture_upload &&
       (screen->chip_id == 0x2537 || screen->chip_id == 0x2538 ||
        screen->chip_id == 0x2539)) {
      fprintf(stderr,
              "parhelia: safe LX texture upload ABI is unavailable\n");
      PH_CREATE_FAIL();
   }
   if (sampler_storage)
      create.flags = PARHELIA_CREATE_RENDER_TEXTURE;
   else if (native_depth)
      create.flags = PARHELIA_CREATE_RENDER_NATIVE_DEPTH;
   else if (native_color)
      create.flags = PARHELIA_CREATE_RENDER_NATIVE_COLOR;
   /* The X front buffer is the one front-rendered scanout image: its
    * display refresh belongs in bank B, away from colour and textures. */
   /* With page flipping the image the CRTC refreshes is a *client* colour
    * buffer, which carries SCANOUT but not USE_FRONT_RENDERING, so it landed
    * in the render band and its refresh competed with colour, depth and
    * texture traffic - the very cost the display band exists to avoid.
    * PARHELIA_FLIP_DISPLAY_BAND=1 places flip candidates there too. */
   if (native_color && (templ->bind & PIPE_BIND_SCANOUT) &&
       !(templ->bind & PIPE_BIND_USE_FRONT_RENDERING) &&
       ph_env("PARHELIA_FLIP_DISPLAY_BAND")) {
      create.flags = PARHELIA_CREATE_RENDER_NATIVE_SCANOUT;
      if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &create)) {
         create.flags = PARHELIA_CREATE_RENDER_NATIVE_COLOR;
         create.handle = create.pitch = 0;
         create.size = create.mmap_offset = 0;
      }
   }
   if (!create.handle && native_color &&
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
      PH_CREATE_FAIL();
   }
   r->handle = create.handle;
   r->handle_owned = true;
   r->create_width = create.width;
   r->create_height = create.height;
   r->create_cpp = create.cpp;
   r->create_flags = create.flags;
   r->size = create.size;
   /* The DRM allocation ABI reports byte pitch, while the recovered surface
    * descriptor and p3d_tile_offset_layout() both consume pitch in pixels. */
   if ((create.pitch & 3) || create.pitch < templ->width0 * r->cpp ||
       create.pitch % r->cpp)
      PH_CREATE_FAIL();
   /* Layout 2 uses a power-of-two texel pitch, which can exceed the current
    * allocation ABI's 64-pixel pitch for NPOT images.  Reject any object
    * whose reported storage cannot contain the software-swizzled image. */
   if ((create.flags & PARHELIA_CREATE_RENDER_TEXTURE) &&
       create.size < r->mip.size)
      PH_CREATE_FAIL();
   r->stride = create.pitch / r->cpp;
   r->mmap_offset = create.mmap_offset;
   r->upload_texture = screen->texture_upload &&
                       create.flags == PARHELIA_CREATE_RENDER_TEXTURE;
   if (r->upload_texture) {
      if (create.mmap_offset)
         PH_CREATE_FAIL();
      r->map = align_malloc(r->size, 64);
      if (!r->map)
         PH_CREATE_FAIL();
      memset(r->map, 0, r->size);
      /* PARHELIA_TEX_ADDRESS_FILL=1 (diagnostic): every unwritten dword
       * names its own offset, so a sampler read outside the stored texels
       * shows where it went instead of black. */
      if (ph_env("PARHELIA_TEX_ADDRESS_FILL"))
         for (size_t i = 0; i < r->size / 4; i++)
            ((uint32_t *)r->map)[i] = 0xfff00000u | (uint32_t)(i & 0xfffff);
      r->upload_dirty = true;
   } else if (native_depth || native_color) {
      /* Compact P690 target classes live above the raw 16-MiB host window.
       * Their zero mmap offset is intentional.  Imported/present resources
       * which already have a CPU-visible linear companion use GPU BITBLT;
       * direct CPU transfers on a private class target remain unavailable. */
      if (create.mmap_offset)
         PH_CREATE_FAIL();
      if (screen->render_transfer) {
         r->map = align_malloc(r->size, 64);
         if (!r->map)
            PH_CREATE_FAIL();
         memset(r->map, 0, r->size);
         r->transfer_shadow = true;
      }
   } else if (screen->render_transfer && !screen->native_rt_classes) {
      /* The original board's BAR0 is descriptor translated as well, so a
       * render object's mmap offset does not reach its VRAM: glReadPixels
       * returned zeros.  Keep a CPU shadow moved through the kernel's
       * aperture copy, as the P690's class targets do. */
      r->map = align_malloc(r->size, 64);
      if (!r->map)
         PH_CREATE_FAIL();
      memset(r->map, 0, r->size);
      r->transfer_shadow = true;
   } else {
      if (!r->mmap_offset)
         PH_CREATE_FAIL();
      r->map = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    ph_screen(pscreen)->fd, r->mmap_offset);
      if (r->map == MAP_FAILED) {
         r->map = NULL;
         PH_CREATE_FAIL();
      }
   }
   /* A compressed image has no linear companion: layout 5 is the only
    * storage the sampler decodes, and nothing detiles blocks.  Neither has a
    * 16 bpp render or depth surface: it is never shared or sampled, and its
    * CPU image is the byte-exact native shadow. */
   if (!share_native && !block_bytes && r->cpp == 4 && !layered &&
       (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                       PIPE_BIND_SHARED | PIPE_BIND_SAMPLER_VIEW))) {
      /* CREATE_DUMB is a modeset-only ioctl and is unavailable through a
       * render node.  A second render allocation is valid linear sampler
       * storage when its descriptor selects texture layout 0; it also gives
       * the conservative non-aliasing placement required by this hardware. */
      if (!ph_mip_layout_init_cpp(&r->linear_mip, templ->width0,
                                  templ->height0, mip_levels, false, false,
                                  cpp))
         PH_CREATE_FAIL();
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
            PH_CREATE_FAIL();
         memset(r->linear_map, 0, r->linear_size);
      } else {
         if (ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER,
                   &linear)) {
            fprintf(stderr,
                    "parhelia: create linear shadow %ux%u failed: %s\n",
                    linear.width, linear.height, strerror(errno));
            PH_CREATE_FAIL();
         }
         r->linear_handle = linear.handle;
         r->linear_size = linear.size;
         r->linear_stride = linear.pitch;
         if (r->linear_stride < ALIGN_POT(templ->width0, 32) * 4 ||
             (r->linear_stride & 3))
            PH_CREATE_FAIL();
         if ((mip_levels > 1 || cpp != 4) &&
             linear.size < r->linear_mip.size)
            PH_CREATE_FAIL();
         r->linear_handle_owned = true;
         r->linear_map = mmap(NULL, r->linear_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, ph_screen(pscreen)->fd,
                              linear.mmap_offset);
         if (r->linear_map == MAP_FAILED) {
            r->linear_map = NULL;
            PH_CREATE_FAIL();
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
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: create %ux%u format=%s bind=%#x failed at parhelia_screen.c:%d (%s)\n",
              templ->width0, templ->height0,
              util_format_short_name(templ->format), templ->bind,
              fail_line, strerror(errno));
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
ph_resource_create(struct pipe_screen *pscreen, const struct pipe_resource *templ)
{
   const bool npot = (templ->width0 & (templ->width0 - 1)) ||
                     (templ->height0 & (templ->height0 - 1));

   if (util_format_is_s3tc(templ->format) &&
       (templ->target != PIPE_TEXTURE_2D || (npot && templ->last_level))) {
      struct parhelia_resource *r = CALLOC_STRUCT(parhelia_resource);
      struct pipe_resource decoded = *templ;

      if (!r)
         return NULL;
      decoded.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      r->base = *templ;
      pipe_reference_init(&r->base.reference, 1);
      r->base.screen = pscreen;
      r->decoded = ph_resource_create_class(pscreen, &decoded, false);
      r->blocks = CALLOC((templ->last_level + 1) *
                         MAX2(templ->array_size, templ->depth0),
                         sizeof(*r->blocks));
      if (!r->decoded || !r->blocks) {
         pipe_resource_reference(&r->decoded, NULL);
         FREE(r->blocks);
         FREE(r);
         return NULL;
      }
      return &r->base;
   }
   return ph_resource_create_class(pscreen, templ, false);
}

struct parhelia_resource *
ph_resource_create_target_storage(struct parhelia_resource *r)
{
   struct pipe_resource *storage =
      ph_resource_create_class(r->base.screen, &r->base, true);

   return storage ? ph_resource(storage) : NULL;
}

void
ph_resource_swap_storage(struct parhelia_resource *a,
                         struct parhelia_resource *b)
{
   const struct pipe_resource base_a = a->base, base_b = b->base;
   const struct parhelia_resource tmp = *a;

   *a = *b;
   *b = tmp;
   a->base = base_a;
   b->base = base_b;
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
         unsigned import_cpp = 4;

         ph_target_format(templ->format, NULL, &import_cpp);
         if (!info.pitch || (info.pitch & 255) ||
             info.pitch % import_cpp ||
             info.pitch < (uint64_t)ALIGN_POT(templ->width0,
                                              64 * 4 / import_cpp) *
                          import_cpp ||
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
         /* GET_BO_INFO reports no pixel size; the importer's own format
          * decides how many bytes a pixel of this surface has. */
         ph_target_format(templ->format, &r->surf_fmt, &r->cpp);
         r->stride = info.pitch / r->cpp;
         r->tile_layout = screen->tile_layout;
         r->native_shared = true;
         r->mip_defined = BITFIELD_BIT(0);
         /* CPU access goes through a byte-exact shadow like that of the
          * native targets this driver shares itself (an X pixmap's BO);
          * transfers take the image extent from the one-level layout. */
         if (screen->render_transfer) {
            r->map = align_malloc(r->size, 64);
            if (!r->map ||
                !ph_mip_layout_init_cpp(&r->mip, templ->width0,
                                        templ->height0, 1, true, false, 4)) {
               align_free(r->map);
               if (imported_owned) {
                  struct drm_gem_close close = {.handle = imported_handle};
                  ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
               }
               FREE(r);
               return NULL;
            }
            r->transfer_shadow = true;
         }
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

   if (r->decoded) {
      const unsigned n = (pres->last_level + 1) *
                         MAX2(pres->array_size, pres->depth0);

      for (unsigned i = 0; i < n; i++)
         FREE(r->blocks[i]);
      FREE(r->blocks);
      pipe_resource_reference(&r->decoded, NULL);
      FREE(r);
      return;
   }

   free(r->bounds);
   /* Nothing can consume an outstanding hardware depth clear once the surface
    * is gone; only the resolve target's reference has to be released. */
   r->zinit_pending = false;
   pipe_resource_reference(&r->zinit_colour, NULL);
   if (r->premul_rgb_handle) {
      struct drm_gem_close close = {.handle = r->premul_rgb_handle};
      ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
   if (r->alpha_rgb_handle) {
      struct drm_gem_close close = {.handle = r->alpha_rgb_handle};
      ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
   if (r->rgb5a1_handle) {
      struct drm_gem_close close = {.handle = r->rgb5a1_handle};
      ioctl(ph_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
   if (r->rgb5a1_map)
      align_free(r->rgb5a1_map);
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
   if (r->sw_level) {
      for (unsigned l = 0; l <= r->base.last_level; l++)
         FREE(r->sw_level[l]);
      FREE(r->sw_level);
   }
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
      struct drm_parhelia_get_bo_info info;

      /* The DRI3/KMS stride is that of the kernel's CPU-linear view; the
       * native pitch travels through GET_BO_INFO. */
      whandle->stride = ph_query_bo(ph_screen(pscreen), r->handle, &info) &&
                        info.host_pitch ? info.host_pitch : r->stride * r->cpp;
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
   struct parhelia_screen *s;
   struct drm_parhelia_get_param chip = {.param = PARHELIA_PARAM_CHIP_ID};
   struct drm_parhelia_get_param tile = {.param = PARHELIA_PARAM_TILE_LAYOUT};
   struct drm_parhelia_get_param units = {
      .param = PARHELIA_PARAM_TEXTURE_UNITS,
   };
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
   struct drm_parhelia_get_param draw_ranges = {
      .param = PARHELIA_PARAM_DRAW_RANGES,
   };
   struct drm_parhelia_get_param indexed = {
      .param = PARHELIA_PARAM_INDEXED_DRAWS,
   };
   struct drm_parhelia_get_param max_packets = {
      .param = PARHELIA_PARAM_MAX_PACKETS,
   };
   s = CALLOC_STRUCT(parhelia_screen);
   (void)config;
   if (!s)
      return NULL;
   s->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (s->fd < 0 || ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &chip) ||
       ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &tile) ||
       ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &units) ||
       ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &vram) ||
       ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &pipe_mode))
      goto fail;
   s->texture_units = units.value >= 1 && units.value <= P3D_MAX_TEXTURE_UNITS ?
      (unsigned)units.value : 2u;
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
   /* The vendor splits 2-D fills into strips on both families (P-series
    * mtxpserd.dll 0x112480, Parhelia MTXPARD.dll 0x1105bd).  Measured on
    * the P690; the original Parhelia follows the vendor until measured. */
   parhelia_batch_set_fill_strips(true);
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
   s->draw_ranges = s->async_submit &&
      debug_get_bool_option("PARHELIA_DRAW_RANGES", true) &&
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &draw_ranges) &&
      draw_ranges.value == 1;
   /* Indexed draws: the kernel says how many triangles one packet takes. */
   s->indexed_tris = s->draw_ranges && ph_env("PARHELIA_INDEXED") &&
      !ioctl(s->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &indexed) ?
      (unsigned)indexed.value : 0;
   /* Native 8- and 16-bit sampler storage is on where its texel order has
    * been measured: parhelia-drm-test --texture-format-probe reproduces every
    * texel of L8, A1R5G5B5, R5G6B5 and A4R4G4B4 over two tile rows and two
    * tile columns on the P690.  PARHELIA_NO_NARROW_FORMATS=1 opts out. */
   s->narrow_formats = s->texture_upload &&
      p3d_texture_uses_p690_narrow(s->chip_id, s->tile_layout, 2) &&
      !ph_env("PARHELIA_NO_NARROW_FORMATS");
   /* S3TC block order, chain and NPOT bases are measured on the LX family
    * (--texture-dxt-probe); the original family takes it on request until
    * measured there.  PARHELIA_NO_S3TC=1 opts out. */
   s->s3tc = s->texture_upload && !ph_env("PARHELIA_NO_S3TC") &&
             ((s->chip_id >= 0x2537 && s->chip_id <= 0x2539) ||
              ph_env("PARHELIA_S3TC"));
   /* Volume and cube-map targets need the native 32-bit texture order, whose
    * storage tile carries the volume slice interleave, and the address unit's
    * three-coordinate mode.  Both are measured: --texture-volume-probe
    * reproduces every texel of four volume geometries, and
    * tools/parhelia-gl-texture-target-test compares every texel of every
    * slice and of all six cube faces at the GL level.  See
    * docs/parhelia-p690-volume-cube-targets.md.  PARHELIA_NO_VOLUME_TARGETS=1
    * returns the driver to 2-D and 1-D only. */
   /* The original family's volume order and its cube faces, stacked as rows
    * of one 64x32-order image, are measured the same way on the 0528. */
   s->volume_targets = s->texture_upload &&
      ((s->chip_id == 0x2539 &&
        p3d_texture_uses_native_64x16(s->chip_id, s->tile_layout, 4)) ||
       p3d_texture_uses_native_64x32(s->chip_id, s->tile_layout, 4)) &&
      !ph_env("PARHELIA_NO_VOLUME_TARGETS");
   s->cube_targets = s->volume_targets;
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
   /* With one target, per-target blend state is rt[0], which every path
    * reads; st needs it to pin destination alpha of RGB and emulated L/I
    * targets to one. */
   caps->indep_blend_enable = caps->indep_blend_func = true;
   /* Vendor ranges: aliased 1..2048, smooth 0.25..10 in 0.25 steps.  Sized
    * points and wide or smooth lines are expanded to triangles, so smooth
    * sizes below one pixel have no coverage path and start at 1.0. */
   caps->min_point_size = caps->min_point_size_aa = 1.0f;
   caps->min_line_width = caps->min_line_width_aa = 1.0f;
   caps->max_point_size = caps->max_line_width = 2048.0f;
   caps->max_point_size_aa = caps->max_line_width_aa = 10.0f;
   /* The vendor's 10.0 smooth-point clamp is dead code (ICD 0x114343): its
    * smooth points take any size.  The original family draws them as
    * coverage sprites at any size, so its range is the sprites'. */
   if (s->chip_id == 0x0527 || s->chip_id == 0x0528)
      caps->max_point_size_aa = caps->max_point_size;
   caps->point_size_granularity = caps->line_width_granularity = 0.25f;
   /* The PLNWT solid fill takes an arbitrary rectangle and bit-plane mask;
    * partial-tile fills are hardware-verified only on the P690 64x16 layout. */
   caps->clear_scissored = caps->clear_masked =
      s->tile_layout == PARHELIA_TILE_64X16_4K;
   /* Native shared render/scanout images support the chip's full 2048-pixel
    * surface extent.  DRI rejects a GBM image before resource_create() when
    * either dimension exceeds this cap, so reporting 1024 prevented glamor
    * from creating a 1920x1200 front buffer despite that path being valid. */
   /* The sampler reaches the same 2048 the vendor's GL advertises: every
    * level of a 528-, 640-, 1024- and 2048-wide chain is byte exact
    * (--mip-address-probe).  A 512 limit measured earlier was the probe's own
    * fixed 512x512 clip window discarding the part of the quad past it, whose
    * undrawn pixels the check then counted as texels the sampler got wrong. */
   caps->max_texture_2d_size = 2048;
   /* Volume and cube-map targets.  The chip advertises 2048 for both (vendor
    * libGL 0x13bce0 and 0x13c539); a volume also has to fit its slices in
    * VRAM, so 512 faces and 256-cubed volumes are the honest limits.  The cap is a level count,
    * so it bounds the extent; only level 0 is stored either way.  Array
    * textures have no hardware target at all and stay at zero. */
   if (s->volume_targets)
      caps->max_texture_3d_levels = 9;
   /* The original family's cube chains are measured up to 256. */
   if (s->cube_targets)
      caps->max_texture_cube_levels =
         s->chip_id == 0x0527 || s->chip_id == 0x0528 ? 9 : 10;
   /* There is no pipe fence object yet.  Native window swaps are ordered by
    * the following Present blit, so the DRI frontend must not request a NULL
    * fence solely to throttle every swap in the client. */
   caps->throttle = false;
   /* The fragment pipeline evaluates only what the combiner can express, and
    * there is no software fragment fallback.  The data-flow compiler covers
    * the fixed-function environments and the GLSL 1.20 shapes of
    * tools/parhelia-gl-combiner-test.  GL 2.1 needs 1.20: reporting 1.10
    * makes Mesa fail shading_language_version(). */
   caps->glsl_feature_level = caps->glsl_feature_level_compatibility = 120;
   caps->essl_feature_level = 100;
   caps->npot_textures = true;
   /* The address unit's 3-bit mode per axis covers repeat, clamp-to-edge,
    * mirrored repeat, mirror-once (clamp-to-edge) and the two border clamps,
    * each measured by parhelia-drm-test --wrap-probe.  It mirrors once into
    * the edge only, so GL_EXT_texture_mirror_clamp's mirror-into-border forms
    * stay off; ARB_texture_mirror_clamp_to_edge is exact. */
   caps->texture_mirror_clamp_to_edge = true;
   caps->texture_mirror_clamp = false;
   caps->anisotropic_filter = true;
   caps->max_texture_anisotropy = 2.0f;
   /* The bias field reaches -12 .. +19.9 (ph_max_lod_bias): the P690 takes
    * the symmetric +/-12 (measured to +/-5).  The 0528 applies every bias of
    * +/-12 exactly but clamps a magnified raw LOD at -4 first (lodprobe2), so
    * GL's lodbias test fails there while tex-miplevel-selection needs 12. */
   s->max_lod_bias = s->chip_id == 0x2539 ||
                     s->chip_id == 0x0527 || s->chip_id == 0x0528 ? 12.0f : 2.0f;
   /* PARHELIA_LOD_BIAS_RANGE=n (diagnostic): clamp and advertise +/-n */
   if (getenv("PARHELIA_LOD_BIAS_RANGE"))
      s->max_lod_bias = CLAMP(atof(getenv("PARHELIA_LOD_BIAS_RANGE")),
                              0.0, 12.0);
   caps->max_texture_lod_bias = s->max_lod_bias;
   /* BLENDCTL has an equation field per pipe; the vendor writes both from
    * one GL enum, independent pipes are measured on the P690 and the 0528. */
   caps->blend_equation_separate = true;
   /* Colour and depth surfaces carry their own pitch and base, and draws and
    * clears are bounded by the framebuffer state, the attachments' minimum
    * (measured on the P690 and the 0528). */
   caps->mixed_framebuffer_sizes = s->chip_id == 0x2539 ||
                                   s->chip_id == 0x0527 || s->chip_id == 0x0528;
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
   /* The WARP engine has no texture path and the context never hands vertex
    * sampler views to draw, so vertex texture fetch is not available. */
   ((struct pipe_shader_caps *)&s->base.shader_caps[MESA_SHADER_VERTEX])->max_texture_samplers = 0;
   ((struct pipe_shader_caps *)&s->base.shader_caps[MESA_SHADER_VERTEX])->max_sampler_views = 0;
   struct pipe_shader_caps *fs =
      (struct pipe_shader_caps *)&s->base.shader_caps[MESA_SHADER_FRAGMENT];
   /* What a program may declare, at least ARB_fragment_program's minimums:
    * the compiler, not these limits, refuses what the combiner cannot run
    * (a GLSL link failure, an ARB program's draws dropped). */
   fs->max_instructions = fs->max_alu_instructions = 72;
   fs->max_tex_instructions = 24;
   fs->max_tex_indirections = 8;
   fs->max_inputs = 10;	/* ARB_fragment_program attribs; routing decides */
   fs->max_outputs = 1;
   /* the preshader reads uniforms from the buffer at any offset */
   fs->max_const_buffer0_size = 32 * 4 * sizeof(float);
   fs->max_const_buffers = 1;
   fs->max_temps = 16;
   /* The chip's own count, from the kernel: four on the original Parhelia
    * (0x0527/0x0528), two on the LX family, whose pair executes through one
    * physical pipe. */
   fs->max_texture_samplers = fs->max_sampler_views = ph_units_driven(s);
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
