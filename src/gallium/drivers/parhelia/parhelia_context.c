/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Software vertex processing followed by native Parhelia raster submission. */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_pstipple.h"
#include "util/u_sampler.h"
#include "compiler/nir/nir_constant_expressions.h"
#include "draw/draw_context.h"
#include "draw/draw_fs.h"
#include "draw/draw_pipe.h"
#include "draw/draw_vertex.h"
#include "tgsi/tgsi_from_mesa.h"
#include "translate/translate.h"
#include "translate/translate_cache.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/blend.h"
#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/u_draw.h"
#include "util/u_dynarray.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_prim.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"

#include "parhelia_batch.h"
#include "parhelia_internal.h"
#include "parhelia_sampler.h"
#include "parhelia_texel.h"
#include "parhelia_tile_copy.h"
#include "parhelia_fs_compile.h"
#include "parhelia_fs_lookup.h"
#include "util/os_time.h"
#include "parhelia_vs_compile.h"

struct ph_velems {
   unsigned count;
   struct pipe_vertex_element elem[PIPE_MAX_ATTRIBS];
};

/* 0x2537, 0x2538 and 0x2539 are the LX/P650/P750/P690 family: the vendor's
 * class mask 0x7fc00 covers exactly these three device ids. */
static inline bool ph_lx_family(uint32_t chip_id)
{
   return chip_id == 0x2537 || chip_id == 0x2538 || chip_id == 0x2539;
}

/* 0x0527 and 0x0528, the original Parhelia (Parhelia-512 / -8X). */
static inline bool ph_original_family(uint32_t chip_id)
{
   return chip_id == 0x0527 || chip_id == 0x0528;
}

/* RGBA draws take a T2-free program after the per-slot T2 prime: the LX
 * class, and the original family (latch measured on the 0528). */
static inline bool ph_t2_free_rgba(uint32_t chip_id)
{
   return ph_lx_family(chip_id) || ph_original_family(chip_id);
}

/* Fixed-function vertex program recognised for hardware transform: the only
 * computed output is POSITION = MVP * input, every other output copies an
 * input attribute verbatim.  The WARP transform programs express exactly that
 * shape, so anything else keeps the software vertex pipeline. */
struct ph_vs {
   struct draw_vertex_shader *draw;
   bool ff_transform;
   unsigned mvp_offset;         /* vec4 index of MVP row 0 in constbuf 0 */
   int pos_input;               /* driver_location of the position input */
   int colour_input;            /* -1 when the program writes no colour */
   int texcoord_input[2];       /* -1 when the set is not written */
   /* A shader the WARP compiler accepted: its position is still the matrix
    * product above, so the CPU can keep bounding the clip volume, but every
    * other output is compiled rather than pattern matched.  The program runs
    * from a kernel instruction-store slot this context uploaded. */
   bool hw_program;
   /* A program whose position is no matrix product keeps its position
    * slice here, so the CPU can still bound the clip volume per vertex. */
   struct ph_slice *pos_slice;
   struct ph_vs_program prog;
   struct ph_vs_inputs prog_inputs;
   unsigned prog_slot;          /* kernel WARP program slot, 0 = none yet */
   uint32_t prog_entry;
   bool prog_upload_failed;
};

/* P690 serializes fixed-function texture shapes without a proven native
 * route.  MODULATE uses the hardware-proven one-pass two-unit path; the other
 * exact shapes retain their two-pass equivalents.  Anything else falls back
 * to the first texture rather than being accidentally treated as a lightmap. */
enum ph_serial_op {
   PH_SERIAL_UNSUPPORTED,
   PH_SERIAL_MODULATE,
   PH_SERIAL_ADD,
   PH_SERIAL_REPLACE1,
};

/* Why a draw stayed off the hardware transform (PARHELIA_STATS). */
enum ph_tcl_reason {
   PH_TCL_NO_UNAVAILABLE,     /* no WARP transform, no or uncompiled program */
   PH_TCL_NO_PRIMITIVE,       /* points, lines or polygon-mode outlines */
   PH_TCL_NO_FIXUP,           /* logic-op grid or uniform coordinate stage */
   PH_TCL_NO_UNIT_REMAP,
   PH_TCL_NO_TWO_SIDED_STENCIL,
   PH_TCL_NO_CLIP_PLANES,
   PH_TCL_NO_POLYGON_OFFSET,
   PH_TCL_NO_FLATSHADE,
   PH_TCL_NO_RASTER_STATE,    /* discard, half-z, depth clamp, cull both */
   PH_TCL_NO_COLOUR1,         /* secondary colour or fog in v1 */
   PH_TCL_NO_RGBA_COLOUR,
   PH_TCL_NO_ALPHA_ROUTE,
   PH_TCL_NO_PROGRAM_IO,
   PH_TCL_NO_TEXCOORD,
   PH_TCL_NO_MULTITEXTURE,
   PH_TCL_NO_THREE_COORD,
   PH_TCL_NO_DRAW_SHAPE,      /* restart, instancing, mode, vertex format */
   PH_TCL_NO_CONSTANTS,
   PH_TCL_REASONS
};

static const char *const ph_tcl_reason_names[PH_TCL_REASONS] = {
   "unavailable", "points/lines/outlines", "vertex fix-up", "unit remap",
   "two-sided stencil", "user clip planes", "polygon offset", "flat shading",
   "raster state", "colour 1", "RGBA colour", "alpha route",
   "program inputs/outputs", "texture coordinate", "multitexture",
   "three-coordinate texture", "draw shape", "constants",
};

struct ph_fs {
   struct draw_fragment_shader *draw;
   bool uses_texture;
   bool uses_color;
   uint32_t sampler_mask;
   uint32_t projective_mask;   /* samplers looked up with a projector */
   /* Why draws with this program are dropped, or NULL. */
   const char *drop_reason;
   /* The program's samplers packed onto units 0.., lowest first, when it
    * leaves a gap: unit u samples the GL unit unit_source[u]. */
   bool unit_remap;
   uint8_t unit_source[P3D_MAX_TEXTURE_UNITS];
   enum ph_serial_op serial_op;
   enum p3d_texenv texenv;
   unsigned coord_semantic;
   unsigned coord_index;
   /* The data-flow compiler's result.  Valid only for shaders whose colour
    * expression the combiner can evaluate; everything else keeps the
    * instruction-shape matcher above and the draw module. */
   struct ph_fs_program program;
   /* The program saturates the colour, exact for fixed-point targets. */
   bool colour_saturated;
   /* The program reads gl_PointCoord's screen derivatives (driver constants
    * staged per batch). */
   bool point_deriv;
   /* The evaluator of a program that samples its own lookup texture. */
   struct ph_fs_lookup *lookup;
   /* A discard folded into the alpha test, see ph_fs_build_kill(). */
   struct ph_fs_kill *kill;
   /* Samples whose coordinate is a uniform (a constant glTexCoord), which
    * the vertex stage stages per vertex; see ph_fs_find_uniform_coords(). */
   struct ph_uniform_coord *ucoord;
   /* Further taps of a texture at other coordinates: unit samples GL unit
    * gl_unit at coord, an affine function of varyings that the vertex
    * stage evaluates per vertex (in the shader's own NIR). */
   struct ph_tap {
      uint8_t unit, gl_unit;
      nir_def *coord;
   } taps[P3D_MAX_TEXTURE_UNITS];
   unsigned tap_count;
   unsigned ucoord_count;
   /* glDrawPixels(GL_DEPTH_COMPONENT)'s shader: the depth output is channel
    * x of the sample of GL unit depth_unit, the colour the vertex colour. */
   bool depth_from_texture;
   unsigned depth_unit;
   /* gl_FragDepth from uniforms only: the program's depth, evaluated per
    * draw and given to every vertex, so the combiner runs the colour. */
   nir_def *depth_uniform;
   /* The vertex output each GL unit's samples take their coordinate from,
    * as ph_fs_find_coord_sources() traced it: unit k need not use set k. */
   /* The unit is sampled as a rectangle: unnormalised texel coordinates. */
   bool coord_rect[PIPE_MAX_SAMPLERS];
   bool coord_src_known[PIPE_MAX_SAMPLERS];
   uint8_t coord_src_name[PIPE_MAX_SAMPLERS];
   uint8_t coord_src_index[PIPE_MAX_SAMPLERS];
   /* This program with the polygon stipple's kill on unit stipple_unit. */
   struct ph_fs *stipple;
   unsigned stipple_unit;
   bool stipple_failed;
   /* This program with GL_POINT_SMOOTH's coverage on unit smooth_unit. */
   struct ph_fs *smooth;
   unsigned smooth_unit;
   bool smooth_failed;
   bool stipple_affine;    /* a stipple variant sampling nothing else */
   /* Samplers looked up with a depth comparison, and this program with the
    * comparison lowered onto the combiner, per bound function and swizzle. */
   uint32_t compare_mask;
   struct util_dynarray compare_variants;
};

/* Semantic index of the extra attribute carrying unit k's uniform coordinate,
 * above every TEXCOORD a vertex shader exports. */
#define PH_UNIFORM_COORD_INDEX(k) (32u + (k))
/* ... and of the staged coordinate of a floor ramp on unit k. */
#define PH_RAMP_COORD_INDEX(k) (40u + (k))

struct ph_uniform_coord {
   nir_def *coord, *projector;
   unsigned unit;          /* the GL unit, as ph_unit_texcoord() looks it up */
   unsigned comps;
   bool window;            /* the stipple's window position / 32 instead */
};

struct ph_transfer {
   struct pipe_transfer base;
   void *staging;
   void *wide;      /* a widened format's 32-bit texels, see ph_format_widened */
   bool blocks;     /* the host blocks of a decoded S3TC image */
};

/* A sampler view plus, for a first level the hardware cannot use as its
 * base, a private chain holding levels first..last. */
struct ph_sampler_view {
   struct pipe_sampler_view base;
   struct pipe_sampler_view *shadow_view;
   uint64_t shadow_generation;
   /* A copy written with its source view's swizzle: the border colour
    * needs the same swizzle. */
   bool baked, baked_depth;
   uint8_t baked_swizzle[4];
   /* The same image unswizzled: a comparison reads the depth itself. */
   struct pipe_sampler_view *compare_view;
};

struct ph_context;
static bool ph_fs_window_lookups_only(const struct ph_fs *fs);
static unsigned ph_sampler_tiled_pitch(unsigned width);
static unsigned ph_sampler_storage_pitch(struct pipe_screen *screen,
                                         unsigned width, unsigned cpp);
static unsigned ph_texture_cpp(enum pipe_format format);
static bool ph_prepare_rgb5a1(struct ph_context *ctx,
                              struct parhelia_resource *texture);
static int ph_prepare_alpha_companions(struct ph_context *ctx,
                                       struct parhelia_resource *texture,
                                       const struct pipe_sampler_state *sampler,
                                       bool native_only);
static uint64_t ph_texture_base_hash(const struct parhelia_resource *texture,
                                     bool tiled, unsigned *alpha_zero,
                                     unsigned *alpha_nonzero);
static int ph_nir_coord_location(nir_def *def);
static bool ph_nir_eval_vertex(struct ph_context *ctx, nir_def *def,
                               const struct vertex_header *v,
                               nir_const_value out[NIR_MAX_VEC_COMPONENTS],
                               unsigned depth);
static void ph_pass_unit_state(const struct ph_context *ctx, unsigned unit,
                               struct p3d_texture *t);
static const struct p3d_comb_program *ph_compiled_combiner(struct ph_context *ctx,
                                                          bool two_units);
static bool ph_copy_texture_box(struct ph_context *ctx,
                                struct pipe_resource *dst, unsigned dst_level,
                                int dstx, int dsty,
                                struct pipe_resource *src, unsigned src_level,
                                const struct pipe_box *src_box,
                                int dst_width, int dst_height);

/* The same texel in slice or cube face @layer.  A cube map's faces are whole
 * level images side by side, a volume's slices are interleaved tile by tile;
 * both are measured by parhelia-drm-test --texture-volume-probe. */
static size_t
ph_mip_layer_texel_offset(const struct ph_mip_layout *mip, unsigned level,
                          unsigned layer, unsigned x, unsigned y, bool linear,
                          unsigned cpp)
{
   if (mip->probe_linear && !linear)
      return mip->level[level].offset + (size_t)layer * mip->level[level].bytes +
             ((size_t)y * mip->level[level].width + x) * cpp;
   if (mip->level[level].face_tail && !linear)
      return mip->level[level].offset +
         ph_volume_texel_offset(x + mip->level[level].tail_x,
                                y + mip->level[level].tail_y, layer, 32,
                                mip->slices, cpp);
   /* The original family's cube faces follow each other in 32x32 cells,
    * which for one level is the faces stacked as rows of one image. */
   if (mip->native_64x32 && mip->cube && !linear) {
      const struct ph_mip_level *image = &mip->level[level];
      const size_t cells = image->pitch / 32 *
                           (ph_mip_align(image->height, 32) / 32);

      return ph_64x32_cell_texel_offset(image->offset / 0x2000 * 2 +
                                        image->half + layer * cells,
                                        x + image->tail_x,
                                        y + image->tail_y, image->pitch);
   }
   if (mip->native_64x32 && mip->slices && !linear)
      return mip->level[level].offset +
         ph_volume_texel_offset_64x32(x, y, layer, mip->level[level].pitch,
                                      mip->slices, cpp);
   if (mip->slices && !mip->cube && !linear)
      return mip->level[level].offset +
         ph_volume_texel_offset(x, y, layer, mip->level[level].pitch,
                                mip->slices, cpp);
   return ph_mip_texel_offset_cpp(mip, level, x, y, linear, cpp) +
          (size_t)layer * mip->level[level].bytes;
}

/* R8 and R8G8 are stored as the native B8G8R8A8 texel (r, g or 0, 0, 1),
 * which the sampler decodes to exactly what GL_RED and GL_RG define; render
 * and clear paths keep the missing channels at those values. */
static bool
ph_format_widened(enum pipe_format format)
{
   return format == PIPE_FORMAT_R8_UNORM || format == PIPE_FORMAT_R8G8_UNORM;
}

/* The GL channels a colour target holds, for the write masks. */
static unsigned
ph_target_channels(enum pipe_format format)
{
   return ph_format_widened(format) ?
          util_format_colormask(util_format_description(format)) :
          PIPE_MASK_RGBA;
}

static unsigned
ph_texture_cpp(enum pipe_format format)
{
   struct ph_sampler_format native;

   return ph_sampler_format(format, &native) ? native.cpp : 4;
}

/* Fill the format half of a texture descriptor.  An untiled sampler reads the
 * paired linear shadow (layout 0); a tiled one selects the storage order for
 * its texel size.  @packed marks the temporary A1R5G5B5 repacking of an
 * otherwise 32-bit image. */
static void
ph_texture_descriptor_format(const struct parhelia_resource *texture,
                             bool packed, bool tiled, struct p3d_texture *tex)
{
   struct ph_sampler_format native = { 0x32, 4 };

   if (packed)
      native = (struct ph_sampler_format){ 0x26, 2 };
   else if (!ph_sampler_format(texture->base.format, &native) &&
            (texture->base.format == PIPE_FORMAT_R8G8B8X8_UNORM))
      native.code = 0x33;
   tex->fmt = native.code;
   /* Layout 5 makes the storage and the width/height fields describe 4x4
    * blocks; a compressed image has no linear companion to fall back to. */
   tex->texelcode = native.block_bytes ? 5 :
                    tiled ? ph_sampler_texelcode(native.cpp) : 0;
}

static unsigned
ph_resource_mip_levels(const struct parhelia_resource *r)
{
   return r->mip.levels ? r->mip.levels : 1;
}

enum ph_stat_kind {
   PH_STAT_OTHER, PH_STAT_CLEAR, PH_STAT_COLOUR, PH_STAT_TEX_LINEAR,
   PH_STAT_TEX_TILED, PH_STAT_TEX_MIP, PH_STAT_MULTITEX, PH_STAT_SERIAL,
   PH_STAT_ALPHA_TWO_PASS, PH_STAT_ALPHA_MICRO, PH_STAT_KINDS,
};

static const char *const ph_stat_kind_names[PH_STAT_KINDS] = {
   "other", "clear", "colour", "tex-linear", "tex-tiled", "tex-mip",
   "multitex", "serial-lightmap", "alpha-two-pass", "alpha-micro",
};

struct ph_context {
   struct pipe_context base;
   struct draw_context *draw;
   /* Measured on the P690 and the original Parhelia (0528): the Z unit's
    * trunc(d * 2^n) grid, the raster op's wide source colour, and the
    * combiner alpha that carries a discard into the alpha test. */
   bool z_grid, logicop_grid, kill_alpha;
   bool broadcasting;      /* ph_draw_broadcast is replaying a draw */
   /* Polygon stipple: the pattern as a 32x32 kill texture, see
    * ph_draw_stippled(). */
   uint32_t stipple[32];
   bool stipple_dirty, stippling;
   bool comparing;
   /* Smooth points as sprites with a coverage table, one per point size. */
   bool smoothing;
   struct pipe_rasterizer_state smooth_rast;
   struct ph_coverage_tex {
      float size;
      struct pipe_resource *tex;
      struct pipe_sampler_view *view;
      uint64_t last_use;
   } *coverage;
   unsigned coverage_count, coverage_cap;
   uint64_t coverage_clock;
   void *coverage_sampler;
   struct pipe_resource *stipple_tex;
   struct pipe_sampler_view *stipple_view;
   void *stipple_sampler;
   /* The floor ramps fragment programs sample, built on first use. */
   struct ph_ramp_tex {
      struct ph_fs_ramp key;
      struct pipe_resource *tex;
      struct pipe_sampler_view *view;
   } *ramp_tex;
   unsigned ramp_tex_count, ramp_tex_cap;
   void *ramp_sampler;
   bool ramping;
   /* Lookup textures of fragment programs (ph_lookup_view). */
   struct ph_lookup_tex {
      uint32_t uid;
      uint32_t *key;          /* the uniform values the table was built for */
      unsigned key_count, size_px;
      size_t bytes;
      uint64_t last_use;
      struct pipe_resource *tex;
      struct pipe_sampler_view *view;
   } *lookup_tex;
   unsigned lookup_tex_count, lookup_tex_cap;
   size_t lookup_bytes;
   uint64_t lookup_clock;
   void *lookup_sampler;
   /* The domain each varying carrier is staged in, (x - lo) / span, and
    * a compiled combiner refusing the batch it was asked for. */
   float dom_lo[PH_FS_CARRIERS][4], dom_span[PH_FS_CARRIERS][4];
   /* gl_PointCoord's screen derivatives over the staged sprites. */
   float pntc_deriv[4];
   bool comb_refused;
   /* The framebuffer as the state tracker set it, and the stand-in render
    * resources for surfaces the engine cannot address in place (a mip level
    * above 0 or a slice): ctx->fb binds the stand-ins. */
   struct pipe_framebuffer_state fb_api;
   /* The engine always writes a colour target: a framebuffer without one
    * draws into this scratch target with every colour plane masked. */
   struct pipe_resource *colour_stub;
   bool colour_stub_bound;
   struct pipe_blend_state *blend_api, blend_nocolour;
   struct ph_level_proxy {
      struct pipe_resource *res, *proxy;
      unsigned level, layer;
   } proxy[PIPE_MAX_COLOR_BUFS + 1];
   unsigned proxy_count;
   /* The native rasterise stage, and the logic-op colour grid in front. */
   struct draw_stage *hw_stage, *quant_stage, *zpix_stage;
   /* A fragment shader stand-in that passes the vertex colour, bound while
    * ph_zpix_stage feeds the native point path. */
   struct ph_fs colour_fs;
   struct pipe_transfer *zpix_transfer;
   const uint8_t *zpix_map;
   unsigned zpix_width, zpix_height;
   enum pipe_format zpix_format;
   bool zpix_unnormalized;
   struct pipe_framebuffer_state fb;
   struct pipe_rasterizer_state *rasterizer;
   struct pipe_depth_stencil_alpha_state *dsa;
   struct pipe_blend_state *blend;
   struct pipe_blend_color blend_color;
   struct pipe_stencil_ref stencil_ref;
   struct pipe_scissor_state scissor;
   struct ph_velems *velems;
   struct ph_fs *fs;
   struct pipe_sampler_state *samplers[PIPE_MAX_SAMPLERS];
   /* Effective views: a bound view whose first level cannot be the texture
    * unit's base is replaced by its shadow chain's view at draw time. */
   struct pipe_sampler_view *sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   struct pipe_sampler_view *bound_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   struct pipe_sampler_state *bound_samplers[PIPE_MAX_SAMPLERS];
   unsigned num_bound_views, num_bound_samplers;
   unsigned num_samplers;
   unsigned num_sampler_views;
   struct pipe_resource *constants[MESA_SHADER_STAGES];
   const void *user_constants[MESA_SHADER_STAGES];
   /* A hardware-transform batch ran since the last grouped passthrough
    * triangle packet; see ph_restore_after_transform(). */
   bool restore_after_transform;
   /* The fragment constants the staged draws were issued with. */
   uint8_t *fs_const_snap;
   unsigned fs_const_snap_size, fs_const_snap_cap;
   unsigned constant_offset[MESA_SHADER_STAGES];
   unsigned constant_size[MESA_SHADER_STAGES];
   uint32_t warp_passthrough;
   uint32_t warp_textured;
   uint32_t warp_textured_multitex;
   uint32_t warp_replace;
   /* Hardware transform: the kernel's WARP transform programs and the
    * constant loader that uploads the matrix and viewport. */
   uint32_t warp_transform;
   uint32_t warp_transform_textured;
   uint32_t warp_transform_multitex;
   uint32_t warp_constant_loader;
   /* The WARP_PROGRAM upload ABI: instructions one slot holds and how many
    * slots this file may hold.  Zero when the kernel has no upload path. */
   unsigned warp_program_instructions;
   unsigned warp_program_slots;
   unsigned warp_programs_held;
   /* A point wider than one pixel is rasterised by the hardware: the program
    * states the extent as o9 = (x-r, y-r, x+r, y+r) and 0x1130 carries
    * primitive class 3 (parhelia-point-size-re.md section 12).  The radius is
    * constant c0.x, so a batch holds points of one size and @point_radius is
    * the size the staged points carry. */
   uint8_t colour1;	/* second colour in the stream: p3d vertex_colour1 flags */
   bool oow;		/* the staged stream carries 1/w after z */
   bool proj;		/* set 0 carries s,t,r,q for the projective mode */
   bool colour_alpha;	/* the staged colour carries alpha: RGBA, not RGB */
   bool packed_colour;	/* the staged colour is one D3DCOLOR dword */
   bool packed_colour_fetch;	/* the fetch unpacks it (format 6) */
   /* An indexed batch: this many vertices staged, then triangles * 3 u32
    * indices into them. */
   unsigned indexed_verts;
   unsigned batch_index_dwords;	/* the indices of the batch being submitted */
   unsigned pending_index_dwords;	/* those in the pending aggregate */
   float *deindex;
   unsigned deindex_cap;
   uint32_t *tcl_seq;
   unsigned tcl_seq_cap;
   /* a submit without its repeated plain register writes */
   bool drop_repeats;
   struct parhelia_batch *repeat_batch;
   /* A smooth line's coverage rides in the colour's alpha: the staged
    * triangles carry their final colour and bypass the fragment program. */
   bool line_coverage;
   bool ov_valid;		/* ph_stage_tri takes ov_colour, not the vertex's */
   bool fog_splitting;	/* staging the pieces of a triangle cut at fogc 0 */
   float ov_colour[3][4];
   uint32_t warp_sized_point;
   uint32_t warp_sized_point_slot;
   /* Pass-through plus "mov o2, v6": the second colour reaches the combiner
    * as v1, which is where a lowered GL fog expression reads its coordinate. */
   uint32_t warp_colour1;
   /* The textured pass-through plus "mov o2, v6", for a textured draw whose
    * program reads v1 (GL_SEPARATE_SPECULAR_COLOR). */
   uint32_t warp_colour1_tex;
   /* Pass-through without the T2 write, for the RGBA colour stream. */
   uint32_t warp_rgba;
   uint32_t warp_no_colour[2];
   /* The pass-through moving o3..o6 from v7..v10, for three or four
    * coordinate sets (texture_units_probe on the 0528). */
   uint32_t warp_four_sets;
   /* Units past the second the staged triangles carry coordinates for. */
   unsigned extra_units;
   float point_radius;
   bool tcl_available;
   bool tcl_forced_off;
   bool tcl_multitexture;
   bool tcl_programs;
   bool have_viewport;
   struct ph_vs *vs;
   struct pipe_viewport_state viewport;      /* depth range on the Z grid */
   struct pipe_viewport_state api_viewport;  /* as the state tracker set it */
   /* Set while the pending batch carries object-space vertices.  mvp holds
    * the WARP c0..c3 rows, i.e. the transpose of the shader's matrix. */
   bool transform;
   float mvp[16];
   /* Set while the pending batch runs a compiled vertex program: its entry and
    * the constant block the loader has to publish before the draw. */
   uint32_t warp_program_entry;
   float *vs_const, *vs_const_pending;
   unsigned vs_const_vec4s, vs_const_cap;
   unsigned vp_slot;
   struct translate_cache *tcl_cache;
   uint32_t *tcl_elts;
   unsigned tcl_elts_cap;
   unsigned tcl_draws, tcl_fallbacks, tcl_clip_fallbacks;
   unsigned tcl_reason[PH_TCL_REASONS];
   uint64_t tcl_reason_verts[PH_TCL_REASONS];
   uint64_t tcl_verts, tcl_clip_verts;
   uint64_t tcl_range_verts;    /* vertices the draws' index ranges span */
   unsigned tcl_clip_why[4];	/* w <= 0, guard band, near, far */
   unsigned tcl_clipped_tris;   /* clipped on the CPU, kept transforming */
   float *tcl_clip_copy;
   unsigned tcl_clip_cap;
   enum ph_tcl_reason tcl_last_reason;
   unsigned tcl_program_draws;
   unsigned tcl_box_draws;      /* cleared by the position box, no per-vertex test */
   unsigned tcl_slice_draws;    /* bounded through a CPU position slice */
   uint32_t fence;
   bool async_pending;
   int submit_error;
   /* The one depth resource whose hardware clear is still outstanding.  The
    * Z unit tracks a single surface, so arming another resolves this one. */
   struct parhelia_resource *zinit_res;
   /* Consecutive state batches share one ioctl until an ABI bound or a CPU
    * visibility boundary is reached.  Resource references keep every GEM
    * handle alive until the kernel has copied and pinned the aggregate. */
   struct parhelia_batch pending_batch;
   /* Reused per-draw builder storage keeps the expanded aggregate command
    * allocation off the hot draw-call stack. */
   struct parhelia_batch build_batch;
   /* The aggregate's vertices: in the ring slot it will be submitted from
    * while a ring is mapped, else in pending_vertex_store. */
   uint32_t *pending_vertices;
   uint32_t *pending_vertex_store;
   /* built state prefixes, reused while a state repeats */
   struct parhelia_state_cache *state_cache;
   /* packed CALLs of the batch being submitted (-1 unknown), and an upper
    * bound for the pending aggregate (UINT_MAX unknown) */
   int batch_calls;
   unsigned batch_stages;	/* its vertex-program binds, with batch_calls */
   unsigned pending_call_bound;
   int pending_ring_slot;        /* -1: not in the ring */
   uint32_t pending_ring_fence;  /* last submit that read the slot */
   unsigned pending_vertex_dwords;
   struct parhelia_batch_limits batch_limits;
   unsigned pending_submit_count;
   unsigned pending_stage_calls;
   struct parhelia_batch_mark pending_marks[PH_BATCH_PACKETS];
   unsigned pending_mark_count;
   struct pipe_resource *pending_resources[PH_BATCH_RELOCS];
   unsigned pending_resource_count;
   /* Raw aliases into pending_resources.  They identify colour/depth writes
    * so a CPU/GPU resolve of an unrelated sampled texture need not publish
    * the whole userspace aggregate first. */
   struct pipe_resource *pending_writes[PH_BATCH_RELOCS];
   unsigned pending_write_count;
   bool aggregate_batches;
   bool aggregate_state_batches;
   bool aggregate_state_stream;
   /* Primitives carried by one draw packet.  The vendor's own packets hold
    * 24 vertices; one packet per primitive is the conservative fallback. */
   unsigned prims_per_packet;
   /* Submission fallback is a property of this context.  One client's
    * transient DMA allocation failure must not change another context's
    * execution path through mutable pipe_screen state. */
   bool async_submit;
   bool pack_adjacent_packets;
   /* Hardware-proven P690 one-pass diffuse x lightmap path. */
   bool p690_multitexture;
   bool trace_multitex_emitted;
	/* PARHELIA_TRACE records each depth-alpha gate outcome once.  This is
	 * diagnostic state only; it never participates in submission. */
   uint32_t trace_depth_alpha_seen;
	/* A bounded exact-state histogram for the unresolved texture-alpha route.
	 * This is populated only by PARHELIA_TRACE_ALPHA_TEST and keeps a Q3 trace
	 * useful without printing every cutout triangle. */
	uint64_t trace_alpha_test_state[16];
	unsigned trace_alpha_test_count;
	/* PARHELIA_TRACE_BLEND_ALPHA fingerprints the distinct textured blend
	 * states that need sampled alpha.  Keep it bounded: the Q3 trace must be
	 * inspectable without making an ordinary frame log-sized. */
	uint64_t trace_blend_alpha_state[512];
	unsigned trace_blend_alpha_count;
	/* The single proven RGBA setup is selected per pending batch.  It is
	 * cleared on submission so no later RGB draw can inherit its layout. */
	bool vertex_alpha;
	/* The texture unit does not propagate sampled alpha on this generation.
	 * Small source-alpha HUD textures are instead split into texel-scale
	 * primitives carrying sampled alpha in the proven vertex-alpha slot. */
	bool alpha_micro;
	/* Binary alpha-test counterpart to alpha_micro.  Transparent cells are
	 * omitted before submission, preserving alpha-test's no-colour/no-Z-write
	 * behaviour while opaque cells remain native textured GPU triangles. */
	bool alpha_micro_cutout;
	/* An opt-in, image-fingerprinted CPU path for the P690 texture-alpha
	 * hole.  Its ten-float stream is never submitted to the GPU. */
	bool cpu_alpha_cutout;
	const struct parhelia_resource *cpu_cutout_hash_resource;
	uint32_t cpu_cutout_hash_handle;
	uint64_t cpu_cutout_hash_generation;
	uint64_t cpu_cutout_hash;
	/* The one observed Q3 source-alpha UI route uses two generated RGB
	 * companions and two native draws.  Its ten-float staging stream retains
	 * RGBA so each hardware pass can be packed exactly. */
	bool rgb_alpha_two_pass;
	/* One-draw P690 route: unit 0 supplies RGB and the alpha companion on
	 * unit 1 supplies fragment alpha for blending or the comparator. */
	bool native_sampled_alpha;
	/* Process-start diagnostic selection for the two native sampled-alpha
	 * classes.  Parse it once; this predicate runs for every triangle. */
	unsigned native_sampled_alpha_modes;
   /* Hashing a tiled RGBA base level is O(texture pixels).  The draw frontend
    * calls the state predicate per triangle, so retain the result until this
    * texture receives a new linear upload. */
   const struct parhelia_resource *cpu_alpha_hash_resource;
   uint32_t cpu_alpha_hash_handle;
   uint64_t cpu_alpha_hash_generation;
   uint64_t cpu_alpha_hash;
   bool cpu_alpha_hash_zero, cpu_alpha_hash_nonzero;
   /* Kept separately from the functional hash cache so the selector can be
    * inspected without producing one line per Q3 triangle. */
	const struct parhelia_resource *trace_cpu_alpha_resource;
	const struct parhelia_resource *trace_cpu_alpha_stage_resource;
	const struct parhelia_resource *trace_alpha_cutout_resource;
	const struct parhelia_resource *trace_alpha_source_resource;
	const struct parhelia_resource *trace_native_alpha_resource;
	const struct parhelia_resource *alpha_micro_hash_resource;
	uint64_t alpha_micro_hash_generation;
	uint64_t alpha_micro_hash;
   /* The widest packet has XY, Z, RGB and two ST sets: ten dwords per vertex.
    * Keep the CPU staging buffer sized for that form even though simpler
    * draws use five, six, or seven dwords. */
   float vertex_store[PH_BATCH_MAX_DRAWS * 3 * 10];
   /* Where the current batch is staged: the aggregate's tail in its ring
    * slot when a ring is mapped (it is submitted from there, no copy),
    * else vertex_store.  stage_room is the ring slot's room, 0 otherwise. */
   float *vertices;
   unsigned stage_room;
   /* The fallback P690 path packs each logical texture stage for its serial
    * draw.  Proven one-pass MODULATE uses vertices[] directly. */
   float packed_vertices[PH_BATCH_MAX_DRAWS * 3 * 8];
	/* P690-only persistent class-3 vertex ring. The pointer is a GEM mmap,
	 * never a GPU address; all draw ranges remain kernel relocations. */
	float *vram_vertices;
	uint64_t vram_vertex_size;
	uint32_t vram_vertex_handle;
	uint32_t vram_vertex_fence[PH_VRAM_VERTEX_SLOTS_MAX];
	unsigned vram_vertex_slot;
	unsigned vram_vertex_slot_count;
	unsigned vram_vertex_slot_dwords;
	bool vram_vertex_enabled;
   unsigned triangles;
   unsigned lines;
   unsigned points;
   /* The current draw's triangles come from expanded points or lines or
    * unfilled polygons: GL never face-culls those. */
   bool derived_triangles;
   bool stencil_back;	/* the staged triangles face back: stencil[1] */
   /* Viewport 0 maps GL window y downwards (window-system framebuffer). */
   bool viewport_y_flip;
   /* PARHELIA_STATS: submission counters averaged over 100 frames.
    * PARHELIA_STATS=sync additionally times every submit to its fence. */
   bool stats, stats_sync;
   unsigned stat_kind;
   double stat_pixels;
   struct {
      unsigned frames, batches, draw_batches, triangles, submits, clears;
      unsigned draw_packets;
      unsigned waits, stage_evictions;
      uint64_t repeats_dropped;
      uint64_t vertex_dwords, in_place_dwords, clear_pixels, wait_ns, start_ns;
      unsigned kind_count[PH_STAT_KINDS];
      uint64_t kind_ns[PH_STAT_KINDS];
      double kind_pixels[PH_STAT_KINDS];
   } stat;
};

struct ph_stage {
   struct draw_stage base;
   struct ph_context *ctx;
};

enum ph_kill_route {
   PH_KILL_NONE,          /* no discard */
   PH_KILL_ALPHA_X,
   PH_KILL_ALPHA_SUB,
   PH_KILL_ALL,           /* the GL alpha test rejects every fragment */
   PH_KILL_UNSUPPORTED,
};

static enum ph_kill_route
ph_fs_kill_route(struct ph_context *ctx, struct ph_fs_program **prog,
                 enum pipe_compare_func *func, uint8_t *ref);
static bool ph_fs_untextured_program(struct ph_context *ctx);
static bool ph_nir_def_uniform(nir_def *def, unsigned depth);
static void ph_proxy_writeback(struct ph_context *ctx);
static void ph_proxy_release(struct ph_context *ctx);
static float ph_depth_window(double d, unsigned bits);

static inline struct ph_context *
ph_context(struct pipe_context *p)
{
   return (struct ph_context *)p;
}

/* Texture unit 0 is in use only when the fragment shader samples it; the
 * state tracker may leave a view bound after texturing is disabled. */
static bool
ph_samples_unit0(const struct ph_context *ctx)
{
   return ctx->fs && (ctx->fs->sampler_mask & BITFIELD_BIT(0)) &&
          ctx->num_sampler_views && ctx->sampler_views[0] &&
          ctx->sampler_views[0]->texture;
}

/* The fixed-function two-unit routes compute alpha as t0.a * v0.a * t1.a; a
 * shape classified with a unit's alpha taken as 1.0 is exact only while that
 * unit holds an alpha-less format. */
static bool
ph_serial_alpha_exact(const struct ph_context *ctx)
{
   const struct ph_fs *fs = ctx->fs;

   for (unsigned u = 0; u < 2; u++) {
      const struct pipe_sampler_view *view =
         u < ctx->num_sampler_views ? ctx->sampler_views[u] : NULL;

      if (!(fs->program.valid && (fs->program.alpha_one_mask & (1u << u))))
         continue;
      if (!view || util_format_has_alpha(view->format))
         return false;
   }
   return true;
}

/* The window position of a staged vertex for PARHELIA_STATS=sync.  An
 * object-space vertex of the fixed-function transform goes through the
 * batch's matrix and viewport; a compiled vertex program's output is not
 * known on the CPU, so its triangles count as empty. */
static void
ph_stat_window_xy(const struct ph_context *ctx, const float *v, float out[2])
{
   const float *m = ctx->mvp;
   float x, y, w;

   if (!ctx->transform) {
      out[0] = v[0];
      out[1] = v[1];
      return;
   }
   if (ctx->warp_program_entry) {
      out[0] = out[1] = 0.0f;
      return;
   }
   x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
   y = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
   w = m[12] * v[0] + m[13] * v[1] + m[14] * v[2] + m[15];
   w = w > 1e-6f ? w : 1e-6f;
   out[0] = ctx->viewport.scale[0] * x / w + ctx->viewport.translate[0];
   out[1] = ctx->viewport.scale[1] * y / w + ctx->viewport.translate[1];
}

/* Whether a draw may bind the second unit the fragment shader samples.  A
 * single-pipe chip runs only the classified two-unit shapes, and only when
 * every unit whose alpha the shape takes as 1.0 holds an alpha-less format. */
static bool
ph_second_unit_bindable(const struct ph_context *ctx)
{
   const struct ph_fs *fs = ctx->fs;

   if (!fs || !(fs->sampler_mask & BITFIELD_BIT(1)))
      return false;
   if (!ph_screen(ctx->base.screen)->single_texture_pipe)
      return true;
   /* A compiled two-unit program runs in one pass whatever its algebra. */
   if (ctx->p690_multitexture && fs->program.valid &&
       !(fs->program.sampler_mask & ~3u))
      return true;
   return fs->serial_op != PH_SERIAL_UNSUPPORTED && ph_serial_alpha_exact(ctx);
}

static unsigned
ph_hardware_cull(const struct pipe_rasterizer_state *rasterizer)
{
   if (!rasterizer || rasterizer->cull_face == PIPE_FACE_NONE ||
       rasterizer->cull_face == PIPE_FACE_FRONT_AND_BACK)
      return 0;

   /* The P690 probe proves code 1 rejects det>0 (the draw module's CW
    * winding) and code 2 rejects det<0 (CCW). */
   if (rasterizer->cull_face == PIPE_FACE_BACK)
      return rasterizer->front_ccw ? 1 : 2;
   if (rasterizer->cull_face == PIPE_FACE_FRONT)
      return rasterizer->front_ccw ? 2 : 1;
   return 0;
}

/* GL two-sided stencil, which the engine has no state for (one stencil block,
 * parhelia-fragment-ops.md section 5): triangles are split into single-face
 * batches, each emitted with its face's stencil state. */
static bool
ph_two_sided_stencil(const struct ph_context *ctx)
{
   const struct pipe_stencil_state *s = ctx->dsa ? ctx->dsa->stencil : NULL;

   return s && s[0].enabled && s[1].enabled && ctx->fb.zsbuf.texture &&
          util_format_has_stencil(util_format_description(ctx->fb.zsbuf.format)) &&
          (memcmp(&s[0], &s[1], sizeof(s[0])) ||
           ctx->stencil_ref.ref_value[0] != ctx->stencil_ref.ref_value[1]);
}

/* The draw module's own facing rule (draw_pipe_cull.c) on window-space
 * positions; derived primitives, points and lines take the front state. */
static bool
ph_triangle_back_facing(const struct ph_context *ctx,
                        const struct prim_header *prim, int pos)
{
   const float *v0 = prim->v[0]->data[pos], *v1 = prim->v[1]->data[pos];
   const float *v2 = prim->v[2]->data[pos];
   const float det = (v0[0] - v2[0]) * (v1[1] - v2[1]) -
                     (v0[1] - v2[1]) * (v1[0] - v2[0]);

   return det != 0.0f && (det < 0.0f) != !!ctx->rasterizer->front_ccw;
}

static inline unsigned
ph_triangle_batch_limit(const struct ph_context *ctx)
{
   unsigned packets = ctx->batch_limits.packets;

   /* Every draw packet is one independently relocated 0x97/0x8f descriptor,
    * and one packet carries a run of triangles.  The limit stays at one
    * packet per triangle: the CPU staging buffers are sized for exactly that
    * many vertices.  Reserve room for the complete recovered state sequence;
    * the builder's own bounds remain the final guard if a future state
    * grows. */
   if (packets <= PH_BATCH_STATE_PACKET_RESERVE)
      return 1;
   return MIN2(PH_BATCH_MAX_DRAWS,
               packets - PH_BATCH_STATE_PACKET_RESERVE);
}

static inline unsigned
ph_pending_vertex_limit(const struct ph_context *ctx)
{
   return ctx->batch_limits.vertex_dwords;
}

static inline unsigned
ph_logical_vertex_limit(const struct ph_context *ctx)
{
   if (ctx->stage_room)
      return ctx->stage_room;
   /* CREATE_VERTEX relocates every triangle range independently, so the
    * logical stream may fill one negotiated ring slot while every hardware
    * fetch remains only 3 * dwords-per-vertex. */
   return ctx->vram_vertex_enabled ? ctx->batch_limits.vertex_dwords :
                                     PH_BATCH_LEGACY_DWORDS;
}

static int ph_drain_batch_at(struct ph_context *ctx, unsigned line);
/* PARHELIA_TRACE_DRAIN=1 counts aggregate drains per call site. */
#define ph_drain_batch(ctx) ph_drain_batch_at((ctx), __LINE__)
static struct {
   unsigned *count;
   unsigned cap;
} ph_drain_sites;

static uint64_t
ph_now_ns(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void ph_hang_write(void);

static int
ph_wait_fence_counted(struct ph_context *ctx, uint32_t fence)
{
   uint64_t start = ctx->stats ? ph_now_ns() : 0;
   int ret = parhelia_wait_fence(ph_screen(ctx->base.screen)->fd, fence, -1,
                                 NULL);

   if (ctx->stats) {
      ctx->stat.waits++;
      ctx->stat.wait_ns += ph_now_ns() - start;
   }
   if (ret == -EIO)
      ph_hang_write();
   return ret;
}

static void
ph_stats_frame(struct ph_context *ctx)
{
   const double n = 100.0;
   uint64_t now;

   if (!ctx->stats || ++ctx->stat.frames < 100)
      return;
   for (unsigned l = 0; l < ph_drain_sites.cap; l++)
      if (ph_drain_sites.count[l]) {
         fprintf(stderr, "parhelia: drains/frame from line %u: %.1f\n", l,
                 ph_drain_sites.count[l] / 100.0);
         ph_drain_sites.count[l] = 0;
      }
   now = ph_now_ns();
   if (ctx->stat.start_ns)
      fprintf(stderr,
              "parhelia stats/frame: %.2f ms, %.0f batches (%.0f draw), %.0f triangles, %.0f draw packets, %.0f submits, %.0f vertex dwords (%.0f staged in place, %.1f moved out), %.1f clears (%.2f Mpixel), %.1f waits %.2f ms\n",
              (now - ctx->stat.start_ns) / n / 1e6, ctx->stat.batches / n,
              ctx->stat.draw_batches / n, ctx->stat.triangles / n,
              ctx->stat.draw_packets / n,
              ctx->stat.submits / n, ctx->stat.vertex_dwords / n,
              ctx->stat.in_place_dwords / n, ctx->stat.stage_evictions / n,
              ctx->stat.clears / n, ctx->stat.clear_pixels / n / 1e6,
              ctx->stat.waits / n, ctx->stat.wait_ns / n / 1e6);
   if (ctx->stat.start_ns && ctx->drop_repeats)
      fprintf(stderr, "parhelia stats/frame: %.0f repeated register writes dropped\n",
              ctx->stat.repeats_dropped / n);
   if (ctx->stat.start_ns && (ctx->tcl_draws || ctx->tcl_fallbacks ||
                              ctx->tcl_reason[PH_TCL_NO_PRIMITIVE] ||
                              ctx->tcl_reason[PH_TCL_NO_FIXUP]))
      fprintf(stderr,
              "parhelia stats/frame: %.1f hardware-transform draws (%.1f compiled vertex programs, %.1f cleared by the position box, %.1f bounded by a position slice), %.1f not expressible, %.1f handed back to the software clipper\n",
              ctx->tcl_draws / n, ctx->tcl_program_draws / n,
              ctx->tcl_box_draws / n, ctx->tcl_slice_draws / n,
              ctx->tcl_fallbacks / n, ctx->tcl_clip_fallbacks / n);
   if (ctx->stat.start_ns && (ctx->tcl_draws || ctx->tcl_clip_fallbacks))
      fprintf(stderr, "  hardware transform: %.0f vertices over index ranges "
              "of %.0f, %.1f triangles "
              "clipped; software clipper tails: %.0f vertices (w<=0 %.1f, "
              "guard band %.1f, near %.1f, far %.1f)\n", ctx->tcl_verts / n,
              ctx->tcl_range_verts / n,
              ctx->tcl_clipped_tris / n, ctx->tcl_clip_verts / n,
              ctx->tcl_clip_why[0] / n, ctx->tcl_clip_why[1] / n,
              ctx->tcl_clip_why[2] / n, ctx->tcl_clip_why[3] / n);
   for (unsigned r = 0; ctx->stat.start_ns && r < PH_TCL_REASONS; r++)
      if (ctx->tcl_reason[r])
         fprintf(stderr, "  off the hardware transform: %-24s %.1f draws %.0f vertices\n",
                 ph_tcl_reason_names[r], ctx->tcl_reason[r] / n,
                 ctx->tcl_reason_verts[r] / n);
   memset(ctx->tcl_reason, 0, sizeof(ctx->tcl_reason));
   memset(ctx->tcl_reason_verts, 0, sizeof(ctx->tcl_reason_verts));
   memset(ctx->tcl_clip_why, 0, sizeof(ctx->tcl_clip_why));
   ctx->tcl_verts = ctx->tcl_clip_verts = ctx->tcl_range_verts = 0;
   ctx->tcl_clipped_tris = 0;
   ctx->tcl_draws = ctx->tcl_fallbacks = ctx->tcl_clip_fallbacks = 0;
   ctx->tcl_program_draws = ctx->tcl_box_draws = ctx->tcl_slice_draws = 0;
   for (unsigned k = 0; ctx->stats_sync && k < PH_STAT_KINDS; k++)
      if (ctx->stat.kind_count[k])
         fprintf(stderr,
                 "  %-15s %6.1f submits %7.3f ms %6.3f Mpixel (%.0f Mpixel/s)\n",
                 ph_stat_kind_names[k], ctx->stat.kind_count[k] / n,
                 ctx->stat.kind_ns[k] / n / 1e6,
                 ctx->stat.kind_pixels[k] / n / 1e6,
                 ctx->stat.kind_ns[k] ?
                    ctx->stat.kind_pixels[k] * 1e3 / ctx->stat.kind_ns[k] : 0);
   memset(&ctx->stat, 0, sizeof(ctx->stat));
   ctx->stat.start_ns = now;
}

/* A queued class-3 submission owns its command and vertex copies in the
 * kernel, but a front-buffer handoff has to observe its terminal DWGSYNC tag.
 * Normal GPU-to-GPU resolves are ordered by the same kernel producer queue;
 * this wait is for CPU/scanout consumers. */
static int
ph_wait_async(struct ph_context *ctx)
{
   int ret;

   ret = ph_drain_batch(ctx);
   if (ret)
      return ret;
   if (!ctx->async_pending)
      return 0;
   if (ph_env("PARHELIA_TRACE_BATCH"))
      fprintf(stderr, "parhelia: aggregate wait fence=%u\n", ctx->fence);
   ret = ph_wait_fence_counted(ctx, ctx->fence);
   if (ret)
      ctx->submit_error = ret;
   else
      ctx->async_pending = false;
   return ret;
}

static void
ph_destroy_vertex_ring(struct ph_context *ctx)
{
   if (ctx->vram_vertices)
      munmap(ctx->vram_vertices, ctx->vram_vertex_size);
   if (ctx->vram_vertex_handle) {
      struct drm_gem_close close = {.handle = ctx->vram_vertex_handle};
      ioctl(ph_screen(ctx->base.screen)->fd, DRM_IOCTL_GEM_CLOSE, &close);
   }
   ctx->vram_vertices = NULL;
   ctx->pending_vertices = ctx->pending_vertex_store;
   ctx->pending_ring_slot = -1;
   ctx->vertices = ctx->vertex_store;
   ctx->stage_room = 0;
   ctx->vram_vertex_size = 0;
   ctx->vram_vertex_handle = 0;
   ctx->vram_vertex_slot_count = 0;
   ctx->vram_vertex_slot_dwords = 0;
   ctx->vram_vertex_enabled = false;
}

/* A failed optional allocation must never make the known-good DMA32 path
 * unavailable. CREATE_VERTEX is a P690 capability query, not a chip-id
 * guess, and a client only starts using it after mmap succeeds. */
/* CREATE_VERTEX has a 1 MiB allocation ABI on supported kernels.  A
 * deeper async queue cannot enlarge that allocation; retain complete slots. */
static unsigned
ph_vertex_ring_slot_count(unsigned async_slots, uint64_t slot_bytes)
{
   unsigned wanted = async_slots > PH_VRAM_VERTEX_LEGACY_SLOTS &&
                     async_slots <= PH_VRAM_VERTEX_SLOTS_MAX ?
                     async_slots - 1 : PH_VRAM_VERTEX_LEGACY_SLOTS;
   const uint64_t max_bytes = UINT64_C(1) << 20;

   if (!slot_bytes || slot_bytes > max_bytes)
      return 0;
   return MIN2(wanted, max_bytes / slot_bytes);
}

static void
ph_create_vertex_ring(struct ph_context *ctx)
{
   struct parhelia_batch_limits mapped_limits;
   struct parhelia_screen *screen = ph_screen(ctx->base.screen);
   uint64_t slot_bytes;
   unsigned slot_count;
   struct drm_parhelia_create_vertex create = {
      .size = 0,
   };
   void *map;

   parhelia_batch_negotiate_limits(screen->kernel_max_packets, true,
                                   &mapped_limits);
   slot_bytes = (uint64_t)mapped_limits.vertex_dwords * sizeof(uint32_t);
   /* Leave queue headroom, and keep the mapped allocation inside the
    * CREATE_VERTEX ABI even when a kernel reports sixteen async slots. */
   slot_count = ph_vertex_ring_slot_count(screen->async_slots, slot_bytes);
   if (!slot_count)
      return;
   create.size = slot_count * slot_bytes;
   if (!screen->vram_vertex ||
       ioctl(screen->fd, DRM_IOCTL_PARHELIA_CREATE_VERTEX, &create))
      return;
   if (!create.handle ||
       create.size < slot_count * slot_bytes) {
      if (create.handle) {
         struct drm_gem_close close = {.handle = create.handle};
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
      }
      return;
   }
   map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
              screen->fd, create.mmap_offset);
   if (map == MAP_FAILED) {
      struct drm_gem_close close = {.handle = create.handle};
      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
      return;
   }
   ctx->vram_vertices = map;
   ctx->vram_vertex_size = create.size;
   ctx->vram_vertex_handle = create.handle;
   ctx->vram_vertex_slot_count = slot_count;
   ctx->vram_vertex_slot_dwords = mapped_limits.vertex_dwords;
   ctx->vram_vertex_enabled = true;
   ctx->batch_limits = mapped_limits;
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: enabled P690 class-3 DMA vertex ring handle=%u bytes=%llu slots=%u slot_dwords=%u queue-depth=%u\n",
              create.handle, (unsigned long long)create.size,
              ctx->vram_vertex_slot_count, ctx->vram_vertex_slot_dwords,
              screen->async_slots);
}

/* The P690 and 0528 draw programs depend on a per-vertex latch (WARP T2)
 * that the setup unit rejects against like a clip-space position; only the
 * passthrough, multitexture and replace programs write it, from the vertex
 * colour, and every other program inherits it.  It exists once per primitive
 * slot of a draw packet: after power-on the second slot held a rejecting
 * value and Mesa's two-triangle packets lost their second triangle until a
 * two-primitive passthrough packet ran.  Prime every slot with black
 * triangles in packets as wide as this context's. */
static void
ph_prime_warp(struct ph_context *ctx)
{
   struct parhelia_screen *screen = ph_screen(ctx->base.screen);
   /* native colour objects are a P690 class; the original family primes
    * into a plain render object */
   struct drm_parhelia_create_render target = {
      .flags = screen->chip_id == 0x2539 ? PARHELIA_CREATE_RENDER_NATIVE_COLOR : 0,
      .width = 64, .height = 16,
   };
   struct parhelia_batch *batch = &ctx->build_batch;
   float point[PH_BATCH_MAX_PACKET_VERTICES][5];
   const unsigned group = MIN2(MAX2(ctx->prims_per_packet, 1u),
                               PH_BATCH_MAX_PACKET_VERTICES / 3);
   uint32_t fence = 0;

   if ((screen->chip_id != 0x2539 && !ph_original_family(screen->chip_id)) ||
       ioctl(screen->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &target))
      return;
   struct p3d_state state = {
      .colour = {.pitch = target.pitch / 4, .fmt = 0x32},
      .screen_w = 64, .screen_h = 16, .prim = P3D_TRIANGLES,
      .warp_entry = ctx->warp_passthrough, .dw_per_vertex = 5,
      .vertices_per_packet = 3 * group,
   };

   /* distinct small black triangles along the target */
   for (unsigned v = 0; v < 3 * group; v++) {
      point[v][0] = (float)((v / 3) * 4 % 60 + (v % 3 == 1) * 3);
      point[v][1] = (float)((v % 3 == 2) * 3);
      point[v][2] = point[v][3] = point[v][4] = 0.0f;
   }
   if (!parhelia_batch_state_run(batch, target.handle, 0, 0, 0, &state,
                                 3 * group) &&
       !parhelia_batch_submit_async(screen->fd, batch, point, 15 * group,
                                    &fence))
      parhelia_wait_fence(screen->fd, fence, 2000000000ll, NULL);
   struct drm_gem_close close_bo = {.handle = target.handle};
   ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
}

static bool
ph_batch_has_full_boundary(const struct parhelia_batch *batch)
{
   for (unsigned i = 0; i < batch->reloc_count; i++)
      if (batch->relocs[i].type == PARHELIA_RELOC_BATCH_BOUNDARY)
         return true;
   return false;
}

static int
ph_submit_batch_now_untimed(struct ph_context *ctx,
                            struct parhelia_batch *batch,
                            const void *vertices, unsigned vertex_dwords);

static void
ph_write_batch(FILE *dump, const struct parhelia_batch *batch,
               unsigned serial, uint32_t fence)
{
   fprintf(dump, "submit %u fence %#x packets %u dwords %u relocs %u\n",
           serial, fence, batch->packet_count, batch->command_count,
           batch->reloc_count);
   for (unsigned i = 0; i < batch->packet_count; i++) {
      const struct drm_parhelia_packet *packet = &batch->packets[i];

      fprintf(dump, " p%u:", i);
      for (unsigned w = 0; w < packet->length &&
                           packet->offset + w < batch->command_count; w++)
         fprintf(dump, " %08x", batch->commands[packet->offset + w]);
      fprintf(dump, "\n");
   }
   for (unsigned i = 0; i < batch->reloc_count; i++)
      fprintf(dump, " r%u: type %u packet %u dword %u handle %u delta %#x\n",
              i, batch->relocs[i].type, batch->relocs[i].packet,
              batch->relocs[i].dword, batch->relocs[i].handle,
              batch->relocs[i].delta);
}

/* PARHELIA_DUMP_BATCHES=path appends every submitted batch's packets. */
static void
ph_dump_batch(const struct parhelia_batch *batch, const void *vertices,
              unsigned vertex_dwords)
{
   static FILE *dump;
   static unsigned serial;
   const char *path;

   /* Asked once per batch, so the lookup itself is worth caching. */
   if (!ph_env("PARHELIA_DUMP_BATCHES"))
      return;
   path = getenv("PARHELIA_DUMP_BATCHES");
   if (!path)
      return;
   if (!dump && !(dump = fopen(path, "a")))
      return;
   ph_write_batch(dump, batch, serial++, 0);
   /* The vertex stream too, as dwords, when PARHELIA_DUMP_VERTICES is set. */
   if (vertices && ph_env("PARHELIA_DUMP_VERTICES")) {
      const uint32_t *v = vertices;

      fprintf(dump, " vertices %u:", vertex_dwords);
      for (unsigned i = 0; i < vertex_dwords; i++)
         fprintf(dump, "%s%08x", i % 16 ? " " : "\n  ", v[i]);
      fputc('\n', dump);
   }
   fflush(dump);
}

/* The last submitted batches, written to /tmp/parhelia-hang-PID.txt (or
 * PARHELIA_HANG_DUMP) when the kernel first reports an engine hang. */
#define PH_HANG_HISTORY 16

static struct {
   struct parhelia_batch *batch[PH_HANG_HISTORY];
   float *vertices[PH_HANG_HISTORY];
   unsigned vertex_dwords[PH_HANG_HISTORY];
   uint32_t fence[PH_HANG_HISTORY];
   unsigned next, serial;
   bool written;
} ph_hang;

static void
ph_hang_record(const struct parhelia_batch *batch, const void *vertices,
               unsigned vertex_dwords, uint32_t fence)
{
   unsigned slot;

   /* Copying every batch and its vertices cost a fifth of Q3's CPU time at
    * 1280x1024; keep the history only when PARHELIA_HANG_DUMP asks for it. */
   if (!ph_env("PARHELIA_HANG_DUMP"))
      return;
   slot = ph_hang.next++ % PH_HANG_HISTORY;

   if (!ph_hang.batch[slot] &&
       (!(ph_hang.batch[slot] = MALLOC(sizeof(*batch))) ||
        !(ph_hang.vertices[slot] = MALLOC(PH_BATCH_VERTEX_DWORDS * 4)))) {
      FREE(ph_hang.batch[slot]);
      ph_hang.batch[slot] = NULL;
      return;
   }
   memcpy(ph_hang.batch[slot], batch, sizeof(*batch));
   ph_hang.vertex_dwords[slot] = vertices ?
      MIN2(vertex_dwords, PH_BATCH_VERTEX_DWORDS) : 0;
   memcpy(ph_hang.vertices[slot], vertices ? vertices : ph_hang.vertices[slot],
          ph_hang.vertex_dwords[slot] * 4);
   ph_hang.fence[slot] = fence;
   ph_hang.serial++;
}

static void
ph_hang_write(void)
{
   char path[64];
   const char *env = getenv("PARHELIA_HANG_DUMP");
   FILE *dump;

   if (ph_hang.written)
      return;
   ph_hang.written = true;
   snprintf(path, sizeof(path), "/tmp/parhelia-hang-%d.txt", (int)getpid());
   dump = fopen(env ? env : path, "w");
   if (!dump)
      return;
   for (unsigned i = 0; i < PH_HANG_HISTORY; i++) {
      unsigned slot = (ph_hang.next + i) % PH_HANG_HISTORY;

      if (!ph_hang.batch[slot])
         continue;
      ph_write_batch(dump, ph_hang.batch[slot],
                     ph_hang.serial - PH_HANG_HISTORY + i,
                     ph_hang.fence[slot]);
      fprintf(dump, " vertices %u:", ph_hang.vertex_dwords[slot]);
      for (unsigned v = 0; v < ph_hang.vertex_dwords[slot]; v++)
         fprintf(dump, " %g", ph_hang.vertices[slot][v]);
      fprintf(dump, "\n");
   }
   fclose(dump);
   fprintf(stderr, "parhelia: engine hang, last batches written to %s\n",
           env ? env : path);
}

static int
ph_submit_batch_now(struct ph_context *ctx, struct parhelia_batch *batch,
                    const void *vertices, unsigned vertex_dwords)
{
   uint64_t start;
   int ret;

   if (ctx->drop_repeats &&
       (ctx->repeat_batch ||
        (ctx->repeat_batch = MALLOC(sizeof(*ctx->repeat_batch))))) {
      unsigned dropped;

      if (!parhelia_batch_drop_repeats(ctx->repeat_batch, batch, &dropped)) {
         batch = ctx->repeat_batch;
         ctx->stat.repeats_dropped += dropped;
      }
   }
   if (ph_env("PARHELIA_DUMP_BATCHES"))
      ph_dump_batch(batch, vertices, vertex_dwords);

   if (!ctx->stats_sync) {
      ret = ph_submit_batch_now_untimed(ctx, batch, vertices, vertex_dwords);
      if (!ret)
         ph_hang_record(batch, vertices, vertex_dwords, ctx->fence);
   } else {
      start = ph_now_ns();
      ret = ph_submit_batch_now_untimed(ctx, batch, vertices, vertex_dwords);
      /* Record before the wait: synchronous mode retires one batch at a time,
       * so the batch the engine stops on is the last one recorded. */
      if (!ret)
         ph_hang_record(batch, vertices, vertex_dwords, ctx->fence);
      if (!ret && ctx->async_pending) {
         ret = parhelia_wait_fence(ph_screen(ctx->base.screen)->fd, ctx->fence,
                                   -1, NULL);
         ctx->async_pending = ret != 0;
      }
      ctx->stat.kind_count[ctx->stat_kind]++;
      ctx->stat.kind_ns[ctx->stat_kind] += ph_now_ns() - start;
      ctx->stat.kind_pixels[ctx->stat_kind] += ctx->stat_pixels;
   }
   if (ret == -EIO)
      ph_hang_write();
   return ret;
}

/* The aggregate's vertices are written straight into the ring slot they are
 * submitted from: wait for that slot's previous reader once, when the first
 * vertices arrive, instead of copying the whole aggregate at submit. */
static int
ph_pending_ring_acquire(struct ph_context *ctx, bool block)
{
   unsigned slot;

   if (ctx->pending_ring_slot >= 0 || ctx->pending_vertex_dwords ||
       !ctx->vram_vertex_enabled || !ctx->async_submit ||
       ph_pending_vertex_limit(ctx) > ctx->vram_vertex_slot_dwords)
      return 0;
   slot = ctx->vram_vertex_slot;
   /* a drain inside a batch's submission leaves that batch staged in the
    * slot just released, until it is appended */
   if (ctx->stage_room && (ctx->triangles || ctx->points || ctx->lines) &&
       (const uint32_t *)ctx->vertices >=
          (const uint32_t *)ctx->vram_vertices +
          (size_t)slot * ctx->vram_vertex_slot_dwords &&
       (const uint32_t *)ctx->vertices <
          (const uint32_t *)ctx->vram_vertices +
          (size_t)(slot + 1) * ctx->vram_vertex_slot_dwords)
      slot = ctx->vram_vertex_slot = (slot + 1) % ctx->vram_vertex_slot_count;
   if (ctx->vram_vertex_fence[slot]) {
      int ret = block ?
         ph_wait_fence_counted(ctx, ctx->vram_vertex_fence[slot]) :
         parhelia_wait_fence(ph_screen(ctx->base.screen)->fd,
                             ctx->vram_vertex_fence[slot], 0, NULL);

      if (ret)
         return ret;
      ctx->vram_vertex_fence[slot] = 0;
   }
   ctx->pending_vertices = (uint32_t *)ctx->vram_vertices +
                           (size_t)slot * ctx->vram_vertex_slot_dwords;
   ctx->pending_ring_slot = slot;
   ctx->pending_ring_fence = 0;
   return 0;
}

/* After the aggregate's submits the slot stays busy until the last one. */
static void
ph_pending_ring_release(struct ph_context *ctx)
{
   if (ctx->pending_ring_slot < 0)
      return;
   if (ctx->pending_ring_fence) {
      ctx->vram_vertex_fence[ctx->pending_ring_slot] = ctx->pending_ring_fence;
      ctx->vram_vertex_slot = (ctx->pending_ring_slot + 1) %
                              ctx->vram_vertex_slot_count;
   }
   ctx->pending_ring_slot = -1;
   ctx->pending_vertices = ctx->pending_vertex_store;
}

/* Less room than this left in a ring slot drains the aggregate before the
 * next batch is staged, rather than staging it in a sliver. */
#define PH_STAGE_MIN_DWORDS 2048u

DEBUG_GET_ONCE_BOOL_OPTION(stage_ring, "PARHELIA_STAGE_RING", true)

/* Point the staging buffer at the aggregate's tail in its ring slot, so a
 * batch's vertices are written where they are submitted from; without a
 * ring, or while the next slot is still being read, at the private store.
 * Only while nothing is staged. */
static void
ph_stage_at_ring_tail(struct ph_context *ctx)
{
   unsigned left;

   if (ctx->triangles || ctx->points || ctx->lines)
      return;
   ctx->vertices = ctx->vertex_store;
   ctx->stage_room = 0;
   if (!ctx->vram_vertex_enabled || !ctx->async_submit ||
       !ctx->aggregate_batches || !ctx->aggregate_state_stream ||
       !debug_get_option_stage_ring() || ph_pending_ring_acquire(ctx, false) ||
       ctx->pending_ring_slot < 0)
      return;
   left = ctx->vram_vertex_slot_dwords - ctx->pending_vertex_dwords;
   if (left < PH_STAGE_MIN_DWORDS) {
      /* the drain stages the next batch at the start of a fresh slot */
      ph_drain_batch(ctx);
      return;
   }
   ctx->vertices = (float *)(ctx->pending_vertices + ctx->pending_vertex_dwords);
   /* a staged batch must fit the store if it has to be moved out */
   ctx->stage_room = MIN2(left, ARRAY_SIZE(ctx->vertex_store));
}

/* Another batch is appended while one is staged at the aggregate's tail:
 * move the staged one out of its way. */
static void
ph_stage_evict(struct ph_context *ctx, const void *vertices)
{
   if (!ctx->stage_room || !(ctx->triangles || ctx->points || ctx->lines) ||
       ((const float *)vertices >= ctx->vertices &&
        (const float *)vertices < ctx->vertices + ctx->stage_room))
      return;
   memcpy(ctx->vertex_store, ctx->vertices,
          ctx->stage_room * sizeof(ctx->vertex_store[0]));
   ctx->vertices = ctx->vertex_store;
   ctx->stage_room = 0;
   ctx->stat.stage_evictions++;
}

static int
ph_submit_batch_now_untimed(struct ph_context *ctx,
                            struct parhelia_batch *batch,
                            const void *vertices, unsigned vertex_dwords)
{
   struct parhelia_screen *screen = ph_screen(ctx->base.screen);
   bool boundary_aware = ctx->pack_adjacent_packets &&
                         !ph_batch_has_full_boundary(batch);
   int ret;
   uint64_t bytes = (uint64_t)vertex_dwords * sizeof(uint32_t);

   ctx->stat.submits++;

   if (vertices && vertex_dwords && ctx->async_submit &&
       ctx->vram_vertex_enabled &&
       bytes <= (uint64_t)ctx->vram_vertex_slot_dwords * sizeof(uint32_t)) {
      /* An aggregate staged in its ring slot is submitted in place. */
      const bool in_ring = ctx->pending_ring_slot >= 0 &&
         (const uint32_t *)vertices >= ctx->pending_vertices &&
         (const uint32_t *)vertices + vertex_dwords <=
            ctx->pending_vertices + ctx->vram_vertex_slot_dwords;
      unsigned slot = ctx->vram_vertex_slot;
      uint64_t offset = in_ring ?
         (uint64_t)((const uint8_t *)vertices -
                    (const uint8_t *)ctx->vram_vertices) :
         (uint64_t)slot * ctx->vram_vertex_slot_dwords * sizeof(uint32_t);

      if (!in_ring) {
         if (ctx->vram_vertex_fence[slot]) {
            ret = ph_wait_fence_counted(ctx, ctx->vram_vertex_fence[slot]);
            if (ret)
               return ret;
            ctx->vram_vertex_fence[slot] = 0;
         }
         memcpy((uint8_t *)ctx->vram_vertices + offset, vertices, bytes);
      }
      __sync_synchronize();
      ret = screen->draw_ranges ?
         parhelia_batch_submit_ranges_async(
            screen->fd, batch, ctx->vram_vertex_handle, offset,
            vertex_dwords, boundary_aware, screen->draw_runs,
            &ctx->fence) : -ENOTSUP;
      if (ret == -ENOTSUP)
         ret = boundary_aware ?
            parhelia_batch_submit_vram_async_bounded(
               screen->fd, batch, ctx->vram_vertex_handle, offset,
               vertex_dwords, &ctx->fence) :
            parhelia_batch_submit_vram_async(
               screen->fd, batch, ctx->vram_vertex_handle, offset,
               vertex_dwords, &ctx->fence);
      if (!ret) {
         if (in_ring) {
            ctx->pending_ring_fence = ctx->fence;
         } else {
            ctx->vram_vertex_fence[slot] = ctx->fence;
            ctx->vram_vertex_slot = (slot + 1) % ctx->vram_vertex_slot_count;
         }
         ctx->async_pending = true;
         return 0;
      }
      if (ret != -EOPNOTSUPP && ret != -ENOMEM)
         return ret;
      if (ret == -EOPNOTSUPP) {
         ctx->vram_vertex_enabled = false;
      } else {
         /* The request has not reached the doorbell.  Retry through the
          * synchronous CREATE_VERTEX path below; a large local-VRAM batch
          * cannot fall back to the kernel's 4096-dword class-3 copy slot. */
         if (ph_env("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: async vertex slot exhausted; retrying synchronously\n");
         ctx->async_submit = false;
         ctx->pack_adjacent_packets = false;
      }
   }
   if (vertices && vertex_dwords && !ctx->async_submit &&
       ctx->vram_vertex_enabled &&
       bytes <= (uint64_t)ctx->vram_vertex_slot_dwords * sizeof(uint32_t)) {
      /* Async submission can be disabled after a pre-doorbell allocation
       * failure.  Slot zero may still belong to an earlier queued draw at
       * that point, so retire its fence before reusing the synchronous
       * subrange.  The synchronous ioctl itself drains the remaining kernel
       * queue before issuing this draw. */
      if (ctx->vram_vertex_fence[0]) {
         ret = parhelia_wait_fence(screen->fd, ctx->vram_vertex_fence[0],
                                   -1, NULL);
         if (ret)
            return ret;
         ctx->vram_vertex_fence[0] = 0;
      }
      memmove(ctx->vram_vertices, vertices, bytes);
      /* The map can be write-combined. Finish the CPU store before the ioctl
       * arms class-3 fetching; the kernel's fence then makes reuse safe. */
      __sync_synchronize();
      ret = parhelia_batch_submit_vram(ph_screen(ctx->base.screen)->fd, batch,
                                       ctx->vram_vertex_handle, 0,
                                       vertex_dwords, &ctx->fence);
      if (ret != -EOPNOTSUPP) {
         if (!ret)
            ctx->async_pending = false;
         return ret;
      }
      /* A new userspace against an older/reloaded module may lose the
	   * optional capability between context creation and submit. Fall back
	   * only for that pre-submit rejection, never after an engine error. */
      ctx->vram_vertex_enabled = false;
   }
   /* Copy-submit fallback requests retain their own kernel vertex snapshot.
    * CREATE_VERTEX submissions above use three fenced class-3 DMA subranges. */
   if (ctx->async_submit) {
      ret = boundary_aware ?
         parhelia_batch_submit_async_bounded(
            screen->fd, batch, vertices, vertex_dwords, &ctx->fence) :
         parhelia_batch_submit_async(
            screen->fd, batch, vertices, vertex_dwords, &ctx->fence);
      if (!ret) {
         ctx->async_pending = true;
         return 0;
      }
      /* An async slot can reject a large, pre-doorbell request when its
       * bounded DMA32 backing cannot be obtained.  Nothing reached the GPU
       * in that case, so retain rendering correctness by switching this
       * screen to the proven fenced synchronous path. */
      if (ret != -EOPNOTSUPP && ret != -ENOMEM)
         return ret;
      if (ret == -ENOMEM && ph_env("PARHELIA_TRACE"))
         fprintf(stderr, "parhelia: async DMA32 slot exhausted; falling back to synchronous submits\n");
      ctx->async_submit = false;
      ctx->pack_adjacent_packets = false;
   }
   return parhelia_batch_submit(screen->fd, batch, vertices, vertex_dwords,
                                &ctx->fence);
}

static void
ph_release_pending_resources(struct ph_context *ctx)
{
   for (unsigned i = 0; i < ctx->pending_resource_count; i++)
      pipe_resource_reference(&ctx->pending_resources[i], NULL);
   ctx->pending_resource_count = 0;
   ctx->pending_write_count = 0;
}

static bool
ph_pending_writes_resource(const struct ph_context *ctx,
                           const struct parhelia_resource *resource)
{
   for (unsigned i = 0; resource && i < ctx->pending_write_count; i++)
      if (ctx->pending_writes[i] == &resource->base)
         return true;
   return false;
}

static void
ph_record_pending_write(struct ph_context *ctx,
                        struct parhelia_resource *resource)
{
   if (!resource || ph_pending_writes_resource(ctx, resource))
      return;
   /* Every written resource is also retained in pending_resources. */
   assert(ctx->pending_write_count < PH_BATCH_RELOCS);
   ctx->pending_writes[ctx->pending_write_count++] = &resource->base;
}

/* parhelia_vertex_bind_mode() in the kernel turns this exact vertex-program
 * bind into a second primary CALL immediately after its parent packet.  The
 * submit ABI admits 221 userspace packets because three of the 224 hardware
 * CALL slots are reserved for opening/EOB/drain, so account for these hidden
 * calls before combining complete state sequences. */
static unsigned
ph_batch_stage_calls(const struct parhelia_batch *batch)
{
   unsigned stages = 0;

   for (unsigned i = 0; i < batch->packet_count; i++) {
      const struct drm_parhelia_packet *packet = &batch->packets[i];
      const uint32_t *cmd;

      if (packet->offset > batch->command_count ||
          packet->length > batch->command_count - packet->offset)
         continue;
      cmd = batch->commands + packet->offset;
      if ((packet->length == 8 || packet->length == 10) &&
          cmd[0] == packet->length - 2 && cmd[1] == 0x212121ddu &&
          cmd[4] == 0x20000002u && cmd[5] == cmd[4] &&
          ((packet->length == 8 && cmd[3] == 0x3e403c3cu && cmd[7] == 2) ||
           (packet->length == 10 && cmd[3] == 0x41403c3cu &&
            cmd[6] == cmd[7] && cmd[8] == 0x2121213eu && cmd[9] == 2)))
         stages++;
   }
   return stages;
}

static bool
ph_batch_has_draw(const struct parhelia_batch *batch)
{
   for (unsigned i = 0; i < batch->reloc_count; i++)
      if (batch->relocs[i].type == PARHELIA_RELOC_DRAW)
         return true;
   return false;
}

static int
ph_submit_state_stream_range(struct ph_context *ctx,
                             struct parhelia_batch *slice,
                             unsigned first_mark, unsigned end_mark)
{
   const struct parhelia_batch_mark begin = first_mark ?
      ctx->pending_marks[first_mark - 1] : (struct parhelia_batch_mark) {0};
   const struct parhelia_batch_mark *end = &ctx->pending_marks[end_mark - 1];
   unsigned vertex_dwords = end->vertex_dwords - begin.vertex_dwords;
   int ret;

   ret = parhelia_batch_slice(slice, &ctx->pending_batch, &begin, end);
   if (ret)
      return ret;
   ret = ph_submit_batch_now(
      ctx, slice,
      vertex_dwords ? ctx->pending_vertices + begin.vertex_dwords : NULL,
      vertex_dwords);
   if (ret != -E2BIG || end_mark - first_mark == 1)
      return ret;

   /* E2BIG is guaranteed to precede the producer update.  If a kernel lacks
    * the draw packer assumed by the estimator, bisect this group while still
    * preserving every original state-stream mark and its ordering. */
   unsigned middle = first_mark + (end_mark - first_mark) / 2;
   ret = ph_submit_state_stream_range(ctx, slice, first_mark, middle);
   if (!ret)
      ret = ph_submit_state_stream_range(ctx, slice, middle, end_mark);
   return ret;
}

static int
ph_submit_state_stream_groups(struct ph_context *ctx)
{
   struct parhelia_batch *slice = CALLOC_STRUCT(parhelia_batch);
   unsigned first = 0;
   int ret = -ENOMEM;

   if (!slice)
      return ret;
   while (first < ctx->pending_mark_count) {
      const bool pack_adjacent =
         ctx->pack_adjacent_packets;
      unsigned first_packet = first ? ctx->pending_marks[first - 1].packets : 0;
      unsigned end = first, calls = 0;

      while (end < ctx->pending_mark_count) {
         unsigned candidate_calls;

         ret = parhelia_batch_packed_call_count(
            &ctx->pending_batch, first_packet,
            ctx->pending_marks[end].packets, pack_adjacent,
            &candidate_calls);
         if (ret)
            goto out;
         if (end > first && candidate_calls > PH_BATCH_HW_CALLS)
            break;
         calls = candidate_calls;
         end++;
         if (calls >= PH_BATCH_HW_CALLS)
            break;
      }
      if (ph_env("PARHELIA_TRACE_BATCH"))
         fprintf(stderr,
                 "parhelia: aggregate group marks=%u..%u estimated_calls=%u\n",
                 first, end, calls);
      ret = ph_submit_state_stream_range(ctx, slice, first, end);
      if (ret)
         goto out;
      first = end;
   }
out:
   FREE(slice);
   return ret;
}

static int
ph_drain_batch_at(struct ph_context *ctx, unsigned line)
{
   unsigned packed_calls = 0;
   bool packed_calls_valid;
   int ret;

   if (!ctx->pending_batch.packet_count)
      return 0;
   if (ph_env("PARHELIA_TRACE_DRAIN")) {
      if (line >= ph_drain_sites.cap) {
         unsigned cap = line * 2 + 1;
         unsigned *grown = realloc(ph_drain_sites.count, cap * sizeof(*grown));

         if (grown) {
            memset(grown + ph_drain_sites.cap, 0,
                   (cap - ph_drain_sites.cap) * sizeof(*grown));
            ph_drain_sites.count = grown;
            ph_drain_sites.cap = cap;
         }
      }
      if (line < ph_drain_sites.cap)
         ph_drain_sites.count[line]++;
   }
   /* the exact count only where the bound cannot rule out a split */
   packed_calls_valid =
      (ph_env("PARHELIA_TRACE_BATCH") ||
       (ctx->aggregate_state_stream && ctx->pending_mark_count > 1 &&
        ctx->pending_call_bound > PH_BATCH_HW_CALLS)) &&
      !parhelia_batch_packed_call_count(
         &ctx->pending_batch, 0, ctx->pending_batch.packet_count,
         ctx->pack_adjacent_packets, &packed_calls);
   if (ph_env("PARHELIA_TRACE_BATCH"))
      fprintf(stderr,
              "parhelia: aggregate submits=%u commands=%u packets=%u stages=%u relocs=%u vertices=%u resources=%u estimated_calls=%u call_bound=%u\n",
              ctx->pending_submit_count,
              ctx->pending_batch.command_count,
              ctx->pending_batch.packet_count,
              ctx->pending_stage_calls,
              ctx->pending_batch.reloc_count,
              ctx->pending_vertex_dwords,
              ctx->pending_resource_count,
              packed_calls_valid ? packed_calls : 0,
              ctx->pending_call_bound);
   if (ctx->aggregate_state_stream && ctx->pending_mark_count > 1 &&
       packed_calls_valid &&
       packed_calls > PH_BATCH_HW_CALLS)
      ret = ph_submit_state_stream_groups(ctx);
   else
      ret = ph_submit_batch_now(ctx, &ctx->pending_batch,
                                ctx->pending_vertex_dwords ?
                                   ctx->pending_vertices : NULL,
                                ctx->pending_vertex_dwords);
   /* The kernel returns E2BIG before publishing a primary producer update.
    * A state-stream aggregate retains every complete original batch, so retry
    * ordered groups within the hardware CALL bound.  The recursive range
    * helper can still reach individual marks when userspace and kernel packet
    * packing capabilities differ. */
   if (ret == -E2BIG && ctx->aggregate_state_stream &&
       ctx->pending_mark_count > 1)
      ret = ph_submit_state_stream_groups(ctx);
   ph_release_pending_resources(ctx);
   ph_pending_ring_release(ctx);
   ctx->pending_batch.command_count = 0;
   ctx->pending_batch.packet_count = 0;
   ctx->pending_batch.reloc_count = 0;
   ctx->pending_vertex_dwords = 0;
   ctx->pending_index_dwords = 0;
   ctx->pending_submit_count = 0;
   ctx->pending_call_bound = 2;
   ctx->pending_stage_calls = 0;
   ctx->pending_mark_count = 0;
   if (ret)
      ctx->submit_error = ret;
   else
      ph_stage_at_ring_tail(ctx);
   return ret;
}

static unsigned
ph_pending_new_resources(const struct ph_context *ctx,
                         struct parhelia_resource *const resources[4])
{
   unsigned count = 0;

   for (unsigned i = 0; i < 4; i++) {
      bool found = !resources[i];

      for (unsigned j = 0; !found && j < i; j++)
         found = resources[i] == resources[j];
      for (unsigned j = 0; !found && j < ctx->pending_resource_count; j++)
         found = &resources[i]->base == ctx->pending_resources[j];
      count += !found;
   }
   return count;
}

static void
ph_reference_pending_resources(struct ph_context *ctx,
                               struct parhelia_resource *const resources[4])
{
   for (unsigned i = 0; i < 4; i++) {
      bool found = !resources[i];

      for (unsigned j = 0; !found && j < ctx->pending_resource_count; j++)
         found = &resources[i]->base == ctx->pending_resources[j];
      if (!found)
         pipe_resource_reference(
            &ctx->pending_resources[ctx->pending_resource_count++],
            &resources[i]->base);
   }
}

static int
ph_append_state_stream(struct ph_context *ctx,
                       const struct parhelia_batch *batch,
                       const void *vertices, unsigned vertex_dwords)
{
   if (ctx->pack_adjacent_packets)
      return parhelia_batch_append_call_bounded_limited(
         &ctx->pending_batch, ctx->pending_vertices,
         &ctx->pending_vertex_dwords, batch, vertices, vertex_dwords,
         &ctx->batch_limits);
   return parhelia_batch_append_limited(
      &ctx->pending_batch, ctx->pending_vertices,
      &ctx->pending_vertex_dwords, batch, vertices, vertex_dwords,
      &ctx->batch_limits);
}

static int
ph_submit_batch(struct ph_context *ctx, struct parhelia_batch *batch,
                const void *vertices, unsigned vertex_dwords,
                struct parhelia_resource *r0, struct parhelia_resource *r1,
                struct parhelia_resource *r2, struct parhelia_resource *r3)
{
   struct parhelia_resource *resources[4] = {r0, r1, r2, r3};
   bool draw_batch;
   int ret;
   int batch_calls = ctx->batch_calls;
   const bool cached = batch_calls >= 0;
   unsigned calls, stage_calls;

   ctx->batch_calls = -1;
   /* uncached batches (clears, copies) are short: count them here */
   if (batch_calls < 0 && ctx->pack_adjacent_packets && batch->packet_count &&
       !parhelia_batch_packed_call_count(batch, 0, batch->packet_count, true,
                                         &calls))
      batch_calls = (int)calls;
   ctx->stat.batches++;
   ctx->stat.draw_batches += vertex_dwords != 0;
   ctx->stat.vertex_dwords += vertex_dwords;

   /* Same-state aggregation extends only the draw tail.  State-stream mode
    * keeps every complete state and DRAW sequence while sharing one terminal
    * EOB/DWGSYNC.  The older stateful experiment instead retains the one
    * internal fence boundary supported by the current kernel ABI. */
   if (!ctx->aggregate_batches)
      return ph_submit_batch_now(ctx, batch, vertices, vertex_dwords);
   if (vertex_dwords)
      ph_stage_evict(ctx, vertices);
   if (vertex_dwords && (const float *)vertices == ctx->vertices &&
       ctx->stage_room)
      ctx->stat.in_place_dwords += vertex_dwords;

   if (ctx->aggregate_state_stream &&
       ctx->pending_mark_count >= ctx->batch_limits.packets) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }

   stage_calls = cached ? ctx->batch_stages : ph_batch_stage_calls(batch);
   draw_batch = ph_batch_has_draw(batch);
   if (ctx->aggregate_state_stream && ctx->pending_batch.packet_count &&
       ph_batch_has_draw(&ctx->pending_batch) && !draw_batch) {
      /* A clear/blit after rendering must observe the preceding pixel-pipe
       * drain.  In the other direction p3d_emit_state() explicitly rebinds
       * the surfaces and 3D engine, so leading colour/depth clears can share
       * the draw stream exactly as p3d_emit_clear_frame() does. */
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }
   /* The original family's column-strip fills are still landing when 3D
    * work in the same stream starts (0528: depth and colour strips over
    * finished pixels); the vendor drains after every fill. */
   if (ctx->aggregate_state_stream && draw_batch &&
       ph_original_family(ph_screen(ctx->base.screen)->chip_id) &&
       ctx->pending_batch.packet_count &&
       !ph_batch_has_draw(&ctx->pending_batch)) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }
   if (ctx->aggregate_state_stream &&
       batch->packet_count + stage_calls > ctx->batch_limits.packets)
      return -E2BIG;
   if (ctx->aggregate_state_stream && ctx->pending_batch.packet_count &&
       ctx->pending_batch.packet_count + ctx->pending_stage_calls +
          batch->packet_count + stage_calls > ctx->batch_limits.packets) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }
   if (vertex_dwords > ph_pending_vertex_limit(ctx))
      return -E2BIG;
   if (ctx->pending_vertex_dwords &&
       vertex_dwords > ph_pending_vertex_limit(ctx) -
                          ctx->pending_vertex_dwords) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }
   if (ctx->pending_resource_count +
       ph_pending_new_resources(ctx, resources) > ctx->batch_limits.relocs) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }
   /* the kernel copies a submit's indices into 4096 dwords of its own */
   if (ctx->batch_index_dwords && ctx->pending_index_dwords &&
       ctx->pending_index_dwords + ctx->batch_index_dwords > 4096) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }
   if (vertex_dwords && (ret = ph_pending_ring_acquire(ctx, true)))
      return ret;
   ret = ctx->aggregate_state_stream ?
      ph_append_state_stream(ctx, batch, vertices, vertex_dwords) :
      ctx->aggregate_state_batches ?
      parhelia_batch_append_stateful_limited(
         &ctx->pending_batch, ctx->pending_vertices,
         &ctx->pending_vertex_dwords, batch, vertices, vertex_dwords,
         &ctx->batch_limits) :
      parhelia_batch_append_same_state_limited(
         &ctx->pending_batch, ctx->pending_vertices,
         &ctx->pending_vertex_dwords, batch, vertices, vertex_dwords,
         &ctx->batch_limits);
   if ((ret == -E2BIG || ret == -EAGAIN) &&
       ctx->pending_batch.packet_count) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
      if (vertex_dwords && (ret = ph_pending_ring_acquire(ctx, true)))
         return ret;
      ret = ctx->aggregate_state_stream ?
         ph_append_state_stream(ctx, batch, vertices, vertex_dwords) :
         ctx->aggregate_state_batches ?
         parhelia_batch_append_stateful_limited(
            &ctx->pending_batch, ctx->pending_vertices,
            &ctx->pending_vertex_dwords, batch, vertices, vertex_dwords,
            &ctx->batch_limits) :
         parhelia_batch_append_same_state_limited(
            &ctx->pending_batch, ctx->pending_vertices,
            &ctx->pending_vertex_dwords, batch, vertices, vertex_dwords,
            &ctx->batch_limits);
   }
   if (ret)
      return ret;
   /* packing across batches only joins runs, so the batches' counts bound
    * the stream's; each counts the stream's final EOB and drain itself */
   ctx->pending_call_bound =
      batch_calls < 2 || !ctx->pack_adjacent_packets ||
      ctx->pending_call_bound == UINT_MAX ? UINT_MAX :
      ctx->pending_call_bound + (unsigned)batch_calls - 2;
   ctx->pending_submit_count++;
   ctx->pending_stage_calls += stage_calls;
   if (ctx->aggregate_state_stream) {
      ctx->pending_marks[ctx->pending_mark_count++] =
         (struct parhelia_batch_mark) {
            .command_dwords = ctx->pending_batch.command_count,
            .packets = ctx->pending_batch.packet_count,
            .relocs = ctx->pending_batch.reloc_count,
            .vertex_dwords = ctx->pending_vertex_dwords,
         };
   }
   ph_record_pending_write(ctx, r0);
   ph_record_pending_write(ctx, r1);
   ph_reference_pending_resources(ctx, resources);
   ctx->pending_index_dwords += ctx->batch_index_dwords;
   ctx->batch_index_dwords = 0;
   /* re-point staging past what was just appended */
   ph_stage_at_ring_tail(ctx);
   return 0;
}

static enum p3d_cmp
ph_compare(unsigned func)
{
   static const enum p3d_cmp map[] = {
      P3D_CMP_NEVER, P3D_CMP_LESS, P3D_CMP_EQUAL, P3D_CMP_LEQUAL,
      P3D_CMP_GREATER, P3D_CMP_NOTEQUAL, P3D_CMP_GEQUAL, P3D_CMP_ALWAYS,
   };
   return func < ARRAY_SIZE(map) ? map[func] : P3D_CMP_ALWAYS;
}

static struct p3d_blend_factor
ph_blend_factor(unsigned factor)
{
   switch (factor) {
   case PIPE_BLENDFACTOR_ZERO: return P3D_BF_ZERO;
   case PIPE_BLENDFACTOR_ONE: return P3D_BF_ONE;
   case PIPE_BLENDFACTOR_SRC_COLOR: return P3D_BF_SRC_COLOR;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR: return P3D_BF_ONE_MINUS_SRC_COLOR;
   case PIPE_BLENDFACTOR_SRC_ALPHA: return P3D_BF_SRC_ALPHA;
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA: return P3D_BF_ONE_MINUS_SRC_ALPHA;
   case PIPE_BLENDFACTOR_DST_ALPHA: return P3D_BF_DST_ALPHA;
   case PIPE_BLENDFACTOR_INV_DST_ALPHA: return P3D_BF_ONE_MINUS_DST_ALPHA;
   case PIPE_BLENDFACTOR_DST_COLOR: return P3D_BF_DST_COLOR;
   case PIPE_BLENDFACTOR_INV_DST_COLOR: return P3D_BF_ONE_MINUS_DST_COLOR;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE: return P3D_BF_SRC_ALPHA_SATURATE;
   case PIPE_BLENDFACTOR_CONST_COLOR: return P3D_BF_CONST_COLOR;
   case PIPE_BLENDFACTOR_INV_CONST_COLOR: return P3D_BF_ONE_MINUS_CONST_COLOR;
   case PIPE_BLENDFACTOR_CONST_ALPHA: return P3D_BF_CONST_ALPHA;
   case PIPE_BLENDFACTOR_INV_CONST_ALPHA: return P3D_BF_ONE_MINUS_CONST_ALPHA;
   default: return P3D_BF_ONE;
   }
}

static enum p3d_blend_eq
ph_blend_equation(unsigned equation)
{
   switch (equation) {
   case PIPE_BLEND_SUBTRACT: return P3D_BEQ_SUBTRACT;
   case PIPE_BLEND_REVERSE_SUBTRACT: return P3D_BEQ_REVERSE_SUBTRACT;
   case PIPE_BLEND_MIN: return P3D_BEQ_MIN;
   case PIPE_BLEND_MAX: return P3D_BEQ_MAX;
   default: return P3D_BEQ_ADD;
   }
}

static enum p3d_stencil_op
ph_stencil_op(unsigned op)
{
   switch (op) {
   case PIPE_STENCIL_OP_ZERO: return P3D_STENCIL_ZERO;
   case PIPE_STENCIL_OP_REPLACE: return P3D_STENCIL_REPLACE;
   case PIPE_STENCIL_OP_INCR: return P3D_STENCIL_INCR_SAT;
   case PIPE_STENCIL_OP_DECR: return P3D_STENCIL_DECR_SAT;
   case PIPE_STENCIL_OP_INCR_WRAP: return P3D_STENCIL_INCR_WRAP;
   case PIPE_STENCIL_OP_DECR_WRAP: return P3D_STENCIL_DECR_WRAP;
   case PIPE_STENCIL_OP_INVERT: return P3D_STENCIL_INVERT;
   default: return P3D_STENCIL_KEEP;
   }
}

/* Two 10-bit blend constants, the first in [31:16].  Measured on the P690:
 * 0x12ec is A<<16 | R and 0x12f0 is G<<16 | B, not the documented RGBA. */
static uint32_t
ph_pack_2x10(float first, float second)
{
   unsigned a = (unsigned)(CLAMP(first, 0.0f, 1.0f) * 1023.0f);
   unsigned b = (unsigned)(CLAMP(second, 0.0f, 1.0f) * 1023.0f);
   return (a << 16) | b;
}

static void
ph_apply_fragment_state(struct ph_context *ctx, struct p3d_state *state,
                        bool has_depth)
{
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;

   if (ctx->dsa) {
      state->alpha_test = ctx->dsa->alpha_enabled;
      state->alpha_func = ph_compare(ctx->dsa->alpha_func);
      state->alpha_ref = CLAMP((int)lrintf(ctx->dsa->alpha_ref_value * 255.0f),
                               0, 255);
      if (has_depth) {
         /* No two-sided stencil in the engine: a batch holds one face. */
         const unsigned face = ctx->stencil_back &&
                               ctx->dsa->stencil[1].enabled;
         const struct pipe_stencil_state *s = &ctx->dsa->stencil[face];
         state->depth_test = ctx->dsa->depth_enabled;
         state->depth_write = ctx->dsa->depth_writemask;
         state->depth_func = ph_compare(ctx->dsa->depth_func);
         state->stencil_test = s->enabled;
         state->stencil_func = ph_compare(s->func);
         state->stencil_fail = ph_stencil_op(s->fail_op);
         state->stencil_zfail = ph_stencil_op(s->zfail_op);
         state->stencil_zpass = ph_stencil_op(s->zpass_op);
         state->stencil_ref = ctx->stencil_ref.ref_value[face];
         state->stencil_value_mask = s->valuemask;
         state->stencil_write_mask = s->writemask;
      }
   }
   if (blend) {
      struct p3d_blend_factor rgb_src = ph_blend_factor(blend->rgb_src_factor);
      struct p3d_blend_factor rgb_dst = ph_blend_factor(blend->rgb_dst_factor);
      struct p3d_blend_factor a_src = ph_blend_factor(blend->alpha_src_factor);
      struct p3d_blend_factor a_dst = ph_blend_factor(blend->alpha_dst_factor);

      /* An enabled logic op replaces blending, in GL and in the engine: the
       * raster op lives in DWGCTL [7:4] and the back end honours blending
       * only while that nibble is GXcopy. */
      state->logicop_enable = ctx->blend->logicop_enable;
      state->logicop = ctx->blend->logicop_func;
      state->blend = blend->blend_enable && !ctx->blend->logicop_enable;
      state->blend_src = (struct p3d_blend_factor){rgb_src.rgb, a_src.a};
      state->blend_dst = (struct p3d_blend_factor){rgb_dst.rgb, a_dst.a};
      state->blend_eq = ph_blend_equation(blend->rgb_func);
      state->blend_eq_alpha = ph_blend_equation(blend->alpha_func);
      state->blend_eq_alpha_separate = state->blend_eq_alpha != state->blend_eq;
      state->blend_const_rg = ph_pack_2x10(ctx->blend_color.color[3],
                                           ctx->blend_color.color[0]);
      state->blend_const_ba = ph_pack_2x10(ctx->blend_color.color[1],
                                           ctx->blend_color.color[2]);
      const unsigned colormask = blend->colormask &
         (ctx->fb.nr_cbufs ? ph_target_channels(ctx->fb.cbufs[0].format) :
                             PIPE_MASK_RGBA);

      state->mask_r = !!(colormask & PIPE_MASK_R);
      state->mask_g = !!(colormask & PIPE_MASK_G);
      state->mask_b = !!(colormask & PIPE_MASK_B);
      state->mask_a = !!(colormask & PIPE_MASK_A);
      state->mask_explicit = true;
   }
   {
      struct ph_fs_program *kill_prog;
      enum pipe_compare_func kill_func;
      uint8_t kill_ref;
      const enum ph_kill_route route =
         ph_fs_kill_route(ctx, &kill_prog, &kill_func, &kill_ref);

      /* The discard became an alpha test; GL's own was folded in. */
      if (route == PH_KILL_ALPHA_X || route == PH_KILL_ALPHA_SUB) {
         state->alpha_test = true;
         state->alpha_func = ph_compare(kill_func);
         state->alpha_ref = kill_ref;
      }
   }
   if (ctx->rasterizer && ctx->rasterizer->scissor) {
      state->scissor = true;
      state->scissor_minx = MIN2(ctx->scissor.minx, state->screen_w);
      state->scissor_miny = MIN2(ctx->scissor.miny, state->screen_h);
      state->scissor_maxx = MIN2(ctx->scissor.maxx, state->screen_w);
      state->scissor_maxy = MIN2(ctx->scissor.maxy, state->screen_h);
   }
}

/* The RGBA descriptor is proven only for this fixed-function shape.  Both
 * non-Z and Z-bound forms use a single MODULATE sampler and source-alpha ADD;
 * the depth form also requires an enabled depth compare.  Keep the predicate
 * deliberately narrower than GL's general blend model: every other blend,
 * texture, alpha-test, multi-texture, and shader form retains the existing
 * RGB vertex stream. */
/* Colour targets the measured alpha routes may write.  Those routes drive the
 * blend unit, the alpha comparator and the depth compare, and none of the
 * three sees the target's format: parhelia-drm-test --narrow-alpha-probe
 * reproduces all eight live forms -- vertex-alpha source-over with and
 * without depth, vertex-alpha additive, the sampled-alpha companion in its
 * source-over and its accepted and rejected cutout shapes, and the two passes
 * of the RGB alpha two-pass route -- on a 5:6:5 target against a Z16 buffer,
 * within one colour code of the same draw into a 32-bit target with Z24. */
static bool
ph_alpha_route_colour(enum pipe_format format)
{
   return format == PIPE_FORMAT_B8G8R8A8_UNORM ||
          format == PIPE_FORMAT_B8G8R8X8_UNORM ||
          format == PIPE_FORMAT_B5G6R5_UNORM;
}

static bool
ph_alpha_route_depth(enum pipe_format format)
{
   return format == PIPE_FORMAT_Z24_UNORM_S8_UINT ||
          format == PIPE_FORMAT_Z24X8_UNORM ||
          format == PIPE_FORMAT_Z16_UNORM;
}

/* A target with no destination alpha.  The RGB alpha two-pass route's pair of
 * hardware passes is then the complete visible source-over result for any
 * RGBA texture, with no image fingerprint needed; 5:6:5 has no alpha channel
 * at all, and p3d_colour_mask() already drops the route's mask_a for it. */
static bool
ph_target_no_dest_alpha(enum pipe_format format)
{
   return format == PIPE_FORMAT_B8G8R8X8_UNORM ||
          format == PIPE_FORMAT_B5G6R5_UNORM;
}

static bool
ph_blend_factor_src_alpha(unsigned factor)
{
   switch (factor) {
   case PIPE_BLENDFACTOR_SRC_ALPHA:
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
   case PIPE_BLENDFACTOR_SRC1_ALPHA:
   case PIPE_BLENDFACTOR_INV_SRC1_ALPHA:
      return true;
   default:
      return false;
   }
}

/* The vertex colour's alpha reaches a visible result: an alpha test, a blend,
 * or a target that stores alpha.  Otherwise the RGB stream and the default
 * attribute's alpha of 1.0 (0x112c) are equivalent and one dword shorter.
 * The original family asks the blend whether its colour factors read it. */
static bool
ph_colour_alpha_observable(const struct ph_context *ctx)
{
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;

   if (ctx->dsa && ctx->dsa->alpha_enabled)
      return true;
   if (blend && blend->blend_enable &&
       (!ph_original_family(ph_screen(ctx->base.screen)->chip_id) ||
        ph_blend_factor_src_alpha(blend->rgb_src_factor) ||
        ph_blend_factor_src_alpha(blend->rgb_dst_factor)))
      return true;
   return ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture &&
          util_format_has_alpha(ctx->fb.cbufs[0].format) &&
          (!blend || (blend->colormask & PIPE_MASK_A));
}

/* The fragment program reads v1, which only the triangle stream carries. */
static bool
ph_fs_reads_v1(const struct ph_context *ctx)
{
   return ctx->fs && ctx->fs->program.valid && ctx->fs->program.uses_color1;
}

/* The alpha approximation routes, all off by default.
 *
 * Each of them replaces a texel's own alpha with something cheaper, and they
 * exist because the texture-alpha path was believed broken on the P690.  It
 * is not: with every route disabled, GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA
 * blending of a texture whose alpha steps per texel row reproduces the
 * reference values to within one LSB, and GL_ALPHA_TEST GEQUAL 0.5 on the
 * same texture discards exactly the rows below the reference
 * (Q3FMT_GROUPS=bias, "blend ... over black" and "GL_ALPHA_TEST on a texel's
 * own alpha").  With the micro route enabled the same blend returns roughly
 * alpha squared, which is what put a dark line through every ioquake3 glyph.
 *
 * They are kept, selectable, because each was measured against a live game
 * artefact: PARHELIA_ALPHA_ROUTES is a mask of vertex(1), micro(2),
 * micro-cutout(4), cpu-cutout(8) and rgb-two-pass(16). */
static bool ph_alpha_route_enabled(unsigned bit)
{
	/* Read once: this is asked several times per draw, and an environment
	 * lookup per draw is measurable in a profile. */
	static unsigned routes;
	static bool read;

	if (!read) {
		const char *mask = getenv("PARHELIA_ALPHA_ROUTES");

		routes = mask && !ph_env("PARHELIA_PLAIN_ALPHA") ?
			 (unsigned)strtoul(mask, NULL, 0) : 0;
		read = true;
	}
	return (routes & bit) != 0;
}

static bool
ph_use_vertex_alpha(struct ph_context *ctx, bool textured,
		    bool multitextured, bool depth, bool has_colour)
{
	const struct pipe_rt_blend_state *blend =
		ctx->blend ? &ctx->blend->rt[0] : NULL;
	const char *reason = "selected";
	unsigned reason_bit = 0;

	/* PARHELIA_NO_VERTEX_ALPHA=1 keeps every blended draw on the ordinary
	 * route, so the approximation can be measured against the plain path. */
	if (!ph_alpha_route_enabled(1))
		return false;
	if (!textured) {
		reason = "no-texture"; reason_bit = 1;
	} else if (multitextured) {
		reason = "multi-texture"; reason_bit = 2;
	} else if (!has_colour) {
		reason = "no-vertex-colour"; reason_bit = 3;
	} else if (!ctx->fs) {
		reason = "no-fragment-shader"; reason_bit = 4;
	} else if (!ctx->fs->uses_color) {
		reason = "fragment-colour-unused"; reason_bit = 5;
	} else if (ctx->fs->sampler_mask != BITFIELD_BIT(0)) {
		reason = "sampler-mask"; reason_bit = 6;
	} else if (ctx->fs->texenv != P3D_TEXENV_MODULATE) {
		reason = "texenv-not-modulate"; reason_bit = 7;
	} else if (ctx->dsa && ctx->dsa->alpha_enabled) {
		reason = "alpha-test"; reason_bit = 8;
	} else if (!blend) {
		reason = "no-blend-state"; reason_bit = 9;
	} else if (depth && (!ctx->dsa || !ctx->dsa->depth_enabled)) {
		reason = "depth-disabled"; reason_bit = 10;
	} else if (!blend->blend_enable) {
		reason = "blend-disabled"; reason_bit = 11;
	} else if (blend->rgb_func != PIPE_BLEND_ADD) {
		reason = "rgb-equation"; reason_bit = 12;
	} else if (blend->alpha_func != PIPE_BLEND_ADD) {
		reason = "alpha-equation"; reason_bit = 13;
	} else if (blend->rgb_src_factor != PIPE_BLENDFACTOR_SRC_ALPHA) {
		reason = "rgb-factors"; reason_bit = 14;
	} else if (blend->alpha_src_factor != PIPE_BLENDFACTOR_SRC_ALPHA) {
		reason = "alpha-factors"; reason_bit = 15;
	} else if (!((blend->rgb_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA &&
	             blend->alpha_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA) ||
	            (blend->rgb_dst_factor == PIPE_BLENDFACTOR_ONE &&
	             blend->alpha_dst_factor == PIPE_BLENDFACTOR_ONE))) {
		reason = "dst-factors"; reason_bit = 16;
	}

	/* One concise line per distinct rejection reason makes a Q3 trace useful
	 * without producing one line per triangle.  Depth-bound texture draws are
	 * the only forms relevant to the new packet. */
	if (depth && textured && ph_env("PARHELIA_TRACE") &&
	    !(ctx->trace_depth_alpha_seen & BITFIELD_BIT(reason_bit))) {
		ctx->trace_depth_alpha_seen |= BITFIELD_BIT(reason_bit);
		fprintf(stderr,
			"parhelia: depth-alpha gate=%s selected=%u colour=%u multi=%u "
			"depth=%u alpha-test=%u fs=color:%u sampler:%#x env:%u "
			"blend=en:%u rgb:%u/%u,%u alpha:%u/%u,%u\n",
			reason, !reason_bit, has_colour, multitextured,
			ctx->dsa ? ctx->dsa->depth_enabled : 0,
			ctx->dsa ? ctx->dsa->alpha_enabled : 0,
			ctx->fs ? ctx->fs->uses_color : 0,
			ctx->fs ? ctx->fs->sampler_mask : 0,
			ctx->fs ? ctx->fs->texenv : 0,
			blend ? blend->blend_enable : 0,
			blend ? blend->rgb_func : 0,
			blend ? blend->rgb_src_factor : 0,
			blend ? blend->rgb_dst_factor : 0,
			blend ? blend->alpha_func : 0,
			blend ? blend->alpha_src_factor : 0,
			blend ? blend->alpha_dst_factor : 0);
	}
	return !reason_bit;
}

/* P690's texture-alpha path is broken, while its depth-tested vertex-alpha
 * source-over path is verified by parhelia-drm-test.  Select only compact,
 * base-level RGBA HUD sprites.  They are depth-read-only so the existing
 * hardware compare remains authoritative and this route can never introduce
 * a CPU depth or colour shadow. */
static bool
ph_use_alpha_micro(struct ph_context *ctx, bool textured, bool multitextured,
                   bool depth, bool has_colour)
{
	if (!ph_alpha_route_enabled(2))
		return false;
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;
   const struct parhelia_resource *texture;
   const struct pipe_sampler_state *sampler;

   /* Q3's font shader intentionally has no COLOR output.  Unlike the normal
    * RGBA stream, this path supplies white RGB itself, so that is the one
    * vertex-alpha gate it may relax. */
   (void)has_colour;
   if (!textured || multitextured || !ctx->fs ||
       ctx->fs->sampler_mask != BITFIELD_BIT(0) ||
       ctx->fs->texenv != P3D_TEXENV_MODULATE ||
       (ctx->dsa && ctx->dsa->alpha_enabled) || !blend ||
       !blend->blend_enable || blend->rgb_func != PIPE_BLEND_ADD ||
       blend->alpha_func != PIPE_BLEND_ADD ||
       blend->rgb_src_factor != PIPE_BLENDFACTOR_SRC_ALPHA ||
       blend->alpha_src_factor != PIPE_BLENDFACTOR_SRC_ALPHA ||
       !ctx->dsa ||
       (ctx->dsa->depth_enabled && ctx->dsa->depth_writemask) ||
       !ctx->num_sampler_views ||
       !ctx->sampler_views[0] || !ctx->sampler_views[0]->texture ||
       !ctx->num_samplers || !(sampler = ctx->samplers[0]) ||
       !(sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
         sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST ||
         sampler->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR))
      return false;

   texture = ph_resource(ctx->sampler_views[0]->texture);
   if (!texture->linear_map ||
       texture->base.width0 > 256 || texture->base.height0 > 256 ||
       (texture->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        texture->base.format != PIPE_FORMAT_R8G8B8A8_UNORM) ||
       !blend || blend->rgb_dst_factor != PIPE_BLENDFACTOR_INV_SRC_ALPHA)
      return false;

   /* This is Q3's central source-alpha reticle.  It declares but does not
    * define a mip chain; accept its base image only after matching the live
    * content fingerprint.  Other declared chains keep the native route. */
   if (texture->base.last_level) {
      if (texture->base.width0 != 32 || texture->base.height0 != 32)
         return false;
      if (ctx->alpha_micro_hash_resource != texture ||
          ctx->alpha_micro_hash_generation != texture->content_generation) {
         ctx->alpha_micro_hash_resource = texture;
         ctx->alpha_micro_hash_generation = texture->content_generation;
         ctx->alpha_micro_hash = ph_texture_base_hash(texture, false, NULL,
                                                       NULL);
      }
      if (ctx->alpha_micro_hash != UINT64_C(0xfb3c80dfe2b85d45))
         return false;
   }

   return true;
}

static bool
ph_use_alpha_micro_cutout(struct ph_context *ctx, bool textured,
                          bool multitextured, bool depth, bool has_colour)
{
	if (!ph_alpha_route_enabled(4))
		return false;
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;
   const struct parhelia_resource *texture;
   const struct pipe_sampler_state *sampler;

   if (!textured || multitextured || !depth || !has_colour || !ctx->fs ||
       ctx->fs->sampler_mask != BITFIELD_BIT(0) ||
       ctx->fs->texenv != P3D_TEXENV_MODULATE || !ctx->dsa ||
       !ctx->dsa->alpha_enabled || ctx->dsa->alpha_func != PIPE_FUNC_GEQUAL ||
       fabsf(ctx->dsa->alpha_ref_value - 0.5f) > 0.0001f ||
       !ctx->dsa->depth_enabled || !blend || blend->blend_enable ||
       blend->colormask != PIPE_MASK_RGBA || !ctx->num_sampler_views ||
       !ctx->sampler_views[0] || !ctx->sampler_views[0]->texture ||
       !ctx->num_samplers || !(sampler = ctx->samplers[0]))
      return false;
   texture = ph_resource(ctx->sampler_views[0]->texture);
   /* Q3 declares a mip chain for this image but leaves every level beyond
    * level zero undefined; the native descriptor therefore samples only the
    * base image. */
   return texture->linear_map && !texture->mip_defined &&
      texture->base.width0 <= 64 && texture->base.height0 <= 64 &&
      (texture->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ||
       texture->base.format == PIPE_FORMAT_R8G8B8A8_UNORM) &&
      (sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
       sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST ||
       sampler->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR);
}

static bool ph_auto_tiled_texture(const struct ph_context *ctx,
                                  const struct parhelia_resource *texture,
                                  const struct pipe_sampler_state *sampler);
static bool ph_resource_uses_sampler_layout(const struct parhelia_resource *r);

/* A native import with no linear companion, presented onto tiled storage,
 * on a chip that cannot sample it directly.  On the original board every
 * render object keeps a transfer shadow; the copy writes VRAM and CPU access
 * downloads the shadow first, so it may take such a destination. */
static bool
ph_present_tiled_source(const struct ph_context *ctx,
                        const struct parhelia_resource *src,
                        const struct parhelia_resource *dst)
{
   return !ph_auto_tiled_texture(ctx, src, NULL) &&
          src->native_shared && src->handle && !src->linear_handle &&
          src->stride && src->stride <= 0x3fff &&
          dst->handle && dst->stride && dst->stride <= 0x3fff &&
          !dst->upload_texture &&
          (!dst->transfer_shadow ||
           !ph_lx_family(ph_screen(ctx->base.screen)->chip_id)) &&
          !dst->mip.block_bytes;
}

struct ph_present_rect {
   int x0, y0, x1, y1, offx, offy;
};

/* Dwords per staged vertex of a plain textured stream, and where S,T sit:
 * x, y[, z, 1/w], r, g, b, s, t. */
static unsigned
ph_stream_stride(const struct ph_context *ctx)
{
   return ctx->oow ? 9 : 7;
}

/* Quad @q of glamor's copy: an integer rectangle whose texels are a 1:1
 * translation of its pixels, clipped to the scissor and both surfaces. */
static const char *
ph_present_quad_rect(const struct ph_context *ctx, unsigned q,
                     const struct parhelia_resource *src,
                     const struct parhelia_resource *dst,
                     struct ph_present_rect *r)
{
   const unsigned stride = ph_stream_stride(ctx), st = stride - 2;
   const float *quad = &ctx->vertices[q * 6 * stride];
   const float sw = src->base.width0, sh = src->base.height0;
   float minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;

   for (unsigned v = 0; v < 6; v++) {
      minx = MIN2(minx, quad[v * stride]);
      miny = MIN2(miny, quad[v * stride + 1]);
      maxx = MAX2(maxx, quad[v * stride]);
      maxy = MAX2(maxy, quad[v * stride + 1]);
   }
   r->x0 = lrintf(minx);
   r->y0 = lrintf(miny);
   r->x1 = lrintf(maxx);
   r->y1 = lrintf(maxy);
   r->offx = lrintf(quad[st] * sw - quad[0]);
   r->offy = lrintf(quad[st + 1] * sh - quad[1]);
   if (fabsf(minx - r->x0) > 0.01f || fabsf(miny - r->y0) > 0.01f ||
       fabsf(maxx - r->x1) > 0.01f || fabsf(maxy - r->y1) > 0.01f)
      return "fractional rectangle";
   for (unsigned v = 0; v < 6; v++) {
      const float *p = &quad[v * stride];

      if ((fabsf(p[0] - minx) > 0.01f && fabsf(p[0] - maxx) > 0.01f) ||
          (fabsf(p[1] - miny) > 0.01f && fabsf(p[1] - maxy) > 0.01f))
         return "not an axis-aligned rectangle";
      if (fabsf(p[st] * sw - p[0] - r->offx) > 0.05f ||
          fabsf(p[st + 1] * sh - p[1] - r->offy) > 0.05f)
         return "texture coordinates are not a 1:1 translation";
   }
   if (ctx->rasterizer && ctx->rasterizer->scissor) {
      r->x0 = MAX2(r->x0, (int)ctx->scissor.minx);
      r->y0 = MAX2(r->y0, (int)ctx->scissor.miny);
      r->x1 = MIN2(r->x1, (int)ctx->scissor.maxx);
      r->y1 = MIN2(r->y1, (int)ctx->scissor.maxy);
   }
   r->x0 = MAX2(r->x0, MAX2(0, -r->offx));
   r->y0 = MAX2(r->y0, MAX2(0, -r->offy));
   r->x1 = MIN2(r->x1, MIN2((int)dst->base.width0, (int)sw - r->offx));
   r->y1 = MIN2(r->y1, MIN2((int)dst->base.height0, (int)sh - r->offy));
   return NULL;
}

/* A native import has only its tiled storage.  The surface-to-surface
 * BITBLT copies it exactly (--tiled-blit-probe) and runs after the client's
 * queued rendering. */
static bool
ph_present_tiled_quads(struct ph_context *ctx, struct parhelia_resource *dst,
                       struct parhelia_resource *src)
{
   const unsigned quads = ctx->triangles / 2;
   struct ph_present_rect r;
   unsigned copied = 0;

   for (unsigned q = 0; q < quads; q++) {
      const char *why = ph_present_quad_rect(ctx, q, src, dst, &r);

      if (why) {
         if (ph_env("PARHELIA_TRACE"))
            fprintf(stderr, "parhelia: tiled present quad %u/%u rejected: %s\n",
                    q, quads, why);
         return false;
      }
   }
   if (ctx->aggregate_batches && ph_drain_batch(ctx))
      return false;
   for (unsigned q = 0; q < quads; q++) {
      struct parhelia_batch *batch = &ctx->build_batch;
      int ret;

      ph_present_quad_rect(ctx, q, src, dst, &r);
      if (r.x0 >= r.x1 || r.y0 >= r.y1)
         continue;
      ret = parhelia_batch_blit(batch, dst->handle, dst->stride, 2, r.x0, r.y0,
                                src->handle, src->stride, 2,
                                r.x0 + r.offx, r.y0 + r.offy,
                                r.x1 - r.x0, r.y1 - r.y0);
      /* Queue behind the client's rendering and sleep on this copy's fence:
       * a synchronous copy waits for an empty kernel queue, which a busy
       * client never leaves, and the server's next access expects it done. */
      if (!ret)
         ret = ph_submit_batch_now_untimed(ctx, batch, NULL, 0);
      if (!ret)
         ret = ph_wait_async(ctx);
      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr,
                 "parhelia: GPU tiled present src=%u dst=%u rect=%d,%d %dx%d from %d,%d ret=%d\n",
                 src->handle, dst->handle, r.x0, r.y0, r.x1 - r.x0,
                 r.y1 - r.y0, r.x0 + r.offx, r.y0 + r.offy, ret);
      if (ret) {
         ctx->submit_error = ret;
         return true;
      }
      copied++;
   }
   if (copied) {
      dst->tiled_dirty = false;
      dst->linear_dirty = !!dst->linear_map;
      dst->mip_defined |= BITFIELD_BIT(0);
      dst->content_generation++;
   }
   return true;
}

static bool
ph_copy_present_quad(struct ph_context *ctx,
                     struct parhelia_resource *dst,
                     struct parhelia_resource *src)
{
   float minx = INFINITY, miny = INFINITY;
   float maxx = -INFINITY, maxy = -INFINITY;
   int x0, y0, width, height;
   const unsigned stride = ph_stream_stride(ctx), st = stride - 2;

   /* This is the exact 1:1 GL_REPLACE quad emitted by glamor Present.  Keep
    * the recognition strict so arbitrary application texture draws continue
    * through the recovered hardware sampler path. */
   if ((src->linear_map ? ctx->triangles != 2 :
                          !ctx->triangles || ctx->triangles % 2) ||
       !ctx->fs || ctx->fs->uses_color ||
       !(dst->base.bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) ||
       dst->cpp != 4 || src->cpp != 4 ||
       (!src->linear_map && !ph_present_tiled_source(ctx, src, dst)) ||
       (!dst->map && !dst->linear_map))
      return false;
   /* Only the plain XY[ZW]/RGB/ST stream, and no fragment state which
    * would make the result differ from a copy. */
   if (ctx->vertex_alpha || ctx->alpha_micro || ctx->alpha_micro_cutout ||
       ctx->cpu_alpha_cutout || ctx->rgb_alpha_two_pass ||
       ctx->native_sampled_alpha ||
       (ctx->blend && (ctx->blend->rt[0].blend_enable ||
                       ctx->blend->logicop_enable ||
                       ctx->blend->rt[0].colormask != PIPE_MASK_RGBA)) ||
       (ctx->dsa && (ctx->dsa->alpha_enabled ||
                     ctx->dsa->stencil[0].enabled)) ||
       (src->linear_map && ctx->rasterizer && ctx->rasterizer->scissor))
      return false;
   if (!src->linear_map)
      return ph_present_tiled_quads(ctx, dst, src);
   for (unsigned v = 0; v < 6; v++) {
      const float *p = &ctx->vertices[v * stride];
      minx = MIN2(minx, p[0]);
      miny = MIN2(miny, p[1]);
      maxx = MAX2(maxx, p[0]);
      maxy = MAX2(maxy, p[1]);
   }
   x0 = lrintf(minx);
   y0 = lrintf(miny);
   width = lrintf(maxx - minx);
   height = lrintf(maxy - miny);
   if (fabsf(minx - x0) > 0.01f || fabsf(miny - y0) > 0.01f ||
       fabsf(maxx - minx - width) > 0.01f ||
       fabsf(maxy - miny - height) > 0.01f ||
       width != (int)src->base.width0 ||
       height != (int)src->base.height0 ||
       x0 < 0 || y0 < 0 || x0 + width > (int)dst->base.width0 ||
       y0 + height > (int)dst->base.height0)
      return false;
   /* The copy places texel (s,t) at (x0 + s*w, y0 + t*h): every vertex must
    * carry exactly that texture coordinate, unflipped. */
   for (unsigned v = 0; v < 6; v++) {
      const float *p = &ctx->vertices[v * stride];

      if (fabsf(p[st] - (p[0] - x0) / width) > 0.001f ||
          fabsf(p[st + 1] - (p[1] - y0) / height) > 0.001f)
         return false;
   }
   /* A render-shadow target has no linear companion for the GPU copy, and
    * CPU writes to its shadow would never reach VRAM. */
   if (dst->transfer_shadow && !dst->linear_handle)
      return false;
   /* The copy and its CPU fallback consume both mappings immediately. */
   if (ctx->aggregate_batches && ph_drain_batch(ctx))
      return false;
   ph_resource_resolve_linear(src);
   ph_resource_resolve_linear(dst);
   if (!ph_env("PARHELIA_CPU_BLIT") &&
       src->linear_handle && !(src->linear_stride & 3) &&
       src->linear_stride / 4 <= 0x3fff) {
      struct parhelia_batch *batch = &ctx->build_batch;
      int ret = -EINVAL;

      if (dst->linear_handle && !(dst->linear_stride & 3) &&
          dst->linear_stride / 4 <= 0x3fff) {
         ret = parhelia_batch_blit(batch,
                                   dst->linear_handle,
                                   dst->linear_stride / 4,
                                   1, x0, y0,
                                   src->linear_handle,
                                   src->linear_stride / 4,
                                   1, 0, 0, width, height);
         if (!ret)
            ret = ph_submit_batch_now_untimed(ctx, batch, NULL, 0);
         if (!ret)
            ret = ph_wait_async(ctx);
      }
      if (!ret) {
         dst->tiled_dirty = false;
         dst->linear_dirty = true;
         dst->mip_defined |= BITFIELD_BIT(0);
         dst->content_generation++;
         if (ph_env("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: GPU present src=%u dst=%u/%u rect=%d,%d %dx%d\n",
                    src->linear_handle, dst->handle, dst->linear_handle,
                    x0, y0, width, height);
         return true;
      }
      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr, "parhelia: GPU present failed: %d\n", ret);
   }
   for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
         const uint8_t *pixel = (const uint8_t *)src->linear_map +
                                (size_t)y * src->linear_stride + x * 4;
         if (dst->linear_map)
            memcpy((uint8_t *)dst->linear_map +
                      (size_t)(y0 + y) * dst->linear_stride + (x0 + x) * 4,
                   pixel, 4);
         if (dst->map)
            memcpy((uint8_t *)dst->map +
                      p3d_tile_offset_layout(x0 + x, y0 + y, dst->stride,
                                             dst->tile_layout), pixel, 4);
      }
   }
   dst->tiled_dirty = !dst->linear_map;
   dst->linear_dirty = false;
   dst->mip_defined |= BITFIELD_BIT(0);
   dst->content_generation++;
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: CPU present fallback src=%u dst=%u rect=%d,%d %dx%d\n",
              src->linear_handle, dst->handle, x0, y0, width, height);
   return true;
}

static void
ph_pack_multitex_vertices(struct ph_context *ctx, bool second, bool depth,
                          bool oow)
{
   /* x, y[, z[, 1/w]], r, g, b (or one packed dword), s0, t0, s1, t1 */
   const unsigned pos = oow ? 4 : depth ? 3 : 2;
   const unsigned colour = ctx->packed_colour ? 1 : 3;
   const unsigned src_dpv = pos + colour + 4;
   const unsigned dst_dpv = pos + colour + 2;
   const unsigned st = pos + colour + (second ? 2 : 0);

   for (unsigned v = 0; v < ctx->triangles * 3; v++) {
      const float *src = &ctx->vertices[v * src_dpv];
      float *dst = &ctx->packed_vertices[v * dst_dpv];

      for (unsigned i = 0; i < pos + colour; i++)
         *dst++ = src[i];
      *dst++ = src[st];
      *dst++ = src[st + 1];
   }
}

static enum p3d_mip
ph_sampler_mip_filter(const struct pipe_sampler_state *sampler)
{
   if (!sampler)
      return P3D_MIP_NONE;
   switch (sampler->min_mip_filter) {
   case PIPE_TEX_MIPFILTER_NEAREST:
      return P3D_MIP_NEAREST;
   case PIPE_TEX_MIPFILTER_LINEAR:
      return P3D_MIP_LINEAR;
   default:
      return P3D_MIP_NONE;
   }
}

/* A view's first level can be the hardware base only when it is stored in
 * the native base-level permutation; the texture unit derives the rest of the
 * chain from that base exactly like the vendor layout does from level 0. */
static unsigned
ph_view_first_level(const struct parhelia_resource *texture,
                    const struct pipe_sampler_view *view)
{
   unsigned first = view ? view->u.tex.first_level : 0;

   /* a compressed chain is derived from its base by the unit */
   if (!first || first >= ph_resource_mip_levels(texture) ||
       texture->mip.block_bytes ||
       texture->mip.level[first].pitch < 64 ||
       /* a 64x32 tail level inside its tile is no base the unit can name */
       (texture->mip.native_64x32 &&
        (texture->mip.level[first].tail_x || texture->mip.level[first].tail_y)) ||
       !(texture->mip_defined & BITFIELD_BIT(first)))
      return 0;
   return first;
}

static unsigned
ph_view_mip_levels(const struct parhelia_resource *texture,
                   const struct pipe_sampler_view *view)
{
   unsigned levels = ph_resource_mip_levels(texture);
   unsigned first = ph_view_first_level(texture, view), last;

   if (texture->mip.sampled_levels)
      levels = MIN2(levels, texture->mip.sampled_levels);

   if (!view || !levels || view->u.tex.first_level != first)
      return 1;
   last = MIN2(levels - 1, view->u.tex.last_level);
   return last >= first ? last - first + 1 : 1;
}

static void
ph_apply_view_base(const struct parhelia_resource *texture,
                   const struct pipe_sampler_view *view, bool tiled,
                   struct p3d_texture *tex)
{
   unsigned first = tiled ? ph_view_first_level(texture, view) : 0;

   if (!first)
      return;
   tex->width = texture->mip.level[first].width;
   tex->height = texture->mip.level[first].height;
   tex->reloc_offset = texture->mip.level[first].offset;
}

/* PARHELIA_NO_MIPS=1 samples the base level only (diagnostic). */
static bool
ph_debug_no_mips(void)
{
   static int cached = -1;

   if (cached < 0)
      cached = debug_get_bool_option("PARHELIA_NO_MIPS", false);
   return cached;
}

static bool
ph_use_mips(const struct parhelia_resource *texture,
            const struct pipe_sampler_state *sampler,
            const struct pipe_sampler_view *view)
{
   unsigned levels;

   if (!texture || !sampler ||
       sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
       ph_debug_no_mips())
      return false;
   levels = ph_view_mip_levels(texture, view);
   return levels > 1 &&
      ph_mip_ready_levels(texture->mip_defined >>
                          ph_view_first_level(texture, view), levels) == levels;
}

/* Drop one draw or clear the hardware cannot do, reporting it once per call
 * site.  Unlike a submit failure this leaves the context usable. */
#define PH_REFUSE(...) do {                                 \
      static bool said_;                                    \
      if (!said_) {                                         \
         said_ = true;                                      \
         fprintf(stderr, "parhelia: " __VA_ARGS__);         \
      }                                                     \
   } while (0)

/* Report an unsupportable state once per process, never per draw. */
static void PRINTFLIKE(1, 2)
ph_warn_once(const char *fmt, ...)
{
   static bool said;
   va_list ap;

   if (said)
      return;
   said = true;
   va_start(ap, fmt);
   fprintf(stderr, "parhelia: ");
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

/* One Gallium wrap mode onto the address unit's 3-bit code.  False means the
 * unit has no such mode: the caller must refuse the draw, because substituting
 * a different one silently renders the wrong image. */
static bool
ph_sampler_wrap(unsigned wrap, enum p3d_wrap *code)
{
   /* PARHELIA_LEGACY_WRAP restores the pre-measurement mapping, which sent
    * every non-repeat mode to code 6.  It is wrong, and exists only to A/B
    * the cost of the real one. */
   if (ph_env("PARHELIA_LEGACY_WRAP")) {
      *code = wrap == PIPE_TEX_WRAP_REPEAT ? P3D_WRAP_REPEAT : P3D_WRAP_CLAMP;
      return true;
   }
   switch (wrap) {
   case PIPE_TEX_WRAP_REPEAT:
      *code = P3D_WRAP_REPEAT;
      return true;
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      *code = P3D_WRAP_CLAMP_EDGE;
      return true;
   case PIPE_TEX_WRAP_CLAMP:
      *code = P3D_WRAP_CLAMP;
      return true;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      *code = P3D_WRAP_BORDER;
      return true;
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      *code = P3D_WRAP_MIRROR;
      return true;
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      *code = P3D_WRAP_MIRROR_ONCE;
      return true;
   default:
      /* PIPE_TEX_WRAP_MIRROR_CLAMP and MIRROR_CLAMP_TO_BORDER: the unit
       * mirrors once only into the edge, never into the border. */
      return false;
   }
}

/* GL_TEXTURE_BORDER_COLOR as an A8R8G8B8 value; p3d_border_colour() then
 * packs it for the sampled format's class.  GL clamps to [0,1] here, which
 * the vendor's own stack does not. */
static uint32_t
ph_sampler_border_colour(const struct pipe_sampler_state *sampler,
                         const struct pipe_sampler_view *view)
{
   static const unsigned shift[4] = {16, 8, 0, 24};
   const struct ph_sampler_view *pv = (const struct ph_sampler_view *)view;
   uint32_t colour = 0;

   if (!sampler)
      return 0;
   for (unsigned i = 0; i < 4; i++) {
      const unsigned s = pv && pv->baked ? pv->baked_swizzle[i] : i;
      /* A depth copy holds (z, 0, 0, 1) before the swizzle. */
      const bool z = pv && pv->baked_depth;
      const float c[6] = { sampler->border_color.f[0],
                           z ? 0.0f : sampler->border_color.f[1],
                           z ? 0.0f : sampler->border_color.f[2],
                           z ? 1.0f : sampler->border_color.f[3], 0.0f, 1.0f };
      float v = CLAMP(s <= PIPE_SWIZZLE_1 ? c[s] : 0.0f, 0.0f, 1.0f);

      colour |= (uint32_t)(v * 255.0f + 0.5f) << shift[i];
   }
   return colour;
}

/* Fill a descriptor's per-axis address modes and its border colour.  The R
 * axis is only programmed where the sampler runs three coordinates, so a
 * GL_TEXTURE_WRAP_R the unit cannot express never refuses a 2-D draw. */
static bool
ph_sampler_wrap_axes(const struct pipe_sampler_state *sampler,
                     const struct pipe_sampler_view *view,
                     enum pipe_texture_target target, struct p3d_texture *tex)
{
   const bool three = target == PIPE_TEXTURE_3D ||
                      target == PIPE_TEXTURE_CUBE ||
                      target == PIPE_TEXTURE_CUBE_ARRAY;

   if (!sampler) {
      tex->wrap_s = tex->wrap_t = tex->wrap_r = P3D_WRAP_REPEAT;
      return true;
   }
   tex->border_colour = ph_sampler_border_colour(sampler, view);
   /* A 1-D image is one row of a 2-D surface and GL ignores t for it, so
    * its T axis must never reach the border or a filtered neighbour row. */
   if (target == PIPE_TEXTURE_1D) {
      tex->wrap_t = P3D_WRAP_CLAMP_EDGE;
      return ph_sampler_wrap(sampler->wrap_s, &tex->wrap_s);
   }
   return ph_sampler_wrap(sampler->wrap_s, &tex->wrap_s) &&
          ph_sampler_wrap(sampler->wrap_t, &tex->wrap_t) &&
          (!three || ph_sampler_wrap(sampler->wrap_r, &tex->wrap_r));
}

/* The LOD bias range the sampler is given: 0x11c0[24:16] is signed 4.4 with
 * a -4.0 offset, so it reaches -12 .. +19.9; the vendor clamps to +/-4.  On
 * the P690 biases -5 .. +11 select exactly the expected level (MEASURED,
 * tex-miplevel-selection and a magnified 512^2 chain); -12 .. -6 follow
 * the field.  The original family keeps the vendor clamp. */
static float
ph_max_lod_bias(const struct ph_context *ctx)
{
   return ph_screen(ctx->base.screen)->max_lod_bias;
}

static uint32_t
ph_sampler_filter_word(const struct p3d_texture *texture,
                       const struct pipe_sampler_state *sampler,
                       float max_bias)
{
   float bias = sampler ? CLAMP(sampler->lod_bias, -max_bias, max_bias) : 0.0f;
   int fixed_bias = (int)((bias - 4.0f) * 16.0f);
   uint32_t word = 0x1000u |
      ((uint32_t)texture->min_filter & 3u) |
      (((uint32_t)texture->mag_filter & 3u) << 3) |
      (((uint32_t)texture->mip_filter & 3u) << 6) |
      (((uint32_t)fixed_bias & 0x1ffu) << 16);

   /* The vendor builder sets this address-pipe quirk for the one asymmetric
    * minification combination. */
   if (texture->min_filter == P3D_FILTER_NEAREST &&
       texture->mag_filter == P3D_FILTER_LINEAR &&
       texture->mip_filter != P3D_MIP_NONE)
      word |= 1u << 10;
   return word;
}

static uint32_t
ph_sampler_wrap_word(const struct p3d_texture *texture,
                     const struct pipe_sampler_state *sampler)
{
   float min_lod = sampler ? CLAMP(sampler->min_lod, 0.0f, 15.0f) : 0.0f;
   float max_lod = sampler ? CLAMP(sampler->max_lod, 0.0f, 15.0f) : 15.0f;
   unsigned fixed_min = CLAMP((int)(min_lod * 16.0f), 0, 240);
   unsigned fixed_max = CLAMP((int)(max_lod * 16.0f), 0, 240);

   return ((uint32_t)texture->wrap_s & 7u) |
      (((uint32_t)texture->wrap_t & 7u) << 3) |
      (((uint32_t)texture->wrap_r & 7u) << 6) |
      (fixed_min << 16) | (fixed_max << 24);
}

/* P690's working OpenGL texture route is layout 2.  It accepts the complete
 * family of ordinary 2-D texture sizes used by Q3, including vendor-layout
 * mip chains.  Scanout/presentation images retain layout 0 because they are
 * consumed through the CPU-visible linear shadow instead. */
static bool
ph_auto_tiled_texture(const struct ph_context *ctx,
                      const struct parhelia_resource *texture,
                      const struct pipe_sampler_state *sampler)
{
   (void)sampler;
   /* Render-target storage keeps the surface order, which is the sampler's
    * with y bits 0 and 1 exchanged: it is sampled from its linear shadow. */
   return texture && ph_resource_uses_sampler_layout(texture) &&
       ph_lx_family(ph_screen(ctx->base.screen)->chip_id) &&
       (texture->base.target == PIPE_TEXTURE_2D ||
        texture->base.target == PIPE_TEXTURE_1D ||
        texture->base.target == PIPE_TEXTURE_3D ||
        texture->base.target == PIPE_TEXTURE_CUBE) &&
       !(texture->base.bind & PIPE_BIND_DEPTH_STENCIL) &&
       !(texture->base.bind & (PIPE_BIND_SCANOUT |
                               PIPE_BIND_DISPLAY_TARGET |
                               PIPE_BIND_SHARED));
}

/* Unit 0 samples a volume or a cube map, so the draw needs a three-component
 * coordinate set.  That set replaces the vertex colour: the attribute port
 * carries 7 dwords per vertex (8 with a depth position) and position plus
 * colour plus three coordinates does not fit in either. */
static bool
ph_three_coord_texture(const struct ph_context *ctx)
{
   const struct parhelia_resource *r;

   if (!ctx->num_sampler_views || !ctx->sampler_views[0] ||
       !ctx->sampler_views[0]->texture)
      return false;
   r = ph_resource(ctx->sampler_views[0]->texture);
   return r->mip.slices != 0;
}

/* GL_MODULATE by white is the texture itself, so a white primitive is exact
 * without the colour attribute. */
static bool
ph_primitive_colour_is_white(const struct prim_header *prim, int col)
{
   if (col < 0)
      return true;
   for (unsigned v = 0; v < 3; v++) {
      const float *c = prim->v[v]->data[col];

      for (unsigned i = 0; i < 3; i++)
         if (c[i] < 0.998f)
            return false;
   }
   return true;
}

/* P690 can recover RGBA sampled alpha in one draw by binding the original
 * image on unit 0 and an opaque (A,A,A,1) companion on unit 1.  The final
 * combiner moves unit-0 RGB and takes fragment alpha from unit-1 red.  Keep
 * this to the two live Q3 fixed-function equations and base-level sampling;
 * the companion does not carry a mip chain. */
static bool
ph_use_native_sampled_alpha(struct ph_context *ctx, bool textured,
                            bool multitextured, bool depth)
{
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;
   const struct pipe_sampler_state *sampler;
   const struct parhelia_resource *texture, *color, *zs = NULL;
   bool source_over, cutout;

   /* Keep the newly proven combiner route explicit until both live Q3 draw
    * classes have passed sustained scene testing.  "cutout" and "blend"
    * isolate the two state classes; "all" enables both. */
   if (!ctx->native_sampled_alpha_modes || !textured || multitextured ||
       !ctx->p690_multitexture ||
       !ctx->warp_textured_multitex || !ctx->fs || !ctx->dsa || !blend ||
       ctx->fs->sampler_mask != BITFIELD_BIT(0) ||
       ctx->fs->texenv != P3D_TEXENV_MODULATE ||
       !ctx->num_sampler_views || !ctx->sampler_views[0] ||
       !ctx->sampler_views[0]->texture || !ctx->num_samplers ||
       !(sampler = ctx->samplers[0]) || !ctx->fb.nr_cbufs ||
       !ctx->fb.cbufs[0].texture)
      return false;

   texture = ph_resource(ctx->sampler_views[0]->texture);
   color = ph_resource(ctx->fb.cbufs[0].texture);
   if (depth) {
      if (!ctx->fb.zsbuf.texture)
         return false;
      zs = ph_resource(ctx->fb.zsbuf.texture);
   }
   if (texture->base.target != PIPE_TEXTURE_2D ||
       texture->base.format != PIPE_FORMAT_R8G8B8A8_UNORM ||
       !texture->upload_texture || !texture->handle || !texture->map ||
       !texture->mip.levels ||
       (texture->mip_defined & ~BITFIELD_BIT(0)) ||
       ph_use_mips(texture, sampler, ctx->sampler_views[0]) ||
       ph_alpha_companion_extent(texture->base.width0) > 0x3fff ||
       ph_alpha_companion_extent(texture->base.height0) > 0x3fff ||
       !color->handle ||
       !ph_alpha_route_colour(color->base.format) ||
       (depth && (!zs->handle ||
                  !ph_alpha_route_depth(ctx->fb.zsbuf.format))) ||
       !((sampler->min_img_filter == PIPE_TEX_FILTER_NEAREST ||
          sampler->min_img_filter == PIPE_TEX_FILTER_LINEAR) &&
         (sampler->mag_img_filter == PIPE_TEX_FILTER_NEAREST ||
          sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR)) ||
       !((sampler->wrap_s == PIPE_TEX_WRAP_REPEAT ||
          sampler->wrap_s == PIPE_TEX_WRAP_CLAMP_TO_EDGE) &&
         (sampler->wrap_t == PIPE_TEX_WRAP_REPEAT ||
          sampler->wrap_t == PIPE_TEX_WRAP_CLAMP_TO_EDGE)) ||
       ctx->dsa->stencil[0].enabled || ctx->dsa->stencil[1].enabled ||
       blend->colormask != PIPE_MASK_RGBA ||
       (ctx->rasterizer && (ctx->rasterizer->flatshade ||
                            ctx->rasterizer->poly_stipple_enable)))
      return false;

   source_over = !ctx->dsa->alpha_enabled && blend->blend_enable &&
      blend->rgb_func == PIPE_BLEND_ADD &&
      blend->alpha_func == PIPE_BLEND_ADD &&
      blend->rgb_src_factor == PIPE_BLENDFACTOR_SRC_ALPHA &&
      blend->rgb_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA &&
      blend->alpha_src_factor == PIPE_BLENDFACTOR_SRC_ALPHA &&
      blend->alpha_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   /* Both live cutout images (8x16 mixed alpha and 32x64 opaque) report only
    * BITFIELD_BIT(0).  Accept that correct bookkeeping value, while the mask
    * above rejects every populated child level. */
   cutout = texture->mip_defined == BITFIELD_BIT(0) &&
      ctx->dsa->alpha_enabled &&
      ctx->dsa->alpha_func == PIPE_FUNC_GEQUAL &&
      fabsf(ctx->dsa->alpha_ref_value - 0.5f) <= 0.0001f &&
      depth && ctx->dsa->depth_enabled && ctx->dsa->depth_writemask &&
      ctx->dsa->depth_func == PIPE_FUNC_LEQUAL && !blend->blend_enable;
   return (source_over && (ctx->native_sampled_alpha_modes & 2)) ||
          (cutout && (ctx->native_sampled_alpha_modes & 1));
}

static bool
ph_native_sampled_alpha_primitive(const struct prim_header *prim, int col)
{
   if (col < 0)
      return true;
   for (unsigned i = 0; i < 3; i++)
      if (fabsf(prim->v[i]->data[col][3] - 1.0f) > 0.0001f)
         return false;
   return true;
}

/* Small texture dimensions recur throughout Q3.  The alpha-cutout trace must
 * therefore record an image identity before a fallback can be gated safely.
 * This hashes the GPU-order base level as sampled, without changing resource
 * ownership or dirty state, and runs only under the diagnostic environment. */
static uint64_t
ph_texture_base_hash(const struct parhelia_resource *texture, bool tiled,
                     unsigned *alpha_zero, unsigned *alpha_nonzero)
{
   const uint8_t *base;
   uint64_t hash = 0xcbf29ce484222325ULL;
   unsigned zero = 0, nonzero = 0;
   bool use_tiled;

   if (alpha_zero)
      *alpha_zero = 0;
   if (alpha_nonzero)
      *alpha_nonzero = 0;
   if (!texture || (!texture->map && !texture->linear_map))
      return 0;
   use_tiled = tiled && texture->map;
   base = use_tiled ? texture->map :
          (texture->linear_map ? texture->linear_map : texture->map);
   for (unsigned y = 0; y < texture->base.height0; y++) {
      for (unsigned x = 0; x < texture->base.width0; x++) {
         size_t off;
         const uint8_t *pixel;

         if (use_tiled) {
            off = texture->mip.levels ?
                  ph_mip_texel_offset(&texture->mip, 0, x, y, false) :
                  p3d_texture_tile_offset_chip(
                     x, y, ph_sampler_storage_pitch(texture->base.screen,
                                                    texture->base.width0, 4), 4,
                     ph_screen(texture->base.screen)->chip_id,
                     texture->tile_layout);
         } else if (texture->linear_map) {
            off = texture->linear_mip.levels ?
               ph_mip_texel_offset(&texture->linear_mip, 0, x, y, true) :
               (size_t)y * texture->linear_stride + x * 4;
         } else {
            off = p3d_tile_offset_layout_cpp(x, y, texture->stride,
                                             texture->tile_layout,
                                             texture->cpp);
         }
         pixel = base + off;
         for (unsigned c = 0; c < 4; c++)
            hash = (hash ^ pixel[c]) * 0x100000001b3ULL;
         if (pixel[3])
            nonzero++;
         else
            zero++;
      }
   }
   if (alpha_zero)
      *alpha_zero = zero;
   if (alpha_nonzero)
      *alpha_nonzero = nonzero;
   return hash;
}

/* Build the ordinary 0x32 images consumed by the native one-pass and older
 * two-pass sampled-alpha routes.  The source stays in its normal RGBA
 * allocation; companion BOs are refreshed only when level zero or padding
 * wrap semantics change. */
static int
ph_prepare_alpha_companions(struct ph_context *ctx,
                            struct parhelia_resource *texture,
                            const struct pipe_sampler_state *sampler,
                            bool native_only)
{
   struct parhelia_screen *screen = ph_screen(ctx->base.screen);
   const unsigned companion_width = native_only ?
      ph_alpha_companion_extent(texture->base.width0) : texture->base.width0;
   const unsigned companion_height = native_only ?
      ph_alpha_companion_extent(texture->base.height0) : texture->base.height0;
   const bool need_premul = !native_only;
   const bool repeat_s = sampler->wrap_s == PIPE_TEX_WRAP_REPEAT;
   const bool repeat_t = sampler->wrap_t == PIPE_TEX_WRAP_REPEAT;
   struct drm_parhelia_create_render alpha = {
      .width = p3d_texture_uses_native_64x16(screen->chip_id,
                                             screen->tile_layout, 4) ?
                  MAX2(companion_width, 64u) : companion_width,
      .height = companion_height,
      .flags = PARHELIA_CREATE_RENDER_TEXTURE,
   };
   struct drm_parhelia_create_render premul = alpha;
   struct drm_parhelia_upload_render upload;
   uint8_t *alpha_image = NULL, *premul_image = NULL;
   int ret = 0;

   if (texture->alpha_rgb_handle &&
       (!need_premul || texture->premul_rgb_handle) &&
       texture->alpha_companion_generation == texture->content_generation &&
       texture->alpha_companion_width == companion_width &&
       texture->alpha_companion_height == companion_height &&
       texture->alpha_companion_repeat_s == repeat_s &&
       texture->alpha_companion_repeat_t == repeat_t)
      return 0;
   if (!texture->map || !texture->mip.levels)
      return -EINVAL;
   /* The kernel orders UPLOAD_RENDER after every submitted draw (queued blit
    * or drained aperture copy): only batched draws must go out first. */
   if ((ret = ph_drain_batch(ctx)))
      return ret;
   texture->alpha_companion_generation = UINT64_MAX;

   if (texture->alpha_companion_width != companion_width ||
       texture->alpha_companion_height != companion_height) {
      if (texture->alpha_rgb_handle) {
         struct drm_gem_close close = {.handle = texture->alpha_rgb_handle};
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
      }
      if (texture->premul_rgb_handle) {
         struct drm_gem_close close = {.handle = texture->premul_rgb_handle};
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
      }
      texture->alpha_rgb_handle = texture->premul_rgb_handle = 0;
      texture->alpha_companion_width = 0;
      texture->alpha_companion_height = 0;
   }
   if (!texture->alpha_rgb_handle) {
      if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &alpha)) {
         ret = -errno;
         goto out;
      }
      if (!alpha.handle || (alpha.pitch & 3) ||
          alpha.pitch / 4 != ph_sampler_storage_pitch(&screen->base,
                                                       companion_width, 4) ||
          alpha.size < (uint64_t)alpha.pitch * companion_height) {
         struct drm_gem_close close = {.handle = alpha.handle};
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
         ret = -EINVAL;
         goto out;
      }
      texture->alpha_rgb_handle = alpha.handle;
      texture->alpha_companion_size = alpha.size;
      texture->alpha_companion_stride = alpha.pitch / 4;
      texture->alpha_companion_width = companion_width;
      texture->alpha_companion_height = companion_height;
   }
   if (need_premul && !texture->premul_rgb_handle) {
      if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &premul)) {
         ret = -errno;
         goto out;
      }
      if (!premul.handle || (premul.pitch & 3) ||
          premul.pitch / 4 != texture->alpha_companion_stride ||
          premul.size != texture->alpha_companion_size) {
         struct drm_gem_close close = {.handle = premul.handle};
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
         ret = -EINVAL;
         goto out;
      }
      texture->premul_rgb_handle = premul.handle;
   }

   alpha_image = calloc(1, texture->alpha_companion_size);
   if (need_premul)
      premul_image = calloc(1, texture->alpha_companion_size);
   if (!alpha_image || (need_premul && !premul_image)) {
      ret = -ENOMEM;
      goto out;
   }
   for (unsigned y = 0; y < companion_height; y++) {
      unsigned src_y = ph_alpha_companion_texel(y, texture->base.height0,
                                                repeat_t);

      for (unsigned x = 0; x < companion_width; x++) {
         unsigned src_x = ph_alpha_companion_texel(x, texture->base.width0,
                                                   repeat_s);
         size_t src_off = ph_mip_texel_offset(&texture->mip, 0, src_x,
                                              src_y, false);
         size_t dst_off = p3d_texture_tile_offset_chip(
            x, y, texture->alpha_companion_stride, 4, screen->chip_id,
            screen->tile_layout);
         const uint8_t *src;
         uint8_t *mask, *rgb;
         unsigned a;

         if (src_off + 4 > texture->size ||
             dst_off + 4 > texture->alpha_companion_size) {
            ret = -EOVERFLOW;
            goto out;
         }
         src = (const uint8_t *)texture->map + src_off;
         mask = alpha_image + dst_off;
         rgb = need_premul ? premul_image + dst_off : NULL;
         a = src[3];
         mask[0] = mask[1] = mask[2] = a;
         mask[3] = 255;
         if (rgb) {
            rgb[0] = (src[0] * a + 127) / 255;
            rgb[1] = (src[1] * a + 127) / 255;
            rgb[2] = (src[2] * a + 127) / 255;
            rgb[3] = 255;
         }
      }
   }
   texture->alpha_companion_repeat_s = repeat_s;
   texture->alpha_companion_repeat_t = repeat_t;
   upload = (struct drm_parhelia_upload_render) {
      .data = (uintptr_t)alpha_image,
      .size = texture->alpha_companion_size,
      .handle = texture->alpha_rgb_handle,
   };
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_UPLOAD_RENDER, &upload)) {
      ret = -errno;
      goto out;
   }
   if (need_premul) {
      upload.data = (uintptr_t)premul_image;
      upload.handle = texture->premul_rgb_handle;
      if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_UPLOAD_RENDER, &upload)) {
         ret = -errno;
         goto out;
      }
   }
   texture->alpha_companion_generation = texture->content_generation;
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: RGB-alpha companions texture=%u alpha=%u premul=%u physical=%ux%u repeat=%u/%u generation=%llu\n",
              texture->handle, texture->alpha_rgb_handle,
              texture->premul_rgb_handle,
              texture->alpha_companion_width,
              texture->alpha_companion_height, repeat_s, repeat_t,
              (unsigned long long)texture->content_generation);
out:
   free(premul_image);
   free(alpha_image);
   return ret;
}

static bool
ph_cpu_alpha_blend_depth_write_image(uint64_t image)
{
   return image == UINT64_C(0xe98fa2a3b0349695);
}

/* P690 does not deliver sampled alpha to its fixed-function comparator even
 * though the documented texture/ALU state is present.  Every condition below
 * comes from the live Q3 trace; any other draw remains on the native hardware
 * path. */
static bool
ph_use_cpu_alpha_cutout(struct ph_context *ctx, bool textured,
                        bool multitextured, bool depth, bool has_colour)
{
	if (!ph_alpha_route_enabled(8))
		return false;
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;
   const struct pipe_sampler_state *sampler;
   struct parhelia_resource *texture;
   const struct parhelia_resource *color, *zs;
   uint64_t image;

   if (!textured || multitextured ||
       !depth || !ctx->fs || !ctx->dsa || !blend ||
       ph_screen(ctx->base.screen)->chip_id != 0x2539 ||
       !ctx->num_sampler_views || !ctx->sampler_views[0] ||
       !ctx->sampler_views[0]->texture || !ctx->num_samplers ||
       !ctx->samplers[0] || !ctx->fb.nr_cbufs ||
       !ctx->fb.cbufs[0].texture || !ctx->fb.zsbuf.texture)
      return false;
   texture = ph_resource(ctx->sampler_views[0]->texture);
   color = ph_resource(ctx->fb.cbufs[0].texture);
   zs = ph_resource(ctx->fb.zsbuf.texture);
   sampler = ctx->samplers[0];
   /* Q3's font draw has no COLOR output.  ph_stage_tri() supplies opaque
    * white in that case, which is exactly the missing MODULATE term. */
   (void)has_colour;
   if (ctx->fs->sampler_mask != BITFIELD_BIT(0) ||
       ctx->fs->texenv != P3D_TEXENV_MODULATE ||
       texture->base.target != PIPE_TEXTURE_2D ||
       texture->base.format != PIPE_FORMAT_R8G8B8A8_UNORM ||
       !texture->upload_texture || !texture->map ||
       /* This route rasterizes on the CPU into the linear companion images,
        * reading and writing a 32-bit pixel and a 32-bit Z24 word directly
        * (ph_cpu_linear_pixel(), ph_cpu_depth_pass()).  A 16 bpp target has
        * neither a linear companion nor those word layouts, so it stays on
        * the native sampled-alpha route above, which now covers 5:6:5. */
       (color->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        color->base.format != PIPE_FORMAT_B8G8R8X8_UNORM) || !color->linear_map ||
       !zs->linear_map || sampler->min_img_filter != PIPE_TEX_FILTER_LINEAR ||
       sampler->mag_img_filter != PIPE_TEX_FILTER_LINEAR ||
       /* P690's native sampler uses the base image for this mode too. */
       sampler->min_mip_filter != PIPE_TEX_MIPFILTER_NEAREST ||
       sampler->wrap_s != PIPE_TEX_WRAP_REPEAT ||
       sampler->wrap_t != PIPE_TEX_WRAP_REPEAT ||
       !ctx->dsa->alpha_enabled || ctx->dsa->alpha_func != PIPE_FUNC_GEQUAL ||
       fabsf(ctx->dsa->alpha_ref_value - 0.5f) > 0.0001f ||
       !ctx->dsa->depth_enabled || !ctx->dsa->depth_writemask ||
       ctx->dsa->depth_func != PIPE_FUNC_LEQUAL ||
       ctx->dsa->stencil[0].enabled || ctx->dsa->stencil[1].enabled ||
       blend->blend_enable || blend->colormask != PIPE_MASK_RGBA ||
       (ctx->rasterizer && (ctx->rasterizer->flatshade ||
                            ctx->rasterizer->poly_stipple_enable)) ||
       (ctx->fb.zsbuf.format != PIPE_FORMAT_Z24_UNORM_S8_UINT &&
        ctx->fb.zsbuf.format != PIPE_FORMAT_Z24X8_UNORM))
      return false;

   /* Stage selection needs the same logical tiled image that submission will
    * sample.  Texture uploads are CPU copies into the mapped resource, so
    * making it current here emits no GPU work or synchronization. */
   ph_resource_upload_tiled(texture);
   if (ctx->cpu_cutout_hash_resource != texture ||
       ctx->cpu_cutout_hash_handle != texture->handle ||
       ctx->cpu_cutout_hash_generation != texture->content_generation) {
      image = ph_texture_base_hash(texture, true, NULL, NULL);
      ctx->cpu_cutout_hash_resource = texture;
      ctx->cpu_cutout_hash_handle = texture->handle;
      ctx->cpu_cutout_hash_generation = texture->content_generation;
      ctx->cpu_cutout_hash = image;
   } else {
      image = ctx->cpu_cutout_hash;
   }
   return image == UINT64_C(0x16a69725e9886957) ||
          image == UINT64_C(0xe936630e9679a61c);
}

/* Q3's base-level RGBA HUD textures contain transparent white texels. P690
 * samples their RGB but loses alpha before SRC_ALPHA blending, producing
 * opaque glyph quads. */
static bool
ph_use_rgb_alpha_two_pass(struct ph_context *ctx, bool textured,
                          bool multitextured, bool has_colour)
{
	if (!ph_alpha_route_enabled(16))
		return false;
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;
   const struct pipe_sampler_state *sampler;
   struct parhelia_resource *texture;
   const struct parhelia_resource *color, *zs = NULL;
   unsigned alpha_zero, alpha_nonzero;
   uint64_t image;
   bool trace_candidate = false;

   /* ph_stage_tri supplies opaque white when this fixed-function UI shader
    * has no COLOR output, which is the correct diffuse term for MODULATE. */
   (void)has_colour;

   if (!textured || multitextured ||
       !ctx->fs || !ctx->dsa || !blend ||
       ph_screen(ctx->base.screen)->chip_id != 0x2539 ||
       !ctx->num_sampler_views || !ctx->sampler_views[0] ||
       !ctx->sampler_views[0]->texture || !ctx->num_samplers ||
       !ctx->samplers[0] || !ctx->fb.nr_cbufs ||
       !ctx->fb.cbufs[0].texture)
      return false;
   texture = ph_resource(ctx->sampler_views[0]->texture);
   color = ph_resource(ctx->fb.cbufs[0].texture);
   if (ctx->dsa->depth_enabled) {
      if (!ctx->fb.zsbuf.texture)
         return false;
      zs = ph_resource(ctx->fb.zsbuf.texture);
   }
   sampler = ctx->samplers[0];
   trace_candidate = ph_env("PARHELIA_TRACE_CPU_ALPHA") &&
      texture->base.width0 <= 256 && texture->base.height0 <= 256 &&
      ctx->trace_cpu_alpha_resource != texture;
   if (trace_candidate) {
      ctx->trace_cpu_alpha_resource = texture;
      fprintf(stderr,
              "parhelia: cpu-alpha candidate tex=%ux%u fmt=%u target=%u map=%u mip=%#x fs=%#x/%u cbuf=%u linear=%u z=%u/%u/%u sampler=%u/%u/%u wrap=%u/%u depth=%u/%u alpha=%u stencil=%u/%u blend=%u/%u,%u alpha-blend=%u/%u,%u mask=%#x raster=%u/%u\\n",
              texture->base.width0, texture->base.height0, texture->base.format,
              texture->base.target, !!texture->map, texture->mip_defined,
              ctx->fs->sampler_mask, ctx->fs->texenv, color->base.format,
              !!color->linear_map, !!ctx->fb.zsbuf.texture,
              zs ? !!zs->linear_map : 0, ctx->fb.zsbuf.format,
              sampler->min_img_filter, sampler->mag_img_filter,
              sampler->min_mip_filter, sampler->wrap_s, sampler->wrap_t,
              ctx->dsa->depth_enabled, ctx->dsa->depth_writemask,
              ctx->dsa->alpha_enabled, ctx->dsa->stencil[0].enabled,
              ctx->dsa->stencil[1].enabled, blend->rgb_func,
              blend->rgb_src_factor, blend->rgb_dst_factor, blend->alpha_func,
              blend->alpha_src_factor, blend->alpha_dst_factor,
              blend->colormask, ctx->rasterizer ? ctx->rasterizer->flatshade : 0,
              ctx->rasterizer ? ctx->rasterizer->poly_stipple_enable : 0);
   }
   if (ctx->fs->sampler_mask != BITFIELD_BIT(0) ||
       ctx->fs->texenv != P3D_TEXENV_MODULATE ||
       texture->base.target != PIPE_TEXTURE_2D ||
       texture->base.format != PIPE_FORMAT_R8G8B8A8_UNORM ||
       !texture->upload_texture || !texture->handle || !texture->map ||
       !ph_alpha_route_colour(color->base.format) || !color->handle ||
       (ctx->dsa->depth_enabled &&
        (!zs->handle ||
         !ph_alpha_route_depth(ctx->fb.zsbuf.format))) ||
       sampler->min_img_filter != PIPE_TEX_FILTER_LINEAR ||
       sampler->mag_img_filter != PIPE_TEX_FILTER_LINEAR ||
       !(sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST ||
         sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
         sampler->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR) ||
       !((sampler->wrap_s == PIPE_TEX_WRAP_REPEAT ||
          sampler->wrap_s == PIPE_TEX_WRAP_CLAMP_TO_EDGE) &&
         (sampler->wrap_t == PIPE_TEX_WRAP_REPEAT ||
          sampler->wrap_t == PIPE_TEX_WRAP_CLAMP_TO_EDGE)) ||
       ctx->dsa->alpha_enabled ||
       ctx->dsa->stencil[0].enabled ||
       ctx->dsa->stencil[1].enabled || blend->rgb_func != PIPE_BLEND_ADD ||
       blend->alpha_func != PIPE_BLEND_ADD ||
       blend->rgb_src_factor != PIPE_BLENDFACTOR_SRC_ALPHA ||
       blend->rgb_dst_factor != PIPE_BLENDFACTOR_INV_SRC_ALPHA ||
       blend->alpha_src_factor != PIPE_BLENDFACTOR_SRC_ALPHA ||
       blend->alpha_dst_factor != PIPE_BLENDFACTOR_INV_SRC_ALPHA ||
       blend->colormask != PIPE_MASK_RGBA ||
       (ctx->rasterizer && (ctx->rasterizer->flatshade ||
                            ctx->rasterizer->poly_stipple_enable)))
      return false;

   /* Q3 supplies a base image even when its optional mip-defined bookkeeping
    * remains clear.  The P690 descriptor binds level zero in either case, and
    * so does this fallback. */
   /* Retain the native-image signature through repeated primitives.  Image
    * generation changes even when an upload updates both backing maps and
    * clears the ordinary dirty flags. */
   ph_resource_upload_tiled(texture);

   /* A target without destination alpha has nothing to preserve.  The two
    * hardware passes therefore implement the complete visible source-over
    * result for every RGBA texture accepted above; no application fingerprint
    * or screen-space heuristic is needed.  Keep the image classification
    * below only for a true ARGB target, whose alpha-channel equation P690
    * cannot reproduce from sampled RGB. */
   if (ph_target_no_dest_alpha(color->base.format))
      return true;

   if (ctx->cpu_alpha_hash_resource != texture ||
       ctx->cpu_alpha_hash_handle != texture->handle ||
       ctx->cpu_alpha_hash_generation != texture->content_generation) {
      image = ph_texture_base_hash(texture, true, &alpha_zero, &alpha_nonzero);
      ctx->cpu_alpha_hash_resource = texture;
      ctx->cpu_alpha_hash_handle = texture->handle;
      ctx->cpu_alpha_hash_generation = texture->content_generation;
      ctx->cpu_alpha_hash = image;
      ctx->cpu_alpha_hash_zero = alpha_zero != 0;
      ctx->cpu_alpha_hash_nonzero = alpha_nonzero != 0;
   } else {
      image = ctx->cpu_alpha_hash;
      alpha_zero = ctx->cpu_alpha_hash_zero;
      alpha_nonzero = ctx->cpu_alpha_hash_nonzero;
   }
   if (trace_candidate)
      fprintf(stderr, "parhelia: cpu-alpha candidate image=%#llx a0=%u an=%u state-selected=%u\\n",
              (unsigned long long)image, alpha_zero, alpha_nonzero,
              alpha_zero && alpha_nonzero &&
              (!(ctx->dsa->depth_enabled && ctx->dsa->depth_writemask) ||
               ph_cpu_alpha_blend_depth_write_image(image)));
   /* This shared Q3 atlas is the only texture for which the target capture
    * proves the alpha-loss artifact.  It is further limited to the small HUD
    * primitives below because the atlas is also used by projected geometry. */
   return alpha_zero && alpha_nonzero &&
          (!(ctx->dsa->depth_enabled && ctx->dsa->depth_writemask) ||
           ph_cpu_alpha_blend_depth_write_image(image));
}

/* Q3 reuses this atlas for both its top/bottom HUD digits and projected map
 * detail.  The former has the lost sampled-alpha symptom; software compositing
 * the latter covers the entire frame and is prohibitively slow.  The HUD
 * primitives are small, wholly in the top 64 pixels or bottom strip of Q3's
 * 640x480 virtual screen.  The engine's first top-right glyph lies at
 * y=32..64, so this needs the complete 64-pixel HUD band. */
static bool
ph_rgb_alpha_primitive(struct ph_context *ctx,
                       const struct prim_header *prim, int pos, int col)
{
	const struct parhelia_resource *texture;
   const struct parhelia_resource *color;
   float xmin = FLT_MAX, ymin = FLT_MAX;
   float xmax = -FLT_MAX, ymax = -FLT_MAX;

   if (pos < 0)
      return false;
	texture = ph_resource(ctx->sampler_views[0]->texture);
   color = ph_resource(ctx->fb.cbufs[0].texture);

   for (unsigned i = 0; i < 3; i++) {
      const float *p = prim->v[i]->data[pos];

      xmin = MIN2(xmin, p[0]); xmax = MAX2(xmax, p[0]);
      ymin = MIN2(ymin, p[1]); ymax = MAX2(ymax, p[1]);
   }
	/* ph_use_rgb_alpha_two_pass() has already updated this cached signature.
    * Hashing the whole atlas here happens once per triangle and can keep Q3
    * in its loading screen indefinitely. */
   /* RGB premultiplication commutes with interpolation only when vertex
    * alpha is constant over the primitive.  Q3's observed HUD uses opaque
    * white; also accept any other constant alpha without approximating it. */
   if (col >= 0) {
      float a = prim->v[0]->data[col][3];

      for (unsigned i = 1; i < 3; i++)
         if (fabsf(prim->v[i]->data[col][3] - a) > 0.0001f)
            return false;
   }

   /* ph_use_rgb_alpha_two_pass() established the exact equation for a target
    * without destination alpha and all texture, blend, depth and stencil
    * requirements.  Position and atlas size do not affect correctness on such
    * a framebuffer format. */
   if (ph_target_no_dest_alpha(color->base.format))
      return true;
   /* The live q3dm1 fingerprint is the centered 24x24 reticle.  It lies
    * outside the HUD bands below and is the most visible sampled-alpha
    * failure, so admit that exact image independently of screen position. */
   if (ctx->cpu_alpha_hash == UINT64_C(0xe98fa2a3b0349695))
      return xmax - xmin <= 48.0f && ymax - ymin <= 48.0f;
   /* The live HUD includes a 128x128 atlas drawn as an 80x60 bottom sprite.
    * Keep larger loading/projected atlases out of this narrow route. */
   if (texture->base.width0 > 128 || texture->base.height0 > 128)
      return false;
   return xmax - xmin <= 96.0f && ymax - ymin <= 64.0f &&
          (ymax <= 64.0f || ymin >= 340.0f);
}

static void
ph_trace_alpha_test(struct ph_context *ctx, const struct p3d_state *state,
                    const struct parhelia_resource *color,
                    const struct parhelia_resource *texture,
                    const struct pipe_sampler_state *sampler,
                    bool texture_tiled)
{
   uint64_t sig = 0xcbf29ce484222325ULL;
   uint64_t texture_hash;
   unsigned alpha_zero, alpha_nonzero;
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;

   if (!ph_env("PARHELIA_TRACE_ALPHA_TEST") || !state->alpha_test ||
       !texture)
      return;
   texture_hash = ph_texture_base_hash(texture, texture_tiled, &alpha_zero,
                                       &alpha_nonzero);
   uint32_t values[] = {
      texture->base.target, texture->base.format, texture->base.width0,
      texture->base.height0, texture->base.last_level, texture->mip_defined,
      texture_tiled, state->alpha_func, state->alpha_ref,
      state->depth_test, state->depth_write, state->depth_func,
      color ? color->base.format : UINT32_MAX, state->colour.fmt, state->dither,
      state->blend, ctx->fs ? ctx->fs->sampler_mask : 0,
      ctx->fs ? ctx->fs->texenv : 0,
      sampler ? sampler->min_img_filter : UINT32_MAX,
      sampler ? sampler->mag_img_filter : UINT32_MAX,
      sampler ? sampler->min_mip_filter : UINT32_MAX,
      sampler ? sampler->wrap_s : UINT32_MAX,
      sampler ? sampler->wrap_t : UINT32_MAX,
      blend ? blend->rgb_src_factor : UINT32_MAX,
      blend ? blend->rgb_dst_factor : UINT32_MAX,
      blend ? blend->alpha_src_factor : UINT32_MAX,
      blend ? blend->alpha_dst_factor : UINT32_MAX,
      blend ? blend->colormask : UINT32_MAX,
      (uint32_t)texture_hash, (uint32_t)(texture_hash >> 32),
   };

   for (unsigned i = 0; i < ARRAY_SIZE(values); i++)
      sig = (sig ^ values[i]) * 0x100000001b3ULL;
   for (unsigned i = 0; i < ctx->trace_alpha_test_count; i++)
      if (ctx->trace_alpha_test_state[i] == sig)
         return;
   if (ctx->trace_alpha_test_count == ARRAY_SIZE(ctx->trace_alpha_test_state)) {
      fprintf(stderr, "parhelia: alpha-test trace state limit reached\n");
      return;
   }
   ctx->trace_alpha_test_state[ctx->trace_alpha_test_count++] = sig;
   fprintf(stderr,
           "parhelia: alpha-test state=%#llx tex=%ux%u fmt=%u target=%u levels=%u defined=%#x layout=%u image=%#llx a0=%u an=%u alpha=%u/%u depth=%u/%u/%u cbuf=%u/%#x dither=%u env=%u samplers=%#x filter=%u/%u/%u wrap=%u/%u blend=%u factors=%u,%u/%u,%u mask=%#x\n",
           (unsigned long long)sig, texture->base.width0, texture->base.height0,
           texture->base.format, texture->base.target, texture->base.last_level,
           texture->mip_defined, texture_tiled ? 2 : 0,
           (unsigned long long)texture_hash, alpha_zero, alpha_nonzero,
           state->alpha_func,
           state->alpha_ref, state->depth_test, state->depth_write,
           state->depth_func, color ? color->base.format : UINT32_MAX,
           state->colour.fmt, state->dither, ctx->fs ? ctx->fs->texenv : 0,
           ctx->fs ? ctx->fs->sampler_mask : 0,
           sampler ? sampler->min_img_filter : UINT32_MAX,
           sampler ? sampler->mag_img_filter : UINT32_MAX,
           sampler ? sampler->min_mip_filter : UINT32_MAX,
           sampler ? sampler->wrap_s : UINT32_MAX,
           sampler ? sampler->wrap_t : UINT32_MAX,
           state->blend, blend ? blend->rgb_src_factor : UINT32_MAX,
           blend ? blend->rgb_dst_factor : UINT32_MAX,
           blend ? blend->alpha_src_factor : UINT32_MAX,
           blend ? blend->alpha_dst_factor : UINT32_MAX,
           blend ? blend->colormask : UINT32_MAX);
   if (ctx->triangles) {
      const float *v = ctx->vertices;
      unsigned rgb = state->has_depth ? 3 : 2;
      unsigned st = rgb + 3 + !!state->vertex_alpha;

      fprintf(stderr,
              "parhelia: alpha-test first-triangle dpv=%u "
              "p0=%g,%g,%g rgb0=%g,%g,%g st0=%g,%g "
              "p1=%g,%g,%g rgb1=%g,%g,%g st1=%g,%g "
              "p2=%g,%g,%g rgb2=%g,%g,%g st2=%g,%g\n",
              state->dw_per_vertex,
              v[0], v[1], state->has_depth ? v[2] : 0.0f,
              v[rgb], v[rgb + 1], v[rgb + 2], v[st], v[st + 1],
              v[state->dw_per_vertex], v[state->dw_per_vertex + 1],
              state->has_depth ? v[state->dw_per_vertex + 2] : 0.0f,
              v[state->dw_per_vertex + rgb], v[state->dw_per_vertex + rgb + 1],
              v[state->dw_per_vertex + rgb + 2],
              v[state->dw_per_vertex + st], v[state->dw_per_vertex + st + 1],
              v[2 * state->dw_per_vertex], v[2 * state->dw_per_vertex + 1],
              state->has_depth ? v[2 * state->dw_per_vertex + 2] : 0.0f,
              v[2 * state->dw_per_vertex + rgb],
              v[2 * state->dw_per_vertex + rgb + 1],
              v[2 * state->dw_per_vertex + rgb + 2],
              v[2 * state->dw_per_vertex + st],
              v[2 * state->dw_per_vertex + st + 1]);
   }
}

/* The P690's sampled-alpha hole also affects SRC_ALPHA blending, not only
 * GL alpha test.  This trace identifies exact Q3 candidates for a future
 * constrained software compositor.  It neither changes resource ownership
 * nor emits hardware state. */
static void
ph_trace_blend_alpha(struct ph_context *ctx, const struct p3d_state *state,
                     const struct parhelia_resource *texture,
                     const struct pipe_sampler_state *sampler,
                     bool texture_tiled)
{
   const struct pipe_rt_blend_state *blend =
      ctx->blend ? &ctx->blend->rt[0] : NULL;
   uint64_t sig = 0xcbf29ce484222325ULL;
   uint64_t image;
   unsigned alpha_zero, alpha_nonzero;
   float minx = INFINITY, miny = INFINITY;
   float maxx = -INFINITY, maxy = -INFINITY;
   uint32_t values[21];

   if (!ph_env("PARHELIA_TRACE_BLEND_ALPHA") || !texture || !blend ||
       !blend->blend_enable || !ctx->triangles)
      return;
   image = ph_texture_base_hash(texture, texture_tiled, &alpha_zero,
                                &alpha_nonzero);
   values[0] = texture->base.width0;
   values[1] = texture->base.height0;
   values[2] = texture->base.format;
   values[3] = texture_tiled;
   values[4] = state->has_depth;
   values[5] = state->depth_test;
   values[6] = state->depth_write;
   values[7] = state->vertex_alpha;
   values[8] = state->dw_per_vertex;
   values[9] = ctx->fs ? ctx->fs->texenv : UINT32_MAX;
   values[10] = ctx->fs ? ctx->fs->sampler_mask : 0;
   values[11] = blend->rgb_func;
   values[12] = blend->rgb_src_factor;
   values[13] = blend->rgb_dst_factor;
   values[14] = blend->alpha_func;
   values[15] = blend->alpha_src_factor;
   values[16] = blend->alpha_dst_factor;
   values[17] = sampler ? sampler->min_img_filter : UINT32_MAX;
   values[18] = sampler ? sampler->mag_img_filter : UINT32_MAX;
   values[19] = sampler ? sampler->wrap_s : UINT32_MAX;
   values[20] = sampler ? sampler->wrap_t : UINT32_MAX;
   sig = (sig ^ (uint32_t)image) * 0x100000001b3ULL;
   sig = (sig ^ (uint32_t)(image >> 32)) * 0x100000001b3ULL;
   for (unsigned i = 0; i < ARRAY_SIZE(values); i++)
      sig = (sig ^ values[i]) * 0x100000001b3ULL;
   for (unsigned i = 0; i < ctx->trace_blend_alpha_count; i++)
      if (ctx->trace_blend_alpha_state[i] == sig)
         return;
   if (ctx->trace_blend_alpha_count == ARRAY_SIZE(ctx->trace_blend_alpha_state)) {
      fprintf(stderr, "parhelia: blend-alpha trace state limit reached\n");
      return;
   }
   ctx->trace_blend_alpha_state[ctx->trace_blend_alpha_count++] = sig;
   for (unsigned i = 0; i < ctx->triangles * 3; i++) {
      const float *v = &ctx->vertices[i * state->dw_per_vertex];
      minx = MIN2(minx, v[0]); miny = MIN2(miny, v[1]);
      maxx = MAX2(maxx, v[0]); maxy = MAX2(maxy, v[1]);
   }
   fprintf(stderr,
           "parhelia: blend-alpha state=%#llx tex=%ux%u fmt=%u layout=%u "
           "image=%#llx a0=%u an=%u depth=%u/%u/%u vertex-a=%u dpv=%u "
           "env=%u samplers=%#x blend=%u/%u,%u alpha=%u/%u,%u "
           "filter=%u/%u wrap=%u/%u bbox=%g,%g..%g,%g tris=%u\n",
           (unsigned long long)sig, texture->base.width0,
           texture->base.height0, texture->base.format, texture_tiled ? 2 : 0,
           (unsigned long long)image, alpha_zero, alpha_nonzero,
           state->has_depth, state->depth_test, state->depth_write,
           state->vertex_alpha, state->dw_per_vertex,
           ctx->fs ? ctx->fs->texenv : 0,
           ctx->fs ? ctx->fs->sampler_mask : 0,
           blend->rgb_func, blend->rgb_src_factor, blend->rgb_dst_factor,
           blend->alpha_func, blend->alpha_src_factor, blend->alpha_dst_factor,
           sampler ? sampler->min_img_filter : UINT32_MAX,
           sampler ? sampler->mag_img_filter : UINT32_MAX,
           sampler ? sampler->wrap_s : UINT32_MAX,
           sampler ? sampler->wrap_t : UINT32_MAX,
           minx, miny, maxx, maxy, ctx->triangles);
}

static unsigned
ph_cpu_repeat(int v, unsigned size)
{
   v %= (int)size;
   return v < 0 ? v + size : v;
}

static void
ph_cpu_sample_rgba(const struct parhelia_resource *texture, float s, float t,
                   float out[4])
{
   const uint8_t *base = texture->map;
   float u = s * texture->base.width0 - 0.5f;
   float v = t * texture->base.height0 - 0.5f;
   int x0 = (int)floorf(u), y0 = (int)floorf(v);
   float fx = u - x0, fy = v - y0;

   for (unsigned c = 0; c < 4; c++) {
      const uint8_t *p00 = base + ph_mip_texel_offset(&texture->mip, 0,
            ph_cpu_repeat(x0, texture->base.width0),
            ph_cpu_repeat(y0, texture->base.height0), false);
      const uint8_t *p10 = base + ph_mip_texel_offset(&texture->mip, 0,
            ph_cpu_repeat(x0 + 1, texture->base.width0),
            ph_cpu_repeat(y0, texture->base.height0), false);
      const uint8_t *p01 = base + ph_mip_texel_offset(&texture->mip, 0,
            ph_cpu_repeat(x0, texture->base.width0),
            ph_cpu_repeat(y0 + 1, texture->base.height0), false);
      const uint8_t *p11 = base + ph_mip_texel_offset(&texture->mip, 0,
            ph_cpu_repeat(x0 + 1, texture->base.width0),
            ph_cpu_repeat(y0 + 1, texture->base.height0), false);
      float a = p00[c] + (p10[c] - p00[c]) * fx;
      float b = p01[c] + (p11[c] - p01[c]) * fx;
      out[c] = a + (b - a) * fy;
   }
}

static unsigned
ph_cpu_texture_coord(int v, unsigned size, enum pipe_tex_wrap wrap)
{
   if (wrap == PIPE_TEX_WRAP_REPEAT)
      return ph_cpu_repeat(v, size);
   return CLAMP(v, 0, (int)size - 1);
}

/* The alpha-over atlas trace admits only REPEAT and CLAMP_TO_EDGE.  Both use
 * the ordinary normalized bilinear footprint; clamp pins the two taps to the
 * edge instead of synthesizing a border colour. */
static void
ph_cpu_sample_rgba_wrapped(const struct parhelia_resource *texture,
                           const struct pipe_sampler_state *sampler,
                           float s, float t, float out[4])
{
   const uint8_t *base = texture->map;
   float u = s * texture->base.width0 - 0.5f;
   float v = t * texture->base.height0 - 0.5f;
   int x0 = (int)floorf(u), y0 = (int)floorf(v);
   float fx = u - x0, fy = v - y0;
   unsigned x00 = ph_cpu_texture_coord(x0, texture->base.width0,
                                       sampler->wrap_s);
   unsigned x10 = ph_cpu_texture_coord(x0 + 1, texture->base.width0,
                                       sampler->wrap_s);
   unsigned y00 = ph_cpu_texture_coord(y0, texture->base.height0,
                                       sampler->wrap_t);
   unsigned y01 = ph_cpu_texture_coord(y0 + 1, texture->base.height0,
                                       sampler->wrap_t);
   const uint8_t *p00 = base + ph_mip_texel_offset(&texture->mip, 0, x00, y00,
                                                   false);
   const uint8_t *p10 = base + ph_mip_texel_offset(&texture->mip, 0, x10, y00,
                                                   false);
   const uint8_t *p01 = base + ph_mip_texel_offset(&texture->mip, 0, x00, y01,
                                                   false);
   const uint8_t *p11 = base + ph_mip_texel_offset(&texture->mip, 0, x10, y01,
                                                   false);

   for (unsigned c = 0; c < 4; c++) {
      float a = p00[c] + (p10[c] - p00[c]) * fx;
      float b = p01[c] + (p11[c] - p01[c]) * fx;
      out[c] = a + (b - a) * fy;
   }
}

static float
ph_cpu_edge(const float *a, const float *b, float x, float y)
{
   return (x - a[0]) * (b[1] - a[1]) -
          (y - a[1]) * (b[0] - a[0]);
}

static bool
ph_cpu_top_left(const float *a, const float *b)
{
   return a[1] < b[1] || (a[1] == b[1] && a[0] > b[0]);
}

static bool
ph_cpu_edge_inside(float edge, bool top_left)
{
   return edge > 0.0f || (edge == 0.0f && top_left);
}

/* CPU alpha fallbacks must not write the tiled VRAM aperture directly.  The
 * scanout/3D engine can retain tiled writes beyond a CPU store, producing
 * progressive frame corruption.  Submission resolves the render surfaces to
 * their synchronized linear shadows first; all CPU raster access goes there.
 * A later native draw performs the normal linear-to-tiled transition. */
static inline uint8_t *
ph_cpu_linear_pixel(struct parhelia_resource *r, int x, int y)
{
   return (uint8_t *)r->linear_map + (size_t)y * r->linear_stride +
          (size_t)x * 4;
}

/* The Z24 code a CPU route stores for window depth @z, the way the Z unit
 * converts it on a z_grid family (trunc(z * 2^24)) or as before. */
static uint32_t
ph_cpu_depth_code(float z, bool z_grid)
{
   return z_grid ? MIN2((uint32_t)(z * 16777216.0f), 0x00ffffffu) :
                   (uint32_t)lrintf(z * 16777215.0f) & 0x00ffffffu;
}

static void
ph_cpu_cutout_triangle(const float *in, struct parhelia_resource *color,
                       struct parhelia_resource *depth,
                       const struct parhelia_resource *texture,
                       unsigned width, unsigned height, bool z_grid)
{
   const float *v[3] = {in, in + 10, in + 20};
   float area = ph_cpu_edge(v[0], v[1], v[2][0], v[2][1]);
   int xmin, ymin, xmax, ymax;

   if (fabsf(area) < 1e-12f)
      return;
   if (area < 0.0f) {
      const float *tmp = v[1];
      v[1] = v[2];
      v[2] = tmp;
      area = -area;
   }
   xmin = MAX2(0, (int)floorf(MIN3(v[0][0], v[1][0], v[2][0])));
   ymin = MAX2(0, (int)floorf(MIN3(v[0][1], v[1][1], v[2][1])));
   xmax = MIN2((int)width - 1,
               (int)ceilf(MAX3(v[0][0], v[1][0], v[2][0])) - 1);
   ymax = MIN2((int)height - 1,
               (int)ceilf(MAX3(v[0][1], v[1][1], v[2][1])) - 1);
   for (int y = ymin; y <= ymax; y++) {
      for (int x = xmin; x <= xmax; x++) {
         float e0 = ph_cpu_edge(v[1], v[2], x + 0.5f, y + 0.5f);
         float e1 = ph_cpu_edge(v[2], v[0], x + 0.5f, y + 0.5f);
         float e2 = ph_cpu_edge(v[0], v[1], x + 0.5f, y + 0.5f);
         float l0, l1, l2, denom, s, t, r, g, b, a, z, texel[4];
         uint32_t old_z, new_z;
         uint8_t *depth_pixel;
         uint8_t *pixel;

         if (!ph_cpu_edge_inside(e0, ph_cpu_top_left(v[1], v[2])) ||
             !ph_cpu_edge_inside(e1, ph_cpu_top_left(v[2], v[0])) ||
             !ph_cpu_edge_inside(e2, ph_cpu_top_left(v[0], v[1])))
            continue;
         l0 = e0 / area;
         l1 = e1 / area;
         l2 = e2 / area;
         denom = l0 * v[0][3] + l1 * v[1][3] + l2 * v[2][3];
         if (denom <= 0.0f)
            continue;
#define PH_CPU_INTERP(i) ((l0 * v[0][i] * v[0][3] + \
                           l1 * v[1][i] * v[1][3] + \
                           l2 * v[2][i] * v[2][3]) / denom)
         r = PH_CPU_INTERP(4); g = PH_CPU_INTERP(5); b = PH_CPU_INTERP(6);
         a = PH_CPU_INTERP(7); s = PH_CPU_INTERP(8); t = PH_CPU_INTERP(9);
#undef PH_CPU_INTERP
         ph_cpu_sample_rgba(texture, s, t, texel);
         a = texel[3] * CLAMP(a, 0.0f, 1.0f);
         if (a < 128.0f)
            continue;
         z = CLAMP(l0 * v[0][2] + l1 * v[1][2] + l2 * v[2][2], 0.0f, 1.0f);
         new_z = ph_cpu_depth_code(z, z_grid);
         depth_pixel = ph_cpu_linear_pixel(depth, x, y);
         memcpy(&old_z, depth_pixel, sizeof(old_z));
         if (new_z > (old_z & 0x00ffffffu))
            continue;
         old_z = (old_z & 0xff000000u) | new_z;
         memcpy(depth_pixel, &old_z, sizeof(old_z));
         pixel = ph_cpu_linear_pixel(color, x, y);
         pixel[0] = CLAMP((int)lrintf(texel[0] * CLAMP(b, 0.0f, 1.0f)), 0, 255);
         pixel[1] = CLAMP((int)lrintf(texel[1] * CLAMP(g, 0.0f, 1.0f)), 0, 255);
         pixel[2] = CLAMP((int)lrintf(texel[2] * CLAMP(r, 0.0f, 1.0f)), 0, 255);
         pixel[3] = CLAMP((int)lrintf(a), 0, 255);
      }
   }
}

static bool
ph_cpu_depth_pass(enum pipe_compare_func func, uint32_t incoming, uint32_t old)
{
   switch (func) {
   case PIPE_FUNC_NEVER: return false;
   case PIPE_FUNC_LESS: return incoming < old;
   case PIPE_FUNC_EQUAL: return incoming == old;
   case PIPE_FUNC_LEQUAL: return incoming <= old;
   case PIPE_FUNC_GREATER: return incoming > old;
   case PIPE_FUNC_NOTEQUAL: return incoming != old;
   case PIPE_FUNC_GEQUAL: return incoming >= old;
   case PIPE_FUNC_ALWAYS: return true;
   default: return false;
   }
}

static void
ph_cpu_alpha_blend_triangle(const float *in, struct parhelia_resource *color,
                            struct parhelia_resource *depth,
                            const struct parhelia_resource *texture,
                            const struct pipe_sampler_state *sampler,
                            const struct pipe_depth_stencil_alpha_state *dsa,
                            unsigned width, unsigned height, bool z_grid)
{
   const float *v[3] = {in, in + 10, in + 20};
   float area = ph_cpu_edge(v[0], v[1], v[2][0], v[2][1]);
   int xmin, ymin, xmax, ymax;

   if (fabsf(area) < 1e-12f)
      return;
   if (area < 0.0f) {
      const float *tmp = v[1];
      v[1] = v[2]; v[2] = tmp;
      area = -area;
   }
   xmin = MAX2(0, (int)floorf(MIN3(v[0][0], v[1][0], v[2][0])));
   ymin = MAX2(0, (int)floorf(MIN3(v[0][1], v[1][1], v[2][1])));
   xmax = MIN2((int)width - 1,
               (int)ceilf(MAX3(v[0][0], v[1][0], v[2][0])) - 1);
   ymax = MIN2((int)height - 1,
               (int)ceilf(MAX3(v[0][1], v[1][1], v[2][1])) - 1);
   for (int y = ymin; y <= ymax; y++) {
      for (int x = xmin; x <= xmax; x++) {
         float e0 = ph_cpu_edge(v[1], v[2], x + 0.5f, y + 0.5f);
         float e1 = ph_cpu_edge(v[2], v[0], x + 0.5f, y + 0.5f);
         float e2 = ph_cpu_edge(v[0], v[1], x + 0.5f, y + 0.5f);
         float l0, l1, l2, denom, s, t, r, g, b, a, z, texel[4];
         uint32_t old_z = 0, new_z = 0;
         uint8_t *pixel;

         if (!ph_cpu_edge_inside(e0, ph_cpu_top_left(v[1], v[2])) ||
             !ph_cpu_edge_inside(e1, ph_cpu_top_left(v[2], v[0])) ||
             !ph_cpu_edge_inside(e2, ph_cpu_top_left(v[0], v[1])))
            continue;
         l0 = e0 / area; l1 = e1 / area; l2 = e2 / area;
         denom = l0 * v[0][3] + l1 * v[1][3] + l2 * v[2][3];
         if (denom <= 0.0f)
            continue;
#define PH_CPU_BLEND_INTERP(i) ((l0 * v[0][i] * v[0][3] + \
                                 l1 * v[1][i] * v[1][3] + \
                                 l2 * v[2][i] * v[2][3]) / denom)
         r = PH_CPU_BLEND_INTERP(4); g = PH_CPU_BLEND_INTERP(5);
         b = PH_CPU_BLEND_INTERP(6); a = PH_CPU_BLEND_INTERP(7);
         s = PH_CPU_BLEND_INTERP(8); t = PH_CPU_BLEND_INTERP(9);
#undef PH_CPU_BLEND_INTERP
         if (dsa->depth_enabled) {
            uint8_t *depth_pixel;
            z = CLAMP(l0 * v[0][2] + l1 * v[1][2] + l2 * v[2][2], 0.0f, 1.0f);
            new_z = ph_cpu_depth_code(z, z_grid);
            depth_pixel = ph_cpu_linear_pixel(depth, x, y);
            memcpy(&old_z, depth_pixel, sizeof(old_z));
            if (!ph_cpu_depth_pass(dsa->depth_func, new_z, old_z & 0x00ffffffu))
               continue;
            if (dsa->depth_writemask) {
               uint32_t updated = (old_z & 0xff000000u) | new_z;
               memcpy(depth_pixel, &updated, sizeof(updated));
            }
         }
         ph_cpu_sample_rgba_wrapped(texture, sampler, s, t, texel);
         r = texel[0] * CLAMP(r, 0.0f, 1.0f);
         g = texel[1] * CLAMP(g, 0.0f, 1.0f);
         b = texel[2] * CLAMP(b, 0.0f, 1.0f);
         a = texel[3] * CLAMP(a, 0.0f, 1.0f) / 255.0f;
         pixel = ph_cpu_linear_pixel(color, x, y);
         pixel[0] = CLAMP((int)lrintf(b * a + pixel[0] * (1.0f - a)), 0, 255);
         pixel[1] = CLAMP((int)lrintf(g * a + pixel[1] * (1.0f - a)), 0, 255);
         pixel[2] = CLAMP((int)lrintf(r * a + pixel[2] * (1.0f - a)), 0, 255);
         pixel[3] = CLAMP((int)lrintf(255.0f * a + pixel[3] * (1.0f - a)),
                          0, 255);
      }
   }
}

static void
ph_cpu_alpha_blend(struct ph_context *ctx, struct parhelia_resource *color,
                   struct parhelia_resource *depth,
                   const struct parhelia_resource *texture,
                   const struct pipe_sampler_state *sampler)
{
   if (ph_env("PARHELIA_TRACE_BLEND_ALPHA")) {
      float xmin = FLT_MAX, ymin = FLT_MAX;
      float xmax = -FLT_MAX, ymax = -FLT_MAX;
      float oow_min = FLT_MAX, oow_max = -FLT_MAX;

      for (unsigned i = 0; i < ctx->triangles * 3; i++) {
         const float *v = &ctx->vertices[i * 10];

         xmin = MIN2(xmin, v[0]); xmax = MAX2(xmax, v[0]);
         ymin = MIN2(ymin, v[1]); ymax = MAX2(ymax, v[1]);
         oow_min = MIN2(oow_min, v[3]); oow_max = MAX2(oow_max, v[3]);
      }
      fprintf(stderr,
              "parhelia: CPU alpha blend tex=%ux%u tris=%u image=%#llx bbox=%.1f,%.1f-%.1f,%.1f oow=%.6f..%.6f\\n",
              texture->base.width0, texture->base.height0, ctx->triangles,
              (unsigned long long)ph_texture_base_hash(texture, true,
                                                        NULL, NULL),
              xmin, ymin, xmax, ymax, oow_min, oow_max);
   }
   for (unsigned i = 0; i < ctx->triangles; i++)
      ph_cpu_alpha_blend_triangle(&ctx->vertices[i * 30], color, depth, texture,
                                  sampler, ctx->dsa, ctx->fb.width, ctx->fb.height,
                                  ctx->z_grid);
   __sync_synchronize();
   color->tiled_dirty = false;
   color->linear_dirty = true;
   color->mip_defined |= BITFIELD_BIT(0);
   color->content_generation++;
   if (ctx->dsa->depth_enabled && ctx->dsa->depth_writemask) {
      depth->tiled_dirty = false, depth->linear_dirty = true,
      depth->content_generation++;
      depth->mip_defined |= BITFIELD_BIT(0);
   }
}

static void
ph_cpu_alpha_cutout(struct ph_context *ctx, struct parhelia_resource *color,
                    struct parhelia_resource *depth,
                    const struct parhelia_resource *texture)
{
   for (unsigned i = 0; i < ctx->triangles; i++)
      ph_cpu_cutout_triangle(&ctx->vertices[i * 30], color, depth, texture,
                             ctx->fb.width, ctx->fb.height, ctx->z_grid);
   __sync_synchronize();
   color->tiled_dirty = false;
   color->linear_dirty = true;
   color->mip_defined |= BITFIELD_BIT(0);
   color->content_generation++;
   depth->tiled_dirty = false;
   depth->linear_dirty = true;
   depth->mip_defined |= BITFIELD_BIT(0);
   depth->content_generation++;
}

static void
ph_pack_rgb_alpha_pass(struct ph_context *ctx, bool depth, bool attenuation,
                       float scale_s, float scale_t)
{
   float *dst = ctx->packed_vertices;

   for (unsigned i = 0; i < ctx->triangles * 3; i++) {
      const float *src = &ctx->vertices[i * 10];
      float a = CLAMP(src[7], 0.0f, 1.0f);

      *dst++ = src[0];
      *dst++ = src[1];
      if (depth)
         *dst++ = src[2];
      if (attenuation) {
         *dst++ = a;
         *dst++ = a;
         *dst++ = a;
      } else {
         *dst++ = src[4] * a;
         *dst++ = src[5] * a;
         *dst++ = src[6] * a;
      }
      *dst++ = src[8] * scale_s;
      *dst++ = src[9] * scale_t;
   }
}

/* Build the state batch for a run of @primitives primitives of @state->prim,
 * grouping them into as few draw packets as the tuned packet size allows.
 * Either texture handle may be zero. */
static int ph_batch_primitives(struct ph_context *ctx, struct parhelia_batch *batch,
                               struct p3d_state *state, uint32_t colour_handle,
                               uint32_t depth_handle, uint32_t texture0_handle,
                               uint32_t texture1_handle, unsigned primitives);

/* After a hardware-transform batch, a software-clipped textured draw rendered
 * nothing (Q3's view weapon near walls and looking down;
 * parhelia-gl-depthrange-test case 15).  Measured on the 0528: an untextured
 * passthrough draw of two triangles in ONE draw packet ahead of it makes the
 * frame identical to the software-transform reference; the same triangles as
 * one-primitive packets left half the weapon hidden, whatever their colour,
 * depth, size or position.  The command streams were otherwise identical.
 * So queue that draw, masked, with the depth target bound under the current
 * test and writes off, before the next batch that does not transform. */
static int
ph_restore_after_transform(struct ph_context *ctx,
                           struct parhelia_resource *color,
                           struct parhelia_resource *depth)
{
   static const float with_z[36] = {
      0, 0, 0.5f, 1, 1, 1,  1, 0, 0.5f, 1, 1, 1,  0, 1, 0.5f, 1, 1, 1,
      0, 0, 0.5f, 1, 1, 1,  1, 0, 0.5f, 1, 1, 1,  0, 1, 0.5f, 1, 1, 1,
   };
   static const float without_z[30] = {
      0, 0, 1, 1, 1,  1, 0, 1, 1, 1,  0, 1, 1, 1, 1,
      0, 0, 1, 1, 1,  1, 0, 1, 1, 1,  0, 1, 1, 1, 1,
   };
   struct parhelia_batch restore;
   struct p3d_state st = {
      .colour = {.pitch = color->stride, .fmt = color->surf_fmt},
      .screen_w = ctx->fb.width, .screen_h = ctx->fb.height,
      .prim = P3D_TRIANGLES, .warp_entry = ctx->warp_passthrough,
      .dw_per_vertex = 5, .mask_explicit = true, .dither = 1,
   };
   int ret;

   if (depth) {
      st.depth.pitch = depth->stride;
      st.depth.fmt = depth->surf_fmt;
      st.depth_slot = 1;
      st.has_depth = 1;
      st.depth_test = ctx->dsa && ctx->dsa->depth_enabled;
      st.depth_func = ctx->dsa ? ph_compare(ctx->dsa->depth_func) : 0;
      st.dw_per_vertex = 6;
   }
   ret = ph_batch_primitives(ctx, &restore, &st, color->handle,
                             depth ? depth->handle : 0, 0, 0, 2);
   if (!ret)
      ret = ph_submit_batch(ctx, &restore, depth ? with_z : without_z,
                            st.dw_per_vertex * 6, color, depth, NULL, NULL);
   return ret;
}

/* The textured programs for a stream without a colour (a REPLACE draw
 * without z, a cube or volume map's three coordinates): the kernel's
 * textured and replace programs read v5, which such a stream never fetches,
 * so they took the previous draw's colour - from any client - and a colour
 * with a channel above its alpha made the setup unit reject the draw
 * (incomplete-cubemap after gl-2.0-vertexattribpointer, measured).  These
 * write o1 = (1,1,1,1) instead and read only fetched inputs; @two_sets adds
 * coordinate set 1. */
static uint32_t
ph_no_colour_program(struct ph_context *ctx, bool two_sets)
{
   static const uint32_t code[] = {
      0x00083e00, 0x00440000, 0xb89a2249,   /* add o0, v0, v0.0000 */
      0x00087ebf, 0x00440200, 0xb8adb649,   /* add o1, v0.1111, v0.0000 | mov o11, v0 */
      0x0078023f, 0x00440200, 0xb01a21a2,   /* mov o3, v7 */
      0x0088024f, 0x00440200, 0xb01a21a2,   /* mov o4, v8 */
      0x00000200, 0x00040000, 0x14c00000,   /* end */
      0x00080200, 0x00440000, 0x301a21a2,   /* wra */
   };
   uint32_t words[18];
   struct drm_parhelia_warp_program program = {
      .code = (uintptr_t)words, .count = two_sets ? 6 : 5,
   };

   if (ctx->warp_no_colour[two_sets] || !ctx->warp_program_instructions)
      return ctx->warp_no_colour[two_sets];
   memcpy(words, code, 9 * sizeof(uint32_t));
   if (two_sets) {
      memcpy(words + 9, code + 9, 9 * sizeof(uint32_t));
   } else {
      memcpy(words + 9, code + 12, 6 * sizeof(uint32_t));
   }
   /* The upload drains the engine: submit the aggregated work first. */
   if (ph_drain_batch(ctx) ||
       ioctl(ph_screen(ctx->base.screen)->fd, DRM_IOCTL_PARHELIA_WARP_PROGRAM,
             &program))
      return 0;
   ctx->warp_no_colour[two_sets] = (uint32_t)program.entry;
   return ctx->warp_no_colour[two_sets];
}

static int
ph_batch_primitives_units(struct ph_context *ctx, struct parhelia_batch *batch,
                          struct p3d_state *state, uint32_t colour_handle,
                          uint32_t depth_handle,
                          const uint32_t handles[P3D_MAX_TEXTURE_UNITS],
                          unsigned primitives)
{
   const unsigned per_prim = p3d_vertices_per_primitive(state->prim);
   /* Only triangle lists are proven to carry a run of primitives in one
    * packet.  A packet with eight lines locks the engine up on the first
    * draw of parhelia-gl-prim-test, so points and lines keep one packet
    * per primitive. */
   unsigned group = state->prim == P3D_TRIANGLES ? ctx->prims_per_packet : 1;

   if (state->indexed_vertices)
      group = ph_screen(ctx->base.screen)->indexed_tris;

   if (!per_prim || !primitives)
      return -EINVAL;
   if (state->tex && !state->transform &&
       ph_lx_family(ph_screen(ctx->base.screen)->chip_id) &&
       !p3d_state_fetches_colour(state)) {
      const uint32_t entry = ph_no_colour_program(ctx, state->tex1 != NULL);

      if (entry)
         state->warp_entry = entry;
   }
   if (group > PH_BATCH_MAX_PACKET_VERTICES / per_prim)
      group = PH_BATCH_MAX_PACKET_VERTICES / per_prim;
   if (group > primitives)
      group = primitives;
   state->vertices_per_packet = group * per_prim;
   if (ctx->stats)
      ctx->stat.draw_packets += (primitives + group - 1) / group;
   /* Only a grouped passthrough triangle packet ends the condition. */
   if (state->transform)
      ctx->restore_after_transform = true;
   else if (state->warp_entry == ctx->warp_passthrough &&
            state->prim == P3D_TRIANGLES && group >= 2)
      ctx->restore_after_transform = false;
   {
      const int ret =
         parhelia_batch_state_run_units_cached(ctx->state_cache, batch,
                                               colour_handle, depth_handle,
                                               handles, state,
                                               primitives * per_prim);

      ctx->batch_calls = ret ? -1 :
                         parhelia_state_cache_last_calls(ctx->state_cache,
                                                         &ctx->batch_stages);
      return ret;
   }
}

static int
ph_batch_primitives(struct ph_context *ctx, struct parhelia_batch *batch,
                    struct p3d_state *state, uint32_t colour_handle,
                    uint32_t depth_handle, uint32_t texture0_handle,
                    uint32_t texture1_handle, unsigned primitives)
{
   return ph_batch_primitives_units(ctx, batch, state, colour_handle,
      depth_handle, (const uint32_t[P3D_MAX_TEXTURE_UNITS]){
         texture0_handle, texture1_handle }, primitives);
}

static int
ph_submit_rgb_alpha_two_pass(struct ph_context *ctx,
                             struct parhelia_resource *color,
                             struct parhelia_resource *depth,
                             struct parhelia_resource *texture,
                             const struct pipe_sampler_state *sampler,
                             const struct p3d_texture *texture_state,
                             const struct p3d_state *base_state)
{
   struct parhelia_batch attenuation_batch, additive_batch;
   struct p3d_texture companion_tex = *texture_state;
   struct p3d_state attenuation = *base_state, additive;
   const unsigned dpv = depth ? 8 : 7;
   float scale_s, scale_t;
   int ret;

   ret = ph_prepare_alpha_companions(ctx, texture, sampler, false);
   if (ret)
      return ret;
   scale_s = ph_alpha_companion_scale(texture->base.width0,
                                      texture->alpha_companion_width);
   scale_t = ph_alpha_companion_scale(texture->base.height0,
                                      texture->alpha_companion_height);
   companion_tex.width = texture->alpha_companion_width;
   companion_tex.height = texture->alpha_companion_height;
   companion_tex.fmt = 0x32;
   companion_tex.texelcode = 2;
   companion_tex.mip_filter = P3D_MIP_NONE;
   companion_tex.max_level = 0;
   companion_tex.env = P3D_TEXENV_MODULATE;

   attenuation.tex = &companion_tex;
   attenuation.tex1 = NULL;
   attenuation.warp_entry = ctx->warp_textured;
   attenuation.dw_per_vertex = dpv;
   attenuation.vertex_alpha = false;
   attenuation.depth_vertex_alpha = false;
   attenuation.depth_vertex_alpha_add = false;
   attenuation.alpha_test = false;
   attenuation.blend = true;
   attenuation.blend_src = P3D_BF_ZERO;
   attenuation.blend_dst = P3D_BF_ONE_MINUS_SRC_COLOR;
   attenuation.depth_write = false;
   /* The P690 cannot derive the GL destination-alpha equation from sampled
    * RGB.  Q3 never consumes framebuffer alpha, so preserve it while updating
    * the exact source-over RGB channels. */
   attenuation.mask_a = false;
   attenuation.mask_r = attenuation.mask_g = attenuation.mask_b = true;
   additive = attenuation;
   additive.blend_src = P3D_BF_ONE;
   additive.blend_dst = P3D_BF_ONE;
   additive.depth_write = base_state->depth_write;

   ret = ph_batch_primitives(ctx, &attenuation_batch, &attenuation,
                             color->handle, depth ? depth->handle : 0,
                             texture->alpha_rgb_handle, 0, ctx->triangles);
   if (!ret)
      ret = ph_batch_primitives(ctx, &additive_batch, &additive, color->handle,
                                depth ? depth->handle : 0,
                                texture->premul_rgb_handle, 0,
                                ctx->triangles);
   if (ret)
      return ret;

   ph_pack_rgb_alpha_pass(ctx, depth != NULL, true, scale_s, scale_t);
   ret = ph_submit_batch(ctx, &attenuation_batch, ctx->packed_vertices,
                         ctx->triangles * 3 * dpv,
                         color, depth, texture, NULL);
   if (ret)
      return ret;
   ph_pack_rgb_alpha_pass(ctx, depth != NULL, false, scale_s, scale_t);
   ret = ph_submit_batch(ctx, &additive_batch, ctx->packed_vertices,
                         ctx->triangles * 3 * dpv,
                         color, depth, texture, NULL);
   if (!ret && ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: GPU RGB-alpha two-pass triangles=%u texture=%u companions=%u/%u depth=%u/%u\n",
              ctx->triangles, texture->handle, texture->alpha_rgb_handle,
              texture->premul_rgb_handle, !!depth,
              base_state->depth_write);
   return ret;
}

/* Unit points and lines use the ordinary transformed-vertex path, carrying
 * one or two vertices per relocated DRAW packet.  Keep consecutive compatible
 * callbacks in one state batch: rebuilding a complete state stream for each
 * primitive is measurable even when the kernel submission is aggregated. */
static void
ph_submit_simple_primitives(struct ph_context *ctx, enum p3d_prim prim)
{
   struct parhelia_batch *batch = &ctx->build_batch;
   struct parhelia_resource *color, *depth = NULL;
   struct p3d_state state = {0};
   unsigned vertices_per_primitive = p3d_vertices_per_primitive(prim);
   unsigned count = prim == P3D_POINTS ? ctx->points : ctx->lines;
   int ret;

   if (!count || !vertices_per_primitive || ctx->submit_error)
      goto out;
   if (!ctx->fb.nr_cbufs || !ctx->fb.cbufs[0].texture) {
      PH_REFUSE("a draw without a colour buffer is dropped\n");
      goto out;
   }

   color = ph_resource(ctx->fb.cbufs[0].texture);
   if (ctx->fb.zsbuf.texture)
      depth = ph_resource(ctx->fb.zsbuf.texture);
   if (!color->handle || (depth && !depth->handle) || color->upload_texture ||
       (depth && depth->upload_texture)) {
      PH_REFUSE("a point or line draw into sampler-only storage is "
                "dropped\n");
      goto out;
   }
   if (ctx->aggregate_batches && color->linear_dirty && ph_drain_batch(ctx))
      goto out;
   ph_resource_upload_tiled(color);

   state.colour.pitch = color->stride;
   state.colour.fmt = color->surf_fmt;
   state.screen_w = ctx->fb.width;
   state.screen_h = ctx->fb.height;
   state.prim = prim;
   state.warp_entry = ctx->warp_passthrough;
   /* Wide points: the sized program states the extent and 0x1130 carries
    * class 3.  The WARP class stays at the primitive's own, which the state
    * emitter handles. */
   const float point_const[4] = { ctx->point_radius, ctx->point_radius,
                                  ctx->point_radius, ctx->point_radius };
   if (prim == P3D_POINTS && ctx->point_radius > 0.0f &&
       ctx->warp_sized_point) {
      state.warp_entry = ctx->warp_sized_point;
      state.point_class = 3;
      state.const_loader_entry = ctx->warp_constant_loader;
      state.vs_const = point_const;
      state.vs_const_vec4s = 1;
      state.const_mask = 1;
   }
   state.dither = !ctx->blend || ctx->blend->dither;
   state.flat = ctx->rasterizer && ctx->rasterizer->flatshade;
   state.has_depth = depth != NULL;
   state.dw_per_vertex = depth ? 6 : 5;
   if (depth) {
      state.depth.pitch = depth->stride;
      state.depth.fmt = depth->surf_fmt;
      state.depth_slot = 1;
   }
   ph_apply_fragment_state(ctx, &state, depth != NULL);
   /* The fragment program applies to lines and points as to triangles: a
    * constant glColor reaches it as a uniform, not as the vertex colour. */
   ctx->comb_refused = false;
   state.comb = ph_fs_untextured_program(ctx) ?
                ph_compiled_combiner(ctx, false) : NULL;
   if (ctx->comb_refused) {
      PH_REFUSE("a batch whose fragment constants no combiner variant can hold is dropped\n");
      goto out;
   }

   ret = ph_batch_primitives(ctx, batch, &state, color->handle,
                             depth ? depth->handle : 0, 0, 0, count);
   if (!ret)
      ret = ph_submit_batch(ctx, batch, ctx->vertices,
                            count * vertices_per_primitive *
                               state.dw_per_vertex,
                            color, depth, NULL, NULL);
   if (ret) {
      ctx->submit_error = ret;
      goto out;
   }
   color->tiled_dirty = true;
   color->linear_dirty = false;
   color->mip_defined |= BITFIELD_BIT(0);
   color->content_generation++;
   if (depth) {
      depth->tiled_dirty = true;
      depth->content_generation++;
      depth->mip_defined |= BITFIELD_BIT(0);
   }
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: submit %s=%u color=%u depth=%u ret=0\n",
              prim == P3D_POINTS ? "points" : "lines", count,
              color->handle, depth ? depth->handle : 0);
out:
   if (prim == P3D_POINTS) {
      ctx->points = 0;
      ctx->point_radius = 0.0f;
   } else {
      ctx->lines = 0;
   }
   ph_stage_at_ring_tail(ctx);
}

/* The pass-through for an RGBA colour: warp_textured without its o3 move
 * (parhelia-p690-warp-isa-re.md 5.2).  The kernel pass-through's
 * "mul T2, v5, r1.0000" makes T2 follow the colour, and with a real alpha
 * every triangle whose vertices all have a colour channel above alpha (or
 * alpha 0) is rejected (measured, --colour-alpha-probe COLOUR_ALPHA_FLAT).
 * T2 keeps the value ph_prime_warp() left, as for the textured programs. */
static bool
ph_rgba_program(struct ph_context *ctx)
{
   static const uint32_t code[] = {
      0x00083e00, 0x00440000, 0xb89a2249,
      0x08087ebf, 0x00440215, 0xb89a2249,
      0x00000200, 0x00040000, 0x14c00000,
      0x00080200, 0x00440000, 0x301a21a2,
   };
   struct drm_parhelia_warp_program program = {
      .code = (uintptr_t)code, .count = 4,
   };

   if (ctx->warp_rgba)
      return true;
   if (!ctx->warp_program_instructions ||
       ioctl(ph_screen(ctx->base.screen)->fd, DRM_IOCTL_PARHELIA_WARP_PROGRAM,
             &program))
      return false;
   ctx->warp_rgba = (uint32_t)program.entry;
   return true;
}

/* The kernel's textured pass-through grown to four coordinate sets, the form
 * the 0528's three- and four-unit draws were measured with.  Uploaded once
 * per context; the upload drains the engine. */
static bool
ph_four_set_program(struct ph_context *ctx)
{
   const struct p3d_warp_shape shape = {
      .write_t2 = 1, .clip_copy = 1, .texcoords = 4,
   };
   uint32_t code[32 * 3];
   struct drm_parhelia_warp_program program = {
      .code = (uintptr_t)code,
   };

   if (ctx->warp_four_sets)
      return true;
   program.count = p3d_warp_build(&shape, code, 32);
   if (!program.count || !ctx->warp_program_instructions ||
       ioctl(ph_screen(ctx->base.screen)->fd, DRM_IOCTL_PARHELIA_WARP_PROGRAM,
             &program))
      return false;
   ctx->warp_four_sets = (uint32_t)program.entry;
   return true;
}

/* Units 2 and 3 of an original-family draw: the program samples GL units
 * 0..n-1 one to one, n of 3 or 4, each with a bound view. */
static unsigned
ph_extra_units(const struct ph_context *ctx)
{
   const struct ph_fs *fs = ctx->fs;
   const unsigned driven = ph_units_driven(ph_screen(ctx->base.screen));
   unsigned n;

   if (!fs || !(fs->sampler_mask & ~3u) || fs->unit_remap || driven < 3)
      return 0;
   n = util_last_bit(fs->sampler_mask);
   if (n > driven || fs->sampler_mask != BITFIELD_MASK(n) ||
       ctx->num_sampler_views < n)
      return 0;
   for (unsigned u = 0; u < n; u++)
      if (!ctx->sampler_views[u] || !ctx->sampler_views[u]->texture)
         return 0;
   return n - 2;
}

/* The pass-through with one move that forwards the second colour attribute to
 * output 2, so the combiner sees it as v1.  Uploaded once per context, on the
 * first draw whose fragment program reads it. */
static bool
ph_colour1_program(struct ph_context *ctx)
{
   static const uint32_t head[] = {
      0x00083e00, 0x00440000, 0xb89a2249,
      0x0800bebf, 0x00440215, 0xba9a2249,
      0x08087e00, 0x00440015, 0xb89a2249,
   };
   /* The LX class without "mul T2, v5, r1": with a real alpha in the colour
    * set, T2 = colour rejects every triangle whose corners all have a
    * channel above alpha, as for the RGBA stream (--colour-alpha-probe).
    * add o1, v5 | mov o11, v0, the RGBA pass-through's own move. */
   static const uint32_t head_lx[] = {
      0x00083e00, 0x00440000, 0xb89a2249,
      0x08087ebf, 0x00440215, 0xb89a2249,
   };
   const bool lx = ph_t2_free_rgba(ph_screen(ctx->base.screen)->chip_id);
   const unsigned head_dw = lx ? ARRAY_SIZE(head_lx) : ARRAY_SIZE(head);
   static const uint32_t tail[] = {
      0x00000200, 0x00040000, 0x14c00000,
      0x00080200, 0x00440000, 0x301a21a2,
   };
   struct drm_parhelia_warp_program program = {0};
   struct p3d_warp_inst move = {
      .op = P3D_WARP_WRA,
      .sop = P3D_WARP_SOP_MOV,
      .sdst_file = P3D_WARP_FILE_DST_O,
      .sdst_index = 2,
      .sdst_mask = 0xf,
      .nsrc = 3,
   };
   uint32_t code[18];

   if (ctx->warp_colour1)
      return true;
   if (!ctx->warp_program_instructions)
      return false;
   for (unsigned i = 0; i < 3; i++) {
      move.src[i].file = P3D_WARP_FILE_V;
      move.src[i].index = P3D_WARP_V_COLOUR1;
      move.src[i].swizzle = P3D_WARP_SWZ_IDENTITY;
   }
   memcpy(code, lx ? head_lx : head, head_dw * sizeof(uint32_t));
   p3d_warp_encode(&move, code + head_dw);
   memcpy(code + head_dw + 3, tail, sizeof tail);
   program.code = (uintptr_t)code;
   program.count = (head_dw + 3 + ARRAY_SIZE(tail)) / 3;
   if (ioctl(ph_screen(ctx->base.screen)->fd, DRM_IOCTL_PARHELIA_WARP_PROGRAM,
             &program))
      return false;
   ctx->warp_colour1 = (uint32_t)program.entry;
   return true;
}

/* The kernel's textured pass-through (no T2 write) plus "mov o2, v6": the
 * second colour of a one-set textured stream reaches the combiner as v1. */
static bool
ph_colour1_textured_program(struct ph_context *ctx)
{
   static const uint32_t head[] = {
      0x00083e00, 0x00440000, 0xb89a2249,
      0x08087ebf, 0x00440215, 0xb89a2249,
      0x0078023f, 0x00440200, 0xb01a21a2,
   };
   static const uint32_t tail[] = {
      0x00000200, 0x00040000, 0x14c00000,
      0x00080200, 0x00440000, 0x301a21a2,
   };
   struct drm_parhelia_warp_program program = {0};
   struct p3d_warp_inst move = {
      .op = P3D_WARP_WRA,
      .sop = P3D_WARP_SOP_MOV,
      .sdst_file = P3D_WARP_FILE_DST_O,
      .sdst_index = 2,
      .sdst_mask = 0xf,
      .nsrc = 3,
   };
   uint32_t code[ARRAY_SIZE(head) + 3 + ARRAY_SIZE(tail)];

   if (ctx->warp_colour1_tex)
      return true;
   if (!ctx->warp_program_instructions)
      return false;
   for (unsigned i = 0; i < 3; i++) {
      move.src[i].file = P3D_WARP_FILE_V;
      move.src[i].index = P3D_WARP_V_COLOUR1;
      move.src[i].swizzle = P3D_WARP_SWZ_IDENTITY;
   }
   memcpy(code, head, sizeof(head));
   p3d_warp_encode(&move, code + ARRAY_SIZE(head));
   memcpy(code + ARRAY_SIZE(head) + 3, tail, sizeof(tail));
   program.code = (uintptr_t)code;
   program.count = ARRAY_SIZE(code) / 3;
   if (ioctl(ph_screen(ctx->base.screen)->fd, DRM_IOCTL_PARHELIA_WARP_PROGRAM,
             &program))
      return false;
   ctx->warp_colour1_tex = (uint32_t)program.entry;
   return true;
}

/* The sized-point program: the kernel's pass-through with two adds that state
 * the point's extent as o9 = (x - r, y - r, x + r, y + r), r from c0.x.  It is
 * uploaded once per context, on the first wide point, because the upload
 * drains the engine and most contexts never draw one. */
static bool
ph_sized_point_program(struct ph_context *ctx)
{
   /* add o0, v0, v0.0000 | mul T2, v5, r1.0000 | mov o11, v0 |
    * add o1, v5, r1.0000 ... end | wra */
   static const uint32_t head[] = {
      0x00083e00, 0x00440000, 0xb89a2249,
      0x0800bebf, 0x00440215, 0xba9a2249,
      0x08087e00, 0x00440015, 0xb89a2249,
   };
   static const uint32_t tail[] = {
      0x00000200, 0x00040000, 0x14c00000,
      0x00080200, 0x00440000, 0x301a21a2,
   };
   struct drm_parhelia_warp_program program = {0};
   struct p3d_warp_inst add = {
      .op = P3D_WARP_ADD,
      .dst_file = P3D_WARP_FILE_DST_O,
      .dst_index = 9,
      .nsrc = 2,
   };
   uint32_t code[21];		/* seven bundles of three dwords */

   if (ctx->warp_sized_point)
      return true;
   if (!ctx->warp_constant_loader || !ctx->warp_program_instructions)
      return false;

   add.src[0].file = P3D_WARP_FILE_V;
   add.src[0].index = 0;
   add.src[0].swizzle = P3D_WARP_SWZ(0, 1, 0, 1);
   add.src[1].file = P3D_WARP_FILE_C;
   add.src[1].index = 0;
   add.src[1].swizzle = P3D_WARP_SWZ(0, 0, 0, 0);

   memcpy(code, head, sizeof head);
   add.dst_mask = 0x3;
   add.src[1].neg = 1;
   p3d_warp_encode(&add, code + 9);
   add.dst_mask = 0xc;
   add.src[1].neg = 0;
   p3d_warp_encode(&add, code + 12);
   memcpy(code + 15, tail, sizeof tail);

   program.code = (uintptr_t)code;
   program.count = 7;
   if (ioctl(ph_screen(ctx->base.screen)->fd, DRM_IOCTL_PARHELIA_WARP_PROGRAM,
             &program))
      return false;
   ctx->warp_sized_point = (uint32_t)program.entry;
   ctx->warp_sized_point_slot = program.slot;
   return true;
}

/* Pick the WARP program: the transforming variants when the batch carries
 * object-space vertices, the pass-through variants otherwise.  There is no
 * transforming GL_REPLACE program; the transformed textured one leaves the
 * unused colour in o1 and the combiner ignores it. */
static uint32_t
ph_warp_entry(const struct ph_context *ctx, bool textured, bool two_sets,
              bool replace)
{
   if (ctx->transform && ctx->warp_program_entry)
      return ctx->warp_program_entry;
   if (ctx->transform)
      return two_sets ? ctx->warp_transform_multitex :
             textured ? ctx->warp_transform_textured : ctx->warp_transform;
   return two_sets ? ctx->warp_textured_multitex :
          textured ? (replace ? ctx->warp_replace : ctx->warp_textured) :
                     ctx->warp_passthrough;
}

/* A compiled program reading a generic varying routed into v0 needs the
 * textured program that forwards the vertex colour, not REPLACE's. */
static bool
ph_fs_reads_routed_v0(const struct ph_fs *fs)
{
   return fs && fs->program.valid && fs->program.route[0] >= 0;
}

/* Ready unit @u (2 or 3) of an original-family draw: its texture must live in
 * the sampler storage, uploaded, and its descriptor is unit 1's form.  False
 * drops the draw. */
static bool
ph_extra_unit(struct ph_context *ctx, unsigned u, uint32_t *handle,
              struct p3d_texture *tex)
{
   struct parhelia_resource *r = ph_resource(ctx->sampler_views[u]->texture);
   const struct pipe_sampler_state *sampler =
      ctx->num_samplers > u ? ctx->samplers[u] : NULL;
   bool mipped;

   if (!r->upload_texture || !r->handle || r->mip.slices ||
       r->base.width0 > 0x3fff || ph_resource_upload_texture(r))
      return false;
   mipped = ph_use_mips(r, sampler, ctx->sampler_views[u]);
   memset(tex, 0, sizeof(*tex));
   tex->width = r->base.width0;
   tex->height = r->base.height0;
   ph_texture_descriptor_format(r, false, true, tex);
   tex->min_filter = ph_sampler_image_filter(
      sampler && sampler->min_img_filter == PIPE_TEX_FILTER_LINEAR,
      sampler ? sampler->max_anisotropy : 0);
   tex->mag_filter = ph_sampler_image_filter(
      sampler && sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR,
      sampler ? sampler->max_anisotropy : 0);
   tex->mip_filter = mipped ? ph_sampler_mip_filter(sampler) : P3D_MIP_NONE;
   if (!ph_sampler_wrap_axes(sampler, ctx->sampler_views[u], r->base.target,
                             tex))
      return false;
   tex->max_level = mipped ?
      ph_view_mip_levels(r, ctx->sampler_views[u]) - 1 : 0;
   ph_apply_view_base(r, ctx->sampler_views[u], true, tex);
   tex->filter = ph_sampler_filter_word(tex, sampler, ph_max_lod_bias(ctx));
   tex->wrap = ph_sampler_wrap_word(tex, sampler);
   tex->env = P3D_TEXENV_MODULATE;
   *handle = r->handle;
   return true;
}

/* Expand an indexed batch into its triangles' vertices, for the routes
 * that take a plain vertex list. */
static bool
ph_tcl_deindex(struct ph_context *ctx, unsigned dpv)
{
   const unsigned n = ctx->triangles * 3;
   const uint32_t *idx = (const uint32_t *)ctx->vertices +
                         (size_t)ctx->indexed_verts * dpv;

   if (ctx->deindex_cap < n * dpv) {
      float *grown = realloc(ctx->deindex, (size_t)n * dpv * sizeof(*grown));

      if (!grown)
         return false;
      ctx->deindex = grown;
      ctx->deindex_cap = n * dpv;
   }
   for (unsigned v = 0; v < n; v++)
      memcpy(ctx->deindex + (size_t)v * dpv,
             ctx->vertices + (size_t)idx[v] * dpv, dpv * sizeof(float));
   ctx->vertices = ctx->deindex;
   ctx->stage_room = 0;
   ctx->indexed_verts = 0;
   return true;
}

static void
ph_submit_vertices_staged(struct ph_context *ctx)
{
   struct parhelia_batch *batch = &ctx->build_batch;
   struct p3d_state state = {0};
   struct parhelia_resource *color, *depth = NULL;
   struct parhelia_resource *texture = NULL, *texture1 = NULL;
   struct p3d_texture tex = {0}, tex1 = {0};
   uint32_t texture_handle = 0, texture1_handle = 0;
   const struct pipe_sampler_state *sampler = NULL, *sampler1 = NULL;
   const struct pipe_rt_blend_state *blend;
   unsigned dpv;
   bool zpos;
   bool submitted = false;
   bool tiled_textures = ph_env("PARHELIA_TEXTURE_TILED");
   bool small_texture_linear = ph_env("PARHELIA_SMALL_TEXTURE_LINEAR");
   bool texture_tiled = false, texture1_tiled = false;
   bool texture_mipped = false, texture1_mipped = false;
   bool rgb5a1_texture = false;
   bool one_pass_multitexture = false;
   int ret;

   assert((!!ctx->points + !!ctx->lines + !!ctx->triangles) <= 1);
   if (ctx->points) {
      ph_submit_simple_primitives(ctx, P3D_POINTS);
      return;
   }
   if (ctx->lines) {
      ph_submit_simple_primitives(ctx, P3D_LINES);
      return;
   }
   if (!ctx->triangles || ctx->submit_error)
      goto out;
   ctx->stat.triangles += ctx->triangles;
   if (!ctx->fb.nr_cbufs || !ctx->fb.cbufs[0].texture) {
      PH_REFUSE("a draw without a colour buffer is dropped\n");
      goto out;
   }
   color = ph_resource(ctx->fb.cbufs[0].texture);
   if (ctx->fb.zsbuf.texture)
      depth = ph_resource(ctx->fb.zsbuf.texture);
   /* The upload class has CPU shadows but no reverse aperture/download path.
    * Never let a framebuffer write make those shadows stale and later
    * overwrite the rendered result on sampler bind. */
   if (color->upload_texture || (depth && depth->upload_texture)) {
      PH_REFUSE("a draw into sampler-only storage is dropped\n");
      goto out;
   }
   /* GL may keep a depth attachment bound while disabling depth test for a
    * HUD overlay.  The 8-dword alpha stream is the validated no-Z packet;
    * do not accidentally turn that into the depth/RGBA descriptor merely
    * because the framebuffer retained its attachment. */
   if (ctx->alpha_micro && (!ctx->dsa || !ctx->dsa->depth_enabled))
      depth = NULL;
   blend = ctx->blend ? &ctx->blend->rt[0] : NULL;
   if (ph_samples_unit0(ctx)) {
      texture = ph_resource(ctx->sampler_views[0]->texture);
      if (ctx->num_samplers)
         sampler = ctx->samplers[0];
   }
   if (texture && ctx->fs && ph_second_unit_bindable(ctx) &&
       ctx->num_sampler_views > 1 && ctx->sampler_views[1] &&
       ctx->sampler_views[1]->texture) {
      texture1 = ph_resource(ctx->sampler_views[1]->texture);
      if (ctx->num_samplers > 1)
         sampler1 = ctx->samplers[1];
   }
   /* A three-coordinate unit-0 texture owns the whole coordinate stream, so
    * there is no room for a second coordinate set and none for the
    * transform program's object-space position. */
   if (texture && texture->mip.slices) {
      texture1 = NULL;
      sampler1 = NULL;
      ctx->transform = false;
   }
   /* Original-family units 2 and 3, as the staged stream carries them. */
   const unsigned extra = texture && texture1 ? ctx->extra_units : 0;
   uint32_t handles[P3D_MAX_TEXTURE_UNITS] = { 0 };
   struct p3d_texture tex_x[2];

   if (texture && ctx->fs && (ctx->fs->sampler_mask & ~3u) &&
       extra + 2 != util_last_bit(ctx->fs->sampler_mask)) {
      PH_REFUSE("a draw sampling units its stream carries no coordinates for "
                "is dropped\n");
      goto out;
   }
   /* An explicit env setting remains a force-enable for the established Q3
    * launch. Without it, use only the P690 base-level format/dimension shape
    * recovered for layout 2; all small, mip-described and unusual textures
    * retain the conservative linear sampler backing. */
   /* A compressed image exists only in layout 5; it has no linear shadow. */
   if (texture)
      texture_tiled = texture->upload_texture || texture->mip.block_bytes ?
         true : tiled_textures ?
         (!small_texture_linear ||
          (texture->base.width0 >= 32 && texture->base.height0 >= 32)) :
         ph_auto_tiled_texture(ctx, texture, sampler);
   if (texture1)
      texture1_tiled = texture1->upload_texture || texture1->mip.block_bytes ?
         true : tiled_textures ?
         (!small_texture_linear ||
          (texture1->base.width0 >= 32 && texture1->base.height0 >= 32)) :
         ph_auto_tiled_texture(ctx, texture1, sampler1);
   texture_mipped = texture_tiled &&
      ph_use_mips(texture, sampler, ctx->sampler_views[0]);
   if (texture && ph_env("PARHELIA_TRACE_MIPS") && !texture->trace_mips) {
      texture->trace_mips = true;
      fprintf(stderr,
              "parhelia: mips tex=%u %ux%u last_level=%u levels=%u defined=%#x tiled=%u mipfilter=%u view=%u..%u used=%u\n",
              texture->handle, texture->base.width0, texture->base.height0,
              texture->base.last_level, texture->mip.levels,
              texture->mip_defined, texture_tiled,
              sampler ? sampler->min_mip_filter : 99,
              ctx->sampler_views[0]->u.tex.first_level,
              ctx->sampler_views[0]->u.tex.last_level, texture_mipped);
   }
   texture1_mipped = texture1_tiled &&
      ph_use_mips(texture1, sampler1, ctx->sampler_views[1]);
   if (!color->handle || (depth && !depth->handle)) {
      PH_REFUSE("a draw into a surface without storage is dropped\n");
      goto out;
   }
   /* A native import this chip cannot sample has no linear handle; its
    * only valid use here is the present copy. */
   if (texture && !texture_tiled && !texture->linear_handle && !texture1 &&
       !depth && !ctx->transform && ph_copy_present_quad(ctx, color, texture))
      goto out;
   if ((texture && (!(texture_tiled ? texture->handle : texture->linear_handle) ||
                    texture->base.width0 > 0x3fff)) ||
       (texture1 && (!(texture1_tiled ? texture1->handle : texture1->linear_handle) ||
                     texture1->base.width0 > 0x3fff)) ||
       (texture1 && !ph_screen(ctx->base.screen)->single_texture_pipe &&
        !ctx->warp_textured_multitex)) {
      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr,
                 "parhelia: reject textured draw tex=%u/%u tiled=%u native_shared=%u tex1=%p depth=%p transform=%u\n",
                 texture ? texture->handle : 0,
                 texture ? texture->linear_handle : 0, texture_tiled,
                 texture ? texture->native_shared : 0, (void *)texture1,
                 (void *)depth, ctx->transform);
      PH_REFUSE("a textured draw whose texture the unit cannot address is "
                "dropped\n");
      goto out;
   }
   if (texture)
      texture_handle = texture_tiled ? texture->handle : texture->linear_handle;
   if (texture1)
      texture1_handle = texture1_tiled ? texture1->handle : texture1->linear_handle;
   /* Layout 0 samples the CPU-visible shadow allocation.  Make a render
    * result authoritative there before it is rebound as a texture. */
   if (ctx->aggregate_batches && texture && !texture_tiled &&
       texture->tiled_dirty && ph_pending_writes_resource(ctx, texture) &&
       ph_drain_batch(ctx))
      goto out;
   if (texture && !texture_tiled)
      ph_resource_resolve_linear(texture);
   if (ctx->aggregate_batches && texture1 && !texture1_tiled &&
       texture1->tiled_dirty && ph_pending_writes_resource(ctx, texture1) &&
       ph_drain_batch(ctx))
      goto out;
   if (texture1 && !texture1_tiled)
      ph_resource_resolve_linear(texture1);

   if (!ctx->rgb_alpha_two_pass && !ctx->native_sampled_alpha &&
       texture && texture_tiled && !texture1 && blend &&
       blend->blend_enable && blend->rgb_func == PIPE_BLEND_ADD &&
       blend->alpha_func == PIPE_BLEND_ADD &&
       blend->rgb_src_factor == PIPE_BLENDFACTOR_SRC_ALPHA &&
       blend->rgb_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA &&
       blend->alpha_src_factor == PIPE_BLENDFACTOR_SRC_ALPHA &&
       blend->alpha_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA) {
      rgb5a1_texture = ph_prepare_rgb5a1(ctx, texture);
      if (ctx->submit_error)
         goto out;
      if (rgb5a1_texture)
         texture_handle = texture->rgb5a1_handle;
   }
   if (texture && texture->upload_texture &&
       (ret = ph_resource_upload_texture(texture))) {
      ctx->submit_error = ret;
      goto out;
   }
   if (texture1 && texture1->upload_texture &&
       (ret = ph_resource_upload_texture(texture1))) {
      ctx->submit_error = ret;
      goto out;
   }
   /* PARHELIA_VERIFY_DRAW=1 compares the sampled texture's VRAM with its
    * CPU image before every draw (debug only: waits for idle). */
   if (texture && texture->upload_texture && ph_env("PARHELIA_VERIFY_DRAW") &&
       !ph_wait_async(ctx)) {
      uint32_t *back = malloc(texture->size);
      struct drm_parhelia_upload_render down = {
         .data = (uintptr_t)back, .size = texture->size,
         .handle = texture->handle,
      };
      const uint32_t *want = texture->map;
      size_t first = SIZE_MAX, bad = 0;

      if (back && !ioctl(ph_screen(ctx->base.screen)->fd,
                         DRM_IOCTL_PARHELIA_DOWNLOAD_RENDER, &down))
         for (size_t i = 0; i < texture->size / 4; i++)
            if (back[i] != want[i]) {
               first = MIN2(first, i);
               bad++;
            }
      if (bad)
         fprintf(stderr, "parhelia: texture %ux%u handle=%u before draw: %zu dwords differ from byte %#zx (vram %08x cpu %08x)\n",
                 texture->base.width0, texture->base.height0,
                 texture->handle, bad, first * 4, back[first], want[first]);
      free(back);
   }
   if (ctx->native_sampled_alpha &&
       (ret = ph_prepare_alpha_companions(ctx, texture, sampler, true))) {
      ctx->submit_error = ret;
      goto out;
   }
   if (ctx->native_sampled_alpha && ph_env("PARHELIA_TRACE_NATIVE_ALPHA") &&
       ctx->trace_native_alpha_resource != texture) {
      ctx->trace_native_alpha_resource = texture;
      fprintf(stderr,
              "parhelia: native sampled alpha texture=%u logical=%ux%u companion=%u/%ux%u blend=%u alpha-test=%u depth=%u/%u\n",
              texture->handle, texture->base.width0, texture->base.height0,
              texture->alpha_rgb_handle, texture->alpha_companion_width,
              texture->alpha_companion_height, blend->blend_enable,
              ctx->dsa->alpha_enabled, ctx->dsa->depth_enabled,
              ctx->dsa->depth_writemask);
   }
   if (texture && !texture1 && !depth && !ctx->transform &&
       ph_copy_present_quad(ctx, color, texture))
      goto out;
   if (ctx->cpu_alpha_cutout) {
      if (ctx->aggregate_batches && ph_drain_batch(ctx))
         goto out;
      ph_resource_resolve_linear(color);
      ph_resource_resolve_linear(depth);
      ph_cpu_alpha_cutout(ctx, color, depth, texture);
      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr, "parhelia: CPU alpha cutout triangles=%u image=%#llx\n",
                 ctx->triangles,
                 (unsigned long long)ph_texture_base_hash(texture, true,
                                                           NULL, NULL));
      goto out;
   }
   if (ctx->aggregate_batches && color->linear_dirty && ph_drain_batch(ctx))
      goto out;
   ph_resource_upload_tiled(color);
   /* PARHELIA_NO_T2_RESTORE=1 (diagnostic) leaves the restore draw out. */
   if (!ctx->transform && ctx->restore_after_transform &&
       ctx->warp_passthrough && !ph_env("PARHELIA_NO_T2_RESTORE") &&
       (ctx->submit_error = ph_restore_after_transform(ctx, color, depth)))
      goto out;

   state.colour.pitch = color->stride;
   state.colour.fmt = color->surf_fmt;
   state.screen_w = ctx->fb.width;
   state.screen_h = ctx->fb.height;
   state.prim = P3D_TRIANGLES;
   state.warp_entry = ph_warp_entry(ctx, false, false, false);
   /* An untextured draw can still need a compiled fragment program - fixed
    * function fog is a mix between the fragment colour and a constant - as
    * long as that program samples nothing.  A smooth line's triangles carry
    * their final colour and take the plain colour combiner. */
   ctx->comb_refused = false;
   state.comb = ph_fs_untextured_program(ctx) && !ctx->line_coverage ?
                ph_compiled_combiner(ctx, false) : NULL;
   if (ctx->comb_refused) {
      PH_REFUSE("a batch whose fragment constants no combiner variant can hold is dropped\n");
      goto out;
   }
   state.dither = !ctx->blend || ctx->blend->dither;
   state.flat = ctx->rasterizer && ctx->rasterizer->flatshade;
   state.cull = ctx->derived_triangles ? 0 :
                ph_hardware_cull(ctx->rasterizer);
	state.has_depth = depth != NULL;
	/* A transforming batch carries object-space positions, the matrix in
	 * c0..c3 and the viewport in c8/c9, so its position always has z. */
	state.transform = ctx->transform;
	if (state.transform) {
		/* A compiled program brings its own block at c0 and names the
		 * slots its viewport pair sits in.  mvp stays set either way:
		 * the batch does transform with that matrix, the builder
		 * requires a transforming batch to name one, and
		 * p3d_emit_constant_group() emits the program's own block in
		 * preference to it. */
		state.mvp = ctx->mvp;
		if (ctx->warp_program_entry) {
			state.vs_const = ctx->vs_const;
			state.vs_const_vec4s = ctx->vs_const_vec4s;
			state.vp_slot = ctx->vp_slot;
		}
		state.const_loader_entry = ctx->warp_constant_loader;
		for (unsigned i = 0; i < 3; i++) {
			state.vp_scale[i] = ctx->viewport.scale[i];
			state.vp_offset[i] = ctx->viewport.translate[i];
		}
		state.vp_scale[3] = 1.0f;
		state.vp_offset[3] = 2.0f * fabsf(ctx->viewport.scale[1]);
	}
	state.oow = ctx->oow && texture && !ctx->native_sampled_alpha &&
	            !state.transform;
	zpos = depth != NULL || state.transform || state.oow;
	state.dw_per_vertex = texture ?
	                              ((texture1 || ctx->native_sampled_alpha) ?
	                               (zpos ? 10 : 9) :
	                               (zpos ? 8 : 7)) :
	                               (zpos ? 6 : 5);
	if (state.oow)
		state.dw_per_vertex++;
	state.texcoord_q = ctx->proj && texture && !texture1 &&
	                   (texture->mip.slices ? !texture->mip.cube : state.oow);
	if (state.texcoord_q && !texture->mip.slices)
		state.dw_per_vertex += 2;
	/* The staged stream carries a second colour: declare the kind-6
	 * attribute and bind the program that forwards it to output 2, so the
	 * combiner sees it as v1. */
	if (ctx->colour1 && (texture ? ctx->warp_colour1_tex && !texture1 :
	                                ctx->warp_colour1)) {
		state.vertex_colour1 = ctx->colour1;
		state.dw_per_vertex += 3 + !!(ctx->colour1 & 2) +
				      !!(ctx->colour1 & 4);
		state.warp_entry = texture ? ctx->warp_colour1_tex :
		                             ctx->warp_colour1;
	}
	/* ph_stage_tri selected the exact one-texture MODULATE source-alpha
	 * predicate.  Re-evaluate it against the resolved depth/texture resources,
	 * then label the Z-bound 9-dword form explicitly for batch validation. */
	if (ctx->vertex_alpha &&
	    (ctx->alpha_micro ||
	     ph_use_vertex_alpha(ctx, texture != NULL, texture1 != NULL,
	                         depth != NULL, true))) {
		state.vertex_alpha = 1;
		state.depth_vertex_alpha = depth != NULL;
		state.dw_per_vertex++;
	}
	/* The measured untextured RGBA stream: x,y,z,r,g,b,a. */
	if (ctx->colour_alpha && !texture) {
		state.vertex_alpha = 1;
		state.pos_z = !zpos;
		state.dw_per_vertex = 7;
		/* The T2-free pass-through: the kernel's writes T2 = colour, which
		 * rejects (and on the 0528 locked up) alpha-below-colour draws. */
		if (!state.transform &&
		    ph_t2_free_rgba(ph_screen(ctx->base.screen)->chip_id))
			state.warp_entry = ctx->warp_rgba;
	}
	if (ctx->packed_colour) {
		state.packed_colour = 1;
		state.dw_per_vertex -= 2 + !!state.vertex_alpha;
	}
   state.single_texture_pipe = ph_screen(ctx->base.screen)->single_texture_pipe;
   /* One texture ganged on pipes 0+2: +25% on trilinear minification. */
   /* 0x11e8, the global texture-cache partition latch.  The production path
    * writes zero while the fill probes use 0x00500000 and the vendor writes
    * 0x100000; measured in the ioquake3 timedemo, all three are within noise
    * of each other (66.5 / 66.6 / 66.9 fps), so zero stays and the knob is
    * here for the next attempt at decoding the register. */
   state.texture_cache_probe = ph_env("PARHELIA_TEX_CACHE") ?
      (uint32_t)strtoul(getenv("PARHELIA_TEX_CACHE"), NULL, 0) : 0;
   /* The vendor's per-family 0x12f4 (31.0 on the LX class, KRN mtx2572);
    * PARHELIA_NO_LX_GUARD_BAND=1 emits the original family's 32.0. */
   state.lx_family = !ph_env("PARHELIA_NO_LX_GUARD_BAND") &&
      ph_lx_family(ph_screen(ctx->base.screen)->chip_id);
   /* The 0x11e8 latch is an LX-family register; writing it on the original
    * Parhelia stops its texture engine retiring. */
   state.tex_cache_latch =
      ph_lx_family(ph_screen(ctx->base.screen)->chip_id);
   state.lx_pipe_route = ph_lx_family(ph_screen(ctx->base.screen)->chip_id) &&
                         !ph_env("PARHELIA_NO_LX_PIPES");
   /* The kernel advertises this only under its independent default-off
    * module gate.  CREATE_VERTEX mmap success is also required, keeping the
    * ordinary 0x97/class-3 builder as the fallback context path. */
   state.bound_ranges = ph_screen(ctx->base.screen)->bound_range &&
                        ctx->vram_vertex_enabled && !ctx->aggregate_batches;
   if (texture) {
      /* A texture descriptor carries its true width.  Layout 0 reads the
       * paired linear shadow; layout 2 reads the native 32x16 sampler tiles. */
      tex.width = texture->base.width0;
      tex.height = texture->base.height0;
      ph_texture_descriptor_format(texture, rgb5a1_texture, texture_tiled,
                                   &tex);
      /* A volume or cube map samples with three coordinates: 0x11e0[20:19]
       * names the target, 0x1200+4u[18:16] switches the address unit to three
       * coordinates and 0x11e4[27:16] carries the slice count, which for a
       * cube map is the face edge length rather than six (vendor mtx3887,
       * .text+0x279bd). */
      if (texture->mip.slices) {
         tex.type = texture->mip.cube ? P3D_TEX_TYPE_CUBE :
                                        P3D_TEX_TYPE_VOLUME;
         tex.depth = texture->mip.cube ? texture->base.width0 :
                                         texture->mip.slices;
         tex.coord_mode = P3D_TEX_COORD_3D;
         state.texcoord3 = 1;
      }
      if (state.texcoord_q)
         tex.coord_mode = state.texcoord3 ? P3D_TEX_COORD_3D_PROJ :
                                            P3D_TEX_COORD_2D_PROJ;
      tex.min_filter = ph_sampler_image_filter(
         sampler && sampler->min_img_filter == PIPE_TEX_FILTER_LINEAR,
         sampler ? sampler->max_anisotropy : 0);
      tex.mag_filter = ph_sampler_image_filter(
         sampler && sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR,
         sampler ? sampler->max_anisotropy : 0);
      tex.mip_filter = texture_mipped ? ph_sampler_mip_filter(sampler) :
                                       P3D_MIP_NONE;
      if (!ph_sampler_wrap_axes(sampler, ctx->sampler_views[0],
                                texture->base.target, &tex)) {
         ph_warn_once("texture wrap mode %u/%u/%u has no hardware address "
                      "code; the draw is dropped rather than rendered with a "
                      "different one\n", sampler->wrap_s, sampler->wrap_t,
                      sampler->wrap_r);
         goto out;
      }
      tex.max_level = texture_mipped ?
         ph_view_mip_levels(texture, ctx->sampler_views[0]) - 1 : 0;
      /* The vendor's cube level field holds the level count, not count - 1
       * (mtx3887 0x279ad); the 0528 samples both alike (MEASURED). */
      if (texture->mip.native_64x32 && texture->mip.cube && texture_mipped)
         tex.max_level++;
      ph_apply_view_base(texture, ctx->sampler_views[0], texture_tiled, &tex);
      tex.filter = ph_sampler_filter_word(&tex, sampler, ph_max_lod_bias(ctx));
      tex.wrap = ph_sampler_wrap_word(&tex, sampler);
      tex.env = ctx->fs->texenv;
      ctx->comb_refused = false;
      state.comb = ph_compiled_combiner(ctx, texture1 != NULL);
      if (ctx->comb_refused) {
         PH_REFUSE("a batch whose fragment constants no combiner variant can hold is dropped\n");
         goto out;
      }
      state.comb_slot_base = ph_env("PARHELIA_COMB_SLOT_BASE") ?
         (unsigned)atoi(getenv("PARHELIA_COMB_SLOT_BASE")) : 0;
      state.tex = &tex;
      ph_pass_unit_state(ctx, 0, &tex);
      state.warp_entry =
         ph_warp_entry(ctx, true, texture1 || ctx->native_sampled_alpha,
                       !ctx->fs->uses_color && !ctx->alpha_micro &&
                       !ph_fs_reads_routed_v0(ctx->fs));
      if (state.vertex_colour1)
         state.warp_entry = ctx->warp_colour1_tex;
      if (ctx->native_sampled_alpha) {
         tex1 = tex;
         tex1.reloc_offset = 0;
         tex1.width = texture->alpha_companion_width;
         tex1.height = texture->alpha_companion_height;
         tex1.fmt = 0x32;
         tex1.texelcode = 2;
         tex1.mip_filter = P3D_MIP_NONE;
         tex1.max_level = 0;
         tex1.env = P3D_TEXENV_MODULATE;
         state.tex1 = &tex1;
         state.sampled_alpha_companion = true;
         texture1_handle = texture->alpha_rgb_handle;
         texture1_tiled = true;
      } else if (texture1) {
         tex1.width = texture1->base.width0;
         tex1.height = texture1->base.height0;
         ph_texture_descriptor_format(texture1, false, texture1_tiled, &tex1);
         tex1.min_filter = ph_sampler_image_filter(
            sampler1 && sampler1->min_img_filter == PIPE_TEX_FILTER_LINEAR,
            sampler1 ? sampler1->max_anisotropy : 0);
         tex1.mag_filter = ph_sampler_image_filter(
            sampler1 && sampler1->mag_img_filter == PIPE_TEX_FILTER_LINEAR,
            sampler1 ? sampler1->max_anisotropy : 0);
         tex1.mip_filter = texture1_mipped ? ph_sampler_mip_filter(sampler1) :
                                            P3D_MIP_NONE;
         if (!ph_sampler_wrap_axes(sampler1, ctx->sampler_views[1],
                                   texture1->base.target, &tex1)) {
            ph_warn_once("texture wrap mode %u/%u/%u has no hardware address "
                         "code on the second unit; the draw is dropped\n",
                         sampler1->wrap_s, sampler1->wrap_t, sampler1->wrap_r);
            goto out;
         }
         tex1.max_level = texture1_mipped ?
            ph_view_mip_levels(texture1, ctx->sampler_views[1]) - 1 : 0;
         ph_apply_view_base(texture1, ctx->sampler_views[1], texture1_tiled,
                            &tex1);
         tex1.filter = ph_sampler_filter_word(&tex1, sampler1, ph_max_lod_bias(ctx));
         tex1.wrap = ph_sampler_wrap_word(&tex1, sampler1);
         /* Quake's base world pass is diffuse times lightmap. */
         tex1.env = P3D_TEXENV_MODULATE;
         state.tex1 = &tex1;
         ph_pass_unit_state(ctx, 1, &tex1);
      }
   }
   if (extra) {
      handles[0] = texture_handle;
      handles[1] = texture1_handle;
      for (unsigned i = 0; i < extra; i++)
         if (!ph_extra_unit(ctx, 2 + i, &handles[2 + i], &tex_x[i])) {
            PH_REFUSE("a draw whose third or fourth texture the unit cannot "
                      "address is dropped\n");
            goto out;
         }
      state.tex2 = &tex_x[0];
      state.tex3 = extra > 1 ? &tex_x[1] : NULL;
      state.dw_per_vertex += 2 * extra;
      state.warp_entry = ctx->warp_four_sets;
      /* the texenv fallback would combine units 0 and 1 only */
      if (!state.comb) {
         PH_REFUSE("a three- or four-texture draw without a compiled program "
                   "is dropped\n");
         goto out;
      }
   }
   if (depth) {
      state.depth.pitch = depth->stride;
      state.depth.fmt = depth->surf_fmt;
      state.depth_slot = 1;
      state.depth_test = ctx->dsa && ctx->dsa->depth_enabled;
      state.depth_write = ctx->dsa && ctx->dsa->depth_writemask;
      state.depth_func = ph_compare(ctx->dsa ? ctx->dsa->depth_func :
                                               PIPE_FUNC_LESS);
   }
   if (ctx->dsa) {
      state.alpha_test = ctx->dsa->alpha_enabled;
      state.alpha_func = ph_compare(ctx->dsa->alpha_func);
      state.alpha_ref = CLAMP((int)lrintf(ctx->dsa->alpha_ref_value * 255.0f),
                              0, 255);
   }
   if (blend) {
		state.blend = blend->blend_enable;
		state.blend_src = ph_blend_factor(blend->rgb_src_factor);
		state.blend_dst = ph_blend_factor(blend->rgb_dst_factor);
		/* This marker is the batch validator's explicit permission for the
		 * P690-proven depth/RGBA SRC_ALPHA,ONE ADD packet.  It cannot be
		 * reached unless ph_use_vertex_alpha() accepted every fixed-function
		 * restriction above, including matching alpha-pipe factors. */
		state.depth_vertex_alpha_add = state.depth_vertex_alpha &&
			blend->rgb_dst_factor == PIPE_BLENDFACTOR_ONE &&
			blend->alpha_dst_factor == PIPE_BLENDFACTOR_ONE;
      const unsigned colormask = blend->colormask &
         (ctx->fb.nr_cbufs ? ph_target_channels(ctx->fb.cbufs[0].format) :
                             PIPE_MASK_RGBA);

      state.mask_r = !!(colormask & PIPE_MASK_R);
      state.mask_g = !!(colormask & PIPE_MASK_G);
      state.mask_b = !!(colormask & PIPE_MASK_B);
      state.mask_a = !!(colormask & PIPE_MASK_A);
      state.mask_explicit = true;
   }
   ph_apply_fragment_state(ctx, &state, depth != NULL);
   if (ctx->alpha_micro_cutout) {
      /* Alpha was classified before splitting.  The surviving cells are
       * opaque, so suppress the broken sampled-alpha comparator. */
      state.alpha_test = false;
      state.blend = false;
   }
	/* One pass binds both units on the P690's pipes 0 and 2: any program
	 * the combiner compiled, or the fixed-function MODULATE route. */
	one_pass_multitexture = ctx->p690_multitexture && ctx->fs &&
	   (ctx->native_sampled_alpha ?
	       ctx->fs->serial_op == PH_SERIAL_MODULATE :
	       texture1 && (state.comb ||
	                    (ctx->fs->serial_op == PH_SERIAL_MODULATE &&
	                     ph_serial_alpha_exact(ctx))));
	if (ctx->stats_sync) {
		const unsigned stride = ctx->rgb_alpha_two_pass ? 10 :
		                        state.dw_per_vertex;

		ctx->stat_pixels = 0;
		for (unsigned t = 0; t < ctx->triangles; t++) {
			const float *v = ctx->vertices + t * 3 * stride;
			float p[3][2];

			for (unsigned i = 0; i < 3; i++)
				ph_stat_window_xy(ctx, v + i * stride, p[i]);
			ctx->stat_pixels += fabsf((p[1][0] - p[0][0]) *
			                          (p[2][1] - p[0][1]) -
			                          (p[2][0] - p[0][0]) *
			                          (p[1][1] - p[0][1])) * 0.5f;
		}
		ctx->stat_kind = ctx->rgb_alpha_two_pass ? PH_STAT_ALPHA_TWO_PASS :
		   (ctx->alpha_micro || ctx->alpha_micro_cutout) ?
		      PH_STAT_ALPHA_MICRO :
		   (texture1 && ph_screen(ctx->base.screen)->single_texture_pipe &&
		    !one_pass_multitexture) ? PH_STAT_SERIAL :
		   (texture1 || ctx->native_sampled_alpha) ? PH_STAT_MULTITEX :
		   !texture ? PH_STAT_COLOUR :
		   texture_mipped ? PH_STAT_TEX_MIP :
		   texture_tiled ? PH_STAT_TEX_TILED : PH_STAT_TEX_LINEAR;
	}
	if (ctx->rgb_alpha_two_pass) {
	   if (!texture || texture1 || !texture_tiled) {
	      ret = -EINVAL;
	   } else {
	      ret = ph_submit_rgb_alpha_two_pass(ctx, color, depth, texture,
	                                          sampler,
	                                          &tex, &state);
	   }
	   submitted = true;
	   goto submission_done;
	}
	ph_trace_alpha_test(ctx, &state, color, texture, sampler, texture_tiled);

   dpv = state.dw_per_vertex;
   /* An indexed batch keeps its indices on the plain route; the others
    * take its triangles expanded. */
   if (ctx->indexed_verts) {
      if (ctx->native_sampled_alpha || extra ||
          (texture1 && ph_screen(ctx->base.screen)->single_texture_pipe &&
           !one_pass_multitexture)) {
         if (!ph_tcl_deindex(ctx, dpv)) {
            ctx->submit_error = -ENOMEM;
            goto out;
         }
      } else {
         state.indexed_vertices = ctx->indexed_verts;
         ctx->batch_index_dwords = ctx->triangles * 3;
      }
   }
	ph_trace_blend_alpha(ctx, &state, texture, sampler, texture_tiled);
	if (!texture && ph_env("PARHELIA_TRACE") && ctx->triangles) {
		const float *v = ctx->vertices;

		fprintf(stderr, "parhelia: untextured vertices dpv=%u", dpv);
		for (unsigned i = 0; i < 3 * dpv; i++)
			fprintf(stderr, "%s%g", i ? "," : " ", v[i]);
		fputc('\n', stderr);
	}
	if (texture && ph_env("PARHELIA_TRACE") && ctx->triangles) {
		const float *v0 = &ctx->vertices[0];
		const float *v1 = &ctx->vertices[dpv];
		const float *v2 = &ctx->vertices[2 * dpv];
		unsigned st = state.vertex_alpha ? (zpos ? 7 : 6) :
		              (zpos ? 6 : 5);
      fprintf(stderr,
              "parhelia: textured vertices p0=%g,%g st0=%g,%g p1=%g,%g st1=%g,%g p2=%g,%g st2=%g,%g\n",
		      v0[0], v0[1], v0[st], v0[st + 1],
		      v1[0], v1[1], v1[st], v1[st + 1],
		      v2[0], v2[1], v2[st], v2[st + 1]);
   }
   if (ctx->native_sampled_alpha) {
      ret = ph_batch_primitives(ctx, batch, &state, color->handle,
                                depth ? depth->handle : 0, texture_handle,
                                texture1_handle, ctx->triangles);
   } else if (texture1 && ph_screen(ctx->base.screen)->single_texture_pipe &&
              !one_pass_multitexture &&
              (ctx->fs->serial_op == PH_SERIAL_UNSUPPORTED ||
               !ph_serial_alpha_exact(ctx))) {
      PH_REFUSE("a two-texture draw with no one-pass or serial form is "
                "dropped\n");
      goto out;
   } else if (texture1 && ph_screen(ctx->base.screen)->single_texture_pipe &&
       !one_pass_multitexture) {
      struct p3d_state lightmap = state;
      struct p3d_texture lightmap_tex = tex1;
      unsigned packed_dpv = (zpos ? 8 : 7) + !!state.oow -
                            (ctx->packed_colour ? 2 : 0);

      submitted = true;

      /* The P690 advertises two texture units, but they share one physical
       * pipe.  The known fixed-function combinations have serial equivalents:
       * MODULATE is DST_COLOR,ZERO, ADD is ONE,ONE, and unit-1 REPLACE is a
       * single second-coordinate draw.  This retains a GPU path without the
       * unsupported simultaneous two-pipe state which wedges this silicon. */
      if (ctx->fs->serial_op == PH_SERIAL_REPLACE1) {
         /* Fixed-function unit-1 REPLACE has no unit-0 contribution. */
         lightmap_tex.env = P3D_TEXENV_REPLACE;
         lightmap.tex = &lightmap_tex;
         lightmap.tex1 = NULL;
         lightmap.dw_per_vertex = packed_dpv;
         lightmap.warp_entry = ph_warp_entry(ctx, true, false, true);
         ph_pack_multitex_vertices(ctx, true, zpos, state.oow);
         ret = ph_batch_primitives(ctx, batch, &lightmap, color->handle,
                                   depth ? depth->handle : 0, texture1_handle,
                                   0, ctx->triangles);
         if (!ret)
            ret = ph_submit_batch(ctx, batch, ctx->packed_vertices,
                                  ctx->triangles * 3 * packed_dpv,
                                  color, depth, texture1, NULL);
      } else {
         state.tex1 = NULL;
         state.dw_per_vertex = packed_dpv;
         state.warp_entry = ph_warp_entry(ctx, true, false,
                                          !ctx->fs->uses_color);
         ph_pack_multitex_vertices(ctx, false, zpos, state.oow);
         ret = ph_batch_primitives(ctx, batch, &state, color->handle,
                                   depth ? depth->handle : 0, texture_handle,
                                   0, ctx->triangles);
         if (!ret)
            ret = ph_submit_batch(ctx, batch, ctx->packed_vertices,
                                  ctx->triangles * 3 * packed_dpv,
                                  color, depth, texture, NULL);
      }
      if (ctx->fs->serial_op != PH_SERIAL_REPLACE1 &&
          !ret && !ph_env("PARHELIA_SKIP_LIGHTMAP")) {
         lightmap_tex.env = P3D_TEXENV_REPLACE;
         lightmap.tex = &lightmap_tex;
         lightmap.tex1 = NULL;
         lightmap.dw_per_vertex = packed_dpv;
         lightmap.warp_entry = ph_warp_entry(ctx, true, false, true);
         lightmap.blend = true;
         if (ctx->fs->serial_op == PH_SERIAL_ADD) {
            lightmap.blend_src = P3D_BF_ONE;
            lightmap.blend_dst = P3D_BF_ONE;
         } else {
            lightmap.blend_src = P3D_BF_DST_COLOR;
            lightmap.blend_dst = P3D_BF_ZERO;
         }
         if (ph_env("PARHELIA_LIGHTMAP_RAW")) {
            lightmap.blend = false;
            lightmap.blend_src = P3D_BF_ONE;
            lightmap.blend_dst = P3D_BF_ZERO;
         }
         if (depth) {
            lightmap.depth_test = true;
            lightmap.depth_write = false;
            lightmap.depth_func = P3D_CMP_EQUAL;
         }
         ph_pack_multitex_vertices(ctx, true, zpos, state.oow);
         ret = ph_batch_primitives(ctx, batch, &lightmap, color->handle,
                                   depth ? depth->handle : 0, texture1_handle,
                                   0, ctx->triangles);
         if (!ret)
            ret = ph_submit_batch(ctx, batch, ctx->packed_vertices,
                                  ctx->triangles * 3 * packed_dpv,
                                  color, depth, texture1, NULL);
      }
   } else if (extra)
      ret = ph_batch_primitives_units(ctx, batch, &state, color->handle,
                                      depth ? depth->handle : 0, handles,
                                      ctx->triangles);
   else
      ret = ph_batch_primitives(ctx, batch, &state, color->handle,
                                depth ? depth->handle : 0,
                                texture ? texture_handle : 0,
                                texture1 ? texture1_handle : 0,
                                ctx->triangles);
   if (!ret && !submitted)
      ret = ph_submit_batch(ctx, batch, ctx->vertices,
                            state.indexed_vertices ?
                               state.indexed_vertices * dpv +
                               ctx->triangles * 3 :
                               ctx->triangles * 3 * dpv,
                            color, depth, texture, texture1);
submission_done:
   if (ret)
      ctx->submit_error = ret;
   else {
      color->tiled_dirty = true;
      color->linear_dirty = false;
      color->mip_defined |= BITFIELD_BIT(0);
      color->content_generation++;
      if (depth) {
         depth->tiled_dirty = true, depth->content_generation++;
         depth->mip_defined |= BITFIELD_BIT(0);
      }
   }
   if (ret || ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: submit triangles=%u color=%u depth=%u texture=%u/%u fmt=%#x/%#x layout=%u/%u native1555=%u warp=%#x fs_tex=%u views=%u p690_multitex=%u ret=%d\n",
              ctx->triangles, color->handle, depth ? depth->handle : 0,
              texture_handle, texture1_handle, tex.fmt, tex1.fmt,
              texture_tiled ? 2 : 0, texture1_tiled ? 2 : 0,
              rgb5a1_texture, state.warp_entry,
              ctx->fs ? ctx->fs->uses_texture : 0, ctx->num_sampler_views,
              one_pass_multitexture, ret);
out:
   ctx->triangles = 0;
   ctx->stat_kind = PH_STAT_OTHER;
   ctx->stat_pixels = 0;
	/* The second colour describes the stream that was just submitted; a
	 * later draw that stages five dwords while this still says eight
	 * declares an attribute the stream does not carry. */
	ctx->colour1 = 0;
	ctx->proj = false;
	ctx->vertex_alpha = false;
	ctx->stencil_back = false;
	ctx->colour_alpha = false;
	ctx->packed_colour = false;
	ctx->indexed_verts = 0;
	ctx->batch_index_dwords = 0;
	ctx->line_coverage = false;
	ctx->alpha_micro = false;
	ctx->alpha_micro_cutout = false;
	ctx->cpu_alpha_cutout = false;
	ctx->rgb_alpha_two_pass = false;
	ctx->native_sampled_alpha = false;
	ctx->transform = false;
	ctx->warp_program_entry = 0;
	ctx->vs_const_vec4s = 0;
}

static void
ph_submit_vertices(struct ph_context *ctx)
{
   ph_submit_vertices_staged(ctx);
   ph_stage_at_ring_tail(ctx);
}

static float
ph_alpha_micro_sample(const struct parhelia_resource *texture,
                      const struct pipe_sampler_state *sampler,
                      float s, float t)
{
   unsigned x, y;
   const uint8_t *pixel;

   if (sampler->wrap_s == PIPE_TEX_WRAP_REPEAT)
      s -= floorf(s);
   else
      s = CLAMP(s, 0.0f, 1.0f);
   if (sampler->wrap_t == PIPE_TEX_WRAP_REPEAT)
      t -= floorf(t);
   else
      t = CLAMP(t, 0.0f, 1.0f);
   x = CLAMP((int)floorf(s * texture->base.width0), 0,
             (int)texture->base.width0 - 1);
   y = CLAMP((int)floorf(t * texture->base.height0), 0,
             (int)texture->base.height0 - 1);
   pixel = (const uint8_t *)texture->linear_map +
      (texture->linear_mip.levels ?
       ph_mip_texel_offset(&texture->linear_mip, 0, x, y, true) :
       (size_t)y * texture->linear_stride + x * 4);
   return pixel[3] * (1.0f / 255.0f);
}

static void
ph_alpha_micro_vertex(float out[9], const float *p0, const float *p1,
                      const float *p2, const float *c0, const float *c1,
                      const float *c2, const float *st0, const float *st1,
                      const float *st2, float w0, float w1, float w2,
                      float alpha)
{
   out[0] = w0 * p0[0] + w1 * p1[0] + w2 * p2[0];
   out[1] = w0 * p0[1] + w1 * p1[1] + w2 * p2[1];
   out[2] = CLAMP(w0 * p0[2] + w1 * p1[2] + w2 * p2[2], 0.0f, 1.0f);
   out[3] = w0 * c0[0] + w1 * c1[0] + w2 * c2[0];
   out[4] = w0 * c0[1] + w1 * c1[1] + w2 * c2[1];
   out[5] = w0 * c0[2] + w1 * c1[2] + w2 * c2[2];
   out[6] = CLAMP(alpha * (w0 * c0[3] + w1 * c1[3] + w2 * c2[3]),
                  0.0f, 1.0f);
   out[7] = w0 * st0[0] + w1 * st1[0] + w2 * st2[0];
   out[8] = w0 * st0[1] + w1 * st1[1] + w2 * st2[1];
}

static void
ph_alpha_micro_emit(struct ph_context *ctx, const float vertices[3][9],
                    bool depth, bool vertex_alpha, bool cutout)
{
   float *dst;
   const unsigned dpv = 5 + !!depth + 2 + !!vertex_alpha;

   if (ctx->triangles == ph_triangle_batch_limit(ctx) ||
       (ctx->triangles + 1) * 3 * dpv > ph_logical_vertex_limit(ctx))
      ph_submit_vertices(ctx);
   /* ph_submit_vertices clears the batch tag.  Every split primitive has the
    * same narrow state, so restore it before continuing the packet. */
   ctx->vertex_alpha = vertex_alpha;
   ctx->alpha_micro = !cutout;
   ctx->alpha_micro_cutout = cutout;
   dst = &ctx->vertices[ctx->triangles * 3 * dpv];
   if (depth && vertex_alpha) {
      memcpy(dst, vertices, 3 * 9 * sizeof(*dst));
   } else if (!depth && vertex_alpha) {
      for (unsigned v = 0; v < 3; v++) {
         *dst++ = vertices[v][0];
         *dst++ = vertices[v][1];
         memcpy(dst, &vertices[v][3], 6 * sizeof(*dst));
         dst += 6;
      }
   } else {
      for (unsigned v = 0; v < 3; v++) {
         *dst++ = vertices[v][0];
         *dst++ = vertices[v][1];
         if (depth)
            *dst++ = vertices[v][2];
         *dst++ = vertices[v][3];
         *dst++ = vertices[v][4];
         *dst++ = vertices[v][5];
         *dst++ = vertices[v][7];
         *dst++ = vertices[v][8];
      }
   }
   ctx->triangles++;
}

static void
ph_stage_alpha_micro(struct ph_context *ctx, struct prim_header *prim,
                     int pos, int col, int texcoord, bool depth, bool cutout)
{
   const struct parhelia_resource *texture =
      ph_resource(ctx->sampler_views[0]->texture);
   const struct pipe_sampler_state *sampler = ctx->samplers[0];
   const float *p0 = prim->v[0]->data[pos], *p1 = prim->v[1]->data[pos],
               *p2 = prim->v[2]->data[pos];
   const float white[4] = {1, 1, 1, 1};
   const float *c0 = col < 0 ? white : prim->v[0]->data[col],
               *c1 = col < 0 ? white : prim->v[1]->data[col],
               *c2 = col < 0 ? white : prim->v[2]->data[col];
   const float *st0 = prim->v[0]->data[texcoord],
               *st1 = prim->v[1]->data[texcoord],
               *st2 = prim->v[2]->data[texcoord];
   /* Crosshair alpha masks can have isolated one-texel strokes.  Sample the
    * binary GEQUAL path at the source's 16-pixel granularity; blended HUD
    * glyphs keep the lighter 8x8 subdivision. */
   const unsigned n = cutout ? 16 : 8;
   unsigned samples = 0, accepted = 0;
   float amin = 1.0f, amax = 0.0f;

   if (ctx->aggregate_batches && texture->tiled_dirty &&
       ph_pending_writes_resource(ctx, texture) && ph_drain_batch(ctx))
      return;
   ph_resource_resolve_linear((struct parhelia_resource *)texture);
   for (unsigned i = 0; i < n; i++) {
      for (unsigned j = 0; j < n - i; j++) {
         float v[3][9];
         float a, s, t;
         float w00[3] = {1.0f - (i + j) / (float)n, i / (float)n,
                          j / (float)n};
         float w10[3] = {1.0f - (i + 1 + j) / (float)n,
                          (i + 1) / (float)n, j / (float)n};
         float w01[3] = {1.0f - (i + j + 1) / (float)n, i / (float)n,
                          (j + 1) / (float)n};

         s = (w00[0] * st0[0] + w00[1] * st1[0] + w00[2] * st2[0] +
              w10[0] * st0[0] + w10[1] * st1[0] + w10[2] * st2[0] +
              w01[0] * st0[0] + w01[1] * st1[0] + w01[2] * st2[0]) / 3.0f;
         t = (w00[0] * st0[1] + w00[1] * st1[1] + w00[2] * st2[1] +
              w10[0] * st0[1] + w10[1] * st1[1] + w10[2] * st2[1] +
              w01[0] * st0[1] + w01[1] * st1[1] + w01[2] * st2[1]) / 3.0f;
         a = ph_alpha_micro_sample(texture, sampler, s, t);
         samples++;
         amin = MIN2(amin, a); amax = MAX2(amax, a);
         ph_alpha_micro_vertex(v[0], p0, p1, p2, c0, c1, c2, st0, st1, st2,
                               w00[0], w00[1], w00[2], a);
         ph_alpha_micro_vertex(v[1], p0, p1, p2, c0, c1, c2, st0, st1, st2,
                               w10[0], w10[1], w10[2], a);
         ph_alpha_micro_vertex(v[2], p0, p1, p2, c0, c1, c2, st0, st1, st2,
                               w01[0], w01[1], w01[2], a);
         if (!cutout || a >= 0.5f) {
            ph_alpha_micro_emit(ctx, v, depth, !cutout, cutout);
            accepted++;
         }

         if (i + j + 1 < n) {
            float w11[3] = {1.0f - (i + 1 + j + 1) / (float)n,
                             (i + 1) / (float)n, (j + 1) / (float)n};
            s = (w10[0] * st0[0] + w10[1] * st1[0] + w10[2] * st2[0] +
                 w11[0] * st0[0] + w11[1] * st1[0] + w11[2] * st2[0] +
                 w01[0] * st0[0] + w01[1] * st1[0] + w01[2] * st2[0]) / 3.0f;
            t = (w10[0] * st0[1] + w10[1] * st1[1] + w10[2] * st2[1] +
                 w11[0] * st0[1] + w11[1] * st1[1] + w11[2] * st2[1] +
                 w01[0] * st0[1] + w01[1] * st1[1] + w01[2] * st2[1]) / 3.0f;
            a = ph_alpha_micro_sample(texture, sampler, s, t);
            samples++;
            amin = MIN2(amin, a); amax = MAX2(amax, a);
            ph_alpha_micro_vertex(v[0], p0, p1, p2, c0, c1, c2, st0, st1, st2,
                                  w10[0], w10[1], w10[2], a);
            ph_alpha_micro_vertex(v[1], p0, p1, p2, c0, c1, c2, st0, st1, st2,
                                  w11[0], w11[1], w11[2], a);
            ph_alpha_micro_vertex(v[2], p0, p1, p2, c0, c1, c2, st0, st1, st2,
                                  w01[0], w01[1], w01[2], a);
            if (!cutout || a >= 0.5f) {
               ph_alpha_micro_emit(ctx, v, depth, !cutout, cutout);
               accepted++;
            }
         }
      }
   }
   if (cutout && ph_env("PARHELIA_TRACE_ALPHA_CUTOUT") &&
       ctx->trace_alpha_cutout_resource != texture) {
      ctx->trace_alpha_cutout_resource = texture;
      fprintf(stderr, "parhelia: alpha-cutout micro tex=%ux%u samples=%u accepted=%u range=%g..%g\\n",
              texture->base.width0, texture->base.height0, samples, accepted,
              amin, amax);
   }
}

/* The vertex output holding hardware unit @unit's coordinate set. */
static int
ph_unit_texcoord(const struct ph_context *ctx, unsigned unit)
{
   const unsigned index = ctx->fs && ctx->fs->unit_remap ?
                          ctx->fs->unit_source[unit] : unit;
   int texcoord;

   if (ctx->fs && index < PIPE_MAX_SAMPLERS && ctx->fs->coord_src_known[index]) {
      texcoord = draw_find_shader_output(ctx->draw, ctx->fs->coord_src_name[index],
                                         ctx->fs->coord_src_index[index]);
      if (texcoord >= 0)
         return texcoord;
   }
   texcoord = draw_find_shader_output(ctx->draw, TGSI_SEMANTIC_GENERIC, index);

   if (texcoord < 0)
      texcoord = draw_find_shader_output(ctx->draw, TGSI_SEMANTIC_TEXCOORD,
                                         index);
   return texcoord;
}

/* Unit 0's coordinate has a q other than one at some corner. */
static bool
ph_primitive_projective(const struct ph_context *ctx,
                        const struct prim_header *prim)
{
   int texcoord = ph_unit_texcoord(ctx, 0);

   if (texcoord < 0)
      return false;
   for (unsigned v = 0; v < 3; v++)
      if (prim->v[v]->data[texcoord][3] != 1.0f)
         return true;
   return false;
}

/* The fog factor reads |fog coordinate|: the state tracker keeps the
 * absolute value per fragment for an eye-plane distance (mesa issue #15407). */
static bool
ph_fog_abs_coord(const struct ph_fs_program *program)
{
   for (unsigned i = 0; i < program->fog_ops; i++)
      if (program->fog[i].kind == PH_FOG_ABS &&
          program->fog[program->fog[i].a].kind == PH_FOG_COORD)
         return true;
   return false;
}

/* The fog coordinates at which the per-vertex factor stops being linear:
 * zero under an absolute value, and where a saturated linear factor (GL_LINEAR
 * fog) reaches 0 and 1.  Between them the factor is linear in the coordinate,
 * so interpolating per-vertex values is exact.  Returns the count. */
static unsigned
ph_fog_cuts(const struct ph_context *ctx, float cut[6])
{
   const struct ph_fs_program *prog = &ctx->fs->program;
   const struct ph_fs_fog_op *root = &prog->fog[prog->fog_ops - 1];
   const bool abs_coord = ph_fog_abs_coord(prog);
   unsigned n = 0;

   if (abs_coord)
      cut[n++] = 0.0f;
   if (root->kind == PH_FOG_SAT) {
      const void *k = ctx->fs_const_snap;
      const unsigned size = ctx->fs_const_snap_size;
      const float v0 = ph_fs_fog_eval_step(prog, root->a, 1.0f, k, size);
      const float v1 = ph_fs_fog_eval_step(prog, root->a, 2.0f, k, size);
      const float v2 = ph_fs_fog_eval_step(prog, root->a, 3.0f, k, size);
      const float slope = v1 - v0;

      /* Only a factor linear in the coordinate (beyond zero) has two edges. */
      if (slope != 0.0f && fabsf(v2 - v1 - slope) <= 1e-4f * fabsf(slope)) {
         const float base = v0 - slope;

         for (unsigned e = 0; e < 2; e++) {
            const float c = ((float)e - base) / slope;

            cut[n++] = c;
            if (abs_coord && c != 0.0f)
               cut[n++] = -c;
         }
      }
   }
   return n;
}

static void ph_stage_tri(struct draw_stage *stage, struct prim_header *prim);

/* Stage a triangle cut at every fog coordinate in @cut it straddles, so the
 * colour-1 carrier interpolates a factor that is linear on each piece. */
static void
ph_stage_fog_pieces(struct draw_stage *stage, struct prim_header *prim,
                    int fogc, const float *cut, unsigned cuts)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   const unsigned attrs = draw_num_shader_outputs(stage->draw);
   const size_t size = sizeof(struct vertex_header) + attrs * 4 * sizeof(float);
   struct vertex_header *poly[2][4], *cv[2];
   unsigned n[2] = {0, 0}, made = 0;
   float d[3];
   uint8_t *mem;

   while (cuts) {
      for (unsigned v = 0; v < 3; v++)
         d[v] = prim->v[v]->data[fogc][0] - cut[0];
      if (MIN3(d[0], d[1], d[2]) < 0.0f && MAX3(d[0], d[1], d[2]) > 0.0f)
         break;
      cut++;
      cuts--;
   }
   if (!cuts || !(mem = malloc(2 * size))) {
      ph_stage_tri(stage, prim);
      return;
   }
   cv[0] = (struct vertex_header *)mem;
   cv[1] = (struct vertex_header *)(mem + size);
   for (unsigned i = 0; i < 3; i++) {
      const unsigned j = (i + 1) % 3;

      if (d[i] >= 0.0f)
         poly[0][n[0]++] = prim->v[i];
      if (d[i] <= 0.0f)
         poly[1][n[1]++] = prim->v[i];
      if ((d[i] < 0.0f && d[j] > 0.0f) || (d[i] > 0.0f && d[j] < 0.0f)) {
         const float t = d[i] / (d[i] - d[j]);
         struct vertex_header *c = cv[made++];

         memcpy(c, prim->v[i], sizeof(*c));
         for (unsigned k = 0; k < 4; k++)
            c->clip_pos[k] = prim->v[i]->clip_pos[k] +
               t * (prim->v[j]->clip_pos[k] - prim->v[i]->clip_pos[k]);
         for (unsigned a = 0; a < attrs; a++)
            for (unsigned k = 0; k < 4; k++)
               c->data[a][k] = prim->v[i]->data[a][k] +
                  t * (prim->v[j]->data[a][k] - prim->v[i]->data[a][k]);
         c->data[fogc][0] = cut[0];
         poly[0][n[0]++] = c;
         poly[1][n[1]++] = c;
      }
   }
   for (unsigned side = 0; side < 2 && !ctx->submit_error; side++)
      for (unsigned k = 1; k + 1 < n[side]; k++) {
         struct prim_header tri = *prim;

         tri.v[0] = poly[side][0];
         tri.v[1] = poly[side][k];
         tri.v[2] = poly[side][k + 1];
         ph_stage_fog_pieces(stage, &tri, fogc, cut + 1, cuts - 1);
      }
   free(mem);
}

/* Cut the triangle where its per-vertex fog factor would interpolate wrong;
 * false when nothing needs cutting. */
static bool
ph_split_fog_triangle(struct draw_stage *stage, struct prim_header *prim,
                      int fogc)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   float cut[6];
   const unsigned cuts = ph_fog_cuts(ctx, cut);

   if (!cuts)
      return false;
   ctx->fog_splitting = true;
   ph_stage_fog_pieces(stage, prim, fogc, cut, cuts);
   ctx->fog_splitting = false;
   return true;
}

/* The draw output feeding colour interpolator @k: a generic varying the
 * compiled program routes there, else the colour (or fog) it carries. */
static int
ph_v_output(struct draw_context *draw, const struct ph_context *ctx, unsigned k)
{
   const struct ph_fs *fs = ctx->fs;

   if (fs && fs->program.valid && fs->program.route[k] >= 0) {
      unsigned name, index;
      int out;

      /* texcoord semantics first; without them the generics follow the
       * eight texture coordinate sets */
      tgsi_get_gl_varying_semantic(fs->program.route[k], true, &name, &index);
      out = draw_find_shader_output(draw, name, index);
      if (out < 0) {
         tgsi_get_gl_varying_semantic(fs->program.route[k], false, &name,
                                      &index);
         out = draw_find_shader_output(draw, name, index);
      }
      return out;
   }
   if (k == 0)
      return draw_find_shader_output(draw, TGSI_SEMANTIC_COLOR, 0);
   return fs && fs->program.valid && fs->program.uses_fogc ?
          draw_find_shader_output(draw, TGSI_SEMANTIC_FOG, 0) :
          draw_find_shader_output(draw, TGSI_SEMANTIC_COLOR, 1);
}

/* A generic varying routed through a colour interpolator: MEASURED on the
 * P690, the interpolators clamp to [0, 1] (-0.5, 1.5 and 2.0 read as 0, 1
 * and 1).  A vertex outside that range would render a different value, so
 * such a primitive is dropped rather than drawn wrong. */
static bool
ph_routed_in_range(struct draw_context *draw, const struct ph_context *ctx,
                   const struct vertex_header *const *v, unsigned n)
{
   const struct ph_fs *fs = ctx->fs;

   if (!fs || !fs->program.valid)
      return true;
   for (unsigned k = 0; k < 2; k++) {
      int out;

      if (fs->program.route[k] < 0)
         continue;
      out = ph_v_output(draw, ctx, k);
      if (out < 0)
         continue;
      for (unsigned i = 0; i < n; i++)
         for (unsigned c = 0; c < 4; c++)
            if (!(v[i]->data[out][c] >= -1e-6f &&
                  v[i]->data[out][c] <= 1.0f + 1e-6f)) {
               ph_warn_once("a varying routed through a colour interpolator "
                            "left [0, 1]; the primitive is dropped\n");
               return false;
            }
   }
   return true;
}

/* gl_PointCoord's window-space gradient over a sprite triangle, d(s)/dx,
 * d(t)/dx, d(s)/dy, d(t)/dy; zero without a point coordinate. */
static void
ph_pntc_derivatives(struct draw_context *draw, const struct prim_header *prim,
                    int pos, float d[4])
{
   const int pc = draw_find_shader_output(draw, TGSI_SEMANTIC_PCOORD, 0);
   const float *p0 = prim->v[0]->data[pos], *p1 = prim->v[1]->data[pos],
               *p2 = prim->v[2]->data[pos];
   const float ax = p1[0] - p0[0], ay = p1[1] - p0[1];
   const float bx = p2[0] - p0[0], by = p2[1] - p0[1];
   const float det = ax * by - bx * ay;

   memset(d, 0, 4 * sizeof(float));
   if (pc < 0 || det == 0.0f)
      return;
   for (unsigned c = 0; c < 2; c++) {
      const float fa = prim->v[1]->data[pc][c] - prim->v[0]->data[pc][c];
      const float fb = prim->v[2]->data[pc][c] - prim->v[0]->data[pc][c];

      d[c] = (fa * by - fb * ay) / det;
      d[2 + c] = (ax * fb - bx * fa) / det;
   }
}

static void
ph_stage_tri(struct draw_stage *stage, struct prim_header *prim)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   if (!ph_routed_in_range(stage->draw, ctx,
                           (const struct vertex_header *const *)prim->v, 3))
      return;
   int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   int col = ph_v_output(stage->draw, ctx, 0);
   int texcoord = -1, texcoord1 = -1, texcoord_x[2] = { -1, -1 };
   unsigned extra = 0;
   bool textured = ph_samples_unit0(ctx);
	bool multitextured = textured && ctx->fs &&
                        ph_second_unit_bindable(ctx) &&
                        ctx->num_sampler_views > 1 &&
                        ctx->sampler_views[1] &&
                        ctx->sampler_views[1]->texture;
   bool depth = ctx->fb.zsbuf.texture != NULL;
   /* A volume or cube map needs a third coordinate, which costs the colour
    * attribute and every alpha route that re-shapes the vertex stream. */
   bool three_coord = textured && ph_three_coord_texture(ctx);
	bool vertex_alpha;
	bool alpha_micro;
	bool alpha_micro_cutout;
	bool alpha_micro_depth;
	bool cpu_alpha_cutout;
	bool rgb_alpha_two_pass;
	bool native_sampled_alpha;
	unsigned dpv;
	bool oow = false;
	bool proj = false;
	unsigned colour1;
	bool textured_colour1;
	bool colour_alpha;
   float *dst;
   const int fogc = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_FOG, 0);
   const int col1 = ph_v_output(stage->draw, ctx, 1);

   /* The hardware exposes only one-face cull selectors.  FRONT_AND_BACK has
    * no packet encoding and must reject the polygon before staging it. */
   if (!ctx->derived_triangles && ctx->rasterizer &&
       ctx->rasterizer->cull_face == PIPE_FACE_FRONT_AND_BACK)
      return;

   /* Flat shading takes the last vertex's colour; a cyclic rotation makes the
    * first-vertex convention's provoking vertex last and keeps the winding. */
   struct prim_header rotated;
   if (!ctx->derived_triangles && ctx->rasterizer &&
       ctx->rasterizer->flatshade && ctx->rasterizer->flatshade_first) {
      rotated = *prim;
      rotated.v[0] = prim->v[1];
      rotated.v[1] = prim->v[2];
      rotated.v[2] = prim->v[0];
      prim = &rotated;
   }

   /* Preserve mixed-primitive ordering while allowing every run of compatible
    * line callbacks to share one state block. */
   if (ctx->points || ctx->lines) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return;
   }
   if (!ctx->fog_splitting && fogc >= 0 && ctx->fs &&
       ctx->fs->program.valid && ctx->fs->program.fog &&
       ph_split_fog_triangle(stage, prim, fogc))
      return;
   if (pos >= 0) {
      const bool back = !ctx->derived_triangles && ctx->rasterizer &&
                        ph_two_sided_stencil(ctx) &&
                        ph_triangle_back_facing(ctx, prim, pos);

      if (ctx->triangles && ctx->stencil_back != back) {
         ph_submit_vertices(ctx);
         if (ctx->submit_error)
            return;
      }
      ctx->stencil_back = back;
   }
   if (pos >= 0 && ctx->fs && ctx->fs->point_deriv) {
      float d[4];

      ph_pntc_derivatives(stage->draw, prim, pos, d);
      if (memcmp(d, ctx->pntc_deriv, sizeof(d))) {
         if (ctx->triangles) {
            ph_submit_vertices(ctx);
            if (ctx->submit_error)
               return;
         }
         memcpy(ctx->pntc_deriv, d, sizeof(d));
      }
   }

	vertex_alpha = ph_use_vertex_alpha(ctx, textured, multitextured, depth,
				  col >= 0);
	native_sampled_alpha =
	   ph_use_native_sampled_alpha(ctx, textured, multitextured, depth) &&
	   ph_native_sampled_alpha_primitive(prim, col);
	if (native_sampled_alpha)
	   vertex_alpha = false;
	rgb_alpha_two_pass = !native_sampled_alpha &&
	                     ph_use_rgb_alpha_two_pass(ctx, textured,
	                                               multitextured, col >= 0) &&
		  ph_rgb_alpha_primitive(ctx, prim, pos, col);
	alpha_micro = !native_sampled_alpha && !rgb_alpha_two_pass &&
	              ph_use_alpha_micro(ctx, textured, multitextured, depth,
	                                 col >= 0);
	alpha_micro_cutout = !native_sampled_alpha &&
	                     ph_use_alpha_micro_cutout(ctx, textured,
	                                                multitextured, depth,
	                                                col >= 0);
	if (alpha_micro)
	   vertex_alpha = true;
	if (alpha_micro_cutout)
	   vertex_alpha = false;
	cpu_alpha_cutout = !native_sampled_alpha &&
	                   ph_use_cpu_alpha_cutout(ctx, textured, multitextured,
	                                            depth, col >= 0);
	if (alpha_micro || alpha_micro_cutout)
	   cpu_alpha_cutout = rgb_alpha_two_pass = false;
	if (three_coord) {
	   multitextured = false;
	   vertex_alpha = alpha_micro = alpha_micro_cutout = false;
	   cpu_alpha_cutout = rgb_alpha_two_pass = native_sampled_alpha = false;
	   if (ctx->fs && ctx->fs->uses_color &&
	       !ph_primitive_colour_is_white(prim, col)) {
	      static bool said;

	      if (!said) {
	         said = true;
	         fprintf(stderr,
	                 "parhelia: a volume or cube-map draw has no room for a "
	                 "vertex colour: position, colour and a three-component "
	                 "coordinate do not fit the attribute port\n");
	      }
	      return;
	   }
	}
	alpha_micro_depth = depth && ctx->dsa && ctx->dsa->depth_enabled;
	if (alpha_micro)
	   depth = alpha_micro_depth;
	if (ph_env("PARHELIA_TRACE_CPU_ALPHA") && textured && ctx->blend &&
	    ctx->blend->rt[0].blend_enable && ctx->num_sampler_views &&
	    ctx->sampler_views[0] && ctx->sampler_views[0]->texture) {
		const struct parhelia_resource *trace_texture =
			ph_resource(ctx->sampler_views[0]->texture);
		if (trace_texture->base.width0 <= 128 &&
		    trace_texture->base.height0 <= 128 &&
		    ctx->trace_cpu_alpha_stage_resource != trace_texture) {
			const float *p = pos >= 0 ? prim->v[0]->data[pos] : NULL;
			ctx->trace_cpu_alpha_stage_resource = trace_texture;
			fprintf(stderr,
				"parhelia: alpha stage tex=%ux%u two-pass=%u micro=%u pos=%g,%g blend=%u/%u,%u alpha=%u/%u,%u mip=%u alpha-test=%u\\n",
				trace_texture->base.width0, trace_texture->base.height0,
				rgb_alpha_two_pass, alpha_micro, p ? p[0] : -1.0f, p ? p[1] : -1.0f,
				ctx->blend->rt[0].rgb_func,
				ctx->blend->rt[0].rgb_src_factor,
				ctx->blend->rt[0].rgb_dst_factor,
				ctx->blend->rt[0].alpha_func,
				ctx->blend->rt[0].alpha_src_factor,
				ctx->blend->rt[0].alpha_dst_factor,
				ctx->samplers[0] ? ctx->samplers[0]->min_mip_filter : 0,
				ctx->dsa ? ctx->dsa->alpha_enabled : 0);
		}
	}
	if (ph_env("PARHELIA_TRACE_ALPHA_SOURCE") && textured && ctx->blend &&
	    ctx->blend->rt[0].blend_enable && ctx->blend->rt[0].rgb_func == PIPE_BLEND_ADD &&
	    ctx->blend->rt[0].rgb_src_factor == PIPE_BLENDFACTOR_SRC_ALPHA &&
	    ctx->blend->rt[0].rgb_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA &&
	    ctx->sampler_views[0] && ctx->sampler_views[0]->texture) {
		const struct parhelia_resource *trace_texture =
			ph_resource(ctx->sampler_views[0]->texture);
		if (ctx->trace_alpha_source_resource != trace_texture) {
			const float *p = pos >= 0 ? prim->v[0]->data[pos] : NULL;
			ctx->trace_alpha_source_resource = trace_texture;
			fprintf(stderr,
				"parhelia: alpha-source tex=%ux%u defined=%#x hash=%#llx pos=%g,%g z=%u/%u\\n",
				trace_texture->base.width0, trace_texture->base.height0,
				trace_texture->mip_defined,
				(unsigned long long)ph_texture_base_hash(trace_texture, false,
									       NULL, NULL),
				p ? p[0] : -1.0f, p ? p[1] : -1.0f,
				ctx->dsa ? ctx->dsa->depth_enabled : 0,
				ctx->dsa ? ctx->dsa->depth_writemask : 0);
		}
	}
	dpv = textured ? ((multitextured || native_sampled_alpha) ?
	                                 (depth ? 10 : 9) :
	                                  (depth ? 8 : 7)) :
	                                  (depth ? 6 : 5);
	if (vertex_alpha)
	   dpv++;
	/* A fragment program that reads the second colour - a lowered GL fog
	 * expression is the one that does - needs the kind-6 attribute and the
	 * program that forwards it to output 2.  Untextured draws only.
	 *
	 * It rides with the fragment compiler, because the carrier is only
	 * useful when the compiled combiner is the thing that reads v1; with
	 * the fixed-function combiner it would widen the stream for nothing.
	 * PARHELIA_NO_FOG_COLOUR1 turns it off on its own. */
	/* The original family also carries it in a one-set textured stream
	 * (separate specular), x,y[,z,1/w],rgb,rgb1,st. */
	textured_colour1 = textured && !multitextured && !three_coord &&
	                   !native_sampled_alpha && !rgb_alpha_two_pass &&
	                   !alpha_micro && !alpha_micro_cutout &&
	                   !cpu_alpha_cutout &&
	                   ph_original_family(ph_screen(ctx->base.screen)->chip_id);
	colour1 = !ph_env("PARHELIA_FS_TEXENV") &&
	          !ph_env("PARHELIA_NO_FOG_COLOUR1") &&
	          (!textured || textured_colour1) && !vertex_alpha && ctx->fs &&
	          ctx->fs->program.valid && ctx->fs->program.uses_color1 &&
	          (ctx->fs->program.uses_fogc ? fogc : col1) >= 0;
	if (colour1 && !(textured ? ctx->warp_colour1_tex : ctx->warp_colour1)) {
	   /* Uploading a program drains the engine, so never do it with
	    * vertices already staged: the batch under construction would be
	    * submitted around the stall and the draws after it read stale
	    * state. */
	   if (ctx->triangles || ctx->points || ctx->lines) {
	      ph_submit_vertices(ctx);
	      if (ctx->submit_error)
	         return;
	   }
	   colour1 = textured ? ph_colour1_textured_program(ctx) :
	                        ph_colour1_program(ctx);
	}
	/* An alpha the program reads widens that colour set to four
	 * components. */
	if (colour1) {
	   colour1 = 1 | (ctx->fs->program.uses_color1_alpha &&
	                  !ctx->fs->program.uses_fogc ? 2 : 0) |
	             (ctx->fs->program.uses_color0_alpha ? 4 : 0);
	   dpv += 3 + !!(colour1 & 2) + !!(colour1 & 4);
	}
	if (ph_env("PARHELIA_TRACE_FOG"))
	   fprintf(stderr,
	           "parhelia: fog carrier want=%#x fs=%p valid=%u uses_v1=%u fogc=%d textured=%u entry=%#x\n",
	           colour1, (void *)ctx->fs,
	           ctx->fs ? ctx->fs->program.valid : 0,
	           ctx->fs ? ctx->fs->program.uses_color1 : 0, fogc, textured,
	           ctx->warp_colour1);
	/* An untextured colour with a visible alpha takes the measured
	 * x,y,z,r,g,b,a stream; z rides along without a depth surface. */
	colour_alpha = !textured && !vertex_alpha && !colour1 &&
	               ph_colour_alpha_observable(ctx);
	if (colour_alpha && !ctx->warp_rgba &&
	    ph_t2_free_rgba(ph_screen(ctx->base.screen)->chip_id)) {
	   /* Uploading drains the engine: publish the staged batch first. */
	   if (ctx->triangles || ctx->points || ctx->lines) {
	      ph_submit_vertices(ctx);
	      if (ctx->submit_error)
	         return;
	   }
	   colour_alpha = ph_rgba_program(ctx);
	}
	if (colour_alpha)
	   dpv = 7;
	if (cpu_alpha_cutout || rgb_alpha_two_pass)
	   dpv = 10; /* Internal XYZOOW,RGBA,ST; packed to 7/8 GPU dwords later. */
	/* The textured stream carries z and 1/w, so the setup unit interpolates
	 * perspective-correctly (ph-glprobe perspective, exact on the 0528). */
	/* PARHELIA_NO_OOW=1 (diagnostic) keeps the affine stream. */
	/* The stipple's window coordinates interpolate linearly. */
	oow = textured && !ctx->fs->stipple_affine &&
	      !ph_fs_window_lookups_only(ctx->fs) &&
	      !native_sampled_alpha &&
	      !cpu_alpha_cutout && !rgb_alpha_two_pass && !alpha_micro &&
	      !alpha_micro_cutout && !three_coord && !ph_env("PARHELIA_NO_OOW");
	if (oow)
	   dpv = (multitextured ? 10 : 8) + 1 + !!vertex_alpha +
	         (colour1 ? 3 + !!(colour1 & 2) + !!(colour1 & 4) : 0);
	/* Original-family units 2 and 3 add their coordinate sets. */
	extra = multitextured ? ph_extra_units(ctx) : 0;
	if (extra && !ctx->warp_four_sets) {
	   /* the upload drains the engine: publish the staged batch first */
	   if (ctx->triangles || ctx->points || ctx->lines) {
	      ph_submit_vertices(ctx);
	      if (ctx->submit_error)
	         return;
	   }
	   if (!ph_four_set_program(ctx))
	      extra = 0;
	}
	dpv += 2 * extra;
	/* A projector other than one switches unit 0 to the per-fragment
	 * divide, which reads q from a four-component coordinate set. */
	proj = (three_coord ? !ph_resource(ctx->sampler_views[0]->texture)->mip.cube :
	                      oow && !multitextured && !vertex_alpha) &&
	       (ctx->fs->projective_mask & BITFIELD_BIT(0)) &&
	       ph_primitive_projective(ctx, prim);
	/* A volume's q takes the stream's spare dword. */
	if (proj && !three_coord)
	   dpv += 2;
	/* A state setter normally drains a batch before reaching this point.  The
	 * extra guard keeps a future draw-stage change from mixing 7- and 8-dword
	 * streams if the exact blend predicate changes within a primitive list. */
	if (ctx->triangles && (ctx->colour1 != colour1 ||
	                       ctx->extra_units != extra ||
	                       ctx->oow != oow || ctx->proj != proj ||
	                       ctx->colour_alpha != colour_alpha ||
	                       ctx->line_coverage != ctx->ov_valid ||
	                       ctx->vertex_alpha != vertex_alpha ||
	                       ctx->alpha_micro != alpha_micro ||
	                       ctx->alpha_micro_cutout != alpha_micro_cutout ||
	                       ctx->cpu_alpha_cutout != cpu_alpha_cutout ||
	                       ctx->rgb_alpha_two_pass != rgb_alpha_two_pass ||
	                       ctx->native_sampled_alpha !=
	                          native_sampled_alpha))
	   ph_submit_vertices(ctx);

   /* Bound the complete logical stream to one negotiated CREATE_VERTEX slot.
    * The descriptor layout still matters: a depth-textured triangle consumes
    * 24 dwords, while each individual fetch is checked independently. */
   if (ctx->triangles == ph_triangle_batch_limit(ctx) ||
       (ctx->triangles + 1) * 3 * dpv > ph_logical_vertex_limit(ctx))
      ph_submit_vertices(ctx);
   if (textured) {
      /* The fixed-function vertex path exports the texture varyings directly.
       * Never ask draw for an extra attribute here: an extra attribute is an
       * output slot owned and initialized by a draw stage, not a copy of ST.
       * NIR-to-TGSI uses GENERIC by default, but honor screens which retain
       * TEXCOORD semantics too. */
      texcoord = ph_unit_texcoord(ctx, 0);
      if (multitextured)
         texcoord1 = ph_unit_texcoord(ctx, 1);
      for (unsigned i = 0; i < extra; i++)
         texcoord_x[i] = ph_unit_texcoord(ctx, 2 + i);
   }
   /* A program that samples only unit 1 - GL1's unit-1 REPLACE - leaves
    * unit 0 without a coordinate set, and the draw was dropped.  Stage unit
    * 1's set in its place; unit 0's sample is not read. */
   if (textured && texcoord < 0 && texcoord1 >= 0 && ctx->fs &&
       ctx->fs->program.valid &&
       !(ctx->fs->program.sampler_mask & BITFIELD_BIT(0)))
      texcoord = texcoord1;
   if (textured && texcoord < 0 && ph_env("PARHELIA_TRACE")) {
      const struct tgsi_shader_info *info = draw_get_shader_info(stage->draw);
      fprintf(stderr, "parhelia: no real texture varying; VS has %u outputs\n",
              info->num_outputs);
      for (unsigned i = 0; i < info->num_outputs; i++)
         fprintf(stderr, "parhelia: vs output %u semantic=%u/%u\n", i,
                 info->output_semantic_name[i], info->output_semantic_index[i]);
   }
   if (ctx->submit_error || pos < 0 || (textured && texcoord < 0) ||
       (multitextured && texcoord1 < 0) ||
       (extra > 0 && texcoord_x[0] < 0) || (extra > 1 && texcoord_x[1] < 0))
      return;
	ctx->colour1 = colour1;
	ctx->oow = oow;
	ctx->extra_units = extra;
	ctx->proj = proj;
	ctx->colour_alpha = colour_alpha;
	ctx->line_coverage = ctx->ov_valid;
	ctx->vertex_alpha = vertex_alpha;
	ctx->alpha_micro = alpha_micro;
	ctx->alpha_micro_cutout = alpha_micro_cutout;
	ctx->cpu_alpha_cutout = cpu_alpha_cutout;
	ctx->rgb_alpha_two_pass = rgb_alpha_two_pass;
	ctx->native_sampled_alpha = native_sampled_alpha;
	if (alpha_micro || alpha_micro_cutout) {
	   ph_stage_alpha_micro(ctx, prim, pos, col, texcoord, depth,
	                        alpha_micro_cutout);
	   return;
	}
   if (multitextured && !ctx->trace_multitex_emitted &&
       ph_env("PARHELIA_TRACE")) {
      const float *st0 = prim->v[0]->data[texcoord];
      const float *st1 = prim->v[0]->data[texcoord1];
      fprintf(stderr,
              "parhelia: multitex real VS slots st0=%d (%g,%g) st1=%d (%g,%g)\\n",
              texcoord, st0[0], st0[1], texcoord1, st1[0], st1[1]);
      ctx->trace_multitex_emitted = true;
   }
   dst = &ctx->vertices[ctx->triangles * 3 * dpv];
   for (unsigned v = 0; v < 3; v++) {
      const float *p = prim->v[v]->data[pos];
      const float white[4] = {1, 1, 1, 1};
      const float *c = ctx->ov_valid ? ctx->ov_colour[v] :
                       col < 0 ? white : prim->v[v]->data[col];
		if (cpu_alpha_cutout || rgb_alpha_two_pass) {
			const float *tc = prim->v[v]->data[texcoord];
			*dst++ = p[0];
			*dst++ = p[1];
			*dst++ = CLAMP(p[2], 0.0f, 1.0f);
			*dst++ = p[3];
			*dst++ = c[0];
			*dst++ = c[1];
			*dst++ = c[2];
			*dst++ = c[3];
			*dst++ = tc[0];
			*dst++ = tc[1];
			continue;
		}
		if (native_sampled_alpha) {
			const struct parhelia_resource *texture =
			   ph_resource(ctx->sampler_views[0]->texture);
			const float *tc = prim->v[v]->data[texcoord];
			/* Scale by the companion actually allocated, not by a
			 * recomputed padding extent: the two disagree whenever
			 * the companion was built unpadded, and the alpha is
			 * then sampled over the wrong extent - which reads as
			 * half a sprite missing rather than as a wrong
			 * texture. */
			float scale_s = ph_alpha_companion_scale(
			   texture->base.width0,
			   texture->alpha_companion_width);
			float scale_t = ph_alpha_companion_scale(
			   texture->base.height0,
			   texture->alpha_companion_height);

			*dst++ = p[0];
			*dst++ = p[1];
			if (depth)
				*dst++ = CLAMP(p[2], 0.0f, 1.0f);
			*dst++ = c[0];
			*dst++ = c[1];
			*dst++ = c[2];
			*dst++ = tc[0];
			*dst++ = tc[1];
			*dst++ = tc[0] * scale_s;
			*dst++ = tc[1] * scale_t;
			continue;
		}
		if (three_coord) {
			const float *tc = prim->v[v]->data[texcoord];

			*dst++ = p[0];
			*dst++ = p[1];
			if (depth)
				*dst++ = CLAMP(p[2], 0.0f, 1.0f);
			*dst++ = tc[0];
			*dst++ = tc[1];
			*dst++ = tc[2];
			/* The stream keeps its two-coordinate width; the spare
			 * dwords carry q for a projective volume, else nothing. */
			*dst++ = proj ? tc[3] : 0.0f;
			*dst++ = 0.0f;
			continue;
		}
      *dst++ = p[0];
      *dst++ = p[1];
      if (depth || oow || colour_alpha)
         *dst++ = CLAMP(p[2], 0.0f, 1.0f);
      if (oow)
         *dst++ = p[3];
		*dst++ = c[0];
		*dst++ = c[1];
		*dst++ = c[2];
		if (vertex_alpha || colour_alpha || (colour1 & 4))
			*dst++ = c[3];
      if (colour1 && ctx->fs->program.uses_fogc) {
         /* The fog coordinate, broadcast: the combiner reads v1 as a colour
          * and the lowered expression selects one channel of it. */
         const float f = ctx->fs->program.fog ?
            ph_fs_fog_eval(&ctx->fs->program, prim->v[v]->data[fogc][0],
                           ctx->fs_const_snap, ctx->fs_const_snap_size) :
            prim->v[v]->data[fogc][0];

         *dst++ = f;
         *dst++ = f;
         *dst++ = f;
      } else if (colour1) {
         const float *s = prim->v[v]->data[col1];

         *dst++ = s[0];
         *dst++ = s[1];
         *dst++ = s[2];
         if (colour1 & 2)
            *dst++ = s[3];
      }
      if (textured) {
         const float *tc = prim->v[v]->data[texcoord];
         *dst++ = tc[0];
         *dst++ = tc[1];
         if (proj) {
            *dst++ = tc[2];
            *dst++ = tc[3];
         }
         if (multitextured) {
            const float *tc1 = prim->v[v]->data[texcoord1];
            *dst++ = tc1[0];
            *dst++ = tc1[1];
            for (unsigned i = 0; i < extra; i++) {
               const float *tcx = prim->v[v]->data[texcoord_x[i]];

               *dst++ = tcx[0];
               *dst++ = tcx[1];
            }
         }
      }
   }
   ctx->triangles++;
}

/* Emit a window-space polygon as a fan through the triangle stage.  Corner i
 * copies every attribute of src[i] and replaces only its x/y; a flat-shaded
 * polygon takes all colours from @provoking. */
static void
ph_stage_fan(struct draw_stage *stage, const struct vertex_header *const *src,
             const float (*xy)[2], unsigned n,
             const struct vertex_header *provoking)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   const int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   const int col = ph_v_output(stage->draw, ctx, 0);
   struct prim_header tri = {0};

   for (unsigned i = 1; i + 1 < n; i++) {
      const unsigned corner[3] = {0, i, i + 1};

      for (unsigned k = 0; k < 3; k++) {
         struct vertex_header *v = dup_vert(stage, src[corner[k]], k);

         v->data[pos][0] = xy[corner[k]][0];
         v->data[pos][1] = xy[corner[k]][1];
         if (provoking && col >= 0)
            memcpy(v->data[col], provoking->data[col], 4 * sizeof(float));
         tri.v[k] = v;
      }
      ph_stage_tri(stage, &tri);
      if (ctx->submit_error)
         return;
   }
}

/* GL centres an odd aliased point on floor(v) + 1/2 and an even one on
 * floor(v + 1/2) in bottom-up window space; a flipped axis mirrors the ties. */
static float
ph_point_centre(float v, bool odd, bool flip)
{
   if (flip)
      return odd ? ceilf(v) - 0.5f : ceilf(v - 0.5f);
   return odd ? floorf(v) + 0.5f : floorf(v + 0.5f);
}

/* Aliased points are integer squares, odd sizes centred on a pixel centre
 * and even sizes on a pixel corner.  Smooth points become an aliased disc
 * polygon: the open path has no coverage antialiasing. */
static void
ph_expand_point(struct draw_stage *stage, struct prim_header *prim,
                float size, bool smooth)
{
   const struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   const struct pipe_caps *caps = &ctx->base.screen->caps;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   const int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   const float *p = prim->v[0]->data[pos];
   const struct vertex_header *src[32];
   float xy[32][2];
   unsigned n;

   if (!smooth) {
      const float s = CLAMP(roundf(size), caps->min_point_size,
                            caps->max_point_size);
      /* Integer pixel centres when half_pixel_center is off. */
      const float bias = rast && !rast->half_pixel_center ? 0.5f : 0.0f;
      const bool odd = (unsigned)s & 1;
      const float cx = ph_point_centre(p[0] + bias, odd, false);
      const float cy = ph_point_centre(p[1] + bias, odd, ctx->viewport_y_flip);
      const float h = 0.5f * s;

      xy[0][0] = cx - h - bias; xy[0][1] = cy - h - bias;
      xy[1][0] = cx + h - bias; xy[1][1] = cy - h - bias;
      xy[2][0] = cx + h - bias; xy[2][1] = cy + h - bias;
      xy[3][0] = cx - h - bias; xy[3][1] = cy + h - bias;
      n = 4;
   } else {
      /* Rasterization clamps a derived size to the advertised range. */
      const float r = 0.5f * CLAMP(size, caps->min_point_size_aa,
                                   caps->max_point_size_aa);

      n = r < 2.0f ? 8 : r < 8.0f ? 16 : 32;
      for (unsigned i = 0; i < n; i++) {
         const float a = 2.0f * (float)M_PI * i / n;

         xy[i][0] = p[0] + r * cosf(a);
         xy[i][1] = p[1] + r * sinf(a);
      }
   }
   for (unsigned i = 0; i < n; i++)
      src[i] = prim->v[0];
   ph_stage_fan(stage, src, (const float (*)[2])xy, n, NULL);
}

static bool ph_fs_constant_colour(struct ph_context *ctx, float out[4]);

/* The colour a smooth line's fragments get, when it can be staged as the
 * final colour: an untextured program that is a constant or the vertex
 * colour itself.  The alpha has to reach the blender or the alpha test. */
static bool
ph_line_coverage_colours(struct ph_context *ctx, const struct prim_header *prim,
                         int col, float out[2][4])
{
   const struct ph_fs *fs = ctx->fs;

   if (!fs || !fs->program.valid || fs->program.sampler_mask ||
       fs->program.uses_color1 || ph_samples_unit0(ctx) ||
       !ph_colour_alpha_observable(ctx))
      return false;
   if (!fs->program.uses_color) {
      if (!ph_fs_constant_colour(ctx, out[0]))
         return false;
      memcpy(out[1], out[0], sizeof(out[0]));
      return true;
   }
   if (fs->program.cls != PH_FS_CLASS_COLOUR || col < 0)
      return false;
   for (unsigned v = 0; v < 2; v++)
      memcpy(out[v], prim->v[v]->data[col], sizeof(out[v]));
   if (ctx->rasterizer && ctx->rasterizer->flatshade) {
      const unsigned pv = ctx->rasterizer->flatshade_first ? 0 : 1;

      memcpy(out[!pv], out[pv], sizeof(out[0]));
   }
   return true;
}

/* One quad of a smooth line, corners in order, each with its own colour. */
static void
ph_stage_coverage_quad(struct draw_stage *stage, struct prim_header *prim,
                       const float (*xy)[2], const float (*rgba)[4])
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   const int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   static const unsigned corner[2][3] = { {0, 1, 2}, {0, 2, 3} };
   static const unsigned from[4] = { 0, 1, 1, 0 };

   ctx->ov_valid = true;
   for (unsigned t = 0; t < 2 && !ctx->submit_error; t++) {
      struct prim_header tri = {0};

      for (unsigned k = 0; k < 3; k++) {
         const unsigned c = corner[t][k];
         struct vertex_header *v = dup_vert(stage, prim->v[from[c]], k);

         v->data[pos][0] = xy[c][0];
         v->data[pos][1] = xy[c][1];
         memcpy(ctx->ov_colour[k], rgba[c], sizeof(rgba[c]));
         tri.v[k] = v;
      }
      ph_stage_tri(stage, &tri);
   }
   ctx->ov_valid = false;
}

/* A smooth line as coverage in alpha.  Across it, a full-alpha core of
 * width w - 1 and a one-pixel ramp to zero on either side, so each column
 * sums to the width (GL's area coverage under a tent); along it, the same
 * one-pixel ramp centred on each end, which is a pixel's box overlap with
 * the end - what a stippled smooth line's dashes need. */
static bool
ph_coverage_line(struct draw_stage *stage, struct prim_header *prim,
                 float width)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   const int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   const int col = ph_v_output(stage->draw, ctx, 0);
   const float *p0 = prim->v[0]->data[pos], *p1 = prim->v[1]->data[pos];
   const float dx = p1[0] - p0[0], dy = p1[1] - p0[1];
   const float len = sqrtf(dx * dx + dy * dy);
   const float h = 0.5f * CLAMP(width, 1.0f, 2048.0f);
   const float inner = h - 0.5f, outer = h + 0.5f;
   float c[2][4], along[4], across[4], a_al[4], a_ac[4];
   unsigned na, nc;

   if (len <= 0.0f || !ph_line_coverage_colours(ctx, prim, col, c))
      return false;
   /* Stations along the line (from p0) and across it, with their coverage:
    * along, a pixel's overlap with [0, len], a trapezoid. */
   {
      const float lo = MIN2(0.5f, len - 0.5f), hi = MAX2(0.5f, len - 0.5f);
      const float st[4] = { -0.5f, lo, hi, len + 0.5f };
      const float m = MIN2(len, 1.0f);
      const float al[4] = { 0.0f, m, m, 0.0f };

      memcpy(along, st, sizeof(st));
      memcpy(a_al, al, sizeof(al));
      na = hi > lo ? 4 : 3;
      if (na == 3) {
         along[2] = along[3];
         a_al[2] = 0.0f;
      }
   }
   if (inner > 0.0f) {
      const float st[4] = { -outer, -inner, inner, outer };
      const float ac[4] = { 0.0f, 1.0f, 1.0f, 0.0f };

      memcpy(across, st, sizeof(st));
      memcpy(a_ac, ac, sizeof(ac));
      nc = 4;
   } else {
      const float st[3] = { -outer, 0.0f, outer };
      const float ac[3] = { 0.0f, 1.0f, 0.0f };

      memcpy(across, st, sizeof(st));
      memcpy(a_ac, ac, sizeof(ac));
      nc = 3;
   }
   const float ux = dx / len, uy = dy / len, nx = -uy, ny = ux;

   for (unsigned i = 0; i + 1 < na; i++) {
      for (unsigned j = 0; j + 1 < nc; j++) {
         static const unsigned di[4] = { 0, 1, 1, 0 }, dj[4] = { 0, 0, 1, 1 };
         float xy[4][2], rgba[4][4];

         for (unsigned k = 0; k < 4; k++) {
            const float sa = along[i + di[k]], sc = across[j + dj[k]];
            const float t = CLAMP(sa / len, 0.0f, 1.0f);

            xy[k][0] = p0[0] + ux * sa + nx * sc;
            xy[k][1] = p0[1] + uy * sa + ny * sc;
            for (unsigned ch = 0; ch < 4; ch++)
               rgba[k][ch] = c[0][ch] + t * (c[1][ch] - c[0][ch]);
            rgba[k][3] *= a_al[i + di[k]] * a_ac[j + dj[k]];
         }
         ph_stage_coverage_quad(stage, prim, (const float (*)[2])xy,
                                (const float (*)[4])rgba);
      }
   }
   return true;
}

/* Aliased lines use the draw module's wide-line quad (minor-axis offset and
 * half-pixel end adjustment); smooth lines use the vendor's perpendicular
 * +-width/2 displacement. */
static void
ph_expand_line(struct draw_stage *stage, struct prim_header *prim,
               float width, bool smooth)
{
   const struct pipe_rasterizer_state *rast = ((struct ph_stage *)stage)->ctx->rasterizer;
   const int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   const float *p0 = prim->v[0]->data[pos], *p1 = prim->v[1]->data[pos];
   const struct vertex_header *src[4] = {
      prim->v[0], prim->v[1], prim->v[1], prim->v[0],
   };
   const struct vertex_header *provoking = NULL;
   const float dx = p1[0] - p0[0], dy = p1[1] - p0[1];
   float xy[4][2] = {
      {p0[0], p0[1]}, {p1[0], p1[1]}, {p1[0], p1[1]}, {p0[0], p0[1]},
   };
   float ox = 0.0f, oy = 0.0f;

   if (rast && rast->flatshade)
      provoking = rast->flatshade_first ? prim->v[0] : prim->v[1];
   if (smooth && ph_coverage_line(stage, prim, width))
      return;
   if (smooth) {
      const float len = sqrtf(dx * dx + dy * dy);
      const float h = 0.5f * CLAMP(width, 1.0f, 2048.0f);

      if (len <= 0.0f)
         return;
      ox = -dy / len * h;
      oy = dx / len * h;
   } else {
      const float h = 0.5f * CLAMP(roundf(width), 1.0f, 2048.0f);
      const bool half = rast && rast->half_pixel_center;
      /* The original family's native lines follow GL's diamond rule
       * (MEASURED): slide the ends along the line, not the whole line. */
      const bool exact = ph_original_family(
         ph_screen(((struct ph_stage *)stage)->ctx->base.screen)->chip_id);
      const float bias = half && !exact ? 0.125f : 0.0f;

      if (fabsf(dx) > fabsf(dy)) {
         const float shift = !half ? 0.0f : dx > 0.0f ? -0.5f : 0.5f;

         oy = h;
         for (unsigned i = 0; i < 4; i++) {
            xy[i][0] += shift;
            xy[i][1] += exact ? shift * dy / dx : -bias;
         }
      } else {
         const float shift = !half ? 0.0f : dy > 0.0f ? -0.5f : 0.5f;

         ox = h;
         for (unsigned i = 0; i < 4; i++) {
            xy[i][0] += exact ? shift * dx / dy : bias;
            xy[i][1] += shift;
         }
      }
   }
   xy[0][0] -= ox; xy[0][1] -= oy;
   xy[1][0] -= ox; xy[1][1] -= oy;
   xy[2][0] += ox; xy[2][1] += oy;
   xy[3][0] += ox; xy[3][1] += oy;
   ph_stage_fan(stage, src, (const float (*)[2])xy, 4, provoking);
}

/* The vendor's GL capture proves the complete unit-point draw delta from a
 * triangle: class 0, primitive word 0x00060020, raster image 0x04000024 and a
 * one-vertex byte range.  Its own client produces exactly one covered pixel.
 * Sized, smooth, per-vertex-size and textured points are expanded to
 * triangles; sprites are expanded by the draw module. */
static void
ph_stage_point(struct draw_stage *stage, struct prim_header *prim)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   if (!ph_routed_in_range(stage->draw, ctx,
                           (const struct vertex_header *const *)prim->v, 1))
      return;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   float *dst, size = rast ? rast->point_size : 1.0f;
   int pos, col;
   bool has_texture;
   bool depth;
   unsigned dpv;

   if (ctx->submit_error || !ctx->fb.nr_cbufs ||
       !ctx->fb.cbufs[0].texture)
      return;

   pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   col = ph_v_output(stage->draw, ctx, 0);
   if (pos < 0)
      return;
   if (rast && rast->point_size_per_vertex) {
      const int psize = draw_find_shader_output(stage->draw,
                                                TGSI_SEMANTIC_PSIZE, 0);

      if (psize >= 0)
         size = prim->v[0]->data[psize][0];
   }
   has_texture = ph_samples_unit0(ctx);
   if (ph_env("PARHELIA_TRACE_PRIM"))
      fprintf(stderr, "parhelia: point %g,%g size=%g per-vertex=%u tex=%u smooth=%u cull=%u derived=%u\n",
              prim->v[0]->data[pos][0], prim->v[0]->data[pos][1], size,
              rast ? rast->point_size_per_vertex : 0, has_texture,
              rast ? rast->point_smooth : 0, rast ? rast->cull_face : 0,
              ctx->derived_triangles);
   /* The vendor's ICD splits on a plain inequality against 1.0, with no
    * rounding: glPointSize(1.2) takes the wide path there while roundf()
    * kept it on the unit-point one (parhelia-point-size-re.md section 11). */
   const bool wide = size != 1.0f;
   const bool native_wide = wide && !has_texture &&
                            !(rast && (rast->point_smooth ||
                                       rast->point_quad_rasterization)) &&
                            ph_sized_point_program(ctx);

   /* As for lines, a visible alpha takes the triangle expansion. */
   if (has_texture || (wide && !native_wide) ||
       ph_colour_alpha_observable(ctx) || ph_fs_reads_v1(ctx) ||
       (rast && (rast->point_smooth || rast->point_quad_rasterization))) {
      ph_expand_point(stage, prim, size, rast && rast->point_smooth);
      return;
   }
   /* The radius rides in one constant, so a batch carries one size. */
   if (native_wide) {
      const float radius = 0.5f * CLAMP(size, 1.0f, 2048.0f);

      if (ctx->points && ctx->point_radius != radius) {
         ph_submit_vertices(ctx);
         if (ctx->submit_error)
            return;
      }
      ctx->point_radius = radius;
   } else if (ctx->points && ctx->point_radius != 0.0f) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return;
      ctx->point_radius = 0.0f;
   } else {
      ctx->point_radius = 0.0f;
   }

   if (ctx->triangles || ctx->lines) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return;
   }
   depth = ctx->fb.zsbuf.texture != NULL;
   dpv = depth ? 6 : 5;
   if (ctx->points == ph_triangle_batch_limit(ctx) ||
       (ctx->points + 1) * dpv > ph_logical_vertex_limit(ctx)) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return;
   }
   dst = &ctx->vertices[ctx->points * dpv];

   const float *p = prim->v[0]->data[pos];
   const float white[4] = {1, 1, 1, 1};
   const float *c = col < 0 ? white : prim->v[0]->data[col];

   /* A wide point's corners are the staged centre plus and minus the radius,
    * so the centre has to be snapped here exactly as the expansion path
    * snaps it: an even size centres on a pixel corner, an odd one on a pixel
    * centre.  Without this a point at a half-pixel centre lands a pixel off. */
   if (ctx->point_radius > 0.0f) {
      const float s = 2.0f * ctx->point_radius;
      const float bias = rast && !rast->half_pixel_center ? 0.5f : 0.0f;
      const bool odd = (unsigned)s & 1;

      *dst++ = ph_point_centre(p[0] + bias, odd, false) - bias;
      *dst++ = ph_point_centre(p[1] + bias, odd, ctx->viewport_y_flip) - bias;
   } else {
      *dst++ = p[0];
      *dst++ = p[1];
   }
   if (depth)
      *dst++ = CLAMP(p[2], 0.0f, 1.0f);
   *dst++ = c[0];
   *dst++ = c[1];
   *dst++ = c[2];
   ctx->points++;
}

/* The draw module has already transformed and clipped both endpoints when it
 * reaches this stage.  A unit-width line therefore uses the same window-space
 * passthrough WARP program and attribute layout as an untextured triangle;
 * only the primitive class and the two-vertex DRAW byte range differ.  The
 * vendor GL capture confirms exactly that delta (0x10b0 = 0x0006103d).
 *
 * Textured and smooth lines are expanded to triangles: their fragment and
 * coverage semantics on the native line primitive are unproven.  Aliased
 * lines of width 1.5 and more arrive already expanded by the draw module. */
/* A line end the clipper made on the viewport edge is not where the line
 * ends: GL draws the edge pixel the line passes through.  Move such an end
 * one pixel further along the line, extrapolating every attribute, and let
 * the screen bounds trim the overhang.  Clipped vertices carry no vertex id
 * (draw_pipe_clip.c). */
static struct prim_header *
ph_extend_clipped_line(struct draw_stage *stage, struct prim_header *prim,
                       int pos, struct prim_header *out)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   const float *t = ctx->viewport.translate, *sc = ctx->viewport.scale;
   const float *p0 = prim->v[0]->data[pos], *p1 = prim->v[1]->data[pos];
   const float dx = p1[0] - p0[0], dy = p1[1] - p0[1];
   const float major = MAX2(fabsf(dx), fabsf(dy));
   const unsigned attrs = draw_num_shader_outputs(stage->draw);
   bool any = false;

   if (major <= 0.0f)
      return prim;
   *out = *prim;
   for (unsigned e = 0; e < 2; e++) {
      const struct vertex_header *v = prim->v[e];
      const float *p = v->data[pos];
      const bool edge =
         fabsf(p[0] - (t[0] - fabsf(sc[0]))) < 1e-3f ||
         fabsf(p[0] - (t[0] + fabsf(sc[0]))) < 1e-3f ||
         fabsf(p[1] - (t[1] - fabsf(sc[1]))) < 1e-3f ||
         fabsf(p[1] - (t[1] + fabsf(sc[1]))) < 1e-3f;
      struct vertex_header *x;
      float k;

      if (v->vertex_id != UNDEFINED_VERTEX_ID || !edge)
         continue;
      /* One pixel along the major axis, away from the other end. */
      k = 1.0f / major;
      x = dup_vert(stage, v, 3 + e);
      for (unsigned a = 0; a < attrs; a++)
         for (unsigned c = 0; c < 4; c++)
            x->data[a][c] = v->data[a][c] +
               k * (v->data[a][c] - prim->v[!e]->data[a][c]);
      out->v[e] = x;
      any = true;
   }
   return any ? out : prim;
}

static void
ph_stage_line(struct draw_stage *stage, struct prim_header *prim)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   if (!ph_routed_in_range(stage->draw, ctx,
                           (const struct vertex_header *const *)prim->v, 2))
      return;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   float *dst;
   int pos, col;
   bool has_texture;
   bool depth;
   unsigned dpv;

   if (ctx->submit_error || !ctx->fb.nr_cbufs ||
       !ctx->fb.cbufs[0].texture)
      return;

   pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   col = ph_v_output(stage->draw, ctx, 0);
   if (pos < 0)
      return;
   struct prim_header extended;
   prim = ph_extend_clipped_line(stage, prim, pos, &extended);
   has_texture = ph_samples_unit0(ctx);
   if (ph_env("PARHELIA_TRACE_PRIM"))
      fprintf(stderr, "parhelia: line %g,%g-%g,%g width=%g tex=%u smooth=%u cull=%u derived=%u\n",
              prim->v[0]->data[pos][0], prim->v[0]->data[pos][1],
              prim->v[1]->data[pos][0], prim->v[1]->data[pos][1],
              rast ? rast->line_width : 1.0f, has_texture,
              rast ? rast->line_smooth : 0, rast ? rast->cull_face : 0,
              ctx->derived_triangles);
   /* A visible alpha needs the RGBA stream, which the native line
    * primitive does not take (it stopped the P690); triangles do. */
   if (has_texture || ph_colour_alpha_observable(ctx) || ph_fs_reads_v1(ctx) ||
       (rast && (rast->line_smooth || roundf(rast->line_width) > 1.0f))) {
      ph_expand_line(stage, prim, rast ? rast->line_width : 1.0f,
                     rast && rast->line_smooth);
      return;
   }

   /* A pending triangle batch owns the shared staging array.  Publish it once
    * at the primitive-class transition, then retain following line segments. */
   if (ctx->points || ctx->triangles) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return;
   }
   depth = ctx->fb.zsbuf.texture != NULL;
   dpv = depth ? 6 : 5;
   if (ctx->lines == ph_triangle_batch_limit(ctx) ||
       (ctx->lines + 1) * 2 * dpv > ph_logical_vertex_limit(ctx)) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return;
   }
   dst = &ctx->vertices[ctx->lines * 2 * dpv];

   for (unsigned v = 0; v < 2; v++) {
      const float *p = prim->v[v]->data[pos];
      const float white[4] = {1, 1, 1, 1};
      const float *c = col < 0 ? white : prim->v[v]->data[col];

      *dst++ = p[0];
      *dst++ = p[1];
      if (depth)
         *dst++ = CLAMP(p[2], 0.0f, 1.0f);
      *dst++ = c[0];
      *dst++ = c[1];
      *dst++ = c[2];
   }
   ctx->lines++;
}
static void ph_stage_flush(struct draw_stage *s, unsigned flags)
{
   /* draw_flush() ends the software frontend's draw, but each transformed
    * triangle or line is already copied into ctx->vertices by its stage.
    * Keep compatible DRAW packets together until a packet-state setter,
    * resource operation, explicit pipe flush, or the negotiated packet cap
    * submits them.
    */
   (void)s;
   (void)flags;
}
static void ph_stage_reset(struct draw_stage *s) { (void)s; }
static void ph_stage_destroy(struct draw_stage *s)
{
   draw_free_temp_verts(s);
   FREE(s);
}

static struct draw_stage *
ph_create_stage(struct ph_context *ctx)
{
   struct ph_stage *s = CALLOC_STRUCT(ph_stage);
   if (!s)
      return NULL;
   s->ctx = ctx;
   s->base.draw = ctx->draw;
   s->base.point = ph_stage_point;
   s->base.line = ph_stage_line;
   s->base.tri = ph_stage_tri;
   s->base.flush = ph_stage_flush;
   s->base.reset_stipple_counter = ph_stage_reset;
   s->base.destroy = ph_stage_destroy;
   /* One fan triangle at a time (0..2), plus two extended line ends. */
   if (!draw_alloc_temp_verts(&s->base, 5)) {
      FREE(s);
      return NULL;
   }
   return &s->base;
}

/* Level @src_level of the depth image behind @view, written into colour
 * level @dst_level of @dst as the view's swizzle of (z, 0, 0, 1).  The
 * sampler has no depth texel format the driver can program, so this is how
 * GL_DEPTH_TEXTURE_MODE luminance, intensity and alpha reach it. */
static bool
ph_depth_to_colour(struct ph_context *ctx, const struct pipe_sampler_view *view,
                   struct pipe_resource *dst, unsigned dst_level,
                   unsigned src_level, const struct pipe_box *box)
{
   struct pipe_context *p = &ctx->base;
   struct pipe_transfer *st = NULL, *dt = NULL;
   const uint8_t swizzle[4] = { view->swizzle_r, view->swizzle_g,
                                view->swizzle_b, view->swizzle_a };
   const uint8_t *src = p->texture_map(p, view->texture, src_level,
                                       PIPE_MAP_READ, box, &st);
   uint8_t *out = src ? p->texture_map(p, dst, dst_level, PIPE_MAP_WRITE,
                                        box, &dt) : NULL;
   float *z = MALLOC(box->width * sizeof(float));
   bool ok = src && out && z;

   for (int y = 0; ok && y < box->height; y++) {
      util_format_unpack_z_float(view->texture->format, z,
                                 src + (size_t)y * st->stride, box->width);
      for (int x = 0; x < box->width; x++) {
         const float c[6] = { z[x], 0.0f, 0.0f, 1.0f, 0.0f, 1.0f };
         float rgba[4];

         for (unsigned i = 0; i < 4; i++)
            rgba[i] = swizzle[i] <= PIPE_SWIZZLE_1 ? c[swizzle[i]] : 0.0f;
         util_format_pack_rgba(dst->format, out + (size_t)y * dt->stride +
                               x * 4, rgba, 1);
      }
   }
   FREE(z);
   if (dt)
      p->texture_unmap(p, dt);
   if (st)
      p->texture_unmap(p, st);
   return ok;
}

static bool
ph_view_swizzled(const struct pipe_sampler_view *view)
{
   return view->swizzle_r != PIPE_SWIZZLE_X ||
          view->swizzle_g != PIPE_SWIZZLE_Y ||
          view->swizzle_b != PIPE_SWIZZLE_Z ||
          view->swizzle_a != PIPE_SWIZZLE_W;
}

/* Level @src_level of @view's colour image written into level @dst_level of
 * @dst (same format) with the view's swizzle applied: the sampler has no
 * channel swizzle, and st emulates luminance, intensity and alpha formats in
 * RGBA storage through one. */
static bool
ph_swizzle_to_colour(struct ph_context *ctx,
                     const struct pipe_sampler_view *view,
                     struct pipe_resource *dst, unsigned dst_level,
                     unsigned src_level, const struct pipe_box *box)
{
   struct pipe_context *p = &ctx->base;
   struct pipe_transfer *st = NULL, *dt = NULL;
   const uint8_t swizzle[4] = { view->swizzle_r, view->swizzle_g,
                                view->swizzle_b, view->swizzle_a };
   const unsigned cpp = util_format_get_blocksize(dst->format);
   const uint8_t *src = p->texture_map(p, view->texture, src_level,
                                       PIPE_MAP_READ, box, &st);
   uint8_t *out = src ? p->texture_map(p, dst, dst_level, PIPE_MAP_WRITE,
                                        box, &dt) : NULL;
   bool ok = src && out;

   for (int y = 0; ok && y < box->height; y++) {
      for (int x = 0; x < box->width; x++) {
         float c[6] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f }, rgba[4];

         util_format_unpack_rgba(view->format, c,
                                 src + (size_t)y * st->stride + x * cpp, 1);
         for (unsigned i = 0; i < 4; i++)
            rgba[i] = swizzle[i] <= PIPE_SWIZZLE_1 ? c[swizzle[i]] : 0.0f;
         util_format_pack_rgba(view->format,
                               out + (size_t)y * dt->stride + x * cpp,
                               rgba, 1);
      }
   }
   if (dt)
      p->texture_unmap(p, dt);
   if (st)
      p->texture_unmap(p, st);
   return ok;
}

/* Levels first..last of @v's texture copied into a private chain whose
 * level 0 is the view's first level; rebuilt when the texture changes. */
static struct pipe_sampler_view *
ph_shadow_view(struct ph_context *ctx, struct ph_sampler_view *v)
{
   struct parhelia_resource *r = ph_resource(v->base.texture);
   const unsigned first = v->base.u.tex.first_level;
   /* Levels past the target's storage live in host memory (ph_sw_level)
    * and are copied like the others. */
   const unsigned last = MIN2(v->base.u.tex.last_level, r->base.last_level);
   const bool depth = util_format_has_depth(util_format_description(r->base.format));
   const bool swizzled = !depth && ph_view_swizzled(&v->base) &&
      util_format_get_blocksize(v->base.format) ==
      util_format_get_blocksize(r->base.format);

   /* A 16-bit target copies into the 16-bit sampler storage of its format. */
   if ((!depth && util_format_get_blocksize(r->base.format) != 4 &&
        !ph_format_widened(r->base.format) &&
        !ph_sampler_format(r->base.format, NULL)) || first > last)
      return NULL;
   if (!v->shadow_view) {
      struct pipe_resource templ = r->base, *shadow;
      struct pipe_sampler_view view = v->base;

      /* A depth image is sampled as the colour its view's swizzle makes of
       * the depth value (GL_DEPTH_TEXTURE_MODE). */
      if (depth) {
         templ.format = view.format = PIPE_FORMAT_B8G8R8A8_UNORM;
         view.swizzle_r = PIPE_SWIZZLE_X;
         view.swizzle_g = PIPE_SWIZZLE_Y;
         view.swizzle_b = PIPE_SWIZZLE_Z;
         view.swizzle_a = PIPE_SWIZZLE_W;
      }
      if (swizzled) {
         view.swizzle_r = PIPE_SWIZZLE_X;
         view.swizzle_g = PIPE_SWIZZLE_Y;
         view.swizzle_b = PIPE_SWIZZLE_Z;
         view.swizzle_a = PIPE_SWIZZLE_W;
      }
      /* An RGBX view of an RGBA image (texture_from_pixmap of a depth-24
       * pixmap) gets an RGBX copy, so the sampler returns alpha 1. */
      if (!depth && view.format != r->base.format &&
          util_format_rgbx_to_rgba(view.format) == r->base.format)
         templ.format = view.format;
      templ.width0 = u_minify(r->base.width0, first);
      templ.height0 = u_minify(r->base.height0, first);
      templ.last_level = last - first;
      templ.bind = PIPE_BIND_SAMPLER_VIEW;
      shadow = ctx->base.screen->resource_create(ctx->base.screen, &templ);
      if (!shadow)
         return NULL;
      view.u.tex.first_level = 0;
      view.u.tex.last_level = last - first;
      v->shadow_view = ctx->base.create_sampler_view(&ctx->base, shadow,
                                                    &view);
      pipe_resource_reference(&shadow, NULL);
      if (!v->shadow_view)
         return NULL;
      if (depth || swizzled) {
         struct ph_sampler_view *sv = (struct ph_sampler_view *)v->shadow_view;

         sv->baked = true;
         sv->baked_depth = depth;
         sv->baked_swizzle[0] = v->base.swizzle_r;
         sv->baked_swizzle[1] = v->base.swizzle_g;
         sv->baked_swizzle[2] = v->base.swizzle_b;
         sv->baked_swizzle[3] = v->base.swizzle_a;
      }
      v->shadow_generation = ~0ull;
   }
   if (v->shadow_generation != r->content_generation) {
      for (unsigned l = first; l <= last; l++) {
         const unsigned w = u_minify(r->base.width0, l);
         const unsigned h = u_minify(r->base.height0, l);
         const struct pipe_box box = {.width = w, .height = h, .depth = 1};

         if (!(r->mip_defined & BITFIELD_BIT(l)))
            continue;
         if (depth) {
            if (!ph_depth_to_colour(ctx, &v->base, v->shadow_view->texture,
                                    l - first, l, &box))
               return NULL;
            continue;
         }
         if (swizzled) {
            if (!ph_swizzle_to_colour(ctx, &v->base, v->shadow_view->texture,
                                      l - first, l, &box))
               return NULL;
            continue;
         }
         if (!ph_copy_texture_box(ctx, v->shadow_view->texture, l - first,
                                  0, 0, &r->base, l, &box, w, h)) {
            /* Host-only levels go through the transfer paths. */
            util_resource_copy_region(&ctx->base, v->shadow_view->texture,
                                      l - first, 0, 0, 0, &r->base, l, &box);
            if (ctx->submit_error)
               return NULL;
         }
      }
      v->shadow_generation = r->content_generation;
   }
   return v->shadow_view;
}

static bool
ph_sampled_through_copy(const struct parhelia_resource *r)
{
   if (util_format_has_depth(util_format_description(r->base.format)))
      return r->base.target == PIPE_TEXTURE_2D ||
             r->base.target == PIPE_TEXTURE_RECT ||
             r->base.target == PIPE_TEXTURE_1D;
   /* A raw-mapped P690 16-bit target is copied from its mapping like a
    * shadow (MEASURED on the P690 only), a native import (texture_from_pixmap)
    * from its shadow: every bind makes a new view and so a fresh copy. */
   return (r->transfer_shadow ||
           (r->map && ph_lx_family(ph_screen(r->base.screen)->chip_id))) &&
          !ph_resource_uses_sampler_layout(r) &&
          !(r->base.bind & PIPE_BIND_DEPTH_STENCIL) &&
          (r->base.target == PIPE_TEXTURE_2D ||
           r->base.target == PIPE_TEXTURE_RECT ||
           r->base.target == PIPE_TEXTURE_1D);
}

/* Select the effective view for every bound sampler view before a draw. */
static void
ph_update_effective_views(struct ph_context *ctx)
{
   const struct ph_fs *fs = ctx->fs;
   const unsigned views = MAX2(ctx->num_bound_views, ctx->num_sampler_views);
   const unsigned samplers = MAX2(ctx->num_bound_samplers, ctx->num_samplers);

   for (unsigned i = 0; i < samplers; i++) {
      unsigned src = fs && fs->unit_remap ?
         (i < ARRAY_SIZE(fs->unit_source) ? fs->unit_source[i] : ~0u) : i;
      struct pipe_sampler_state *use =
         src < ctx->num_bound_samplers ? ctx->bound_samplers[src] : NULL;

      if (ctx->samplers[i] != use) {
         ph_submit_vertices(ctx);
         ctx->samplers[i] = use;
      }
   }
   ctx->num_samplers = samplers;
   while (ctx->num_samplers && !ctx->samplers[ctx->num_samplers - 1])
      ctx->num_samplers--;
   for (unsigned i = 0; i < views; i++) {
      unsigned src = fs && fs->unit_remap ?
         (i < ARRAY_SIZE(fs->unit_source) ? fs->unit_source[i] : ~0u) : i;
      struct pipe_sampler_view *bound =
         src < ctx->num_bound_views ? ctx->bound_views[src] : NULL;
      struct pipe_sampler_view *use = bound;

      /* A render target keeps the surface order, the sampler's with y bits
       * 0 and 1 exchanged: it is sampled through a sampler-order copy that
       * follows its content generation.  Storage order 0 is no linear
       * alternative on the P690: --texture-volume-probe W 32 1 with
       * PROBE_TEXELCODES=0 fetches x2..x4 and y2.. from one address, and the
       * vendor's untiled 32-bit class writes order 2 (mtx2029). */
      if (bound && ((bound->u.tex.first_level &&
                     !ph_view_first_level(ph_resource(bound->texture),
                                          bound)) ||
                    ph_sampled_through_copy(ph_resource(bound->texture)))) {
         struct pipe_sampler_view *shadow =
            ph_shadow_view(ctx, (struct ph_sampler_view *)bound);

         if (shadow)
            use = shadow;
      }
      if (ctx->sampler_views[i] != use) {
         ph_submit_vertices(ctx);
         pipe_sampler_view_reference(&ctx->sampler_views[i], use);
      }
   }
   ctx->num_sampler_views = views;
   while (ctx->num_sampler_views &&
          !ctx->sampler_views[ctx->num_sampler_views - 1])
      ctx->num_sampler_views--;
}

/* ------------------------------------------------------- hardware transform
 *
 * The WARP transform programs read an object-space position from the vertex
 * stream, the matrix from constant slots c0..c3 and the viewport from c8/c9,
 * and hand the setup unit a window-space position.  That is exactly the shape
 * of Mesa's fixed-function vertex program, so a bound vertex shader whose only
 * computed output is POSITION = MVP * input can run on the chip instead of in
 * the software vertex pipeline.  Everything else keeps the draw module.
 */
enum ph_val_kind {
   PH_VAL_NONE = 0,
   PH_VAL_CONST,
   PH_VAL_DEREF,
   PH_VAL_UBO,
   PH_VAL_INPUT,
   PH_VAL_ROW,
};

struct ph_val {
   enum ph_val_kind kind;
   nir_variable *var;      /* DEREF */
   uint32_t value;         /* CONST */
   uint32_t base;          /* UBO: vec4 index; ROW: the matrix's row-0 index */
   uint32_t input;         /* INPUT/ROW: input driver_location */
   uint8_t comp;           /* ROW: destination component, or all four */
   uint8_t terms;          /* ROW: which of the four products are folded in */
};

static const struct ph_val *
ph_val_of(const struct ph_val *vals, unsigned count, const nir_def *def)
{
   return def->index < count ? &vals[def->index] : NULL;
}

/* xyzw on all @n components. */
static bool
ph_swizzle_identity(const nir_alu_instr *alu, unsigned src, unsigned n)
{
   for (unsigned i = 0; i < n; i++)
      if (alu->src[src].swizzle[i] != i)
         return false;
   return true;
}

/* One component of @src broadcast over all @n components. */
static bool
ph_swizzle_broadcast(const nir_alu_instr *alu, unsigned src, unsigned n,
                     unsigned *comp)
{
   *comp = alu->src[src].swizzle[0];
   for (unsigned i = 1; i < n; i++)
      if (alu->src[src].swizzle[i] != *comp)
         return false;
   return true;
}

/* One term of a matrix product: the matrix row in @ubo scaled by the vertex
 * component in @in.  @row_comp is the destination component for the scalar
 * form and PH_ALL_COMPONENTS for the vector form. */
#define PH_ALL_COMPONENTS 0xffu

static bool
ph_match_matrix_term(const nir_alu_instr *alu, const struct ph_val *a,
                     const struct ph_val *b, unsigned n,
                     const struct ph_val **ubo, unsigned *row_comp,
                     const struct ph_val **in, unsigned *term)
{
   unsigned ui;

   if (a && a->kind == PH_VAL_UBO && b && b->kind == PH_VAL_INPUT) {
      ui = 0;
      *ubo = a;
      *in = b;
   } else if (b && b->kind == PH_VAL_UBO && a && a->kind == PH_VAL_INPUT) {
      ui = 1;
      *ubo = b;
      *in = a;
   } else {
      return false;
   }
   if (n == 1) {
      *row_comp = alu->src[ui].swizzle[0];
      *term = alu->src[ui ^ 1].swizzle[0];
      return true;
   }
   *row_comp = PH_ALL_COMPONENTS;
   return ph_swizzle_identity(alu, ui, n) &&
          ph_swizzle_broadcast(alu, ui ^ 1, n, term);
}

static bool
ph_vs_analyze_alu(const nir_alu_instr *alu, struct ph_val *vals,
                  unsigned count, struct ph_val *out)
{
   const struct ph_val *src[4] = {0};
   unsigned n = nir_op_infos[alu->op].num_inputs;
   unsigned width = alu->def.num_components;

   if (n > 4 || alu->def.bit_size != 32)
      return false;
   for (unsigned i = 0; i < n; i++)
      src[i] = ph_val_of(vals, count, alu->src[i].src.ssa);

   switch (alu->op) {
   case nir_op_fmul: {
      const struct ph_val *ubo, *in;
      unsigned row_comp, term;

      if ((width != 1 && width != 4) ||
          !ph_match_matrix_term(alu, src[0], src[1], width, &ubo, &row_comp,
                                &in, &term) ||
          term > 3 || ubo->base < term)
         return false;
      /* The scalar form starts the chain at term 0; the vector form adds its
       * four products in any order. */
      if (width == 1 && term)
         return false;
      out->kind = PH_VAL_ROW;
      out->base = ubo->base - term;
      out->input = in->input;
      out->comp = row_comp;
      out->terms = 1u << term;
      return true;
   }
   case nir_op_fadd: {
      const struct ph_val *a = src[0], *b = src[1];

      if (width != 4 || !a || !b || a->kind != PH_VAL_ROW ||
          b->kind != PH_VAL_ROW || a->comp != PH_ALL_COMPONENTS ||
          b->comp != PH_ALL_COMPONENTS || a->base != b->base ||
          a->input != b->input || (a->terms & b->terms) ||
          !ph_swizzle_identity(alu, 0, 4) || !ph_swizzle_identity(alu, 1, 4))
         return false;
      *out = *a;
      out->terms |= b->terms;
      return true;
   }
   case nir_op_ffma:
   case nir_op_ffma_weak: {
      const struct ph_val *ubo, *in, *prev = src[2];
      unsigned row_comp, term;

      if (width != 1 || !prev || prev->kind != PH_VAL_ROW ||
          !ph_match_matrix_term(alu, src[0], src[1], 1, &ubo, &row_comp, &in,
                                &term) ||
          term == 0 || term > 3 || prev->terms != (1u << term) - 1u ||
          prev->comp != row_comp || prev->input != in->input ||
          prev->base + term != ubo->base)
         return false;
      *out = *prev;
      out->terms |= 1u << term;
      return true;
   }
   case nir_op_vec4:
      for (unsigned i = 0; i < 4; i++) {
         if (!src[i] || src[i]->kind != PH_VAL_ROW || src[i]->terms != 0xf ||
             src[i]->comp != i || src[i]->base != src[0]->base ||
             src[i]->input != src[0]->input)
            return false;
      }
      out->kind = PH_VAL_ROW;
      out->base = src[0]->base;
      out->input = src[0]->input;
      out->comp = PH_ALL_COMPONENTS;
      out->terms = 0xf;
      return true;
   default:
      return false;
   }
}

/* True when @nir is the fixed-function shape the WARP transform expresses.
 *
 * With @relaxed only the position store has to be that shape: the CPU bounds
 * the hardware clip volume by evaluating the matrix product itself, so a
 * compiled vertex program still needs the matrix, while everything else in the
 * shader is the WARP compiler's business. */
static bool
ph_vs_analyze(nir_shader *nir, struct ph_vs *vs, bool relaxed)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct ph_val *vals;
   bool ok = true;
   bool have_position = false;

   vs->pos_input = vs->colour_input = -1;
   vs->texcoord_input[0] = vs->texcoord_input[1] = -1;
   if (!impl || exec_list_length(&impl->body) != 1)
      return false;
   nir_index_ssa_defs(impl);
   vals = calloc(impl->ssa_alloc, sizeof(*vals));
   if (!vals)
      return false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         struct ph_val val = {0};
         nir_def *def = NULL;
         bool bad = false;

         switch (instr->type) {
         case nir_instr_type_load_const: {
            nir_load_const_instr *lc = nir_instr_as_load_const(instr);

            if (lc->def.num_components != 1 || lc->def.bit_size != 32) {
               bad = true;
               break;
            }
            val.kind = PH_VAL_CONST;
            val.value = lc->value[0].u32;
            def = &lc->def;
            break;
         }
         case nir_instr_type_deref: {
            nir_deref_instr *deref = nir_instr_as_deref(instr);

            if (deref->deref_type != nir_deref_type_var) {
               bad = true;
               break;
            }
            val.kind = PH_VAL_DEREF;
            val.var = deref->var;
            def = &deref->def;
            break;
         }
         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            const struct ph_val *a, *b;

            switch (in->intrinsic) {
            case nir_intrinsic_load_ubo:
            case nir_intrinsic_load_ubo_vec4: {
               uint32_t vec4;

               a = ph_val_of(vals, impl->ssa_alloc, in->src[0].ssa);
               b = ph_val_of(vals, impl->ssa_alloc, in->src[1].ssa);
               if (in->def.num_components != 4 || !a || !b ||
                   a->kind != PH_VAL_CONST || a->value != 0 ||
                   b->kind != PH_VAL_CONST) {
                  bad = true;
                  break;
               }
               /* load_ubo counts bytes, load_ubo_vec4 counts vec4s and puts
                * the constant part in its base index. */
               if (in->intrinsic == nir_intrinsic_load_ubo) {
                  if (b->value & 15u) {
                     bad = true;
                     break;
                  }
                  vec4 = b->value / 16u;
               } else {
                  /* load_ubo_vec4's index is base plus the offset source.
                   * GLSL puts it in the source and leaves the base zero, so
                   * reading only the base refuses every GLSL shader. */
                  if (nir_intrinsic_component(in) ||
                      b->value > (1u << 16)) {
                     bad = true;
                     break;
                  }
                  vec4 = nir_intrinsic_base(in) + b->value;
               }
               /* Bound it: the row arithmetic below would otherwise wrap. */
               if (vec4 > (1u << 16)) {
                  bad = true;
                  break;
               }
               val.kind = PH_VAL_UBO;
               val.base = vec4;
               def = &in->def;
               break;
            }
            case nir_intrinsic_load_deref:
               a = ph_val_of(vals, impl->ssa_alloc, in->src[0].ssa);
               if (!a || a->kind != PH_VAL_DEREF ||
                   a->var->data.mode != nir_var_shader_in) {
                  bad = true;
                  break;
               }
               val.kind = PH_VAL_INPUT;
               val.input = a->var->data.driver_location;
               def = &in->def;
               break;
            case nir_intrinsic_store_deref: {
               const struct ph_val *value;
               int *slot;

               a = ph_val_of(vals, impl->ssa_alloc, in->src[0].ssa);
               value = ph_val_of(vals, impl->ssa_alloc, in->src[1].ssa);
               if (!a || a->kind != PH_VAL_DEREF ||
                   a->var->data.mode != nir_var_shader_out ||
                   nir_intrinsic_write_mask(in) != 0xf || !value) {
                  bad = true;
                  break;
               }
               if (a->var->data.location == VARYING_SLOT_POS) {
                  if (value->kind != PH_VAL_ROW ||
                      value->comp != PH_ALL_COMPONENTS ||
                      value->terms != 0xf || have_position) {
                     ok = false;
                     break;
                  }
                  have_position = true;
                  vs->mvp_offset = value->base;
                  vs->pos_input = value->input;
                  break;
               }
               if (a->var->data.location == VARYING_SLOT_COL0)
                  slot = &vs->colour_input;
               else if (a->var->data.location == VARYING_SLOT_TEX0)
                  slot = &vs->texcoord_input[0];
               else if (a->var->data.location == VARYING_SLOT_TEX1)
                  slot = &vs->texcoord_input[1];
               else {
                  bad = true;
                  break;
               }
               if (value->kind != PH_VAL_INPUT || *slot >= 0) {
                  bad = true;
                  break;
               }
               *slot = value->input;
               break;
            }
            default:
               bad = true;
               break;
            }
            break;
         }
         case nir_instr_type_alu:
            if (!ph_vs_analyze_alu(nir_instr_as_alu(instr), vals,
                                   impl->ssa_alloc, &val)) {
               bad = true;
               break;
            }
            def = &nir_instr_as_alu(instr)->def;
            break;
         case nir_instr_type_jump:
            break;
         default:
            bad = true;
            break;
         }
         if (bad && !relaxed)
            ok = false;
         if (!ok)
            break;
         if (!bad && def && def->index < impl->ssa_alloc)
            vals[def->index] = val;
      }
      if (!ok)
         break;
   }
   free(vals);
   return ok && have_position;
}

/* ------------------------------------------------ compiled vertex programs
 *
 * The fetch slots the transforming attribute descriptors declare, with the
 * dword each occupies in the object-space vertex stream.  A compiled program
 * reads these and nothing else, so the stream layout and the fixed-function
 * one stay identical.
 */
struct ph_tcl_slot {
   uint8_t slot;                /* WARP v# */
   uint8_t offset;              /* stream dword */
   uint8_t max_components;      /* accepted in the vertex buffer */
   enum pipe_format format;     /* what the stream carries */
};

/* The colour block takes an RGBA attribute and drops its alpha, exactly as the
 * fixed-function route does; the transforming stream has no alpha dword. */
static const struct ph_tcl_slot ph_tcl_slots[] = {
   { P3D_WARP_V_POSITION,      0, 3, PIPE_FORMAT_R32G32B32_FLOAT },
   { P3D_WARP_V_COLOUR,        3, 4, PIPE_FORMAT_R32G32B32_FLOAT },
   { P3D_WARP_V_TEXCOORD0,     6, 2, PIPE_FORMAT_R32G32_FLOAT },
   { P3D_WARP_V_TEXCOORD0 + 1, 8, 2, PIPE_FORMAT_R32G32_FLOAT },
};

static const struct ph_tcl_slot *
ph_tcl_slot_of(unsigned slot)
{
   for (unsigned i = 0; i < ARRAY_SIZE(ph_tcl_slots); i++)
      if (ph_tcl_slots[i].slot == slot)
         return &ph_tcl_slots[i];
   return NULL;
}

/* Assign the shader's vertex inputs to fetch slots.  The position drives v0
 * because the clip-volume bound reads the stream's first three dwords; the
 * remaining inputs follow the descriptor order in attribute order. */
static bool
ph_vs_assign_inputs(nir_shader *nir, struct ph_vs *vs)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   bool used[PIPE_MAX_ATTRIBS] = {0};
   unsigned next = 1;

   vs->prog_inputs.count = PIPE_MAX_ATTRIBS;
   for (unsigned i = 0; i < PIPE_MAX_ATTRIBS; i++)
      vs->prog_inputs.slot[i] = -1;
   if (!impl || vs->pos_input < 0 || vs->pos_input >= PIPE_MAX_ATTRIBS)
      return false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         const nir_intrinsic_instr *load;
         unsigned location;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         load = nir_instr_as_intrinsic(instr);
         if (load->intrinsic == nir_intrinsic_load_input) {
            location = nir_intrinsic_base(load);
         } else if (load->intrinsic == nir_intrinsic_load_deref) {
            const nir_deref_instr *deref = nir_src_as_deref(load->src[0]);

            if (!deref || deref->deref_type != nir_deref_type_var ||
                deref->var->data.mode != nir_var_shader_in)
               continue;
            location = deref->var->data.driver_location;
         } else {
            continue;
         }
         if (location >= PIPE_MAX_ATTRIBS)
            return false;
         used[location] = true;
      }
   }
   used[vs->pos_input] = false;
   vs->prog_inputs.slot[vs->pos_input] = ph_tcl_slots[0].slot;
   for (unsigned i = 0; i < PIPE_MAX_ATTRIBS; i++) {
      if (!used[i])
         continue;
      if (next >= ARRAY_SIZE(ph_tcl_slots))
         return false;          /* more attributes than the stream carries */
      vs->prog_inputs.slot[i] = ph_tcl_slots[next++].slot;
   }
   return true;
}

/* Upload a compiled program into a kernel instruction-store slot.  The engine
 * serves an already-executed entry from an instruction cache, so the kernel
 * hands out a fresh entry per upload and only the value it returns may be
 * bound (docs/parhelia-p690-warp-isa-re.md section 9.3). */
static bool
ph_vs_upload_program(struct ph_context *ctx, struct ph_vs *vs)
{
   struct drm_parhelia_warp_program program = {
      .code = (uintptr_t)vs->prog.code,
      .count = vs->prog.count,
      .slot = vs->prog_slot,
   };

   if (vs->prog_entry)
      return true;
   if (vs->prog_upload_failed || !vs->prog.valid)
      return false;
   if (ioctl(ph_screen(ctx->base.screen)->fd,
             DRM_IOCTL_PARHELIA_WARP_PROGRAM, &program) || !program.entry) {
      vs->prog_upload_failed = true;
      return false;
   }
   vs->prog_slot = program.slot;
   vs->prog_entry = (uint32_t)program.entry;
   ctx->warp_programs_held++;
   if (ph_env("PARHELIA_TRACE_TCL"))
      fprintf(stderr,
              "parhelia: WARP program slot=%u entry=%#x instructions=%u "
              "constants=%u+%u vp=c%u/c%u inputs=%#x outputs=%#x\n",
              vs->prog_slot, vs->prog_entry, vs->prog.count,
              vs->prog.constants.vec4s, vs->prog.literals,
              vs->prog.vp_scale_slot, vs->prog.vp_offset_slot,
              vs->prog.input_mask, vs->prog.output_mask);
   return true;
}

/* The constant block a compiled program reads: the shader's own uniform
 * vec4s followed by the literals the compiler spelled out.  Built into the
 * candidate buffer so the caller can compare it against the pending batch's. */
static bool
ph_tcl_program_constants(struct ph_context *ctx, const struct ph_vs *vs,
                         unsigned *vec4s)
{
   unsigned count = vs->prog.constants.vec4s + vs->prog.literals;
   const uint8_t *base = NULL;
   float *block;

   /* One block load carries 1..63 vectors, and every accepted program reads at
    * least the four matrix vec4s. */
   if (!count || count > 63)
      return false;
   if (ctx->vs_const_cap < count * 4u) {
      block = realloc(ctx->vs_const_pending, count * 4u * sizeof(*block));
      if (!block)
         return false;
      ctx->vs_const_pending = block;
      block = realloc(ctx->vs_const, count * 4u * sizeof(*block));
      if (!block)
         return false;
      ctx->vs_const = block;
      ctx->vs_const_cap = count * 4u;
   }
   block = ctx->vs_const_pending;
   if (vs->prog.constants.vec4s) {
      if (ctx->user_constants[MESA_SHADER_VERTEX])
         base = ctx->user_constants[MESA_SHADER_VERTEX];
      else if (ctx->constants[MESA_SHADER_VERTEX])
         base = ph_resource(ctx->constants[MESA_SHADER_VERTEX])->malloc_data;
      if (!base ||
          ctx->constant_size[MESA_SHADER_VERTEX] <
             vs->prog.constants.vec4s * 16u)
         return false;
      base += ctx->constant_offset[MESA_SHADER_VERTEX];
      memcpy(block, base, vs->prog.constants.vec4s * 16u);
   }
   memcpy(block + vs->prog.constants.vec4s * 4u, vs->prog.literal,
          vs->prog.literals * 16u);
   *vec4s = count;
   return true;
}

/* The shader's matrix rows are the rows of the transposed matrix, so
 * ubo_row[k][i] scales input component k into output component i.  WARP
 * computes r2.i = dot(v, c_i), so c_i[k] = ubo_row[k][i]. */
static bool
ph_tcl_matrix(struct ph_context *ctx, float mvp[16])
{
   const uint8_t *base = NULL;
   const float *rows;

   if (ctx->user_constants[MESA_SHADER_VERTEX])
      base = ctx->user_constants[MESA_SHADER_VERTEX];
   else if (ctx->constants[MESA_SHADER_VERTEX])
      base = ph_resource(ctx->constants[MESA_SHADER_VERTEX])->malloc_data;
   if (!base)
      return false;
   base += ctx->constant_offset[MESA_SHADER_VERTEX];
   /* The matrix offset comes out of the shader, so it bounds nothing by
    * itself: the constant buffer has to be known and big enough. */
   if (ctx->constant_size[MESA_SHADER_VERTEX] < 64 ||
       ctx->vs->mvp_offset > (ctx->constant_size[MESA_SHADER_VERTEX] - 64) / 16)
      return false;
   rows = (const float *)(base + ctx->vs->mvp_offset * 16);
   for (unsigned i = 0; i < 4; i++)
      for (unsigned k = 0; k < 4; k++)
         mvp[i * 4 + k] = rows[k * 4 + i];
   return true;
}

/* Object-space positions still have to reach the raster inside the parts of
 * clip space the hardware can carry: the transform program divides by w
 * itself, and the vendor stack never asked this silicon to survive w <= 0
 * (docs/parhelia-raster-clip.md section 8).  Everything outside the frustum
 * in z, or outside the guard band in x and y, keeps the software clipper. */
#define PH_TCL_GUARD_BAND 8160.0f

/* A compiled combiner takes its runtime constants when the batch is built,
 * and a state tracker may rewrite the same constant buffer in place before
 * the next draw: three scissored glClears of different colours all came out
 * in the last one's.  Submit the staged draws when the constants change, and
 * keep the copy they were issued with. */
static void
ph_fs_snapshot_constants(struct ph_context *ctx)
{
   const uint8_t *base = NULL;
   unsigned size = ctx->constant_size[MESA_SHADER_FRAGMENT];

   if (ctx->user_constants[MESA_SHADER_FRAGMENT])
      base = ctx->user_constants[MESA_SHADER_FRAGMENT];
   else if (ctx->constants[MESA_SHADER_FRAGMENT])
      base = ph_resource(ctx->constants[MESA_SHADER_FRAGMENT])->malloc_data;
   if (base)
      base += ctx->constant_offset[MESA_SHADER_FRAGMENT];
   else
      size = 0;
   /* PARHELIA_FS_LIVE_CONSTANTS=1 (diagnostic): no submit on a change. */
   if ((ctx->triangles || ctx->points || ctx->lines) &&
       !ph_env("PARHELIA_FS_LIVE_CONSTANTS") &&
       (size != ctx->fs_const_snap_size ||
        (size && memcmp(ctx->fs_const_snap, base, size))))
      ph_submit_vertices(ctx);
   if (size > ctx->fs_const_snap_cap) {
      uint8_t *grown = realloc(ctx->fs_const_snap, size);

      if (!grown) {
         ctx->fs_const_snap_size = 0;
         return;
      }
      ctx->fs_const_snap = grown;
      ctx->fs_const_snap_cap = size;
   }
   if (size)
      memcpy(ctx->fs_const_snap, base, size);
   ctx->fs_const_snap_size = size;
}

/* The fragment colour when it is a plain constant: GL's current colour, set
 * with glColor and not through a colour array, reaches the fragment stage as a
 * uniform rather than a COL0 varying.  The vertex shader then writes no
 * colour, and a draw that stages white into the colour slot renders white
 * instead of the colour the application asked for.  The fragment compiler
 * already resolves such a shader to one instruction moving a constant-buffer
 * vec4, so evaluate the program and stage its result. */
static bool
ph_fs_constant_colour(struct ph_context *ctx, float out[4])
{
   static const float zero[4][4];
   struct ph_fs *fs = ctx->fs;

   if (!fs || !fs->program.valid || fs->program.sampler_mask ||
       fs->program.uses_color)
      return false;
   if (fs->program.const_ref_count) {
      if (!ctx->fs_const_snap_size)
         return false;
      if (!ph_fs_update_constants(&fs->program, ctx->fs_const_snap,
                                  ctx->fs_const_snap_size))
         return false;
   }
   ph_fs_eval(&fs->program.comb, zero, (const float (*)[4])zero, 1.0f, out);
   return true;
}

static bool
ph_tcl_vertices_fit(struct ph_context *ctx, const float *vertices,
                    unsigned count, unsigned dpv)
{
   const float *m = ctx->mvp;
   float gx = (PH_TCL_GUARD_BAND - fabsf(ctx->viewport.translate[0])) /
              MAX2(fabsf(ctx->viewport.scale[0]), 1e-6f);
   float gy = (PH_TCL_GUARD_BAND - fabsf(ctx->viewport.translate[1])) /
              MAX2(fabsf(ctx->viewport.scale[1]), 1e-6f);

   struct ph_slice *slice = ctx->warp_program_entry && ctx->vs ?
                            ctx->vs->pos_slice : NULL;
   const float *constants = NULL;
   unsigned constant_size = 0;

   if (gx <= 1.0f || gy <= 1.0f)
      return false;
   if (slice) {
      const uint8_t *base = ctx->user_constants[MESA_SHADER_VERTEX];

      if (!base && ctx->constants[MESA_SHADER_VERTEX])
         base = ph_resource(ctx->constants[MESA_SHADER_VERTEX])->malloc_data;
      if (base) {
         constants = (const float *)(base +
                                     ctx->constant_offset[MESA_SHADER_VERTEX]);
         constant_size = ctx->constant_size[MESA_SHADER_VERTEX];
      }
   }
   for (unsigned v = 0; v < count; v++) {
      const float *p = vertices + v * dpv;
      float x, y, z, w;

      if (slice) {
         float input[32][4];
         float clip[4];

         /* The stream carries each input at its fetch slot's dword, with
          * the slot's component count; the rest takes GL's 0,0,0,1. */
         u_foreach_bit(i, slice->input_mask) {
            const int slot = ctx->vs->prog_inputs.slot[i];
            const struct ph_tcl_slot *desc =
               slot >= 0 ? ph_tcl_slot_of((unsigned)slot) : NULL;
            const unsigned n = desc ?
               util_format_get_nr_components(desc->format) : 0;

            input[i][0] = input[i][1] = input[i][2] = 0.0f;
            input[i][3] = 1.0f;
            for (unsigned c = 0; c < n; c++)
               input[i][c] = p[desc->offset + c];
         }
         ph_slice_eval(slice, (const float (*)[4])input, constants,
                       constant_size, clip);
         x = clip[0];
         y = clip[1];
         z = clip[2];
         w = clip[3];
      } else {
         x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
         y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
         z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
         w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
      }

      if (!(w > 1e-6f) || !(fabsf(x) <= gx * w) || !(fabsf(y) <= gy * w) ||
          !(z >= -w) || !(z <= w)) {
         ctx->tcl_clip_why[!(w > 1e-6f) ? 0 :
                           !(fabsf(x) <= gx * w && fabsf(y) <= gy * w) ? 1 :
                           !(z >= -w) ? 2 : 3]++;
         return false;
      }
   }
   return true;
}

/* The planes a clipped vertex has to stay inside: w > 0, near, far and the
 * x and y bounds, each pulled in by a hair so rounding in the transform never
 * puts a clipped vertex outside.  The transform is linear, so clipping in clip
 * space and interpolating the object-space vertex with the same parameter
 * gives exactly the clipped point. */
#define PH_TCL_CLIP_PLANES 7
#define PH_TCL_CLIP_MAX (3 + PH_TCL_CLIP_PLANES)
#define PH_TCL_MAX_DPV 11

/* A clipped triangle is cut at the viewport plus this many pixels, not at
 * the guard band: clipped vertices on the guard band edge (window x 8160)
 * stalled the 0528 engine, and GL only asks for |x|, |y| <= w. */
#define PH_TCL_CLIP_MARGIN 16.0f

struct ph_tcl_run {
   unsigned dpv, limit, vs_const_vec4s;
   uint32_t entry;
   bool colour_alpha;
   int packed_colour;           /* dword of a packed colour, or -1 */
   float gx, gy;                /* guard band: triangles inside stay whole */
   float cx, cy;                /* the clipper's x and y planes */
};

static float
ph_tcl_plane_distance(unsigned plane, const float c[4], float gx, float gy)
{
   const float in = 1.0f - 1.0f / 65536.0f;

   switch (plane) {
   case 0: return c[3] - 2e-6f;
   case 1: return c[2] + in * c[3];
   case 2: return in * c[3] - c[2];
   case 3: return in * gx * c[3] - c[0];
   case 4: return in * gx * c[3] + c[0];
   case 5: return in * gy * c[3] - c[1];
   default: return in * gy * c[3] + c[1];
   }
}

static void
ph_tcl_clip_coords(const float *m, const float *p, float c[4])
{
   for (unsigned r = 0; r < 4; r++)
      c[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] +
             m[r * 4 + 3];
}

/* Append one object-space triangle to the transforming batch, publishing a
 * full batch first; a submit clears the batch's program state. */
static bool
ph_tcl_append_tri(struct ph_context *ctx, const struct ph_tcl_run *r,
                  const float *a, const float *b, const float *c)
{
   const unsigned room = ph_logical_vertex_limit(ctx) / (3 * r->dpv);
   float *dst;

   if (ctx->triangles >= MIN2(r->limit, room)) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return false;
   }
   ctx->transform = true;
   ctx->warp_program_entry = r->entry;
   ctx->vs_const_vec4s = r->vs_const_vec4s;
   ctx->colour_alpha = r->colour_alpha;
   dst = &ctx->vertices[ctx->triangles * 3 * r->dpv];
   memcpy(dst, a, r->dpv * sizeof(float));
   memcpy(dst + r->dpv, b, r->dpv * sizeof(float));
   memcpy(dst + 2 * r->dpv, c, r->dpv * sizeof(float));
   ctx->triangles++;
   return true;
}

/* A packed colour interpolated byte by byte. */
static uint32_t
ph_packed_colour_lerp(uint32_t a, uint32_t b, float t)
{
   uint32_t c = 0;

   for (unsigned shift = 0; shift < 32; shift += 8) {
      const float ca = (float)((a >> shift) & 0xff);
      const float cb = (float)((b >> shift) & 0xff);

      c |= (uint32_t)(ca + t * (cb - ca) + 0.5f) << shift;
   }
   return c;
}

/* Sutherland-Hodgman against the transform's planes; the intersection is
 * always taken from the inside vertex, so an edge two triangles share is cut
 * at the same point in both. */
static bool
ph_tcl_clip_tri(struct ph_context *ctx, const struct ph_tcl_run *r,
                const float *const tri[3], const float tri_clip[3][4])
{
   float vert[2][PH_TCL_CLIP_MAX][PH_TCL_MAX_DPV];
   float clip[2][PH_TCL_CLIP_MAX][4];
   unsigned n = 3, cur = 0;

   for (unsigned i = 0; i < 3; i++) {
      memcpy(vert[0][i], tri[i], r->dpv * sizeof(float));
      memcpy(clip[0][i], tri_clip[i], sizeof(clip[0][i]));
   }
   for (unsigned plane = 0; plane < PH_TCL_CLIP_PLANES; plane++) {
      const unsigned nxt = cur ^ 1;
      unsigned m = 0;

      for (unsigned i = 0; i < n; i++) {
         const unsigned j = i + 1 < n ? i + 1 : 0;
         const float di = ph_tcl_plane_distance(plane, clip[cur][i],
                                                r->cx, r->cy);
         const float dj = ph_tcl_plane_distance(plane, clip[cur][j],
                                                r->cx, r->cy);

         if (di >= 0.0f) {
            memcpy(vert[nxt][m], vert[cur][i], r->dpv * sizeof(float));
            memcpy(clip[nxt][m], clip[cur][i], sizeof(clip[0][0]));
            m++;
         }
         /* a vertex on the plane is its own intersection */
         if ((di > 0.0f && dj < 0.0f) || (di < 0.0f && dj > 0.0f)) {
            const unsigned in = di >= 0.0f ? i : j, out = in == i ? j : i;
            const float din = in == i ? di : dj, dout = in == i ? dj : di;
            const float t = din / (din - dout);

            for (unsigned k = 0; k < r->dpv; k++)
               vert[nxt][m][k] = (int)k == r->packed_colour ?
                  uif(ph_packed_colour_lerp(fui(vert[cur][in][k]),
                                            fui(vert[cur][out][k]), t)) :
                  vert[cur][in][k] +
                  t * (vert[cur][out][k] - vert[cur][in][k]);
            for (unsigned k = 0; k < 4; k++)
               clip[nxt][m][k] = clip[cur][in][k] +
                                 t * (clip[cur][out][k] - clip[cur][in][k]);
            m++;
         }
      }
      n = m;
      cur = nxt;
      if (n < 3)
         return true;
   }
   ctx->tcl_clipped_tris++;
   if (ph_env("PARHELIA_TRACE_TCL_CLIP")) {
      fprintf(stderr, "parhelia clip:");
      for (unsigned i = 0; i < 3; i++) {
         float c[4];

         ph_tcl_clip_coords(ctx->mvp, tri[i], c);
         fprintf(stderr, " (%g %g %g %g)", c[0], c[1], c[2], c[3]);
      }
      fprintf(stderr, " ->");
      for (unsigned i = 0; i < n; i++)
         fprintf(stderr, " (%g %g %g %g)", clip[cur][i][0], clip[cur][i][1],
                 clip[cur][i][2], clip[cur][i][3]);
      fprintf(stderr, "\n");
   }
   for (unsigned k = 1; k + 1 < n; k++)
      if (!ph_tcl_append_tri(ctx, r, vert[cur][0], vert[cur][k],
                             vert[cur][k + 1]))
         return false;
   return true;
}

/* A fixed-function chunk with vertices outside the transform's planes: keep
 * the triangles that fit as they are and clip the others, all on the
 * hardware transform. */
static bool
ph_tcl_clip_chunk(struct ph_context *ctx, const struct ph_tcl_run *r,
                  const float *staged, unsigned ntri)
{
   const unsigned bytes = ntri * 3 * r->dpv * sizeof(float);
   float *copy;

   /* the staged chunk is overwritten by what gets appended */
   if (ctx->tcl_clip_cap < bytes) {
      float *grown = realloc(ctx->tcl_clip_copy, bytes);

      if (!grown)
         return false;
      ctx->tcl_clip_copy = grown;
      ctx->tcl_clip_cap = bytes;
   }
   copy = ctx->tcl_clip_copy;
   memcpy(copy, staged, bytes);
   for (unsigned t = 0; t < ntri; t++) {
      const float *tri[3];
      float c[3][4];
      bool inside = true, outside = false;

      for (unsigned v = 0; v < 3; v++) {
         tri[v] = copy + (t * 3 + v) * r->dpv;
         ph_tcl_clip_coords(ctx->mvp, tri[v], c[v]);
         if (!(c[v][3] > 1e-6f) || !(fabsf(c[v][0]) <= r->gx * c[v][3]) ||
             !(fabsf(c[v][1]) <= r->gy * c[v][3]) || !(c[v][2] >= -c[v][3]) ||
             !(c[v][2] <= c[v][3]))
            inside = false;
      }
      /* all three outside one plane: the clipper keeps nothing of it */
      for (unsigned plane = 0; !inside && !outside &&
                               plane < PH_TCL_CLIP_PLANES; plane++)
         outside = ph_tcl_plane_distance(plane, c[0], r->cx, r->cy) < 0.0f &&
                   ph_tcl_plane_distance(plane, c[1], r->cx, r->cy) < 0.0f &&
                   ph_tcl_plane_distance(plane, c[2], r->cx, r->cy) < 0.0f;
      if (outside)
         continue;
      if (!(inside ? ph_tcl_append_tri(ctx, r, tri[0], tri[1], tri[2]) :
                     ph_tcl_clip_tri(ctx, r, tri, (const float (*)[4])c)))
         return false;
   }
   return true;
}

/* An integer whose signed order is the order of the float @bits (NaN aside):
 * min/max without float compares, which carry a trap barrier each on Alpha. */
static inline int32_t
ph_float_order_key(uint32_t bits)
{
   return (int32_t)(bits & 0x80000000u ? bits ^ 0x7fffffffu : bits);
}

static inline float
ph_float_from_order_key(int32_t key)
{
   const uint32_t k = (uint32_t)key;

   return uif(k & 0x80000000u ? k ^ 0x7fffffffu : k);
}

/* Min/max of @comps float components over vertices [lo, hi]; false if any is
 * NaN, which makes the box unusable. */
static bool
ph_tcl_box_scan(const uint8_t *base, unsigned stride, unsigned lo,
                unsigned hi, unsigned comps, float lo_out[3], float hi_out[3])
{
   int32_t kmin[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
   int32_t kmax[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
   uint32_t nan = 0;

   for (unsigned v = lo; v <= hi; v++) {
      const uint8_t *src = base + (size_t)v * stride;

      for (unsigned c = 0; c < comps; c++) {
         uint32_t bits;
         int32_t key;

         memcpy(&bits, src + c * 4, 4);
         nan |= (bits & 0x7fffffffu) > 0x7f800000u;
         key = ph_float_order_key(bits);
         kmin[c] = key < kmin[c] ? key : kmin[c];
         kmax[c] = key > kmax[c] ? key : kmax[c];
      }
   }
   if (nan)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      lo_out[c] = c < comps ? ph_float_from_order_key(kmin[c]) : 0.0f;
      hi_out[c] = c < comps ? ph_float_from_order_key(kmax[c]) : 0.0f;
   }
   return true;
}

/* Whether the position buffer is one the box cache cannot hold. */
static bool
ph_tcl_position_uncached(const struct ph_context *ctx)
{
   const struct pipe_vertex_buffer *vb;

   if (!ctx->vs || ctx->vs->pos_input < 0 ||
       (unsigned)ctx->vs->pos_input >= ctx->velems->count)
      return false;
   vb = &ctx->draw->pt.vertex_buffer[
      ctx->velems->elem[ctx->vs->pos_input].vertex_buffer_index];
   return vb->is_user_buffer || !vb->buffer.resource ||
          ph_resource(vb->buffer.resource)->data_volatile;
}

/* The object-space box of the positions a draw references: from the
 * buffer's cache while its contents are unchanged, else one min/max pass
 * over the referenced vertex range. */
static bool
ph_tcl_position_box(struct ph_context *ctx, const uint32_t range[2],
                    float lo_out[3], float hi_out[3])
{
   const struct pipe_vertex_element *e;
   const struct pipe_vertex_buffer *vb;
   struct parhelia_resource *res = NULL;
   const uint8_t *map;
   const unsigned lo = range[0], hi = range[1];
   unsigned comps, offset, stride;

   if (!ctx->vs || ctx->vs->pos_input < 0 ||
       (unsigned)ctx->vs->pos_input >= ctx->velems->count || lo > hi)
      return false;
   e = &ctx->velems->elem[ctx->vs->pos_input];
   if (e->src_format == PIPE_FORMAT_R32G32B32_FLOAT)
      comps = 3;
   else if (e->src_format == PIPE_FORMAT_R32G32_FLOAT)
      comps = 2;
   else
      return false;
   vb = &ctx->draw->pt.vertex_buffer[e->vertex_buffer_index];
   if (vb->is_user_buffer) {
      map = vb->buffer.user;
   } else {
      if (!vb->buffer.resource)
         return false;
      res = ph_resource(vb->buffer.resource);
      map = res->malloc_data;
   }
   if (!map)
      return false;
   offset = vb->buffer_offset + e->src_offset;
   stride = e->src_stride;
   if (res && (uint64_t)hi * stride + offset + comps * 4u > res->base.width0)
      return false;
   if (res && !res->data_volatile) {
      for (unsigned i = 0; i < res->bounds_count; i++) {
         const struct ph_vertex_bounds *b = &res->bounds[i];

         if (b->gen == res->data_gen && b->offset == offset &&
             b->stride == stride && b->format == e->src_format &&
             b->lo == lo && b->hi == hi) {
            memcpy(lo_out, b->min, 3 * sizeof(float));
            memcpy(hi_out, b->max, 3 * sizeof(float));
            return true;
         }
      }
   }
   if (!ph_tcl_box_scan(map + offset, stride, lo, hi, comps, lo_out, hi_out))
      return false;
   if (res && !res->data_volatile) {
      struct ph_vertex_bounds *b = NULL;

      /* Entries of an older content generation are reused first. */
      for (unsigned i = 0; i < res->bounds_count && !b; i++)
         if (res->bounds[i].gen != res->data_gen)
            b = &res->bounds[i];
      if (!b) {
         if (res->bounds_count == res->bounds_cap) {
            unsigned cap = MAX2(4u, res->bounds_cap * 2u);
            struct ph_vertex_bounds *grown =
               realloc(res->bounds, cap * sizeof(*grown));

            if (grown) {
               res->bounds = grown;
               res->bounds_cap = cap;
            }
         }
         if (res->bounds_count < res->bounds_cap)
            b = &res->bounds[res->bounds_count++];
      }
      if (b) {
         *b = (struct ph_vertex_bounds){ .gen = res->data_gen,
                                         .offset = offset, .stride = stride,
                                         .format = e->src_format,
                                         .lo = lo, .hi = hi };
         memcpy(b->min, lo_out, 3 * sizeof(float));
         memcpy(b->max, hi_out, 3 * sizeof(float));
      }
   }
   return true;
}

/* Whether every vertex inside the box lies within the clip volume and guard
 * band: the bounds are linear in the position, so the eight corners decide
 * it, and the draw then needs no per-vertex test at all. */
static bool
ph_tcl_box_fits(const struct ph_context *ctx, const float *m,
                struct ph_slice *slice, const float lo[3], const float hi[3])
{
   const float *constants = NULL;
   unsigned constant_size = 0;

   float gx = (PH_TCL_GUARD_BAND - fabsf(ctx->viewport.translate[0])) /
              MAX2(fabsf(ctx->viewport.scale[0]), 1e-6f);
   float gy = (PH_TCL_GUARD_BAND - fabsf(ctx->viewport.translate[1])) /
              MAX2(fabsf(ctx->viewport.scale[1]), 1e-6f);

   if (gx <= 1.0f || gy <= 1.0f)
      return false;
   if (slice) {
      const uint8_t *base = ctx->user_constants[MESA_SHADER_VERTEX];

      if (!base && ctx->constants[MESA_SHADER_VERTEX])
         base = ph_resource(ctx->constants[MESA_SHADER_VERTEX])->malloc_data;
      if (base) {
         constants = (const float *)(base +
                                     ctx->constant_offset[MESA_SHADER_VERTEX]);
         constant_size = ctx->constant_size[MESA_SHADER_VERTEX];
      }
   }
   for (unsigned k = 0; k < 8; k++) {
      const float p[3] = { k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1],
                           k & 4 ? hi[2] : lo[2] };
      float x, y, z, w;

      if (slice) {
         float input[32][4] = { { 0 } }, clip[4];

         memcpy(input[ctx->vs->pos_input], p, sizeof(p));
         input[ctx->vs->pos_input][3] = 1.0f;
         ph_slice_eval(slice, (const float (*)[4])input, constants,
                       constant_size, clip);
         x = clip[0];
         y = clip[1];
         z = clip[2];
         w = clip[3];
      } else {
         x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
         y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
         z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
         w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
      }

      if (!(w > 1e-6f) || !(fabsf(x) <= gx * w) || !(fabsf(y) <= gy * w) ||
          !(z >= -w) || !(z <= w))
         return false;
   }
   return true;
}

struct ph_tcl_layout {
   bool textured, multitextured;
   bool colour_alpha;           /* the colour slot is RGBA */
   bool packed_colour;          /* the colour slot is one D3DCOLOR dword */
   bool program;                /* a compiled vertex program, not the kernel's */
   unsigned dpv;
   unsigned colour_offset, tex_offset[2];
};

/* Everything the hardware transform cannot express keeps the draw module: the
 * alpha work-arounds own the vertex layout, the software clipper owns user
 * clip planes, and polygon offset has no place in the WARP program. */
DEBUG_GET_ONCE_BOOL_OPTION(tcl_rgba, "PARHELIA_TCL_RGBA", false)
DEBUG_GET_ONCE_BOOL_OPTION(tcl_box, "PARHELIA_TCL_BOX", true)
DEBUG_GET_ONCE_BOOL_OPTION(tcl_clip, "PARHELIA_TCL_CLIP", true)
/* Bounding positions in the fetch costs more than the separate scan on nano
 * (11.21 -> 11.37 ms CPU/frame in ioquake3): it sees every index, the scan
 * every referenced vertex once.  Kept for hosts where the second pass over
 * the positions costs more than the arithmetic. */
DEBUG_GET_ONCE_BOOL_OPTION(tcl_fused_box, "PARHELIA_TCL_FUSED_BOX", false)

static bool
ph_tcl_no(struct ph_context *ctx, enum ph_tcl_reason reason)
{
   ctx->tcl_last_reason = reason;
   return false;
}

static bool
ph_tcl_layout(struct ph_context *ctx, struct ph_tcl_layout *lay)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   bool textured = ph_samples_unit0(ctx);
   bool multitextured = textured && ctx->fs &&
                        ph_second_unit_bindable(ctx) &&
                        ctx->num_sampler_views > 1 && ctx->sampler_views[1] &&
                        ctx->sampler_views[1]->texture;
   bool depth = ctx->fb.zsbuf.texture != NULL;
   bool colour;

   /* The transform stream feeds unit u from GL coordinate set u. */
   if (ctx->fs && ctx->fs->unit_remap)
      return ph_tcl_no(ctx, PH_TCL_NO_UNIT_REMAP);
   /* Units 2 and 3 ride on the software path's four-set stream. */
   if (ctx->fs && (ctx->fs->sampler_mask & ~3u))
      return ph_tcl_no(ctx, PH_TCL_NO_UNIT_REMAP);
   if (!ctx->tcl_available || ctx->tcl_forced_off || !ctx->vs ||
       (!ctx->vs->ff_transform && !ctx->vs->hw_program) || !ctx->velems ||
       ctx->derived_triangles ||
       !ctx->have_viewport || !ctx->fb.nr_cbufs || !ctx->fb.cbufs[0].texture)
      return ph_tcl_no(ctx, PH_TCL_NO_UNAVAILABLE);
   if (ph_two_sided_stencil(ctx))
      return ph_tcl_no(ctx, PH_TCL_NO_TWO_SIDED_STENCIL);
   lay->program = !ctx->vs->ff_transform;
   /* A compiled program always writes o1, so the fragment stage has an
    * interpolated colour whether the shader computed one or not. */
   colour = lay->program || ctx->vs->colour_input >= 0;
   if (rs && rs->clip_plane_enable)
      return ph_tcl_no(ctx, PH_TCL_NO_CLIP_PLANES);
   if (rs && (rs->offset_tri || rs->offset_line || rs->offset_point))
      return ph_tcl_no(ctx, PH_TCL_NO_POLYGON_OFFSET);
   if (rs && rs->flatshade)
      return ph_tcl_no(ctx, PH_TCL_NO_FLATSHADE);
   if (rs && (rs->rasterizer_discard ||
              rs->clip_halfz || !rs->depth_clip_near || !rs->depth_clip_far ||
              rs->fill_front != PIPE_POLYGON_MODE_FILL ||
              rs->fill_back != PIPE_POLYGON_MODE_FILL ||
              /* no packet encoding culls both faces; the draw module does */
              rs->cull_face == PIPE_FACE_FRONT_AND_BACK))
      return ph_tcl_no(ctx, PH_TCL_NO_RASTER_STATE);
   /* v1 - the secondary colour or the fog coordinate - is carried only by
    * the window-space stream. */
   if (ctx->fs && ctx->fs->program.valid && ctx->fs->program.uses_color1)
      return ph_tcl_no(ctx, PH_TCL_NO_COLOUR1);
   /* A generic varying routed into v0 is not what the program writes to o1. */
   if (ctx->fs && ctx->fs->program.valid && ctx->fs->program.route[0] >= 0)
      return ph_tcl_no(ctx, PH_TCL_NO_PROGRAM_IO);
   /* An untextured colour with a visible alpha needs the RGBA stream: its
    * transforming form drew right in --colour-alpha-probe but stopped the
    * engine on a 100000-vertex GL_POLYGON, so the draw module stages it
    * unless PARHELIA_TCL_RGBA=1 asks for the transform route. */
   const bool rgba = !textured && ph_colour_alpha_observable(ctx);
   if (rgba && !debug_get_option_tcl_rgba())
      return ph_tcl_no(ctx, PH_TCL_NO_RGBA_COLOUR);
   /* The alpha routes below re-shape or replace the vertex stream. */
   if (ph_use_vertex_alpha(ctx, textured, multitextured, depth, colour) ||
       ph_use_alpha_micro(ctx, textured, multitextured, depth, colour) ||
       ph_use_alpha_micro_cutout(ctx, textured, multitextured, depth, colour) ||
       ph_use_cpu_alpha_cutout(ctx, textured, multitextured, depth, colour) ||
       ph_use_rgb_alpha_two_pass(ctx, textured, multitextured, colour) ||
       ph_use_native_sampled_alpha(ctx, textured, multitextured, depth))
      return ph_tcl_no(ctx, PH_TCL_NO_ALPHA_ROUTE);
   if (lay->program) {
      /* The program may only read fetch slots this stream declares, and the
       * hardware drops coordinate set 1 unless o4 is written. */
      unsigned available = (1u << P3D_WARP_V_POSITION) |
                           (1u << P3D_WARP_V_COLOUR) |
                           (textured ? 1u << P3D_WARP_V_TEXCOORD0 : 0) |
                           (multitextured ? 1u << (P3D_WARP_V_TEXCOORD0 + 1) :
                                            0);
      unsigned needed = (textured ? 1u << 3 : 0) |
                        (multitextured ? 1u << 4 : 0);

      if ((ctx->vs->prog.input_mask & ~available) ||
          (ctx->vs->prog.output_mask & needed) != needed)
         return ph_tcl_no(ctx, PH_TCL_NO_PROGRAM_IO);
      if (!ph_vs_upload_program(ctx, ctx->vs))
         return ph_tcl_no(ctx, PH_TCL_NO_UNAVAILABLE);
   } else if ((textured && ctx->vs->texcoord_input[0] < 0) ||
              (multitextured && ctx->vs->texcoord_input[1] < 0)) {
      return ph_tcl_no(ctx, PH_TCL_NO_TEXCOORD);
   }
   /* A two-set transformed draw wedges the engine: the GL test's two-unit
    * scene stopped the pipeline dead on 2026-09-11 (async fence 0x180 made no
    * progress for 2001 ms) while the untextured and single-texture scenes
    * were pixel-identical to the software path.  Keep the route behind an
    * explicit opt-in until that is understood. */
   if (multitextured && !ctx->tcl_multitexture)
      return ph_tcl_no(ctx, PH_TCL_NO_MULTITEXTURE);
   if (!lay->program &&
       ((multitextured && !ctx->warp_transform_multitex) ||
        (textured && !ctx->warp_transform_textured)))
      return ph_tcl_no(ctx, PH_TCL_NO_UNAVAILABLE);
   /* A three-coordinate set does not fit the transform program's stream. */
   if (textured && ph_three_coord_texture(ctx))
      return ph_tcl_no(ctx, PH_TCL_NO_THREE_COORD);

   lay->textured = textured;
   lay->multitextured = multitextured;
   lay->colour_alpha = rgba;
   lay->packed_colour = false;
   lay->colour_offset = 3;
   lay->tex_offset[0] = 6;
   lay->tex_offset[1] = 8;
   lay->dpv = 6 + rgba + (textured ? 2 : 0) + (multitextured ? 2 : 0);
   return true;
}

/* One translate element per attribute the WARP program consumes. */
static bool
ph_tcl_add_element(struct ph_context *ctx, struct translate_key *key,
                   int input, enum pipe_format output, unsigned offset,
                   unsigned max_components)
{
   const struct pipe_vertex_element *e;

   if (input < 0 || (unsigned)input >= ctx->velems->count)
      return false;
   e = &ctx->velems->elem[input];
   if (e->instance_divisor || e->dual_slot ||
       util_format_get_nr_components(e->src_format) > max_components ||
       e->vertex_buffer_index >= ctx->draw->pt.nr_vertex_buffers)
      return false;
   key->element[key->nr_elements].type = TRANSLATE_ELEMENT_NORMAL;
   key->element[key->nr_elements].input_format = e->src_format;
   key->element[key->nr_elements].output_format = output;
   key->element[key->nr_elements].input_buffer = e->vertex_buffer_index;
   key->element[key->nr_elements].input_offset = e->src_offset;
   key->element[key->nr_elements].instance_divisor = 0;
   key->element[key->nr_elements].output_offset = offset;
   key->nr_elements++;
   return true;
}

static uint32_t *
ph_tcl_elts(struct ph_context *ctx, unsigned count)
{
   if (ctx->tcl_elts_cap < count) {
      uint32_t *elts = realloc(ctx->tcl_elts, count * sizeof(*elts));

      if (!elts)
         return NULL;
      ctx->tcl_elts = elts;
      ctx->tcl_elts_cap = count;
   }
   return ctx->tcl_elts;
}

static uint32_t
ph_tcl_index(const void *indices, unsigned size, unsigned i, int bias)
{
   uint32_t v = size == 1 ? ((const uint8_t *)indices)[i] :
                size == 2 ? ((const uint16_t *)indices)[i] :
                            ((const uint32_t *)indices)[i];
   return v + bias;
}

/* Expand the draw into an independent-triangle index list.  The hardware draws
 * one triangle per packet, so strips, fans and quads are resolved here rather
 * than by a draw-module stage. */
static unsigned
ph_tcl_expand(struct ph_context *ctx, const struct pipe_draw_info *info,
              const struct pipe_draw_start_count_bias *draw, uint32_t **out,
              uint32_t range[2])
{
   const void *indices = NULL;
   unsigned size = info->index_size, count = draw->count, ntri, n = 0;
   uint32_t *elts;

   switch (info->mode) {
   case MESA_PRIM_TRIANGLES:
      ntri = count / 3;
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_POLYGON:
      ntri = count >= 3 ? count - 2 : 0;
      break;
   case MESA_PRIM_QUADS:
      ntri = (count / 4) * 2;
      break;
   case MESA_PRIM_QUAD_STRIP:
      ntri = count >= 4 ? (count / 2 - 1) * 2 : 0;
      break;
   default:
      return 0;
   }
   if (!ntri || ntri > UINT_MAX / 3)
      return 0;
   if (size) {
      indices = info->has_user_indices ? info->index.user :
                (info->index.resource ?
                 ph_resource(info->index.resource)->malloc_data : NULL);
      if (!indices)
         return 0;
      indices = (const uint8_t *)indices + (size_t)draw->start * size;
   }
   elts = ph_tcl_elts(ctx, ntri * 3);
   if (!elts)
      return 0;

#define PH_ELT(i) (size ? ph_tcl_index(indices, size, (i), draw->index_bias) : \
                          draw->start + (i))
   switch (info->mode) {
   case MESA_PRIM_TRIANGLES:
      for (unsigned i = 0; i < ntri * 3; i++)
         elts[n++] = PH_ELT(i);
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      for (unsigned i = 0; i < ntri; i++) {
         /* An odd strip triangle swaps its first two vertices so the
          * independent triangle keeps the strip's winding. */
         elts[n++] = PH_ELT(i & 1 ? i + 1 : i);
         elts[n++] = PH_ELT(i & 1 ? i : i + 1);
         elts[n++] = PH_ELT(i + 2);
      }
      break;
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_POLYGON:
      for (unsigned i = 0; i < ntri; i++) {
         elts[n++] = PH_ELT(0);
         elts[n++] = PH_ELT(i + 1);
         elts[n++] = PH_ELT(i + 2);
      }
      break;
   case MESA_PRIM_QUADS:
      /* Split on the same diagonal as the draw module, so a quad that the
       * clip predicate hands back interpolates identically. */
      for (unsigned q = 0; q < ntri / 2; q++) {
         elts[n++] = PH_ELT(q * 4);
         elts[n++] = PH_ELT(q * 4 + 1);
         elts[n++] = PH_ELT(q * 4 + 3);
         elts[n++] = PH_ELT(q * 4 + 1);
         elts[n++] = PH_ELT(q * 4 + 2);
         elts[n++] = PH_ELT(q * 4 + 3);
      }
      break;
   case MESA_PRIM_QUAD_STRIP:
      /* Quad q has corners 2q, 2q+1, 2q+3, 2q+2 in that order. */
      for (unsigned q = 0; q < ntri / 2; q++) {
         elts[n++] = PH_ELT(q * 2);
         elts[n++] = PH_ELT(q * 2 + 1);
         elts[n++] = PH_ELT(q * 2 + 2);
         elts[n++] = PH_ELT(q * 2 + 1);
         elts[n++] = PH_ELT(q * 2 + 3);
         elts[n++] = PH_ELT(q * 2 + 2);
      }
      break;
   default:
      return 0;
   }
#undef PH_ELT
   /* the vertices the elements can reference, from the draw where it knows */
   if (!size) {
      range[0] = draw->start;
      range[1] = draw->start + count - 1;
   } else if (info->index_bounds_valid &&
              (int64_t)info->min_index + draw->index_bias >= 0 &&
              (int64_t)info->max_index + draw->index_bias <= UINT32_MAX) {
      range[0] = info->min_index + draw->index_bias;
      range[1] = info->max_index + draw->index_bias;
   } else {
      range[0] = UINT32_MAX;
      range[1] = 0;
      for (unsigned i = 0; i < n; i++) {
         range[0] = MIN2(range[0], elts[i]);
         range[1] = MAX2(range[1], elts[i]);
      }
   }
   *out = elts;
   return ntri;
}

/* Draw the triangles [first, ntri) of an expanded list through the software
 * pipeline, used when a chunk does not fit the hardware clip volume. */
static void
ph_tcl_software_tail(struct ph_context *ctx, const uint32_t *elts,
                     unsigned first, unsigned ntri)
{
   struct pipe_draw_info info = {0};
   struct pipe_draw_start_count_bias draw = {0};

   info.mode = MESA_PRIM_TRIANGLES;
   info.index_size = 4;
   info.has_user_indices = true;
   info.index.user = elts;
   info.instance_count = 1;
   draw.start = first * 3;
   draw.count = (ntri - first) * 3;
   ctx->tcl_clip_fallbacks++;
   ctx->tcl_clip_verts += draw.count;
   draw_set_indexes(ctx->draw, elts, 4, ~0u);
   draw_vbo(ctx->draw, &info, 0, NULL, &draw, 1, 0);
   draw_flush(ctx->draw);
   draw_set_indexes(ctx->draw, NULL, 0, 0);
}

/* The fixed-function inputs - float vectors and RGBA8 colours - copied
 * straight into the stream.  The generic translate runs every element through
 * format callbacks, which costs more than the copy on a slow CPU; the result
 * is the same, byte for byte. */
/* Order keys of the positions a fetch copied (ph_float_order_key). */
struct ph_tcl_box {
   int32_t lo[3], hi[3];
   uint32_t nan;
};

struct ph_tcl_fetch {
   unsigned count;
   int box_elem;                   /* the position element, or -1 */
   struct {
      const uint8_t *src;          /* buffer start plus element offset */
      unsigned stride;
      unsigned out;                /* output dword */
      unsigned comps;
      bool unorm8;
      bool packed;                 /* RGBA8 to one D3DCOLOR dword */
      uint32_t alpha;              /* or-ed into a packed colour */
   } e[PIPE_MAX_ATTRIBS];
};

static bool
ph_tcl_fetch_init(struct ph_tcl_fetch *f, const struct translate_key *key,
                  const uint8_t *const *maps, const unsigned *strides)
{
   f->count = 0;
   f->box_elem = -1;
   for (unsigned i = 0; i < key->nr_elements; i++) {
      const struct translate_element *el = &key->element[i];
      const uint8_t *src = maps[el->input_buffer] + el->input_offset;
      unsigned in, out;
      bool unorm8 = false;

      switch (el->output_format) {
      case PIPE_FORMAT_R32G32B32A32_FLOAT: out = 4; break;
      case PIPE_FORMAT_R32G32B32_FLOAT: out = 3; break;
      case PIPE_FORMAT_R32G32_FLOAT: out = 2; break;
      default: return false;
      }
      switch (el->input_format) {
      case PIPE_FORMAT_R32G32B32A32_FLOAT: in = 4; break;
      case PIPE_FORMAT_R32G32B32_FLOAT: in = 3; break;
      case PIPE_FORMAT_R32G32_FLOAT: in = 2; break;
      case PIPE_FORMAT_R8G8B8A8_UNORM: in = 4; unorm8 = true; break;
      default: return false;
      }
      /* Missing components take defaults; an unaligned float load traps. */
      if (el->type != TRANSLATE_ELEMENT_NORMAL || in < out ||
          el->output_offset % 4 ||
          (!unorm8 && (((uintptr_t)src | strides[el->input_buffer]) & 3)))
         return false;
      f->e[f->count].src = src;
      f->e[f->count].stride = strides[el->input_buffer];
      f->e[f->count].out = el->output_offset / 4;
      f->e[f->count].comps = out;
      f->e[f->count].unorm8 = unorm8;
      f->e[f->count].packed = false;
      /* the position: object-space x,y,z at dword 0 */
      if (!el->output_offset && out == 3 && !unorm8 && f->box_elem < 0)
         f->box_elem = f->count;
      f->count++;
   }
   return true;
}

/* The position copy, bounding what it copies on the way. */
static void
ph_tcl_fetch_position_box(const uint8_t *base, size_t stride,
                          const uint32_t *elts, unsigned count, uint32_t *d,
                          unsigned dpv, struct ph_tcl_box *box)
{
   int32_t lo0 = box->lo[0], lo1 = box->lo[1], lo2 = box->lo[2];
   int32_t hi0 = box->hi[0], hi1 = box->hi[1], hi2 = box->hi[2];
   uint32_t nan = box->nan;

   for (unsigned v = 0; v < count; v++, d += dpv) {
      const uint32_t *s = (const uint32_t *)(base + elts[v] * stride);
      const uint32_t x = s[0], y = s[1], z = s[2];
      const int32_t kx = ph_float_order_key(x), ky = ph_float_order_key(y);
      const int32_t kz = ph_float_order_key(z);

      d[0] = x;
      d[1] = y;
      d[2] = z;
      nan |= ((x & 0x7fffffffu) > 0x7f800000u) |
             ((y & 0x7fffffffu) > 0x7f800000u) |
             ((z & 0x7fffffffu) > 0x7f800000u);
      lo0 = kx < lo0 ? kx : lo0;
      hi0 = kx > hi0 ? kx : hi0;
      lo1 = ky < lo1 ? ky : lo1;
      hi1 = ky > hi1 ? ky : hi1;
      lo2 = kz < lo2 ? kz : lo2;
      hi2 = kz > hi2 ? kz : hi2;
   }
   box->lo[0] = lo0; box->lo[1] = lo1; box->lo[2] = lo2;
   box->hi[0] = hi0; box->hi[1] = hi1; box->hi[2] = hi2;
   box->nan = nan;
}

/* The bits of ubyte_to_float() for every byte: on Alpha each float
 * arithmetic instruction carries a trap barrier, a table load none. */
static uint32_t ph_unorm8_bits[256];

static void
ph_unorm8_bits_init(void)
{
   for (unsigned i = 0; i < 256; i++)
      ph_unorm8_bits[i] = fui(ubyte_to_float(i));
}

/* Element-major: one specialised loop per element, so the per-vertex work is
 * the copy alone. */
static void
ph_tcl_fetch_elements(const struct ph_tcl_fetch *f, const uint32_t *elts,
                      unsigned count, uint32_t *dst, unsigned dpv)
{
   for (unsigned i = 0; i < f->count; i++) {
      const uint8_t *base = f->e[i].src;
      const size_t stride = f->e[i].stride;
      uint32_t *d = dst + f->e[i].out;

      if (f->e[i].packed) {
         const uint32_t alpha = f->e[i].alpha;

         for (unsigned v = 0; v < count; v++, d += dpv) {
            uint32_t c;

            memcpy(&c, base + elts[v] * stride, 4);
            d[0] = (c & 0xff00ff00u) | ((c >> 16) & 0xffu) |
                   ((c & 0xffu) << 16) | alpha;
         }
         continue;
      }
      if (f->e[i].unorm8) {
         for (unsigned v = 0; v < count; v++, d += dpv) {
            const uint8_t *s = base + elts[v] * stride;

            d[0] = ph_unorm8_bits[s[0]];
            d[1] = ph_unorm8_bits[s[1]];
            if (f->e[i].comps > 2)
               d[2] = ph_unorm8_bits[s[2]];
            if (f->e[i].comps > 3)
               d[3] = ph_unorm8_bits[s[3]];
         }
         continue;
      }
      switch (f->e[i].comps) {
      case 2:
         for (unsigned v = 0; v < count; v++, d += dpv) {
            const uint32_t *s = (const uint32_t *)(base + elts[v] * stride);

            d[0] = s[0];
            d[1] = s[1];
         }
         break;
      case 3:
         for (unsigned v = 0; v < count; v++, d += dpv) {
            const uint32_t *s = (const uint32_t *)(base + elts[v] * stride);

            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
         }
         break;
      default:
         for (unsigned v = 0; v < count; v++, d += dpv) {
            const uint32_t *s = (const uint32_t *)(base + elts[v] * stride);

            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
         }
         break;
      }
   }
}

/* Vertices per element-major block: the block's output stays cached between
 * its element passes even on an 8 KB first-level cache. */
#define PH_TCL_FETCH_BLOCK 48u

static void
ph_tcl_fetch_run(const struct ph_tcl_fetch *f, const uint32_t *elts,
                 unsigned count, float *dst, unsigned dpv)
{
   for (unsigned b = 0; b < count; b += PH_TCL_FETCH_BLOCK)
      ph_tcl_fetch_elements(f, elts + b, MIN2(count - b, PH_TCL_FETCH_BLOCK),
                            (uint32_t *)dst + (size_t)b * dpv, dpv);
}

/* The fetch with the position copy bounding what it copies; whether the
 * chunk's box lies inside the clip volume and guard band.  Kept out of line
 * so the default fetch in ph_draw_tcl compiles as without it. */
static bool ATTRIBUTE_NOINLINE
ph_tcl_fetch_run_boxed(const struct ph_context *ctx,
                       const struct ph_tcl_fetch *f, const uint32_t *elts,
                       unsigned count, float *dst, unsigned dpv,
                       const float *mvp)
{
   struct ph_tcl_box box = {
      .lo = { INT32_MAX, INT32_MAX, INT32_MAX },
      .hi = { INT32_MIN, INT32_MIN, INT32_MIN },
   };
   float lo[3], hi[3];
   struct ph_tcl_fetch rest = *f;

   /* the bounded copy replaces the position element's plain one */
   rest.e[f->box_elem] = rest.e[--rest.count];
   for (unsigned b = 0; b < count; b += PH_TCL_FETCH_BLOCK) {
      const unsigned n = MIN2(count - b, PH_TCL_FETCH_BLOCK);
      uint32_t *d = (uint32_t *)dst + (size_t)b * dpv;

      ph_tcl_fetch_position_box(f->e[f->box_elem].src,
                                f->e[f->box_elem].stride, elts + b, n,
                                d + f->e[f->box_elem].out, dpv, &box);
      ph_tcl_fetch_elements(&rest, elts + b, n, d, dpv);
   }
   if (box.nan)
      return false;
   for (unsigned c = 0; c < 3; c++) {
      lo[c] = ph_float_from_order_key(box.lo[c]);
      hi[c] = ph_float_from_order_key(box.hi[c]);
   }
   return ph_tcl_box_fits(ctx, mvp, NULL, lo, hi);
}

/* A stream without a colour input takes the fragment stage's constant one. */
static void
ph_tcl_constant_colour(struct ph_context *ctx, const struct ph_tcl_layout *lay,
                       float *dst, unsigned count)
{
   float constant[4];
   bool have;

   if (lay->program ?
          ctx->vs->prog.input_mask & (1u << P3D_WARP_V_COLOUR) :
          ctx->vs->colour_input >= 0)
      return;
   have = ph_fs_constant_colour(ctx, constant);
   for (unsigned v = 0; v < count; v++) {
      float *c = dst + v * lay->dpv + lay->colour_offset;

      c[0] = have ? constant[0] : 1.0f;
      c[1] = have ? constant[1] : 1.0f;
      c[2] = have ? constant[2] : 1.0f;
      if (lay->colour_alpha)
         c[3] = have ? constant[3] : 1.0f;
   }
}

/* An indexed draw: the vertices of the draw's index range staged once and
 * its indices, rebased to that range, behind them, all of it one batch.
 * False when indexing would not stage less or does not fit; the caller then
 * expands the triangles. */
static bool
ph_tcl_draw_indexed(struct ph_context *ctx, const struct ph_tcl_layout *lay,
                    const struct ph_tcl_fetch *fetch, const uint32_t *elts,
                    unsigned ntri, uint32_t entry, unsigned vs_const_vec4s)
{
   uint32_t lo = UINT32_MAX, hi = 0, *idx;
   unsigned nverts;
   float *dst;

   for (unsigned i = 0; i < ntri * 3; i++) {
      lo = MIN2(lo, elts[i]);
      hi = MAX2(hi, elts[i]);
   }
   nverts = hi - lo + 1;
   if (nverts >= ntri * 3)
      return false;
   if (ctx->triangles) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return true;
      ctx->transform = true;
      ctx->warp_program_entry = entry;
      ctx->vs_const_vec4s = vs_const_vec4s;
      ctx->colour_alpha = lay->colour_alpha;
      ctx->packed_colour = lay->packed_colour;
   }
   if ((size_t)nverts * lay->dpv + ntri * 3 > ph_logical_vertex_limit(ctx))
      return false;
   if (ctx->tcl_seq_cap < nverts) {
      uint32_t *grown = realloc(ctx->tcl_seq, nverts * sizeof(*grown));

      if (!grown)
         return false;
      ctx->tcl_seq = grown;
      ctx->tcl_seq_cap = nverts;
   }
   for (unsigned v = 0; v < nverts; v++)
      ctx->tcl_seq[v] = lo + v;
   dst = ctx->vertices;
   ph_tcl_fetch_run(fetch, ctx->tcl_seq, nverts, dst, lay->dpv);
   ph_tcl_constant_colour(ctx, lay, dst, nverts);
   idx = (uint32_t *)dst + (size_t)nverts * lay->dpv;
   for (unsigned i = 0; i < ntri * 3; i++)
      idx[i] = elts[i] - lo;
   ctx->triangles = ntri;
   ctx->indexed_verts = nverts;
   ph_submit_vertices(ctx);
   return true;
}

/* Fill the object-space vertex stream for one draw and, when every vertex
 * lands inside the hardware clip volume, keep it for a transforming batch.
 * Returns false when the caller must use the software pipeline instead. */
static bool
ph_draw_tcl(struct ph_context *ctx, const struct pipe_draw_info *info,
            const struct pipe_draw_start_count_bias *draw)
{
   struct ph_tcl_layout lay;
   struct translate_key key = {0};
   struct translate *translate = NULL;
   struct ph_tcl_fetch fetch;
   const uint8_t *maps[PIPE_MAX_ATTRIBS];
   unsigned strides[PIPE_MAX_ATTRIBS];
   uint32_t *elts = NULL, range[2];
   unsigned ntri, done = 0, limit;
   unsigned vs_const_vec4s = 0;
   uint32_t entry = 0;
   float mvp[16];
   bool box_fits, fused_box, chunks_fit = true;

   ctx->tcl_last_reason = PH_TCL_NO_DRAW_SHAPE;
   if (info->primitive_restart || info->instance_count != 1 ||
       !ph_tcl_layout(ctx, &lay))
      goto fallback;
   /* A position slice needs no matrix: the program brings its constants. */
   if (lay.program && ctx->vs->pos_slice)
      memset(mvp, 0, sizeof(mvp));
   else if (!ph_tcl_matrix(ctx, mvp))
      goto fallback;
   /* An unsampleable native import can only be presented by copy, which
    * matches the window-space quad of the software path. */
   if (ph_samples_unit0(ctx)) {
      const struct parhelia_resource *t =
         ph_resource(ctx->sampler_views[0]->texture);

      if (t->native_shared && !t->linear_handle &&
          !ph_auto_tiled_texture(ctx, t, NULL))
         goto fallback;
   }
   if (lay.program) {
      entry = ctx->vs->prog_entry;
      if (!ph_tcl_program_constants(ctx, ctx->vs, &vs_const_vec4s)) {
         ctx->tcl_last_reason = PH_TCL_NO_CONSTANTS;
         goto fallback;
      }
   }

   key.output_stride = lay.dpv * sizeof(float);
   if (lay.program) {
      /* One element per fetch slot the compiled program reads. */
      for (unsigned i = 0; i < PIPE_MAX_ATTRIBS; i++) {
         int slot = ctx->vs->prog_inputs.slot[i];
         const struct ph_tcl_slot *desc;

         if (slot < 0 || !(ctx->vs->prog.input_mask & (1u << slot)))
            continue;
         desc = ph_tcl_slot_of((unsigned)slot);
         if (!desc ||
             !ph_tcl_add_element(ctx, &key, (int)i,
                                 lay.colour_alpha &&
                                    desc->slot == P3D_WARP_V_COLOUR ?
                                    PIPE_FORMAT_R32G32B32A32_FLOAT :
                                    desc->format,
                                 desc->offset * sizeof(float),
                                 desc->max_components))
            goto fallback;
      }
   } else {
      if (!ph_tcl_add_element(ctx, &key, ctx->vs->pos_input,
                              PIPE_FORMAT_R32G32B32_FLOAT, 0, 3))
         goto fallback;
      if (ctx->vs->colour_input >= 0 &&
          !ph_tcl_add_element(ctx, &key, ctx->vs->colour_input,
                              lay.colour_alpha ?
                                 PIPE_FORMAT_R32G32B32A32_FLOAT :
                                 PIPE_FORMAT_R32G32B32_FLOAT,
                              lay.colour_offset * sizeof(float), 4))
         goto fallback;
      for (unsigned u = 0; u < 2; u++) {
         if (!(u ? lay.multitextured : lay.textured))
            continue;
         if (!ph_tcl_add_element(ctx, &key, ctx->vs->texcoord_input[u],
                                 PIPE_FORMAT_R32G32_FLOAT,
                                 lay.tex_offset[u] * sizeof(float), 2))
            goto fallback;
      }
   }
   ntri = ph_tcl_expand(ctx, info, draw, &elts, range);
   if (!ntri)
      goto fallback;
   if (ctx->stats)
      ctx->tcl_range_verts += range[1] - range[0] + 1;
   if (!ctx->tcl_cache) {
      ctx->tcl_cache = translate_cache_create();
      if (!ctx->tcl_cache)
         goto fallback;
   }
   for (unsigned i = 0; i < key.nr_elements; i++) {
      unsigned b = key.element[i].input_buffer;
      const struct pipe_vertex_buffer *vb = &ctx->draw->pt.vertex_buffer[b];
      const uint8_t *map = vb->is_user_buffer ? vb->buffer.user :
                           (vb->buffer.resource ?
                            ph_resource(vb->buffer.resource)->malloc_data :
                            NULL);

      if (!map)
         goto fallback;
      maps[b] = map + vb->buffer_offset;
      strides[b] = 0;
      for (unsigned e = 0; e < ctx->velems->count; e++)
         if (ctx->velems->elem[e].vertex_buffer_index == b)
            strides[b] = ctx->velems->elem[e].src_stride;
   }
   if (!ph_tcl_fetch_init(&fetch, &key, maps, strides)) {
      fetch.count = 0;
      translate = translate_cache_find(ctx->tcl_cache, &key);
      if (!translate)
         goto fallback;
      for (unsigned i = 0; i < key.nr_elements; i++) {
         unsigned b = key.element[i].input_buffer;

         translate->set_buffer(translate, b, maps[b], strides[b], ~0u);
      }
   } else if (ctx->packed_colour_fetch && !lay.program) {
      /* An RGBA8 colour goes to the fetch as it is, one dword instead of
       * three or four floats; an unobservable alpha reads as one, as the
       * float RGB fetch's does. */
      const unsigned saved = 2 + lay.colour_alpha;

      for (unsigned i = 0; i < fetch.count && !lay.packed_colour; i++)
         if (fetch.e[i].unorm8 && fetch.e[i].out == lay.colour_offset) {
            fetch.e[i].packed = true;
            fetch.e[i].alpha = lay.colour_alpha ? 0 : 0xff000000u;
            lay.packed_colour = true;
         }
      if (lay.packed_colour) {
         for (unsigned i = 0; i < fetch.count; i++)
            if (fetch.e[i].out > lay.colour_offset)
               fetch.e[i].out -= saved;
         lay.tex_offset[0] -= saved;
         lay.tex_offset[1] -= saved;
         lay.dpv -= saved;
      }
   }

   /* Mixed primitive kinds keep their order, and a batch carrying
    * window-space or differently transformed vertices ends here.  A batch
    * binds one WARP program and publishes one constant block, so a change of
    * either ends it too. */
   if (ctx->points || ctx->lines ||
       (ctx->triangles &&
        (!ctx->transform || ctx->colour_alpha != lay.colour_alpha ||
         ctx->packed_colour != lay.packed_colour ||
         memcmp(ctx->mvp, mvp, sizeof mvp) ||
         ctx->warp_program_entry != entry ||
         ctx->vs_const_vec4s != vs_const_vec4s ||
         (vs_const_vec4s &&
          memcmp(ctx->vs_const, ctx->vs_const_pending,
                 vs_const_vec4s * 16u)))))
      ph_submit_vertices(ctx);
   if (ctx->submit_error)
      return true;
   memcpy(ctx->mvp, mvp, sizeof mvp);
   if (vs_const_vec4s) {
      float *swap = ctx->vs_const;

      ctx->vs_const = ctx->vs_const_pending;
      ctx->vs_const_pending = swap;
   }
   ctx->vs_const_vec4s = vs_const_vec4s;
   ctx->vp_slot = lay.program ? ctx->vs->prog.vp_scale_slot : 0;
   ctx->warp_program_entry = entry;
   ctx->transform = true;
   ctx->colour_alpha = lay.colour_alpha;
   ctx->packed_colour = lay.packed_colour;
   ctx->tcl_draws++;
   ctx->tcl_verts += draw->count;
   if (lay.program)
      ctx->tcl_program_draws++;

   /* A box of all referenced positions inside the clip volume clears the
    * whole draw at once; otherwise each chunk is tested per vertex. */
   {
      float lo[3], hi[3];

      struct ph_slice *slice = lay.program ? ctx->vs->pos_slice : NULL;

      /* An affine position slice that reads only the position input is
       * bounded by its corners just like a matrix product. */
      /* The vendor tests every vertex.  A box inside the volume implies
       * every vertex is, and clears 113 of 125 ioquake3 draws per frame on
       * the Alpha (6.9 -> 7.2 fps); PARHELIA_TCL_BOX=0 tests each vertex. */
      const bool box = debug_get_option_tcl_box() &&
                 (!slice || (slice->affine && ctx->vs->pos_input >= 0 &&
                             ctx->vs->pos_input < 32 &&
                             slice->input_mask == 1u << ctx->vs->pos_input));

      /* A buffer the box cache cannot hold is bounded chunk by chunk while
       * the fixed-function fetch copies its positions. */
      fused_box = box && debug_get_option_tcl_fused_box() && !lay.program &&
                  fetch.count && fetch.box_elem >= 0 &&
                  ph_tcl_position_uncached(ctx);
      box_fits = box && !fused_box &&
                 ph_tcl_position_box(ctx, range, lo, hi) &&
                 ph_tcl_box_fits(ctx, mvp, slice, lo, hi);
      if (box_fits)
         ctx->tcl_box_draws++;
      else if (lay.program && ctx->vs->pos_slice)
         ctx->tcl_slice_draws++;
   }
   limit = ph_triangle_batch_limit(ctx);
   if (ph_screen(ctx->base.screen)->indexed_tris && box_fits && fetch.count &&
       ntri <= limit &&
       ph_tcl_draw_indexed(ctx, &lay, &fetch, elts, ntri, entry,
                           vs_const_vec4s))
      return true;
   while (done < ntri) {
      unsigned room = ph_logical_vertex_limit(ctx) / (3 * lay.dpv);
      unsigned space = MIN2(limit, room);
      unsigned chunk;
      float *dst;

      if (ctx->triangles >= space) {
         ph_submit_vertices(ctx);
         if (ctx->submit_error)
            return true;
         continue;
      }
      /* A submit inside the draw cleared the batch's program state. */
      ctx->transform = true;
      ctx->warp_program_entry = entry;
      ctx->vs_const_vec4s = vs_const_vec4s;
      ctx->colour_alpha = lay.colour_alpha;
      ctx->packed_colour = lay.packed_colour;
      chunk = MIN2(ntri - done, space - ctx->triangles);
      dst = &ctx->vertices[ctx->triangles * 3 * lay.dpv];
      bool chunk_fits = box_fits;

      if (fused_box)
         chunk_fits = ph_tcl_fetch_run_boxed(ctx, &fetch, elts + done * 3,
                                             chunk * 3, dst, lay.dpv, mvp);
      else if (fetch.count)
         ph_tcl_fetch_run(&fetch, elts + done * 3, chunk * 3, dst, lay.dpv);
      else
         translate->run_elts(translate, elts + done * 3, chunk * 3, 0, 0,
                             dst);
      ph_tcl_constant_colour(ctx, &lay, dst, chunk * 3);
      chunks_fit &= chunk_fits;
      if (!chunk_fits && !ph_tcl_vertices_fit(ctx, dst, chunk * 3, lay.dpv)) {
         /* A fixed-function transform clips the crossing triangles itself. */
         if (!lay.program && lay.dpv <= PH_TCL_MAX_DPV &&
             debug_get_option_tcl_clip()) {
            const struct ph_tcl_run run = {
               .dpv = lay.dpv, .limit = limit,
               .vs_const_vec4s = vs_const_vec4s, .entry = entry,
               .colour_alpha = lay.colour_alpha,
               .packed_colour = lay.packed_colour ?
                                (int)lay.colour_offset : -1,
               .gx = (PH_TCL_GUARD_BAND - fabsf(ctx->viewport.translate[0])) /
                     MAX2(fabsf(ctx->viewport.scale[0]), 1e-6f),
               .gy = (PH_TCL_GUARD_BAND - fabsf(ctx->viewport.translate[1])) /
                     MAX2(fabsf(ctx->viewport.scale[1]), 1e-6f),
               .cx = 1.0f + PH_TCL_CLIP_MARGIN /
                     MAX2(fabsf(ctx->viewport.scale[0]), 1e-6f),
               .cy = 1.0f + PH_TCL_CLIP_MARGIN /
                     MAX2(fabsf(ctx->viewport.scale[1]), 1e-6f),
            };

            /* only the copy's allocation fails before appending anything */
            if (run.gx > run.cx && run.gy > run.cy &&
                (ph_tcl_clip_chunk(ctx, &run, dst, chunk) ||
                 ctx->submit_error)) {
               if (ctx->submit_error)
                  return true;
               done += chunk;
               continue;
            }
         }
         /* Publish what already fitted, then let the software clipper own
          * the rest of this draw. */
         if (ctx->triangles)
            ph_submit_vertices(ctx);
         ctx->transform = false;
         if (!ctx->submit_error)
            ph_tcl_software_tail(ctx, elts, done, ntri);
         return true;
      }
      ctx->triangles += chunk;
      done += chunk;
   }
   if (fused_box && chunks_fit)
      ctx->tcl_box_draws++;
   return true;
fallback:
   ctx->tcl_fallbacks++;
   ctx->tcl_reason[ctx->tcl_last_reason]++;
   ctx->tcl_reason_verts[ctx->tcl_last_reason] += draw->count;
   return false;
}

/* ------------------------------------------------ logic-op colour grid
 *
 * The pixel pipe applies the raster op to a colour wider than the target:
 * a source of k/255 XORs exactly into an 8-bit target for every k and every
 * destination, while 0.5 or 0.25 come out one code apart between two
 * identical draws, so XOR twice does not restore the image (MEASURED on the
 * P690, all 256x256 source/destination pairs).  GL converts the fragment
 * colour to the framebuffer's fixed point before the logic op, so vertex
 * colours are put on the target's grid while a non-COPY logic op is active.
 */
struct ph_quant_stage {
   struct draw_stage base;
   struct ph_context *ctx;
   bool quantize;
   int slot[2];
   float scale[4];
   /* Constant coordinates staged into extra vertex attributes. */
   unsigned coord_count;
   int coord_slot[P3D_MAX_TEXTURE_UNITS];
   float coord[P3D_MAX_TEXTURE_UNITS][4];
   bool coord_window[P3D_MAX_TEXTURE_UNITS];
   int win_slot;
   /* A constant window depth for every vertex (depth_uniform). */
   bool const_z;
   int pos_slot;
   float z;
   /* Rectangle coordinates scaled to the normalised ones the unit takes. */
   unsigned rect_count;
   int rect_slot[P3D_MAX_TEXTURE_UNITS];
   float rect_scale[P3D_MAX_TEXTURE_UNITS][2];
   /* Floor ramp coordinates: (x - lo) / span of the varying's components. */
   unsigned ramp_count;
   int ramp_slot[P3D_MAX_TEXTURE_UNITS], ramp_src[P3D_MAX_TEXTURE_UNITS];
   struct ph_fs_ramp ramp[P3D_MAX_TEXTURE_UNITS];
   /* Tap coordinates staged per vertex. */
   unsigned tap_count;
   int tap_slot[P3D_MAX_TEXTURE_UNITS];
   /* Generic varyings routed through the colour interpolators, staged in
    * their carrier's domain. */
   unsigned route_count;
   int route_slot[2], route_carrier[2];
   /* The draw's extra attributes were checked after the stages ahead. */
   bool attached;
};

static bool
ph_logicop_quantize(const struct ph_context *ctx)
{
   return ctx->logicop_grid && ctx->blend && ctx->blend->logicop_enable &&
          ctx->blend->logicop_func != PIPE_LOGICOP_COPY &&
          ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture;
}

/* The colour target's code count per GL channel; 0 leaves a channel alone. */
static void
ph_logicop_grid(const struct ph_context *ctx, float scale[4])
{
   const struct util_format_description *desc =
      util_format_description(ctx->fb.cbufs[0].format);

   for (unsigned c = 0; c < 4; c++) {
      const unsigned s = desc->swizzle[c];

      scale[c] = s <= PIPE_SWIZZLE_W && desc->channel[s].normalized &&
                 desc->channel[s].size ?
                 (float)((1u << desc->channel[s].size) - 1) : 0.0f;
   }
}

/* A coordinate unit: texture-address opcode 2 with mode 3 passes the
 * staged coordinate instead of a texel (vendor TEXCOORD, MEASURED by
 * parhelia-drm-test --texcoord-pass-probe). */
static void
ph_pass_unit_state(const struct ph_context *ctx, unsigned unit,
                   struct p3d_texture *t)
{
   const struct ph_fs_program *prog = ctx->fs ? &ctx->fs->program : NULL;

   for (unsigned r = 0; prog && prog->valid && r < prog->ramp_count; r++)
      if (prog->ramps[r].kind == PH_FS_RAMP_PASS && prog->ramps[r].unit == unit) {
         t->ta_op = 2;
         t->coord_mode = 3;
      }
}

static struct pipe_sampler_view *
ph_lookup_view(struct ph_context *ctx, const struct ph_fs_ramp *ramp);

/* A pixel lookup's domain: window position [0, 2^n) per dimension covering
 * the framebuffer, one texel per pixel. */
static void
ph_pixel_domain(const struct ph_context *ctx, const struct ph_fs_ramp *r,
                float lo[3], float span[3])
{
   const unsigned fb[2] = { ctx->fb.width, ctx->fb.height };

   for (unsigned d = 0; d < r->dims; d++) {
      const unsigned e = r->comp[d] < 2 ? fb[r->comp[d]] : 1;

      lo[d] = 0.0f;
      span[d] = (float)CLAMP(util_next_power_of_two(MAX2(e, 1)), 8, 2048);
   }
}

static void
ph_domain_reset(struct ph_context *ctx)
{
   for (unsigned k = 0; k < PH_FS_CARRIERS; k++)
      for (unsigned c = 0; c < 4; c++) {
         ctx->dom_lo[k][c] = 0.0f;
         ctx->dom_span[k][c] = 1.0f;
      }
}

/* [lo, lo + span) with a power-of-two span covering [lo, hi]; [0, 1] when
 * that already does.  The power of two keeps the scale an exact shift. */
static void
ph_domain_cover(float lo, float hi, float *out_lo, float *out_span)
{
   float span = 1.0f;

   if (lo >= 0.0f && hi <= 1.0f) {
      *out_lo = 0.0f;
      *out_span = 1.0f;
      return;
   }
   while (span < hi - lo && span < 65536.0f)
      span *= 2.0f;
   *out_lo = lo;
   *out_span = span;
}

/* Keep every routed value of @prim inside its carrier's domain: a value
 * outside submits what is staged and widens the domain, so each batch
 * interpolates one domain (the colour interpolators clamp to [0, 1]). */
static void
ph_quant_domain(struct ph_quant_stage *q, const struct prim_header *prim,
                unsigned n, int slot, unsigned k, const uint8_t *comp,
                unsigned channels)
{
   struct ph_context *ctx = q->ctx;
   float lo[4], hi[4];
   bool fits = true;

   for (unsigned c = 0; c < channels; c++) {
      lo[c] = hi[c] = prim->v[0]->data[slot][comp[c]];
      for (unsigned v = 1; v < n; v++) {
         const float x = prim->v[v]->data[slot][comp[c]];

         lo[c] = MIN2(lo[c], x);
         hi[c] = MAX2(hi[c], x);
      }
      fits &= lo[c] >= ctx->dom_lo[k][c] &&
              hi[c] <= ctx->dom_lo[k][c] + ctx->dom_span[k][c];
   }
   if (fits)
      return;
   if (ctx->triangles || ctx->points || ctx->lines)
      ph_submit_vertices(ctx);
   for (unsigned c = 0; c < channels; c++) {
      /* Grow over the previous domain: the next batch likely needs it. */
      const float l = MIN2(lo[c], ctx->dom_lo[k][c]);
      const float h = MAX2(hi[c], ctx->dom_lo[k][c] + ctx->dom_span[k][c]);

      if (!(l >= -1e30f && h <= 1e30f))
         continue;
      ph_domain_cover(l, h, &ctx->dom_lo[k][c], &ctx->dom_span[k][c]);
   }
}

/* Every unit of the program looks up a function of the window position
 * (gl_FragCoord): those coordinates are affine in window space, so the
 * draw takes the affine stream without 1/w. */
static bool
ph_fs_window_lookups_only(const struct ph_fs *fs)
{
   const struct ph_fs_program *prog = fs ? &fs->program : NULL;
   unsigned units = 0;

   if (!prog || !prog->valid || !prog->ramp_count || fs->tap_count)
      return false;
   for (unsigned r = 0; r < prog->ramp_count; r++) {
      const struct ph_fs_ramp *rp = &prog->ramps[r];

      if ((rp->kind != PH_FS_RAMP_SCALAR && rp->kind != PH_FS_RAMP_PLANE) ||
          rp->location != VARYING_SLOT_POS)
         return false;
      units |= 1u << rp->unit;
   }
   return units == prog->sampler_mask;
}

/* Keep every carried value of @prim inside its carrier's domain: a value
 * outside submits what is staged and widens the domain, so each batch
 * interpolates one domain (both carriers clamp to [0, 1]). */
static bool
ph_quant_domains(struct ph_quant_stage *q, const struct prim_header *prim,
                 unsigned n)
{
   static const uint8_t identity[4] = { 0, 1, 2, 3 };

   /* The colour interpolators are affine, as for gl_Color on this
    * hardware and in the vendor driver: a routed varying interpolates
    * affinely rather than the primitive being dropped.  Varyings the
    * compiler can give a coordinate unit are perspective-correct. */
   for (unsigned r = 0; r < q->route_count; r++)
      ph_quant_domain(q, prim, n, q->route_slot[r], q->route_carrier[r],
                      identity, 4);
   /* A lookup reading gl_PointCoord's derivatives is built for the batch's
    * sprites: new derivatives submit what is staged and rebuild it. */
   if (n == 3 && q->ctx->fs && q->ctx->fs->point_deriv && q->win_slot >= 0) {
      float d[4];

      ph_pntc_derivatives(q->ctx->draw, prim, q->win_slot, d);
      if (memcmp(d, q->ctx->pntc_deriv, sizeof(d))) {
         if (q->ctx->triangles || q->ctx->points || q->ctx->lines)
            ph_submit_vertices(q->ctx);
         memcpy(q->ctx->pntc_deriv, d, sizeof(d));
         for (unsigned i = 0; i < q->ramp_count; i++) {
            const struct ph_fs_ramp *r = &q->ramp[i];
            struct pipe_sampler_view *view;

            if (r->kind != PH_FS_RAMP_SCALAR && r->kind != PH_FS_RAMP_PLANE)
               continue;
            if (!(view = ph_lookup_view(q->ctx, r))) {
               ph_warn_once("no lookup table represents the colour for the "
                            "sprite size; the primitive is dropped\n");
               return false;
            }
            q->ctx->base.set_sampler_views(&q->ctx->base, MESA_SHADER_FRAGMENT,
                                           r->unit, 1, 0, &view);
         }
      }
   }
   for (unsigned i = 0; i < q->ramp_count; i++) {
      const struct ph_fs_ramp *r = &q->ramp[i];
      const unsigned k = 2 + r->unit;
      float lo[3], span[3];

      if (r->kind == PH_FS_RAMP_PASS && !r->saturated)
         ph_quant_domain(q, prim, n, q->ramp_src[i], k, r->comp, r->dims);
      if (r->kind != PH_FS_RAMP_SCALAR && r->kind != PH_FS_RAMP_PLANE)
         continue;
      memcpy(lo, q->ctx->dom_lo[k], sizeof(lo));
      memcpy(span, q->ctx->dom_span[k], sizeof(span));
      if (r->fn->pixel) {
         /* The unit interpolates perspective-correctly, the window
          * position affinely: they agree at equal w only, unless the
          * draw takes the affine stream. */
         for (unsigned v = 1; !ph_fs_window_lookups_only(q->ctx->fs) && v < n; v++)
            if (prim->v[v]->data[q->ramp_src[i]][3] !=
                prim->v[0]->data[q->ramp_src[i]][3]) {
               ph_warn_once("a lookup of the window position across corners "
                            "of different w is not exact; the primitive is "
                            "dropped\n");
               return false;
            }
         float plo[3], pspan[3];

         ph_pixel_domain(q->ctx, r, plo, pspan);
         if (memcmp(plo, lo, r->dims * sizeof(float)) ||
             memcmp(pspan, span, r->dims * sizeof(float))) {
            if (q->ctx->triangles || q->ctx->points || q->ctx->lines)
               ph_submit_vertices(q->ctx);
            memcpy(q->ctx->dom_lo[k], plo, r->dims * sizeof(float));
            memcpy(q->ctx->dom_span[k], pspan, r->dims * sizeof(float));
         }
      } else {
         ph_quant_domain(q, prim, n, q->ramp_src[i], k, r->comp, r->dims);
      }
      /* A new domain needs the table over it (the old batch is submitted). */
      if (memcmp(lo, q->ctx->dom_lo[k], sizeof(lo)) ||
          memcmp(span, q->ctx->dom_span[k], sizeof(span))) {
         struct pipe_sampler_view *view = ph_lookup_view(q->ctx, r);

         if (!view) {
            ph_warn_once("no lookup table represents the colour over the "
                         "batch's varying range; the primitive is dropped\n");
            return false;
         }
         q->ctx->base.set_sampler_views(&q->ctx->base, MESA_SHADER_FRAGMENT,
                                        r->unit, 1, 0, &view);
      }
   }
   return true;
}

static struct vertex_header *
ph_quant_vertex(struct draw_stage *stage, const struct vertex_header *v,
                unsigned idx)
{
   struct ph_quant_stage *q = (struct ph_quant_stage *)stage;
   /* Staging that only writes its own attributes from other ones, or is
    * idempotent, runs on the shared vertex itself: a vertex several
    * primitives share stages to the same values each time.  The routed
    * domains and rectangle scales rewrite their own input and need a copy. */
   struct vertex_header *out = q->route_count || q->rect_count ?
                               dup_vert(stage, v, idx) :
                               (struct vertex_header *)v;

   for (unsigned i = 0; i < q->coord_count; i++) {
      float *c = out->data[q->coord_slot[i]];

      memcpy(c, q->coord[i], sizeof(q->coord[i]));
      if (q->coord_window[i]) {
         c[0] = out->data[q->win_slot][0] * (1.0f / 32.0f);
         c[1] = out->data[q->win_slot][1] * (1.0f / 32.0f);
      }
   }
   if (q->const_z)
      out->data[q->pos_slot][2] = q->z;
   for (unsigned i = 0; i < q->rect_count; i++) {
      out->data[q->rect_slot[i]][0] *= q->rect_scale[i][0];
      out->data[q->rect_slot[i]][1] *= q->rect_scale[i][1];
   }
   for (unsigned t = 0; t < q->tap_count; t++) {
      nir_const_value c[NIR_MAX_VEC_COMPONENTS];
      float *x = out->data[q->tap_slot[t]];
      nir_def *coord = q->ctx->fs->taps[t].coord;

      x[0] = x[1] = x[2] = 0.0f;
      x[3] = 1.0f;
      if (ph_nir_eval_vertex(q->ctx, coord, v, c, 0))
         for (unsigned i = 0; i < MIN2(coord->num_components, 4); i++)
            x[i] = c[i].f32;
   }
   for (unsigned r = 0; r < q->route_count; r++) {
      const unsigned k = q->route_carrier[r];
      float *x = out->data[q->route_slot[r]];

      for (unsigned c = 0; c < 4; c++)
         x[c] = (x[c] - q->ctx->dom_lo[k][c]) / q->ctx->dom_span[k][c];
   }
   for (unsigned i = 0; i < q->ramp_count; i++) {
      const struct ph_fs_ramp *r = &q->ramp[i];
      const float *src = v->data[q->ramp_src[i]];
      float *c = out->data[q->ramp_slot[i]];

      if (r->kind != PH_FS_RAMP_FLOOR && r->kind != PH_FS_RAMP_PASS) {
         ph_fs_lookup_stage(r, q->ctx->dom_lo[2 + r->unit],
                            q->ctx->dom_span[2 + r->unit], src, c);
         continue;
      }
      c[0] = c[1] = c[2] = 0.5f;
      c[3] = 1.0f;
      for (unsigned d = 0; d < r->dims; d++)
         c[d] = r->kind == PH_FS_RAMP_PASS ?
            (src[r->comp[d]] - q->ctx->dom_lo[2 + r->unit][d]) /
            q->ctx->dom_span[2 + r->unit][d] :
            (src[r->comp[d]] - r->lo[d]) / r->span[d];
   }
   for (unsigned i = 0; q->quantize && i < 2; i++) {
      if (q->slot[i] < 0)
         continue;
      for (unsigned c = 0; c < 4; c++)
         if (q->scale[c] > 0.0f)
            out->data[q->slot[i]][c] =
               roundf(CLAMP(out->data[q->slot[i]][c], 0.0f, 1.0f) *
                      q->scale[c]) / q->scale[c];
   }
   return out;
}

/* Re-allocate the extra attribute holding semantic TEXCOORD @index when a
 * stage ahead removed it. */
static void
ph_quant_reattach(struct draw_context *draw, int *slot, unsigned index)
{
   if (draw_find_shader_output(draw, TGSI_SEMANTIC_TEXCOORD, index) != *slot)
      *slot = draw_alloc_extra_vertex_attrib(draw, TGSI_SEMANTIC_TEXCOORD,
                                             index);
}

/* draw's wide-point stage drops every extra attribute at its first point
 * and adds the sprite coordinates it generates, so the staged coordinate
 * sets are attached again at the first primitive this stage sees. */
static void
ph_quant_attach(struct ph_quant_stage *q)
{
   const struct ph_fs *fs = q->ctx->fs;
   struct draw_context *draw = q->ctx->draw;

   if (q->attached)
      return;
   q->attached = true;
   for (unsigned i = 0; fs && i < q->coord_count; i++)
      ph_quant_reattach(draw, &q->coord_slot[i],
                        PH_UNIFORM_COORD_INDEX(fs->ucoord[i].unit));
   for (unsigned i = 0; i < q->ramp_count; i++)
      ph_quant_reattach(draw, &q->ramp_slot[i],
                        PH_RAMP_COORD_INDEX(q->ramp[i].unit));
   for (unsigned t = 0; fs && t < q->tap_count; t++)
      ph_quant_reattach(draw, &q->tap_slot[t],
                        PH_RAMP_COORD_INDEX(fs->taps[t].unit));
}

/* A ramp is exact on [lo, lo + span) only: a primitive whose coordinate
 * leaves it would sample a different floor, so it is dropped. */
static bool
ph_quant_ramps_in_range(struct ph_quant_stage *q,
                        const struct prim_header *prim, unsigned n)
{
   ph_quant_attach(q);
   for (unsigned i = 0; i < q->ramp_count; i++) {
      unsigned name, index;

      if (q->ramp_src[i] >= 0)
         continue;
      tgsi_get_gl_varying_semantic(q->ramp[i].location, true, &name, &index);
      q->ramp_src[i] = draw_find_shader_output(q->ctx->draw, name, index);
      if (q->ramp_src[i] < 0) {
         ph_warn_once("a coordinate unit's varying the vertex and point "
                      "stages do not write; the primitive is dropped\n");
         return false;
      }
   }
   for (unsigned i = 0; i < q->ramp_count; i++)
      for (unsigned v = 0; v < n && q->ramp[i].kind == PH_FS_RAMP_FLOOR; v++)
         for (unsigned d = 0; d < q->ramp[i].dims; d++) {
            const float x = prim->v[v]->data[q->ramp_src[i]][q->ramp[i].comp[d]];

            if (!(x >= q->ramp[i].lo[d] &&
                  x < q->ramp[i].lo[d] + q->ramp[i].span[d])) {
               ph_warn_once("a floored coordinate left its ramp's exact "
                            "range; the primitive is dropped\n");
               return false;
            }
         }
   return true;
}

static void
ph_quant_tri(struct draw_stage *stage, struct prim_header *prim)
{
   struct prim_header tmp = *prim;

   if (!ph_quant_ramps_in_range((struct ph_quant_stage *)stage, prim, 3))
      return;
   if (!ph_quant_domains((struct ph_quant_stage *)stage, prim, 3))
      return;
   for (unsigned i = 0; i < 3; i++)
      tmp.v[i] = ph_quant_vertex(stage, prim->v[i], i);
   stage->next->tri(stage->next, &tmp);
}

static void
ph_quant_line(struct draw_stage *stage, struct prim_header *prim)
{
   struct prim_header tmp = *prim;

   if (!ph_quant_ramps_in_range((struct ph_quant_stage *)stage, prim, 2))
      return;
   if (!ph_quant_domains((struct ph_quant_stage *)stage, prim, 2))
      return;
   for (unsigned i = 0; i < 2; i++)
      tmp.v[i] = ph_quant_vertex(stage, prim->v[i], i);
   stage->next->line(stage->next, &tmp);
}

static void
ph_quant_point(struct draw_stage *stage, struct prim_header *prim)
{
   struct prim_header tmp = *prim;

   if (!ph_quant_ramps_in_range((struct ph_quant_stage *)stage, prim, 1))
      return;
   if (!ph_quant_domains((struct ph_quant_stage *)stage, prim, 1))
      return;
   tmp.v[0] = ph_quant_vertex(stage, prim->v[0], 0);
   stage->next->point(stage->next, &tmp);
}

static void
ph_quant_flush(struct draw_stage *stage, unsigned flags)
{
   stage->next->flush(stage->next, flags);
}

static void
ph_quant_reset(struct draw_stage *stage)
{
   stage->next->reset_stipple_counter(stage->next);
}

static void
ph_quant_destroy(struct draw_stage *stage)
{
   draw_free_temp_verts(stage);
   FREE(stage);
}

static struct draw_stage *
ph_quant_create_stage(struct ph_context *ctx)
{
   struct ph_quant_stage *q = CALLOC_STRUCT(ph_quant_stage);

   if (!q)
      return NULL;
   q->ctx = ctx;
   q->base.draw = ctx->draw;
   q->base.next = ctx->hw_stage;
   q->base.point = ph_quant_point;
   q->base.line = ph_quant_line;
   q->base.tri = ph_quant_tri;
   q->base.flush = ph_quant_flush;
   q->base.reset_stipple_counter = ph_quant_reset;
   q->base.destroy = ph_quant_destroy;
   if (!draw_alloc_temp_verts(&q->base, 3)) {
      FREE(q);
      return NULL;
   }
   return &q->base;
}

/* Evaluate a value built only from constants and the fragment constant
 * buffer (ph_nir_def_uniform), with NIR's own opcode semantics. */
static bool
ph_nir_eval_uniform(struct ph_context *ctx, nir_def *def,
                    nir_const_value out[NIR_MAX_VEC_COMPONENTS], unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 16)
      return false;
   memset(out, 0, NIR_MAX_VEC_COMPONENTS * sizeof(*out));
   switch (instr->type) {
   case nir_instr_type_load_const:
      memcpy(out, nir_instr_as_load_const(instr)->value,
             def->num_components * sizeof(*out));
      return true;
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      nir_const_value offset[NIR_MAX_VEC_COMPONENTS];
      uint64_t base;

      if ((in->intrinsic != nir_intrinsic_load_ubo &&
           in->intrinsic != nir_intrinsic_load_ubo_vec4) ||
          def->bit_size != 32 ||
          !ph_nir_eval_uniform(ctx, in->src[1].ssa, offset, depth + 1))
         return false;
      base = in->intrinsic == nir_intrinsic_load_ubo ? offset[0].u32 :
             ((uint64_t)nir_intrinsic_base(in) + offset[0].u32) * 16 +
             nir_intrinsic_component(in) * 4;
      for (unsigned c = 0; c < def->num_components; c++) {
         const uint64_t at = base + 4 * c;

         if (at + 4 > ctx->fs_const_snap_size)
            return false;
         memcpy(&out[c].u32, ctx->fs_const_snap + at, 4);
      }
      return true;
   }
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      const nir_op_info *info = &nir_op_infos[alu->op];
      nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
      nir_const_value *srcs[NIR_ALU_MAX_INPUTS];
      unsigned bit_size = 0;

      if (!nir_alu_type_get_type_size(info->output_type))
         bit_size = alu->def.bit_size;
      for (unsigned i = 0; i < info->num_inputs; i++) {
         nir_const_value v[NIR_MAX_VEC_COMPONENTS];

         if (!ph_nir_eval_uniform(ctx, alu->src[i].src.ssa, v, depth + 1))
            return false;
         if (!bit_size && !nir_alu_type_get_type_size(info->input_types[i]))
            bit_size = alu->src[i].src.ssa->bit_size;
         for (unsigned c = 0; c < nir_ssa_alu_instr_src_components(alu, i); c++)
            src[i][c] = v[alu->src[i].swizzle[c]];
         srcs[i] = src[i];
      }
      nir_eval_const_opcode(alu->op, out, NULL, alu->def.num_components,
                            bit_size ? bit_size : 32, srcs,
                            ctx->fs->draw->base.ir.nir->info.float_controls_execution_mode);
      return true;
   }
   default:
      return false;
   }
}

/* A value of constants, uniforms and varyings that is affine in the
 * varyings, so its per-vertex values interpolate to its per-fragment ones. */
static bool
ph_nir_affine_in_varyings(nir_def *def, bool *varying, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   *varying = false;
   if (depth > 16)
      return false;
   switch (instr->type) {
   case nir_instr_type_load_const:
      return true;
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

      if (in->intrinsic == nir_intrinsic_load_ubo ||
          in->intrinsic == nir_intrinsic_load_ubo_vec4)
         return nir_src_is_const(in->src[0]) && nir_src_is_const(in->src[1]);
      if (in->intrinsic == nir_intrinsic_load_interpolated_input ||
          in->intrinsic == nir_intrinsic_load_input) {
         *varying = true;
         return nir_src_is_const(nir_get_io_offset_src(in)[0]);
      }
      if (in->intrinsic == nir_intrinsic_load_deref) {
         nir_deref_instr *d = nir_src_as_deref(in->src[0]);

         *varying = d && d->deref_type == nir_deref_type_var &&
                    (d->var->data.mode & nir_var_shader_in);
         return *varying;
      }
      return false;
   }
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      bool v[NIR_ALU_MAX_INPUTS] = { false };
      const unsigned n = nir_op_infos[alu->op].num_inputs;
      unsigned vary = 0;

      for (unsigned i = 0; i < n; i++)
         if (!ph_nir_affine_in_varyings(alu->src[i].src.ssa, &v[i], depth + 1))
            return false;
      for (unsigned i = 0; i < n; i++)
         vary += v[i];
      *varying = vary != 0;
      switch (alu->op) {
      case nir_op_mov: case nir_op_fneg: case nir_op_fadd: case nir_op_fsub:
      case nir_op_vec2: case nir_op_vec3: case nir_op_vec4:
      case nir_op_fcanonicalize:
         return true;
      case nir_op_fmul:
         return vary <= 1;
      case nir_op_ffma:
         return !(v[0] && v[1]);
      default:
         return !vary;
      }
   }
   default:
      return false;
   }
}

/* @def at vertex @v: varyings read from the draw's vertex outputs. */
static bool
ph_nir_eval_vertex(struct ph_context *ctx, nir_def *def,
                   const struct vertex_header *v,
                   nir_const_value out[NIR_MAX_VEC_COMPONENTS], unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);
   int location = -1;
   unsigned first = 0;

   if (depth > 16)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

      if (in->intrinsic == nir_intrinsic_load_interpolated_input ||
          in->intrinsic == nir_intrinsic_load_input) {
         location = nir_intrinsic_io_semantics(in).location;
         first = nir_intrinsic_component(in);
      } else if (in->intrinsic == nir_intrinsic_load_deref) {
         nir_deref_instr *d = nir_src_as_deref(in->src[0]);

         if (d && d->deref_type == nir_deref_type_var &&
             (d->var->data.mode & nir_var_shader_in)) {
            location = d->var->data.location;
            first = d->var->data.location_frac;
         }
      }
   }
   if (location >= 0) {
      unsigned name, index;
      int slot;

      tgsi_get_gl_varying_semantic(location, true, &name, &index);
      slot = draw_find_shader_output(ctx->draw, name, index);
      if (slot < 0)
         return false;
      memset(out, 0, NIR_MAX_VEC_COMPONENTS * sizeof(*out));
      for (unsigned c = 0; c < def->num_components && first + c < 4; c++)
         out[c].f32 = v->data[slot][first + c];
      return true;
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      const nir_op_info *info = &nir_op_infos[alu->op];
      nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
      nir_const_value *srcs[NIR_ALU_MAX_INPUTS];

      for (unsigned i = 0; i < info->num_inputs; i++) {
         nir_const_value val[NIR_MAX_VEC_COMPONENTS];

         if (!ph_nir_eval_vertex(ctx, alu->src[i].src.ssa, v, val, depth + 1))
            return false;
         for (unsigned c = 0; c < nir_ssa_alu_instr_src_components(alu, i); c++)
            src[i][c] = val[alu->src[i].swizzle[c]];
         srcs[i] = src[i];
      }
      memset(out, 0, NIR_MAX_VEC_COMPONENTS * sizeof(*out));
      nir_eval_const_opcode(alu->op, out, NULL, alu->def.num_components, 32,
                            srcs, 0);
      return true;
   }
   return ph_nir_eval_uniform(ctx, def, out, depth);
}

static void
ph_nir_collect_tex(nir_shader *nir, nir_tex_instr **out, unsigned cap,
                   unsigned *count)
{
   *count = 0;
   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            if (instr->type == nir_instr_type_tex) {
               if (*count < cap)
                  out[*count] = nir_instr_as_tex(instr);
               (*count)++;
            }
}

static bool
ph_tex_same_lookup(const nir_tex_instr *a, const nir_tex_instr *b)
{
   if (a->op != b->op || a->num_srcs != b->num_srcs ||
       a->sampler_dim != b->sampler_dim ||
       a->sampler_index != b->sampler_index)
      return false;
   for (unsigned i = 0; i < a->num_srcs; i++) {
      const int j = nir_tex_instr_src_index(b, a->src[i].src_type);

      if (j < 0 || b->src[j].src.ssa != a->src[i].src.ssa)
         return false;
   }
   return true;
}

/* Further lookups of a unit at other coordinates (a filter's taps) move to
 * free units that sample the same texture, their coordinate evaluated per
 * vertex; the compile copy is renumbered in step.  A tap whose coordinate
 * is not affine in the varyings, or one without a free unit, stays and the
 * compile refuses it. */
static void
ph_fs_split_taps(struct ph_fs *fs, nir_shader *orig, nir_shader *copy,
                 unsigned used, unsigned max_units)
{
   nir_tex_instr *a[32], *b[32];
   nir_def *coord_of[32];
   unsigned na, nb, unit = 0, moved_to[32];
   int first[P3D_MAX_TEXTURE_UNITS];

   fs->tap_count = 0;
   ph_nir_collect_tex(orig, a, ARRAY_SIZE(a), &na);
   ph_nir_collect_tex(copy, b, ARRAY_SIZE(b), &nb);
   if (na != nb || nb > ARRAY_SIZE(b) || fs->unit_remap)
      return;
   for (unsigned u = 0; u < P3D_MAX_TEXTURE_UNITS; u++)
      first[u] = -1;
   for (unsigned i = 0; i < nb; i++) {
      const unsigned s = b[i]->sampler_index;
      const int c = nir_tex_instr_src_index(a[i], nir_tex_src_coord);
      bool varying;

      moved_to[i] = s;
      coord_of[i] = c >= 0 ? a[i]->src[c].src.ssa : NULL;
      if (s >= P3D_MAX_TEXTURE_UNITS)
         continue;
      if (first[s] < 0) {
         first[s] = i;
         continue;
      }
      if (ph_tex_same_lookup(b[first[s]], b[i]))
         continue;
      /* The same tap already moved shares that unit. */
      for (unsigned j = 0; j < i && moved_to[i] == s; j++)
         if (moved_to[j] != s && coord_of[j] == coord_of[i] &&
             b[j]->op == b[i]->op)
            moved_to[i] = moved_to[j];
      if (moved_to[i] == s) {
         if (c < 0 || a[i]->num_srcs != 1 || b[i]->op != nir_texop_tex ||
             b[i]->sampler_dim != GLSL_SAMPLER_DIM_2D ||
             !ph_nir_affine_in_varyings(coord_of[i], &varying, 0))
            continue;
         while (unit < max_units && (used & (1u << unit)))
            unit++;
         if (unit >= max_units)
            continue;
         used |= 1u << unit;
         fs->taps[fs->tap_count++] = (struct ph_tap){ unit, s, coord_of[i] };
         moved_to[i] = unit;
      }
      b[i]->sampler_index = b[i]->texture_index = moved_to[i];
   }
   /* A unit whose own lookup is at an expression rather than a varying's
    * leading components (the centre tap at an offset, a packed varying's
    * .yz) stages that coordinate too. */
   for (unsigned s = 0; s < P3D_MAX_TEXTURE_UNITS; s++) {
      bool staged = false, varying;

      for (unsigned k = 0; k < fs->tap_count; k++)
         staged |= fs->taps[k].unit == s;
      if (staged || first[s] < 0 || !coord_of[first[s]] ||
          ph_nir_coord_location(coord_of[first[s]]) >= 0 ||
          a[first[s]]->num_srcs != 1 ||
          a[first[s]]->sampler_dim != GLSL_SAMPLER_DIM_2D ||
          !ph_nir_affine_in_varyings(coord_of[first[s]], &varying, 0) ||
          !varying)
         continue;
      fs->taps[fs->tap_count++] = (struct ph_tap){ s, s, coord_of[first[s]] };
   }
}

/* Sample coordinates that are uniforms: a constant glTexCoord reaches the
 * fragment shader as one, so the vertex stream carries no coordinate set
 * and the draw had nothing to give the texture unit. */
static void
ph_fs_find_uniform_coords(struct ph_fs *fs, nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_tex_instr *tex;
            int c, q;

            if (instr->type != nir_instr_type_tex)
               continue;
            tex = nir_instr_as_tex(instr);
            c = nir_tex_instr_src_index(tex, nir_tex_src_coord);
            q = nir_tex_instr_src_index(tex, nir_tex_src_projector);
            if (c < 0 || !ph_nir_def_uniform(tex->src[c].src.ssa, 0) ||
                (q >= 0 && !ph_nir_def_uniform(tex->src[q].src.ssa, 0)) ||
                fs->ucoord_count >= P3D_MAX_TEXTURE_UNITS)
               continue;
            fs->ucoord = realloc(fs->ucoord,
                                 (fs->ucoord_count + 1) * sizeof(*fs->ucoord));
            if (!fs->ucoord) {
               fs->ucoord_count = 0;
               return;
            }
            fs->ucoord[fs->ucoord_count++] = (struct ph_uniform_coord){
               .coord = tex->src[c].src.ssa,
               .projector = q >= 0 ? tex->src[q].src.ssa : NULL,
               .unit = tex->texture_index,
               .comps = tex->coord_components,
            };
            if (tex->texture_index < PIPE_MAX_SAMPLERS) {
               fs->coord_src_known[tex->texture_index] = true;
               fs->coord_src_name[tex->texture_index] = TGSI_SEMANTIC_TEXCOORD;
               fs->coord_src_index[tex->texture_index] =
                  PH_UNIFORM_COORD_INDEX(tex->texture_index);
            }
         }
      }
   }
}

/* The input a sample's coordinate is read from, through movs and vecs whose
 * channels all come from that one input. */
static int
ph_nir_coord_location(nir_def *def)
{
   /* comp[i]: the component of @def that coordinate component i reads */
   unsigned comp[NIR_MAX_VEC_COMPONENTS];
   const unsigned n = MIN2(def->num_components, 4);

   for (unsigned i = 0; i < n; i++)
      comp[i] = i;
   for (unsigned hop = 0; hop < 8; hop++) {
      nir_instr *instr = nir_def_instr(def);

      if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         unsigned first;
         int location;

         switch (in->intrinsic) {
         case nir_intrinsic_load_interpolated_input:
         case nir_intrinsic_load_input:
            location = nir_intrinsic_io_semantics(in).location;
            first = nir_intrinsic_component(in);
            break;
         case nir_intrinsic_load_deref: {
            nir_variable *var =
               nir_deref_instr_get_variable(nir_src_as_deref(in->src[0]));

            if (!var || var->data.mode != nir_var_shader_in)
               return -1;
            location = var->data.location;
            first = var->data.location_frac;
            break;
         }
         default:
            return -1;
         }
         /* The unit reads the set's components in order from .x. */
         for (unsigned i = 0; i < n; i++)
            if (first + comp[i] != i)
               return -1;
         return location;
      }
      if (instr->type != nir_instr_type_alu)
         return -1;
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov) {
         for (unsigned i = 0; i < n; i++)
            comp[i] = alu->src[0].swizzle[comp[i]];
         def = alu->src[0].src.ssa;
         continue;
      }
      if (!nir_op_is_vec(alu->op))
         return -1;
      for (unsigned i = 1; i < nir_op_infos[alu->op].num_inputs; i++)
         if (alu->src[i].src.ssa != alu->src[0].src.ssa)
            return -1;
      for (unsigned i = 0; i < n; i++)
         comp[i] = alu->src[comp[i]].swizzle[0];
      def = alu->src[0].src.ssa;
   }
   return -1;
}

static void
ph_fs_find_coord_sources(struct ph_fs *fs, nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_tex_instr *tex;
            unsigned name, index;
            int c, loc;

            if (instr->type != nir_instr_type_tex)
               continue;
            tex = nir_instr_as_tex(instr);
            c = nir_tex_instr_src_index(tex, nir_tex_src_coord);
            if (tex->texture_index < PIPE_MAX_SAMPLERS &&
                tex->sampler_dim == GLSL_SAMPLER_DIM_RECT)
               fs->coord_rect[tex->texture_index] = true;
            if (c < 0 || tex->texture_index >= PIPE_MAX_SAMPLERS ||
                fs->coord_src_known[tex->texture_index] ||
                (loc = ph_nir_coord_location(tex->src[c].src.ssa)) < 0)
               continue;
            /* draw scans NIR with TEXCOORD semantics. */
            tgsi_get_gl_varying_semantic(loc, true, &name, &index);
            fs->coord_src_known[tex->texture_index] = true;
            fs->coord_src_name[tex->texture_index] = name;
            fs->coord_src_index[tex->texture_index] = index;
         }
      }
   }
}

/* Give each uniform coordinate an extra vertex attribute and its value; false
 * when one cannot be evaluated. */
static bool
ph_fix_coords(struct ph_context *ctx, struct ph_quant_stage *q)
{
   q->coord_count = 0;
   q->win_slot = draw_current_shader_position_output(ctx->draw);
   for (unsigned i = 0; ctx->fs && i < ctx->fs->ucoord_count; i++) {
      const struct ph_uniform_coord *u = &ctx->fs->ucoord[i];
      nir_const_value v[NIR_MAX_VEC_COMPONENTS], w[NIR_MAX_VEC_COMPONENTS];
      float *out = q->coord[q->coord_count];

      if (!ph_nir_eval_uniform(ctx, u->coord, v, 0) ||
          (u->projector && !ph_nir_eval_uniform(ctx, u->projector, w, 0)))
         return false;
      for (unsigned c = 0; c < 4; c++)
         out[c] = c < u->comps ? v[c].f32 : c == 3 ? 1.0f : 0.0f;
      if (u->projector)
         out[3] = w[0].f32;
      q->coord_window[q->coord_count] = u->window;
      q->coord_slot[q->coord_count++] =
         draw_alloc_extra_vertex_attrib(ctx->draw, TGSI_SEMANTIC_TEXCOORD,
                                        PH_UNIFORM_COORD_INDEX(u->unit));
   }
   return true;
}

/* Scale each rectangle unit's coordinate set by its image's extent; the
 * texture unit only addresses normalised coordinates. */
static void
ph_fix_rects(struct ph_context *ctx, struct ph_quant_stage *q)
{
   q->rect_count = 0;
   for (unsigned u = 0; ctx->fs && u < P3D_MAX_TEXTURE_UNITS; u++) {
      const unsigned gl = ctx->fs->unit_remap ? ctx->fs->unit_source[u] : u;
      const struct pipe_sampler_view *v;
      int slot;

      if (gl >= PIPE_MAX_SAMPLERS || !ctx->fs->coord_rect[gl] ||
          gl >= ctx->num_sampler_views || !(v = ctx->bound_views[gl]) ||
          !v->texture || (slot = ph_unit_texcoord(ctx, u)) < 0)
         continue;
      q->rect_slot[q->rect_count] = slot;
      q->rect_scale[q->rect_count][0] =
         1.0f / u_minify(v->texture->width0, v->u.tex.first_level);
      q->rect_scale[q->rect_count][1] =
         1.0f / u_minify(v->texture->height0, v->u.tex.first_level);
      q->rect_count++;
   }
}

/* An extra attribute per tap for its per-vertex coordinate. */
static void
ph_fix_taps(struct ph_context *ctx, struct ph_quant_stage *q)
{
   q->tap_count = 0;
   for (unsigned t = 0; ctx->fs && ctx->fs->program.valid &&
                        t < ctx->fs->tap_count; t++)
      q->tap_slot[q->tap_count++] =
         draw_alloc_extra_vertex_attrib(ctx->draw, TGSI_SEMANTIC_TEXCOORD,
                                        PH_RAMP_COORD_INDEX(ctx->fs->taps[t].unit));
}

/* Stage each floor ramp's coordinate into an extra attribute; false when
 * the varying it floors is not in the vertex output. */
static bool
ph_fix_ramps(struct ph_context *ctx, struct ph_quant_stage *q)
{
   const struct ph_fs_program *prog = ctx->fs ? &ctx->fs->program : NULL;

   q->ramp_count = 0;
   for (unsigned i = 0; prog && prog->valid && i < prog->ramp_count; i++) {
      const struct ph_fs_ramp *r = &prog->ramps[i];
      unsigned name, index;
      int src;

      tgsi_get_gl_varying_semantic(r->location, true, &name, &index);
      src = draw_find_shader_output(ctx->draw, name, index);
      /* gl_PointCoord is generated by draw's wide-point stage, which adds
       * its attribute at the first point: resolved per primitive then. */
      if (src < 0 && r->location != VARYING_SLOT_PNTC)
         return false;
      q->ramp[q->ramp_count] = *r;
      q->ramp_src[q->ramp_count] = src;
      q->ramp_slot[q->ramp_count++] =
         draw_alloc_extra_vertex_attrib(ctx->draw, TGSI_SEMANTIC_TEXCOORD,
                                        PH_RAMP_COORD_INDEX(r->unit));
   }
   return true;
}

/* The routed generic varyings' vertex outputs, staged in their domains. */
static void
ph_fix_routes(struct ph_context *ctx, struct ph_quant_stage *q)
{
   const struct ph_fs_program *prog = ctx->fs ? &ctx->fs->program : NULL;

   q->route_count = 0;
   for (unsigned k = 0; prog && prog->valid && k < 2; k++) {
      const int slot = prog->route[k] >= 0 ? ph_v_output(ctx->draw, ctx, k) : -1;

      if (slot < 0)
         continue;
      q->route_slot[q->route_count] = slot;
      q->route_carrier[q->route_count++] = k;
   }
}

static bool
ph_fs_samples_rect(const struct ph_fs *fs)
{
   for (unsigned k = 0; fs && k < PIPE_MAX_SAMPLERS; k++)
      if (fs->coord_rect[k])
         return true;
   return false;
}

/* gl_FragDepth from uniforms: every vertex at that window depth, put on the
 * Z grid the way a clear to the same value is (ph_depth_window). */
static bool
ph_fix_depth(struct ph_context *ctx, struct ph_quant_stage *q)
{
   nir_const_value v[NIR_MAX_VEC_COMPONENTS];
   const struct util_format_description *zd;
   unsigned bits;

   q->const_z = false;
   if (!ctx->fs || !ctx->fs->depth_uniform)
      return true;
   if (!ph_nir_eval_uniform(ctx, ctx->fs->depth_uniform, v, 0))
      return false;
   zd = ctx->fb.zsbuf.texture ? util_format_description(ctx->fb.zsbuf.format) :
                                NULL;
   bits = zd && util_format_has_depth(zd) ? zd->channel[zd->swizzle[0]].size : 0;
   q->pos_slot = draw_find_shader_output(ctx->draw, TGSI_SEMANTIC_POSITION, 0);
   q->z = ctx->z_grid && bits && bits < 32 ? ph_depth_window(v[0].f32, bits) :
          CLAMP(v[0].f32, 0.0f, 1.0f);
   q->const_z = q->pos_slot >= 0;
   return true;
}

static void
ph_quant_prepare(struct ph_context *ctx, bool quantize)
{
   struct ph_quant_stage *q = (struct ph_quant_stage *)ctx->quant_stage;

   q->quantize = quantize;
   q->attached = false;
   q->slot[0] = draw_find_shader_output(ctx->draw, TGSI_SEMANTIC_COLOR, 0);
   q->slot[1] = draw_find_shader_output(ctx->draw, TGSI_SEMANTIC_COLOR, 1);
   if (quantize)
      ph_logicop_grid(ctx, q->scale);
}

/* ------------------------------------------------ depth pixels as points
 *
 * The combiner cannot write depth.  The vendor ICD draws
 * glDrawPixels(GL_DEPTH_COMPONENT) - and glCopyPixels(GL_DEPTH), which reads
 * back and draws - as one GL_POINTS vertex per pixel with the pixel's depth
 * as z (libGL 0x1bdbf0, DrawPixelsDepth row method 0xfc1c0; one quad per
 * pixel at a pixel zoom other than 1), so the points take the depth,
 * stencil, scissor and blend tests like any primitive.  Mesa's form of the
 * same draw is a window-aligned quad whose shader writes the depth sample;
 * this stage walks that quad's pixel centres and hands each one to the
 * native point path at the depth of the texel it samples.
 */
static bool
ph_nir_depth_from_texture(nir_shader *nir, unsigned *unit)
{
   nir_foreach_function_impl(impl, nir) {
      if (exec_list_length(&impl->body) != 1)
         return false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;
            nir_def *value = NULL;
            unsigned comp = 0;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_store_deref) {
               nir_deref_instr *d = nir_src_as_deref(in->src[0]);

               if (d && d->deref_type == nir_deref_type_var &&
                   d->var->data.location == FRAG_RESULT_DEPTH) {
                  value = in->src[1].ssa;
                  comp = 0;
               }
            } else if (in->intrinsic == nir_intrinsic_store_output &&
                       nir_intrinsic_io_semantics(in).location ==
                       FRAG_RESULT_DEPTH) {
               value = in->src[0].ssa;
               comp = 0;
            }
            if (!value)
               continue;
            /* Through movs and vecs to the sample. */
            for (unsigned hop = 0; hop < 8; hop++) {
               nir_alu_instr *alu = nir_def_as_alu_or_null(value);

               if (!alu || (alu->op != nir_op_mov &&
                            !nir_op_is_vec(alu->op)))
                  break;
               if (alu->op == nir_op_mov) {
                  comp = alu->src[0].swizzle[comp];
                  value = alu->src[0].src.ssa;
               } else {
                  value = alu->src[comp].src.ssa;
                  comp = alu->src[comp].swizzle[0];
               }
            }
            if (comp || nir_def_instr(value)->type != nir_instr_type_tex)
               return false;
            {
               nir_tex_instr *tex = nir_instr_as_tex(nir_def_instr(value));

               if (tex->op != nir_texop_tex ||
                   nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0 ||
                   nir_tex_instr_src_index(tex, nir_tex_src_comparator) >= 0)
                  return false;
               *unit = tex->texture_index;
               return true;
            }
         }
      }
   }
   return false;
}

/* One run of pixels [x0, x1) on row y at depth z, as the two triangles of
 * its rectangle (PARHELIA_ZPIX_RUNS=1, the comparison form); by default
 * each pixel is one native unit point, the vendor's form. */
static void
ph_zpix_run(struct draw_stage *stage, const struct prim_header *prim, int pos,
            int x0, int x1, int y, float z)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   struct draw_stage *next = ctx->hw_stage;
   const float cx[4] = { x0, x1, x1, x0 }, cy[4] = { y, y, y + 1, y + 1 };
   struct vertex_header *v[4];
   struct prim_header tri = *prim;

   for (unsigned c = 0; c < 4; c++) {
      v[c] = dup_vert(stage, prim->v[0], c);
      v[c]->data[pos][0] = cx[c];
      v[c]->data[pos][1] = cy[c];
      v[c]->data[pos][2] = z;
      v[c]->data[pos][3] = 1.0f;
   }
   tri.v[0] = v[0]; tri.v[1] = v[1]; tri.v[2] = v[2];
   next->tri(next, &tri);
   tri.v[0] = v[0]; tri.v[1] = v[2]; tri.v[2] = v[3];
   next->tri(next, &tri);
}

static void
ph_zpix_tri(struct draw_stage *stage, struct prim_header *prim)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   const int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   int tc = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_TEXCOORD, 0);
   const float k = (float)0xffffff / (float)0x1000000;
   const unsigned cpp = util_format_get_blocksize(ctx->zpix_format);
   const bool runs = ph_env("PARHELIA_ZPIX_RUNS");
   const float *v[3];
   float area;
   int xmin, ymin, xmax, ymax;

   if (tc < 0)
      tc = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_GENERIC, 0);
   if (pos < 0 || tc < 0 || !ctx->zpix_map)
      return;
   for (unsigned i = 0; i < 3; i++)
      v[i] = prim->v[i]->data[pos];
   area = ph_cpu_edge(v[0], v[1], v[2][0], v[2][1]);
   if (ph_env("PARHELIA_TRACE_ZPIX"))
      fprintf(stderr, "parhelia: zpix tri %.6f,%.6f %.6f,%.6f %.6f,%.6f st %.6f,%.6f %.6f,%.6f %.6f,%.6f\n",
              v[0][0], v[0][1], v[1][0], v[1][1], v[2][0], v[2][1],
              prim->v[0]->data[tc][0], prim->v[0]->data[tc][1],
              prim->v[1]->data[tc][0], prim->v[1]->data[tc][1],
              prim->v[2]->data[tc][0], prim->v[2]->data[tc][1]);
   if (area == 0.0f)
      return;
   xmin = MAX2(0, (int)floorf(MIN3(v[0][0], v[1][0], v[2][0])));
   ymin = MAX2(0, (int)floorf(MIN3(v[0][1], v[1][1], v[2][1])));
   xmax = MIN2((int)ctx->fb.width - 1, (int)ceilf(MAX3(v[0][0], v[1][0], v[2][0])));
   ymax = MIN2((int)ctx->fb.height - 1, (int)ceilf(MAX3(v[0][1], v[1][1], v[2][1])));
   for (int y = ymin; y <= ymax; y++) {
      int run = -1;
      float run_z = 0.0f;

      for (int x = xmin; x <= xmax + 1; x++) {
         const float px = x + 0.5f, py = y + 0.5f;
         float e[3] = { 0 }, st[2], d, z = 0.0f;
         bool inside = x <= xmax;
         int tx, ty;

         for (unsigned i = 0; inside && i < 3; i++) {
            const float *a = v[(i + 1) % 3], *b = v[(i + 2) % 3];
            /* Evaluate a shared edge in one vertex order for both of its
             * triangles, so they see exactly opposite values and a centre on
             * it is claimed by one of them. */
            const bool swap = a[0] > b[0] || (a[0] == b[0] && a[1] > b[1]);
            const float edge = swap ? -ph_cpu_edge(b, a, px, py) :
                                      ph_cpu_edge(a, b, px, py);
            const float side = area > 0.0f ? edge : -edge;

            e[i] = edge / area;
            inside = side > 0.0f ||
                     (side == 0.0f && (area > 0.0f ? ph_cpu_top_left(a, b) :
                                                     ph_cpu_top_left(b, a)));
         }
         if (inside) {
            for (unsigned c = 0; c < 2; c++)
               st[c] = e[0] * prim->v[0]->data[tc][c] +
                       e[1] * prim->v[1]->data[tc][c] +
                       e[2] * prim->v[2]->data[tc][c];
            tx = (int)floorf(ctx->zpix_unnormalized ? st[0] :
                             st[0] * ctx->zpix_width);
            ty = (int)floorf(ctx->zpix_unnormalized ? st[1] :
                             st[1] * ctx->zpix_height);
            tx = CLAMP(tx, 0, (int)ctx->zpix_width - 1);
            ty = CLAMP(ty, 0, (int)ctx->zpix_height - 1);
            util_format_unpack_z_float(ctx->zpix_format, &d,
                                       ctx->zpix_map +
                                       (size_t)ty * ctx->zpix_transfer->stride +
                                       (size_t)tx * cpp, 1);
            /* On the Z grid ph_update_viewport puts fragment depths on. */
            z = CLAMP(d, 0.0f, 1.0f) * (ctx->z_grid ? k : 1.0f);
         }
         if (!runs) {
            if (inside) {
               struct prim_header point = *prim;
               struct vertex_header *vh = dup_vert(stage, prim->v[0], 0);

               vh->data[pos][0] = px;
               vh->data[pos][1] = py;
               vh->data[pos][2] = z;
               vh->data[pos][3] = 1.0f;
               point.v[0] = vh;
               ctx->hw_stage->point(ctx->hw_stage, &point);
            }
            continue;
         }
         if (run >= 0 && (!inside || z != run_z)) {
            ph_zpix_run(stage, prim, pos, run, x, y, run_z);
            run = -1;
         }
         if (inside && run < 0) {
            run = x;
            run_z = z;
         }
      }
   }
}

static void ph_zpix_line(struct draw_stage *s, struct prim_header *p)
{ (void)s; (void)p; }
static void ph_zpix_point(struct draw_stage *s, struct prim_header *p)
{ (void)s; (void)p; }
static void ph_zpix_flush(struct draw_stage *s, unsigned flags)
{ (void)s; (void)flags; }
static void ph_zpix_reset(struct draw_stage *s) { (void)s; }
static void
ph_zpix_destroy(struct draw_stage *stage)
{
   draw_free_temp_verts(stage);
   FREE(stage);
}

static struct draw_stage *
ph_zpix_create_stage(struct ph_context *ctx)
{
   struct ph_stage *z = CALLOC_STRUCT(ph_stage);

   if (!z)
      return NULL;
   z->ctx = ctx;
   z->base.draw = ctx->draw;
   z->base.point = ph_zpix_point;
   z->base.line = ph_zpix_line;
   z->base.tri = ph_zpix_tri;
   z->base.flush = ph_zpix_flush;
   z->base.reset_stipple_counter = ph_zpix_reset;
   z->base.destroy = ph_zpix_destroy;
   if (!draw_alloc_temp_verts(&z->base, 4)) {
      FREE(z);
      return NULL;
   }
   return &z->base;
}

/* Draw a depth-writing quad as per-pixel native points; false when the draw
 * is not that form. */
static bool
ph_draw_depth_points(struct ph_context *ctx, const struct pipe_draw_info *info,
                     unsigned drawid_offset,
                     const struct pipe_draw_start_count_bias *draws)
{
   struct ph_fs *fs = ctx->fs;
   const unsigned unit = fs ? fs->depth_unit : 0;
   struct pipe_sampler_view *view = fs && fs->depth_from_texture &&
                                    unit < ctx->num_sampler_views ?
                                    ctx->bound_views[unit] : NULL;
   struct pipe_box box;
   unsigned level;

   if (!view || !view->texture ||
       !util_format_has_depth(util_format_description(view->format)) ||
       u_reduced_prim(info->mode) != MESA_PRIM_TRIANGLES)
      return false;
   if (!ctx->zpix_stage && !(ctx->zpix_stage = ph_zpix_create_stage(ctx)))
      return false;
   level = view->u.tex.first_level;
   ctx->zpix_width = u_minify(view->texture->width0, level);
   ctx->zpix_height = u_minify(view->texture->height0, level);
   ctx->zpix_format = view->format;
   ctx->zpix_unnormalized = view->target == PIPE_TEXTURE_RECT ||
      (unit < ctx->num_samplers && ctx->samplers[unit] &&
       ctx->samplers[unit]->unnormalized_coords);
   u_box_2d(0, 0, ctx->zpix_width, ctx->zpix_height, &box);
   ctx->zpix_map = ctx->base.texture_map(&ctx->base, view->texture, level,
                                         PIPE_MAP_READ, &box,
                                         &ctx->zpix_transfer);
   if (!ctx->zpix_map)
      return true;
   if (ph_env("PARHELIA_TRACE")) {
      float d0;

      util_format_unpack_z_float(ctx->zpix_format, &d0, ctx->zpix_map, 1);
      fprintf(stderr, "parhelia: depth pixels as points from %s %ux%u level %u, first depth %f\n",
              util_format_short_name(ctx->zpix_format), ctx->zpix_width,
              ctx->zpix_height, level, d0);
   }
   ph_submit_vertices(ctx);
   ctx->fs = &ctx->colour_fs;
   draw_set_rasterize_stage(ctx->draw, ctx->zpix_stage);
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, 1, 0);
   draw_flush(ctx->draw);
   draw_set_rasterize_stage(ctx->draw, ctx->hw_stage);
   /* The points are staged against the stand-in; submit them under it. */
   ph_submit_vertices(ctx);
   ctx->fs = fs;
   ctx->base.texture_unmap(&ctx->base, ctx->zpix_transfer);
   ctx->zpix_map = NULL;
   ctx->zpix_transfer = NULL;
   return true;
}

/* A framebuffer with several colour buffers (GL_FRONT_AND_BACK binds the
 * front and the back) is drawn once per buffer; the engine has one colour
 * target.  Every pass but the last runs with depth and stencil writes off,
 * so all passes test the contents the draw found and the last one updates
 * them; only primitives of one draw that overlap each other can see the
 * difference. */
static bool
ph_draw_broadcast(struct ph_context *ctx, const struct pipe_draw_info *info,
                  unsigned drawid_offset,
                  const struct pipe_draw_start_count_bias *draws)
{
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   const bool zs_writes = ctx->fb.zsbuf.texture && dsa &&
      ((dsa->depth_enabled && dsa->depth_writemask) ||
       (dsa->stencil[0].enabled && dsa->stencil[0].writemask) ||
       (dsa->stencil[1].enabled && dsa->stencil[1].writemask));
   struct pipe_framebuffer_state saved = { 0 }, one;
   unsigned bound = 0;

   if (ctx->broadcasting || ctx->fb.nr_cbufs < 2)
      return false;
   for (unsigned i = 0; i < ctx->fb.nr_cbufs; i++)
      bound += !!ctx->fb.cbufs[i].texture;
   if (bound < 2 && ctx->fb.cbufs[0].texture)
      return false;
   void *saved_dsa = ctx->dsa, *readonly = NULL;

   if (zs_writes) {
      struct pipe_depth_stencil_alpha_state ro = *dsa;

      ro.depth_writemask = 0;
      ro.stencil[0].writemask = ro.stencil[1].writemask = 0;
      readonly = ctx->base.create_depth_stencil_alpha_state(&ctx->base, &ro);
      if (!readonly)
         return false;
   }
   /* The state tracker's surfaces: stand-ins are rebound per pass. */
   util_copy_framebuffer_state(&saved, &ctx->fb_api);
   ctx->broadcasting = true;
   for (unsigned i = 0, done = 0; i < saved.nr_cbufs; i++) {
      if (!saved.cbufs[i].texture)
         continue;
      one = saved;
      one.nr_cbufs = 1;
      one.cbufs[0] = saved.cbufs[i];
      ctx->base.set_framebuffer_state(&ctx->base, &one);
      if (readonly)
         ctx->base.bind_depth_stencil_alpha_state(&ctx->base,
            ++done < bound ? readonly : saved_dsa);
      ctx->base.draw_vbo(&ctx->base, info, drawid_offset, NULL, draws, 1);
   }
   if (readonly) {
      ctx->base.bind_depth_stencil_alpha_state(&ctx->base, saved_dsa);
      ctx->base.delete_depth_stencil_alpha_state(&ctx->base, readonly);
   }
   ctx->base.set_framebuffer_state(&ctx->base, &saved);
   ctx->broadcasting = false;
   util_unreference_framebuffer_state(&saved);
   return true;
}

static void *ph_create_fs(struct pipe_context *p,
                          const struct pipe_shader_state *s);
static void ph_delete_fs(struct pipe_context *p, void *s);

/* @fs with "discard if the stipple texel is set" on the unit after its
 * samplers, which the discard path turns into an alpha test; the vendor
 * kills through a texture unit the same way (TEXKILL on a 32x32 texture
 * at window coordinates / 32).  NULL when the program cannot take it. */
static struct ph_fs *
ph_fs_stipple_variant(struct ph_context *ctx, struct ph_fs *fs)
{
   const unsigned unit = util_last_bit(fs->sampler_mask);
   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR };
   nir_shader *src, *nir;
   nir_tex_instr *tex;
   struct ph_fs *v;

   if (fs->stipple || fs->stipple_failed)
      return fs->stipple;
   fs->stipple_failed = true;
   if (!fs->draw || fs->draw->base.type != PIPE_SHADER_IR_NIR ||
       unit >= P3D_MAX_TEXTURE_UNITS)
      return NULL;
   src = fs->draw->base.ir.nir;
   if (src->info.fs.uses_discard)
      return NULL;
   nir = nir_shader_clone(NULL, src);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_at(nir_before_impl(impl));

   tex = nir_tex_instr_create(nir, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = tex->sampler_index = unit;
   /* A placeholder: the vertex stage stages the window position. */
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                     nir_imm_vec2(&b, 0.0f, 0.0f));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_discard_if(&b, nir_fneu_imm(&b, nir_channel(&b, &tex->def, 0), 0.0));
   nir->info.fs.uses_discard = true;
   BITSET_SET(nir->info.textures_used, unit);
   BITSET_SET(nir->info.samplers_used, unit);
   nir_progress(true, impl, nir_metadata_none);

   state.ir.nir = nir;
   v = ph_create_fs(&ctx->base, &state);
   if (!v || v->drop_reason || !v->kill) {
      ph_delete_fs(&ctx->base, v);
      ralloc_free(nir);
      return NULL;
   }
   for (unsigned i = 0; i < v->ucoord_count; i++)
      v->ucoord[i].window = v->ucoord[i].unit == unit;
   v->stipple_affine = !fs->sampler_mask;
   fs->stipple = v;
   fs->stipple_unit = unit;
   fs->stipple_failed = false;
   return v;
}

static bool
ph_stipple_texture(struct ph_context *ctx)
{
   if (!ctx->stipple_tex) {
      const struct pipe_resource templ = {
         .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R8_UNORM,
         .width0 = 32, .height0 = 32, .depth0 = 1, .array_size = 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      struct pipe_sampler_view vt;

      ctx->stipple_tex = ctx->base.screen->resource_create(ctx->base.screen,
                                                           &templ);
      if (!ctx->stipple_tex)
         return false;
      u_sampler_view_default_template(&vt, ctx->stipple_tex,
                                      ctx->stipple_tex->format);
      ctx->stipple_view = ctx->base.create_sampler_view(&ctx->base,
                                                        ctx->stipple_tex, &vt);
      ctx->stipple_sampler = util_pstipple_create_sampler(&ctx->base);
      ctx->stipple_dirty = true;
   }
   if (!ctx->stipple_view || !ctx->stipple_sampler)
      return false;
   if (ctx->stipple_dirty) {
      util_pstipple_update_stipple_texture(&ctx->base, ctx->stipple_tex,
                                           ctx->stipple);
      ctx->stipple_dirty = false;
   }
   return true;
}

/* Polygon stipple for filled polygons: replay the draw with the stipple
 * variant of the program and the pattern bound on its unit.  Like the
 * vendor, a program without a free unit draws unstippled. */
static bool
ph_draw_stippled(struct ph_context *ctx, const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_start_count_bias *draws)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   struct pipe_sampler_view *old_view = NULL;
   struct ph_fs *fs = ctx->fs, *v;
   bool all_on = true, all_off = true;
   void *old_sampler;
   unsigned unit;

   if (ctx->stippling || !rs || !rs->poly_stipple_enable || !fs ||
       u_reduced_prim(info->mode) != MESA_PRIM_TRIANGLES ||
       rs->fill_front != PIPE_POLYGON_MODE_FILL ||
       rs->fill_back != PIPE_POLYGON_MODE_FILL)
      return false;
   for (unsigned i = 0; i < 32; i++) {
      all_on &= ctx->stipple[i] == ~0u;
      all_off &= !ctx->stipple[i];
   }
   if (all_on)
      return false;
   if (all_off)
      return true;
   if (!(v = ph_fs_stipple_variant(ctx, fs)) || !ph_stipple_texture(ctx)) {
      PH_REFUSE("polygon stipple needs a free texture unit and a discard-free program; drawn unstippled\n");
      return false;
   }
   unit = fs->stipple_unit;
   pipe_sampler_view_reference(&old_view, ctx->bound_views[unit]);
   old_sampler = ctx->bound_samplers[unit];
   ctx->stippling = true;
   ctx->base.bind_fs_state(&ctx->base, v);
   ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1, 0,
                               &ctx->stipple_view);
   ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1,
                                 &ctx->stipple_sampler);
   ctx->base.draw_vbo(&ctx->base, info, drawid_offset, NULL, draws, 1);
   ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1,
                                 &old_sampler);
   ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1, 0,
                               &old_view);
   ctx->base.bind_fs_state(&ctx->base, fs);
   ctx->stippling = false;
   pipe_sampler_view_reference(&old_view, NULL);
   return true;
}


/* One lowered variant of a program with depth comparisons: the bound
 * function and depth-mode swizzle of each compared unit. */
struct ph_compare_variant {
   enum compare_func func[P3D_MAX_TEXTURE_UNITS];
   nir_lower_tex_shadow_swizzle swizzle[P3D_MAX_TEXTURE_UNITS];
   struct ph_fs *fs;
};

static uint32_t
ph_nir_compare_mask(nir_shader *nir)
{
   uint32_t mask = 0;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);

               if (tex->texture_index < 32 &&
                   nir_tex_instr_src_index(tex, nir_tex_src_comparator) >= 0)
                  mask |= BITFIELD_BIT(tex->texture_index);
            }
         }
      }
   }
   return mask;
}

/* The reference R read through a load of its own: the coordinate load then
 * stays a coordinate, and R alone rides on a coordinate unit like the
 * vendor's TEXCOORD helper, perspective-correct and unscaled. */
static void
ph_nir_split_compare_ref(nir_builder *b, nir_tex_instr *tex)
{
   const int cmp = nir_tex_instr_src_index(tex, nir_tex_src_comparator);
   nir_scalar s;
   nir_intrinsic_instr *in, *ref;

   if (cmp < 0)
      return;
   s = nir_scalar_resolved(tex->src[cmp].src.ssa, 0);
   if (!nir_scalar_is_intrinsic(s))
      return;
   in = nir_def_as_intrinsic(s.def);
   if (in->intrinsic == nir_intrinsic_load_deref) {
      nir_deref_instr *deref = nir_src_as_deref(in->src[0]);
      nir_variable *var, *one;

      if (!deref || deref->deref_type != nir_deref_type_var ||
          !(deref->var->data.mode & nir_var_shader_in) ||
          !glsl_type_is_vector_or_scalar(deref->var->type))
         return;
      var = deref->var;
      one = nir_variable_clone(var, b->shader);
      one->type = glsl_float_type();
      one->data.location_frac = var->data.location_frac + s.comp;
      nir_shader_add_variable(b->shader, one);
      b->cursor = nir_after_instr(&in->instr);
      nir_src_rewrite(&tex->src[cmp].src, nir_load_var(b, one));
      return;
   }
   if (in->intrinsic != nir_intrinsic_load_input &&
       in->intrinsic != nir_intrinsic_load_interpolated_input)
      return;
   ref = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &in->instr));
   ref->num_components = 1;
   nir_intrinsic_set_component(ref, nir_intrinsic_component(in) + s.comp);
   nir_def_init(&ref->instr, &ref->def, 1, in->def.bit_size);
   b->cursor = nir_after_instr(&in->instr);
   nir_builder_instr_insert(b, &ref->instr);
   nir_src_rewrite(&tex->src[cmp].src, &ref->def);
}

/* A comparison's R on its own load, without the projector: q reaches no
 * unit (see ph_nir_projective_mask's users), so R is taken as it is, like
 * s and t. */
static bool
ph_nir_drop_compare_projector(nir_builder *b, nir_tex_instr *tex, void *data)
{
   const int proj = nir_tex_instr_src_index(tex, nir_tex_src_projector);

   (void)data;
   if (nir_tex_instr_src_index(tex, nir_tex_src_comparator) < 0)
      return false;
   if (proj >= 0)
      nir_tex_instr_remove_src(tex, proj);
   ph_nir_split_compare_ref(b, tex);
   return true;
}

struct ph_compare_key_ref {
   const enum compare_func *func;
   const nir_lower_tex_shadow_swizzle *swizzle;
};

/* nir_lower_tex_shadow's lowering, R taken as one varying channel and
 * compared with the depth as the sampled copy stores it. */
static bool
ph_nir_lower_compare(nir_builder *b, nir_tex_instr *tex, void *data)
{
   const struct ph_compare_key_ref *key = data;
   const int cmp = nir_tex_instr_src_index(tex, nir_tex_src_comparator);
   const unsigned u = tex->texture_index;

   if (cmp < 0 || u >= P3D_MAX_TEXTURE_UNITS)
      return false;
   b->cursor = nir_after_instr(&tex->instr);
   nir_def *r = nir_fsat(b, tex->src[cmp].src.ssa);
   tex->is_shadow = false;
   nir_tex_instr_remove_src(tex, cmp);
   const unsigned n = tex->def.num_components;
   tex->def.num_components = 4;
   nir_def *pass = nir_b2f32(b, nir_compare_func(b, key->func[u], r,
                                                 nir_channel(b, &tex->def, 0)));
   nir_def *one = nir_imm_float(b, 1.0f), *zero = nir_imm_float(b, 0.0f);
   nir_def *lookup[6] = { pass, zero, zero, one, zero, one };
   const nir_lower_tex_shadow_swizzle *sw = &key->swizzle[u];
   nir_def *c[4] = { lookup[sw->swizzle_r], lookup[sw->swizzle_g],
                     lookup[sw->swizzle_b], lookup[sw->swizzle_a] };
   nir_def *res = nir_vec(b, c, n);

   nir_def_rewrite_uses_after(&tex->def, res);
   return true;
}

/* x * b2f(c) as c ? x : 0: the combiner's CMP selects in one slot where the
 * product costs two (the compared result times the vertex colour). */
static bool
ph_nir_select_b2f_product(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)data;
   if (alu->op != nir_op_fmul)
      return false;
   for (unsigned i = 0; i < 2; i++) {
      nir_scalar c = nir_scalar_resolved(alu->src[i].src.ssa,
                                         alu->src[i].swizzle[0]);
      bool same = true;

      for (unsigned k = 1; k < alu->def.num_components; k++)
         same &= alu->src[i].swizzle[k] == alu->src[i].swizzle[0];
      if (!same || !nir_scalar_is_alu(c) ||
          nir_scalar_alu_op(c) != nir_op_b2f32)
         continue;
      b->cursor = nir_before_instr(&alu->instr);
      nir_def *cond = nir_scalar_chase_alu_src(c, 0).def;
      nir_def *x = nir_ssa_for_alu_src(b, alu, 1 - i);

      cond = nir_channel(b, cond, nir_scalar_chase_alu_src(c, 0).comp);
      nir_def_replace(&alu->def,
                      nir_bcsel(b, cond, x, nir_imm_zero(b, x->num_components,
                                                         x->bit_size)));
      return true;
   }
   return false;
}

/* @fs with each depth comparison evaluated by the combiner on the sampled
 * depth and the clamped R, as the vendor's original-family ICD does (four
 * combiner slots and a coordinate unit for R, MTXPARD 0x1810d0); the
 * texture's depth-mode swizzle applies to the result.  NULL when the
 * program cannot take it. */
static struct ph_fs *
ph_fs_compare_variant(struct ph_context *ctx, struct ph_fs *fs)
{
   struct ph_compare_variant key = {0};
   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR };
   nir_shader *nir;
   struct ph_fs *v;

   u_foreach_bit(u, fs->compare_mask) {
      const struct pipe_sampler_state *s =
         u < PIPE_MAX_SAMPLERS ? ctx->bound_samplers[u] : NULL;
      const struct pipe_sampler_view *view =
         u < PIPE_MAX_SAMPLERS ? ctx->bound_views[u] : NULL;

      if (u >= P3D_MAX_TEXTURE_UNITS || !view)
         return NULL;
      /* A shadow sampler without a comparison is undefined; the vendor
       * returns the lit value then (MTXPARD 0x1812a0). */
      key.func[u] = s && s->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE ?
                    (enum compare_func)s->compare_func : COMPARE_FUNC_ALWAYS;
      key.swizzle[u] = (nir_lower_tex_shadow_swizzle) {
         view->swizzle_r, view->swizzle_g, view->swizzle_b, view->swizzle_a,
      };
   }
   util_dynarray_foreach(&fs->compare_variants, struct ph_compare_variant, c)
      if (!memcmp(c->func, key.func, sizeof(key.func)) &&
          !memcmp(c->swizzle, key.swizzle, sizeof(key.swizzle)))
         return c->fs;
   if (!fs->draw || fs->draw->base.type != PIPE_SHADER_IR_NIR)
      return NULL;
   /* The product with the colour or its select, whichever fits. */
   v = NULL;
   for (unsigned select = 0; select < 2 && !v; select++) {
      const struct ph_compare_key_ref ref = { key.func, key.swizzle };

      nir = nir_shader_clone(NULL, fs->draw->base.ir.nir);
      NIR_PASS(_, nir, nir_shader_tex_pass, ph_nir_drop_compare_projector,
               nir_metadata_control_flow, NULL);
      NIR_PASS(_, nir, nir_shader_tex_pass, ph_nir_lower_compare,
               nir_metadata_control_flow, (void *)&ref);
      NIR_PASS(_, nir, nir_opt_algebraic);
      if (select)
         NIR_PASS(_, nir, nir_shader_alu_pass, ph_nir_select_b2f_product,
                  nir_metadata_control_flow, NULL);
      NIR_PASS(_, nir, nir_opt_dce);
      state.ir.nir = nir;
      v = ph_create_fs(&ctx->base, &state);
      if (v && (v->drop_reason || v->compare_mask || !v->program.valid)) {
         if (ph_env("PARHELIA_TRACE"))
            fprintf(stderr, "parhelia: depth comparison variant %u refused: %s\n",
                    select, v->drop_reason ? v->drop_reason :
                    v->compare_mask ? "comparison left" : "no combiner program");
         ph_delete_fs(&ctx->base, v);
         v = NULL;
      }
      if (!v)
         ralloc_free(nir);
   }
   key.fs = v;
   util_dynarray_append(&fs->compare_variants, key);
   return v;
}

/* The bound view without its swizzle, for the depth itself. */
static struct pipe_sampler_view *
ph_compare_view(struct ph_context *ctx, struct pipe_sampler_view *view)
{
   struct ph_sampler_view *v = (struct ph_sampler_view *)view;

   if (!ph_view_swizzled(view))
      return view;
   if (!v->compare_view) {
      struct pipe_sampler_view templ = *view;

      templ.swizzle_r = PIPE_SWIZZLE_X;
      templ.swizzle_g = PIPE_SWIZZLE_Y;
      templ.swizzle_b = PIPE_SWIZZLE_Z;
      templ.swizzle_a = PIPE_SWIZZLE_W;
      v->compare_view = ctx->base.create_sampler_view(&ctx->base,
                                                      view->texture, &templ);
   }
   return v->compare_view;
}

/* A draw whose program compares depth: replay it with the lowered variant
 * and the compared units' unswizzled views bound. */
static bool
ph_draw_compared(struct ph_context *ctx, const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_start_count_bias *draws)
{
   struct pipe_sampler_view *old[P3D_MAX_TEXTURE_UNITS] = {0};
   struct ph_fs *fs = ctx->fs, *v;

   if (ctx->comparing || !fs || !fs->compare_mask)
      return false;
   if (!(v = ph_fs_compare_variant(ctx, fs))) {
      PH_REFUSE("a depth comparison the combiner cannot take is dropped\n");
      return true;
   }
   ctx->comparing = true;
   u_foreach_bit(u, fs->compare_mask) {
      struct pipe_sampler_view *raw = ph_compare_view(ctx, ctx->bound_views[u]);

      pipe_sampler_view_reference(&old[u], ctx->bound_views[u]);
      ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, u, 1, 0,
                                  &raw);
   }
   ctx->base.bind_fs_state(&ctx->base, v);
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr, "parhelia: depth comparison fs=%p variant=%p valid=%d ramps=%u units=%#x\n",
              (void *)fs, (void *)v, v->program.valid, v->program.ramp_count,
              v->sampler_mask);
   ctx->base.draw_vbo(&ctx->base, info, drawid_offset, NULL, draws, 1);
   ctx->base.bind_fs_state(&ctx->base, fs);
   u_foreach_bit(u, fs->compare_mask) {
      ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, u, 1, 0,
                                  &old[u]);
      pipe_sampler_view_reference(&old[u], NULL);
   }
   ctx->comparing = false;
   return true;
}

/* @fs with its colour alpha multiplied by the coverage table on the unit
 * after its samplers, read at gl_PointCoord: GL's smooth point drawn as a
 * sprite, coverage clamp(size / 2 - |pc - 0.5| * size, 0, 1) as Mesa's
 * nir_lower_point_smooth computes it.  NULL when the program cannot take
 * it. */
static struct ph_fs *
ph_fs_smooth_variant(struct ph_context *ctx, struct ph_fs *fs)
{
   const unsigned unit = util_last_bit(fs->sampler_mask);
   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR };
   nir_variable *pntc = NULL;
   nir_shader *nir;
   nir_tex_instr *tex;
   unsigned stores = 0;
   struct ph_fs *v;

   if (fs->smooth || fs->smooth_failed)
      return fs->smooth;
   fs->smooth_failed = true;
   if (!fs->draw || fs->draw->base.type != PIPE_SHADER_IR_NIR ||
       unit >= MIN2(ph_screen(ctx->base.screen)->texture_units,
                    PH_TEXTURE_UNITS_DRIVEN))
      return NULL;
   nir = nir_shader_clone(NULL, fs->draw->base.ir.nir);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_at(nir_before_impl(impl));

   nir_foreach_shader_in_variable(var, nir)
      if (var->data.location == VARYING_SLOT_PNTC)
         pntc = var;
   if (!pntc) {
      unsigned slots = 0;

      /* draw scans inputs by driver location: take the next free one */
      nir_foreach_shader_in_variable(var, nir)
         slots = MAX2(slots, var->data.driver_location +
                             glsl_count_attribute_slots(var->type, false));
      pntc = nir_variable_create(nir, nir_var_shader_in, glsl_vec_type(2),
                                 "gl_PointCoord");
      pntc->data.location = VARYING_SLOT_PNTC;
      pntc->data.driver_location = slots;
      nir->num_inputs = MAX2(nir->num_inputs, slots + 1);
   }
   tex = nir_tex_instr_create(nir, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = tex->sampler_index = unit;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                     nir_channels(&b, nir_load_var(&b, pntc), 0x3));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *coverage = nir_channel(&b, &tex->def, 3);

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *st;
         nir_variable *out;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         st = nir_instr_as_intrinsic(instr);
         if (st->intrinsic != nir_intrinsic_store_deref ||
             !(out = nir_intrinsic_get_var(st, 0)) ||
             out->data.mode != nir_var_shader_out ||
             (out->data.location != FRAG_RESULT_COLOR &&
              out->data.location != FRAG_RESULT_DATA0))
            continue;
         if (nir_intrinsic_write_mask(st) != 0xf ||
             st->src[1].ssa->num_components != 4) {
            ralloc_free(nir);
            return NULL;
         }
         b.cursor = nir_before_instr(instr);
         nir_def *c = st->src[1].ssa;
         nir_src_rewrite(&st->src[1],
                         nir_vector_insert_imm(&b, c,
                            nir_fmul(&b, nir_channel(&b, c, 3), coverage), 3));
         stores++;
      }
   }
   if (!stores) {
      ralloc_free(nir);
      return NULL;
   }
   nir->info.inputs_read |= VARYING_BIT_PNTC;
   BITSET_SET(nir->info.textures_used, unit);
   BITSET_SET(nir->info.samplers_used, unit);
   nir_progress(true, impl, nir_metadata_none);

   state.ir.nir = nir;
   v = ph_create_fs(&ctx->base, &state);
   if (!v || v->drop_reason || !v->program.valid ||
       !(v->sampler_mask & BITFIELD_BIT(unit))) {
      ph_delete_fs(&ctx->base, v);
      ralloc_free(nir);
      return NULL;
   }
   fs->smooth = v;
   fs->smooth_unit = unit;
   fs->smooth_failed = false;
   return v;
}

/* The coverage table of a smooth point of @size pixels over gl_PointCoord,
 * texel centres as the lookup tables place them, at their size; the
 * least recently used of more than eight sizes is released. */
static struct pipe_sampler_view *
ph_coverage_view(struct ph_context *ctx, float size)
{
   const unsigned n = ph_fs_lookup_size(PH_FS_RAMP_PLANE);
   const struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = n, .height0 = n, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW,
   };
   struct ph_coverage_tex *t;
   struct pipe_sampler_view vt;
   struct pipe_box box;
   uint8_t *data;

   ctx->coverage_clock++;
   for (unsigned i = 0; i < ctx->coverage_count; i++)
      if (ctx->coverage[i].size == size) {
         ctx->coverage[i].last_use = ctx->coverage_clock;
         return ctx->coverage[i].view;
      }
   if (ctx->coverage_count >= 8) {
      unsigned lru = 0;

      for (unsigned i = 1; i < ctx->coverage_count; i++)
         if (ctx->coverage[i].last_use < ctx->coverage[lru].last_use)
            lru = i;
      if (ctx->triangles || ctx->points || ctx->lines)
         ph_submit_vertices(ctx);
      pipe_sampler_view_reference(&ctx->coverage[lru].view, NULL);
      pipe_resource_reference(&ctx->coverage[lru].tex, NULL);
      ctx->coverage[lru] = ctx->coverage[--ctx->coverage_count];
   }
   if (ctx->coverage_count == ctx->coverage_cap) {
      const unsigned cap = ctx->coverage_cap ? ctx->coverage_cap * 2 : 2;
      struct ph_coverage_tex *grown =
         realloc(ctx->coverage, cap * sizeof(*grown));

      if (!grown)
         return NULL;
      ctx->coverage = grown;
      ctx->coverage_cap = cap;
   }
   if (!(data = malloc((size_t)n * n * 4)))
      return NULL;
   for (unsigned y = 0; y < n; y++)
      for (unsigned x = 0; x < n; x++) {
         const float dx = (x + 0.5f) / n - 0.5f, dy = (y + 0.5f) / n - 0.5f;
         const float a = CLAMP(0.5f * size - sqrtf(dx * dx + dy * dy) * size,
                               0.0f, 1.0f);
         uint8_t *texel = data + 4 * ((size_t)y * n + x);

         texel[0] = texel[1] = texel[2] = 255;
         texel[3] = (uint8_t)lrintf(a * 255.0f);
      }
   t = &ctx->coverage[ctx->coverage_count];
   memset(t, 0, sizeof(*t));
   t->tex = ctx->base.screen->resource_create(ctx->base.screen, &templ);
   if (t->tex) {
      u_box_2d(0, 0, n, n, &box);
      ctx->base.texture_subdata(&ctx->base, t->tex, 0, PIPE_MAP_WRITE, &box,
                                data, n * 4, 0);
      u_sampler_view_default_template(&vt, t->tex, t->tex->format);
      t->view = ctx->base.create_sampler_view(&ctx->base, t->tex, &vt);
   }
   free(data);
   if (!t->view) {
      pipe_resource_reference(&t->tex, NULL);
      return NULL;
   }
   t->size = size;
   t->last_use = ctx->coverage_clock;
   ctx->coverage_count++;
   return t->view;
}

/* GL_POINT_SMOOTH on the original family: replay the points as sprites with
 * the coverage variant of the program, as the shader-based smooth points
 * of Mesa's drivers do.  The vendor draws an unsnapped square without any
 * coverage; a program without a free unit keeps the aliased disc. */
static bool
ph_draw_smooth_points(struct ph_context *ctx, const struct pipe_draw_info *info,
                      unsigned drawid_offset,
                      const struct pipe_draw_start_count_bias *draws)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer;
   const struct pipe_caps *caps = &ctx->base.screen->caps;
   struct pipe_sampler_view *old_view = NULL, *view;
   struct ph_fs *fs = ctx->fs, *v;
   void *old_sampler;
   unsigned unit;
   float size;

   if (ctx->smoothing || !rs || !rs->point_smooth || rs->multisample ||
       rs->point_quad_rasterization || rs->point_size_per_vertex || !fs ||
       u_reduced_prim(info->mode) != MESA_PRIM_POINTS ||
       !ph_original_family(ph_screen(ctx->base.screen)->chip_id))
      return false;
   size = CLAMP(rs->point_size, caps->min_point_size_aa,
                caps->max_point_size_aa);
   if (!ctx->coverage_sampler) {
      const struct pipe_sampler_state templ = {
         .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .min_img_filter = PIPE_TEX_FILTER_LINEAR,
         .mag_img_filter = PIPE_TEX_FILTER_LINEAR,
         .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      };

      ctx->coverage_sampler = ctx->base.create_sampler_state(&ctx->base,
                                                             &templ);
   }
   if (!ctx->coverage_sampler || !(v = ph_fs_smooth_variant(ctx, fs)) ||
       !(view = ph_coverage_view(ctx, size))) {
      PH_REFUSE("a smooth point without a free texture unit for its coverage keeps the aliased disc\n");
      return false;
   }
   unit = fs->smooth_unit;
   pipe_sampler_view_reference(&old_view, ctx->bound_views[unit]);
   old_sampler = ctx->bound_samplers[unit];
   ctx->smoothing = true;
   ctx->smooth_rast = *rs;
   ctx->smooth_rast.point_smooth = 0;
   ctx->smooth_rast.point_quad_rasterization = 1;
   ctx->smooth_rast.sprite_coord_enable = 0;
   ctx->smooth_rast.point_size = size;
   ctx->base.bind_rasterizer_state(&ctx->base, &ctx->smooth_rast);
   ctx->base.bind_fs_state(&ctx->base, v);
   ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1, 0,
                               &view);
   ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1,
                                 &ctx->coverage_sampler);
   ctx->base.draw_vbo(&ctx->base, info, drawid_offset, NULL, draws, 1);
   ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1,
                                 &old_sampler);
   ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, unit, 1, 0,
                               &old_view);
   ctx->base.bind_fs_state(&ctx->base, fs);
   ctx->base.bind_rasterizer_state(&ctx->base, (void *)rs);
   ctx->smoothing = false;
   pipe_sampler_view_reference(&old_view, NULL);
   return true;
}

static void
ph_lookup_release(struct ph_context *ctx, unsigned i)
{
   struct ph_lookup_tex *t = &ctx->lookup_tex[i];

   /* Queued draws keep sampling it until they are submitted. */
   if (ctx->triangles || ctx->points || ctx->lines)
      ph_submit_vertices(ctx);
   ctx->lookup_bytes -= t->bytes;
   pipe_sampler_view_reference(&t->view, NULL);
   pipe_resource_reference(&t->tex, NULL);
   free(t->key);
   *t = ctx->lookup_tex[--ctx->lookup_tex_count];
}

/* Drop the lookup textures of program @uid, whose shader is deleted. */
static void
ph_lookup_forget(struct ph_context *ctx, uint32_t uid)
{
   for (unsigned i = ctx->lookup_tex_count; i-- > 0;)
      if (ctx->lookup_tex[i].uid == uid)
         ph_lookup_release(ctx, i);
}

/* The lookup texture of @r's program for the uniform values it reads.
 * Tables stay cached, least recently used first out, within a memory
 * budget (PARHELIA_LOOKUP_CACHE_MB, default 16). */
static struct pipe_sampler_view *
ph_lookup_view(struct ph_context *ctx, const struct ph_fs_ramp *ramp)
{
   const bool cube = ramp->kind == PH_FS_RAMP_DIRECTION;
   struct ph_fs_ramp dom = *ramp;
   const struct ph_fs_ramp *r = &dom;
   float plo[3], pspan[3];

   if (ramp->fn->pixel) {
      ph_pixel_domain(ctx, ramp, plo, pspan);
      for (unsigned d = 0; d < ramp->dims; d++) {
         dom.size[d] = (uint16_t)pspan[d];
         dom.extent[d] = ramp->comp[d] ? ctx->fb.height : ctx->fb.width;
      }
   }
   const unsigned n = dom.size[0], h = cube ? n : dom.size[1];
   const size_t bytes = (size_t)n * h * (cube ? 6 : 1) * 4;
   struct pipe_resource templ = {
      .target = cube ? PIPE_TEXTURE_CUBE : PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = n, .height0 = h, .depth0 = 1, .array_size = cube ? 6 : 1,
      .bind = PIPE_BIND_SAMPLER_VIEW,
   };
   static size_t budget;
   struct ph_lookup_tex *t;
   struct pipe_sampler_view vt;
   struct pipe_box box;
   uint32_t *key;
   unsigned key_count;
   uint8_t *data;

   if (!budget) {
      const char *e = getenv("PARHELIA_LOOKUP_CACHE_MB");

      budget = (size_t)(e && atoi(e) > 0 ? atoi(e) : 16) << 20;
   }
   ph_fs_snapshot_constants(ctx);
   if (ctx->fs && ctx->fs->point_deriv)
      ph_fs_set_point_derivatives(&ctx->fs->program, ctx->pntc_deriv);
   const float *drv = ctx->fs ? ctx->fs->program.drv : NULL;
   if (!ph_fs_lookup_key(r->fn, ctx->fs_const_snap, ctx->fs_const_snap_size,
                         drv, &key, &key_count))
      return NULL;
   /* A table over the varying's range is built for the batch's domain. */
   if (!cube) {
      uint32_t *grown = realloc(key, (key_count + 2 * r->dims) * sizeof(*key));

      if (!grown) {
         free(key);
         return NULL;
      }
      key = grown;
      for (unsigned d = 0; d < r->dims; d++) {
         dom.lo[d] = ramp->fn->pixel ? plo[d] : ctx->dom_lo[2 + r->unit][d];
         dom.span[d] = ramp->fn->pixel ? pspan[d] : ctx->dom_span[2 + r->unit][d];
         memcpy(&key[key_count++], &dom.lo[d], 4);
         memcpy(&key[key_count++], &dom.span[d], 4);
      }
   }
   ctx->lookup_clock++;
   for (unsigned i = 0; i < ctx->lookup_tex_count; i++) {
      t = &ctx->lookup_tex[i];
      if (t->uid == r->fn->uid && t->size_px == n &&
          t->key_count == key_count &&
          !memcmp(t->key, key, key_count * sizeof(*key))) {
         t->last_use = ctx->lookup_clock;
         free(key);
         return t->view;
      }
   }
   while (ctx->lookup_tex_count && ctx->lookup_bytes + bytes > budget) {
      unsigned lru = 0;

      for (unsigned i = 1; i < ctx->lookup_tex_count; i++)
         if (ctx->lookup_tex[i].last_use < ctx->lookup_tex[lru].last_use)
            lru = i;
      ph_lookup_release(ctx, lru);
   }
   if (ctx->lookup_tex_count == ctx->lookup_tex_cap) {
      const unsigned cap = ctx->lookup_tex_cap ? ctx->lookup_tex_cap * 2 : 4;
      struct ph_lookup_tex *grown =
         realloc(ctx->lookup_tex, cap * sizeof(*grown));

      if (!grown) {
         free(key);
         return NULL;
      }
      ctx->lookup_tex = grown;
      ctx->lookup_tex_cap = cap;
   }
   data = malloc(bytes);
   if (!data) {
      free(key);
      return NULL;
   }
   {
      const int64_t t0 = os_time_get_nano();

      if (!ph_fs_lookup_fill(r->fn, r, ctx->fs_const_snap,
                             ctx->fs_const_snap_size, drv, data)) {
         free(data);
         free(key);
         return NULL;
      }
      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr, "parhelia: lookup %u filled %ux%u in %.1f ms "
                 "(%u cached), domain [%g, +%g] [%g, +%g]\n", r->fn->uid, n, h,
                 (os_time_get_nano() - t0) / 1e6, ctx->lookup_tex_count,
                 r->lo[0], r->span[0], r->lo[1], r->span[1]);
   }
   t = &ctx->lookup_tex[ctx->lookup_tex_count];
   memset(t, 0, sizeof(*t));
   t->tex = ctx->base.screen->resource_create(ctx->base.screen, &templ);
   if (t->tex) {
      u_box_3d(0, 0, 0, n, h, cube ? 6 : 1, &box);
      ctx->base.texture_subdata(&ctx->base, t->tex, 0, PIPE_MAP_WRITE, &box,
                                data, n * 4, n * h * 4);
      u_sampler_view_default_template(&vt, t->tex, t->tex->format);
      t->view = ctx->base.create_sampler_view(&ctx->base, t->tex, &vt);
   }
   free(data);
   if (!t->view) {
      pipe_resource_reference(&t->tex, NULL);
      free(key);
      return NULL;
   }
   t->uid = r->fn->uid;
   t->size_px = n;
   t->key = key;
   t->key_count = key_count;
   t->bytes = bytes;
   t->last_use = ctx->lookup_clock;
   ctx->lookup_bytes += bytes;
   ctx->lookup_tex_count++;
   return t->view;
}

/* The ramp texture of @r: texel (x, y, z) holds code index * step in the
 * channel of each floored component, zero elsewhere. */
static struct pipe_sampler_view *
ph_ramp_view(struct ph_context *ctx, const struct ph_fs_ramp *r)
{
   static const unsigned byte_of[4] = { 2, 1, 0, 3 };   /* B8G8R8A8 */
   /* A coordinate unit fetches nothing; any small surface satisfies the
    * descriptor. */
   struct pipe_resource templ = {
      .target = r->dims == 3 && r->kind != PH_FS_RAMP_PASS ? PIPE_TEXTURE_3D :
                                                           PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .width0 = r->kind == PH_FS_RAMP_PASS ? 2 : r->size[0],
      .height0 = r->kind == PH_FS_RAMP_PASS ? 2 : r->size[1],
      .depth0 = r->dims == 3 && r->kind != PH_FS_RAMP_PASS ? r->size[2] : 1,
      .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW,
   };
   struct ph_ramp_tex *t;
   struct pipe_sampler_view vt;
   struct pipe_box box;
   uint8_t *data;

   if (r->kind != PH_FS_RAMP_FLOOR && r->kind != PH_FS_RAMP_PASS)
      return ph_lookup_view(ctx, r);
   for (unsigned i = 0; i < ctx->ramp_tex_count; i++) {
      const struct ph_fs_ramp *k = &ctx->ramp_tex[i].key;

      if (k->kind != r->kind)
         continue;
      /* Every coordinate unit shares one small surface it never fetches. */
      if (r->kind == PH_FS_RAMP_PASS ||
          (k->dims == r->dims && !memcmp(k->size, r->size, sizeof(k->size)) &&
           !memcmp(k->comp, r->comp, sizeof(k->comp))))
         return ctx->ramp_tex[i].view;
   }
   if (ctx->ramp_tex_count == ctx->ramp_tex_cap) {
      const unsigned cap = ctx->ramp_tex_cap ? ctx->ramp_tex_cap * 2 : 4;
      struct ph_ramp_tex *grown = realloc(ctx->ramp_tex, cap * sizeof(*grown));

      if (!grown)
         return NULL;
      ctx->ramp_tex = grown;
      ctx->ramp_tex_cap = cap;
   }
   data = calloc((size_t)templ.width0 * templ.height0 * templ.depth0, 4);
   if (!data)
      return NULL;
   for (unsigned z = 0; z < templ.depth0; z++)
      for (unsigned y = 0; y < templ.height0; y++)
         for (unsigned x = 0; x < templ.width0; x++) {
            const unsigned idx[3] = { x, y, z };
            uint8_t *texel = data + 4 * ((z * templ.height0 + y) *
                                         templ.width0 + x);

            for (unsigned d = 0; d < r->dims && r->kind != PH_FS_RAMP_PASS; d++)
               texel[byte_of[r->comp[d] & 3]] =
                  idx[d] * ph_fs_ramp_step(r->size[d]);
         }
   t = &ctx->ramp_tex[ctx->ramp_tex_count];
   memset(t, 0, sizeof(*t));
   t->key = *r;
   t->tex = ctx->base.screen->resource_create(ctx->base.screen, &templ);
   if (t->tex) {
      u_box_3d(0, 0, 0, templ.width0, templ.height0, templ.depth0, &box);
      ctx->base.texture_subdata(&ctx->base, t->tex, 0, PIPE_MAP_WRITE, &box,
                                data, templ.width0 * 4,
                                templ.width0 * templ.height0 * 4);
      u_sampler_view_default_template(&vt, t->tex, t->tex->format);
      t->view = ctx->base.create_sampler_view(&ctx->base, t->tex, &vt);
   }
   free(data);
   if (!t->view) {
      pipe_resource_reference(&t->tex, NULL);
      return NULL;
   }
   ctx->ramp_tex_count++;
   return t->view;
}

/* Replay the draw with each floor ramp bound on its unit, as the polygon
 * stipple binds its pattern. */
static bool
ph_draw_ramped(struct ph_context *ctx, const struct pipe_draw_info *info,
               unsigned drawid_offset,
               const struct pipe_draw_start_count_bias *draws)
{
   const struct ph_fs_program *prog = ctx->fs ? &ctx->fs->program : NULL;
   struct pipe_sampler_view *old_view[2 * P3D_MAX_TEXTURE_UNITS] = { NULL };
   void *old_sampler[2 * P3D_MAX_TEXTURE_UNITS];

   if (ctx->ramping || !prog || !prog->valid ||
       (!prog->ramp_count && !ctx->fs->tap_count))
      return false;
   if (!ctx->ramp_sampler) {
      const struct pipe_sampler_state templ = {
         .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .min_img_filter = PIPE_TEX_FILTER_NEAREST,
         .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
         .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      };

      ctx->ramp_sampler = ctx->base.create_sampler_state(&ctx->base, &templ);
   }
   if (!ctx->lookup_sampler) {
      const struct pipe_sampler_state templ = {
         .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .min_img_filter = PIPE_TEX_FILTER_LINEAR,
         .mag_img_filter = PIPE_TEX_FILTER_LINEAR,
         .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      };

      ctx->lookup_sampler = ctx->base.create_sampler_state(&ctx->base, &templ);
   }
   for (unsigned i = 0; i < prog->ramp_count; i++)
      if (!ctx->ramp_sampler || !ctx->lookup_sampler ||
          !ph_ramp_view(ctx, &prog->ramps[i])) {
         PH_REFUSE("a draw whose floor ramp texture cannot be built is dropped\n");
         return true;
      }
   ctx->ramping = true;
   for (unsigned i = 0; i < prog->ramp_count; i++) {
      const unsigned u = prog->ramps[i].unit;
      struct pipe_sampler_view *view = ph_ramp_view(ctx, &prog->ramps[i]);

      pipe_sampler_view_reference(&old_view[i], ctx->bound_views[u]);
      old_sampler[i] = ctx->bound_samplers[u];
      ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, u, 1, 0,
                                  &view);
      ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT, u, 1,
                                    prog->ramps[i].kind == PH_FS_RAMP_FLOOR ||
                                    (prog->ramps[i].fn && prog->ramps[i].fn->pixel) ?
                                    &ctx->ramp_sampler : &ctx->lookup_sampler);
   }
   /* A tap's unit samples its texture through the same view and sampler. */
   for (unsigned t = 0; t < ctx->fs->tap_count; t++) {
      const struct ph_tap *tap = &ctx->fs->taps[t];
      struct pipe_sampler_view *view = ctx->bound_views[tap->gl_unit];
      void *sampler = ctx->bound_samplers[tap->gl_unit];
      const unsigned k = prog->ramp_count + t;

      pipe_sampler_view_reference(&old_view[k], ctx->bound_views[tap->unit]);
      old_sampler[k] = ctx->bound_samplers[tap->unit];
      ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, tap->unit,
                                  1, 0, &view);
      ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT,
                                    tap->unit, 1, &sampler);
   }
   ctx->base.draw_vbo(&ctx->base, info, drawid_offset, NULL, draws, 1);
   for (unsigned t = 0; t < ctx->fs->tap_count; t++) {
      const unsigned u = ctx->fs->taps[t].unit, k = prog->ramp_count + t;

      ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT, u, 1,
                                    &old_sampler[k]);
      ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, u, 1, 0,
                                  &old_view[k]);
      pipe_sampler_view_reference(&old_view[k], NULL);
   }
   for (unsigned i = 0; i < prog->ramp_count; i++) {
      const unsigned u = prog->ramps[i].unit;

      ctx->base.bind_sampler_states(&ctx->base, MESA_SHADER_FRAGMENT, u, 1,
                                    &old_sampler[i]);
      ctx->base.set_sampler_views(&ctx->base, MESA_SHADER_FRAGMENT, u, 1, 0,
                                  &old_view[i]);
      pipe_sampler_view_reference(&old_view[i], NULL);
   }
   ctx->ramping = false;
   return true;
}

static void
ph_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info,
            unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
            const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct ph_context *ctx = ph_context(pipe);
   const void *indices = NULL;
   bool derived;

   if (num_draws > 1) {
      util_draw_multi(pipe, info, drawid_offset, indirect, draws, num_draws);
      return;
   }
   if (!num_draws || indirect || ctx->submit_error)
      return;
   if (ph_draw_compared(ctx, info, drawid_offset, draws))
      return;
   if (ph_draw_stippled(ctx, info, drawid_offset, draws))
      return;
   if (ph_draw_smooth_points(ctx, info, drawid_offset, draws))
      return;
   if (ph_draw_ramped(ctx, info, drawid_offset, draws))
      return;
   if (ph_draw_broadcast(ctx, info, drawid_offset, draws))
      return;
   /* An empty scissor rectangle passes nothing, but the engine reads a zero
    * maximum as no limit (0x132c is exclusive): skip the draw. */
   if (ctx->rasterizer && ctx->rasterizer->scissor &&
       (ctx->scissor.maxx <= ctx->scissor.minx ||
        ctx->scissor.maxy <= ctx->scissor.miny))
      return;
   if (ctx->fs && ctx->fs->drop_reason) {
      PH_REFUSE("draws with an ARB fragment program the combiner cannot "
                "express are dropped (%s)\n", ctx->fs->drop_reason);
      return;
   }
   if (ctx->fs && ctx->fs->colour_saturated && ctx->fb.nr_cbufs &&
       ctx->fb.cbufs[0].texture &&
       !util_format_is_unorm(ctx->fb.cbufs[0].format)) {
      PH_REFUSE("a saturated-colour program on a target that does not "
                "clamp is dropped\n");
      return;
   }
   {
      struct ph_fs_program *kill_prog;
      enum pipe_compare_func kill_func;
      uint8_t kill_ref;

      ph_fs_snapshot_constants(ctx);
      if (ctx->fs && ctx->fs->program.valid &&
          ((ctx->fs->program.const_ref_count && ctx->fs_const_snap_size) ||
           ctx->fs->program.carriers) &&
          !ph_fs_update_constants(&ctx->fs->program, ctx->fs_const_snap,
                                  ctx->fs_const_snap_size)) {
         PH_REFUSE("draws whose fragment uniforms no combiner variant can "
                   "hold are dropped\n");
         return;
      }
      switch (ph_fs_kill_route(ctx, &kill_prog, &kill_func, &kill_ref)) {
      case PH_KILL_ALL:
         return;
      case PH_KILL_UNSUPPORTED:
         PH_REFUSE("a draw whose discard has no alpha-test form is dropped\n");
         return;
      default:
         break;
      }
   }
   ph_fs_snapshot_constants(ctx);
   ph_update_effective_views(ctx);
   for (unsigned i = 0; i < ctx->draw->pt.nr_vertex_buffers; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->draw->pt.vertex_buffer[i];
      const void *map = vb->is_user_buffer ? vb->buffer.user :
                        (vb->buffer.resource ?
                         ph_resource(vb->buffer.resource)->malloc_data : NULL);
      draw_set_mapped_vertex_buffer(ctx->draw, i, map, ~0u);
   }
   if (info->index_size) {
      indices = info->has_user_indices ? info->index.user :
                (info->index.resource ?
                 ph_resource(info->index.resource)->malloc_data : NULL);
      draw_set_indexes(ctx->draw, indices, info->index_size, ~0u);
   }
   if (ctx->user_constants[MESA_SHADER_VERTEX]) {
      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, 0,
         (const uint8_t *)ctx->user_constants[MESA_SHADER_VERTEX] +
            ctx->constant_offset[MESA_SHADER_VERTEX],
         ctx->constant_size[MESA_SHADER_VERTEX]);
   } else if (ctx->constants[MESA_SHADER_VERTEX]) {
      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, 0,
         (const uint8_t *)ph_resource(ctx->constants[MESA_SHADER_VERTEX])->malloc_data +
            ctx->constant_offset[MESA_SHADER_VERTEX],
         ctx->constant_size[MESA_SHADER_VERTEX]);
   } else {
      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, 0, NULL, 0);
   }
   if (ctx->fs && ctx->fs->depth_from_texture &&
       ph_draw_depth_points(ctx, info, drawid_offset, draws))
      goto unbind;
   derived = u_reduced_prim(info->mode) != MESA_PRIM_TRIANGLES ||
             (ctx->rasterizer &&
              (ctx->rasterizer->fill_front != PIPE_POLYGON_MODE_FILL ||
               ctx->rasterizer->fill_back != PIPE_POLYGON_MODE_FILL));
   if (derived != ctx->derived_triangles) {
      /* The cull selector is per batch; keep polygons and derived
       * triangles in separate batches. */
      if (ctx->triangles)
         ph_submit_vertices(ctx);
      ctx->derived_triangles = derived;
   }
   const bool quantize = ph_logicop_quantize(ctx);
   const bool fix = (quantize || (ctx->fs && ctx->fs->ucoord_count) ||
                     (ctx->fs && ctx->fs->program.valid &&
                      (ctx->fs->program.ramp_count || ctx->fs->tap_count ||
                       ctx->fs->program.route[0] >= 0 ||
                       ctx->fs->program.route[1] >= 0)) ||
                     ph_fs_samples_rect(ctx->fs) ||
                     (ctx->fs && ctx->fs->depth_uniform)) &&
                    (ctx->quant_stage ||
                     (ctx->quant_stage = ph_quant_create_stage(ctx)));
   if (derived || fix)
      ctx->tcl_reason[derived ? PH_TCL_NO_PRIMITIVE : PH_TCL_NO_FIXUP]++;
   if (!derived && !fix && ph_draw_tcl(ctx, info, draws))
      goto unbind;
   /* A software-transformed batch must not inherit an object-space one. */
   if (ctx->transform && ctx->triangles)
      ph_submit_vertices(ctx);
   ctx->transform = false;
   if (fix) {
      ph_quant_prepare(ctx, quantize);
      ph_fix_rects(ctx, (struct ph_quant_stage *)ctx->quant_stage);
      if (!ph_fix_depth(ctx, (struct ph_quant_stage *)ctx->quant_stage)) {
         PH_REFUSE("a draw whose uniform fragment depth cannot be evaluated is dropped\n");
         goto unbind;
      }
      if (!ph_fix_coords(ctx, (struct ph_quant_stage *)ctx->quant_stage)) {
         draw_remove_extra_vertex_attribs(ctx->draw);
         PH_REFUSE("a draw whose uniform texture coordinate cannot be evaluated is dropped\n");
         goto unbind;
      }
      ph_fix_taps(ctx, (struct ph_quant_stage *)ctx->quant_stage);
      ph_fix_routes(ctx, (struct ph_quant_stage *)ctx->quant_stage);
      if (!ph_fix_ramps(ctx, (struct ph_quant_stage *)ctx->quant_stage)) {
         draw_remove_extra_vertex_attribs(ctx->draw);
         PH_REFUSE("a draw whose floored varying the vertex stage does not write is dropped\n");
         goto unbind;
      }
      draw_set_rasterize_stage(ctx->draw, ctx->quant_stage);
   }
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(ctx->draw);
   if (fix) {
      draw_set_rasterize_stage(ctx->draw, ctx->hw_stage);
      draw_remove_extra_vertex_attribs(ctx->draw);
   }
unbind:
   for (unsigned i = 0; i < ctx->draw->pt.nr_vertex_buffers; i++)
      draw_set_mapped_vertex_buffer(ctx->draw, i, NULL, 0);
   if (indices)
      draw_set_indexes(ctx->draw, NULL, 0, 0);
   if (ctx->proxy_count) {
      ph_submit_vertices(ctx);
      ph_proxy_writeback(ctx);
   }
}

static void *ph_memdup(const void *src, size_t size)
{
   void *p = MALLOC(size);
   if (p)
      memcpy(p, src, size);
   return p;
}
static void *ph_create_blend(struct pipe_context *p,
                             const struct pipe_blend_state *s)
{ (void)p; return ph_memdup(s, sizeof(*s)); }
/* The blend state the draw paths see: the bound one, or with the colour
 * stub its copy that writes no colour plane. */
static void
ph_update_blend(struct ph_context *ctx)
{
   if (!ctx->colour_stub_bound || !ctx->blend_api) {
      ctx->blend = ctx->blend_api;
      return;
   }
   ctx->blend_nocolour = *ctx->blend_api;
   ctx->blend_nocolour.logicop_enable = false;
   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
      ctx->blend_nocolour.rt[i].blend_enable = false;
      ctx->blend_nocolour.rt[i].colormask = 0;
   }
   ctx->blend = &ctx->blend_nocolour;
}
static void ph_bind_blend(struct pipe_context *p, void *s)
{
   struct ph_context *ctx = ph_context(p);
   if (ctx->blend_api == s)
      return;
   ph_submit_vertices(ctx);
   ctx->blend_api = s;
   ph_update_blend(ctx);
}
static void *ph_create_dsa(struct pipe_context *p,
                           const struct pipe_depth_stencil_alpha_state *s)
{ (void)p; return ph_memdup(s, sizeof(*s)); }
static void ph_bind_dsa(struct pipe_context *p, void *s)
{
   struct ph_context *ctx = ph_context(p);
   if (ctx->dsa == s)
      return;
   ph_submit_vertices(ctx);
   ctx->dsa = s;
}
static void *ph_create_rasterizer(struct pipe_context *p,
                                  const struct pipe_rasterizer_state *s)
{ (void)p; return ph_memdup(s, sizeof(*s)); }
static void ph_bind_rasterizer(struct pipe_context *p, void *s)
{
   struct ph_context *ctx = ph_context(p);
   if (ctx->rasterizer == s)
      return;
   ph_submit_vertices(ctx);
   /* Early software rejection avoids transforming and uploading backfaces;
    * the independently hardware-proven 0x1130 selector remains programmed as
    * the final raster guard for every surviving batch.  Assign afterwards:
    * the draw flush lets wide-line/point stages re-bind their saved state. */
   draw_set_rasterizer_state(ctx->draw, s, s);
   ctx->rasterizer = s;
}
static void ph_delete_state(struct pipe_context *p, void *s)
{ (void)p; FREE(s); }
static void *ph_create_sampler(struct pipe_context *p,
                               const struct pipe_sampler_state *s)
{ (void)p; return ph_memdup(s, sizeof(*s)); }
static void ph_bind_samplers(struct pipe_context *p, mesa_shader_stage sh,
                             unsigned start, unsigned count, void **states)
{
   struct ph_context *ctx = ph_context(p);
   if (sh != MESA_SHADER_FRAGMENT || start + count > PIPE_MAX_SAMPLERS)
      return;
   for (unsigned i = 0; i < count; i++)
      if (ctx->bound_samplers[start + i] != states[i])
         goto changed;
   return;
changed:
   ph_submit_vertices(ctx);
   for (unsigned i = 0; i < count; i++)
      ctx->bound_samplers[start + i] = states[i];
   ctx->num_bound_samplers = MAX2(ctx->num_bound_samplers, start + count);
   while (ctx->num_bound_samplers &&
          !ctx->bound_samplers[ctx->num_bound_samplers - 1])
      ctx->num_bound_samplers--;
}

static struct pipe_sampler_view *
ph_create_sampler_view(struct pipe_context *p, struct pipe_resource *texture,
                       const struct pipe_sampler_view *templ)
{
   struct ph_sampler_view *view;
   /* The view's target is the GL target, which need not be the resource's:
    * a 1-D view reads a one-row 2-D image, and a volume or cube-map view
    * reads the slices or faces of one. */
   if (!templ || !texture ||
       (templ->target != PIPE_TEXTURE_2D && templ->target != PIPE_TEXTURE_RECT &&
        templ->target != PIPE_TEXTURE_1D && templ->target != PIPE_TEXTURE_3D &&
        templ->target != PIPE_TEXTURE_CUBE) ||
       templ->u.tex.first_level > templ->u.tex.last_level ||
       templ->u.tex.last_level > texture->last_level)
      return NULL;
   view = CALLOC_STRUCT(ph_sampler_view);
   if (!view)
      return NULL;
   if (ph_env("PARHELIA_TRACE_VIEWS"))
      fprintf(stderr, "parhelia: view target=%u format=%s on %s swizzle=%u%u%u%u\n",
              templ->target, util_format_short_name(templ->format),
              util_format_short_name(texture->format), templ->swizzle_r,
              templ->swizzle_g, templ->swizzle_b, templ->swizzle_a);
   view->base = *templ;
   view->base.reference.count = 1;
   view->base.texture = NULL;
   view->base.context = p;
   /* a decoded S3TC image is sampled from its B8G8R8A8 twin */
   if (ph_resource(texture)->decoded) {
      texture = ph_resource(texture)->decoded;
      view->base.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   }
   pipe_resource_reference(&view->base.texture, texture);
   return &view->base;
}

static void
ph_sampler_view_destroy(struct pipe_context *p, struct pipe_sampler_view *view)
{
   struct ph_sampler_view *v = (struct ph_sampler_view *)view;

   (void)p;
   pipe_sampler_view_reference(&v->shadow_view, NULL);
   pipe_sampler_view_reference(&v->compare_view, NULL);
   pipe_resource_reference(&view->texture, NULL);
   FREE(v);
}

static void ph_zinit_resolve(struct ph_context *ctx,
                             struct parhelia_resource *r);

static void ph_set_sampler_views(struct pipe_context *p, mesa_shader_stage sh,
                                 unsigned start, unsigned count,
                                 unsigned trailing,
                                 struct pipe_sampler_view **views)
{
   struct ph_context *ctx = ph_context(p);
   unsigned end = start + count + trailing;
   if (sh != MESA_SHADER_FRAGMENT || end > PIPE_MAX_SHADER_SAMPLER_VIEWS)
      return;
   for (unsigned i = 0; i < count; i++)
      if (ctx->bound_views[start + i] != views[i])
         goto changed;
   for (unsigned i = start + count; i < end; i++)
      if (ctx->bound_views[i])
         goto changed;
   return;
changed:
   ph_submit_vertices(ctx);
   /* Sampling reads memory, not the Z unit's tracked tiles. */
   for (unsigned i = 0; i < count; i++)
      if (views[i])
         ph_zinit_resolve(ctx, ph_resource(views[i]->texture));
   for (unsigned i = 0; i < count; i++) {
      pipe_sampler_view_reference(&ctx->bound_views[start + i], views[i]);
      pipe_sampler_view_reference(&ctx->sampler_views[start + i], views[i]);
   }
   for (unsigned i = start + count; i < end; i++) {
      pipe_sampler_view_reference(&ctx->bound_views[i], NULL);
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   }
   ctx->num_bound_views = MAX2(ctx->num_bound_views, start + count);
   while (ctx->num_bound_views &&
          !ctx->bound_views[ctx->num_bound_views - 1])
      ctx->num_bound_views--;
   if (ph_env("PARHELIA_TRACE_VIEWS"))
      fprintf(stderr,
              "parhelia: sampler views stage=%u start=%u count=%u trailing=%u active=%u view0=%p\n",
              sh, start, count, trailing, ctx->num_sampler_views,
              (void *)ctx->sampler_views[0]);
}
static void ph_set_shader_buffers(struct pipe_context *p, mesa_shader_stage sh,
                                  unsigned start, unsigned count,
                                  const struct pipe_shader_buffer *buffers,
                                  unsigned writable)
{ (void)p; (void)sh; (void)start; (void)count; (void)buffers; (void)writable; }
static void ph_set_shader_images(struct pipe_context *p, mesa_shader_stage sh,
                                 unsigned start, unsigned count,
                                 unsigned trailing,
                                 const struct pipe_image_view *images)
{ (void)p; (void)sh; (void)start; (void)count; (void)trailing; (void)images; }

static void *ph_create_vs(struct pipe_context *p,
                          const struct pipe_shader_state *s)
{
   struct ph_vs *vs = CALLOC_STRUCT(ph_vs);

   if (!vs)
      return NULL;
   vs->pos_input = vs->colour_input = -1;
   vs->texcoord_input[0] = vs->texcoord_input[1] = -1;
   if (s->type == PIPE_SHADER_IR_NIR) {
      struct ph_context *ctx = ph_context(p);

      nir_shader_gather_info(s->ir.nir,
                             nir_shader_get_entrypoint(s->ir.nir));
      /* Analyse before handing the shader over: the draw module rewrites it. */
      vs->ff_transform = ph_vs_analyze(s->ir.nir, vs, false);
      /* Not the fixed-function shape: the position still has to be the matrix
       * product the CPU bounds the clip volume with, but every other output is
       * compiled into a WARP program of this shader's own.  The transforming
       * stream may sample a texture, so the program must never write T2 -
       * ph_prime_warp() primes it at context creation instead. */
      bool analyzed = !vs->ff_transform && ctx->tcl_programs &&
         ph_vs_analyze(s->ir.nir, vs, true);

      /* Any other position: evaluate just its slice on the CPU for the
       * clip-volume bound, as the vendor ICD computes outcodes itself. */
      if (!analyzed && !vs->ff_transform && ctx->tcl_programs &&
          s->type == PIPE_SHADER_IR_NIR &&
          (vs->pos_slice = ph_slice_create(s->ir.nir))) {
         vs->pos_input = -1;
         nir_foreach_shader_in_variable(var, s->ir.nir)
            if (var->data.location == VERT_ATTRIB_POS)
               vs->pos_input = var->data.driver_location;
         if (vs->pos_input < 0 && vs->pos_slice->input_mask)
            vs->pos_input = ffs(vs->pos_slice->input_mask) - 1;
         analyzed = vs->pos_input >= 0;
      }
      const bool inputs_ok = analyzed && ph_vs_assign_inputs(s->ir.nir, vs);

      if (ph_env("PARHELIA_TRACE_TCL"))
         fprintf(stderr,
                 "parhelia: vs gates ff=%u programs=%u analyze=%u inputs=%u slots=%u instructions=%u\n",
                 vs->ff_transform, ctx->tcl_programs, analyzed, inputs_ok,
                 ctx->warp_program_slots, ctx->warp_program_instructions);

      if (inputs_ok)
         vs->hw_program = ph_vs_compile(s->ir.nir, &vs->prog_inputs, 0,
                                        ctx->warp_program_instructions, false,
                                        &vs->prog);
      if (ph_env("PARHELIA_TRACE_TCL") &&
          !strcmp(getenv("PARHELIA_TRACE_TCL"), "dump"))
         nir_print_shader(s->ir.nir, stderr);
      if (ph_env("PARHELIA_TRACE_TCL"))
         fprintf(stderr,
                 "parhelia: vs hardware-transform=%u compiled=%u instructions=%u mvp=%#x pos=%d col=%d st=%d/%d\n",
                 vs->ff_transform, vs->hw_program, vs->prog.count,
                 vs->mvp_offset, vs->pos_input,
                 vs->colour_input, vs->texcoord_input[0],
                 vs->texcoord_input[1]);
   }
   vs->draw = draw_create_vertex_shader(ph_context(p)->draw, s);
   if (!vs->draw) {
      FREE(vs);
      return NULL;
   }
   return vs;
}
static void ph_bind_vs(struct pipe_context *p, void *s)
{
   struct ph_context *ctx = ph_context(p);
   struct ph_vs *vs = s;

   if (ctx->triangles && ctx->transform &&
       (!vs || (!vs->ff_transform && !vs->hw_program) || vs != ctx->vs))
      ph_submit_vertices(ctx);
   ctx->vs = vs;
   draw_bind_vertex_shader(ctx->draw, vs ? vs->draw : NULL);
}
static void ph_delete_vs(struct pipe_context *p, void *s)
{
   struct ph_context *ctx = ph_context(p);
   struct ph_vs *vs = s;

   ph_slice_destroy(vs->pos_slice);

   if (vs->prog_slot) {
      struct drm_parhelia_warp_program release = {
         .slot = vs->prog_slot, .flags = PARHELIA_WARP_PROGRAM_FREE,
      };

      /* A queued draw may still name this entry, and the slot becomes
       * available to another shader, so publish and drain first. */
      if (ctx->triangles && ctx->warp_program_entry == vs->prog_entry)
         ph_submit_vertices(ctx);
      ph_drain_batch(ctx);
      ioctl(ph_screen(ctx->base.screen)->fd,
            DRM_IOCTL_PARHELIA_WARP_PROGRAM, &release);
      if (ctx->warp_programs_held)
         ctx->warp_programs_held--;
   }
   if (ctx->vs == vs)
      ctx->vs = NULL;
   draw_delete_vertex_shader(ctx->draw, vs->draw);
   FREE(vs);
}

static bool
ph_nir_uses_texture(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex)
               return true;
         }
      }
   }
   return false;
}

static uint32_t
ph_nir_projective_mask(nir_shader *nir)
{
   uint32_t mask = 0;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);

               if (tex->sampler_index < 32 &&
                   nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0)
                  mask |= BITFIELD_BIT(tex->sampler_index);
            }
         }
      }
   }
   return mask;
}

static uint32_t
ph_nir_sampler_mask(nir_shader *nir)
{
   uint32_t mask = 0;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               if (tex->sampler_index < 32)
                  mask |= BITFIELD_BIT(tex->sampler_index);
            }
         }
      }
   }
   return mask;
}

/* A program that samples GL units with a gap below them (unit 1 alone, the
 * fixed-function texture on a second unit) is compiled with its samplers
 * packed onto units 0.., so the draw binds the units the program reads.
 * Returns the shader to compile: @nir itself, or a clone the caller frees. */
static nir_shader *
ph_fs_pack_units(struct ph_fs *fs, nir_shader *nir)
{
   const uint32_t used = ph_nir_sampler_mask(nir);
   uint8_t packed[32];
   unsigned n = 0;
   nir_shader *clone;

   if (!used || !(used & (used + 1)) ||
       util_bitcount(used) > ARRAY_SIZE(fs->unit_source))
      return nir;
   u_foreach_bit(k, used) {
      packed[k] = n;
      fs->unit_source[n++] = k;
   }
   clone = nir_shader_clone(NULL, nir);
   if (!clone)
      return nir;
   nir_foreach_function_impl(impl, clone) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);

               tex->sampler_index = packed[tex->sampler_index];
               tex->texture_index = tex->sampler_index;
            }
         }
      }
   }
   fs->unit_remap = true;
   fs->sampler_mask = BITFIELD_MASK(n);
   fs->projective_mask = ph_nir_projective_mask(clone);
   return clone;
}

/* The fixed-function words the special draw routes emit, from the algebra
 * the compiled program computes. */
static void
ph_fs_route_class(struct ph_fs *fs, bool single_texture_pipe)
{
   switch (fs->program.cls) {
   case PH_FS_CLASS_REPLACE:   fs->texenv = P3D_TEXENV_REPLACE; break;
   case PH_FS_CLASS_MODULATE:  fs->texenv = P3D_TEXENV_MODULATE; break;
   case PH_FS_CLASS_DECAL:     fs->texenv = P3D_TEXENV_DECAL; break;
   case PH_FS_CLASS_ADD:       fs->texenv = P3D_TEXENV_ADD; break;
   case PH_FS_CLASS_MODULATE2: fs->serial_op = PH_SERIAL_MODULATE; break;
   /* The serial MODULATE's first pass takes the texenv, REPLACE without a
    * vertex colour, so it leaves t0 for the DST_COLOR pass to scale. */
   case PH_FS_CLASS_MODULATE2_TEX:
      if (!single_texture_pipe)
         break;
      fs->texenv = P3D_TEXENV_REPLACE;
      fs->serial_op = PH_SERIAL_MODULATE;
      break;
   case PH_FS_CLASS_ADD2:      fs->serial_op = PH_SERIAL_ADD; break;
   case PH_FS_CLASS_REPLACE1:  fs->serial_op = PH_SERIAL_REPLACE1; break;
   default: break;
   }
}

/* Combiner slots the texture stages leave a program: the two merges of the
 * paired route, or the original family's three- and four-unit forms - one
 * merge for [3,4,8], none for four units (p3d_orig_multi_mask). */
static unsigned
ph_comb_budget(const struct parhelia_screen *s, unsigned units)
{
   if (units > 2 && ph_original_family(s->chip_id))
      return P3D_COMB_QUANTUM - (units == 3);
   return P3D_COMB_QUANTUM - (units > 0) - (units > 1);
}

/* Use the compiled combiner program when it matches this draw.  The special
 * alpha routes own their own combiner words, and the serial multitexture
 * passes rewrite the environment per pass, so they keep the fixed-function
 * emission.  PARHELIA_FS_TEXENV=1 forces that emission everywhere, for A/B
 * runs. */
static const struct p3d_comb_program *
ph_compiled_combiner(struct ph_context *ctx, bool two_units)
{
   struct ph_fs *fs = ctx->fs;
   struct ph_fs_program *kill_prog = NULL;
   enum pipe_compare_func kill_func;
   uint8_t kill_ref;

   if (fs && (ph_fs_kill_route(ctx, &kill_prog, &kill_func, &kill_ref) ==
              PH_KILL_ALPHA_X ||
              ph_fs_kill_route(ctx, &kill_prog, &kill_func, &kill_ref) ==
              PH_KILL_ALPHA_SUB)) {
      /* The discard's alpha form; its constants are already current. */
      if (!two_units && (kill_prog->sampler_mask & BITFIELD_BIT(1)))
         return NULL;
      return kill_prog->valid ? &kill_prog->comb : NULL;
   }
   if (!fs || !fs->program.valid || ph_env("PARHELIA_FS_TEXENV"))
      return NULL;
   /* The draw binds a second texture unit only for the shapes the
    * instruction-shape matcher recognises.  A program that samples unit 1
    * when the draw has not bound it reads zero and renders black, so leave
    * those to the matcher until the draw path binds by sampler use. */
   if (!two_units && (fs->program.sampler_mask & BITFIELD_BIT(1)))
      return NULL;
   if (ctx->native_sampled_alpha || ctx->vertex_alpha || ctx->alpha_micro ||
       ctx->alpha_micro_cutout || ctx->cpu_alpha_cutout ||
       ctx->rgb_alpha_two_pass)
      return NULL;
   /* The domains the staged carriers of this batch interpolate. */
   for (unsigned k = 0; k < PH_FS_CARRIERS; k++)
      if (fs->program.carriers & (1u << k))
         ph_fs_set_domain(&fs->program, k, ctx->dom_lo[k], ctx->dom_span[k]);
   if (fs->point_deriv)
      ph_fs_set_point_derivatives(&fs->program, ctx->pntc_deriv);
   if (fs->program.const_ref_count || fs->program.carriers) {
      const uint8_t *live = NULL;

      if (ctx->user_constants[MESA_SHADER_FRAGMENT])
         live = ctx->user_constants[MESA_SHADER_FRAGMENT];
      else if (ctx->constants[MESA_SHADER_FRAGMENT])
         live = ph_resource(ctx->constants[MESA_SHADER_FRAGMENT])->malloc_data;
      if (live)
         live += ctx->constant_offset[MESA_SHADER_FRAGMENT];
      /* PARHELIA_TRACE_FS_CONST=1 (diagnostic): name a draw whose snapshot
       * differs from the live constants at submit. */
      if (ph_env("PARHELIA_TRACE_FS_CONST") &&
          (!live || ctx->fs_const_snap_size !=
                       ctx->constant_size[MESA_SHADER_FRAGMENT] ||
           memcmp(ctx->fs_const_snap, live, ctx->fs_const_snap_size)))
         fprintf(stderr, "parhelia: fs=%p constants snapshot %u bytes, live %p %u bytes, differ\n",
                 (void *)fs, ctx->fs_const_snap_size, (void *)live,
                 ctx->constant_size[MESA_SHADER_FRAGMENT]);
      if (ph_env("PARHELIA_FS_LIVE_CONSTANTS")) {
         if (!live)
            return NULL;
         ph_fs_update_constants(&fs->program, live,
                                ctx->constant_size[MESA_SHADER_FRAGMENT]);
         return &fs->program.comb;
      }
      if (!ctx->fs_const_snap_size && fs->program.const_ref_count &&
          ph_fs_reads_user_constants(&fs->program))
         return NULL;
      if (!ph_fs_update_constants(&fs->program, ctx->fs_const_snap,
                                  ctx->fs_const_snap_size)) {
         /* No variant holds this batch's constants or domains: the batch
          * is dropped, never drawn with another program. */
         ctx->comb_refused = true;
         return NULL;
      }
   }
   /* A program that only moves a constant (glColor's current colour) has
    * that constant as its colour: put it on the logic op's target grid. */
   if (ph_logicop_quantize(ctx) && !fs->program.sampler_mask &&
       !fs->program.uses_color && !fs->program.uses_color1 &&
       fs->program.comb.count == 1) {
      struct p3d_comb_inst *inst = &fs->program.comb.inst[0];
      float scale[4];

      ph_logicop_grid(ctx, scale);
      for (unsigned k = 0; k < 2; k++)
         for (unsigned c = 0; k < 2 && inst->has_const[k] && c < 4; c++)
            if (scale[c] > 0.0f)
               inst->constant[k][c] =
                  roundf(CLAMP(inst->constant[k][c], 0.0f, 1.0f) * scale[c]) /
                  scale[c];
   }
   return &fs->program.comb;
}

/* ------------------------------------------------ discard as alpha test
 *
 * The combiner has no kill, but its alpha half can carry any channel of a
 * sample (replicate .r drives the alpha test on the P690, docs/parhelia-
 * p690-combiner-isa-re.md section 3.1), and the alpha test runs per fragment.
 * A shader whose only discard is "discard if x <cmp> c", with x a scalar the
 * combiner computes and c a constant on the 8-bit alpha grid, is rewritten
 * into one of two combiner programs:
 *
 *  - alpha := x, with the alpha test keeping exactly the fragments the
 *    discard keeps.  Exact whenever the written alpha cannot be observed
 *    (no alpha channel or alpha masked, no source-alpha blending, no GL
 *    alpha test) - or when the colour alpha is a draw constant 0 and the
 *    discard keeps only x == 0, since the kept fragments then write 0.
 *  - alpha := sat(a - x) for "discard if x != 0" with a draw-constant
 *    colour alpha a > 0 on the 8-bit grid, tested GEQUAL a: a kept fragment
 *    writes exactly a, a discarded one (x >= 1/255) falls below it.  This is
 *    glBitmap's shader (nir_lower_bitmap) with its constant raster colour.
 */
struct ph_fs_kill {
   enum pipe_compare_func keep;   /* x <keep> ref keeps the fragment */
   uint8_t ref;
   bool ne_zero;                  /* the discard is "x != 0" */
   bool const_alpha;              /* the colour alpha is a draw constant */
   bool have_x, have_sub, have_colour;
   struct ph_fs_program alpha_x, alpha_sub, colour;
};

/* @def only depends on constants and uniforms. */
static bool
ph_nir_def_uniform(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 16)
      return false;
   switch (instr->type) {
   case nir_instr_type_load_const:
      return true;
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

      return in->intrinsic == nir_intrinsic_load_ubo ||
             in->intrinsic == nir_intrinsic_load_ubo_vec4 ||
             in->intrinsic == nir_intrinsic_load_uniform;
   }
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         if (!ph_nir_def_uniform(alu->src[i].src.ssa, depth + 1))
            return false;
      return true;
   }
   default:
      return false;
   }
}

/* The terminate_ifs and the colour store of a straight-line shader; *@kill
 * is the first, *@kills counts them (several only with @many). */
static bool
ph_nir_find_kills(nir_shader *nir, bool many, nir_intrinsic_instr **kill,
                  unsigned *kills, nir_intrinsic_instr **store,
                  unsigned *value_src)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   *kill = *store = NULL;
   *kills = 0;
   if (!impl || exec_list_length(&impl->body) != 1)
      return false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *in;
         int location = -1;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         in = nir_instr_as_intrinsic(instr);
         switch (in->intrinsic) {
         case nir_intrinsic_terminate_if:
         case nir_intrinsic_demote_if:
            if (*kill && !many)
               return false;
            if (!*kill)
               *kill = in;
            (*kills)++;
            break;
         case nir_intrinsic_terminate:
         case nir_intrinsic_demote:
            return false;
         case nir_intrinsic_store_deref: {
            nir_deref_instr *deref = nir_src_as_deref(in->src[0]);

            if (deref && deref->deref_type == nir_deref_type_var &&
                (deref->var->data.mode & nir_var_shader_out))
               location = deref->var->data.location;
            *value_src = 1;
            break;
         }
         case nir_intrinsic_store_output:
            location = nir_intrinsic_io_semantics(in).location;
            *value_src = 0;
            break;
         default:
            break;
         }
         if (location < 0)
            continue;
         if ((location != FRAG_RESULT_COLOR && location != FRAG_RESULT_DATA0) ||
             *store || nir_intrinsic_write_mask(in) != 0xf ||
             (in->intrinsic == nir_intrinsic_store_output &&
              nir_intrinsic_component(in)) ||
             in->src[*value_src].ssa->num_components != 4)
            return false;
         *store = in;
      }
   }
   return *kill && *store;
}

static bool
ph_nir_find_kill(nir_shader *nir, nir_intrinsic_instr **kill,
                 nir_intrinsic_instr **store, unsigned *value_src)
{
   unsigned kills;

   return ph_nir_find_kills(nir, false, kill, &kills, store, value_src);
}

/* Compile @src with every discard folded into a 0/1 alpha: 1 where no
 * condition holds, so an alpha test GEQUAL 0.5 keeps exactly the fragments
 * the shader keeps (ARB KIL of a vector, several KILs). */
static bool
ph_fs_compile_kill_bool(const nir_shader *src, unsigned budget,
                        struct ph_fs_program *out)
{
   nir_shader *nir = nir_shader_clone(NULL, src);
   nir_intrinsic_instr *kill, *store, *alpha_store;
   unsigned kills, value_src = 0;
   nir_def *cond = NULL;
   bool ok = false;

   if (!nir || !ph_nir_find_kills(nir, true, &kill, &kills, &store, &value_src))
      goto done;
   nir_builder b = nir_builder_at(nir_before_instr(&store->instr));

   nir_foreach_block(block, nir_shader_get_entrypoint(nir)) {
      bool after_store = false;

      nir_foreach_instr_safe(instr, block) {
         nir_intrinsic_instr *in;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         in = nir_instr_as_intrinsic(instr);
         after_store |= in == store;
         if (in->intrinsic != nir_intrinsic_terminate_if &&
             in->intrinsic != nir_intrinsic_demote_if)
            continue;
         /* the alpha is computed at the store from every condition */
         if (after_store)
            goto done;
         cond = cond ? nir_ior(&b, cond, in->src[0].ssa) : in->src[0].ssa;
         nir_instr_remove(instr);
      }
   }
   alpha_store = nir_instr_as_intrinsic(nir_instr_clone(nir, &store->instr));
   nir_builder_instr_insert(&b, &alpha_store->instr);
   nir_src_rewrite(&alpha_store->src[value_src],
                   nir_replicate(&b, nir_b2f32(&b, nir_inot(&b, cond)), 4));
   nir_intrinsic_set_write_mask(alpha_store, 0x8);
   nir_intrinsic_set_write_mask(store, 0x7);
   NIR_PASS(_, nir, nir_opt_algebraic);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);
   ok = ph_fs_compile(nir, budget, out);
   /* The draw stages varyings for the shader's own program; a variant
    * that routes generics would read interpolators nothing staged. */
   if (ok && (out->route[0] >= 0 || out->route[1] >= 0)) {
      ph_fs_program_fini(out);
      memset(out, 0, sizeof(*out));
      out->reject = "a discard variant that routes varyings";
      ok = false;
   }
done:
   ralloc_free(nir);
   return ok;
}

enum ph_kill_variant { PH_KILL_V_X, PH_KILL_V_SUB, PH_KILL_V_COLOUR };

/* Compile @nir with its discard rewritten as @variant. */
static bool
ph_fs_compile_kill_variant(const nir_shader *src, enum ph_kill_variant variant,
                           unsigned budget, struct ph_fs_program *out)
{
   nir_shader *nir = nir_shader_clone(NULL, src);
   nir_intrinsic_instr *kill, *store;
   unsigned value_src = 0;
   bool ok = false;

   if (!nir || !ph_nir_find_kill(nir, &kill, &store, &value_src))
      goto done;
   if (variant != PH_KILL_V_COLOUR) {
      nir_alu_instr *cmp = nir_def_as_alu_or_null(kill->src[0].ssa);
      const unsigned xi = cmp && nir_src_is_const(cmp->src[0].src) ? 1 : 0;
      nir_builder b = nir_builder_at(nir_before_instr(&store->instr));
      nir_def *v = store->src[value_src].ssa, *x, *alpha;
      nir_intrinsic_instr *alpha_store;

      if (!cmp)
         goto done;
      x = nir_mov_alu(&b, cmp->src[xi], 1);
      alpha = variant == PH_KILL_V_X ? x :
              nir_fsat(&b, nir_fsub(&b, nir_channel(&b, v, 3), x));
      /* RGB keeps its one source; the alpha gets a store of its own. */
      alpha_store = nir_instr_as_intrinsic(nir_instr_clone(nir, &store->instr));
      nir_builder_instr_insert(&b, &alpha_store->instr);
      nir_src_rewrite(&alpha_store->src[value_src], nir_replicate(&b, alpha, 4));
      nir_intrinsic_set_write_mask(alpha_store, 0x8);
      nir_intrinsic_set_write_mask(store, 0x7);
   }
   nir_instr_remove(&kill->instr);
   NIR_PASS(_, nir, nir_opt_dce);
   ok = ph_fs_compile(nir, budget, out);
   /* The draw stages varyings for the shader's own program; a variant
    * that routes generics would read interpolators nothing staged. */
   if (ok && (out->route[0] >= 0 || out->route[1] >= 0)) {
      ph_fs_program_fini(out);
      memset(out, 0, sizeof(*out));
      out->reject = "a discard variant that routes varyings";
      ok = false;
   }
done:
   ralloc_free(nir);
   return ok;
}

/* A discard of any other form, as the 0/1 keep alpha. */
static struct ph_fs_kill *
ph_fs_build_kill_bool(nir_shader *src, unsigned budget)
{
   struct ph_fs_kill *k = CALLOC_STRUCT(ph_fs_kill);

   if (!k)
      return NULL;
   k->keep = PIPE_FUNC_GEQUAL;
   k->ref = 128;
   k->have_x = ph_fs_compile_kill_bool(src, budget, &k->alpha_x);
   if (!k->have_x) {
      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr, "parhelia: discard has no keep-alpha form: %s\n",
                 k->alpha_x.reject ? k->alpha_x.reject : "not a straight-line kill");
      FREE(k);
      return NULL;
   }
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr, "parhelia: discard as a 0/1 keep alpha, alpha test GEQUAL 128\n");
   return k;
}

static struct ph_fs_kill *
ph_fs_build_kill(nir_shader *src, unsigned budget)
{
   nir_intrinsic_instr *kill, *store;
   unsigned value_src = 0;
   struct ph_fs_kill *k;
   nir_alu_instr *cmp;
   float c;
   unsigned xi;

   if (!ph_nir_find_kill(src, &kill, &store, &value_src))
      return ph_fs_build_kill_bool(src, budget);
   cmp = nir_def_as_alu_or_null(kill->src[0].ssa);
   if (!cmp || nir_op_infos[cmp->op].num_inputs != 2 ||
       cmp->src[0].src.ssa->bit_size != 32 ||
       nir_src_is_const(cmp->src[0].src) == nir_src_is_const(cmp->src[1].src))
      return ph_fs_build_kill_bool(src, budget);
   xi = nir_src_is_const(cmp->src[0].src) ? 1 : 0;
   c = nir_src_comp_as_float(cmp->src[xi ^ 1].src, cmp->src[xi ^ 1].swizzle[0]);
   if (c < 0.0f || c > 1.0f || c * 255.0f != floorf(c * 255.0f))
      return ph_fs_build_kill_bool(src, budget);
   k = CALLOC_STRUCT(ph_fs_kill);
   if (!k)
      return NULL;
   k->ref = (uint8_t)(c * 255.0f);
   /* Keep is the negated discard, with x on the left. */
   switch (cmp->op) {
   case nir_op_fneu: k->keep = PIPE_FUNC_EQUAL; break;
   case nir_op_feq: k->keep = PIPE_FUNC_NOTEQUAL; break;
   case nir_op_flt: k->keep = xi ? PIPE_FUNC_LEQUAL : PIPE_FUNC_GEQUAL; break;
   case nir_op_fge: k->keep = xi ? PIPE_FUNC_GREATER : PIPE_FUNC_LESS; break;
   default:
      FREE(k);
      return ph_fs_build_kill_bool(src, budget);
   }
   k->ne_zero = cmp->op == nir_op_fneu && !k->ref;
   k->const_alpha =
      ph_nir_def_uniform(store->src[value_src].ssa, 0);
   if (!k->const_alpha) {
      /* A vec built from per-channel sources: only its alpha matters. */
      nir_alu_instr *vec = nir_def_as_alu_or_null(store->src[value_src].ssa);

      k->const_alpha = vec && vec->op == nir_op_vec4 &&
                       ph_nir_def_uniform(vec->src[3].src.ssa, 0);
   }
   k->have_x = ph_fs_compile_kill_variant(src, PH_KILL_V_X, budget, &k->alpha_x);
   if (k->ne_zero && k->const_alpha) {
      k->have_sub = ph_fs_compile_kill_variant(src, PH_KILL_V_SUB, budget,
                                               &k->alpha_sub);
      k->have_colour = ph_fs_compile_kill_variant(src, PH_KILL_V_COLOUR,
                                                  budget, &k->colour);
   }
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr, "parhelia: discard as alpha test: keep func %u ref %u const alpha %u, alpha:=x %u, alpha:=a-x %u\n",
              k->keep, k->ref, k->const_alpha, k->have_x, k->have_sub);
   return k;
}

/* The program's constants from the current fragment constant buffer. */
static bool
ph_fs_program_constants(struct ph_context *ctx, struct ph_fs_program *prog)
{
   if (!prog->const_ref_count)
      return true;
   if (!ctx->fs_const_snap_size)
      return false;
   return ph_fs_update_constants(prog, ctx->fs_const_snap,
                                 ctx->fs_const_snap_size);
}

static bool
ph_alpha_observable(const struct ph_context *ctx)
{
   const struct pipe_rt_blend_state *rt = ctx->blend ? &ctx->blend->rt[0] : NULL;
   const struct util_format_description *desc = ctx->fb.nr_cbufs ?
      util_format_description(ctx->fb.cbufs[0].format) : NULL;
   const enum pipe_blendfactor f[4] = {
      rt ? rt->rgb_src_factor : PIPE_BLENDFACTOR_ONE,
      rt ? rt->rgb_dst_factor : PIPE_BLENDFACTOR_ZERO,
      rt ? rt->alpha_src_factor : PIPE_BLENDFACTOR_ONE,
      rt ? rt->alpha_dst_factor : PIPE_BLENDFACTOR_ZERO,
   };

   if (rt && rt->blend_enable)
      for (unsigned i = 0; i < 4; i++)
         if (f[i] == PIPE_BLENDFACTOR_SRC_ALPHA ||
             f[i] == PIPE_BLENDFACTOR_INV_SRC_ALPHA ||
             f[i] == PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE)
            return true;
   return desc && util_format_has_alpha(ctx->fb.cbufs[0].format) &&
          (!rt || (rt->colormask & PIPE_MASK_A));
}

static bool
ph_alpha_func_passes(enum pipe_compare_func func, uint8_t a, uint8_t ref)
{
   switch (func) {
   case PIPE_FUNC_NEVER: return false;
   case PIPE_FUNC_LESS: return a < ref;
   case PIPE_FUNC_EQUAL: return a == ref;
   case PIPE_FUNC_LEQUAL: return a <= ref;
   case PIPE_FUNC_GREATER: return a > ref;
   case PIPE_FUNC_NOTEQUAL: return a != ref;
   case PIPE_FUNC_GEQUAL: return a >= ref;
   default: return true;
   }
}

/* How the bound fragment shader's discard reaches the hardware for this
 * draw: the program to run and the alpha test that replaces the discard. */
static enum ph_kill_route
ph_fs_kill_route(struct ph_context *ctx, struct ph_fs_program **prog,
                 enum pipe_compare_func *func, uint8_t *ref)
{
   struct ph_fs_kill *k = ctx->fs ? ctx->fs->kill : NULL;
   const bool gl_test = ctx->dsa && ctx->dsa->alpha_enabled;
   float a[4];
   uint8_t a8;

   if (!ctx->kill_alpha || !ctx->fs || !ctx->fs->draw ||
       ctx->fs->draw->base.type != PIPE_SHADER_IR_NIR ||
       !ctx->fs->draw->base.ir.nir->info.fs.uses_discard)
      return PH_KILL_NONE;
   if (!k)
      return PH_KILL_UNSUPPORTED;
   if (k->have_x && !gl_test && !ph_alpha_observable(ctx)) {
      *prog = &k->alpha_x;
      *func = k->keep;
      *ref = k->ref;
      return ph_fs_program_constants(ctx, *prog) ? PH_KILL_ALPHA_X :
                                                   PH_KILL_UNSUPPORTED;
   }
   if (!k->ne_zero || !k->const_alpha || !k->have_colour ||
       !ph_fs_program_constants(ctx, &k->colour))
      return PH_KILL_UNSUPPORTED;
   {
      static const float zero[4][4];

      ph_fs_eval(&k->colour.comb, zero, (const float (*)[4])zero, 1.0f, a);
   }
   /* The evaluation models the Q1.10 constant registers, so 1.0 reads as
    * 1023/1024: the code the target stores is the rounded one. */
   a8 = (uint8_t)CLAMP(lrintf(a[3] * 255.0f), 0, 255);
   if (gl_test &&
       !ph_alpha_func_passes(ctx->dsa->alpha_func, a8,
                             CLAMP((int)lrintf(ctx->dsa->alpha_ref_value * 255.0f),
                                   0, 255)))
      return PH_KILL_ALL;
   if (!a8 && k->have_x) {
      *prog = &k->alpha_x;
      *func = PIPE_FUNC_EQUAL;
      *ref = 0;
      return ph_fs_program_constants(ctx, *prog) ? PH_KILL_ALPHA_X :
                                                   PH_KILL_UNSUPPORTED;
   }
   if (!k->have_sub || !ph_fs_program_constants(ctx, &k->alpha_sub))
      return PH_KILL_UNSUPPORTED;
   *prog = &k->alpha_sub;
   *func = PIPE_FUNC_GEQUAL;
   *ref = a8;
   return PH_KILL_ALPHA_SUB;
}

/* The bound fragment program samples nothing, counting a discard's form. */
static bool
ph_fs_untextured_program(struct ph_context *ctx)
{
   struct ph_fs_program *kill_prog = NULL;
   enum pipe_compare_func kill_func;
   uint8_t kill_ref;
   const enum ph_kill_route route = ctx->fs ?
      ph_fs_kill_route(ctx, &kill_prog, &kill_func, &kill_ref) : PH_KILL_NONE;

   if (route == PH_KILL_ALPHA_X || route == PH_KILL_ALPHA_SUB)
      return kill_prog->valid && !kill_prog->sampler_mask;
   return ctx->fs && ctx->fs->program.valid && !ctx->fs->program.sampler_mask;
}

/* The value a straight-line shader stores to FRAG_RESULT_DEPTH, or NULL. */
static nir_intrinsic_instr *
ph_nir_depth_store(nir_shader *nir, nir_def **value)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_store_deref) {
               nir_deref_instr *d = nir_src_as_deref(in->src[0]);

               if (d && d->deref_type == nir_deref_type_var &&
                   d->var->data.location == FRAG_RESULT_DEPTH) {
                  *value = in->src[1].ssa;
                  return in;
               }
            } else if (in->intrinsic == nir_intrinsic_store_output &&
                       nir_intrinsic_io_semantics(in).location ==
                       FRAG_RESULT_DEPTH) {
               *value = in->src[0].ssa;
               return in;
            }
         }
      }
   }
   return NULL;
}

/* gl_FragDepth built from uniforms only: compile the colour without the
 * depth store, and keep the depth for per-draw evaluation. */
static void
ph_fs_compile_uniform_depth(struct ph_fs *fs, nir_shader *orig,
                            nir_shader *compile_src, unsigned budget)
{
   nir_def *value = NULL;
   nir_intrinsic_instr *store = ph_nir_depth_store(orig, &value);
   nir_shader *nir;
   struct ph_fs_program prog;

   if (!store || !value || !ph_nir_def_uniform(value, 0) ||
       nir_shader_get_entrypoint(orig) == NULL)
      return;
   nir = nir_shader_clone(NULL, compile_src);
   if (!nir)
      return;
   store = ph_nir_depth_store(nir, &(nir_def *){ NULL });
   if (store) {
      nir_instr_remove(&store->instr);
      NIR_PASS(_, nir, nir_opt_dce);
      if (ph_fs_compile(nir, budget, &prog) &&
          !(prog.sampler_mask & ~fs->sampler_mask)) {
         ph_fs_program_fini(&fs->program);
         fs->program = prog;
         fs->depth_uniform = value;
      } else if (prog.valid) {
         ph_fs_program_fini(&prog);
      }
   }
   ralloc_free(nir);
}

static void *ph_create_fs(struct pipe_context *p,
                          const struct pipe_shader_state *s)
{
   struct ph_fs *fs = CALLOC_STRUCT(ph_fs);
   if (fs) {
      if (s->type == PIPE_SHADER_IR_NIR)
         nir_shader_gather_info(s->ir.nir,
                                nir_shader_get_entrypoint(s->ir.nir));
      fs->draw = draw_create_fragment_shader(ph_context(p)->draw, s);
      if (fs->draw) {
         const struct tgsi_shader_info *info = &fs->draw->info;
         /* nir_tgsi_scan_shader relies on optional NIR usage bitsets which
          * are not populated for all internal Mesa/glamor shaders.  Inspect
          * the actual IR as the authoritative source for texture use. */
         /* A declared sampler no instruction reads (a texenv source
          * algebra folded away) leaves the draw untextured; binding it
          * would need a coordinate set the vertex program does not write,
          * and the draw was dropped. */
         fs->sampler_mask = info->samplers_declared;
         if (s->type == PIPE_SHADER_IR_NIR) {
            fs->sampler_mask = ph_nir_sampler_mask(s->ir.nir);
            fs->projective_mask = ph_nir_projective_mask(s->ir.nir);
            /* measured on the 0528 only; the P690 keeps dropping these */
            if (ph_original_family(ph_screen(p->screen)->chip_id))
               fs->compare_mask = ph_nir_compare_mask(s->ir.nir);
         }
         fs->uses_texture = fs->sampler_mask != 0 ||
            (s->type == PIPE_SHADER_IR_NIR && ph_nir_uses_texture(s->ir.nir));
         /* draw's TGSI scan does not retain fixed-function varyings for the
          * NIR shader path, so info->num_inputs is zero for Quake's
          * texture*vertex-colour shaders.  NIR's gathered varying mask is
          * authoritative and selects the hardware MODULATE WARP program. */
         if (s->type == PIPE_SHADER_IR_NIR)
            fs->uses_color = !!(s->ir.nir->info.inputs_read &
                                VARYING_BIT_COL0);
         fs->coord_semantic = TGSI_SEMANTIC_GENERIC;
         fs->coord_index = 0;
         for (unsigned i = 0; i < info->num_inputs; i++) {
            if (info->input_semantic_name[i] == TGSI_SEMANTIC_COLOR)
               fs->uses_color = true;
            if (info->input_semantic_name[i] == TGSI_SEMANTIC_TEXCOORD ||
                info->input_semantic_name[i] == TGSI_SEMANTIC_GENERIC) {
               fs->coord_semantic = info->input_semantic_name[i];
               fs->coord_index = info->input_semantic_index[i];
            }
         }
         fs->texenv = fs->uses_color ? P3D_TEXENV_MODULATE :
                                      P3D_TEXENV_REPLACE;
         fs->serial_op = PH_SERIAL_UNSUPPORTED;
         if (s->type == PIPE_SHADER_IR_NIR) {
            /* Slot 0 merges the ganged texture pipes and, with two units,
             * slot 1 merges the second; the rest of the five-instruction
             * quantum is the program's.  A program may sample fewer units
             * than the shader declares, never more. */
            unsigned units = util_bitcount(fs->sampler_mask);
            unsigned budget = ph_comb_budget(ph_screen(p->screen), units);

            nir_shader *compile_nir = ph_fs_pack_units(fs, s->ir.nir);

            /* Without native integers the uniforms hold floats, so the
             * integer arithmetic GLSL produced becomes float arithmetic. */
            {
               /* The combiner has no control flow: flatten every if, as
                * non-CF hardware does, textures and discards included. */
               static const nir_opt_peephole_select_options flatten = {
                  .limit = ~0u, .indirect_load_ok = true,
                  .expensive_alu_ok = true, .discard_ok = true,
               };
               bool progress;

               if (compile_nir == s->ir.nir)
                  compile_nir = nir_shader_clone(NULL, s->ir.nir);
               if (compile_nir->options->no_integers)
                  NIR_PASS(_, compile_nir, nir_lower_int_to_float);
               do {
                  progress = false;
                  NIR_PASS(progress, compile_nir, nir_opt_peephole_select,
                           &flatten);
                  NIR_PASS(progress, compile_nir, nir_opt_dead_cf);
                  NIR_PASS(progress, compile_nir, nir_opt_copy_prop);
                  NIR_PASS(progress, compile_nir, nir_opt_algebraic);
                  NIR_PASS(progress, compile_nir, nir_opt_constant_folding);
                  NIR_PASS(progress, compile_nir, nir_opt_cse);
                  NIR_PASS(progress, compile_nir, nir_opt_dce);
               } while (progress);
               if (ph_fs_lower_point_derivatives(compile_nir)) {
                  fs->point_deriv = true;
                  NIR_PASS(_, compile_nir, nir_opt_algebraic);
                  NIR_PASS(_, compile_nir, nir_opt_dce);
               }
            }

            /* A texture sampled at several coordinates spreads its taps
             * over free units. */
            ph_fs_split_taps(fs, s->ir.nir, compile_nir,
                             ph_nir_sampler_mask(compile_nir),
                             MIN2(ph_screen(p->screen)->texture_units,
                                  PH_TEXTURE_UNITS_DRIVEN));
            for (unsigned t = 0; t < fs->tap_count; t++)
               fs->sampler_mask |= 1u << fs->taps[t].unit;
            if (fs->tap_count) {
               units = util_bitcount(fs->sampler_mask);
               budget = ph_comb_budget(ph_screen(p->screen), units);
            }
            /* floor() of a varying samples a ramp texture on a free unit. */
            const uint32_t unramped_mask = fs->sampler_mask;
            struct ph_fs_ramp ramps[P3D_MAX_TEXTURE_UNITS];
            const char *ramp_reject = NULL;
            unsigned ramp_count = 0, ramp_mask = 0;
            bool ramped = ph_fs_lower_floor_ramps(compile_nir,
               ph_nir_sampler_mask(compile_nir),
               MIN2(ph_screen(p->screen)->texture_units, PH_TEXTURE_UNITS_DRIVEN),
               ramps, &ramp_count, &ramp_reject);

            const unsigned floor_ramps = ramp_count;

            /* Generic varyings beyond the colour interpolators ride on
             * coordinate units. */
            if (ramped) {
               for (unsigned r = 0; r < ramp_count; r++)
                  ramp_mask |= 1u << ramps[r].unit;
               ramped = ph_fs_assign_pass_units(compile_nir,
                  ph_nir_sampler_mask(compile_nir) | ramp_mask,
                  MIN2(ph_screen(p->screen)->texture_units, PH_TEXTURE_UNITS_DRIVEN),
                  ramps, &ramp_count, &ramp_reject);
            }
            ramp_mask = 0;
            for (unsigned r = 0; r < ramp_count; r++)
               ramp_mask |= 1u << ramps[r].unit;
            if (ramped && ramp_count && fs->unit_remap) {
               ramped = false;
               ramp_reject = "a floor ramp next to remapped texture units";
            }
            if (ramped && ramp_count) {
               bool progress = floor_ramps != 0;

               while (progress) {
                  progress = false;
                  NIR_PASS(progress, compile_nir, nir_opt_copy_prop);
                  NIR_PASS(progress, compile_nir, nir_opt_algebraic);
                  NIR_PASS(progress, compile_nir, nir_opt_constant_folding);
                  NIR_PASS(progress, compile_nir, nir_opt_cse);
                  NIR_PASS(progress, compile_nir, nir_opt_dce);
               }
               units = util_bitcount(fs->sampler_mask | ramp_mask);
               budget = ph_comb_budget(ph_screen(p->screen), units);
               fs->sampler_mask |= ramp_mask;
               fs->uses_texture = true;
            }
            if (!ramped) {
               memset(&fs->program, 0, sizeof(fs->program));
               fs->program.reject = ramp_reject;
            } else if (ph_fs_compile_ramps(compile_nir, budget, ramps,
                                           ramp_count, &fs->program) &&
                       (fs->program.sampler_mask & ~fs->sampler_mask)) {
               ph_fs_program_fini(&fs->program), fs->program.valid = false;
            }
            /* The colour targets are fixed-point: a program refused for a
             * value beyond the combiner's range may fit saturated. */
            if (!fs->program.valid && ramped) {
               nir_shader *sat = nir_shader_clone(NULL, compile_nir);
               struct ph_fs_program sat_prog;
               bool progress;

               if (sat && ph_fs_saturate_colour(sat)) {
                  do {
                     progress = false;
                     NIR_PASS(progress, sat, nir_opt_algebraic);
                     NIR_PASS(progress, sat, nir_opt_constant_folding);
                     NIR_PASS(progress, sat, nir_opt_copy_prop);
                     NIR_PASS(progress, sat, nir_opt_cse);
                     NIR_PASS(progress, sat, nir_opt_dce);
                  } while (progress);
                  if (ph_fs_compile_ramps(sat, budget, ramps, ramp_count,
                                          &sat_prog)) {
                     if (sat_prog.sampler_mask & ~fs->sampler_mask) {
                        ph_fs_program_fini(&sat_prog);
                     } else {
                        ph_fs_program_fini(&fs->program);
                        fs->program = sat_prog;
                        fs->colour_saturated = true;
                     }
                  }
               }
               ralloc_free(sat);
            }
            /* A colour beyond the combiner that is a function of one varying
             * samples a lookup texture of the program on unit 0, which
             * replaces the varying's coordinate units. */
            if (!fs->program.valid && ramped && !floor_ramps &&
                !fs->unit_remap && !compile_nir->info.fs.uses_discard) {
               struct ph_fs_program lookup_prog;
               struct ph_fs_ramp lookup_ramp;
               const char *lookup_reject = NULL;

               if (ph_fs_lower_lookup(compile_nir,
                      ph_nir_sampler_mask(compile_nir),
                      MIN2(ph_screen(p->screen)->texture_units,
                           PH_TEXTURE_UNITS_DRIVEN),
                      (1u << PH_FS_RAMP_DIRECTION) | (1u << PH_FS_RAMP_SCALAR) |
                      (1u << PH_FS_RAMP_PLANE), &lookup_ramp, &fs->lookup,
                      &lookup_reject)) {
                  if (ph_fs_compile_ramps(compile_nir, P3D_COMB_QUANTUM - 1,
                                          &lookup_ramp, 1, &lookup_prog)) {
                     ph_fs_program_fini(&fs->program);
                     fs->program = lookup_prog;
                     fs->sampler_mask = unramped_mask | 1u;
                     fs->uses_texture = true;
                  } else {
                     ph_fs_lookup_free(fs->lookup);
                     fs->lookup = NULL;
                  }
               } else if (ph_env("PARHELIA_TRACE")) {
                  fprintf(stderr, "parhelia: fs=%p has no lookup form: %s\n",
                          (void *)fs, lookup_reject);
               }
            }
            if (compile_nir->info.fs.uses_discard)
               fs->kill = ph_fs_build_kill(compile_nir, budget);
            ph_fs_find_uniform_coords(fs, s->ir.nir);
            ph_fs_find_coord_sources(fs, s->ir.nir);
            /* A ramp unit reads its staged coordinate attribute. */
            for (unsigned r = 0; fs->program.valid &&
                                 r < fs->program.ramp_count; r++) {
               const unsigned u = fs->program.ramps[r].unit;

               fs->coord_src_known[u] = true;
               fs->coord_src_name[u] = TGSI_SEMANTIC_TEXCOORD;
               fs->coord_src_index[u] = PH_RAMP_COORD_INDEX(u);
            }
            for (unsigned t = 0; fs->program.valid && t < fs->tap_count; t++) {
               const unsigned u = fs->taps[t].unit;

               fs->coord_src_known[u] = true;
               fs->coord_src_name[u] = TGSI_SEMANTIC_TEXCOORD;
               fs->coord_src_index[u] = PH_RAMP_COORD_INDEX(u);
            }
            if (s->ir.nir->info.outputs_written &
                BITFIELD64_BIT(FRAG_RESULT_DEPTH))
               fs->depth_from_texture =
                  ph_nir_depth_from_texture(s->ir.nir, &fs->depth_unit);
            if (!fs->depth_from_texture && !fs->program.valid &&
                (s->ir.nir->info.outputs_written &
                 BITFIELD64_BIT(FRAG_RESULT_DEPTH)))
               ph_fs_compile_uniform_depth(fs, s->ir.nir, compile_nir, budget);
            if (compile_nir != s->ir.nir)
               ralloc_free(compile_nir);
            /* a discard the alpha test carries is decided per draw */
            if (ph_fs_program_must_drop(s->ir.nir, &fs->program) && !fs->kill)
               fs->drop_reason = fs->program.reject ? fs->program.reject :
                                 "declared samplers";
            if (fs->program.valid)
               ph_fs_route_class(fs, ph_screen(p->screen)->single_texture_pipe);
            else if (ph_env("PARHELIA_TRACE"))
               fprintf(stderr, "parhelia: fs=%p has no combiner program: %s\n",
                       (void *)fs, fs->program.reject ? fs->program.reject :
                                                        "declared samplers");
            /* A linked program the combiner cannot express fails the link with
             * the reason, unless its discard runs as the alpha test or it is
             * the depth-from-texture shape. */
            if (!fs->program.valid && !fs->kill && !fs->depth_from_texture &&
                s->report_compile_error && !s->ir.nir->info.internal) {
               char *msg = NULL;

               if (asprintf(&msg, "parhelia: fragment shader exceeds the "
                            "combiner: %s", fs->program.reject ?
                            fs->program.reject :
                            "samples undeclared texture units") < 0)
                  msg = NULL;
               ((struct pipe_shader_state *)s)->error_message = msg;
               draw_delete_fragment_shader(ph_context(p)->draw, fs->draw);
               ph_fs_program_fini(&fs->program);
               free(fs->ucoord);
               FREE(fs);
               return NULL;
            }
         }
         if (ph_env("PARHELIA_TRACE")) {
            fprintf(stderr,
                    "parhelia: fs=%p combiner %s (%u instructions, %u runtime constants, class %u)\n",
                    (void *)fs, fs->program.valid ? "compiled" : "not compiled",
                    fs->program.comb.count, fs->program.const_ref_count,
                    fs->program.cls);
         }
         if (ph_env("PARHELIA_TRACE_FS_DUMP") && fs->program.valid) {
            fprintf(stderr, "parhelia: fs=%p route %d %d ramps %u taps %u\n",
                    (void *)fs, fs->program.route[0], fs->program.route[1],
                    fs->program.ramp_count, fs->tap_count);
            ph_fs_dump(&fs->program, stderr);
         }
         if (ph_env("PARHELIA_TRACE")) {
            fprintf(stderr,
                    "parhelia: create fs=%p samplers=%#x serial=%u texenv=%u inputs=%u uses_color=%u coord=%u/%u\n",
                    (void *)fs, fs->sampler_mask, fs->serial_op, fs->texenv,
                    info->num_inputs, fs->uses_color,
                    fs->coord_semantic, fs->coord_index);
            for (unsigned i = 0; i < info->num_inputs; i++)
               fprintf(stderr, "parhelia: fs input %u semantic=%u/%u\n",
                       i, info->input_semantic_name[i],
                       info->input_semantic_index[i]);
         }
      }
   }
   if (fs && !fs->draw) {
      FREE(fs);
      return NULL;
   }
   return fs;
}
static void ph_bind_fs(struct pipe_context *p, void *s)
{
   struct ph_context *ctx = ph_context(p);
   struct ph_fs *fs = s;
   if (ctx->fs == fs)
      return;
   ph_submit_vertices(ctx);
   ctx->fs = fs;
   ph_domain_reset(ctx);
   draw_bind_fragment_shader(ctx->draw, fs ? fs->draw : NULL);
}
static void ph_delete_fs(struct pipe_context *p, void *s)
{
   struct ph_fs *fs = s;
   if (fs) {
      if (fs->stipple) {
         nir_shader *nir = fs->stipple->draw->base.ir.nir;

         ph_delete_fs(p, fs->stipple);
         ralloc_free(nir);
      }
      util_dynarray_foreach(&fs->compare_variants, struct ph_compare_variant, c) {
         if (c->fs) {
            nir_shader *nir = c->fs->draw->base.ir.nir;

            ph_delete_fs(p, c->fs);
            ralloc_free(nir);
         }
      }
      util_dynarray_fini(&fs->compare_variants);

      if (fs->smooth) {
         nir_shader *nir = fs->smooth->draw->base.ir.nir;

         ph_delete_fs(p, fs->smooth);
         ralloc_free(nir);
      }
      draw_delete_fragment_shader(ph_context(p)->draw, fs->draw);
      if (fs->kill) {
         ph_fs_program_fini(&fs->kill->alpha_x);
         ph_fs_program_fini(&fs->kill->alpha_sub);
         ph_fs_program_fini(&fs->kill->colour);
      }
      FREE(fs->kill);
      free(fs->ucoord);
      ph_fs_program_fini(&fs->program);
      if (fs->lookup)
         ph_lookup_forget(ph_context(p), fs->lookup->uid);
      ph_fs_lookup_free(fs->lookup);
   }
   FREE(fs);
}

static void *
ph_create_velems(struct pipe_context *p, unsigned count,
                 const struct pipe_vertex_element *elem)
{
   struct ph_velems *v = CALLOC_STRUCT(ph_velems);
   (void)p;
   if (!v || count > PIPE_MAX_ATTRIBS) {
      FREE(v);
      return NULL;
   }
   v->count = count;
   memcpy(v->elem, elem, count * sizeof(*elem));
   return v;
}
static void ph_bind_velems(struct pipe_context *p, void *state)
{
   struct ph_context *ctx = ph_context(p);
   struct ph_velems *v = state;
   ctx->velems = v;
   if (v)
      draw_set_vertex_elements(ctx->draw, v->count, v->elem);
}

static void
ph_set_vertex_buffers(struct pipe_context *p, unsigned count,
                      const struct pipe_vertex_buffer *buffers)
{
   struct draw_context *draw = ph_context(p)->draw;
   util_set_vertex_buffers_count(draw->pt.vertex_buffer,
                                 &draw->pt.nr_vertex_buffers, buffers, count);
}

static void
ph_set_constant_buffer(struct pipe_context *p, mesa_shader_stage sh,
                       uint index, const struct pipe_constant_buffer *cb)
{
   struct ph_context *ctx = ph_context(p);
   if (index || sh >= MESA_SHADER_STAGES)
      return;
   pipe_resource_reference(&ctx->constants[sh], cb ? cb->buffer : NULL);
   ctx->user_constants[sh] = cb ? cb->user_buffer : NULL;
   ctx->constant_offset[sh] = cb ? cb->buffer_offset : 0;
   ctx->constant_size[sh] = cb ? cb->buffer_size : 0;
}

/* The Z unit converts a window depth d to fixed point as d * 2^n (a 0.75
 * quad stores 0xc00000 into Z24, MEASURED on the P690; D16 stores
 * round(d * 65536)), where GL and every clear path use d * (2^n - 1), so an
 * EQUAL test against a cleared 0.75 failed.  Scale the viewport's depth
 * range by (2^n - 1) / 2^n for the bound depth buffer. */
/* The code an n-bit Z buffer receives for window depth @d: d as the
 * viewport transform of the default depth range produces it in single
 * precision, scaled by (2^n - 1) / 2^n and truncated by the Z unit. */
static float
ph_depth_window(double d, unsigned bits)
{
   const float half = 0.5f * (float)((1u << bits) - 1) / (float)(1u << bits);
   const float ndc = (float)(2.0 * CLAMP(d, 0.0, 1.0) - 1.0);

   return ndc * half + half;
}

static uint32_t
ph_depth_code(double d, unsigned bits)
{
   return MIN2((uint32_t)(ph_depth_window(d, bits) * (float)(1u << bits)),
               (1u << bits) - 1);
}

static void
ph_update_viewport(struct ph_context *ctx)
{
   struct pipe_viewport_state vp = ctx->api_viewport;
   const enum pipe_format zf = ctx->fb.zsbuf.texture ? ctx->fb.zsbuf.format :
                                                       PIPE_FORMAT_NONE;
   const struct util_format_description *desc = util_format_description(zf);
   const unsigned bits = desc && util_format_has_depth(desc) ?
      desc->channel[desc->swizzle[0]].size : 0;

   if (!ctx->have_viewport)
      return;
   if (ctx->z_grid && bits && bits < 32) {
      const float k = (float)((1u << bits) - 1) / (float)(1u << bits);

      vp.scale[2] *= k;
      vp.translate[2] *= k;
   }
   ctx->viewport_y_flip = vp.scale[1] < 0.0f;
   /* c8/c9 are per batch: a transforming batch cannot span viewports. */
   if (ctx->triangles && ctx->transform &&
       memcmp(&ctx->viewport, &vp, sizeof ctx->viewport))
      ph_submit_vertices(ctx);
   ctx->viewport = vp;
   draw_set_viewport_states(ctx->draw, 0, 1, &vp);
}

static void ph_set_viewport(struct pipe_context *p, unsigned start,
                            unsigned count, const struct pipe_viewport_state *vp)
{
   struct ph_context *ctx = ph_context(p);

   if (ph_env("PARHELIA_TRACE_VIEWPORT")) {
      for (unsigned i = 0; i < count; i++)
         fprintf(stderr,
                 "parhelia: viewport[%u] scale=%g,%g,%g translate=%g,%g,%g\\n",
                 start + i, vp[i].scale[0], vp[i].scale[1], vp[i].scale[2],
                 vp[i].translate[0], vp[i].translate[1], vp[i].translate[2]);
   }
   draw_set_viewport_states(ctx->draw, start, count, vp);
   if (!start && count) {
      ctx->api_viewport = vp[0];
      ctx->have_viewport = true;
      ph_update_viewport(ctx);
   }
}
static void ph_set_clip(struct pipe_context *p, const struct pipe_clip_state *clip)
{ draw_set_clip_state(ph_context(p)->draw, clip); }
static void ph_set_scissor(struct pipe_context *p, unsigned start, unsigned count,
                           const struct pipe_scissor_state *s)
{
   struct ph_context *ctx = ph_context(p);

   if (!count || start || !s || !memcmp(&ctx->scissor, s, sizeof(*s)))
      return;
   ph_submit_vertices(ctx);
   ctx->scissor = *s;
}
static void ph_set_blend_color(struct pipe_context *p,
                               const struct pipe_blend_color *c)
{
   struct ph_context *ctx = ph_context(p);

   if (!c || !memcmp(&ctx->blend_color, c, sizeof(*c)))
      return;
   ph_submit_vertices(ctx);
   ctx->blend_color = *c;
}
static void ph_set_stencil_ref(struct pipe_context *p,
                               const struct pipe_stencil_ref r)
{
   struct ph_context *ctx = ph_context(p);

   if (!memcmp(&ctx->stencil_ref, &r, sizeof(r)))
      return;
   ph_submit_vertices(ctx);
   ctx->stencil_ref = r;
}
static void ph_noop_sample_mask(struct pipe_context *p, unsigned m)
{ (void)p; (void)m; }
static void ph_set_polygon_stipple(struct pipe_context *p,
                                   const struct pipe_poly_stipple *s)
{
   struct ph_context *ctx = ph_context(p);

   if (!memcmp(ctx->stipple, s->stipple, sizeof(ctx->stipple)))
      return;
   ph_submit_vertices(ctx);
   memcpy(ctx->stipple, s->stipple, sizeof(ctx->stipple));
   ctx->stipple_dirty = true;
}

/* Move an upload-class sampler image that is about to become a colour
 * buffer into render-target storage, keeping its level-0 texels.  The upload
 * class has no path back from the GPU, so rendering into it is refused; the
 * state tracker marks every colour texture renderable, so the class is only
 * decided for real here.  Lower levels exist only in the upload class. */
static bool
ph_promote_target(struct ph_context *ctx, struct pipe_resource *res)
{
   struct parhelia_resource *r = ph_resource(res), *storage;
   const struct pipe_box box = {
      .width = res->width0, .height = res->height0, .depth = 1,
   };

   /* Volume slices and cube faces stay in their sampler order; draws into
    * them are refused. */
   if (!r->upload_texture || r->mip.slices)
      return true;
   if ((ctx->aggregate_batches && ph_drain_batch(ctx)) || ph_wait_async(ctx))
      return false;
   storage = ph_resource_create_target_storage(r);
   if (!storage)
      return false;
   if ((r->mip_defined & BITFIELD_BIT(0)) &&
       !ph_copy_texture_box(ctx, &storage->base, 0, 0, 0, res, 0, &box,
                            res->width0, res->height0)) {
      pipe_resource_reference((struct pipe_resource **)&storage, NULL);
      return false;
   }
   ph_resource_swap_storage(r, storage);
   /* The image is in render order now: sample it through the copy. */
   r->base.bind |= PIPE_BIND_RENDER_TARGET;
   r->content_generation = storage->content_generation + 1;
   pipe_resource_reference((struct pipe_resource **)&storage, NULL);
   return true;
}

/* The engine renders into the base image of a 2-D resource only.  A surface
 * on a mip level above 0, a slice or a cube face is bound through a stand-in render
 * resource of that level's size, loaded from the level when bound and
 * written back after every draw and clear, so readers always see it. */
static void
ph_proxy_writeback(struct ph_context *ctx)
{
   for (unsigned i = 0; i < ctx->proxy_count; i++) {
      const struct ph_level_proxy *x = &ctx->proxy[i];
      struct pipe_box box;

      u_box_3d(0, 0, 0, x->proxy->width0, x->proxy->height0, 1, &box);
      if (ph_env("PARHELIA_TRACE_PROXY"))
         fprintf(stderr, "parhelia: stand-in %ux%u -> level %u layer %u of %ux%ux%u target %u\n",
                 x->proxy->width0, x->proxy->height0, x->level, x->layer,
                 x->res->width0, x->res->height0, x->res->depth0,
                 x->res->target);
      ctx->base.resource_copy_region(&ctx->base, x->res, x->level, 0, 0,
                                     x->layer, x->proxy, 0, &box);
   }
}

static void
ph_proxy_release(struct ph_context *ctx)
{
   ph_proxy_writeback(ctx);
   for (unsigned i = 0; i < ctx->proxy_count; i++) {
      pipe_resource_reference(&ctx->proxy[i].res, NULL);
      pipe_resource_reference(&ctx->proxy[i].proxy, NULL);
   }
   ctx->proxy_count = 0;
}

static void
ph_proxy_bind(struct ph_context *ctx, struct pipe_surface *surf)
{
   struct pipe_resource *res = surf->texture, templ, *proxy;
   const bool zs = util_format_is_depth_or_stencil(surf->format);
   struct ph_level_proxy *x;
   struct pipe_box box;

   /* A volume or cube map keeps sampler-only storage at every slice. */
   if (!res || res->target == PIPE_BUFFER ||
       (!surf->level && !surf->first_layer && !surf->last_layer &&
        res->target != PIPE_TEXTURE_3D && res->target != PIPE_TEXTURE_CUBE) ||
       ctx->proxy_count >= ARRAY_SIZE(ctx->proxy))
      return;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = surf->format;
   templ.width0 = u_minify(res->width0, surf->level);
   templ.height0 = u_minify(res->height0, surf->level);
   templ.depth0 = templ.array_size = 1;
   templ.bind = zs ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET;
   proxy = ctx->base.screen->resource_create(ctx->base.screen, &templ);
   if (!proxy) {
      fprintf(stderr, "parhelia: no %ux%u stand-in for a level %u surface\n",
              templ.width0, templ.height0, surf->level);
      return;
   }
   u_box_3d(0, 0, surf->first_layer, templ.width0, templ.height0, 1, &box);
   ctx->base.resource_copy_region(&ctx->base, proxy, 0, 0, 0, 0, res,
                                  surf->level, &box);
   x = &ctx->proxy[ctx->proxy_count++];
   x->res = NULL;
   x->proxy = NULL;
   pipe_resource_reference(&x->res, res);
   x->proxy = proxy;
   x->level = surf->level;
   x->layer = surf->first_layer;
   pipe_resource_reference(&surf->texture, proxy);
   surf->level = 0;
   surf->first_layer = surf->last_layer = 0;
}

/* Bind the colour stub into @fb when it has a depth/stencil surface but no
 * colour buffer; false when it needs none. */
static bool
ph_bind_colour_stub(struct ph_context *ctx, struct pipe_framebuffer_state *fb)
{
   struct pipe_resource *stub = ctx->colour_stub, templ;

   for (unsigned i = 0; i < fb->nr_cbufs; i++)
      if (fb->cbufs[i].texture)
         return false;
   if (!fb->zsbuf.texture || !fb->width || !fb->height)
      return false;
   if (!stub || stub->width0 < fb->width || stub->height0 < fb->height) {
      memset(&templ, 0, sizeof(templ));
      templ.target = PIPE_TEXTURE_2D;
      templ.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      templ.width0 = MAX2(fb->width, stub ? stub->width0 : 0);
      templ.height0 = MAX2(fb->height, stub ? stub->height0 : 0);
      templ.depth0 = templ.array_size = 1;
      templ.bind = PIPE_BIND_RENDER_TARGET;
      stub = ctx->base.screen->resource_create(ctx->base.screen, &templ);
      if (!stub) {
         fprintf(stderr, "parhelia: no %ux%u colour stub for a depth-only "
                 "framebuffer\n", templ.width0, templ.height0);
         return false;
      }
      pipe_resource_reference(&ctx->colour_stub, NULL);
      ctx->colour_stub = stub;
   }
   memset(&fb->cbufs[0], 0, sizeof(fb->cbufs[0]));
   pipe_resource_reference(&fb->cbufs[0].texture, stub);
   fb->cbufs[0].format = stub->format;
   fb->nr_cbufs = 1;
   return true;
}

static void
ph_set_framebuffer(struct pipe_context *p, const struct pipe_framebuffer_state *fb)
{
   struct ph_context *ctx = ph_context(p);

   if (ph_env("PARHELIA_TRACE_VIEWPORT"))
      fprintf(stderr, "parhelia: framebuffer=%ux%u cbufs=%u zs=%u\\n",
              fb ? fb->width : 0, fb ? fb->height : 0,
              fb ? fb->nr_cbufs : 0, fb && fb->zsbuf.texture);

   /* Gallium rebinds the same Q3 colour/depth pair frequently.  It is not a
    * rendering boundary: draining here turns otherwise batchable triangles
    * into tiny submits and forces async retirement pressure. */
   if (util_framebuffer_state_equal(&ctx->fb_api, fb))
      return;
   ph_submit_vertices(ctx);
   /* Only one depth surface may own the Z unit's tracked state.  A draw that
    * binds no depth surface at all leaves it alone (measured), so a
    * framebuffer without one is not a handover.  Creating a depth stand-in
    * loses an armed clear too (measured), so resolve before the stand-ins. */
   if (ctx->zinit_res && fb && fb->zsbuf.texture &&
       (ctx->zinit_res != ph_resource(fb->zsbuf.texture) ||
        fb->zsbuf.level || fb->zsbuf.first_layer))
      ph_zinit_resolve(ctx, ctx->zinit_res);
   ph_proxy_release(ctx);
   util_copy_framebuffer_state(&ctx->fb_api, fb);
   struct pipe_framebuffer_state bound;

   memset(&bound, 0, sizeof(bound));
   if (fb) {
      util_copy_framebuffer_state(&bound, fb);
      for (unsigned i = 0; i < bound.nr_cbufs; i++)
         ph_proxy_bind(ctx, &bound.cbufs[i]);
      ph_proxy_bind(ctx, &bound.zsbuf);
      ctx->colour_stub_bound = ph_bind_colour_stub(ctx, &bound);
      fb = &bound;
   }
   else {
      ctx->colour_stub_bound = false;
   }
   ph_update_blend(ctx);
   if (ctx->zinit_res && fb && fb->zsbuf.texture &&
       ctx->zinit_res != ph_resource(fb->zsbuf.texture))
      ph_zinit_resolve(ctx, ctx->zinit_res);
   ph_submit_vertices(ctx);
   for (unsigned i = 0; fb && i < fb->nr_cbufs; i++)
      if (fb->cbufs[i].texture && fb->cbufs[i].level == 0 &&
          !ph_promote_target(ctx, fb->cbufs[i].texture))
         fprintf(stderr, "parhelia: cannot move a %ux%u texture to "
                 "render-target storage\n", fb->cbufs[i].texture->width0,
                 fb->cbufs[i].texture->height0);
   util_copy_framebuffer_state(&ctx->fb, fb);
   if (fb && fb->zsbuf.texture)
      draw_set_zs_format(ctx->draw, fb->zsbuf.format);
   ph_update_viewport(ctx);
   util_unreference_framebuffer_state(&bound);
}

/* PLNWT bit planes of the colour components enabled in @colormask.  The fill
 * and the pixel pipeline both mask a 32-bit word, so a 16 bpp mask is
 * replicated into both halves. */
static uint32_t
ph_colour_write_mask(enum pipe_format format, unsigned colormask)
{
   const struct util_format_description *desc = util_format_description(format);
   const unsigned surf_mask = util_format_colormask(desc);
   uint32_t mask = 0;

   if ((colormask & surf_mask) == surf_mask)
      return 0xffffffffu;
   for (unsigned c = 0; c < 4; c++) {
      const unsigned s = desc->swizzle[c];

      if (!(colormask & BITFIELD_BIT(c)) || s > PIPE_SWIZZLE_W)
         continue;
      mask |= BITFIELD_MASK(desc->channel[s].size) << desc->channel[s].shift;
   }
   if (util_format_get_blocksize(format) == 2)
      mask |= mask << 16;
   return mask;
}

/* ------------------------------------------------ hardware depth clear
 *
 * Instead of filling the depth buffer, the P690 Z unit can be told that every
 * tile of the bound depth surface reads as one value: DWGCTL 0x20a arms the
 * surface and 0x12b4 plus DWGCTL 0x208 loads the value
 * (docs/parhelia-p690-fast-depth-clear.md).  Memory is not written.  The Z
 * unit materialises the value the first time it touches a tile, so rendering
 * is bit-identical to a filled buffer, but a tile no draw reached still holds
 * its old bytes: every consumer outside the Z unit needs a resolve first.
 *
 * The tracked state is one global hardware resource.  A 3-D draw against a
 * different depth surface destroys it, which is why only one surface is ever
 * armed and why it is resolved before the framebuffer moves away.  Measured
 * coverage from the surface origin: 640 tiles of 4096 bytes on the P690, and
 * 0x300000 bytes on the 0528 (the vendor's Parhelia-512 budget, MTXPARD
 * 0x9faae); rows past it keep the 2-D fill.
 */
static uint64_t
ph_zinit_coverage(const struct parhelia_screen *screen)
{
   if (ph_lx_family(screen->chip_id))
      return 640u * 4096u;
   if (ph_original_family(screen->chip_id))
      return 0x300000u;
   return 0;
}

/* The Z tile height: 16 rows in the LX 4 KiB tile, 32 in the 64x32 one. */
static unsigned
ph_zinit_tile_rows(const struct parhelia_resource *r)
{
   return r->tile_layout == PARHELIA_TILE_64X16_4K ? 16 : 32;
}

static bool
ph_zinit_enabled(void)
{
   /* The engine's own depth initialiser instead of a plane fill: worth 15%
    * in the ioquake3 timedemo (58.6 -> 67.3 fps at 1280x1024, depth 16) and
    * pixel-identical to the fill on demo frames 200, 400 and 700.  Read once;
    * PARHELIA_ZINIT_CLEAR=0 restores the fill. */
   static bool enabled, read;

   if (!read) {
      const char *value = getenv("PARHELIA_ZINIT_CLEAR");

      enabled = !value || strcmp(value, "0");
      read = true;
   }
   return enabled;
}

static unsigned
ph_zinit_alloc_height(const struct parhelia_resource *r)
{
   return ALIGN_POT(r->base.height0, ph_zinit_tile_rows(r));
}

static bool
ph_zinit_fits(const struct parhelia_resource *r, uint64_t coverage)
{
   const uint64_t bytes = (uint64_t)r->stride * r->cpp *
                          ph_zinit_alloc_height(r);

   return bytes && bytes <= coverage;
}

/* Rows of whole tile rows inside the coverage, counted in address order from
 * the origin (the vendor's own split, mtxpserd.dll 0xa0d95 / 0xa104f). */
static unsigned
ph_zinit_covered_rows(const struct parhelia_resource *r, uint64_t coverage)
{
   const unsigned tile_rows = ph_zinit_tile_rows(r);
   const uint64_t row_bytes = (uint64_t)r->stride * r->cpp * tile_rows;

   if (!row_bytes)
      return 0;
   return (unsigned)(coverage / row_bytes) * tile_rows;
}

/* The vertex depth that stores exactly @value.  Both quotients are exact in
 * binary32 for every representable depth, so the interpolated constant lands
 * in its own bucket whether the hardware rounds or truncates. */
static float
ph_zinit_depth(const struct parhelia_resource *r, uint32_t value)
{
   return r->cpp == 2 ? (float)(value & 0xffffu) / 65536.0f :
                        (float)(value & 0x00ffffffu) / 16777216.0f;
}

/* Write out every tile the Z unit is still holding: a full-surface rectangle
 * that passes only where the depth already reads as the clear value, writes
 * that value back, and touches no colour.  Then drop the tracked state. */
static void
ph_zinit_resolve(struct ph_context *ctx, struct parhelia_resource *r)
{
   struct parhelia_batch *batch = &ctx->build_batch;
   struct parhelia_resource *colour;
   struct p3d_state state;
   float quad[6][6];
   unsigned height;
   float depth;
   int ret;

   if (!r || !r->zinit_pending)
      return;
   r->zinit_pending = false;
   if (ctx->zinit_res == r)
      ctx->zinit_res = NULL;
   /* The clear takes its reference before arming, so the pair cannot come
    * apart; refuse rather than render from a surface that is not there. */
   if (!r->zinit_colour) {
      ctx->submit_error = -EINVAL;
      return;
   }
   colour = ph_resource(r->zinit_colour);
   height = ph_zinit_alloc_height(r);
   depth = ph_zinit_depth(r, r->zinit_value);
   state = (struct p3d_state){
      .colour = {.pitch = colour->stride, .fmt = colour->surf_fmt},
      .depth = {.pitch = r->stride, .fmt = r->surf_fmt},
      .screen_w = r->stride, .screen_h = height,
      .prim = P3D_TRIANGLES, .warp_entry = ctx->warp_passthrough,
      .dw_per_vertex = 6, .has_depth = 1, .depth_slot = 1, .depth_test = 1,
      .depth_func = P3D_CMP_EQUAL, .depth_write = 1, .mask_explicit = 1,
   };
   for (unsigned v = 0; v < 6; v++) {
      const bool right = v == 1 || v == 3 || v == 4;
      const bool bottom = v == 2 || v == 4 || v == 5;

      quad[v][0] = right ? (float)r->stride : 0.0f;
      quad[v][1] = bottom ? (float)height : 0.0f;
      quad[v][2] = depth;
      quad[v][3] = quad[v][4] = quad[v][5] = 0.0f;
   }
   ph_submit_vertices(ctx);
   ret = parhelia_batch_state(batch, colour->handle, r->handle, &state, 2);
   if (!ret)
      ret = ph_submit_batch(ctx, batch, quad, 36, r, colour, NULL, NULL);
   if (!ret)
      ret = ph_original_family(ph_screen(ctx->base.screen)->chip_id) ?
            parhelia_batch_zdiscard_original(batch, r->handle, r->stride,
                                             r->surf_fmt) :
            parhelia_batch_zdiscard(batch, r->handle, r->stride, r->surf_fmt);
   if (!ret)
      ret = ph_submit_batch(ctx, batch, NULL, 0, r, NULL, NULL, NULL);
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr, "parhelia: resolved hardware depth clear %#x on "
              "resource=%u ret=%d\n", r->zinit_value, r->handle, ret);
   if (ret)
      ctx->submit_error = ret;
   pipe_resource_reference(&r->zinit_colour, NULL);
}

/* Native format 0x36 keeps Z24 in [23:0] and stencil in [31:24] (unused for
 * X8Z24); 0x2a is a bare 16-bit depth with no stencil. */
static uint32_t
ph_zs_write_mask(enum pipe_format format, unsigned buffers,
                 uint8_t stencil_mask)
{
   if (format == PIPE_FORMAT_Z16_UNORM || format == PIPE_FORMAT_Z24X8_UNORM)
      return (buffers & PIPE_CLEAR_DEPTH) ? 0xffffffffu : 0;
   return ((buffers & PIPE_CLEAR_DEPTH) ? 0x00ffffffu : 0) |
          ((buffers & PIPE_CLEAR_STENCIL) ? (uint32_t)stencil_mask << 24 : 0);
}

static void ph_fill_surface(struct ph_context *ctx,
                            struct parhelia_resource *r, unsigned format,
                            uint32_t value, uint32_t write_mask, unsigned x,
                            unsigned y, unsigned width, unsigned height);

/* Arm the hardware clear instead of filling.  It carries neither an extent
 * nor a write mask, so only a whole-surface clear of every bit qualifies, and
 * the resolve needs a render target at least as wide as the depth surface. */
static bool
ph_zinit_clear(struct ph_context *ctx, struct parhelia_resource *r,
               uint32_t value, uint32_t write_mask, unsigned x, unsigned y,
               unsigned width, unsigned height)
{
   struct parhelia_batch *batch = &ctx->build_batch;
   struct parhelia_screen *screen = ph_screen(ctx->base.screen);
   const uint64_t coverage = ph_zinit_coverage(screen);
   struct parhelia_resource *colour;
   unsigned tail;
   int ret;

   if (!ph_zinit_enabled() || !coverage || !r || !r->handle ||
       ctx->submit_error)
      return false;
   if (x || y || width != r->base.width0 || height != r->base.height0 ||
       write_mask != 0xffffffffu || r->upload_texture)
      return false;
   tail = ph_zinit_fits(r, coverage) ? r->base.height0 :
                                       ph_zinit_covered_rows(r, coverage);
   if (!tail)
      return false;
   if (!ctx->fb.nr_cbufs || !ctx->fb.cbufs[0].texture)
      return false;
   colour = ph_resource(ctx->fb.cbufs[0].texture);
   if (!colour->handle || colour->upload_texture ||
       colour->stride < r->stride ||
       colour->size < (uint64_t)colour->stride * colour->cpp *
                      ph_zinit_alloc_height(r))
      return false;
   if (ctx->zinit_res && ctx->zinit_res != r)
      ph_zinit_resolve(ctx, ctx->zinit_res);
   /* The original family's vendor form, its colour target bound: the depth
    * surface is placed in the window above it before the state exists. */
   if (ph_original_family(screen->chip_id) ?
       parhelia_batch_zclear_original(batch, colour->handle, colour->stride,
                                    colour->surf_fmt, r->handle, r->stride,
                                    r->surf_fmt, value) :
       parhelia_batch_zclear(batch, r->handle, r->stride, r->surf_fmt, value))
      return false;
   ctx->stat.clears++;
   ctx->stat_kind = PH_STAT_CLEAR;
   ret = ph_submit_batch(ctx, batch, NULL, 0, r, NULL, NULL, NULL);
   ctx->stat_kind = PH_STAT_OTHER;
   if (ret) {
      ctx->submit_error = ret;
      return true;
   }
   r->zinit_pending = true;
   r->zinit_value = value;
   pipe_resource_reference(&r->zinit_colour, &colour->base);
   ctx->zinit_res = r;
   /* Rows past the coverage are plain memory: fill them with the same value.
    * Tracked tiles of a partial tile row get it twice, which is harmless. */
   if (tail < r->base.height0)
      ph_fill_surface(ctx, r, r->surf_fmt, value, write_mask, 0, tail,
                      width, r->base.height0 - tail);
   r->tiled_dirty = true;
   r->linear_dirty = false;
   r->mip_defined |= BITFIELD_BIT(0);
   r->content_generation++;
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: hardware depth clear resource=%u fmt=%#x value=%#x "
              "%ux%u\n", r->handle, r->surf_fmt, value, width, height);
   return true;
}

/* Clear part of a surface whose tiles the Z unit still owns.  A 2-D fill would
 * be overwritten when the Z unit later materialises its value into the same
 * tiles, so the rectangle goes through the depth pipe instead: depth ALWAYS
 * with depth writes and no colour writes.  This is the vendor's own P690
 * default (Use3DClearSundog = 1). */
static bool
ph_zinit_rect_clear(struct ph_context *ctx, struct parhelia_resource *r,
                    uint32_t value, uint32_t write_mask, unsigned x,
                    unsigned y, unsigned width, unsigned height)
{
   struct parhelia_batch *batch = &ctx->build_batch;
   struct parhelia_resource *colour;
   struct p3d_state state;
   float quad[6][6];
   int ret;

   if (!r->zinit_pending || ctx->submit_error || !r->zinit_colour)
      return false;
   /* Stencil planes have no depth-pipe equivalent here, and a masked clear
    * has no equivalent at all. */
   if ((r->surf_fmt != 0x2a && r->base.format != PIPE_FORMAT_Z24X8_UNORM) ||
       write_mask != 0xffffffffu)
      return false;
   colour = ph_resource(r->zinit_colour);
   state = (struct p3d_state){
      .colour = {.pitch = colour->stride, .fmt = colour->surf_fmt},
      .depth = {.pitch = r->stride, .fmt = r->surf_fmt},
      .screen_w = r->stride, .screen_h = ph_zinit_alloc_height(r),
      .prim = P3D_TRIANGLES, .warp_entry = ctx->warp_passthrough,
      .dw_per_vertex = 6, .has_depth = 1, .depth_slot = 1, .depth_test = 1,
      .depth_func = P3D_CMP_ALWAYS, .depth_write = 1, .mask_explicit = 1,
      .scissor = 1, .scissor_minx = (uint16_t)x, .scissor_miny = (uint16_t)y,
      .scissor_maxx = (uint16_t)(x + width),
      .scissor_maxy = (uint16_t)(y + height),
   };
   for (unsigned v = 0; v < 6; v++) {
      const bool right = v == 1 || v == 3 || v == 4;
      const bool bottom = v == 2 || v == 4 || v == 5;

      quad[v][0] = right ? (float)(x + width) : (float)x;
      quad[v][1] = bottom ? (float)(y + height) : (float)y;
      quad[v][2] = ph_zinit_depth(r, value);
      quad[v][3] = quad[v][4] = quad[v][5] = 0.0f;
   }
   ph_submit_vertices(ctx);
   ctx->stat.clears++;
   ctx->stat.clear_pixels += (uint64_t)width * height;
   ctx->stat_kind = PH_STAT_CLEAR;
   ctx->stat_pixels = (double)width * height;
   ret = parhelia_batch_state(batch, colour->handle, r->handle, &state, 2);
   if (!ret)
      ret = ph_submit_batch(ctx, batch, quad, 36, r, colour, NULL, NULL);
   ctx->stat_kind = PH_STAT_OTHER;
   ctx->stat_pixels = 0;
   if (ret) {
      ctx->submit_error = ret;
      return true;
   }
   r->tiled_dirty = true;
   r->content_generation++;
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: depth-pipe clear resource=%u value=%#x %u,%u %ux%u\n",
              r->handle, value, x, y, width, height);
   return true;
}

/* The 2-D fill itself, for a surface with no Z unit state to resolve. */
static void
ph_fill_surface(struct ph_context *ctx, struct parhelia_resource *r,
                unsigned format, uint32_t value, uint32_t write_mask,
                unsigned x, unsigned y, unsigned width, unsigned height)
{
   struct parhelia_batch *batch = &ctx->build_batch;
   unsigned alloc_height;
   bool full;
   int ret;

   if (r->upload_texture) {
      PH_REFUSE("a clear of sampler-only storage is dropped\n");
      return;
   }
   if (x >= r->base.width0 || y >= r->base.height0)
      return;
   width = MIN2(width, r->base.width0 - x);
   height = MIN2(height, r->base.height0 - y);
   if (!width || !height)
      return;
   full = !x && !y && width == r->base.width0 &&
          height == r->base.height0 && write_mask == 0xffffffffu;
   /* A partial clear keeps old tiled bits, so an authoritative linear image
    * must first become the tiled one. */
   if (!full && r->linear_dirty) {
      ph_resource_upload_tiled(r);
      if (r->linear_dirty) {
         ctx->submit_error = -EIO;
         return;
      }
   }
   if (ph_env("PARHELIA_CPU_CLEAR") && r->map && !r->transfer_shadow) {
      /* The normal path queues this packet behind previous 3D work.  A CPU
       * diagnostic must establish the same ordering before touching the
       * tiled mapping directly. */
      ph_wait_async(ctx);
      for (unsigned j = y; j < y + height; j++)
         for (unsigned i = x; i < x + width; i++) {
            uint8_t *texel = (uint8_t *)r->map +
               p3d_tile_offset_layout_cpp(i, j, r->stride, r->tile_layout,
                                          r->cpp);
            uint32_t old = 0;

            memcpy(&old, texel, r->cpp);
            old = (old & ~write_mask) | (value & write_mask);
            memcpy(texel, &old, r->cpp);
         }
      r->upload_dirty |= r->upload_texture;
      r->tiled_dirty = true;
      r->linear_dirty = false;
      r->mip_defined |= BITFIELD_BIT(0);
      r->content_generation++;
      if (ph_env("PARHELIA_TRACE"))
         fprintf(stderr,
                 "parhelia: CPU clear resource=%u fmt=%#x value=%#x mask=%#x %u,%u %ux%u stride=%u layout=%u\n",
                 r->handle, format, value, write_mask, x, y, width, height,
                 r->stride, r->tile_layout);
      return;
   }
   ctx->stat.clears++;
   ctx->stat.clear_pixels += (uint64_t)width * height;
   ctx->stat_kind = PH_STAT_CLEAR;
   ctx->stat_pixels = (double)width * height;
   /* The kernel allocates whole tile rows: 16 on P690, 32 otherwise. */
   alloc_height = ALIGN_POT(r->base.height0,
                            r->tile_layout == PARHELIA_TILE_64X16_4K ? 16 : 32);
   /* The whole-surface fill may also cover the padding tile rows. */
   if (full)
      height = alloc_height;
   ret = parhelia_batch_clear_rect_surface(batch, r->handle, r->base.width0,
                                           alloc_height, r->stride, format,
                                           value, write_mask, x, y, width,
                                           height);
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: GPU clear resource=%u fmt=%#x value=%#x mask=%#x %u,%u %ux%u ret=%d\n",
              r->handle, format, value, write_mask, x, y, width, height, ret);
   if (!ret)
      /* A clear has no CPU consumer before the next ordered 3D batch.  Use
       * the same fenced async queue as draws so Q3 does not retire colour and
       * depth clears on the CPU before it can queue world rendering. */
      ret = ph_submit_batch(ctx, batch, NULL, 0, r, NULL, NULL, NULL);
   ctx->stat_kind = PH_STAT_OTHER;
   ctx->stat_pixels = 0;
   if (ret)
      ctx->submit_error = ret;
   else
      r->tiled_dirty = true;
   if (!ret)
      r->linear_dirty = false;
   if (!ret) {
      r->mip_defined |= BITFIELD_BIT(0);
      r->content_generation++;
   }
}

static void
ph_clear_surface(struct ph_context *ctx, struct parhelia_resource *r,
                 unsigned format, uint32_t value, uint32_t write_mask,
                 unsigned x, unsigned y, unsigned width, unsigned height)
{
   if (!r || !r->handle || ctx->submit_error || !write_mask)
      return;
   /* A fill only writes the bits of its mask inside its rectangle, so the
    * tiles the Z unit still owns have to reach memory first. */
   ph_zinit_resolve(ctx, r);
   ph_fill_surface(ctx, r, format, value, write_mask, x, y, width, height);
}

/* The format whose byte layout a colour surface's storage really has. */
static enum pipe_format
ph_storage_format(enum pipe_format format)
{
   uint8_t order[4];

   if (!ph_rgba8_order(format, order))
      return format;
   return order[3] == 0xff ? PIPE_FORMAT_B8G8R8X8_UNORM :
                             PIPE_FORMAT_B8G8R8A8_UNORM;
}

static void
ph_clear_colour(struct ph_context *ctx, const struct pipe_surface *surf,
                const union pipe_color_union *color, unsigned colormask,
                unsigned x, unsigned y, unsigned width, unsigned height)
{
   struct parhelia_resource *r = ph_resource(surf->texture);
   const bool widened = ph_format_widened(surf->format);
   const unsigned channels = ph_target_channels(surf->format);
   const enum pipe_format format = widened ? PIPE_FORMAT_B8G8R8A8_UNORM :
                                   ph_storage_format(surf->format);
   /* A widened target's missing channels stay at 0, 0, 1: a clear of every
    * channel it has writes the whole texel. */
   const uint32_t mask = !widened ? ph_colour_write_mask(format, colormask) :
      (colormask & channels) == channels ? 0xffffffffu :
      ph_colour_write_mask(format, colormask & channels);
   union util_color packed;
   uint32_t value;
   const float wide_colour[4] = {
      color->f[0], (channels & PIPE_MASK_G) ? color->f[1] : 0.0f, 0.0f, 1.0f,
   };

   if (!mask)
      return;
   util_pack_color(widened ? wide_colour : color->f, format, &packed);
   /* The 2-D fill writes whole dwords, so a 16 bpp value has to appear in
    * both halves; the rectangle itself is honoured at pixel granularity. */
   value = r->cpp == 2 ? (packed.ui[0] & 0xffffu) * 0x10001u : packed.ui[0];
   ph_clear_surface(ctx, r, r->surf_fmt, value, mask, x, y, width, height);
}

static void
ph_clear_zs(struct ph_context *ctx, const struct pipe_surface *surf,
            unsigned buffers, uint8_t stencil_mask, double depth,
            unsigned stencil, unsigned x, unsigned y, unsigned width,
            unsigned height)
{
   struct parhelia_resource *r = ph_resource(surf->texture);
   const uint32_t mask = ph_zs_write_mask(surf->format, buffers, stencil_mask);
   uint32_t value;

   if (!mask)
      return;
   if (surf->format != PIPE_FORMAT_Z24_UNORM_S8_UINT &&
       surf->format != PIPE_FORMAT_Z24X8_UNORM &&
       surf->format != PIPE_FORMAT_Z16_UNORM) {
      PH_REFUSE("a depth clear of format %u is dropped\n", surf->format);
      return;
   }
   /* The Z unit stores a window depth d as trunc(d * 2^n) (the vendor packs
    * D16 clears as min(d * 65536, 0xffff), docs/parhelia-p690-zcompression-
    * clear-banks-re.md 4.3); ph_update_viewport scales d by (2^n - 1) / 2^n
    * in single precision, so a clear stores what a fragment at d stores. */
   if (!ctx->z_grid)
      value = surf->format == PIPE_FORMAT_Z16_UNORM ?
              (uint32_t)MIN2(depth * 65536.0, 65535.0) * 0x10001u :
              util_pack_z_stencil(surf->format, depth, stencil);
   else if (surf->format == PIPE_FORMAT_Z16_UNORM)
      value = ph_depth_code(depth, 16) * 0x10001u;
   else
      value = ph_depth_code(depth, 24) | (stencil & 0xffu) << 24;
   if (ph_zinit_clear(ctx, r, value, mask, x, y, width, height) ||
       ph_zinit_rect_clear(ctx, r, value, mask, x, y, width, height))
      return;
   ph_clear_surface(ctx, r, r->surf_fmt, value, mask, x, y, width, height);
}

static bool ph_surface_is_base(const struct pipe_surface *surf);

/* A clear of a surface the clear packet cannot address (a mip level other
 * than the base, a slice): the level's texels through the transfer path,
 * honouring the colour mask. */
static void
ph_clear_level(struct ph_context *ctx, const struct pipe_surface *surf,
               const union pipe_color_union *color, unsigned colormask,
               unsigned zs_buffers, double depth, unsigned stencil,
               uint8_t stencil_mask, unsigned x, unsigned y, unsigned width,
               unsigned height)
{
   const enum pipe_format fmt = surf->format;
   struct pipe_transfer *t;
   struct pipe_box box;
   uint8_t *map;

   u_box_3d(x, y, surf->first_layer, width, height,
            surf->last_layer - surf->first_layer + 1, &box);
   map = ctx->base.texture_map(&ctx->base, surf->texture, surf->level,
                               PIPE_MAP_READ | PIPE_MAP_WRITE, &box, &t);
   if (!map)
      return;
   for (int z = 0; z < box.depth; z++)
      for (int row = 0; row < box.height; row++) {
         uint8_t *p = map + (size_t)z * t->layer_stride +
                      (size_t)row * t->stride;

         for (int col = 0; col < box.width; col++, p += util_format_get_blocksize(fmt)) {
            if (color) {
               float rgba[4];

               util_format_unpack_rgba(fmt, rgba, p, 1);
               for (unsigned c = 0; c < 4; c++)
                  if (colormask & (PIPE_MASK_R << c))
                     rgba[c] = color->f[c];
               util_format_pack_rgba(fmt, p, rgba, 1);
               continue;
            }
            if ((zs_buffers & PIPE_CLEAR_DEPTH) &&
                util_format_has_depth(util_format_description(fmt))) {
               const float d = (float)depth;

               util_format_pack_z_float(fmt, p, &d, 1);
            }
            if ((zs_buffers & PIPE_CLEAR_STENCIL) &&
                util_format_has_stencil(util_format_description(fmt))) {
               uint8_t sv;

               util_format_unpack_s_8uint(fmt, &sv, p, 1);
               sv = (sv & ~stencil_mask) | (stencil & stencil_mask);
               util_format_pack_s_8uint(fmt, p, &sv, 1);
            }
         }
      }
   ctx->base.texture_unmap(&ctx->base, t);
}

static void
ph_clear(struct pipe_context *p, unsigned buffers, uint32_t color_mask,
         uint8_t stencil_mask, const struct pipe_scissor_state *scissor,
         const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct ph_context *ctx = ph_context(p);
   unsigned x0 = 0, y0 = 0, x1 = ctx->fb.width, y1 = ctx->fb.height;

   ph_submit_vertices(ctx);
   if (scissor) {
      x0 = MIN2(scissor->minx, x1);
      y0 = MIN2(scissor->miny, y1);
      x1 = MIN2(scissor->maxx, x1);
      y1 = MIN2(scissor->maxy, y1);
   }
   if (x0 >= x1 || y0 >= y1)
      return;
   /* Every bound colour buffer: glDrawBuffer(GL_FRONT_AND_BACK) binds two. */
   for (unsigned i = 0; i < ctx->fb.nr_cbufs; i++) {
      const struct pipe_surface *cb = &ctx->fb.cbufs[i];

      if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !cb->texture)
         continue;
      if (ph_surface_is_base(cb))
         ph_clear_colour(ctx, cb, &color[0],
                         color_mask & PIPE_MASK_RGBA, x0, y0, x1 - x0, y1 - y0);
      else
         ph_clear_level(ctx, cb, &color[0], color_mask,
                        0, 0, 0, 0, x0, y0, x1 - x0, y1 - y0);
   }
   if ((buffers & PIPE_CLEAR_DEPTHSTENCIL) && ctx->fb.zsbuf.texture) {
      if (ph_surface_is_base(&ctx->fb.zsbuf))
         ph_clear_zs(ctx, &ctx->fb.zsbuf, buffers, stencil_mask, depth,
                     stencil, x0, y0, x1 - x0, y1 - y0);
      else
         ph_clear_level(ctx, &ctx->fb.zsbuf, NULL, 0, buffers, depth,
                        stencil, stencil_mask, x0, y0, x1 - x0, y1 - y0);
   }
   if (ctx->proxy_count)
      ph_proxy_writeback(ctx);
}

/* The clear packet addresses the base image only. */
static bool
ph_surface_is_base(const struct pipe_surface *surf)
{
   return surf->texture && surf->texture->target != PIPE_BUFFER &&
          !surf->level && !surf->first_layer && !surf->last_layer;
}

static void
ph_clear_render_target(struct pipe_context *p, struct pipe_surface *dst,
                       const union pipe_color_union *color,
                       unsigned dstx, unsigned dsty,
                       unsigned width, unsigned height,
                       bool render_condition_enabled)
{
   struct ph_context *ctx = ph_context(p);

   (void)render_condition_enabled;
   ph_submit_vertices(ctx);
   if (!ph_surface_is_base(dst) || ph_resource(dst->texture)->upload_texture) {
      util_clear_render_target(p, dst, color, dstx, dsty, width, height);
      return;
   }
   ph_clear_colour(ctx, dst, color, PIPE_MASK_RGBA, dstx, dsty, width,
                   height);
}

static void
ph_clear_depth_stencil(struct pipe_context *p, struct pipe_surface *dst,
                       unsigned clear_flags, double depth, unsigned stencil,
                       unsigned dstx, unsigned dsty,
                       unsigned width, unsigned height,
                       bool render_condition_enabled)
{
   struct ph_context *ctx = ph_context(p);

   (void)render_condition_enabled;
   ph_submit_vertices(ctx);
   if (!ph_surface_is_base(dst) || ph_resource(dst->texture)->upload_texture) {
      util_clear_depth_stencil(p, dst, clear_flags, depth, stencil, dstx,
                               dsty, width, height);
      return;
   }
   ph_clear_zs(ctx, dst, clear_flags, 0xff, depth, stencil, dstx, dsty,
               width, height);
}

static void
ph_flush(struct pipe_context *p, struct pipe_fence_handle **fence,
         unsigned flags)
{
   struct ph_context *ctx = ph_context(p);
   struct parhelia_resource *color = NULL;

   ph_submit_vertices(ctx);
   if (flags & PIPE_FLUSH_END_OF_FRAME)
      ph_stats_frame(ctx);
   /* The following X-server blit can order against an async kernel request,
    * but it cannot see an aggregate still owned by this userspace context. */
   if (ctx->aggregate_batches && ph_drain_batch(ctx)) {
      if (fence)
         *fence = NULL;
      return;
   }
   if (ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture && !ctx->colour_stub_bound)
      color = ph_resource(ctx->fb.cbufs[0].texture);
   /* The X server's native DRI2 blit submits synchronously on the same
    * device.  The kernel submit lock drains prior async render work before
    * executing it, so waiting here would serialize the pipeline twice. */
   /* A native window swap is followed by the X server's synchronous blit on
    * this same device and can remain pipelined.  A finish hint is the GL
    * completion contract, and a caller asking for a fence cannot be given a
    * NULL fence until this driver implements a real pipe_fence_handle. */
   if (!color || !color->native_shared ||
       (flags & PIPE_FLUSH_HINT_FINISH) ||
       ((flags & PIPE_FLUSH_FENCE_FD) && fence) ||
       (fence && !(flags & PIPE_FLUSH_END_OF_FRAME)))
      ph_wait_async(ctx);
   if (color) {
      if (!color->native_shared)
         ph_resource_resolve_linear(color);
   }
   if (fence)
      *fence = NULL;
}

static void
ph_flush_resource(struct pipe_context *p, struct pipe_resource *resource)
{
   struct ph_context *ctx = ph_context(p);

   ph_zinit_resolve(ctx, ph_resource(resource));
   ph_submit_vertices(ctx);
   /* Export/present consumers can observe only work already handed to the
    * kernel.  Keep it asynchronous for native resources, then rely on the
    * server's ordered blit; CPU-visible resources wait below. */
   if (ctx->aggregate_batches && ph_drain_batch(ctx))
      return;
   /* Native presentation is ordered by the server's synchronous GPU blit.
    * Linear exports still need the client wait before their CPU resolve. */
   if (resource->target == PIPE_BUFFER ||
       !ph_resource(resource)->native_shared)
      ph_wait_async(ctx);
   if (ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: flush resource=%p target=%u dirty=%u error=%d\n",
              (void *)resource, resource->target,
              resource->target == PIPE_BUFFER ? 0 :
                 ph_resource(resource)->tiled_dirty,
              ctx->submit_error);
   if (resource->target != PIPE_BUFFER &&
       !ph_resource(resource)->native_shared)
      ph_resource_resolve_linear(ph_resource(resource));
}

static bool ph_resource_uses_sampler_layout(const struct parhelia_resource *r);

/* Byte offset of texel (x,y) of @level in the linear or the native backing,
 * using the same addressing as the transfer paths. */
static size_t
ph_texel_offset(const struct parhelia_resource *r, unsigned level,
                unsigned x, unsigned y, unsigned cpp, bool linear)
{
   if (linear)
      return r->linear_mip.levels ?
         ph_mip_texel_offset_cpp(&r->linear_mip, level, x, y, true, cpp) :
         (size_t)y * r->linear_stride + x * cpp;
   if (ph_resource_uses_sampler_layout(r))
      return r->mip.levels ?
         ph_mip_texel_offset_cpp(&r->mip, level, x, y, false, cpp) :
         p3d_texture_tile_offset_chip(
            x, y, ph_sampler_storage_pitch(r->base.screen, r->base.width0,
                                           cpp),
            cpp, ph_screen(r->base.screen)->chip_id, r->tile_layout);
   return p3d_tile_offset_layout_cpp(x, y, r->stride, r->tile_layout, r->cpp);
}

/* Levels above zero exist only in the sampler mip chain or a linear chain. */
static bool
ph_level_extent(const struct parhelia_resource *r, unsigned level,
                unsigned *width, unsigned *height)
{
   if (!level) {
      *width = r->base.width0;
      *height = r->base.height0;
      return true;
   }
   if (level >= ph_resource_mip_levels(r) ||
       (r->map && !ph_resource_uses_sampler_layout(r)) ||
       (r->linear_map && r->linear_mip.levels <= level))
      return false;
   *width = r->mip.level[level].width;
   *height = r->mip.level[level].height;
   return true;
}

struct ph_byte_range {
   uint64_t start, end;
};

static int
ph_byte_range_cmp(const void *a, const void *b)
{
   const struct ph_byte_range *ra = a, *rb = b;

   return ra->start < rb->start ? -1 : ra->start > rb->start;
}

/* Boxes above this many texels move whole tile rows instead of sorted runs. */
#define PH_SHADOW_BOX_TEXELS (64 * 1024)
/* A transfer call costs about as much as 64 downloaded bytes (MEASURED). */
#define PH_SHADOW_GAP 64
/* The host aperture addresses VRAM in 32-byte units on the P690 and in
 * 64-byte units on the 0528 (MEASURED). */
static unsigned
ph_shadow_align(const struct parhelia_resource *r)
{
   return ph_original_family(ph_screen(r->base.screen)->chip_id) ? 64 : 32;
}

/* Move only the aperture units holding the texels of a box between the CPU
 * shadow and VRAM (aperture reads cost ~0.4 us per dword, MEASURED on the
 * P690).  Uploads cover whole runs: download the same box before writing. */
static int
ph_shadow_transfer_box(struct parhelia_resource *r, unsigned level,
                       int x0, int y0, int w, int h, bool upload)
{
   const unsigned cpp = util_format_get_blocksize(r->base.format);
   const unsigned align = ph_shadow_align(r);
   struct util_dynarray runs;
   struct ph_byte_range *run;
   unsigned count, n = 0;
   int ret = 0;

   if (w <= 0 || h <= 0)
      return 0;
   if ((uint64_t)w * h > PH_SHADOW_BOX_TEXELS) {
      if (level)
         return upload ? ph_resource_upload_shadow(r) :
                         ph_resource_download_shadow(r);
      return upload ? ph_resource_upload_shadow_rows(r, y0, y0 + h) :
                      ph_resource_download_shadow_rows(r, y0, y0 + h);
   }
   util_dynarray_init(&runs, NULL);
   for (int y = y0; y < y0 + h; y++)
      for (int x = x0; x < x0 + w; x++) {
         const size_t off = ph_texel_offset(r, level, x, y, cpp, false);
         const uint64_t start = off & ~(uint64_t)(align - 1);
         const uint64_t end = align64(off + cpp, align);
         struct ph_byte_range *last = runs.size ?
            util_dynarray_top_ptr(&runs, struct ph_byte_range) : NULL;
         struct ph_byte_range *next;

         if (last && start >= last->start && start <= last->end)
            last->end = MAX2(last->end, end);
         else if (!(next = util_dynarray_grow(&runs, struct ph_byte_range, 1))) {
            util_dynarray_fini(&runs);
            return -ENOMEM;
         } else
            *next = (struct ph_byte_range) { start, end };
      }
   run = util_dynarray_begin(&runs);
   count = util_dynarray_num_elements(&runs, struct ph_byte_range);
   qsort(run, count, sizeof(*run), ph_byte_range_cmp);
   for (unsigned i = 1; i < count; i++) {
      if (run[i].start <= run[n].end + PH_SHADOW_GAP)
         run[n].end = MAX2(run[n].end, run[i].end);
      else
         run[++n] = run[i];
   }
   for (unsigned i = 0; count && i <= n && !ret; i++)
      ret = ph_resource_shadow_range(r, upload, run[i].start,
                                     MIN2(run[i].end, r->size) - run[i].start);
   util_dynarray_fini(&runs);
   return ret;
}

static bool
ph_copy_texture_box(struct ph_context *ctx,
                    struct pipe_resource *dst, unsigned dst_level,
                    int dstx, int dsty,
                    struct pipe_resource *src, unsigned src_level,
                    const struct pipe_box *src_box,
                    int dst_width, int dst_height)
{
   struct parhelia_resource *sr = ph_resource(src);
   struct parhelia_resource *dr = ph_resource(dst);
   const int width = abs(src_box->width);
   const int height = abs(src_box->height);
   const int dwidth = abs(dst_width);
   const int dheight = abs(dst_height);
   const unsigned cpp = util_format_get_blocksize(src->format);

   /* decoded S3TC images have no storage of their own to copy */
   if (sr->decoded || dr->decoded)
      return false;
   const int dst_x0 = dst_width < 0 ? dstx - width : dstx;
   const int dst_y0 = dst_height < 0 ? dsty - height : dsty;
   unsigned src_w, src_h, dst_w, dst_h;
   uint8_t *staging;

   ph_submit_vertices(ctx);
   if (!width || !height || width != dwidth || height != dheight ||
       src_box->depth != 1 || (cpp != 4 && cpp != 2 && cpp != 1) ||
       ph_format_widened(src->format) || ph_format_widened(dst->format) ||
       util_format_get_blocksize(dst->format) != cpp ||
       !ph_level_extent(sr, src_level, &src_w, &src_h) ||
       !ph_level_extent(dr, dst_level, &dst_w, &dst_h) ||
       (uint64_t)width * height * cpp > SIZE_MAX)
      return false;

   /* This implementation copies through CPU-visible mappings.  The source
    * can still be owned by the asynchronous render queue when ReadPixels
    * asks the state tracker to blit into a staging texture.  Waiting here is
    * required before either its linear shadow or its tiled map is sampled. */
   if (ph_wait_async(ctx) || ctx->submit_error)
      return false;
   if (sr->transfer_shadow &&
       ph_shadow_transfer_box(sr, src_level,
          src_box->width < 0 ? src_box->x - width : src_box->x,
          src_box->height < 0 ? src_box->y - height : src_box->y,
          width, height, false))
      return false;
   if (dr->transfer_shadow && (dr->mip_defined & BITFIELD_BIT(dst_level)) &&
       ph_shadow_transfer_box(dr, dst_level, dst_x0, dst_y0, dwidth, dheight,
                              false))
      return false;

   /* Imported drawable/renderbuffer resources have a linear companion.
    * Use the recovered type-2-to-type-1 hardware BITBLT to detile them once;
    * the format/orientation copy below can then read normal pitched rows. */
   /* A transfer shadow was just downloaded and is exact; the linear
    * companion is only resolved for images without one. */
   if (sr->linear_map && !sr->transfer_shadow)
      ph_resource_resolve_linear(sr);

   staging = MALLOC((size_t)width * height * cpp);
   if (!staging)
      return false;


   for (int y = 0; y < height; y++) {
      const int sy = src_box->y + (src_box->height < 0 ? -1 - y : y);
      for (int x = 0; x < width; x++) {
         const int sx = src_box->x + (src_box->width < 0 ? -1 - x : x);
         uint8_t *out = staging + ((size_t)y * width + x) * cpp;
         const bool linear = sr->linear_map && !sr->tiled_dirty &&
                             !sr->transfer_shadow;

         if (sx < 0 || sy < 0 || sx >= (int)src_w || sy >= (int)src_h ||
             (!linear && !sr->map)) {
            FREE(staging);
            return false;
         }
         memcpy(out, (uint8_t *)(linear ? sr->linear_map : sr->map) +
                     ph_texel_offset(sr, src_level, sx, sy, cpp, linear),
                cpp);
      }
   }

   for (int y = 0; y < height; y++) {
      const int dy = dsty + (dst_height < 0 ? -1 - y : y);
      for (int x = 0; x < width; x++) {
         const int dx = dstx + (dst_width < 0 ? -1 - x : x);
         const uint8_t *in = staging + ((size_t)y * width + x) * cpp;

         if (dx < 0 || dy < 0 || dx >= (int)dst_w || dy >= (int)dst_h ||
             (!dr->linear_map && !dr->map)) {
            FREE(staging);
            return false;
         }
         if (dr->linear_map)
            memcpy((uint8_t *)dr->linear_map +
                      ph_texel_offset(dr, dst_level, dx, dy, cpp, true),
                   in, cpp);
         if (dr->map)
            memcpy((uint8_t *)dr->map +
                      ph_texel_offset(dr, dst_level, dx, dy, cpp, false),
                   in, cpp);
      }
   }
   dr->tiled_dirty = dr->map && !dr->linear_map;
   dr->linear_dirty = dr->linear_map && !dr->map;
   if (dr->transfer_shadow) {
      int ret = ph_shadow_transfer_box(dr, dst_level, dst_x0, dst_y0,
                                       dwidth, dheight, true);

      if (ret) {
         ctx->submit_error = ret;
         FREE(staging);
         return false;
      }
      dr->tiled_dirty = false;
      dr->linear_dirty = false;
   }
   dr->upload_dirty |= dr->upload_texture;
   dr->mip_defined |= BITFIELD_BIT(dst_level);
   dr->content_generation++;
   FREE(staging);
   return true;
}

static void
ph_resource_copy_region(struct pipe_context *p,
                        struct pipe_resource *dst, unsigned dst_level,
                        unsigned dstx, unsigned dsty, unsigned dstz,
                        struct pipe_resource *src, unsigned src_level,
                        const struct pipe_box *src_box)
{
   struct ph_context *ctx = ph_context(p);

   ph_zinit_resolve(ctx, ph_resource(src));
   ph_zinit_resolve(ctx, ph_resource(dst));
   if (dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER) {
      struct parhelia_resource *dr = ph_resource(dst);
      struct parhelia_resource *sr = ph_resource(src);

      if (src_box->x < 0 || src_box->width < 0 ||
          (uint64_t)src_box->x + src_box->width > src->width0 ||
          (uint64_t)dstx + src_box->width > dst->width0)
         return;
      dr->data_gen++;
      memmove((uint8_t *)dr->malloc_data + dstx,
              (uint8_t *)sr->malloc_data + src_box->x, src_box->width);
      return;
   }
   if (dst->target == PIPE_BUFFER || src->target == PIPE_BUFFER)
      return;
   /* The direct copy writes the plain 2-D order; slice 0 of a volume
    * interleaves with the others from texel row 16 on. */
   bool ok = !dstz && !src_box->z && !ph_resource(dst)->mip.slices &&
             !ph_resource(src)->mip.slices &&
             ph_copy_texture_box(ctx, dst, dst_level, dstx, dsty, src,
                                 src_level, src_box, src_box->width,
                                 src_box->height);
   /* Volume slices and anything the direct copy refuses go through the
    * transfer paths, which know every layout. */
   if (!ok) {
      util_resource_copy_region(p, dst, dst_level, dstx, dsty, dstz, src,
                                src_level, src_box);
      ok = !ctx->submit_error;
   }
   if (ph_env("PARHELIA_TRACE_COPY"))
      fprintf(stderr, "parhelia: copy %ux%u l%u -> %ux%u l%u at %d,%d,%d box %d,%d %dx%d: %s (err %d)\n",
              src->width0, src->height0, src_level, dst->width0,
              dst->height0, dst_level, dstx, dsty, dstz, src_box->x, src_box->y,
              src_box->width, src_box->height, ok ? "ok" : "FAILED",
              ctx->submit_error);
}

static void
ph_normalize_box(const struct pipe_box *in, struct pipe_box *out)
{
   *out = *in;
   if (out->width < 0) {
      out->x += out->width;
      out->width = -out->width;
   }
   if (out->height < 0) {
      out->y += out->height;
      out->height = -out->height;
   }
}

/* @box clipped to level @level of @res; false when nothing is left. */
static bool
ph_clip_box(const struct pipe_box *box, const struct pipe_resource *res,
            unsigned level, struct pipe_box *out)
{
   const int64_t w = u_minify(res->width0, level);
   const int64_t h = u_minify(res->height0, level);
   const int64_t x0 = MAX2((int64_t)box->x, 0);
   const int64_t y0 = MAX2((int64_t)box->y, 0);
   const int64_t x1 = MIN2((int64_t)box->x + box->width, w);
   const int64_t y1 = MIN2((int64_t)box->y + box->height, h);

   *out = *box;
   if (x0 >= x1 || y0 >= y1)
      return false;
   out->x = x0;
   out->y = y0;
   out->width = x1 - x0;
   out->height = y1 - y0;
   return true;
}

/* Source sample position of destination pixel @d of a @dsize span scaled
 * from a @ssize span, relative to a clipped source starting @offset texels
 * after the span. */
static float
ph_blit_coord(int64_t d, int ssize, int dsize, int64_t offset, int clipped)
{
   const double f = ((double)d + 0.5) * ssize / dsize - 0.5 + offset;

   return (float)CLAMP(f, -1.0, (double)clipped);
}

/* General blit through the CPU transfer paths: scaling with nearest or
 * bilinear filtering, colour and depth/stencil format conversion, flips,
 * write masks, scissor and whole volume slices. */
static bool
ph_blit_cpu(struct pipe_context *p, const struct pipe_blit_info *info)
{
   const enum pipe_format sf = info->src.format, df = info->dst.format;
   const bool zs = util_format_is_depth_or_stencil(sf) ||
                   util_format_is_depth_or_stencil(df);
   const struct util_format_description *sd = util_format_description(sf);
   const struct util_format_description *dd = util_format_description(df);
   const bool sz = util_format_has_depth(sd), ss = util_format_has_stencil(sd);
   const bool dz = util_format_has_depth(dd), ds = util_format_has_stencil(dd);
   unsigned mask = info->mask & (zs ? PIPE_MASK_ZS : PIPE_MASK_RGBA);
   const bool flip_x = (info->src.box.width < 0) != (info->dst.box.width < 0);
   const bool flip_y = (info->src.box.height < 0) != (info->dst.box.height < 0);
   struct pipe_transfer *st = NULL, *dt = NULL;
   struct pipe_box sb, db, scb, dcb;
   const uint8_t *src;
   uint8_t *dst;
   float *pixels = NULL, *row = NULL;
   uint32_t *zsrc = NULL, *zrow = NULL;
   uint8_t *ssrc = NULL, *srow = NULL;
   /* a compressed source is decoded a block-aligned rectangle at a time */
   const bool src_blocks = util_format_is_compressed(sf);
   struct pipe_box mb;
   bool ok = false;

   if (util_format_is_compressed(df))
      return false;
   if (zs) {
      if (!util_format_is_depth_or_stencil(sf) ||
          !util_format_is_depth_or_stencil(df))
         return false;
      /* A channel neither side holds is not part of the copy. */
      if (!sz || !dz)
         mask &= ~PIPE_MASK_Z;
      if (!ss || !ds)
         mask &= ~PIPE_MASK_S;
   }
   const bool keep = zs ? (dz && !(mask & PIPE_MASK_Z)) ||
                          (ds && !(mask & PIPE_MASK_S)) ||
                          info->scissor_enable :
                          mask != PIPE_MASK_RGBA || info->scissor_enable;

   ph_normalize_box(&info->src.box, &sb);
   ph_normalize_box(&info->dst.box, &db);
   if (!mask || !sb.width || !sb.height || !db.width || !db.height ||
       sb.depth != db.depth || sb.depth < 1)
      return !mask;
   /* GL does not clip a scaled blit to the surfaces: the boxes keep the
    * scale, only the clipped parts are mapped and written. */
   if (!ph_clip_box(&sb, info->src.resource, info->src.level, &scb))
      return true;
   if (!ph_clip_box(&db, info->dst.resource, info->dst.level, &dcb))
      return true;
   mb = scb;
   if (src_blocks) {
      const int bw = util_format_get_blockwidth(sf);
      const int bh = util_format_get_blockheight(sf);
      const int lw = u_minify(info->src.resource->width0, info->src.level);
      const int lh = u_minify(info->src.resource->height0, info->src.level);

      mb.x = scb.x / bw * bw;
      mb.y = scb.y / bh * bh;
      mb.width = MIN2((scb.x + scb.width + bw - 1) / bw * bw, lw) - mb.x;
      mb.height = MIN2((scb.y + scb.height + bh - 1) / bh * bh, lh) - mb.y;
   }
   src = p->texture_map(p, info->src.resource, info->src.level,
                        PIPE_MAP_READ, &mb, &st);
   dst = p->texture_map(p, info->dst.resource, info->dst.level,
                        PIPE_MAP_WRITE | (keep ? PIPE_MAP_READ : 0), &dcb,
                        &dt);
   if (zs) {
      zsrc = MALLOC((size_t)scb.width * scb.height * sizeof(*zsrc));
      ssrc = MALLOC((size_t)scb.width * scb.height);
      zrow = MALLOC((size_t)dcb.width * sizeof(*zrow));
      srow = MALLOC(dcb.width);
   } else {
      pixels = MALLOC((size_t)scb.width * scb.height * 4 * sizeof(float));
      row = MALLOC((size_t)dcb.width * 4 * sizeof(float));
   }
   if (!src || !dst || (zs ? !zsrc || !ssrc || !zrow || !srow :
                             !pixels || !row)) {
      if (ph_env("PARHELIA_TRACE") || ph_env("PARHELIA_DEBUG"))
         fprintf(stderr,
                 "parhelia: blit CPU fallback could not map src=%p dst=%p\n",
                 (void *)src, (void *)dst);
      goto out;
   }
   for (int l = 0; l < sb.depth; l++) {
      const uint8_t *slice = src + (size_t)l * st->layer_stride;
      uint8_t *dslice = dst + (size_t)l * dt->layer_stride;

      if (src_blocks) {
         /* the unpacker writes whole blocks, also past an edge level's size */
         const unsigned aw = align(mb.width, util_format_get_blockwidth(sf));
         const unsigned ah = align(mb.height, util_format_get_blockheight(sf));
         float *all = MALLOC((size_t)aw * ah * 4 * sizeof(float));

         if (!all)
            goto out;
         util_format_unpack_rgba_rect(sf, all, aw * 4 * sizeof(float),
                                      slice, st->stride, mb.width, mb.height);
         for (int y = 0; y < scb.height; y++)
            memcpy(pixels + (size_t)y * scb.width * 4,
                   all + ((size_t)(scb.y - mb.y + y) * aw +
                          (scb.x - mb.x)) * 4,
                   (size_t)scb.width * 4 * sizeof(float));
         FREE(all);
      }
      for (int y = 0; y < scb.height && !src_blocks; y++) {
         const uint8_t *in = slice + (size_t)y * st->stride;

         if (!zs) {
            util_format_unpack_rgba(sf, pixels + (size_t)y * scb.width * 4,
                                    in, scb.width);
            continue;
         }
         if (sz)
            util_format_unpack_z_32unorm(sf, zsrc + (size_t)y * scb.width,
                                         in, scb.width);
         if (ss)
            util_format_unpack_s_8uint(sf, ssrc + (size_t)y * scb.width,
                                       in, scb.width);
      }
      for (int y = 0; y < dcb.height; y++) {
         const int64_t ly = (int64_t)dcb.y - db.y + y;
         /* Source coordinates relative to the clipped source image. */
         const float fy = ph_blit_coord(flip_y ? db.height - 1 - ly : ly,
                                        sb.height, db.height,
                                        sb.y - scb.y, scb.height);
         uint8_t *out = dslice + (size_t)y * dt->stride;
         const int wy = dcb.y + y;

         if (zs) {
            if (keep && dz)
               util_format_unpack_z_32unorm(df, zrow, out, dcb.width);
            if (keep && ds)
               util_format_unpack_s_8uint(df, srow, out, dcb.width);
            for (int x = 0; x < dcb.width; x++) {
               const int64_t lx = (int64_t)dcb.x - db.x + x;
               const float fx = ph_blit_coord(flip_x ? db.width - 1 - lx : lx,
                                              sb.width, db.width,
                                              sb.x - scb.x, scb.width);
               const int sx = CLAMP((int)lroundf(fx), 0, scb.width - 1);
               const int sy = CLAMP((int)lroundf(fy), 0, scb.height - 1);
               const size_t i = (size_t)sy * scb.width + sx;

               if (info->scissor_enable &&
                   (dcb.x + x < info->scissor.minx ||
                    dcb.x + x >= info->scissor.maxx ||
                    wy < info->scissor.miny || wy >= info->scissor.maxy))
                  continue;
               if ((mask & PIPE_MASK_Z) &&
                   info->filter == PIPE_TEX_FILTER_LINEAR &&
                   (sb.width != db.width || sb.height != db.height)) {
                  /* Depth filters like colour (mipmap generation); stencil
                   * stays nearest. */
                  const float cx = CLAMP(fx, 0.0f, scb.width - 1.0f);
                  const float cy = CLAMP(fy, 0.0f, scb.height - 1.0f);
                  const int x0 = (int)cx, y0 = (int)cy;
                  const int x1 = MIN2(x0 + 1, scb.width - 1);
                  const int y1 = MIN2(y0 + 1, scb.height - 1);
                  const double ax = cx - x0, ay = cy - y0;
                  const double z =
                     (zsrc[(size_t)y0 * scb.width + x0] * (1 - ax) +
                      zsrc[(size_t)y0 * scb.width + x1] * ax) * (1 - ay) +
                     (zsrc[(size_t)y1 * scb.width + x0] * (1 - ax) +
                      zsrc[(size_t)y1 * scb.width + x1] * ax) * ay;

                  zrow[x] = (uint32_t)MIN2(z + 0.5, 4294967295.0);
               } else if (mask & PIPE_MASK_Z)
                  zrow[x] = zsrc[i];
               if (mask & PIPE_MASK_S)
                  srow[x] = ssrc[i];
            }
            if (dz)
               util_format_pack_z_32unorm(df, out, zrow, dcb.width);
            if (ds)
               util_format_pack_s_8uint(df, out, srow, dcb.width);
            continue;
         }
         if (keep)
            util_format_unpack_rgba(df, row, out, dcb.width);
         for (int x = 0; x < dcb.width; x++) {
            const int64_t lx = (int64_t)dcb.x - db.x + x;
            const float fx = ph_blit_coord(flip_x ? db.width - 1 - lx : lx,
                                           sb.width, db.width,
                                           sb.x - scb.x, scb.width);
            const int wx = dcb.x + x;
            float texel[4];

            if (info->scissor_enable &&
                (wx < info->scissor.minx || wx >= info->scissor.maxx ||
                 wy < info->scissor.miny || wy >= info->scissor.maxy))
               continue;
            if (info->filter == PIPE_TEX_FILTER_LINEAR &&
                (sb.width != db.width || sb.height != db.height)) {
               const float cx = CLAMP(fx, 0.0f, scb.width - 1.0f);
               const float cy = CLAMP(fy, 0.0f, scb.height - 1.0f);
               const int x0 = (int)cx, y0 = (int)cy;
               const int x1 = MIN2(x0 + 1, scb.width - 1);
               const int y1 = MIN2(y0 + 1, scb.height - 1);
               const float ax = cx - x0, ay = cy - y0;

               for (unsigned c = 0; c < 4; c++) {
                  const float *p00 = pixels + ((size_t)y0 * scb.width + x0) * 4;
                  const float *p10 = pixels + ((size_t)y0 * scb.width + x1) * 4;
                  const float *p01 = pixels + ((size_t)y1 * scb.width + x0) * 4;
                  const float *p11 = pixels + ((size_t)y1 * scb.width + x1) * 4;

                  texel[c] = (p00[c] * (1 - ax) + p10[c] * ax) * (1 - ay) +
                             (p01[c] * (1 - ax) + p11[c] * ax) * ay;
               }
            } else {
               const int sx = CLAMP((int)floorf(fx + 0.5f), 0, scb.width - 1);
               const int sy = CLAMP((int)floorf(fy + 0.5f), 0, scb.height - 1);

               memcpy(texel, pixels + ((size_t)sy * scb.width + sx) * 4,
                      sizeof(texel));
            }
            for (unsigned c = 0; c < 4; c++)
               if (mask & (PIPE_MASK_R << c))
                  row[x * 4 + c] = texel[c];
         }
         util_format_pack_rgba(df, out, row, dcb.width);
      }
   }
   ok = true;
out:
   FREE(srow);
   FREE(zrow);
   FREE(ssrc);
   FREE(zsrc);
   FREE(row);
   FREE(pixels);
   if (dt)
      p->texture_unmap(p, dt);
   if (st)
      p->texture_unmap(p, st);
   return ok;
}

static void
ph_blit(struct pipe_context *p, const struct pipe_blit_info *info)
{
   struct ph_context *ctx = ph_context(p);
   bool copied = false;

   if (!info || !info->src.resource || !info->dst.resource ||
       info->src.resource->nr_samples > 1 ||
       info->dst.resource->nr_samples > 1)
      return;
   ph_zinit_resolve(ctx, ph_resource(info->src.resource));
   ph_zinit_resolve(ctx, ph_resource(info->dst.resource));
   if (ph_env("PARHELIA_TRACE_COPY"))
      fprintf(stderr, "parhelia: blit %s t%u %d,%d,%d %dx%dx%d -> %s t%u %d,%d,%d %dx%dx%d\n",
              util_format_short_name(info->src.format),
              info->src.resource->target, info->src.box.x, info->src.box.y,
              info->src.box.z, info->src.box.width, info->src.box.height,
              info->src.box.depth, util_format_short_name(info->dst.format),
              info->dst.resource->target, info->dst.box.x, info->dst.box.y,
              info->dst.box.z, info->dst.box.width, info->dst.box.height,
              info->dst.box.depth);
   if (info->src.format == info->dst.format &&
       info->src.format == info->src.resource->format &&
       info->dst.format == info->dst.resource->format &&
       abs(info->src.box.width) == abs(info->dst.box.width) &&
       abs(info->src.box.height) == abs(info->dst.box.height) &&
       !info->scissor_enable &&
       (info->mask & PIPE_MASK_RGBA) == PIPE_MASK_RGBA &&
       !util_format_is_depth_or_stencil(info->src.format) &&
       !info->src.box.z && !info->dst.box.z)
      copied = ph_copy_texture_box(ctx, info->dst.resource, info->dst.level,
                                   info->dst.box.x, info->dst.box.y,
                                   info->src.resource, info->src.level,
                                   &info->src.box, info->dst.box.width,
                                   info->dst.box.height);
   /* The CPU path converts one slice at a time; volume blits go slice by
    * slice at their own z. */
   if (!copied && abs(info->src.box.depth) == abs(info->dst.box.depth)) {
      struct pipe_blit_info slice = *info;

      copied = true;
      for (int z = 0; copied && z < abs(info->dst.box.depth); z++) {
         slice.src.box.z = info->src.box.z +
                           (info->src.box.depth < 0 ? -1 - z : z);
         slice.dst.box.z = info->dst.box.z +
                           (info->dst.box.depth < 0 ? -1 - z : z);
         slice.src.box.depth = slice.dst.box.depth = 1;
         copied = ph_blit_cpu(p, &slice);
      }
   }
   if (!copied)
      fprintf(stderr,
              "parhelia: unsupported blit %s -> %s %dx%d -> %dx%d\n",
              util_format_short_name(info->src.format),
              util_format_short_name(info->dst.format),
              info->src.box.width, info->src.box.height,
              info->dst.box.width, info->dst.box.height);
}

/* P3D's 0x32/0x33 32-bit sampler and colour formats are little-endian
 * BGR(A/X).  Keep the GPU backing images in that byte order and convert any
 * other 8-bit RGBA byte order at the transfer boundary.  Returns the
 * conversion, or NULL when the format's bytes are stored as they are. */
struct ph_texel_order {
   uint8_t byte[4];
};

static const struct ph_texel_order *
ph_format_texel_order(enum pipe_format format, struct ph_texel_order *order)
{
   static const uint8_t identity[4] = { 0, 1, 2, 3 };

   if (!ph_rgba8_order(format, order->byte) ||
       !memcmp(order->byte, identity, sizeof(identity)))
      return NULL;
   return order;
}

static void
ph_copy_texel_to_gpu(uint8_t *dst, const uint8_t *src,
                     const struct ph_texel_order *order, unsigned cpp)
{
   if (cpp != 4 || !order) {
      memcpy(dst, src, cpp);
      return;
   }
   /* Either side may be an uncached aperture mapping: one access per texel. */
   uint8_t in[4], out[4];

   memcpy(in, src, 4);
   for (unsigned i = 0; i < 4; i++)
      out[i] = order->byte[i] == 0xff ? 0xff : in[order->byte[i]];
   memcpy(dst, out, 4);
}

/* Texture layout code 2 addresses 32x16 blocks using a power-of-two row
 * pitch.  This is independent of CREATE_RENDER's 64-pixel render-target
 * pitch, especially for the small Q3 UI and lightmap textures. */
static unsigned
ph_sampler_tiled_pitch(unsigned width)
{
   return MAX2(32u, align(width, 32));
}

static unsigned
ph_sampler_storage_pitch(struct pipe_screen *pscreen, unsigned width,
                         unsigned cpp)
{
   struct parhelia_screen *screen = ph_screen(pscreen);
   unsigned pitch = ph_sampler_tiled_pitch(width);

   if (p3d_texture_uses_native_64x16(screen->chip_id, screen->tile_layout,
                                     cpp))
      pitch = MAX2(pitch, 64u);
   return pitch;
}

/* Q3's retained vendor path deliberately changes its binary-alpha atlas from
 * RGBA client pixels into A1R5G5B5 storage for SRC_ALPHA source-over.  The
 * packed image lives in a companion object of the texture's own shape, built
 * once per content generation: repacking the texture in place and restoring
 * it for its ordinary uses drained the batch queue ~21 times per ioquake3
 * frame and re-uploaded the texture each time. */
static bool
ph_prepare_rgb5a1(struct ph_context *ctx, struct parhelia_resource *texture)
{
   struct parhelia_screen *screen;
   struct drm_parhelia_upload_render upload;
   const unsigned pitch = texture ? ph_sampler_tiled_pitch(texture->base.width0) : 0;
   const uint8_t *src;

   if (!texture || texture->base.format != PIPE_FORMAT_R8G8B8A8_UNORM ||
       !texture->map || !texture->linear_map || texture->base.last_level ||
       !texture->upload_texture)
      return false;
   if (texture->rgb5a1_handle &&
       texture->rgb5a1_generation == texture->content_generation)
      return true;
   screen = ph_screen(texture->base.screen);
   src = texture->linear_map;
   if (!texture->rgb5a1_checked ||
       texture->rgb5a1_checked_generation != texture->content_generation) {
      texture->rgb5a1_checked = true;
      texture->rgb5a1_eligible = true;
      texture->rgb5a1_checked_generation = texture->content_generation;
      for (unsigned y = 0;
           y < texture->base.height0 && texture->rgb5a1_eligible; y++)
         for (unsigned x = 0; x < texture->base.width0; x++) {
            const uint8_t *p = src +
               (size_t)y * texture->linear_stride + x * 4;
            if (p[3] != 0 && p[3] != 255) {
               texture->rgb5a1_eligible = false;
               break;
            }
         }
   }
   if (!texture->rgb5a1_eligible)
      return false;
   /* The two-byte order must fit the four-byte image's storage. */
   for (unsigned y = 0; y < texture->base.height0; y++)
      for (unsigned x = 0; x < texture->base.width0; x++)
         if (p3d_texture_tile_offset_chip(x, y, pitch, 2, screen->chip_id,
                                          texture->tile_layout) + 2 >
             texture->size)
            return false;
   if (texture->rgb5a1_handle) {
      /* An older companion may still be named by batched draws; the kernel
       * orders UPLOAD_RENDER after submitted work only. */
      if (ph_drain_batch(ctx))
         return false;
   } else {
      struct drm_parhelia_create_render create = {
         .width = texture->create_width, .height = texture->create_height,
         .cpp = texture->create_cpp, .flags = texture->create_flags,
      };

      if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, &create))
         return false;
      texture->rgb5a1_map = create.size == texture->size ?
                            align_malloc(texture->size, 64) : NULL;
      if (!texture->rgb5a1_map) {
         struct drm_gem_close close = {.handle = create.handle};

         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close);
         return false;
      }
      memset(texture->rgb5a1_map, 0, texture->size);
      texture->rgb5a1_handle = create.handle;
   }
   for (unsigned y = 0; y < texture->base.height0; y++)
      for (unsigned x = 0; x < texture->base.width0; x++) {
         const uint8_t *p = src + (size_t)y * texture->linear_stride + x * 4;
         uint16_t value = (p[3] ? 0x8000u : 0) |
                          ((uint16_t)(p[2] >> 3) << 10) |
                          ((uint16_t)(p[1] >> 3) << 5) | (p[0] >> 3);
         /* The descriptor selects the storage order for a two-byte texel, so
          * the repack must write the order this chip really reads there: the
          * generic vendor order put the high y bits of a small image where
          * the address unit never looks, drawing its top half only. */
         memcpy(texture->rgb5a1_map +
                   p3d_texture_tile_offset_chip(x, y, pitch, 2,
                                                screen->chip_id,
                                                texture->tile_layout),
                &value, sizeof(value));
      }
   upload = (struct drm_parhelia_upload_render) {
      .data = (uintptr_t)texture->rgb5a1_map,
      .size = texture->size,
      .handle = texture->rgb5a1_handle,
   };
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_UPLOAD_RENDER, &upload))
      return false;
   texture->rgb5a1_generation = texture->content_generation;
   return true;
}

static void
ph_copy_texel_from_gpu(uint8_t *dst, const uint8_t *src,
                       const struct ph_texel_order *order, unsigned cpp)
{
   if (cpp != 4 || !order) {
      memcpy(dst, src, cpp);
      return;
   }
   uint8_t in[4], out[4];

   memcpy(in, src, 4);
   memset(out, 0xff, 4);
   for (unsigned i = 0; i < 4; i++)
      if (order->byte[i] != 0xff)
         out[order->byte[i]] = in[i];
   memcpy(dst, out, 4);
}

static size_t
ph_linear_texel_offset(const struct parhelia_resource *r, unsigned level,
                       unsigned x, unsigned y, unsigned cpp)
{
   return r->linear_mip.levels ?
      ph_mip_texel_offset_cpp(&r->linear_mip, level, x, y, true, cpp) :
      (size_t)y * r->linear_stride + (size_t)x * cpp;
}

/* Texels gathered in cached memory before one wide copy to or from an
 * aperture mapping, where every access is a bus transaction. */
#define PH_SPAN_TEXELS 64

struct ph_span {
   uint8_t *base;
   size_t start;
   unsigned bytes;
   uint8_t buf[PH_SPAN_TEXELS * 4];
};

static void
ph_span_flush(struct ph_span *s)
{
   if (s->bytes)
      memcpy(s->base + s->start, s->buf, s->bytes);
   s->bytes = 0;
}

/* Queue one texel for @off, extending the span when it continues it. */
static void
ph_span_put(struct ph_span *s, size_t off, const uint8_t *src,
            const struct ph_texel_order *order, unsigned cpp)
{
   if (s->bytes && (off != s->start + s->bytes ||
                    s->bytes + cpp > sizeof(s->buf)))
      ph_span_flush(s);
   if (!s->bytes)
      s->start = off;
   ph_copy_texel_to_gpu(s->buf + s->bytes, src, order, cpp);
   s->bytes += cpp;
}

/* Move one row of a box between staging and the linear companion. */
static void
ph_linear_row(struct parhelia_resource *r, unsigned level, unsigned x0,
              unsigned y, unsigned w, uint8_t *host, bool to_gpu,
              const struct ph_texel_order *order, unsigned cpp)
{
   const size_t start = ph_linear_texel_offset(r, level, x0, y, cpp);
   uint8_t *lin = (uint8_t *)r->linear_map;
   struct ph_span span = { .base = lin };

   if (!w)
      return;
   if (ph_linear_texel_offset(r, level, x0 + w - 1, y, cpp) ==
       start + (size_t)(w - 1) * cpp) {
      if (cpp != 4 || !order) {
         if (to_gpu)
            memcpy(lin + start, host, (size_t)w * cpp);
         else
            memcpy(host, lin + start, (size_t)w * cpp);
         return;
      }
      for (unsigned x = 0; x < w; x += PH_SPAN_TEXELS) {
         const unsigned n = MIN2(w - x, PH_SPAN_TEXELS);

         if (to_gpu) {
            for (unsigned i = 0; i < n; i++)
               ph_copy_texel_to_gpu(span.buf + i * 4, host + (x + i) * 4,
                                    order, 4);
            memcpy(lin + start + (size_t)x * 4, span.buf, n * 4);
         } else {
            memcpy(span.buf, lin + start + (size_t)x * 4, n * 4);
            for (unsigned i = 0; i < n; i++)
               ph_copy_texel_from_gpu(host + (x + i) * 4, span.buf + i * 4,
                                      order, 4);
         }
      }
      return;
   }
   for (unsigned x = 0; x < w; x++) {
      const size_t off = ph_linear_texel_offset(r, level, x0 + x, y, cpp);

      if (to_gpu)
         ph_span_put(&span, off, host + (size_t)x * cpp, order, cpp);
      else
         ph_copy_texel_from_gpu(host + (size_t)x * cpp, lin + off, order,
                                cpp);
   }
   ph_span_flush(&span);
}

static bool
ph_resource_uses_sampler_layout(const struct parhelia_resource *r)
{
   return r->upload_texture ||
          ((r->base.bind & PIPE_BIND_SAMPLER_VIEW) &&
           !(r->base.bind & (PIPE_BIND_RENDER_TARGET |
                             PIPE_BIND_DISPLAY_TARGET |
                             PIPE_BIND_SCANOUT |
                             PIPE_BIND_DEPTH_STENCIL)));
}

struct ph_texel_walk {
   const struct parhelia_resource *r;
   unsigned level, layer, cpp, sampler_pitch;
   bool sampler;
};

/* Byte offset of texel (x,y) of the walk's level and layer in r->map. */
static size_t
ph_walk_texel_offset(const void *data, unsigned x, unsigned y)
{
   const struct ph_texel_walk *w = data;
   const struct parhelia_resource *r = w->r;

   if (!w->sampler)
      return p3d_tile_offset_layout_cpp(x, y, r->stride, r->tile_layout,
                                        r->cpp);
   if (r->mip.levels)
      return ph_mip_layer_texel_offset(&r->mip, w->level, w->layer, x, y,
                                       false, w->cpp);
   return p3d_texture_tile_offset_chip(x, y, w->sampler_pitch, w->cpp,
                                       ph_screen(r->base.screen)->chip_id,
                                       r->tile_layout);
}

static void
ph_texel_walk_init(struct ph_texel_walk *w, const struct parhelia_resource *r,
                   unsigned level, unsigned layer, unsigned cpp)
{
   *w = (struct ph_texel_walk){
      .r = r, .level = level, .layer = layer, .cpp = cpp,
      .sampler = ph_resource_uses_sampler_layout(r),
   };
   if (w->sampler && !r->mip.levels)
      w->sampler_pitch = ph_sampler_storage_pitch(r->base.screen,
                                                  r->base.width0, cpp);
}

/* Move the S3TC blocks of @box of @level between a block-linear staging
 * image and the tiled hardware image.  Block rows of the staging image are
 * @stride bytes apart; the hardware address comes from ph_dxt_block_offset()
 * inside the level. */
static void
ph_copy_blocks(struct parhelia_resource *r, unsigned level, void *staging,
               unsigned stride, const struct pipe_box *box, bool to_gpu)
{
   const unsigned block = r->mip.block_bytes;
   const unsigned pitch = r->mip.level[level].pitch;
   const unsigned bw = (box->width + 3) / 4, bh = (box->height + 3) / 4;

   for (unsigned j = 0; j < bh; j++)
      for (unsigned i = 0; i < bw; i++) {
         uint8_t *host = (uint8_t *)staging + (size_t)j * stride + i * block;
         uint8_t *gpu = (uint8_t *)r->map + r->mip.level[level].offset +
            ph_dxt_block_offset(pitch, box->x / 4 + i, box->y / 4 + j, block);

         if (to_gpu)
            memcpy(gpu, host, block);
         else
            memcpy(host, gpu, block);
      }
}

/* Host image of a level outside the hardware storage, or NULL. */
static uint8_t *
ph_sw_level(struct parhelia_resource *r, unsigned level)
{
   const struct pipe_resource *b = &r->base;

   if (!r->sw_level) {
      r->sw_level = CALLOC(b->last_level + 1, sizeof(*r->sw_level));
      if (!r->sw_level)
         return NULL;
   }
   if (!r->sw_level[level])
      r->sw_level[level] =
         CALLOC(util_format_get_2d_size(b->format,
                   util_format_get_stride(b->format, u_minify(b->width0, level)),
                   u_minify(b->height0, level)),
                b->target == PIPE_TEXTURE_3D ? u_minify(b->depth0, level) :
                MAX2(b->array_size, 1));
   return r->sw_level[level];
}

static void *
ph_map(struct pipe_context *p, struct pipe_resource *res, unsigned level,
       unsigned usage, const struct pipe_box *box,
       struct pipe_transfer **out, bool texture)
{
   struct ph_context *ctx = ph_context(p);
   struct parhelia_resource *r = ph_resource(res);
   struct ph_transfer *t = CALLOC_STRUCT(ph_transfer);
   /* A widened format moves its stored 32-bit texels here and is narrowed
    * into the caller's staging at the end. */
   const bool widen = ph_format_widened(res->format);
   unsigned cpp = widen ? 4 : util_format_get_blocksize(res->format);
   struct ph_texel_order order_storage;
   const struct ph_texel_order *swap_rb = widen ? NULL :
      ph_format_texel_order(res->format, &order_storage);

   ph_zinit_resolve(ctx, r);
   unsigned image_width, image_height;
   if (!t || !box) {
      FREE(t);
      return NULL;
   }
   t->base.resource = res;
   t->base.usage = usage;
   t->base.level = level;
   t->base.box = *box;
   if (!texture) {
      if (usage & PIPE_MAP_WRITE)
         r->data_gen++;
      if (usage & PIPE_MAP_PERSISTENT)
         r->data_volatile = true;
      *out = &t->base;
      return (uint8_t *)r->malloc_data + box->x;
   }
   if (level < ph_resource_mip_levels(r)) {
      image_width = r->mip.level[level].width;
      image_height = r->mip.level[level].height;
   } else {
      image_width = MAX2(res->width0 >> level, 1);
      image_height = MAX2(res->height0 >> level, 1);
   }
   /* A compressed map is block-linear: the box stays in texels while the
    * stride counts block bytes.  Gallium already requires the origin to be
    * block-aligned unless it is the image edge. */
   if (r->mip.block_bytes) {
      if (level >= ph_resource_mip_levels(r) || !r->map ||
          box->x < 0 || box->y < 0 ||
          (box->x & 3) || (box->y & 3) ||
          (unsigned)box->x + (unsigned)box->width > image_width ||
          (unsigned)box->y + (unsigned)box->height > image_height) {
         FREE(t);
         return NULL;
      }
      t->base.stride = util_format_get_stride(res->format, box->width);
      t->base.layer_stride = (uintptr_t)t->base.stride *
         util_format_get_nblocksy(res->format, box->height);
      t->staging = CALLOC(1, t->base.layer_stride);
      if (!t->staging) {
         FREE(t);
         return NULL;
      }
      if (usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) {
         ph_submit_vertices(ctx);
         if (ph_wait_async(ctx) || ctx->submit_error) {
            FREE(t->staging);
            FREE(t);
            return NULL;
         }
      }
      if ((usage & PIPE_MAP_READ) && (r->mip_defined & BITFIELD_BIT(level)))
         ph_copy_blocks(r, level, t->staging, t->base.stride, box, false);
      *out = &t->base;
      return t->staging;
   }
   /* A level the hardware storage lacks lives in host memory only. */
   if (level && level >= ph_resource_mip_levels(r) &&
       level <= res->last_level && !r->mip.block_bytes) {
      uint8_t *image = ph_sw_level(r, level);

      if (!image) {
         FREE(t);
         return NULL;
      }
      t->base.stride = util_format_get_stride(res->format, image_width);
      t->base.layer_stride = util_format_get_2d_size(res->format,
                                                     t->base.stride,
                                                     image_height);
      *out = &t->base;
      return image + (size_t)box->z * t->base.layer_stride +
             (size_t)util_format_get_nblocksy(res->format, box->y) *
                t->base.stride +
             util_format_get_stride(res->format, box->x);
   }
   /* Volume slices and cube faces are addressed by the box's z range. */
   const unsigned layers = MAX2(box->depth, 1);
   const unsigned slices = MAX2(r->mip.slices, 1u);

   if ((cpp != 4 && cpp != 2 && cpp != 1) || level > res->last_level ||
       (!r->map && !r->linear_map) || box->x < 0 || box->y < 0 ||
       box->width < 0 || box->height < 0 || box->z < 0 ||
       (unsigned)box->z + layers > slices ||
       (unsigned)box->x + (unsigned)box->width > image_width ||
       (unsigned)box->y + (unsigned)box->height > image_height) {
      FREE(t);
      return NULL;
   }
   t->base.stride = box->width * cpp;
   t->base.layer_stride = (uintptr_t)t->base.stride * box->height;
   t->staging = MALLOC(t->base.layer_stride * layers);
   if (!t->staging) {
      FREE(t);
      return NULL;
   }
   if (usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) {
      /* A transfer map is a CPU visibility and ownership boundary.  Reads
       * must see queued rendering; writes must not pass transformed vertices
       * still staged in this context or race an asynchronous request that
       * samples the old image. */
      ph_submit_vertices(ctx);
      if (ph_wait_async(ctx) || ctx->submit_error) {
         FREE(t->staging);
         FREE(t);
         return NULL;
      }
   }
   if ((usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) &&
       r->transfer_shadow && (r->mip_defined & BITFIELD_BIT(level)) &&
       ph_shadow_transfer_box(r, level, box->x, box->y, box->width,
                              box->height, false)) {
      FREE(t->staging);
      FREE(t);
      return NULL;
   }
   if ((usage & PIPE_MAP_READ) && level < ph_resource_mip_levels(r) &&
       (r->mip_defined & BITFIELD_BIT(level))) {
      /* A transfer shadow was just downloaded and is exact; the linear
       * companion only follows it after a resolve blit. */
      if (r->linear_map && !r->transfer_shadow) {
         ph_resource_resolve_linear(r);
         for (int y = 0; y < box->height; y++)
            ph_linear_row(r, level, box->x, box->y + y, box->width,
                          (uint8_t *)t->staging + y * t->base.stride, false,
                          swap_rb, cpp);
      } else {
         struct ph_swizzle sw, inv;

         if (swap_rb) {
            memcpy(sw.src, swap_rb->byte, sizeof(sw.src));
            ph_swizzle_inverse(&inv, &sw);
         }
         for (unsigned z = 0; z < layers; z++) {
            struct ph_texel_walk walk;

            ph_texel_walk_init(&walk, r, level, box->z + z, cpp);
            for (int y = 0; y < box->height; y++)
               ph_tile_row_copy((uint8_t *)r->map, r->size,
                  ph_walk_texel_offset, &walk, box->x, box->y + y,
                  box->width,
                  (uint8_t *)t->staging + z * t->base.layer_stride +
                     y * t->base.stride,
                  false, swap_rb ? &inv : NULL, cpp);
         }
      }
   } else if (usage & PIPE_MAP_READ) {
      /* A lower level outside the P690-supported backing remains a valid
       * Gallium transfer, but it cannot expose stale allocator contents. */
      memset(t->staging, 0, t->base.layer_stride * layers);
   }
   if (widen) {
      const size_t texels = (size_t)box->width * box->height * layers;
      uint8_t *narrow = MALLOC(MAX2(texels * util_format_get_blocksize(res->format), 1));

      if (!narrow) {
         FREE(t->staging);
         FREE(t);
         return NULL;
      }
      const unsigned ncpp = util_format_get_blocksize(res->format);

      for (size_t n = 0; n < texels; n++)
         for (unsigned c = 0; c < ncpp; c++)   /* R from byte 2, G from 1 */
            narrow[n * ncpp + c] = ((const uint8_t *)t->staging)[n * 4 + 2 - c];
      t->wide = t->staging;
      t->staging = narrow;
      t->base.stride = box->width * ncpp;
      t->base.layer_stride = (uintptr_t)t->base.stride * box->height;
   }
   *out = &t->base;
   return t->staging;
}

static void
ph_unmap(struct pipe_context *p, struct pipe_transfer *transfer, bool texture)
{
   struct ph_transfer *t = (struct ph_transfer *)transfer;
   struct parhelia_resource *r = ph_resource(transfer->resource);
   const bool widen = t->wide != NULL;
   unsigned cpp = widen ? 4 : util_format_get_blocksize(transfer->resource->format);
   struct ph_texel_order order_storage;
   const struct ph_texel_order *swap_rb = widen ? NULL :
      ph_format_texel_order(transfer->resource->format, &order_storage);

   if (widen) {
      const size_t texels = (size_t)transfer->box.width *
                            transfer->box.height * MAX2(transfer->box.depth, 1);
      uint8_t *wide = t->wide;

      const unsigned ncpp = util_format_get_blocksize(transfer->resource->format);
      const uint8_t *narrow = t->staging;

      /* B, G, R, A = 0, g or 0, r, 1: the sampler reads (r, g, 0, 1). */
      for (size_t n = 0; n < texels; n++) {
         wide[n * 4 + 0] = 0;
         wide[n * 4 + 1] = ncpp > 1 ? narrow[n * ncpp + 1] : 0;
         wide[n * 4 + 2] = narrow[n * ncpp];
         wide[n * 4 + 3] = 0xff;
      }
      FREE(t->staging);
      t->staging = wide;
      t->wide = NULL;
      transfer->stride = transfer->box.width * 4;
      transfer->layer_stride = (uintptr_t)transfer->stride * transfer->box.height;
   }
   if (texture && !t->staging) {
      /* A host-only level was written in place. */
      if ((transfer->usage & PIPE_MAP_WRITE) && transfer->level) {
         r->mip_defined |= BITFIELD_BIT(transfer->level);
         r->content_generation++;
      }
      FREE(t);
      return;
   }
   if (texture && r->mip.block_bytes && t->staging && r->map) {
      if (transfer->usage & PIPE_MAP_WRITE) {
         ph_copy_blocks(r, transfer->level, t->staging, transfer->stride,
                        &transfer->box, true);
         __sync_synchronize();
         r->upload_dirty |= r->upload_texture;
         r->content_generation++;
         r->mip_defined |= BITFIELD_BIT(transfer->level);
      }
      FREE(t->staging);
      FREE(t);
      return;
   }
   if (texture && t->base.level < ph_resource_mip_levels(r) && t->staging &&
       (transfer->usage & PIPE_MAP_WRITE)) {
      /* A shadowed image reaches VRAM through the shadow upload; its linear
       * companion is then refreshed by the GPU resolve blit instead of a
       * second CPU copy over the bus. */
      const bool gpu_linear = r->transfer_shadow && r->linear_handle &&
                              r->linear_map && !transfer->level;
      if (r->linear_map && !gpu_linear) {
         for (int y = 0; y < transfer->box.height; y++)
            ph_linear_row(r, transfer->level, transfer->box.x,
                          transfer->box.y + y, transfer->box.width,
                          (uint8_t *)t->staging + y * transfer->stride, true,
                          swap_rb, cpp);
      }
      if (r->map) {
         struct ph_swizzle sw;
         unsigned skipped = 0;

         if (swap_rb)
            memcpy(sw.src, swap_rb->byte, sizeof(sw.src));
         for (unsigned z = 0; z < MAX2(transfer->box.depth, 1); z++) {
            struct ph_texel_walk walk;

            ph_texel_walk_init(&walk, r, transfer->level,
                               transfer->box.z + z, cpp);
            for (int y = 0; y < transfer->box.height; y++)
               skipped += ph_tile_row_copy((uint8_t *)r->map, r->size,
                  ph_walk_texel_offset, &walk, transfer->box.x,
                  transfer->box.y + y, transfer->box.width,
                  (uint8_t *)t->staging + z * transfer->layer_stride +
                     y * transfer->stride,
                  true, swap_rb ? &sw : NULL, cpp);
         }
         /* A layout that addresses past the storage would corrupt the
          * heap; such texels were refused. */
         if (skipped) {
            static bool said;

            if (!said) {
               said = true;
               fprintf(stderr,
                       "parhelia: %u texels of level %u are outside the %zu-byte storage of %ux%u\n",
                       skipped, transfer->level, (size_t)r->size,
                       r->base.width0, r->base.height0);
            }
         }
      }
      /* Both representations were updated when a presentation/sampler
       * backing exists; otherwise the tiled render image is authoritative.
       */
      r->tiled_dirty = r->map && !r->linear_map;
      r->linear_dirty = r->linear_map && !r->map;
      r->upload_dirty |= r->upload_texture;
      r->content_generation++;
      r->mip_defined |= BITFIELD_BIT(transfer->level);
      if (r->transfer_shadow) {
         int ret = ph_shadow_transfer_box(r, transfer->level,
                                          transfer->box.x, transfer->box.y,
                                          transfer->box.width,
                                          transfer->box.height, true);

         if (ret)
            ph_context(p)->submit_error = ret;
         else {
            r->tiled_dirty = gpu_linear;
            r->linear_dirty = false;
         }
      }
      /* The upload is deferred to the first draw that samples the texture:
       * uploading the whole BO per level made loading a mip chain cost one
       * full-object aperture transfer per level. */
   }
   FREE(t->staging);
   FREE(t);
}
static void *ph_buffer_map(struct pipe_context *p, struct pipe_resource *r,
                           unsigned l, unsigned u, const struct pipe_box *b,
                           struct pipe_transfer **t)
{ return ph_map(p, r, l, u, b, t, false); }
/* A decoded S3TC image's transfer: its host blocks of the box. */
static void *
ph_map_blocks(struct parhelia_resource *r, unsigned level, unsigned usage,
              const struct pipe_box *box, struct pipe_transfer **out)
{
   const enum pipe_format format = r->base.format;
   const unsigned block = util_format_get_blocksize(format);
   const unsigned layers = MAX2(r->base.array_size, r->base.depth0);
   const unsigned level_stride =
      util_format_get_stride(format, u_minify(r->base.width0, level));
   struct ph_transfer *t;

   if (box->x < 0 || box->y < 0 || box->z < 0 || (box->x & 3) ||
       (box->y & 3) || box->z + box->depth > (int)layers || box->depth < 1 ||
       !(t = CALLOC_STRUCT(ph_transfer)))
      return NULL;
   t->base.resource = NULL;
   pipe_resource_reference(&t->base.resource, &r->base);
   t->base.level = level;
   t->base.usage = usage;
   t->base.box = *box;
   t->base.stride = util_format_get_stride(format, box->width);
   t->base.layer_stride = (uintptr_t)t->base.stride *
                          util_format_get_nblocksy(format, box->height);
   t->blocks = true;
   t->staging = CALLOC(box->depth, t->base.layer_stride);
   if (!t->staging) {
      pipe_resource_reference(&t->base.resource, NULL);
      FREE(t);
      return NULL;
   }
   for (int z = 0; (usage & PIPE_MAP_READ) && z < box->depth; z++) {
      const uint8_t *src = r->blocks[level * layers + box->z + z];

      for (unsigned j = 0; src && j < util_format_get_nblocksy(format,
                                                               box->height);
           j++)
         memcpy((uint8_t *)t->staging + z * t->base.layer_stride +
                   j * t->base.stride,
                src + (size_t)(box->y / 4 + j) * level_stride +
                   box->x / 4 * block,
                t->base.stride);
   }
   *out = &t->base;
   return t->staging;
}

/* Store the written blocks and decode them into the B8G8R8A8 twin. */
static void
ph_unmap_blocks(struct pipe_context *p, struct ph_transfer *t)
{
   struct parhelia_resource *r = ph_resource(t->base.resource);
   const enum pipe_format format = r->base.format;
   const struct pipe_box *box = &t->base.box;
   const unsigned level = t->base.level;
   const unsigned layers = MAX2(r->base.array_size, r->base.depth0);
   const unsigned lw = u_minify(r->base.width0, level);
   const unsigned lh = u_minify(r->base.height0, level);
   const unsigned level_stride = util_format_get_stride(format, lw);
   const unsigned rows = util_format_get_nblocksy(format, box->height);
   /* the unpacker writes whole blocks, also past an edge level's size */
   const unsigned aw = align(box->width, 4), ah = rows * 4;
   uint8_t *texels = (t->base.usage & PIPE_MAP_WRITE) ?
                     MALLOC((size_t)aw * ah * 4) : NULL;

   for (int z = 0; texels && z < box->depth; z++) {
      uint8_t **level_blocks = &r->blocks[level * layers + box->z + z];
      const uint8_t *src = (const uint8_t *)t->staging +
                           z * t->base.layer_stride;
      struct pipe_box dst = { .x = box->x, .y = box->y, .z = box->z + z,
                              .width = MIN2(box->width, (int)lw - box->x),
                              .height = MIN2(box->height, (int)lh - box->y),
                              .depth = 1 };

      if (!*level_blocks)
         *level_blocks = CALLOC(util_format_get_nblocksy(format, lh),
                                level_stride);
      if (!*level_blocks)
         break;
      for (unsigned j = 0; j < rows; j++)
         memcpy(*level_blocks + (size_t)(box->y / 4 + j) * level_stride +
                   box->x / 4 * util_format_get_blocksize(format),
                src + j * t->base.stride, t->base.stride);
      util_format_unpack_rgba_8unorm_rect(format, texels, aw * 4, src,
                                          t->base.stride, box->width,
                                          box->height);
      /* R, G, B, A to the twin's B, G, R, A */
      for (size_t n = 0; n < (size_t)aw * ah; n++) {
         const uint8_t red = texels[n * 4];

         texels[n * 4] = texels[n * 4 + 2];
         texels[n * 4 + 2] = red;
      }
      if (dst.width > 0 && dst.height > 0)
         p->texture_subdata(p, r->decoded, level, PIPE_MAP_WRITE, &dst,
                            texels, aw * 4, 0);
   }
   FREE(texels);
   FREE(t->staging);
   pipe_resource_reference(&t->base.resource, NULL);
   FREE(t);
}

static void *ph_texture_map(struct pipe_context *p, struct pipe_resource *r,
                            unsigned l, unsigned u, const struct pipe_box *b,
                            struct pipe_transfer **t)
{
   if (ph_resource(r)->decoded)
      return ph_map_blocks(ph_resource(r), l, u, b, t);
   return ph_map(p, r, l, u, b, t, true);
}
static void ph_buffer_unmap(struct pipe_context *p, struct pipe_transfer *t)
{
   if (t->usage & PIPE_MAP_WRITE)
      ph_resource(t->resource)->data_gen++;
   ph_unmap(p, t, false);
}
static void ph_texture_unmap(struct pipe_context *p, struct pipe_transfer *t)
{
   if (((struct ph_transfer *)t)->blocks)
      ph_unmap_blocks(p, (struct ph_transfer *)t);
   else
      ph_unmap(p, t, true);
}

static void
ph_destroy_context(struct pipe_context *p)
{
   struct ph_context *ctx = ph_context(p);
   ph_submit_vertices(ctx);
   ph_wait_async(ctx);
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
      pipe_sampler_view_reference(&ctx->bound_views[i], NULL);
   }
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++)
      pipe_resource_reference(&ctx->constants[i], NULL);
   ph_proxy_release(ctx);
   pipe_sampler_view_reference(&ctx->stipple_view, NULL);
   pipe_resource_reference(&ctx->stipple_tex, NULL);
   if (ctx->stipple_sampler)
      p->delete_sampler_state(p, ctx->stipple_sampler);
   for (unsigned i = 0; i < ctx->coverage_count; i++) {
      pipe_sampler_view_reference(&ctx->coverage[i].view, NULL);
      pipe_resource_reference(&ctx->coverage[i].tex, NULL);
   }
   free(ctx->coverage);
   if (ctx->coverage_sampler)
      p->delete_sampler_state(p, ctx->coverage_sampler);
   for (unsigned i = 0; i < ctx->ramp_tex_count; i++) {
      pipe_sampler_view_reference(&ctx->ramp_tex[i].view, NULL);
      pipe_resource_reference(&ctx->ramp_tex[i].tex, NULL);
   }
   free(ctx->ramp_tex);
   if (ctx->ramp_sampler)
      p->delete_sampler_state(p, ctx->ramp_sampler);
   for (unsigned i = 0; i < ctx->lookup_tex_count; i++) {
      pipe_sampler_view_reference(&ctx->lookup_tex[i].view, NULL);
      pipe_resource_reference(&ctx->lookup_tex[i].tex, NULL);
      free(ctx->lookup_tex[i].key);
   }
   free(ctx->lookup_tex);
   if (ctx->lookup_sampler)
      p->delete_sampler_state(p, ctx->lookup_sampler);
   util_unreference_framebuffer_state(&ctx->fb_api);
   util_unreference_framebuffer_state(&ctx->fb);
   pipe_resource_reference(&ctx->colour_stub, NULL);
   if (p->stream_uploader)
      u_upload_destroy(p->stream_uploader);
   ph_destroy_vertex_ring(ctx);
   if (ctx->tcl_cache)
      translate_cache_destroy(ctx->tcl_cache);
   free(ctx->tcl_elts);
   free(ctx->tcl_clip_copy);
   free(ctx->deindex);
   free(ctx->tcl_seq);
   FREE(ctx->repeat_batch);
   FREE(ctx->pending_vertex_store);
   parhelia_state_cache_destroy(ctx->state_cache);
   free(ctx->vs_const);
   free(ctx->vs_const_pending);
   free(ctx->fs_const_snap);
   if (ctx->quant_stage)
      ctx->quant_stage->destroy(ctx->quant_stage);
   if (ctx->zpix_stage)
      ctx->zpix_stage->destroy(ctx->zpix_stage);
   draw_destroy(ctx->draw);
   FREE(ctx);
}

struct pipe_context *
ph_context_create(struct pipe_screen *screen, void *priv, unsigned flags)
{
   struct ph_context *ctx = CALLOC_STRUCT(ph_context);
   const char *native_alpha_mode = getenv("PARHELIA_NATIVE_SAMPLED_ALPHA");
   struct drm_parhelia_get_param warp = {
      .param = PARHELIA_PARAM_WARP_PASSTHROUGH,
   };
   struct drm_parhelia_get_param textured = {
      .param = PARHELIA_PARAM_WARP_TEXTURED,
   };
   struct drm_parhelia_get_param multitex = {
      .param = PARHELIA_PARAM_WARP_TEXTURED_MULTITEX,
   };
   struct drm_parhelia_get_param replace = {
      .param = PARHELIA_PARAM_WARP_REPLACE,
   };
   struct drm_parhelia_get_param transform = {
      .param = PARHELIA_PARAM_WARP_TRANSFORM,
   };
   struct drm_parhelia_get_param transform_textured = {
      .param = PARHELIA_PARAM_WARP_TRANSFORM_TEXTURED,
   };
   struct drm_parhelia_get_param transform_multitex = {
      .param = PARHELIA_PARAM_WARP_TRANSFORM_MULTITEX,
   };
   struct drm_parhelia_get_param constant_loader = {
      .param = PARHELIA_PARAM_WARP_CONSTANT_LOADER,
   };
   struct drm_parhelia_get_param program_instructions = {
      .param = PARHELIA_PARAM_WARP_PROGRAM_INSTRUCTIONS,
   };
   struct drm_parhelia_get_param program_slots = {
      .param = PARHELIA_PARAM_WARP_PROGRAM_SLOTS,
   };
   struct draw_stage *stage;
   (void)flags;
   if (!ctx)
      return NULL;
   ctx->vertices = ctx->vertex_store;
   ctx->pending_vertex_store = MALLOC(PH_BATCH_VERTEX_DWORDS * sizeof(uint32_t));
   if (!ctx->pending_vertex_store) {
      FREE(ctx);
      return NULL;
   }
   ctx->pending_vertices = ctx->pending_vertex_store;
   ctx->pending_ring_slot = -1;
   ctx->state_cache = parhelia_state_cache_create();
   /* All zero, like the state tracker's own initial copy: it only calls
    * set_scissor_states for a rectangle that differs from that copy. */
   ctx->base.screen = screen;
   ctx->base.priv = priv;
   if (native_alpha_mode) {
      if (!strcmp(native_alpha_mode, "cutout"))
         ctx->native_sampled_alpha_modes = 1;
      else if (!strcmp(native_alpha_mode, "blend"))
         ctx->native_sampled_alpha_modes = 2;
      else if (!strcmp(native_alpha_mode, "all"))
         ctx->native_sampled_alpha_modes = 3;
   }
   parhelia_batch_negotiate_limits(ph_screen(screen)->kernel_max_packets,
                                   false, &ctx->batch_limits);
   if (ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &warp) ||
       !warp.value || warp.value > UINT32_MAX)
      goto fail;
   if (ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &textured) ||
       !textured.value || textured.value > UINT32_MAX ||
       ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &replace) ||
       !replace.value || replace.value > UINT32_MAX)
      goto fail;
   ctx->warp_passthrough = warp.value;
   ctx->warp_textured = textured.value;
   ctx->warp_replace = replace.value;
   /* Hardware transform needs the untextured program, its textured variant
    * and the constant loader; an older kernel simply has none of them. */
   if (!ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &transform) &&
       !ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM,
              &transform_textured) &&
       !ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM,
              &constant_loader) &&
       transform.value && transform_textured.value && constant_loader.value) {
      ctx->warp_transform = transform.value;
      ctx->warp_transform_textured = transform_textured.value;
      ctx->warp_constant_loader = constant_loader.value;
      if (!ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM,
                 &transform_multitex))
         ctx->warp_transform_multitex = transform_multitex.value;
      ctx->tcl_available = debug_get_bool_option("PARHELIA_HW_TCL", true);
   }
   ctx->tcl_forced_off = ph_env("PARHELIA_NO_HW_TCL");
   /* A vertex shader the WARP compiler accepts runs from an uploaded program
    * slot.  It needs the same constant loader the fixed-function transform
    * uses, plus the kernel's WARP_PROGRAM upload path. */
   if (ctx->tcl_available &&
       !ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM,
              &program_instructions) &&
       !ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM,
              &program_slots) &&
       program_instructions.value && program_slots.value) {
      ctx->warp_program_instructions = program_instructions.value;
      ctx->warp_program_slots = program_slots.value;
      /* Default off until the GL comparison has run on hardware: the
       * semantics of min, the two scalar divide opcodes and the secondary
       * fract/floor are read out of a vendor binary, not executed. */
      /* On by default since the GL comparison this route was gated behind
       * runs clean on hardware: every scene of parhelia-gl-vs-test matches
       * the software vertex pipeline pixel for pixel, repeatedly, including
       * the fixed-function lighting shader.  PARHELIA_HW_VS=0 restores the
       * software path. */
      ctx->tcl_programs =
         debug_get_bool_option("PARHELIA_HW_VS", true);
   }
   /* The two-set transform was thought to hang the engine; with a reboot
    * between program edits (the WARP instruction cache serves an already
    * executed entry) the 2x2 experiment showed it renders correctly and
    * never hangs, at transform_multitex_ops 0 and 2. */
   ctx->tcl_multitexture =
      debug_get_bool_option("PARHELIA_HW_TCL_MULTITEX", true);
   /* --packed-colour-probe: the P690 fetch unpacks a D3DCOLOR dword
    * exactly; the original family is not measured yet. */
   ctx->packed_colour_fetch =
      ph_lx_family(ph_screen(screen)->chip_id) &&
      debug_get_bool_option("PARHELIA_PACKED_COLOUR", true);
   ctx->drop_repeats = debug_get_bool_option("PARHELIA_DROP_REPEATS", false);
   ctx->batch_calls = -1;
   ctx->pending_call_bound = 2;
   ctx->viewport.scale[0] = ctx->viewport.scale[1] = 1.0f;
   ctx->async_submit = ph_screen(screen)->async_submit;
   ctx->stats_sync = getenv("PARHELIA_STATS") &&
                     !strcmp(getenv("PARHELIA_STATS"), "sync");
   ctx->stats = ctx->stats_sync ||
                debug_get_bool_option("PARHELIA_STATS", false);
   ctx->pack_adjacent_packets = ph_screen(screen)->pack_adjacent_packets;
   ctx->aggregate_state_batches =
      ph_screen(screen)->stateful_submit &&
      ph_env("PARHELIA_BATCH_DIFFERENT_STATE");
   /* One terminal EOB/DWGSYNC per ordered state stream instead of one per
    * state: 9% in the ioquake3 timedemo. */
   ctx->aggregate_state_stream =
      debug_get_bool_option("PARHELIA_BATCH_STATE_STREAM", true);
   ctx->aggregate_batches = ctx->aggregate_state_stream ||
                            ctx->aggregate_state_batches ||
                            ph_env("PARHELIA_BATCH_AGGREGATE");
   /* One packet per primitive until the depth-interlock comparison of
    * overlapping primitives inside one packet has run on this silicon.
    * PARHELIA_PRIMS_PER_PACKET selects the run length; the vendor's own
    * packets hold 24 vertices, i.e. eight triangles. */
   /* Two is measured stable over repeated timedemo runs and every GL test
    * (+6% at 24 bpp, +2.5% at 16 bpp, half the draw packets); four and eight
    * render correctly in the probes but hang ioquake3 in its level load. */
   /* Two triangles per packet is the largest shape that has never produced
    * the P690's probabilistic command-fetcher stall, over every probe run,
    * every GL test and repeated timedemos; three and more wedge the engine
    * within a few thousand packets and need a machine reboot to recover.
    * Raising it buys 0.1-0.2 fps, which is inside run-to-run noise, so the
    * knob exists for experiments and the shipped value stays at the proven
    * shape.  parhelia-p690-multi-primitive-packet-hang.md carries the bisect,
    * the two vendor candidates that were tested and refuted, and what is
    * still unexplained. */
   /* The original family takes the vendor's eight triangles per packet:
    * its captures carry 24-vertex packets throughout
    * (parhelia-p690-multi-primitive-packets-re.md section 5). */
   const bool lx = ph_lx_family(ph_screen(screen)->chip_id);

   ph_unorm8_bits_init();
   ctx->prims_per_packet =
      debug_get_num_option("PARHELIA_PRIMS_PER_PACKET", lx ? 2 : 8);
   if (ctx->prims_per_packet < 1)
      ctx->prims_per_packet = 1;
   if (lx && ctx->prims_per_packet > 2 &&
       !ph_env("PARHELIA_PRIMS_PER_PACKET_UNSAFE")) {
      fprintf(stderr,
              "parhelia: %u primitives per packet wedges this chip; using 2. "
              "Set PARHELIA_PRIMS_PER_PACKET_UNSAFE=1 to measure it anyway.\n",
              ctx->prims_per_packet);
      ctx->prims_per_packet = 2;
   }
   if (ctx->aggregate_state_batches && ph_env("PARHELIA_TRACE_BATCH"))
      fprintf(stderr,
              "parhelia: preserving per-state EOB/DWGSYNC boundaries inside aggregate submits\n");
   if (ctx->aggregate_state_stream && ph_env("PARHELIA_TRACE_BATCH"))
      fprintf(stderr,
              "parhelia: complete ordered state streams share one terminal EOB/DWGSYNC\n");
   /* Older kernels remain usable for the single-texture renderer.  A bound
    * second unit is rejected at submit time until the matching kernel WARP
    * program is installed. */
   if (!ioctl(ph_screen(screen)->fd, DRM_IOCTL_PARHELIA_GET_PARAM, &multitex) &&
       multitex.value && multitex.value <= UINT32_MAX)
      ctx->warp_textured_multitex = multitex.value;
   ctx->p690_multitexture =
      ph_screen(screen)->chip_id == 0x2539 && ctx->warp_textured_multitex &&
      debug_get_bool_option("PARHELIA_P690_MULTITEXTURE", true);
   if (ctx->p690_multitexture && ph_env("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: P690 one-pass two-texture MODULATE enabled\n");
   ctx->draw = draw_create(&ctx->base);
   if (!ctx->draw)
      goto fail;
   draw_set_constant_buffer_stride(ctx->draw, sizeof(float));
   stage = ph_create_stage(ctx);
   if (!stage)
      goto fail;
   draw_set_rasterize_stage(ctx->draw, stage);
   ctx->hw_stage = stage;
   /* trunc(d * 2^n), measured on the P690 and the 0528 (--zcode-probe) */
   ctx->z_grid = ph_screen(screen)->chip_id == 0x2539 ||
                 ph_original_family(ph_screen(screen)->chip_id);
   ctx->logicop_grid = debug_get_bool_option("PARHELIA_LOGICOP_GRID",
                                             ctx->z_grid);
   ctx->kill_alpha = debug_get_bool_option("PARHELIA_KILL_ALPHA", ctx->z_grid);
   ctx->colour_fs.uses_color = true;
   ctx->colour_fs.texenv = P3D_TEXENV_MODULATE;
   ctx->colour_fs.serial_op = PH_SERIAL_UNSUPPORTED;
   /* Quad-rasterized (sprite) points become textured quads in the draw
    * module, which also generates their sprite coordinates. */
   draw_wide_point_sprites(ctx->draw, true);
   /* Every line reaches ph_stage_line, which knows its clipped ends. */
   draw_wide_line_threshold(ctx->draw, 2048.0f);

   ctx->base.destroy = ph_destroy_context;
   ph_domain_reset(ctx);
   ctx->base.draw_vbo = ph_draw_vbo;
   ctx->base.flush = ph_flush;
   ctx->base.flush_resource = ph_flush_resource;
   ctx->base.resource_copy_region = ph_resource_copy_region;
   ctx->base.blit = ph_blit;
   ctx->base.clear = ph_clear;
   ctx->base.clear_render_target = ph_clear_render_target;
   ctx->base.clear_depth_stencil = ph_clear_depth_stencil;
   ctx->base.create_blend_state = ph_create_blend;
   ctx->base.bind_blend_state = ph_bind_blend;
   ctx->base.delete_blend_state = ph_delete_state;
   ctx->base.create_depth_stencil_alpha_state = ph_create_dsa;
   ctx->base.bind_depth_stencil_alpha_state = ph_bind_dsa;
   ctx->base.delete_depth_stencil_alpha_state = ph_delete_state;
   ctx->base.create_rasterizer_state = ph_create_rasterizer;
   ctx->base.bind_rasterizer_state = ph_bind_rasterizer;
   ctx->base.delete_rasterizer_state = ph_delete_state;
   ctx->base.create_sampler_state = ph_create_sampler;
   ctx->base.bind_sampler_states = ph_bind_samplers;
   ctx->base.delete_sampler_state = ph_delete_state;
   ctx->base.create_sampler_view = ph_create_sampler_view;
   ctx->base.sampler_view_destroy = ph_sampler_view_destroy;
   ctx->base.set_sampler_views = ph_set_sampler_views;
   ctx->base.set_shader_buffers = ph_set_shader_buffers;
   ctx->base.set_shader_images = ph_set_shader_images;
   ctx->base.create_vs_state = ph_create_vs;
   ctx->base.bind_vs_state = ph_bind_vs;
   ctx->base.delete_vs_state = ph_delete_vs;
   ctx->base.create_fs_state = ph_create_fs;
   ctx->base.bind_fs_state = ph_bind_fs;
   ctx->base.delete_fs_state = ph_delete_fs;
   ctx->base.create_vertex_elements_state = ph_create_velems;
   ctx->base.bind_vertex_elements_state = ph_bind_velems;
   ctx->base.delete_vertex_elements_state = ph_delete_state;
   ctx->base.set_vertex_buffers = ph_set_vertex_buffers;
   ctx->base.set_constant_buffer = ph_set_constant_buffer;
   ctx->base.set_framebuffer_state = ph_set_framebuffer;
   ctx->base.set_viewport_states = ph_set_viewport;
   ctx->base.set_clip_state = ph_set_clip;
   ctx->base.set_scissor_states = ph_set_scissor;
   ctx->base.set_blend_color = ph_set_blend_color;
   ctx->base.set_stencil_ref = ph_set_stencil_ref;
   ctx->base.set_sample_mask = ph_noop_sample_mask;
   ctx->base.set_polygon_stipple = ph_set_polygon_stipple;
   ctx->base.buffer_map = ph_buffer_map;
   ctx->base.texture_map = ph_texture_map;
   ctx->base.buffer_unmap = ph_buffer_unmap;
   ctx->base.texture_unmap = ph_texture_unmap;
   ctx->base.transfer_flush_region = u_default_transfer_flush_region;
   ctx->base.clear_buffer = u_default_clear_buffer;
   ctx->base.buffer_subdata = u_default_buffer_subdata;
   ctx->base.texture_subdata = u_default_texture_subdata;
   ctx->base.set_debug_callback = u_default_set_debug_callback;
   ctx->base.resource_release = u_default_resource_release;
   ctx->base.sampler_view_release = u_default_sampler_view_release;
   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   ctx->base.const_uploader = ctx->base.stream_uploader;
	if (!ctx->base.stream_uploader)
	   goto fail;
	ph_create_vertex_ring(ctx);
	ph_prime_warp(ctx);
	ph_stage_at_ring_tail(ctx);
   if (ctx->aggregate_batches && ph_env("PARHELIA_TRACE_BATCH"))
      fprintf(stderr,
              "parhelia: aggregation limits commands=%u vertices=%u packets=%u relocs=%u kernel_packets=%u dma_ring=%u\n",
              ctx->batch_limits.command_dwords,
              ctx->batch_limits.vertex_dwords,
              ctx->batch_limits.packets, ctx->batch_limits.relocs,
              ph_screen(screen)->kernel_max_packets,
              ctx->vram_vertex_enabled);
   return &ctx->base;
fail:
	ph_destroy_vertex_ring(ctx);
   if (ctx->draw)
      draw_destroy(ctx->draw);
   FREE(ctx->pending_vertex_store);
   parhelia_state_cache_destroy(ctx->state_cache);
   FREE(ctx);
   return NULL;
}
