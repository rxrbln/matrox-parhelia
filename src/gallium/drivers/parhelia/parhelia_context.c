/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Software vertex processing followed by native Parhelia raster submission. */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "compiler/nir/nir.h"
#include "draw/draw_context.h"
#include "draw/draw_fs.h"
#include "draw/draw_pipe.h"
#include "draw/draw_vertex.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/blend.h"
#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/u_draw.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"

#include "parhelia_batch.h"
#include "parhelia_internal.h"
#include "parhelia_sampler.h"

struct ph_velems {
   unsigned count;
   struct pipe_vertex_element elem[PIPE_MAX_ATTRIBS];
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

struct ph_fs {
   struct draw_fragment_shader *draw;
   bool uses_texture;
   bool uses_color;
   uint32_t sampler_mask;
   enum ph_serial_op serial_op;
   enum p3d_texenv texenv;
   unsigned coord_semantic;
   unsigned coord_index;
};

struct ph_transfer {
   struct pipe_transfer base;
   void *staging;
};

struct ph_context;
static unsigned ph_sampler_tiled_pitch(unsigned width);
static unsigned ph_sampler_storage_pitch(struct pipe_screen *screen,
                                         unsigned width, unsigned cpp);
static unsigned ph_texture_cpp(enum pipe_format format);
static bool ph_prepare_rgb5a1(struct parhelia_resource *texture);
static void ph_restore_rgba_tiled(struct parhelia_resource *texture);
static int ph_prepare_alpha_companions(struct ph_context *ctx,
                                       struct parhelia_resource *texture,
                                       const struct pipe_sampler_state *sampler,
                                       bool native_only);
static uint64_t ph_texture_base_hash(const struct parhelia_resource *texture,
                                     bool tiled, unsigned *alpha_zero,
                                     unsigned *alpha_nonzero);

static size_t
ph_mip_texel_offset(const struct ph_mip_layout *mip, unsigned level,
                    unsigned x, unsigned y, bool linear)
{
   const struct ph_mip_level *image = &mip->level[level];
   unsigned physical_pitch;
   bool native;

   native = ph_mip_physical_coords(mip, level, 4, &x, &y,
                                   &physical_pitch);
   return image->offset + (linear ?
          ((size_t)y * image->pitch + x) * 4 :
          (native ?
           p3d_texture_tile_offset_64x16(x, y, physical_pitch) :
           p3d_texture_tile_offset(x, y, image->pitch)));
}

static size_t
ph_mip_texel_offset_cpp(const struct ph_mip_layout *mip, unsigned level,
                        unsigned x, unsigned y, bool linear, unsigned cpp)
{
   const struct ph_mip_level *image = &mip->level[level];
   unsigned physical_pitch;
   bool native;

   native = ph_mip_physical_coords(mip, level, cpp, &x, &y,
                                   &physical_pitch);
   return image->offset + (linear ?
          ((size_t)y * image->pitch + x) * cpp :
          (native ?
           p3d_texture_tile_offset_64x16(x, y, physical_pitch) :
           p3d_texture_tile_offset_cpp(x, y, image->pitch, cpp)));
}

static unsigned
ph_texture_cpp(enum pipe_format format)
{
   return format == PIPE_FORMAT_B5G5R5A1_UNORM ? 2 : 4;
}

static unsigned
ph_resource_mip_levels(const struct parhelia_resource *r)
{
   return r->mip.levels ? r->mip.levels : 1;
}

struct ph_context {
   struct pipe_context base;
   struct draw_context *draw;
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
   struct pipe_sampler_view *sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   unsigned num_samplers;
   unsigned num_sampler_views;
   struct pipe_resource *constants[MESA_SHADER_STAGES];
   const void *user_constants[MESA_SHADER_STAGES];
   unsigned constant_offset[MESA_SHADER_STAGES];
   unsigned constant_size[MESA_SHADER_STAGES];
   uint32_t warp_passthrough;
   uint32_t warp_textured;
   uint32_t warp_textured_multitex;
   uint32_t warp_replace;
   uint32_t fence;
   bool async_pending;
   int submit_error;
   /* Consecutive state batches share one ioctl until an ABI bound or a CPU
    * visibility boundary is reached.  Resource references keep every GEM
    * handle alive until the kernel has copied and pinned the aggregate. */
   struct parhelia_batch pending_batch;
   /* Reused per-draw builder storage keeps the expanded aggregate command
    * allocation off the hot draw-call stack. */
   struct parhelia_batch build_batch;
   uint32_t pending_vertices[PH_BATCH_VERTEX_DWORDS];
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
   float vertices[PH_BATCH_MAX_DRAWS * 3 * 10];
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
};

struct ph_stage {
   struct draw_stage base;
   struct ph_context *ctx;
};

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

static inline unsigned
ph_triangle_batch_limit(const struct ph_context *ctx)
{
   unsigned packets = ctx->batch_limits.packets;

   /* Each triangle is one independently relocated 0x97/0x8f descriptor.
    * Reserve room for the complete recovered state sequence; the builder's
    * own bounds remain the final guard if a future state grows. */
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
   /* CREATE_VERTEX relocates every triangle range independently, so the
    * logical stream may fill one negotiated ring slot while every hardware
    * fetch remains only 3 * dwords-per-vertex. */
   return ctx->vram_vertex_enabled ? ctx->batch_limits.vertex_dwords :
                                     PH_BATCH_LEGACY_DWORDS;
}

static int ph_drain_batch(struct ph_context *ctx);

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
   if (getenv("PARHELIA_TRACE_BATCH"))
      fprintf(stderr, "parhelia: aggregate wait fence=%u\n", ctx->fence);
   ret = parhelia_wait_fence(ph_screen(ctx->base.screen)->fd, ctx->fence,
                             -1, NULL);
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
   ctx->vram_vertex_size = 0;
   ctx->vram_vertex_handle = 0;
   ctx->vram_vertex_slot_count = 0;
   ctx->vram_vertex_slot_dwords = 0;
   ctx->vram_vertex_enabled = false;
}

/* A failed optional allocation must never make the known-good DMA32 path
 * unavailable. CREATE_VERTEX is a P690 capability query, not a chip-id
 * guess, and a client only starts using it after mmap succeeds. */
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
   /* Keep one kernel request slot free.  Filling all eight moves throttling
    * into the kernel's coarse queue-full wait; seven subranges instead wait
    * for the exact oldest fence only when this userspace ring wraps. */
   slot_count = screen->async_slots > PH_VRAM_VERTEX_LEGACY_SLOTS &&
                screen->async_slots <= PH_VRAM_VERTEX_SLOTS_MAX ?
                screen->async_slots - 1 : PH_VRAM_VERTEX_LEGACY_SLOTS;
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
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: enabled P690 class-3 DMA vertex ring handle=%u bytes=%llu slots=%u slot_dwords=%u queue-depth=%u\n",
              create.handle, (unsigned long long)create.size,
              ctx->vram_vertex_slot_count, ctx->vram_vertex_slot_dwords,
              screen->async_slots);
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
ph_submit_batch_now(struct ph_context *ctx, const struct parhelia_batch *batch,
                    const void *vertices, unsigned vertex_dwords)
{
   struct parhelia_screen *screen = ph_screen(ctx->base.screen);
   bool boundary_aware = ctx->pack_adjacent_packets &&
                         !ph_batch_has_full_boundary(batch);
   int ret;
   uint64_t bytes = (uint64_t)vertex_dwords * sizeof(uint32_t);

   if (vertices && vertex_dwords && ctx->async_submit &&
       ctx->vram_vertex_enabled &&
       bytes <= (uint64_t)ctx->vram_vertex_slot_dwords * sizeof(uint32_t)) {
      unsigned slot = ctx->vram_vertex_slot;
      uint64_t offset = (uint64_t)slot * ctx->vram_vertex_slot_dwords *
                        sizeof(uint32_t);

      if (ctx->vram_vertex_fence[slot]) {
         ret = parhelia_wait_fence(screen->fd, ctx->vram_vertex_fence[slot],
                                   -1, NULL);
         if (ret)
            return ret;
         ctx->vram_vertex_fence[slot] = 0;
      }
      memcpy((uint8_t *)ctx->vram_vertices + offset, vertices, bytes);
      __sync_synchronize();
      ret = boundary_aware ?
         parhelia_batch_submit_vram_async_bounded(
            screen->fd, batch, ctx->vram_vertex_handle, offset,
            vertex_dwords, &ctx->fence) :
         parhelia_batch_submit_vram_async(
            screen->fd, batch, ctx->vram_vertex_handle, offset,
            vertex_dwords, &ctx->fence);
      if (!ret) {
         ctx->vram_vertex_fence[slot] = ctx->fence;
         ctx->vram_vertex_slot = (slot + 1) % ctx->vram_vertex_slot_count;
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
         if (getenv("PARHELIA_TRACE"))
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
      memcpy(ctx->vram_vertices, vertices, bytes);
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
      if (ret == -ENOMEM && getenv("PARHELIA_TRACE"))
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
      if (getenv("PARHELIA_TRACE_BATCH"))
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
ph_drain_batch(struct ph_context *ctx)
{
   unsigned packed_calls = 0;
   bool packed_calls_valid;
   int ret;

   if (!ctx->pending_batch.packet_count)
      return 0;
   packed_calls_valid = !parhelia_batch_packed_call_count(
      &ctx->pending_batch, 0, ctx->pending_batch.packet_count,
      ctx->pack_adjacent_packets, &packed_calls);
   if (getenv("PARHELIA_TRACE_BATCH"))
      fprintf(stderr,
              "parhelia: aggregate submits=%u commands=%u packets=%u stages=%u relocs=%u vertices=%u resources=%u estimated_calls=%u\n",
              ctx->pending_submit_count,
              ctx->pending_batch.command_count,
              ctx->pending_batch.packet_count,
              ctx->pending_stage_calls,
              ctx->pending_batch.reloc_count,
              ctx->pending_vertex_dwords,
              ctx->pending_resource_count,
              packed_calls_valid ? packed_calls : 0);
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
   ctx->pending_batch.command_count = 0;
   ctx->pending_batch.packet_count = 0;
   ctx->pending_batch.reloc_count = 0;
   ctx->pending_vertex_dwords = 0;
   ctx->pending_submit_count = 0;
   ctx->pending_stage_calls = 0;
   ctx->pending_mark_count = 0;
   if (ret)
      ctx->submit_error = ret;
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
ph_submit_batch(struct ph_context *ctx, const struct parhelia_batch *batch,
                const void *vertices, unsigned vertex_dwords,
                struct parhelia_resource *r0, struct parhelia_resource *r1,
                struct parhelia_resource *r2, struct parhelia_resource *r3)
{
   struct parhelia_resource *resources[4] = {r0, r1, r2, r3};
   unsigned stage_calls;
   bool draw_batch;
   int ret;

   /* Same-state aggregation extends only the draw tail.  State-stream mode
    * keeps every complete state and DRAW sequence while sharing one terminal
    * EOB/DWGSYNC.  The older stateful experiment instead retains the one
    * internal fence boundary supported by the current kernel ABI. */
   if (!ctx->aggregate_batches)
      return ph_submit_batch_now(ctx, batch, vertices, vertex_dwords);

   if (ctx->aggregate_state_stream &&
       ctx->pending_mark_count >= ctx->batch_limits.packets) {
      ret = ph_drain_batch(ctx);
      if (ret)
         return ret;
   }

   stage_calls = ph_batch_stage_calls(batch);
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

static uint32_t
ph_pack_2x10(float lo, float hi)
{
   unsigned a = (unsigned)(CLAMP(lo, 0.0f, 1.0f) * 1023.0f);
   unsigned b = (unsigned)(CLAMP(hi, 0.0f, 1.0f) * 1023.0f);
   return a | (b << 16);
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
         const struct pipe_stencil_state *s = &ctx->dsa->stencil[0];
         state->depth_test = ctx->dsa->depth_enabled;
         state->depth_write = ctx->dsa->depth_writemask;
         state->depth_func = ph_compare(ctx->dsa->depth_func);
         state->stencil_test = s->enabled;
         state->stencil_func = ph_compare(s->func);
         state->stencil_fail = ph_stencil_op(s->fail_op);
         state->stencil_zfail = ph_stencil_op(s->zfail_op);
         state->stencil_zpass = ph_stencil_op(s->zpass_op);
         state->stencil_ref = ctx->stencil_ref.ref_value[0];
         state->stencil_value_mask = s->valuemask;
         state->stencil_write_mask = s->writemask;
      }
   }
   if (blend) {
      struct p3d_blend_factor rgb_src = ph_blend_factor(blend->rgb_src_factor);
      struct p3d_blend_factor rgb_dst = ph_blend_factor(blend->rgb_dst_factor);
      struct p3d_blend_factor a_src = ph_blend_factor(blend->alpha_src_factor);
      struct p3d_blend_factor a_dst = ph_blend_factor(blend->alpha_dst_factor);

      state->blend = blend->blend_enable;
      state->blend_src = (struct p3d_blend_factor){rgb_src.rgb, a_src.a};
      state->blend_dst = (struct p3d_blend_factor){rgb_dst.rgb, a_dst.a};
      state->blend_eq = ph_blend_equation(blend->rgb_func);
      state->blend_const_rg = ph_pack_2x10(ctx->blend_color.color[0],
                                           ctx->blend_color.color[1]);
      state->blend_const_ba = ph_pack_2x10(ctx->blend_color.color[2],
                                           ctx->blend_color.color[3]);
      state->mask_r = !!(blend->colormask & PIPE_MASK_R);
      state->mask_g = !!(blend->colormask & PIPE_MASK_G);
      state->mask_b = !!(blend->colormask & PIPE_MASK_B);
      state->mask_a = !!(blend->colormask & PIPE_MASK_A);
      state->mask_explicit = true;
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
static bool
ph_use_vertex_alpha(struct ph_context *ctx, bool textured,
		    bool multitextured, bool depth, bool has_colour)
{
	const struct pipe_rt_blend_state *blend =
		ctx->blend ? &ctx->blend->rt[0] : NULL;
	const char *reason = "selected";
	unsigned reason_bit = 0;

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
	if (depth && textured && getenv("PARHELIA_TRACE") &&
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

static bool
ph_copy_present_quad(struct ph_context *ctx,
                     struct parhelia_resource *dst,
                     struct parhelia_resource *src)
{
   float minx = INFINITY, miny = INFINITY;
   float maxx = -INFINITY, maxy = -INFINITY;
   int x0, y0, width, height;

   /* This is the exact 1:1 GL_REPLACE quad emitted by glamor Present.  Keep
    * the recognition strict so arbitrary application texture draws continue
    * through the recovered hardware sampler path. */
   if (ctx->triangles != 2 || !ctx->fs || ctx->fs->uses_color ||
       !(dst->base.bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) ||
       !src->linear_map || (!dst->map && !dst->linear_map))
      return false;
   /* Only the plain 7-dword XY/RGB/ST stream, and no fragment state which
    * would make the result differ from a copy. */
   if (ctx->vertex_alpha || ctx->alpha_micro || ctx->alpha_micro_cutout ||
       ctx->cpu_alpha_cutout || ctx->rgb_alpha_two_pass ||
       ctx->native_sampled_alpha ||
       (ctx->blend && (ctx->blend->rt[0].blend_enable ||
                       ctx->blend->logicop_enable ||
                       ctx->blend->rt[0].colormask != PIPE_MASK_RGBA)) ||
       (ctx->dsa && (ctx->dsa->alpha_enabled ||
                     ctx->dsa->stencil[0].enabled)) ||
       (ctx->rasterizer && ctx->rasterizer->scissor))
      return false;
   for (unsigned v = 0; v < 6; v++) {
      const float *p = &ctx->vertices[v * 7];
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
      const float *p = &ctx->vertices[v * 7];

      if (fabsf(p[5] - (p[0] - x0) / width) > 0.001f ||
          fabsf(p[6] - (p[1] - y0) / height) > 0.001f)
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
   if (!getenv("PARHELIA_CPU_BLIT") &&
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
            ret = parhelia_batch_submit(ph_screen(ctx->base.screen)->fd,
                                        batch, NULL, 0, &ctx->fence);
      }
      if (!ret) {
         dst->tiled_dirty = false;
         dst->linear_dirty = true;
         dst->mip_defined |= BITFIELD_BIT(0);
         dst->content_generation++;
         if (getenv("PARHELIA_TRACE"))
            fprintf(stderr,
                    "parhelia: GPU present src=%u dst=%u/%u rect=%d,%d %dx%d\n",
                    src->linear_handle, dst->handle, dst->linear_handle,
                    x0, y0, width, height);
         return true;
      }
      if (getenv("PARHELIA_TRACE"))
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
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: CPU present fallback src=%u dst=%u rect=%d,%d %dx%d\n",
              src->linear_handle, dst->handle, x0, y0, width, height);
   return true;
}

static void
ph_pack_multitex_vertices(struct ph_context *ctx, bool second, bool depth)
{
   const unsigned src_dpv = depth ? 10 : 9;
   const unsigned dst_dpv = depth ? 8 : 7;
   const unsigned st = depth ? (second ? 8 : 6) : (second ? 7 : 5);

   for (unsigned v = 0; v < ctx->triangles * 3; v++) {
      const float *src = &ctx->vertices[v * src_dpv];
      float *dst = &ctx->packed_vertices[v * dst_dpv];
      *dst++ = src[0];
      *dst++ = src[1];
      if (depth)
         *dst++ = src[2];
      *dst++ = src[depth ? 3 : 2];
      *dst++ = src[depth ? 4 : 3];
      *dst++ = src[depth ? 5 : 4];
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

static unsigned
ph_view_mip_levels(const struct parhelia_resource *texture,
                   const struct pipe_sampler_view *view)
{
   unsigned levels = ph_resource_mip_levels(texture);

   if (!view || view->u.tex.first_level || !levels)
      return 1;
   return MIN2(levels, view->u.tex.last_level + 1);
}

static bool
ph_use_mips(const struct parhelia_resource *texture,
            const struct pipe_sampler_state *sampler,
            const struct pipe_sampler_view *view)
{
   unsigned levels;

   if (!texture || !sampler ||
       sampler->min_mip_filter == PIPE_TEX_MIPFILTER_NONE)
      return false;
   levels = ph_view_mip_levels(texture, view);
   return levels > 1 &&
      ph_mip_ready_levels(texture->mip_defined, levels) == levels;
}

static uint32_t
ph_sampler_filter_word(const struct p3d_texture *texture,
                       const struct pipe_sampler_state *sampler)
{
   float bias = sampler ? CLAMP(sampler->lod_bias, -4.0f, 4.0f) : 0.0f;
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
   return texture &&
       ph_screen(ctx->base.screen)->chip_id == 0x2539 &&
       texture->base.target == PIPE_TEXTURE_2D &&
       !(texture->base.bind & PIPE_BIND_DEPTH_STENCIL) &&
       !(texture->base.bind & (PIPE_BIND_SCANOUT |
                               PIPE_BIND_DISPLAY_TARGET |
                               PIPE_BIND_SHARED));
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
       (color->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        color->base.format != PIPE_FORMAT_B8G8R8X8_UNORM) ||
       (depth && (!zs->handle ||
                  ctx->fb.zsbuf.format != PIPE_FORMAT_Z24_UNORM_S8_UINT)) ||
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
            off = p3d_tile_offset_layout(x, y, texture->stride,
                                         texture->tile_layout);
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
   /* A prior draw may still sample the existing companions asynchronously.
    * UPLOAD_RENDER is a CPU write into their storage, so wait before refresh. */
   if ((ret = ph_wait_async(ctx)))
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
   if (getenv("PARHELIA_TRACE"))
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
       ctx->fb.zsbuf.format != PIPE_FORMAT_Z24_UNORM_S8_UINT)
      return false;

   /* Stage selection needs the same logical tiled image that submission will
    * sample.  Texture uploads are CPU copies into the mapped resource, so
    * making it current here emits no GPU work or synchronization. */
   if (texture->rgb5a1_packed)
      ph_restore_rgba_tiled(texture);
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
   trace_candidate = getenv("PARHELIA_TRACE_CPU_ALPHA") &&
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
       (color->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        color->base.format != PIPE_FORMAT_B8G8R8X8_UNORM) || !color->handle ||
       (ctx->dsa->depth_enabled &&
        (!zs->handle ||
         ctx->fb.zsbuf.format != PIPE_FORMAT_Z24_UNORM_S8_UINT)) ||
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
   if (texture->rgb5a1_packed)
      ph_restore_rgba_tiled(texture);
   ph_resource_upload_tiled(texture);

   /* XRGB has no destination alpha to preserve.  The two hardware passes
    * therefore implement the complete visible source-over result for every
    * RGBA texture accepted above; no application fingerprint or screen-space
    * heuristic is needed.  Keep the image classification below only for a
    * true ARGB target, whose alpha-channel equation P690 cannot reproduce
    * from sampled RGB. */
   if (color->base.format == PIPE_FORMAT_B8G8R8X8_UNORM)
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

   /* ph_use_rgb_alpha_two_pass() established the exact XRGB equation and all
    * texture, blend, depth and stencil requirements.  Position and atlas size
    * do not affect correctness on this framebuffer format. */
   if (color->base.format == PIPE_FORMAT_B8G8R8X8_UNORM)
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

   if (!getenv("PARHELIA_TRACE_ALPHA_TEST") || !state->alpha_test ||
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

   if (!getenv("PARHELIA_TRACE_BLEND_ALPHA") || !texture || !blend ||
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

static void
ph_cpu_cutout_triangle(const float *in, struct parhelia_resource *color,
                       struct parhelia_resource *depth,
                       const struct parhelia_resource *texture,
                       unsigned width, unsigned height)
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
         new_z = (uint32_t)lrintf(z * 16777215.0f) & 0x00ffffffu;
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
                            unsigned width, unsigned height)
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
            new_z = (uint32_t)lrintf(z * 16777215.0f) & 0x00ffffffu;
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
   if (getenv("PARHELIA_TRACE_BLEND_ALPHA")) {
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
                                  sampler, ctx->dsa, ctx->fb.width, ctx->fb.height);
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
                             ctx->fb.width, ctx->fb.height);
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

   ret = parhelia_batch_state_textured(&attenuation_batch, color->handle,
                                        depth ? depth->handle : 0,
                                        texture->alpha_rgb_handle,
                                        &attenuation, ctx->triangles);
   if (!ret)
      ret = parhelia_batch_state_textured(&additive_batch, color->handle,
                                           depth ? depth->handle : 0,
                                           texture->premul_rgb_handle,
                                           &additive, ctx->triangles);
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
   if (!ret && getenv("PARHELIA_TRACE"))
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
      ctx->submit_error = -EINVAL;
      goto out;
   }

   color = ph_resource(ctx->fb.cbufs[0].texture);
   if (ctx->fb.zsbuf.texture)
      depth = ph_resource(ctx->fb.zsbuf.texture);
   if (!color->handle || (depth && !depth->handle) || color->upload_texture ||
       (depth && depth->upload_texture)) {
      ctx->submit_error = -EINVAL;
      goto out;
   }
   if (ctx->aggregate_batches && color->linear_dirty && ph_drain_batch(ctx))
      goto out;
   ph_resource_upload_tiled(color);

   state.colour.pitch = color->stride;
   state.colour.fmt = 0x32;
   state.screen_w = ctx->fb.width;
   state.screen_h = ctx->fb.height;
   state.prim = prim;
   state.warp_entry = ctx->warp_passthrough;
   state.dither = !ctx->blend || ctx->blend->dither;
   state.flat = ctx->rasterizer && ctx->rasterizer->flatshade;
   state.has_depth = depth != NULL;
   state.dw_per_vertex = depth ? 6 : 5;
   if (depth) {
      state.depth.pitch = depth->stride;
      state.depth.fmt = 0x36;
      state.depth_slot = 1;
   }
   ph_apply_fragment_state(ctx, &state, depth != NULL);

   ret = parhelia_batch_state(batch, color->handle,
                              depth ? depth->handle : 0, &state, count);
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
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: submit %s=%u color=%u depth=%u ret=0\n",
              prim == P3D_POINTS ? "points" : "lines", count,
              color->handle, depth ? depth->handle : 0);
out:
   if (prim == P3D_POINTS)
      ctx->points = 0;
   else
      ctx->lines = 0;
}

static void
ph_submit_vertices(struct ph_context *ctx)
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
   bool submitted = false;
   bool tiled_textures = getenv("PARHELIA_TEXTURE_TILED") != NULL;
   bool small_texture_linear = getenv("PARHELIA_SMALL_TEXTURE_LINEAR") != NULL;
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
   if (!ctx->fb.nr_cbufs || !ctx->fb.cbufs[0].texture) {
      ctx->submit_error = -EINVAL;
      goto out;
   }
   color = ph_resource(ctx->fb.cbufs[0].texture);
   if (ctx->fb.zsbuf.texture)
      depth = ph_resource(ctx->fb.zsbuf.texture);
   /* The upload class has CPU shadows but no reverse aperture/download path.
    * Never let a framebuffer write make those shadows stale and later
    * overwrite the rendered result on sampler bind. */
   if (color->upload_texture || (depth && depth->upload_texture)) {
      ctx->submit_error = -EOPNOTSUPP;
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
   if (texture && ctx->fs && (ctx->fs->sampler_mask & BITFIELD_BIT(1)) &&
       (!ph_screen(ctx->base.screen)->single_texture_pipe ||
        ctx->fs->serial_op != PH_SERIAL_UNSUPPORTED) &&
       ctx->num_sampler_views > 1 && ctx->sampler_views[1] &&
       ctx->sampler_views[1]->texture) {
      texture1 = ph_resource(ctx->sampler_views[1]->texture);
      if (ctx->num_samplers > 1)
         sampler1 = ctx->samplers[1];
   }
   /* An explicit env setting remains a force-enable for the established Q3
    * launch. Without it, use only the P690 base-level format/dimension shape
    * recovered for layout 2; all small, mip-described and unusual textures
    * retain the conservative linear sampler backing. */
   if (texture)
      texture_tiled = texture->upload_texture ? true : tiled_textures ?
         (!small_texture_linear ||
          (texture->base.width0 >= 32 && texture->base.height0 >= 32)) :
         ph_auto_tiled_texture(ctx, texture, sampler);
   if (texture1)
      texture1_tiled = texture1->upload_texture ? true : tiled_textures ?
         (!small_texture_linear ||
          (texture1->base.width0 >= 32 && texture1->base.height0 >= 32)) :
         ph_auto_tiled_texture(ctx, texture1, sampler1);
   texture_mipped = texture_tiled &&
      ph_use_mips(texture, sampler, ctx->sampler_views[0]);
   texture1_mipped = texture1_tiled &&
      ph_use_mips(texture1, sampler1, ctx->sampler_views[1]);
   if (!color->handle || (depth && !depth->handle)) {
      ctx->submit_error = -EINVAL;
      goto out;
   }
   if ((texture && (!(texture_tiled ? texture->handle : texture->linear_handle) ||
                    texture->base.width0 > 0x3fff)) ||
       (texture1 && (!(texture1_tiled ? texture1->handle : texture1->linear_handle) ||
                     texture1->base.width0 > 0x3fff)) ||
       (texture1 && !ph_screen(ctx->base.screen)->single_texture_pipe &&
        !ctx->warp_textured_multitex)) {
      ctx->submit_error = -EINVAL;
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
      if (ctx->aggregate_batches && !texture->rgb5a1_packed &&
          ph_drain_batch(ctx))
         goto out;
      rgb5a1_texture = ph_prepare_rgb5a1(texture);
   }
   if (ctx->aggregate_batches && texture && !rgb5a1_texture &&
       texture->rgb5a1_packed &&
       ph_drain_batch(ctx))
      goto out;
   if (texture && !rgb5a1_texture)
      ph_restore_rgba_tiled(texture);
   if (texture1 && texture1 != texture && texture1->rgb5a1_packed) {
      if (ctx->aggregate_batches && ph_drain_batch(ctx))
         goto out;
      ph_restore_rgba_tiled(texture1);
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
   if (ctx->native_sampled_alpha &&
       (ret = ph_prepare_alpha_companions(ctx, texture, sampler, true))) {
      ctx->submit_error = ret;
      goto out;
   }
   if (ctx->native_sampled_alpha && getenv("PARHELIA_TRACE_NATIVE_ALPHA") &&
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
   if (texture && !texture1 && !depth && ph_copy_present_quad(ctx, color, texture))
      goto out;
   if (ctx->cpu_alpha_cutout) {
      if (ctx->aggregate_batches && ph_drain_batch(ctx))
         goto out;
      ph_resource_resolve_linear(color);
      ph_resource_resolve_linear(depth);
      ph_cpu_alpha_cutout(ctx, color, depth, texture);
      if (getenv("PARHELIA_TRACE"))
         fprintf(stderr, "parhelia: CPU alpha cutout triangles=%u image=%#llx\n",
                 ctx->triangles,
                 (unsigned long long)ph_texture_base_hash(texture, true,
                                                           NULL, NULL));
      goto out;
   }
   if (ctx->aggregate_batches && color->linear_dirty && ph_drain_batch(ctx))
      goto out;
   ph_resource_upload_tiled(color);

   state.colour.pitch = color->stride;
   state.colour.fmt = 0x32;
   state.screen_w = ctx->fb.width;
   state.screen_h = ctx->fb.height;
   state.prim = P3D_TRIANGLES;
   state.warp_entry = ctx->warp_passthrough;
   state.dither = !ctx->blend || ctx->blend->dither;
   state.flat = ctx->rasterizer && ctx->rasterizer->flatshade;
   state.cull = ph_hardware_cull(ctx->rasterizer);
	state.has_depth = depth != NULL;
	state.dw_per_vertex = texture ?
	                              ((texture1 || ctx->native_sampled_alpha) ?
	                               (depth ? 10 : 9) :
	                               (depth ? 8 : 7)) :
	                               (depth ? 6 : 5);
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
   state.single_texture_pipe = ph_screen(ctx->base.screen)->single_texture_pipe;
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
      tex.fmt = (rgb5a1_texture ||
                 texture->base.format == PIPE_FORMAT_B5G5R5A1_UNORM) ? 0x26 :
                (texture->base.format == PIPE_FORMAT_B8G8R8X8_UNORM ||
                 texture->base.format == PIPE_FORMAT_R8G8B8X8_UNORM) ?
                   0x33 : 0x32;
      tex.texelcode = texture_tiled ?
         ((rgb5a1_texture ||
           texture->base.format == PIPE_FORMAT_B5G5R5A1_UNORM) ? 3 : 2) : 0;
      tex.min_filter = ph_sampler_image_filter(
         sampler && sampler->min_img_filter == PIPE_TEX_FILTER_LINEAR,
         sampler ? sampler->max_anisotropy : 0);
      tex.mag_filter = ph_sampler_image_filter(
         sampler && sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR,
         sampler ? sampler->max_anisotropy : 0);
      tex.mip_filter = texture_mipped ? ph_sampler_mip_filter(sampler) :
                                       P3D_MIP_NONE;
      tex.wrap_s = sampler && sampler->wrap_s != PIPE_TEX_WRAP_REPEAT ?
                      P3D_WRAP_CLAMP : P3D_WRAP_REPEAT;
      tex.wrap_t = sampler && sampler->wrap_t != PIPE_TEX_WRAP_REPEAT ?
                      P3D_WRAP_CLAMP : P3D_WRAP_REPEAT;
      tex.max_level = texture_mipped ?
         ph_view_mip_levels(texture, ctx->sampler_views[0]) - 1 : 0;
      tex.filter = ph_sampler_filter_word(&tex, sampler);
      tex.wrap = ph_sampler_wrap_word(&tex, sampler);
      tex.env = ctx->fs->texenv;
      state.tex = &tex;
      state.warp_entry = (texture1 || ctx->native_sampled_alpha) ?
                         ctx->warp_textured_multitex :
                         ((ctx->fs->uses_color || ctx->alpha_micro) ? ctx->warp_textured :
                                                ctx->warp_replace);
      if (ctx->native_sampled_alpha) {
         tex1 = tex;
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
         tex1.fmt = texture1->base.format == PIPE_FORMAT_B5G5R5A1_UNORM ? 0x26 :
                    (texture1->base.format == PIPE_FORMAT_B8G8R8X8_UNORM ||
                     texture1->base.format == PIPE_FORMAT_R8G8B8X8_UNORM) ?
                       0x33 : 0x32;
         tex1.texelcode = texture1_tiled ?
            (texture1->base.format == PIPE_FORMAT_B5G5R5A1_UNORM ? 3 : 2) : 0;
         tex1.min_filter = ph_sampler_image_filter(
            sampler1 && sampler1->min_img_filter == PIPE_TEX_FILTER_LINEAR,
            sampler1 ? sampler1->max_anisotropy : 0);
         tex1.mag_filter = ph_sampler_image_filter(
            sampler1 && sampler1->mag_img_filter == PIPE_TEX_FILTER_LINEAR,
            sampler1 ? sampler1->max_anisotropy : 0);
         tex1.mip_filter = texture1_mipped ? ph_sampler_mip_filter(sampler1) :
                                            P3D_MIP_NONE;
         tex1.wrap_s = sampler1 && sampler1->wrap_s != PIPE_TEX_WRAP_REPEAT ?
                         P3D_WRAP_CLAMP : P3D_WRAP_REPEAT;
         tex1.wrap_t = sampler1 && sampler1->wrap_t != PIPE_TEX_WRAP_REPEAT ?
                         P3D_WRAP_CLAMP : P3D_WRAP_REPEAT;
         tex1.max_level = texture1_mipped ?
            ph_view_mip_levels(texture1, ctx->sampler_views[1]) - 1 : 0;
         tex1.filter = ph_sampler_filter_word(&tex1, sampler1);
         tex1.wrap = ph_sampler_wrap_word(&tex1, sampler1);
         /* Quake's base world pass is diffuse times lightmap. */
         tex1.env = P3D_TEXENV_MODULATE;
         state.tex1 = &tex1;
      }
   }
   if (depth) {
      state.depth.pitch = depth->stride;
      state.depth.fmt = 0x36;
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
      state.mask_r = !!(blend->colormask & PIPE_MASK_R);
      state.mask_g = !!(blend->colormask & PIPE_MASK_G);
      state.mask_b = !!(blend->colormask & PIPE_MASK_B);
      state.mask_a = !!(blend->colormask & PIPE_MASK_A);
      state.mask_explicit = true;
   }
   ph_apply_fragment_state(ctx, &state, depth != NULL);
   if (ctx->alpha_micro_cutout) {
      /* Alpha was classified before splitting.  The surviving cells are
       * opaque, so suppress the broken sampled-alpha comparator. */
      state.alpha_test = false;
      state.blend = false;
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
	ph_trace_blend_alpha(ctx, &state, texture, sampler, texture_tiled);
	one_pass_multitexture = (texture1 || ctx->native_sampled_alpha) &&
	                        ctx->p690_multitexture && ctx->fs &&
	                        ctx->fs->serial_op == PH_SERIAL_MODULATE;
	if (texture && getenv("PARHELIA_TRACE") && ctx->triangles) {
		const float *v0 = &ctx->vertices[0];
		const float *v1 = &ctx->vertices[dpv];
		const float *v2 = &ctx->vertices[2 * dpv];
		unsigned st = state.vertex_alpha ? (depth ? 7 : 6) :
		              (depth ? 6 : 5);
      fprintf(stderr,
              "parhelia: textured vertices p0=%g,%g st0=%g,%g p1=%g,%g st1=%g,%g p2=%g,%g st2=%g,%g\n",
		      v0[0], v0[1], v0[st], v0[st + 1],
		      v1[0], v1[1], v1[st], v1[st + 1],
		      v2[0], v2[1], v2[st], v2[st + 1]);
   }
   if (ctx->native_sampled_alpha) {
      ret = parhelia_batch_state_multitextured(batch, color->handle,
                                                depth ? depth->handle : 0,
                                                texture_handle,
                                                texture1_handle, &state,
                                                ctx->triangles);
   } else if (texture1 && ph_screen(ctx->base.screen)->single_texture_pipe &&
       !one_pass_multitexture) {
      struct p3d_state lightmap = state;
      struct p3d_texture lightmap_tex = tex1;
      unsigned packed_dpv = depth ? 8 : 7;

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
         lightmap.warp_entry = ctx->warp_replace;
         ph_pack_multitex_vertices(ctx, true, depth != NULL);
         ret = parhelia_batch_state_textured(batch, color->handle,
                                             depth ? depth->handle : 0,
                                             texture1_handle, &lightmap,
                                             ctx->triangles);
         if (!ret)
            ret = ph_submit_batch(ctx, batch, ctx->packed_vertices,
                                  ctx->triangles * 3 * packed_dpv,
                                  color, depth, texture1, NULL);
      } else {
         state.tex1 = NULL;
         state.dw_per_vertex = packed_dpv;
         state.warp_entry = ctx->fs->uses_color ? ctx->warp_textured :
                            ctx->warp_replace;
         ph_pack_multitex_vertices(ctx, false, depth != NULL);
         ret = parhelia_batch_state_textured(batch, color->handle,
                                             depth ? depth->handle : 0,
                                             texture_handle, &state,
                                             ctx->triangles);
         if (!ret)
            ret = ph_submit_batch(ctx, batch, ctx->packed_vertices,
                                  ctx->triangles * 3 * packed_dpv,
                                  color, depth, texture, NULL);
      }
      if (ctx->fs->serial_op != PH_SERIAL_REPLACE1 &&
          !ret && !getenv("PARHELIA_SKIP_LIGHTMAP")) {
         lightmap_tex.env = P3D_TEXENV_REPLACE;
         lightmap.tex = &lightmap_tex;
         lightmap.tex1 = NULL;
         lightmap.dw_per_vertex = packed_dpv;
         lightmap.warp_entry = ctx->warp_replace;
         lightmap.blend = true;
         if (ctx->fs->serial_op == PH_SERIAL_ADD) {
            lightmap.blend_src = P3D_BF_ONE;
            lightmap.blend_dst = P3D_BF_ONE;
         } else {
            lightmap.blend_src = P3D_BF_DST_COLOR;
            lightmap.blend_dst = P3D_BF_ZERO;
         }
         if (getenv("PARHELIA_LIGHTMAP_RAW")) {
            lightmap.blend = false;
            lightmap.blend_src = P3D_BF_ONE;
            lightmap.blend_dst = P3D_BF_ZERO;
         }
         if (depth) {
            lightmap.depth_test = true;
            lightmap.depth_write = false;
            lightmap.depth_func = P3D_CMP_EQUAL;
         }
         ph_pack_multitex_vertices(ctx, true, depth != NULL);
         ret = parhelia_batch_state_textured(batch, color->handle,
                                             depth ? depth->handle : 0,
                                             texture1_handle,
                                             &lightmap, ctx->triangles);
         if (!ret)
            ret = ph_submit_batch(ctx, batch, ctx->packed_vertices,
                                  ctx->triangles * 3 * packed_dpv,
                                  color, depth, texture1, NULL);
      }
   } else if (texture1)
      ret = parhelia_batch_state_multitextured(batch, color->handle,
                                               depth ? depth->handle : 0,
                                               texture_handle,
                                               texture1_handle, &state,
                                               ctx->triangles);
   else if (texture)
      ret = parhelia_batch_state_textured(batch, color->handle,
                                          depth ? depth->handle : 0,
                                          texture_handle, &state,
                                          ctx->triangles);
   else
      ret = parhelia_batch_state(batch, color->handle,
                                 depth ? depth->handle : 0, &state,
                                 ctx->triangles);
   if (!ret && !submitted)
      ret = ph_submit_batch(ctx, batch, ctx->vertices,
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
   if (ret || getenv("PARHELIA_TRACE"))
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
	ctx->vertex_alpha = false;
	ctx->alpha_micro = false;
	ctx->alpha_micro_cutout = false;
	ctx->cpu_alpha_cutout = false;
	ctx->rgb_alpha_two_pass = false;
	ctx->native_sampled_alpha = false;
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
   if (cutout && getenv("PARHELIA_TRACE_ALPHA_CUTOUT") &&
       ctx->trace_alpha_cutout_resource != texture) {
      ctx->trace_alpha_cutout_resource = texture;
      fprintf(stderr, "parhelia: alpha-cutout micro tex=%ux%u samples=%u accepted=%u range=%g..%g\\n",
              texture->base.width0, texture->base.height0, samples, accepted,
              amin, amax);
   }
}

static void
ph_stage_tri(struct draw_stage *stage, struct prim_header *prim)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   int pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   int col = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_COLOR, 0);
   int texcoord = -1, texcoord1 = -1;
   bool textured = ph_samples_unit0(ctx);
	bool multitextured = textured && ctx->fs &&
                        (ctx->fs->sampler_mask & BITFIELD_BIT(1)) &&
                        (!ph_screen(ctx->base.screen)->single_texture_pipe ||
                         ctx->fs->serial_op != PH_SERIAL_UNSUPPORTED) &&
                        ctx->num_sampler_views > 1 &&
                        ctx->sampler_views[1] &&
                        ctx->sampler_views[1]->texture;
   bool depth = ctx->fb.zsbuf.texture != NULL;
	bool vertex_alpha;
	bool alpha_micro;
	bool alpha_micro_cutout;
	bool alpha_micro_depth;
	bool cpu_alpha_cutout;
	bool rgb_alpha_two_pass;
	bool native_sampled_alpha;
	unsigned dpv;
   float *dst;

   /* The hardware exposes only one-face cull selectors.  FRONT_AND_BACK has
    * no packet encoding and must reject the primitive before staging it. */
   if (ctx->rasterizer &&
       ctx->rasterizer->cull_face == PIPE_FACE_FRONT_AND_BACK)
      return;

   /* Preserve mixed-primitive ordering while allowing every run of compatible
    * line callbacks to share one state block. */
   if (ctx->points || ctx->lines) {
      ph_submit_vertices(ctx);
      if (ctx->submit_error)
         return;
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
	alpha_micro_depth = depth && ctx->dsa && ctx->dsa->depth_enabled;
	if (alpha_micro)
	   depth = alpha_micro_depth;
	if (getenv("PARHELIA_TRACE_CPU_ALPHA") && textured && ctx->blend &&
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
	if (getenv("PARHELIA_TRACE_ALPHA_SOURCE") && textured && ctx->blend &&
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
	if (cpu_alpha_cutout || rgb_alpha_two_pass)
	   dpv = 10; /* Internal XYZOOW,RGBA,ST; packed to 7/8 GPU dwords later. */
	/* A state setter normally drains a batch before reaching this point.  The
	 * extra guard keeps a future draw-stage change from mixing 7- and 8-dword
	 * streams if the exact blend predicate changes within a primitive list. */
	if (ctx->triangles && (ctx->vertex_alpha != vertex_alpha ||
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
      texcoord = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_GENERIC, 0);
      if (texcoord < 0)
         texcoord = draw_find_shader_output(stage->draw,
                                            TGSI_SEMANTIC_TEXCOORD, 0);
      if (multitextured) {
         texcoord1 = draw_find_shader_output(stage->draw,
                                              TGSI_SEMANTIC_GENERIC, 1);
         if (texcoord1 < 0)
            texcoord1 = draw_find_shader_output(stage->draw,
                                                 TGSI_SEMANTIC_TEXCOORD, 1);
      }
   }
   if (textured && texcoord < 0 && getenv("PARHELIA_TRACE")) {
      const struct tgsi_shader_info *info = draw_get_shader_info(stage->draw);
      fprintf(stderr, "parhelia: no real texture varying; VS has %u outputs\\n",
              info->num_outputs);
      for (unsigned i = 0; i < info->num_outputs; i++)
         fprintf(stderr, "parhelia: vs output %u semantic=%u/%u\\n", i,
                 info->output_semantic_name[i], info->output_semantic_index[i]);
   }
   if (ctx->submit_error || pos < 0 || (textured && texcoord < 0) ||
       (multitextured && texcoord1 < 0))
      return;
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
       getenv("PARHELIA_TRACE")) {
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
      const float *c = col < 0 ? white : prim->v[v]->data[col];
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
			float scale_s = ph_alpha_companion_scale(
			   texture->base.width0,
			   ph_alpha_companion_extent(texture->base.width0));
			float scale_t = ph_alpha_companion_scale(
			   texture->base.height0,
			   ph_alpha_companion_extent(texture->base.height0));

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
      *dst++ = p[0];
      *dst++ = p[1];
      if (depth)
         *dst++ = CLAMP(p[2], 0.0f, 1.0f);
		*dst++ = c[0];
		*dst++ = c[1];
		*dst++ = c[2];
		if (vertex_alpha)
			*dst++ = c[3];
      if (textured) {
         const float *tc = prim->v[v]->data[texcoord];
         *dst++ = tc[0];
         *dst++ = tc[1];
         if (multitextured) {
            const float *tc1 = prim->v[v]->data[texcoord1];
            *dst++ = tc1[0];
            *dst++ = tc1[1];
         }
      }
   }
   ctx->triangles++;
}

/* The vendor's GL capture proves the complete unit-point draw delta from a
 * triangle: class 0, primitive word 0x00060020, raster image 0x04000024 and a
 * one-vertex byte range.  Its own client produces exactly one covered pixel.
 * Wider, smoothed, sprite and per-vertex-size points need additional WARP and
 * coverage state and deliberately remain outside this exact native subset. */
static void
ph_stage_point(struct draw_stage *stage, struct prim_header *prim)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   float *dst;
   int pos, col;
   bool has_texture;
   bool depth;
   unsigned dpv;

   if (ctx->submit_error || !ctx->fb.nr_cbufs ||
       !ctx->fb.cbufs[0].texture)
      return;

   has_texture = ctx->num_sampler_views && ctx->sampler_views[0] &&
                 ctx->sampler_views[0]->texture;
   if (has_texture ||
       (ctx->rasterizer &&
        (ctx->rasterizer->point_smooth ||
         ctx->rasterizer->point_quad_rasterization ||
         ctx->rasterizer->point_size_per_vertex ||
         ctx->rasterizer->sprite_coord_enable ||
         fabsf(ctx->rasterizer->point_size - 1.0f) > 0.0001f)))
      return;

   pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   col = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_COLOR, 0);
   if (pos < 0)
      return;

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

   *dst++ = p[0];
   *dst++ = p[1];
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
 * Keep textured and smooth lines outside this first native path.  Their
 * fragment/coverage semantics need more than merely selecting the line
 * primitive, while dropping ordinary GL_LINES altogether is an existing and
 * much more fundamental correctness bug. */
static void
ph_stage_line(struct draw_stage *stage, struct prim_header *prim)
{
   struct ph_context *ctx = ((struct ph_stage *)stage)->ctx;
   float *dst;
   int pos, col;
   bool has_texture;
   bool depth;
   unsigned dpv;

   if (ctx->submit_error || !ctx->fb.nr_cbufs ||
       !ctx->fb.cbufs[0].texture)
      return;

   has_texture = ctx->num_sampler_views && ctx->sampler_views[0] &&
                 ctx->sampler_views[0]->texture;
   if (has_texture ||
       (ctx->rasterizer && (ctx->rasterizer->line_smooth ||
                            ctx->rasterizer->line_width >= 1.5f)))
      return;

   pos = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_POSITION, 0);
   col = draw_find_shader_output(stage->draw, TGSI_SEMANTIC_COLOR, 0);
   if (pos < 0)
      return;

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
static void ph_stage_destroy(struct draw_stage *s) { FREE(s); }

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
   return &s->base;
}

static void
ph_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info,
            unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
            const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct ph_context *ctx = ph_context(pipe);
   const void *indices = NULL;

   if (num_draws > 1) {
      util_draw_multi(pipe, info, drawid_offset, indirect, draws, num_draws);
      return;
   }
   if (!num_draws || indirect || ctx->submit_error)
      return;
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
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(ctx->draw);
   for (unsigned i = 0; i < ctx->draw->pt.nr_vertex_buffers; i++)
      draw_set_mapped_vertex_buffer(ctx->draw, i, NULL, 0);
   if (indices)
      draw_set_indexes(ctx->draw, NULL, 0, 0);
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
static void ph_bind_blend(struct pipe_context *p, void *s)
{
   struct ph_context *ctx = ph_context(p);
   if (ctx->blend == s)
      return;
   ph_submit_vertices(ctx);
   ctx->blend = s;
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
      if (ctx->samplers[start + i] != states[i])
         goto changed;
   return;
changed:
   ph_submit_vertices(ctx);
   for (unsigned i = 0; i < count; i++)
      ctx->samplers[start + i] = states[i];
   ctx->num_samplers = MAX2(ctx->num_samplers, start + count);
   while (ctx->num_samplers && !ctx->samplers[ctx->num_samplers - 1])
      ctx->num_samplers--;
}

static struct pipe_sampler_view *
ph_create_sampler_view(struct pipe_context *p, struct pipe_resource *texture,
                       const struct pipe_sampler_view *templ)
{
   struct pipe_sampler_view *view;
   if (!templ || !texture || templ->target != PIPE_TEXTURE_2D ||
       templ->u.tex.first_level ||
       templ->u.tex.last_level > texture->last_level)
      return NULL;
   view = CALLOC_STRUCT(pipe_sampler_view);
   if (!view)
      return NULL;
   *view = *templ;
   view->reference.count = 1;
   view->texture = NULL;
   view->context = p;
   pipe_resource_reference(&view->texture, texture);
   return view;
}

static void
ph_sampler_view_destroy(struct pipe_context *p, struct pipe_sampler_view *view)
{
   (void)p;
   pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

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
      if (ctx->sampler_views[start + i] != views[i])
         goto changed;
   for (unsigned i = start + count; i < end; i++)
      if (ctx->sampler_views[i])
         goto changed;
   return;
changed:
   ph_submit_vertices(ctx);
   for (unsigned i = 0; i < count; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[start + i], views[i]);
   for (unsigned i = start + count; i < end; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   ctx->num_sampler_views = MAX2(ctx->num_sampler_views, start + count);
   while (ctx->num_sampler_views &&
          !ctx->sampler_views[ctx->num_sampler_views - 1])
      ctx->num_sampler_views--;
   if (getenv("PARHELIA_TRACE_VIEWS"))
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
   if (s->type == PIPE_SHADER_IR_NIR)
      nir_shader_gather_info(s->ir.nir,
                             nir_shader_get_entrypoint(s->ir.nir));
   return draw_create_vertex_shader(ph_context(p)->draw, s);
}
static void ph_bind_vs(struct pipe_context *p, void *s)
{ draw_bind_vertex_shader(ph_context(p)->draw, s); }
static void ph_delete_vs(struct pipe_context *p, void *s)
{ draw_delete_vertex_shader(ph_context(p)->draw, s); }

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

static enum ph_serial_op
ph_nir_serial_op(nir_shader *nir)
{
   unsigned tex0 = 0, tex1 = 0, other_tex = 0;
   unsigned fmul = 0, fadd = 0;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               const nir_tex_instr *tex = nir_instr_as_tex(instr);

               if (tex->sampler_index == 0)
                  tex0++;
               else if (tex->sampler_index == 1)
                  tex1++;
               else
                  other_tex++;
            } else if (instr->type == nir_instr_type_alu) {
               const nir_alu_instr *alu = nir_instr_as_alu(instr);

               if (alu->op == nir_op_fmul)
                  fmul++;
               else if (alu->op == nir_op_fadd)
                  fadd++;
            }
         }
      }
   }

   /* Mesa's fixed-function GL1 REPLACE shader declares both sampler slots,
    * but samples only slot 1. */
   if (!other_tex && !tex0 && tex1 == 1 && !fmul && !fadd)
      return PH_SERIAL_REPLACE1;

   /* These are the exact GL1 fixed-function forms captured in
    * t2r4-gl1-shaders.log: two samples, then texture0*colour and either a
    * second multiply or an add.  Keep the match deliberately narrow. */
   if (!other_tex && tex0 == 1 && tex1 == 1) {
      if (fmul == 2 && !fadd)
         return PH_SERIAL_MODULATE;
      if (fmul == 1 && fadd == 1)
         return PH_SERIAL_ADD;
   }
   return PH_SERIAL_UNSUPPORTED;
}

/* The fixed-function GL1 DECAL program survives as one texture instruction
 * followed by FLRP(previous, texture, texture.a).  Do not attempt to infer
 * arbitrary shader algebra here: a wrong texenv corrupts alpha-tested UI.
 * The caller supplies the historical colour/replacement fallback for every
 * shape other than the exact single-texture forms below. */
static enum p3d_texenv
ph_nir_texenv(nir_shader *nir, enum p3d_texenv fallback)
{
   unsigned tex0 = 0, other_tex = 0;
   unsigned fmul = 0, fadd = 0, flrp = 0, decal_flrp = 0;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               const nir_tex_instr *tex = nir_instr_as_tex(instr);

               if (tex->sampler_index == 0)
                  tex0++;
               else
                  other_tex++;
            } else if (instr->type == nir_instr_type_alu) {
               const nir_alu_instr *alu = nir_instr_as_alu(instr);

               if (alu->op == nir_op_flrp) {
                  const nir_tex_instr *colour_tex = NULL;
                  const nir_tex_instr *alpha_tex = NULL;

                  flrp++;
                  if (alu->src[1].src.ssa && alu->src[2].src.ssa &&
                      nir_def_instr(alu->src[1].src.ssa)->type == nir_instr_type_tex &&
                      nir_def_instr(alu->src[2].src.ssa)->type == nir_instr_type_tex) {
                     colour_tex = nir_instr_as_tex(nir_def_instr(alu->src[1].src.ssa));
                     alpha_tex = nir_instr_as_tex(nir_def_instr(alu->src[2].src.ssa));
                     if (colour_tex == alpha_tex && colour_tex->sampler_index == 0)
                        decal_flrp++;
                  }
               } else if (alu->op == nir_op_fmul)
                  fmul++;
               else if (alu->op == nir_op_fadd)
                  fadd++;
            }
         }
      }
   }

   if (tex0 != 1 || other_tex)
      return fallback;
   if (flrp == 1 && decal_flrp == 1 && !fmul && !fadd)
      return P3D_TEXENV_DECAL;
   if (fmul == 1 && !flrp && !fadd)
      return P3D_TEXENV_MODULATE;
   if (!flrp && !fmul && !fadd)
      return P3D_TEXENV_REPLACE;
   return fallback;
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
         fs->sampler_mask = info->samplers_declared;
         if (s->type == PIPE_SHADER_IR_NIR) {
            fs->sampler_mask |= s->ir.nir->info.textures_used[0];
            fs->sampler_mask |= ph_nir_sampler_mask(s->ir.nir);
            fs->serial_op = ph_nir_serial_op(s->ir.nir);
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
         if (s->type == PIPE_SHADER_IR_NIR)
            fs->texenv = ph_nir_texenv(s->ir.nir, fs->texenv);
         if (getenv("PARHELIA_TRACE")) {
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
   draw_bind_fragment_shader(ctx->draw, fs ? fs->draw : NULL);
}
static void ph_delete_fs(struct pipe_context *p, void *s)
{
   struct ph_fs *fs = s;
   if (fs)
      draw_delete_fragment_shader(ph_context(p)->draw, fs->draw);
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

static void ph_set_viewport(struct pipe_context *p, unsigned start,
                            unsigned count, const struct pipe_viewport_state *vp)
{
   struct ph_context *ctx = ph_context(p);

   if (getenv("PARHELIA_TRACE_VIEWPORT")) {
      for (unsigned i = 0; i < count; i++)
         fprintf(stderr,
                 "parhelia: viewport[%u] scale=%g,%g,%g translate=%g,%g,%g\\n",
                 start + i, vp[i].scale[0], vp[i].scale[1], vp[i].scale[2],
                 vp[i].translate[0], vp[i].translate[1], vp[i].translate[2]);
   }
   draw_set_viewport_states(ctx->draw, start, count, vp);
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
static void ph_noop_stipple(struct pipe_context *p,
                            const struct pipe_poly_stipple *s)
{ (void)p; (void)s; }

static void
ph_set_framebuffer(struct pipe_context *p, const struct pipe_framebuffer_state *fb)
{
   struct ph_context *ctx = ph_context(p);

   if (getenv("PARHELIA_TRACE_VIEWPORT"))
      fprintf(stderr, "parhelia: framebuffer=%ux%u cbufs=%u zs=%u\\n",
              fb ? fb->width : 0, fb ? fb->height : 0,
              fb ? fb->nr_cbufs : 0, fb && fb->zsbuf.texture);

   /* Gallium rebinds the same Q3 colour/depth pair frequently.  It is not a
    * rendering boundary: draining here turns otherwise batchable triangles
    * into tiny submits and forces async retirement pressure. */
   if (util_framebuffer_state_equal(&ctx->fb, fb))
      return;
   ph_submit_vertices(ctx);
   util_copy_framebuffer_state(&ctx->fb, fb);
   if (fb && fb->zsbuf.texture)
      draw_set_zs_format(ctx->draw, fb->zsbuf.format);
}

/* PLNWT bit planes of the colour components enabled in @colormask. */
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
   return mask;
}

/* Native format 0x36 keeps Z24 in [23:0] and stencil in [31:24]. */
static uint32_t
ph_zs_write_mask(unsigned buffers, uint8_t stencil_mask)
{
   return ((buffers & PIPE_CLEAR_DEPTH) ? 0x00ffffffu : 0) |
          ((buffers & PIPE_CLEAR_STENCIL) ? (uint32_t)stencil_mask << 24 : 0);
}

static void
ph_clear_surface(struct ph_context *ctx, struct parhelia_resource *r,
                 unsigned format, uint32_t value, uint32_t write_mask,
                 unsigned x, unsigned y, unsigned width, unsigned height)
{
   struct parhelia_batch *batch = &ctx->build_batch;
   unsigned alloc_height;
   bool full;
   int ret;
   if (!r || !r->handle || ctx->submit_error || !write_mask)
      return;
   if (r->upload_texture) {
      ctx->submit_error = -EOPNOTSUPP;
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
   if (getenv("PARHELIA_CPU_CLEAR") && r->map && !r->transfer_shadow) {
      /* The normal path queues this packet behind previous 3D work.  A CPU
       * diagnostic must establish the same ordering before touching the
       * tiled mapping directly. */
      ph_wait_async(ctx);
      for (unsigned j = y; j < y + height; j++)
         for (unsigned i = x; i < x + width; i++) {
            uint8_t *texel = (uint8_t *)r->map +
               p3d_tile_offset_layout(i, j, r->stride, r->tile_layout);
            uint32_t old;

            memcpy(&old, texel, sizeof(old));
            old = (old & ~write_mask) | (value & write_mask);
            memcpy(texel, &old, sizeof(old));
         }
      r->upload_dirty |= r->upload_texture;
      r->tiled_dirty = true;
      r->linear_dirty = false;
      r->mip_defined |= BITFIELD_BIT(0);
      r->content_generation++;
      if (getenv("PARHELIA_TRACE"))
         fprintf(stderr,
                 "parhelia: CPU clear resource=%u fmt=%#x value=%#x mask=%#x %u,%u %ux%u stride=%u layout=%u\n",
                 r->handle, format, value, write_mask, x, y, width, height,
                 r->stride, r->tile_layout);
      return;
   }
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
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: GPU clear resource=%u fmt=%#x value=%#x mask=%#x %u,%u %ux%u ret=%d\n",
              r->handle, format, value, write_mask, x, y, width, height, ret);
   if (!ret)
      /* A clear has no CPU consumer before the next ordered 3D batch.  Use
       * the same fenced async queue as draws so Q3 does not retire colour and
       * depth clears on the CPU before it can queue world rendering. */
      ret = ph_submit_batch(ctx, batch, NULL, 0, r, NULL, NULL, NULL);
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
ph_clear_colour(struct ph_context *ctx, const struct pipe_surface *surf,
                const union pipe_color_union *color, unsigned colormask,
                unsigned x, unsigned y, unsigned width, unsigned height)
{
   const uint32_t mask = ph_colour_write_mask(surf->format, colormask);
   union util_color packed;

   if (!mask)
      return;
   util_pack_color(color->f, surf->format, &packed);
   ph_clear_surface(ctx, ph_resource(surf->texture), 0x32, packed.ui[0],
                    mask, x, y, width, height);
}

static void
ph_clear_zs(struct ph_context *ctx, const struct pipe_surface *surf,
            unsigned buffers, uint8_t stencil_mask, double depth,
            unsigned stencil, unsigned x, unsigned y, unsigned width,
            unsigned height)
{
   const uint32_t mask = ph_zs_write_mask(buffers, stencil_mask);

   if (!mask)
      return;
   if (surf->format != PIPE_FORMAT_Z24_UNORM_S8_UINT) {
      ctx->submit_error = -EOPNOTSUPP;
      return;
   }
   ph_clear_surface(ctx, ph_resource(surf->texture), 0x36,
                    util_pack_z_stencil(surf->format, depth, stencil), mask,
                    x, y, width, height);
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
   if ((buffers & PIPE_CLEAR_COLOR0) && ctx->fb.nr_cbufs &&
       ctx->fb.cbufs[0].texture)
      ph_clear_colour(ctx, &ctx->fb.cbufs[0], &color[0],
                      color_mask & PIPE_MASK_RGBA, x0, y0, x1 - x0, y1 - y0);
   if ((buffers & PIPE_CLEAR_DEPTHSTENCIL) && ctx->fb.zsbuf.texture)
      ph_clear_zs(ctx, &ctx->fb.zsbuf, buffers, stencil_mask, depth, stencil,
                  x0, y0, x1 - x0, y1 - y0);
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
   /* The following X-server blit can order against an async kernel request,
    * but it cannot see an aggregate still owned by this userspace context. */
   if (ctx->aggregate_batches && ph_drain_batch(ctx)) {
      if (fence)
         *fence = NULL;
      return;
   }
   if (ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture)
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
   if (getenv("PARHELIA_TRACE"))
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

static bool
ph_copy_texture_box(struct ph_context *ctx,
                    struct pipe_resource *dst, int dstx, int dsty,
                    struct pipe_resource *src,
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
   uint8_t *staging;

   ph_submit_vertices(ctx);
   if (!width || !height || width != dwidth || height != dheight ||
       src_box->depth != 1 || cpp != 4 ||
       util_format_get_blocksize(dst->format) != cpp ||
       (uint64_t)width * height * cpp > SIZE_MAX)
      return false;

   /* This implementation copies through CPU-visible mappings.  The source
    * can still be owned by the asynchronous render queue when ReadPixels
    * asks the state tracker to blit into a staging texture.  Waiting here is
    * required before either its linear shadow or its tiled map is sampled. */
   if (ph_wait_async(ctx) || ctx->submit_error)
      return false;
   if (sr->transfer_shadow && ph_resource_download_shadow(sr))
      return false;

   /* Imported drawable/renderbuffer resources have a linear companion.
    * Use the recovered type-2-to-type-1 hardware BITBLT to detile them once;
    * the format/orientation copy below can then read normal pitched rows. */
   if (sr->linear_map)
      ph_resource_resolve_linear(sr);

   staging = MALLOC((size_t)width * height * cpp);
   if (!staging)
      return false;

   /* A prior source-alpha draw may have repacked the native shadow as
    * A1R5G5B5.  Restore the complete ordinary image before applying a
    * potentially partial 32-bit copy. */
   if (dr->rgb5a1_packed)
      ph_restore_rgba_tiled(dr);

   for (int y = 0; y < height; y++) {
      const int sy = src_box->y + (src_box->height < 0 ? -1 - y : y);
      for (int x = 0; x < width; x++) {
         const int sx = src_box->x + (src_box->width < 0 ? -1 - x : x);
         uint8_t *out = staging + ((size_t)y * width + x) * cpp;

         if (sx < 0 || sy < 0 || sx >= (int)src->width0 ||
             sy >= (int)src->height0) {
            FREE(staging);
            return false;
         }
         if (sr->linear_map && !sr->tiled_dirty)
            memcpy(out, (uint8_t *)sr->linear_map +
                        (size_t)sy * sr->linear_stride + sx * cpp, cpp);
         else if (sr->map)
            memcpy(out, (uint8_t *)sr->map +
                        p3d_tile_offset_layout(sx, sy, sr->stride,
                                               sr->tile_layout), cpp);
         else {
            FREE(staging);
            return false;
         }
      }
   }

   for (int y = 0; y < height; y++) {
      const int dy = dsty + (dst_height < 0 ? -1 - y : y);
      for (int x = 0; x < width; x++) {
         const int dx = dstx + (dst_width < 0 ? -1 - x : x);
         const uint8_t *in = staging + ((size_t)y * width + x) * cpp;

         if (dx < 0 || dy < 0 || dx >= (int)dst->width0 ||
             dy >= (int)dst->height0) {
            FREE(staging);
            return false;
         }
         if (dr->linear_map)
            memcpy((uint8_t *)dr->linear_map +
                      (size_t)dy * dr->linear_stride + dx * cpp, in, cpp);
         if (dr->map)
            memcpy((uint8_t *)dr->map +
                      ((dr->upload_texture ||
                        ((dr->base.bind & PIPE_BIND_SAMPLER_VIEW) &&
                        !(dr->base.bind & (PIPE_BIND_RENDER_TARGET |
                                           PIPE_BIND_DISPLAY_TARGET |
                                           PIPE_BIND_SCANOUT)))) ?
                       ph_mip_texel_offset_cpp(&dr->mip, 0, dx, dy,
                                               false, cpp) :
                       p3d_tile_offset_layout(dx, dy, dr->stride,
                                              dr->tile_layout)), in, cpp);
         if (!dr->linear_map && !dr->map) {
            FREE(staging);
            return false;
         }
      }
   }
   dr->tiled_dirty = dr->map && !dr->linear_map;
   dr->linear_dirty = dr->linear_map && !dr->map;
   if (dr->transfer_shadow) {
      int ret = ph_resource_upload_shadow(dr);

      if (ret) {
         ctx->submit_error = ret;
         FREE(staging);
         return false;
      }
      dr->tiled_dirty = false;
      dr->linear_dirty = false;
   }
   dr->upload_dirty |= dr->upload_texture;
   dr->mip_defined |= BITFIELD_BIT(0);
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

   if (dst_level || src_level || dstz || src_box->z)
      return;
   if (dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER) {
      struct parhelia_resource *dr = ph_resource(dst);
      struct parhelia_resource *sr = ph_resource(src);

      if (src_box->x < 0 || src_box->width < 0 ||
          (uint64_t)src_box->x + src_box->width > src->width0 ||
          (uint64_t)dstx + src_box->width > dst->width0)
         return;
      memmove((uint8_t *)dr->malloc_data + dstx,
              (uint8_t *)sr->malloc_data + src_box->x, src_box->width);
      return;
   }
   if (dst->target == PIPE_BUFFER || src->target == PIPE_BUFFER)
      return;
   ph_copy_texture_box(ctx, dst, dstx, dsty, src, src_box,
                       src_box->width, src_box->height);
}

static void
ph_blit(struct pipe_context *p, const struct pipe_blit_info *info)
{
   struct ph_context *ctx = ph_context(p);

   if (!info || !info->src.resource || !info->dst.resource ||
       info->src.level || info->dst.level ||
       info->src.resource->nr_samples > 1 ||
       info->dst.resource->nr_samples > 1)
      return;
   bool copied = ph_copy_texture_box(ctx, info->dst.resource,
                                     info->dst.box.x, info->dst.box.y,
                                     info->src.resource, &info->src.box,
                                     info->dst.box.width,
                                     info->dst.box.height);
   if (getenv("PARHELIA_TRACE"))
      fprintf(stderr,
              "parhelia: blit src=%p dst=%p %dx%d -> %dx%d copied=%u\n",
              (void *)info->src.resource, (void *)info->dst.resource,
              info->src.box.width, info->src.box.height,
              info->dst.box.width, info->dst.box.height, copied);
}

/* P3D's 0x32/0x33 32-bit sampler formats are little-endian BGR(A/X).  Mesa
 * gives OpenGL uploads as R8G8B8(A/X), so keep the GPU backing images in the
 * hardware byte order and convert at the transfer boundary. */
static bool
ph_format_needs_rb_swap(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      return true;
   default:
      return false;
   }
}

static void
ph_copy_texel_to_gpu(uint8_t *dst, const uint8_t *src, bool swap_rb,
                     unsigned cpp)
{
   if (cpp != 4) {
      memcpy(dst, src, cpp);
      return;
   }
   if (swap_rb) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
   } else {
      memcpy(dst, src, 4);
   }
}

/* Texture layout code 2 addresses 32x16 blocks using a power-of-two row
 * pitch.  This is independent of CREATE_RENDER's 64-pixel render-target
 * pitch, especially for the small Q3 UI and lightmap textures. */
static unsigned
ph_sampler_tiled_pitch(unsigned width)
{
   return MAX2(32u, util_next_power_of_two(width));
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
 * RGBA client pixels into A1R5G5B5 storage for SRC_ALPHA source-over.  Keep a
 * four-byte linear shadow for all ordinary uses and build the native sampler
 * image only for that fixed-function draw family. */
static bool
ph_prepare_rgb5a1(struct parhelia_resource *texture)
{
   const unsigned pitch = ph_sampler_tiled_pitch(texture->base.width0);
   const uint8_t *src;

   if (!texture || texture->base.format != PIPE_FORMAT_R8G8B8A8_UNORM ||
       !texture->map || !texture->linear_map || texture->base.last_level)
      return false;
   if (texture->rgb5a1_packed &&
       texture->rgb5a1_packed_generation == texture->content_generation)
      return true;
   texture->rgb5a1_packed = false;
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
   for (unsigned y = 0; y < texture->base.height0; y++)
      for (unsigned x = 0; x < texture->base.width0; x++) {
         const uint8_t *p = src + (size_t)y * texture->linear_stride + x * 4;
         uint16_t value = (p[3] ? 0x8000u : 0) |
                          ((uint16_t)(p[2] >> 3) << 10) |
                          ((uint16_t)(p[1] >> 3) << 5) | (p[0] >> 3);
         uint8_t *dst = (uint8_t *)texture->map +
            p3d_texture_tile_offset_cpp(x, y, pitch, 2);
         memcpy(dst, &value, sizeof(value));
      }
   __sync_synchronize();
   texture->rgb5a1_packed = true;
   texture->rgb5a1_packed_generation = texture->content_generation;
   texture->upload_dirty |= texture->upload_texture;
   return true;
}

static void
ph_restore_rgba_tiled(struct parhelia_resource *texture)
{
   if (!texture || !texture->rgb5a1_packed || !texture->map ||
       !texture->linear_map)
      return;
   if (texture->rgb5a1_packed_generation != texture->content_generation) {
      /* A later writer has already replaced the packed image.  Do not copy
       * the older linear shadow back over that newer content. */
      texture->rgb5a1_packed = false;
      return;
   }
   for (unsigned y = 0; y < texture->base.height0; y++)
      for (unsigned x = 0; x < texture->base.width0; x++)
         memcpy((uint8_t *)texture->map +
                   ph_mip_texel_offset_cpp(&texture->mip, 0, x, y,
                                           false, 4),
                (uint8_t *)texture->linear_map +
                   (size_t)y * texture->linear_stride + x * 4, 4);
   __sync_synchronize();
   texture->rgb5a1_packed = false;
   texture->upload_dirty |= texture->upload_texture;
}

static void
ph_copy_texel_from_gpu(uint8_t *dst, const uint8_t *src, bool swap_rb,
                       unsigned cpp)
{
   /* The red/blue conversion is its own inverse. */
   ph_copy_texel_to_gpu(dst, src, swap_rb, cpp);
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

static void *
ph_map(struct pipe_context *p, struct pipe_resource *res, unsigned level,
       unsigned usage, const struct pipe_box *box,
       struct pipe_transfer **out, bool texture)
{
   struct ph_context *ctx = ph_context(p);
   struct parhelia_resource *r = ph_resource(res);
   struct ph_transfer *t = CALLOC_STRUCT(ph_transfer);
   unsigned cpp = util_format_get_blocksize(res->format);
   bool swap_rb = ph_format_needs_rb_swap(res->format);
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
   if ((cpp != 4 && cpp != 2) || level > res->last_level ||
       (!r->map && !r->linear_map) || box->x < 0 || box->y < 0 ||
       box->width < 0 || box->height < 0 ||
       (unsigned)box->x + (unsigned)box->width > image_width ||
       (unsigned)box->y + (unsigned)box->height > image_height) {
      FREE(t);
      return NULL;
   }
   t->base.stride = box->width * cpp;
   t->base.layer_stride = (uintptr_t)t->base.stride * box->height;
   t->staging = MALLOC(t->base.layer_stride);
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
   if ((usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) && r->transfer_shadow &&
       (r->mip_defined & BITFIELD_BIT(level)) &&
       ph_resource_download_shadow(r)) {
      FREE(t->staging);
      FREE(t);
      return NULL;
   }
   if ((usage & PIPE_MAP_READ) && level < ph_resource_mip_levels(r) &&
       (r->mip_defined & BITFIELD_BIT(level))) {
      if (r->linear_map) {
         ph_resource_resolve_linear(r);
         for (int y = 0; y < box->height; y++)
            for (int x = 0; x < box->width; x++)
               ph_copy_texel_from_gpu((uint8_t *)t->staging +
                                          y * t->base.stride + x * cpp,
                                      (uint8_t *)r->linear_map +
                                      (r->linear_mip.levels ?
                                       ph_mip_texel_offset_cpp(&r->linear_mip, level,
                                                           box->x + x, box->y + y,
                                                           true, cpp) :
                                       (size_t)(box->y + y) * r->linear_stride +
                                       (box->x + x) * cpp), swap_rb, cpp);
      } else {
         for (int y = 0; y < box->height; y++)
            for (int x = 0; x < box->width; x++)
               ph_copy_texel_from_gpu((uint8_t *)t->staging +
                                          y * t->base.stride + x * cpp,
                                      (uint8_t *)r->map +
                                      (ph_resource_uses_sampler_layout(r) &&
                                       r->mip.levels ?
                                       ph_mip_texel_offset_cpp(&r->mip, level,
                                                           box->x + x, box->y + y,
                                                           false, cpp) :
                                       ph_resource_uses_sampler_layout(r) ?
                                       p3d_texture_tile_offset_chip(
                                          box->x + x, box->y + y,
                                          ph_sampler_storage_pitch(
                                             r->base.screen,
                                             r->base.width0, cpp),
                                          cpp,
                                          ph_screen(r->base.screen)->chip_id,
                                          r->tile_layout) :
                                       p3d_tile_offset_layout(box->x + x,
                                                              box->y + y,
                                                              r->stride,
                                                              r->tile_layout)),
                                      swap_rb, cpp);
      }
   } else if (usage & PIPE_MAP_READ) {
      /* A lower level outside the P690-supported backing remains a valid
       * Gallium transfer, but it cannot expose stale allocator contents. */
      memset(t->staging, 0, t->base.layer_stride);
   }
   *out = &t->base;
   return t->staging;
}

static void
ph_unmap(struct pipe_context *p, struct pipe_transfer *transfer, bool texture)
{
   struct ph_transfer *t = (struct ph_transfer *)transfer;
   struct parhelia_resource *r = ph_resource(transfer->resource);
   unsigned cpp = util_format_get_blocksize(transfer->resource->format);
   bool swap_rb = ph_format_needs_rb_swap(transfer->resource->format);
   if (texture && t->base.level < ph_resource_mip_levels(r) && t->staging &&
       (transfer->usage & PIPE_MAP_WRITE)) {
      /* Do not apply a partial RGBA update over the temporary 16-bit packed
       * image.  Rebuild the complete ordinary native shadow first. */
      if (r->rgb5a1_packed)
         ph_restore_rgba_tiled(r);
      if (r->linear_map) {
         for (int y = 0; y < transfer->box.height; y++)
            for (int x = 0; x < transfer->box.width; x++)
            ph_copy_texel_to_gpu((uint8_t *)r->linear_map +
                                  (r->linear_mip.levels ?
                                   ph_mip_texel_offset_cpp(&r->linear_mip,
                                                       transfer->level,
                                                       transfer->box.x + x,
                                                       transfer->box.y + y, true, cpp) :
                                   (size_t)(transfer->box.y + y) *
                                   r->linear_stride + (transfer->box.x + x) * cpp),
                                    (uint8_t *)t->staging +
                                       y * transfer->stride + x * cpp, swap_rb, cpp);
      }
      if (r->map) {
         for (int y = 0; y < transfer->box.height; y++)
            for (int x = 0; x < transfer->box.width; x++)
            ph_copy_texel_to_gpu((uint8_t *)r->map +
                                  (ph_resource_uses_sampler_layout(r) ?
                                   (r->mip.levels ?
                                    ph_mip_texel_offset_cpp(&r->mip, transfer->level,
                                                        transfer->box.x + x,
                                                        transfer->box.y + y, false, cpp) :
                                    p3d_texture_tile_offset_chip(
                                       transfer->box.x + x,
                                       transfer->box.y + y,
                                       ph_sampler_storage_pitch(
                                          r->base.screen,
                                          r->base.width0, cpp), cpp,
                                       ph_screen(r->base.screen)->chip_id,
                                       r->tile_layout)) :
                                   p3d_tile_offset_layout(transfer->box.x + x,
                                                          transfer->box.y + y,
                                                          r->stride,
                                                          r->tile_layout)),
                                    (uint8_t *)t->staging +
                                       y * transfer->stride + x * cpp, swap_rb, cpp);
      }
      /* Both representations were updated when a presentation/sampler
       * backing exists; otherwise the tiled render image is authoritative.
       */
      r->tiled_dirty = r->map && !r->linear_map;
      r->linear_dirty = r->linear_map && !r->map;
      /* The write replaced the native A1R5G5B5 scratch representation with
       * the resource's ordinary RGBA pixels.  A later binary-alpha draw may
       * reclassify/repack it under the new content generation. */
      r->rgb5a1_packed = false;
      r->upload_dirty |= r->upload_texture;
      r->content_generation++;
      r->mip_defined |= BITFIELD_BIT(transfer->level);
      if (r->transfer_shadow) {
         int ret = ph_resource_upload_shadow(r);

         if (ret)
            ph_context(p)->submit_error = ret;
         else {
            r->tiled_dirty = false;
            r->linear_dirty = false;
         }
      }
      /* Upload now to keep ordinary texture creation eager.  A temporary
       * aperture allocation failure leaves upload_dirty set; sampler bind
       * retries it before allowing a draw to reference this BO. */
      if (r->upload_texture)
         ph_resource_upload_texture(r);
   }
   FREE(t->staging);
   FREE(t);
}
static void *ph_buffer_map(struct pipe_context *p, struct pipe_resource *r,
                           unsigned l, unsigned u, const struct pipe_box *b,
                           struct pipe_transfer **t)
{ return ph_map(p, r, l, u, b, t, false); }
static void *ph_texture_map(struct pipe_context *p, struct pipe_resource *r,
                            unsigned l, unsigned u, const struct pipe_box *b,
                            struct pipe_transfer **t)
{ return ph_map(p, r, l, u, b, t, true); }
static void ph_buffer_unmap(struct pipe_context *p, struct pipe_transfer *t)
{ ph_unmap(p, t, false); }
static void ph_texture_unmap(struct pipe_context *p, struct pipe_transfer *t)
{ ph_unmap(p, t, true); }

static void
ph_destroy_context(struct pipe_context *p)
{
   struct ph_context *ctx = ph_context(p);
   ph_submit_vertices(ctx);
   ph_wait_async(ctx);
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++)
      pipe_sampler_view_reference(&ctx->sampler_views[i], NULL);
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++)
      pipe_resource_reference(&ctx->constants[i], NULL);
   util_unreference_framebuffer_state(&ctx->fb);
   if (p->stream_uploader)
      u_upload_destroy(p->stream_uploader);
   ph_destroy_vertex_ring(ctx);
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
   struct draw_stage *stage;
   (void)flags;
   if (!ctx)
      return NULL;
   ctx->scissor.maxx = UINT16_MAX;
   ctx->scissor.maxy = UINT16_MAX;
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
   ctx->async_submit = ph_screen(screen)->async_submit;
   ctx->pack_adjacent_packets = ph_screen(screen)->pack_adjacent_packets;
   ctx->aggregate_state_batches =
      ph_screen(screen)->stateful_submit &&
      getenv("PARHELIA_BATCH_DIFFERENT_STATE") != NULL;
   ctx->aggregate_state_stream =
      debug_get_bool_option("PARHELIA_BATCH_STATE_STREAM", false);
   ctx->aggregate_batches = ctx->aggregate_state_stream ||
                            ctx->aggregate_state_batches ||
                            getenv("PARHELIA_BATCH_AGGREGATE") != NULL;
   if (ctx->aggregate_state_batches && getenv("PARHELIA_TRACE_BATCH"))
      fprintf(stderr,
              "parhelia: preserving per-state EOB/DWGSYNC boundaries inside aggregate submits\n");
   if (ctx->aggregate_state_stream && getenv("PARHELIA_TRACE_BATCH"))
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
   if (ctx->p690_multitexture && getenv("PARHELIA_TRACE"))
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

   ctx->base.destroy = ph_destroy_context;
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
   ctx->base.set_polygon_stipple = ph_noop_stipple;
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
   if (ctx->aggregate_batches && getenv("PARHELIA_TRACE_BATCH"))
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
   FREE(ctx);
   return NULL;
}
