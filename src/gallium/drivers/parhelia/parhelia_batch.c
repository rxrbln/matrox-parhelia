/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* DRM adapter for René Rebe's recovered command builder. The scratch primary
 * CALLs are metadata only; the kernel constructs the executable primary span.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "parhelia_batch.h"

static const struct parhelia_batch_limits maximum_batch_limits = {
   .command_dwords = PH_BATCH_DWORDS,
   .packets = PH_BATCH_PACKETS,
   .relocs = PH_BATCH_RELOCS,
   .vertex_dwords = PH_BATCH_VERTEX_DWORDS,
};

void
parhelia_batch_negotiate_limits(uint64_t reported_packets,
                                bool vertex_buffer_mapped,
                                struct parhelia_batch_limits *limits)
{
   unsigned packets = PH_BATCH_LEGACY_PACKETS;

   if (!limits)
      return;
   /* Values beyond our allocation ceiling are rejected rather than silently
    * truncated.  A smaller valid report is honored for compatibility with a
    * kernel whose descriptor validator is tighter than the legacy ABI. */
   if (reported_packets && reported_packets <= PH_BATCH_PACKETS)
      packets = reported_packets;
   if (packets > PH_BATCH_LEGACY_PACKETS && !vertex_buffer_mapped)
      packets = PH_BATCH_LEGACY_PACKETS;
   *limits = (struct parhelia_batch_limits) {
      .command_dwords = packets > PH_BATCH_LEGACY_PACKETS ?
                          PH_BATCH_DWORDS : PH_BATCH_LEGACY_DWORDS,
      .packets = packets,
      .relocs = packets + 4,
      .vertex_dwords = vertex_buffer_mapped ? packets * 3 * 10 :
                                               PH_BATCH_LEGACY_DWORDS,
   };
}

static bool
batch_packet_adds_stage_call(const uint32_t *cmd, unsigned length)
{
   return (length == 8 || length == 10) && cmd[0] == length - 2 &&
          cmd[1] == 0x212121ddu && cmd[4] == 0x20000002u &&
          cmd[5] == cmd[4] &&
          ((length == 8 && cmd[3] == 0x3e403c3cu && cmd[7] == 2) ||
           (length == 10 && cmd[3] == 0x41403c3cu &&
            cmd[6] == cmd[7] && cmd[8] == 0x2121213eu && cmd[9] == 2));
}

static bool
batch_packet_is_opening_watermark(const uint32_t *cmd, unsigned length)
{
   return length == 3 && cmd[0] == 1 && cmd[1] == 0x212121acu &&
          (cmd[2] == 0x3fff8842u || cmd[2] == 0x3fff9084u);
}

int
parhelia_batch_packed_call_count(const struct parhelia_batch *batch,
                                 unsigned first_packet,
                                 unsigned end_packet,
                                 bool pack_adjacent_packets,
                                 unsigned *calls)
{
   enum { PACK_NONE, PACK_DRAW, PACK_RANGE } run = PACK_NONE;
   bool draw_packets[PH_BATCH_PACKETS] = {false};
   bool call_boundaries[PH_BATCH_PACKETS] = {false};
   unsigned opening = first_packet, run_words = 0, count = 3;
   unsigned last_call_boundary = 0;
   bool call_boundary_seen = false;

   if (!batch || !calls || first_packet >= end_packet ||
       end_packet > batch->packet_count ||
       batch->reloc_count > PH_BATCH_RELOCS)
      return -EINVAL;
   for (unsigned i = 0; i < batch->reloc_count; i++) {
      const struct drm_parhelia_reloc *reloc = &batch->relocs[i];

      if (reloc->packet >= batch->packet_count)
         return -EINVAL;
      if (reloc->type == PARHELIA_RELOC_BATCH_BOUNDARY)
         return -EINVAL;
      if (reloc->type == PARHELIA_RELOC_CALL_BOUNDARY) {
         if (!reloc->packet ||
             (call_boundary_seen && reloc->packet <= last_call_boundary) ||
             reloc->dword || reloc->handle || reloc->delta ||
             reloc->pad[0] || reloc->pad[1] || reloc->pad[2])
            return -EINVAL;
         call_boundaries[reloc->packet] = true;
         last_call_boundary = reloc->packet;
         call_boundary_seen = true;
      }
      if (reloc->type == PARHELIA_RELOC_DRAW)
         draw_packets[reloc->packet] = true;
   }
   for (unsigned i = first_packet; i < end_packet; i++) {
      const struct drm_parhelia_packet *packet = &batch->packets[i];

      if (packet->offset > batch->command_count || !packet->length ||
          packet->length > PH_BATCH_MAX_DRAW_DWORDS ||
          packet->length > batch->command_count - packet->offset)
         return -EINVAL;
      if (batch_packet_is_opening_watermark(batch->commands + packet->offset,
                                            packet->length)) {
         opening = i;
         break;
      }
   }
   if (pack_adjacent_packets) {
      bool active = false;
      unsigned previous = 0;

      count = 0;
      for (unsigned i = first_packet; i < end_packet; i++) {
         const struct drm_parhelia_packet *packet = &batch->packets[i];
         const uint32_t *cmd;

         if (packet->offset > batch->command_count || !packet->length ||
             packet->length > PH_BATCH_MAX_DRAW_DWORDS ||
             packet->length > batch->command_count - packet->offset)
            return -EINVAL;
         cmd = batch->commands + packet->offset;
         if (i != first_packet && call_boundaries[i]) {
            count += active;
            active = false;
            run_words = 0;
         }
         if (i == opening) {
            count += active;
            active = false;
            run_words = 0;
            count++; /* Kernel-owned opening fence CALL. */
         }
         if (active && batch->packets[previous].offset +
                          batch->packets[previous].length == packet->offset &&
             packet->length <= PH_BATCH_MAX_DRAW_DWORDS - run_words) {
            run_words += packet->length;
         } else {
            count += active;
            active = true;
            run_words = packet->length;
         }
         previous = i;
         if (batch_packet_adds_stage_call(cmd, packet->length)) {
            count++; /* Flush the run ending in its parent bind. */
            count++; /* Synthetic WARP-stage CALL. */
            active = false;
            run_words = 0;
         }
      }
      count += active;
      *calls = count + 2; /* EOB and terminal drain. */
      return 0;
   }
   for (unsigned i = first_packet; i < end_packet; i++) {
      const struct drm_parhelia_packet *packet = &batch->packets[i];
      const uint32_t *cmd;
      unsigned kind = PACK_NONE;

      if (packet->offset > batch->command_count || !packet->length ||
          packet->length > batch->command_count - packet->offset)
         return -EINVAL;
      cmd = batch->commands + packet->offset;
      if (packet->length == 3 && (cmd[0] >> 24) == 0x97 &&
          draw_packets[i])
         kind = PACK_DRAW;
      else if (packet->length == 3 && cmd[0] == 0x8f000001u)
         kind = PACK_RANGE;

      /* The kernel packs only consecutive packet storage and never crosses
       * its 4096-dword secondary-fetch limit. */
      if (kind != PACK_NONE && run == kind && i > first_packet &&
          batch->packets[i - 1].offset +
             batch->packets[i - 1].length == packet->offset &&
          packet->length <= PH_BATCH_MAX_DRAW_DWORDS - run_words) {
         run_words += packet->length;
      } else {
         count++;
         run = kind;
         run_words = packet->length;
      }
      /* Vertex-program binds acquire a separate synthetic stage CALL after
       * their parent packet, which also terminates any packable run. */
      if (batch_packet_adds_stage_call(cmd, packet->length)) {
         count++;
         run = PACK_NONE;
         run_words = 0;
      }
   }
   *calls = count;
   return 0;
}

int
parhelia_batch_slice(struct parhelia_batch *dst,
                     const struct parhelia_batch *src,
                     const struct parhelia_batch_mark *begin,
                     const struct parhelia_batch_mark *end)
{
   uint64_t vertex_begin, vertex_end;

   if (!dst || !src || !begin || !end ||
       begin->command_dwords > end->command_dwords ||
       begin->packets > end->packets || begin->relocs > end->relocs ||
       begin->vertex_dwords > end->vertex_dwords ||
       end->command_dwords > src->command_count ||
       end->packets > src->packet_count || end->relocs > src->reloc_count ||
       end->vertex_dwords > PH_BATCH_VERTEX_DWORDS ||
       end->command_dwords - begin->command_dwords > PH_BATCH_DWORDS ||
       end->packets - begin->packets > PH_BATCH_PACKETS ||
       end->relocs - begin->relocs > PH_BATCH_RELOCS)
      return -EINVAL;
   vertex_begin = (uint64_t)begin->vertex_dwords * sizeof(uint32_t);
   vertex_end = (uint64_t)end->vertex_dwords * sizeof(uint32_t);
   memset(dst, 0, sizeof(*dst));
   dst->command_count = end->command_dwords - begin->command_dwords;
   dst->packet_count = end->packets - begin->packets;
   dst->reloc_count = 0;
   memcpy(dst->commands, src->commands + begin->command_dwords,
          dst->command_count * sizeof(dst->commands[0]));
   for (unsigned i = 0; i < dst->packet_count; i++) {
      const struct drm_parhelia_packet *packet =
         &src->packets[begin->packets + i];

      if (packet->offset < begin->command_dwords ||
          packet->offset > end->command_dwords ||
          packet->length > end->command_dwords - packet->offset)
         return -EINVAL;
      dst->packets[i] = *packet;
      dst->packets[i].offset -= begin->command_dwords;
   }
   for (unsigned i = begin->relocs; i < end->relocs; i++) {
      const struct drm_parhelia_reloc *reloc =
         &src->relocs[i];
      uint64_t range_end = (uint64_t)reloc->dword + reloc->delta;

      if (reloc->packet < begin->packets || reloc->packet >= end->packets)
         return -EINVAL;
      /* A group sliced at an original state mark starts a fresh submit.  Its
       * leading CALL boundary has no preceding section and must not become
       * the kernel-invalid packet-zero marker. */
      if (reloc->type == PARHELIA_RELOC_CALL_BOUNDARY &&
          reloc->packet == begin->packets)
         continue;
      dst->relocs[dst->reloc_count] = *reloc;
      dst->relocs[dst->reloc_count].packet -= begin->packets;
      if (reloc->type == PARHELIA_RELOC_DRAW) {
         if (reloc->dword < vertex_begin || range_end > vertex_end)
            return -EINVAL;
         dst->relocs[dst->reloc_count].dword -= vertex_begin;
      }
      dst->reloc_count++;
   }
   return 0;
}

static bool
compact_state_packets_enabled(void)
{
   /* Hardware validation covers untextured, depth, one/two-texture and
    * vertex-alpha streams.  Retain an immediate diagnostic escape hatch,
    * read once rather than per state packet. */
   static bool enabled, read;

   if (!read) {
      const char *value = getenv("PARHELIA_COMPACT_STATE_PACKETS");

      enabled = !value || strcmp(value, "0");
      read = true;
   }
   return enabled;
}

static int batch_finish(struct parhelia_batch *batch, const struct p3d_cs *cs,
                        const struct p3d_reloc *calls, uint32_t colour_handle,
                        uint32_t depth_handle, uint32_t texture0_handle,
                        uint32_t texture1_handle,
                        const uint32_t *texture_offsets,
                        uint32_t source_handle,
                        unsigned triangle_count,
                        unsigned vertex_dwords,
                        enum p3d_prim prim,
                        unsigned vertices_per_packet,
                        unsigned total_vertices,
                        bool bound_ranges);

int
parhelia_create_render(int fd, unsigned width, unsigned height,
                       struct drm_parhelia_create_render *allocation)
{
   uint32_t flags;

   if (!allocation || !width || width > 1024 || !height || height > 768)
      return -EINVAL;
   flags = allocation->flags;
   *allocation = (struct drm_parhelia_create_render){
      .width = width,
      .height = height,
      .flags = flags,
   };
   return ioctl(fd, DRM_IOCTL_PARHELIA_CREATE_RENDER, allocation) ? -errno : 0;
}

int
parhelia_batch_clear(struct parhelia_batch *batch, uint32_t colour_handle,
                     unsigned width, unsigned height, unsigned pitch,
                     uint32_t colour)
{
   return parhelia_batch_clear_surface(batch, colour_handle, width, height,
                                       pitch, 0x32, colour);
}

int
parhelia_batch_clear_surface(struct parhelia_batch *batch, uint32_t handle,
                             unsigned width, unsigned height, unsigned pitch,
                             unsigned format, uint32_t value)
{
   return parhelia_batch_clear_rect_surface(batch, handle, width, height,
                                            pitch, format, value, 0xffffffffu,
                                            0, 0, width, height);
}

int
parhelia_batch_clear_rect_surface(struct parhelia_batch *batch,
                                  uint32_t handle, unsigned surface_width,
                                  unsigned surface_height, unsigned pitch,
                                  unsigned format, uint32_t value,
                                  uint32_t write_mask, unsigned x, unsigned y,
                                  unsigned width, unsigned height)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_surface surface = {.pitch = pitch, .fmt = format};

   /* Only the allocation is whole 16-row tile rows; the fill rectangle may
    * start and end inside a tile. */
   if (!batch || !handle || !write_mask || !surface_width ||
       !surface_height || surface_width > pitch || (pitch & 63) ||
       pitch > 0x3fff || (surface_height & 15) || surface_height > 0xffff ||
       !width || !height || x > surface_width || width > surface_width - x ||
       y > surface_height || height > surface_height - y)
      return -EINVAL;
   if (!p3d_surface_format_bytes(format))
      return -EINVAL;
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_clear(&cs, &surface, value, write_mask, x, y, width, height);
   return batch_finish(batch, &cs, calls, handle, 0, 0, 0, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false);
}

int
parhelia_batch_depth_control(struct parhelia_batch *batch,
                             uint32_t depth_handle, unsigned pitch,
                             unsigned format, unsigned slot, uint32_t control,
                             bool load, uint32_t value, uint32_t resume)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_surface surface = {.pitch = pitch, .fmt = format};

   /* Only slot 1 carries a relocatable depth base in batch_finish. */
   if (!batch || !depth_handle || slot != 1 || !pitch || (pitch & 63) ||
       pitch > 0x3fff || !p3d_surface_format_bytes(format))
      return -EINVAL;
   if (control != P3D_ZTRACK_OFF && control != P3D_ZTRACK_ON &&
       control != P3D_ZINIT_LOAD && control != P3D_ZINIT_RESET)
      return -EINVAL;
   /* The kernel's drain replays the last DWGCTL of a batch, so the word the
    * command returns to must not be a Z command itself. */
   if ((resume & 0xfu) == 0x8u || (resume & 0xfu) == 0xau)
      return -EINVAL;
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_depth_control(&cs, &surface, slot, control, load, value, resume);
   return batch_finish(batch, &cs, calls, 0, depth_handle, 0, 0, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false);
}

/* The complete hardware clear: arm the surface, then load the value.  Both
 * commands travel in one batch so nothing can bind another depth surface in
 * between. */
int
parhelia_batch_zclear(struct parhelia_batch *batch, uint32_t depth_handle,
                      unsigned pitch, unsigned format, uint32_t value)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_surface surface = {.pitch = pitch, .fmt = format};

   if (!batch || !depth_handle || !pitch || (pitch & 63) || pitch > 0x3fff ||
       !p3d_surface_format_bytes(format))
      return -EINVAL;
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_depth_control(&cs, &surface, 1, P3D_ZTRACK_ON, false, 0,
                          P3D_DWGCTL_SOLID);
   p3d_emit_depth_control(&cs, &surface, 1, P3D_ZINIT_LOAD, true, value,
                          P3D_DWGCTL_SOLID);
   return batch_finish(batch, &cs, calls, 0, depth_handle, 0, 0, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false);
}

/* Drop the tracked tile state after it has been written out. */
int
parhelia_batch_zdiscard(struct parhelia_batch *batch, uint32_t depth_handle,
                        unsigned pitch, unsigned format)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_surface surface = {.pitch = pitch, .fmt = format};

   if (!batch || !depth_handle || !pitch || (pitch & 63) || pitch > 0x3fff ||
       !p3d_surface_format_bytes(format))
      return -EINVAL;
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_depth_control(&cs, &surface, 1, P3D_ZINIT_RESET, false, 0,
                          P3D_DWGCTL_SOLID);
   p3d_emit_depth_control(&cs, &surface, 1, P3D_ZTRACK_OFF, false, 0,
                          P3D_DWGCTL_SOLID);
   return batch_finish(batch, &cs, calls, 0, depth_handle, 0, 0, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false);
}

int
parhelia_batch_blit(struct parhelia_batch *batch,
                    uint32_t dst_handle, unsigned dst_pitch,
                    unsigned dst_type, unsigned dst_x, unsigned dst_y,
                    uint32_t src_handle, unsigned src_pitch,
                    unsigned src_type, unsigned src_x, unsigned src_y,
                    unsigned width, unsigned height)
{
   return parhelia_batch_blit_format(batch, dst_handle, dst_pitch, 0x32,
                                     dst_type, dst_x, dst_y, src_handle,
                                     src_pitch, 0x32, src_type, src_x, src_y,
                                     width, height);
}

int
parhelia_batch_blit_format(struct parhelia_batch *batch,
                           uint32_t dst_handle, unsigned dst_pitch,
                           unsigned dst_format, unsigned dst_type,
                           unsigned dst_x, unsigned dst_y,
                           uint32_t src_handle, unsigned src_pitch,
                           unsigned src_format, unsigned src_type,
                           unsigned src_x, unsigned src_y,
                           unsigned width, unsigned height)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_surface dst = {.pitch = dst_pitch, .fmt = dst_format};
   struct p3d_surface src = {.pitch = src_pitch, .fmt = src_format};

   /* Both ends move the same bytes per pixel; the BITBLT does not convert. */
   if (!p3d_surface_format_bytes(dst_format) ||
       p3d_surface_format_bytes(dst_format) !=
       p3d_surface_format_bytes(src_format))
      return -EINVAL;
   if (!batch || !dst_handle || !src_handle || !width || !height ||
       dst_type < 1 || dst_type > 2 || src_type < 1 || src_type > 2 ||
       !dst_pitch || dst_pitch > 0x3fff ||
       !src_pitch || src_pitch > 0x3fff ||
       dst_x > dst_pitch || width > dst_pitch - dst_x ||
       src_x > src_pitch || width > src_pitch - src_x ||
       dst_y > 0xffff || src_y > 0xffff ||
       height > 0xffff - dst_y || height > 0xffff - src_y)
      return -EINVAL;
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_blit(&cs, &dst, dst_type, dst_x, dst_y,
                 &src, src_type, src_x, src_y, width, height);
   return batch_finish(batch, &cs, calls, dst_handle, 0, 0, 0, NULL,
                       src_handle, 0, 0, P3D_TRIANGLES, 0, 0, false);
}

int
parhelia_batch_triangles(struct parhelia_batch *batch,
                        uint32_t colour_handle, uint32_t warp_entry,
                        unsigned width, unsigned height, unsigned pitch,
                        unsigned triangle_count)
{
   struct p3d_state state = {0};

   if (!batch || !colour_handle || !width || !height ||
       width > pitch || (pitch & 63) || pitch > 0x3fff ||
       height > 0xffff || !triangle_count || triangle_count > 128)
      return -EINVAL;
   state.colour.pitch = pitch;
   state.colour.fmt = 0x32; /* A8R8G8B8, 64x32 tiled */
   state.screen_w = width;
   state.screen_h = height;
   state.prim = P3D_TRIANGLES;
   state.warp_entry = warp_entry;
   state.dw_per_vertex = 5; /* x, y, r, g, b */
   state.dither = 1;
   /* No client fence: the kernel owns the global counter and final drain. */
   state.fence = 0;
   return parhelia_batch_state(batch, colour_handle, 0, &state, triangle_count);
}

/* Vertex granularity of a run: independent primitives consume a fixed number
 * of vertices each, strips and fans one more per further primitive. */
static unsigned
packet_vertex_step(enum p3d_prim prim)
{
   switch (prim) {
   case P3D_LINESTRIP:
   case P3D_TRISTRIP:
   case P3D_TRIFAN:
      return 1;
   default:
      return p3d_minimum_vertices(prim);
   }
}

/* Vertex counts a single P690 draw packet must not carry.  Nine is a proven
 * trap: --depth-interlock-probe renders a nine-vertex strip with 7186 wrong
 * pixels and the engine then never retires the following draw, while every
 * other count from three to sixty is byte-exact against one packet per
 * primitive.  PARHELIA_PACKET_TRAPS is a comma-separated override for
 * bisecting further counts; PARHELIA_NINE_VERTEX_PACKETS=1 clears the set. */
static bool
packet_vertex_trap(unsigned run)
{
   /* This is asked for every draw packet - over a thousand per frame in
    * ioquake3 - so the environment is read once, not per packet. */
   static const char *traps, *allow;
   static bool read;

   if (!read) {
      traps = getenv("PARHELIA_PACKET_TRAPS");
      allow = getenv("PARHELIA_NINE_VERTEX_PACKETS");
      read = true;
   }
   if (allow && strcmp(allow, "0"))
      return false;
   if (!traps)
      return run == 9;
   for (const char *p = traps; *p;) {
      char *end;
      unsigned long value = strtoul(p, &end, 0);

      if (end == p)
         break;
      if (value == run)
         return true;
      p = *end ? end + 1 : end;
   }
   return false;
}

/* Vertices the next packet of a run may consume: the largest multiple of the
 * primitive's vertex step that is neither longer than what is left nor a
 * trapped count. */
static unsigned
packet_run(enum p3d_prim prim, unsigned per_packet, unsigned left)
{
   const unsigned step = packet_vertex_step(prim);
   unsigned run = left < per_packet ? left : per_packet;

   while (run > step && packet_vertex_trap(run))
      run -= step;
   return run;
}

/* Draw packets a run of @vertices needs, or zero when the run cannot be cut
 * into packets: strips and fans share vertices with their neighbours, so they
 * have to be consumed by a single packet. */
static unsigned
packet_count(enum p3d_prim prim, unsigned per_packet, unsigned vertices)
{
   const unsigned min = p3d_minimum_vertices(prim);
   const bool shared = packet_vertex_step(prim) == 1 && min > 1;
   unsigned packets = 0;

   if (!per_packet || !vertices || vertices < min ||
       vertices % packet_vertex_step(prim))
      return 0;
   while (vertices) {
      const unsigned run = packet_run(prim, per_packet, vertices);

      if (run < min || run > vertices || (shared && run != vertices))
         return 0;
      vertices -= run;
      if (vertices && vertices < min)
         return 0;
      packets++;
   }
   return packets;
}

/* @vertices is the whole run consumed by the @triangle_count draw packets;
 * zero means every packet carries a full state.vertices_per_packet. */
static int
batch_state(struct parhelia_batch *batch, uint32_t colour_handle,
            uint32_t depth_handle, uint32_t texture0_handle,
            uint32_t texture1_handle,
            const struct p3d_state *input, unsigned triangle_count,
            unsigned vertices)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_state state;
   unsigned per_packet;
   int ret;

   if (!batch || !input || !colour_handle || !triangle_count ||
       triangle_count > PH_BATCH_MAX_DRAWS ||
       input->prim >= P3D_PRIM_COUNT ||
       ((input->prim != P3D_TRIANGLES && input->prim != P3D_LINES &&
         input->prim != P3D_POINTS) && !input->vertices_per_packet) ||
       (input->vertices_per_packet &&
        (input->vertices_per_packet > PH_BATCH_MAX_PACKET_VERTICES ||
         input->vertices_per_packet <
            p3d_minimum_vertices(input->prim) ||
         input->vertices_per_packet %
            packet_vertex_step(input->prim))) ||
       !input->screen_w || !input->screen_h || input->screen_h > 0xffff ||
       input->screen_w > input->colour.pitch || (input->colour.pitch & 63) ||
       input->colour.pitch > 0x3fff || input->colour.base ||
       (input->colour.fmt != 0x32 && input->colour.fmt != 0x28) ||
       (!!input->tex != !!texture0_handle) ||
       (!!input->tex1 != !!texture1_handle) ||
       (!input->tex && input->tex1) ||
	       (input->tex && input->tex->base) ||
	       (input->tex1 && input->tex1->base) ||
	       (input->second_texcoord_probe &&
	        (!input->tex || input->tex1 || input->has_depth ||
	         input->vertex_alpha || input->transform)) ||
	       (input->dw_per_vertex != 5u + !!p3d_position_has_z(input) +
	        2 * (!!input->tex + !!input->tex1 +
	             !!input->second_texcoord_probe) + !!input->vertex_alpha) ||
	       (input->sampled_alpha_companion &&
	        (!input->tex || !input->tex1 || input->single_texture_pipe != 1 ||
	         input->vertex_alpha || input->transform || input->comb_op ||
	         input->tex->env != P3D_TEXENV_MODULATE ||
	         input->tex->fmt != 0x32 || input->tex->texelcode != 2 ||
	         input->tex->mip_filter != P3D_MIP_NONE ||
	         input->tex->max_level ||
	         input->tex1->env != P3D_TEXENV_MODULATE ||
	         input->tex1->fmt != 0x32 || input->tex1->texelcode != 2 ||
	         input->tex1->width < 64 || input->tex1->height < 64 ||
	         input->tex1->mip_filter != P3D_MIP_NONE ||
	         input->tex1->max_level ||
	         !((input->alpha_test && !input->blend &&
	            input->alpha_func == P3D_CMP_GEQUAL &&
	            input->alpha_ref == 0x80) ||
	           (!input->alpha_test && input->blend &&
	            input->blend_src.rgb == P3D_BF_SRC_ALPHA.rgb &&
	            input->blend_src.a == P3D_BF_SRC_ALPHA.a &&
	            input->blend_dst.rgb ==
	               P3D_BF_ONE_MINUS_SRC_ALPHA.rgb &&
	            input->blend_dst.a == P3D_BF_ONE_MINUS_SRC_ALPHA.a &&
	            input->blend_eq == P3D_BEQ_ADD)))) ||
	       /* RGBA fetch is restricted to the verified one-texture MODULATE
	        * source-alpha ADD form.  The depth+RGBA alpha-test combination is
	        * deliberately test-only: it proves the descriptor and texture-alpha
	        * route without exposing a Mesa production path. */
	       (input->vertex_alpha && (!input->tex || input->tex1 ||
	                               (input->has_depth !=
	                                !!input->depth_vertex_alpha) ||
	                               (input->depth_vertex_alpha &&
	                                !input->depth_test) ||
	                               input->transform ||
	                               input->pos_desc || input->col_desc ||
	                               input->tex->env != P3D_TEXENV_MODULATE ||
	                               (input->depth_alpha_test_probe ?
	                                (!input->depth_vertex_alpha ||
	                                 !input->alpha_test || input->blend ||
	                                 input->alpha_func != P3D_CMP_GREATER ||
	                                 input->alpha_ref != 0x80) :
	                                (input->alpha_test || !input->blend ||
	                                 input->blend_src.rgb != P3D_BF_SRC_ALPHA.rgb ||
	                                 input->blend_src.a != P3D_BF_SRC_ALPHA.a ||
	                                 (input->depth_vertex_alpha_add ?
	                                  (input->blend_dst.rgb != P3D_BF_ONE.rgb ||
	                                   input->blend_dst.a != P3D_BF_ONE.a) :
	                                  (input->blend_dst.rgb != P3D_BF_ONE_MINUS_SRC_ALPHA.rgb ||
	                                   input->blend_dst.a != P3D_BF_ONE_MINUS_SRC_ALPHA.a)) ||
	                                 input->blend_eq != P3D_BEQ_ADD)))) ||
	       (!input->vertex_alpha &&
	        (input->depth_vertex_alpha || input->depth_vertex_alpha_add ||
	         input->depth_alpha_test_probe)) ||
	       (input->depth_vertex_alpha_add &&
	        (!input->depth_vertex_alpha || input->depth_alpha_test_probe)) ||
       (input->transform && (!input->mvp || !input->const_loader_entry)) ||
       input->base_reg_entry ||
       (input->bound_ranges && input->prim != P3D_TRIANGLES) ||
       /* The two surface slots carry independent pitches; a 16 bpp colour
        * target aligns to 128 pixels where a 32 bpp depth one aligns to 64,
        * so they need not agree. */
       (input->has_depth && (!depth_handle || input->depth.base ||
         (input->depth.fmt != 0x36 && input->depth.fmt != 0x2a) ||
         input->depth.pitch < input->screen_w ||
         (input->depth.pitch & 63) || input->depth.pitch > 0x3fff ||
         input->depth_slot > 1)))
      return -EINVAL;
   per_packet = input->vertices_per_packet ? input->vertices_per_packet :
                p3d_vertices_per_primitive(input->prim);
   if (!per_packet)
      return -EINVAL;
   if (!vertices)
      vertices = triangle_count * per_packet;
   /* The packet count has to match the run the builder will emit. */
   if (packet_count(input->prim, per_packet, vertices) != triangle_count)
      return -EINVAL;
   state = *input;
   state.fence = 0; /* global counter and final drain remain kernel-owned */
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_state(&cs, &state);
   if (state.bound_ranges)
      p3d_emit_bound_ranges(&cs, &state, triangle_count);
   else
      p3d_emit_draws(&cs, &state, triangle_count);
   const uint32_t texture_offsets[2] = {
      state.tex ? state.tex->reloc_offset : 0,
      state.tex1 ? state.tex1->reloc_offset : 0,
   };

   ret = batch_finish(batch, &cs, calls, colour_handle,
                      state.has_depth ? depth_handle : 0,
                      texture0_handle, texture1_handle, texture_offsets, 0,
                      triangle_count, state.dw_per_vertex, state.prim,
                      per_packet, vertices, !!state.bound_ranges);
   /* Q3 reaches this builder with already transformed window-space vertices.
    * Keep the constant-loader/WARP-transform packet cadence outside the first
    * hardware experiment; its exact CALL grouping has separate selftests. */
   if (!ret && !state.transform && compact_state_packets_enabled())
      ret = parhelia_batch_compact_reglists(batch, batch);
   return ret;
}

int
parhelia_batch_state(struct parhelia_batch *batch, uint32_t colour_handle,
                     uint32_t depth_handle, const struct p3d_state *state,
                     unsigned triangle_count)
{
   return batch_state(batch, colour_handle, depth_handle, 0, 0, state,
                      triangle_count, 0);
}

int
parhelia_batch_state_run(struct parhelia_batch *batch, uint32_t colour_handle,
                         uint32_t depth_handle, uint32_t texture0_handle,
                         uint32_t texture1_handle,
                         const struct p3d_state *state, unsigned vertices)
{
   unsigned per_packet;

   if (!state)
      return -EINVAL;
   per_packet = state->vertices_per_packet ? state->vertices_per_packet :
                p3d_vertices_per_primitive(state->prim);
   if (!per_packet || !vertices)
      return -EINVAL;
   return batch_state(batch, colour_handle, depth_handle, texture0_handle,
                      texture1_handle, state,
                      packet_count(state->prim, per_packet, vertices),
                      vertices);
}

int
parhelia_batch_state_textured(struct parhelia_batch *batch,
                              uint32_t colour_handle, uint32_t depth_handle,
                              uint32_t texture_handle,
                              const struct p3d_state *state,
                              unsigned triangle_count)
{
   return batch_state(batch, colour_handle, depth_handle, texture_handle, 0,
                      state, triangle_count, 0);
}

int
parhelia_batch_state_multitextured(struct parhelia_batch *batch,
                                   uint32_t colour_handle,
                                   uint32_t depth_handle,
                                   uint32_t texture0_handle,
                                   uint32_t texture1_handle,
                                   const struct p3d_state *state,
                                   unsigned triangle_count)
{
   return batch_state(batch, colour_handle, depth_handle, texture0_handle,
                      texture1_handle, state, triangle_count, 0);
}

static int
batch_finish(struct parhelia_batch *batch, const struct p3d_cs *cs,
             const struct p3d_reloc *calls, uint32_t colour_handle,
             uint32_t depth_handle, uint32_t texture0_handle,
             uint32_t texture1_handle,
             const uint32_t *texture_offsets,
             uint32_t source_handle,
             unsigned triangle_count,
             unsigned vertex_dwords,
             enum p3d_prim prim,
             unsigned vertices_per_packet,
             unsigned total_vertices,
             bool bound_ranges)
{
   unsigned draws = 0, consumed = 0, texture_units = 0, selected_unit = 0;
   unsigned stream_packet = UINT32_MAX;
   if (cs->error)
      return -E2BIG;

   for (unsigned i = 0; i < cs->rel_len; i++) {
      const struct p3d_reloc *call = &calls[i];
      const uint32_t *cmd = batch->commands + call->sec_off;
      batch->packets[i] = (struct drm_parhelia_packet) {
         .offset = call->sec_off, .length = call->sec_len,
      };
      if (call->is_draw) {
         if (bound_ranges) {
            if (call->sec_len != 3 || cmd[0] != 0x8f000001u ||
                cmd[1] != draws * 3 || cmd[2] != draws * 3 + 3)
               return -EINVAL;
            draws++;
            continue;
         }
         if (call->sec_len != 3 || (cmd[0] >> 24) != 0x97 ||
             consumed >= total_vertices)
            return -EINVAL;
         /* The last packet of a run may be shorter than the rest. */
         const unsigned run = packet_run(prim, vertices_per_packet,
                                         total_vertices - consumed);

         batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
            .packet = i,
            .dword = consumed * vertex_dwords * 4,
            .delta = run * vertex_dwords * 4,
            .type = PARHELIA_RELOC_DRAW,
         };
         consumed += run;
         draws++;
         continue;
      }
      if ((cmd[0] >> 24) == 0x41) {
         /* p3d_emit_constant_group packs multiple header-ful loads into
          * ONE CALL. Preserve those bytes and that boundary exactly.
          */
         unsigned pos = 0;
         while (pos < call->sec_len) {
            if (call->sec_len - pos < 5)
               return -EINVAL;
            unsigned words = (cmd[pos] & 0xffff) + 2;
            unsigned vectors = cmd[pos + 1];
            if (words > call->sec_len - pos || !vectors || vectors > 63 ||
                words != vectors * 4 + 5 ||
                cmd[pos] != (0x41000000u | (vectors << 17) | (words - 2)) ||
                cmd[pos + 3] != 1 || cmd[pos + 4])
               return -EINVAL;
            pos += words;
         }
         continue;
      }
      if (cmd[0] >> 24)
         return -EINVAL;
      if (bound_ranges && call->sec_len == 16 && cmd[0] == 0xe &&
          cmd[1] == 0x03020100u && cmd[6] == 0x07060504u &&
          cmd[11] == 0x13121110u && cmd[12] == vertex_dwords * 4 &&
          !cmd[2] && !cmd[13] && !cmd[14] && !cmd[15]) {
         if (stream_packet != UINT32_MAX)
            return -EINVAL;
         stream_packet = i;
      }
      unsigned pos = 1;
      while (pos < call->sec_len) {
         uint32_t tag = cmd[pos++];
         for (unsigned slot = 0; slot < 4; slot++) {
            unsigned index = (tag >> (8 * slot)) & 0xff;
            if (index == 0x21)
               break;
            if (pos >= call->sec_len)
               return -EINVAL;
            /* 0x1200+4u selects the unit bank of the window at 0x11c0 */
            if (index >= 0x80 && index <= 0x83)
               selected_unit = index - 0x80;
            if (index == 0x77 ||
                (index >= 0x88 && index <= 0x8e && !(index & 1))) {
               /* Even unused surface base writes get a valid GEM target;
                * never rely on a literal VRAM offset zero being harmless.
                */
               if (cmd[pos] || batch->reloc_count >= PH_BATCH_PACKETS + 4)
                  return -EINVAL;
               if (index == 0x77 && selected_unit >= 2)
                  return -EINVAL;
               const unsigned unit = selected_unit;

               if (index == 0x77)
                  texture_units |= 1u << unit;
               batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
                  .packet = i, .dword = pos,
                  .delta = index == 0x77 && texture_offsets ?
                           texture_offsets[unit] : 0,
                  .handle = index == 0x77 ?
                            (unit ? texture1_handle : texture0_handle) :
                            (index == 0x8a && depth_handle ? depth_handle :
                             index == 0x8c && source_handle ? source_handle :
                                                                    colour_handle),
                  .type = PARHELIA_RELOC_VRAM,
               };
            }
            pos++;
         }
      }
   }
   if (texture_units != ((texture0_handle != 0) | (texture1_handle != 0) << 1))
      return -EINVAL;
   if (bound_ranges) {
      if (stream_packet == UINT32_MAX ||
          batch->reloc_count >= PH_BATCH_RELOCS)
         return -EINVAL;
      batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
         .packet = stream_packet,
         .dword = 2,
         .type = PARHELIA_RELOC_RANGE_STREAM_VRAM,
      };
   }
   batch->command_count = cs->sec_len;
   batch->packet_count = cs->rel_len;
   if (!bound_ranges && consumed != total_vertices)
      return -EINVAL;
   return draws == triangle_count ? 0 : -EINVAL;
}

static int
parhelia_batch_submit_flags(int fd, const struct parhelia_batch *batch,
                            const void *vertices, unsigned vertex_dwords,
                            unsigned flags, uint32_t *fence)
{
   struct drm_parhelia_reloc *filtered = NULL;
   const struct drm_parhelia_reloc *relocs;
   unsigned reloc_count;
   int saved_errno;

   if (!batch || (!vertices && vertex_dwords) || !fence || vertex_dwords > 4096)
      return -EINVAL;
   relocs = batch->relocs;
   reloc_count = batch->reloc_count;
   if (!(flags & PARHELIA_SUBMIT_CALL_BOUNDARIES)) {
      for (unsigned i = 0; i < batch->reloc_count; i++)
         if (batch->relocs[i].type == PARHELIA_RELOC_CALL_BOUNDARY) {
            filtered = malloc(batch->reloc_count * sizeof(*filtered));
            if (!filtered)
               return -ENOMEM;
            reloc_count = 0;
            for (unsigned j = 0; j < batch->reloc_count; j++)
               if (batch->relocs[j].type != PARHELIA_RELOC_CALL_BOUNDARY)
                  filtered[reloc_count++] = batch->relocs[j];
            relocs = filtered;
            break;
         }
   }
   struct drm_parhelia_submit submit = {
      .commands = (uintptr_t)batch->commands,
      .packets = (uintptr_t)batch->packets,
      .relocs = (uintptr_t)relocs,
      .vertices = (uintptr_t)vertices,
      .command_dwords = batch->command_count,
      .packet_count = batch->packet_count,
      .reloc_count = reloc_count,
      .vertex_dwords = vertex_dwords,
      .flags = flags,
   };
   /* Do not blindly retry an interrupted submission: replaying rendering
    * can have side effects. Let the caller handle failures explicitly.
    */
   if (ioctl(fd, DRM_IOCTL_PARHELIA_SUBMIT, &submit)) {
      saved_errno = errno;
      free(filtered);
      return -saved_errno;
   }
   free(filtered);
   *fence = submit.fence;
   return 0;
}

int
parhelia_batch_submit(int fd, const struct parhelia_batch *batch,
                     const void *vertices, unsigned vertex_dwords,
                     uint32_t *fence)
{
   return parhelia_batch_submit_flags(fd, batch, vertices, vertex_dwords,
                                      PARHELIA_SUBMIT_FENCE, fence);
}

int
parhelia_batch_submit_async(int fd, const struct parhelia_batch *batch,
                            const void *vertices, unsigned vertex_dwords,
                            uint32_t *fence)
{
   return parhelia_batch_submit_flags(fd, batch, vertices, vertex_dwords,
                                      PARHELIA_SUBMIT_FENCE |
                                      PARHELIA_SUBMIT_ASYNC, fence);
}

int
parhelia_batch_submit_async_bounded(int fd,
                                    const struct parhelia_batch *batch,
                                    const void *vertices,
                                    unsigned vertex_dwords,
                                    uint32_t *fence)
{
   return parhelia_batch_submit_flags(
      fd, batch, vertices, vertex_dwords,
      PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC |
      PARHELIA_SUBMIT_CALL_BOUNDARIES, fence);
}

int
parhelia_batch_append_limited(struct parhelia_batch *dst,
                              uint32_t *dst_vertices,
                              unsigned *dst_vertex_dwords,
                              const struct parhelia_batch *src,
                              const void *src_vertices,
                              unsigned src_vertex_dwords,
                              const struct parhelia_batch_limits *limits)
{
   unsigned command_base, packet_base, vertex_base;

   if (!dst || !dst_vertices || !dst_vertex_dwords || !src || !limits ||
       (!src_vertices && src_vertex_dwords) ||
       !limits->command_dwords || limits->command_dwords > PH_BATCH_DWORDS ||
       !limits->packets || limits->packets > PH_BATCH_PACKETS ||
       limits->relocs < limits->packets || limits->relocs > PH_BATCH_RELOCS ||
       !limits->vertex_dwords ||
       limits->vertex_dwords > PH_BATCH_VERTEX_DWORDS ||
       dst->command_count > limits->command_dwords ||
       dst->packet_count > limits->packets ||
       dst->reloc_count > limits->relocs ||
       *dst_vertex_dwords > limits->vertex_dwords ||
       src->command_count > limits->command_dwords ||
       src->packet_count > limits->packets ||
       src->reloc_count > limits->relocs ||
       src_vertex_dwords > limits->vertex_dwords)
      return -EINVAL;
   if (src->command_count > limits->command_dwords - dst->command_count ||
       src->packet_count > limits->packets - dst->packet_count ||
       src->reloc_count > limits->relocs - dst->reloc_count ||
       src_vertex_dwords > limits->vertex_dwords - *dst_vertex_dwords)
      return -E2BIG;

   for (unsigned i = 0; i < src->packet_count; i++) {
      const struct drm_parhelia_packet *packet = &src->packets[i];

      if (packet->offset > src->command_count ||
          packet->length > src->command_count - packet->offset)
         return -EINVAL;
   }
   for (unsigned i = 0; i < src->reloc_count; i++) {
      const struct drm_parhelia_reloc *reloc = &src->relocs[i];

      /* Production aggregation accepts only the normal state-builder ABI.
       * Mode-7 stream/index offsets have different meanings and remain in
       * their isolated diagnostic submits. */
      if (reloc->packet >= src->packet_count ||
          (reloc->type != PARHELIA_RELOC_VRAM &&
           reloc->type != PARHELIA_RELOC_DRAW))
         return -EINVAL;
      if (reloc->type == PARHELIA_RELOC_DRAW) {
         uint64_t end = (uint64_t)reloc->dword + reloc->delta;

         if (reloc->handle || (reloc->dword & 3) || !reloc->delta ||
             (reloc->delta & 3) ||
             reloc->delta > PH_BATCH_MAX_DRAW_DWORDS * sizeof(uint32_t) ||
             end > (uint64_t)src_vertex_dwords * 4)
            return -EINVAL;
      }
   }

   command_base = dst->command_count;
   packet_base = dst->packet_count;
   vertex_base = *dst_vertex_dwords;
   memcpy(dst->commands + command_base, src->commands,
          src->command_count * sizeof(src->commands[0]));
   for (unsigned i = 0; i < src->packet_count; i++) {
      dst->packets[packet_base + i] = src->packets[i];
      dst->packets[packet_base + i].offset += command_base;
   }
   for (unsigned i = 0; i < src->reloc_count; i++) {
      dst->relocs[dst->reloc_count + i] = src->relocs[i];
      dst->relocs[dst->reloc_count + i].packet += packet_base;
      if (src->relocs[i].type == PARHELIA_RELOC_DRAW)
         dst->relocs[dst->reloc_count + i].dword += vertex_base * 4;
   }
   if (src_vertex_dwords)
      memcpy(dst_vertices + vertex_base, src_vertices,
             src_vertex_dwords * sizeof(dst_vertices[0]));
   dst->command_count += src->command_count;
   dst->packet_count += src->packet_count;
   dst->reloc_count += src->reloc_count;
   *dst_vertex_dwords += src_vertex_dwords;
   return 0;
}

int
parhelia_batch_append(struct parhelia_batch *dst, uint32_t *dst_vertices,
                      unsigned *dst_vertex_dwords,
                      const struct parhelia_batch *src,
                      const void *src_vertices, unsigned src_vertex_dwords)
{
   return parhelia_batch_append_limited(dst, dst_vertices, dst_vertex_dwords,
                                        src, src_vertices, src_vertex_dwords,
                                        &maximum_batch_limits);
}

int
parhelia_batch_append_call_bounded_limited(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords,
   const struct parhelia_batch_limits *limits)
{
   unsigned boundary_reloc, first_packet;
   int ret;

   if (!dst || !src)
      return -EINVAL;
   if (!dst->packet_count)
      return parhelia_batch_append_limited(dst, dst_vertices,
                                           dst_vertex_dwords, src,
                                           src_vertices, src_vertex_dwords,
                                           limits);
   if (!limits || !limits->relocs || limits->relocs > PH_BATCH_RELOCS ||
       dst->reloc_count >= limits->relocs ||
       src->reloc_count >= limits->relocs - dst->reloc_count)
      return -E2BIG;

   boundary_reloc = dst->reloc_count;
   first_packet = dst->packet_count;
   ret = parhelia_batch_append_limited(dst, dst_vertices, dst_vertex_dwords,
                                       src, src_vertices,
                                       src_vertex_dwords, limits);
   if (ret)
      return ret;
   memmove(&dst->relocs[boundary_reloc + 1],
           &dst->relocs[boundary_reloc],
           (dst->reloc_count - boundary_reloc) * sizeof(dst->relocs[0]));
   dst->relocs[boundary_reloc] = (struct drm_parhelia_reloc) {
      .packet = first_packet,
      .type = PARHELIA_RELOC_CALL_BOUNDARY,
   };
   dst->reloc_count++;
   return 0;
}

int
parhelia_batch_append_call_bounded(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords)
{
   return parhelia_batch_append_call_bounded_limited(
      dst, dst_vertices, dst_vertex_dwords, src, src_vertices,
      src_vertex_dwords, &maximum_batch_limits);
}

int
parhelia_batch_append_same_state_limited(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords,
   const struct parhelia_batch_limits *limits)
{
   struct parhelia_batch *tail;
   unsigned dst_draw = dst ? dst->packet_count : 0;
   unsigned src_draw = src ? src->packet_count : 0;
   unsigned dst_state_relocs = 0, src_state_relocs = 0;
   uint32_t draw_delta = 0;
   int ret;

   if (!dst || !src)
      return -EINVAL;
   if (!dst->packet_count)
      return parhelia_batch_append_limited(dst, dst_vertices,
                                           dst_vertex_dwords, src,
                                           src_vertices, src_vertex_dwords,
                                           limits);
   for (unsigned i = 0; i < dst->packet_count; i++)
      if ((dst->commands[dst->packets[i].offset] >> 24) == 0x97) {
         dst_draw = i;
         break;
      }
   for (unsigned i = 0; i < src->packet_count; i++)
      if ((src->commands[src->packets[i].offset] >> 24) == 0x97) {
         src_draw = i;
         break;
      }
   if (!dst_draw || dst_draw == dst->packet_count || dst_draw != src_draw ||
       src_draw == src->packet_count ||
       dst->packets[dst_draw].offset != src->packets[src_draw].offset ||
       memcmp(dst->commands, src->commands,
              src->packets[src_draw].offset * sizeof(src->commands[0])))
      return -EAGAIN;
   for (unsigned i = dst_draw; i < dst->packet_count; i++)
      if ((dst->commands[dst->packets[i].offset] >> 24) != 0x97)
         return -EAGAIN;
   for (unsigned i = src_draw; i < src->packet_count; i++)
      if ((src->commands[src->packets[i].offset] >> 24) != 0x97)
         return -EAGAIN;

   for (unsigned i = 0; i < dst->reloc_count; i++) {
      const struct drm_parhelia_reloc *reloc = &dst->relocs[i];

      if (reloc->type == PARHELIA_RELOC_DRAW) {
         if (!draw_delta)
            draw_delta = reloc->delta;
      } else if (reloc->packet < dst_draw) {
         dst_state_relocs++;
      } else {
         return -EAGAIN;
      }
   }
   for (unsigned i = 0; i < src->reloc_count; i++) {
      const struct drm_parhelia_reloc *reloc = &src->relocs[i];

      if (reloc->type == PARHELIA_RELOC_DRAW) {
         if (reloc->packet < src_draw || !draw_delta ||
             reloc->delta != draw_delta)
            return -EAGAIN;
      } else if (reloc->packet < src_draw) {
         src_state_relocs++;
      } else {
         return -EAGAIN;
      }
   }
   if (dst_state_relocs != src_state_relocs)
      return -EAGAIN;
   for (unsigned di = 0, si = 0; di < dst->reloc_count; di++) {
      const struct drm_parhelia_reloc *dr = &dst->relocs[di];

      if (dr->type == PARHELIA_RELOC_DRAW || dr->packet >= dst_draw)
         continue;
      while (si < src->reloc_count &&
             (src->relocs[si].type == PARHELIA_RELOC_DRAW ||
              src->relocs[si].packet >= src_draw))
         si++;
      if (si == src->reloc_count ||
          memcmp(dr, &src->relocs[si++], sizeof(*dr)))
         return -EAGAIN;
   }

   tail = malloc(sizeof(*tail));
   if (!tail)
      return -ENOMEM;
   tail->command_count = 0;
   tail->packet_count = 0;
   tail->reloc_count = 0;
   for (unsigned i = src_draw; i < src->packet_count; i++) {
      const struct drm_parhelia_packet *packet = &src->packets[i];

      if (packet->length > PH_BATCH_DWORDS - tail->command_count) {
         ret = -E2BIG;
         goto out;
      }
      tail->packets[tail->packet_count++] = (struct drm_parhelia_packet) {
         .offset = tail->command_count,
         .length = packet->length,
      };
      memcpy(tail->commands + tail->command_count,
             src->commands + packet->offset,
             packet->length * sizeof(src->commands[0]));
      tail->command_count += packet->length;
      for (unsigned r = 0; r < src->reloc_count; r++)
         if (src->relocs[r].packet == i) {
            if (src->relocs[r].type != PARHELIA_RELOC_DRAW ||
                tail->reloc_count == PH_BATCH_RELOCS) {
               ret = -EAGAIN;
               goto out;
            }
            tail->relocs[tail->reloc_count] = src->relocs[r];
            tail->relocs[tail->reloc_count++].packet = tail->packet_count - 1;
         }
   }
   if (tail->reloc_count != tail->packet_count) {
      ret = -EAGAIN;
      goto out;
   }
   ret = parhelia_batch_append_limited(dst, dst_vertices, dst_vertex_dwords,
                                       tail, src_vertices,
                                       src_vertex_dwords, limits);
out:
   free(tail);
   return ret;
}

int
parhelia_batch_append_same_state(struct parhelia_batch *dst,
                                 uint32_t *dst_vertices,
                                 unsigned *dst_vertex_dwords,
                                 const struct parhelia_batch *src,
                                 const void *src_vertices,
                                 unsigned src_vertex_dwords)
{
   return parhelia_batch_append_same_state_limited(
      dst, dst_vertices, dst_vertex_dwords, src, src_vertices,
      src_vertex_dwords, &maximum_batch_limits);
}

int
parhelia_batch_append_stateful_limited(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords,
   const struct parhelia_batch_limits *limits)
{
   unsigned boundary_reloc;
   int ret;

   if (!dst || !src)
      return -EINVAL;
   if (!dst->packet_count)
      return parhelia_batch_append_limited(dst, dst_vertices,
                                           dst_vertex_dwords, src,
                                           src_vertices, src_vertex_dwords,
                                           limits);
   for (unsigned i = 0; i < dst->reloc_count; i++)
      if (dst->relocs[i].type == PARHELIA_RELOC_BATCH_BOUNDARY)
         return -E2BIG;
   if (!limits || !limits->relocs || limits->relocs > PH_BATCH_RELOCS ||
       dst->reloc_count >= limits->relocs ||
       src->reloc_count >= limits->relocs - dst->reloc_count)
      return -E2BIG;

   /* parhelia_batch_append() validates all source ranges and capacity before
    * changing dst.  Reserve the boundary only after that validation succeeds,
    * then move it ahead of the newly appended relocations. */
   boundary_reloc = dst->reloc_count;
   ret = parhelia_batch_append_limited(dst, dst_vertices, dst_vertex_dwords,
                                       src, src_vertices,
                                       src_vertex_dwords, limits);
   if (ret)
      return ret;
   memmove(&dst->relocs[boundary_reloc + 1],
           &dst->relocs[boundary_reloc],
           (dst->reloc_count - boundary_reloc) * sizeof(dst->relocs[0]));
   dst->relocs[boundary_reloc] = (struct drm_parhelia_reloc) {
      .packet = dst->packet_count - src->packet_count,
      .type = PARHELIA_RELOC_BATCH_BOUNDARY,
   };
   dst->reloc_count++;
   return 0;
}

int
parhelia_batch_append_stateful(struct parhelia_batch *dst,
                               uint32_t *dst_vertices,
                               unsigned *dst_vertex_dwords,
                               const struct parhelia_batch *src,
                               const void *src_vertices,
                               unsigned src_vertex_dwords)
{
   return parhelia_batch_append_stateful_limited(
      dst, dst_vertices, dst_vertex_dwords, src, src_vertices,
      src_vertex_dwords, &maximum_batch_limits);
}

#define PH_COMPACT_REGLIST_DWORDS 64

/* Some indexed registers are latches, FIFO ports, or synchronization points.
 * The vendor emits large ordinary register lists, but retains boundaries at
 * these classes.  Keep the first experiment equally conservative: only lists
 * which avoid every known ordering-sensitive class may be joined. */
static bool
compact_reg_barrier(unsigned index)
{
   if (index == (P3D_R_STREAM0 - P3D_REG_BASE) / 4 ||
       index == (P3D_R_STRIDE0 - P3D_REG_BASE) / 4 ||
       index == (P3D_R_VTXCAP - P3D_REG_BASE) / 4 ||
       index == (P3D_R_VTXCAP_HI - P3D_REG_BASE) / 4 ||
       index == (P3D_R_PRIMWORD - P3D_REG_BASE) / 4 ||
       index == (P3D_R_DESC_INDEX - P3D_REG_BASE) / 4 ||
       index == (P3D_R_DESC - P3D_REG_BASE) / 4 ||
       index == (P3D_R_WARP_MODE - P3D_REG_BASE) / 4 ||
       index == (P3D_R_WARP_PAGE - P3D_REG_BASE) / 4 ||
       index == (P3D_R_WARP_ENTRY - P3D_REG_BASE) / 4 ||
       index == (P3D_R_WARP_ENTRY2 - P3D_REG_BASE) / 4 ||
       index == (P3D_R_PIPECTL - P3D_REG_BASE) / 4 ||
       index == (P3D_R_WATERMARK - P3D_REG_BASE) / 4 ||
       index == (P3D_R_EOBTAG - P3D_REG_BASE) / 4 ||
       index == (P3D_R_FENCE - P3D_REG_BASE) / 4 ||
       index == (P3D_R_DWGCTL - P3D_REG_BASE) / 4 ||
       index == (P3D_R_STROBE - P3D_REG_BASE) / 4)
      return true;

   /* 0x1200..0x120c select a texture bank for the window at 0x11c0, and
    * 0x1220..0x123c describe render/depth surfaces. */
   return (index >= 0x80 && index <= 0x83) ||
          (index >= 0x88 && index <= 0x8f);
}

static bool
compact_reglist_packet(const uint32_t *cmd, unsigned length)
{
   unsigned pos = 1;

   if (!cmd || length < 2 || (cmd[0] & 0xffff0000u) ||
       (cmd[0] & 0xffffu) + 2u != length)
      return false;
   while (pos < length) {
      uint32_t tag = cmd[pos++];

      for (unsigned slot = 0; slot < 4; slot++) {
         unsigned index = (tag >> (slot * 8)) & 0xff;

         if (index == 0x21)
            break;
         /* 0x20 is a hardware pad selector rather than an ordinary indexed
          * register.  Our builder uses 0x21 termination, so leave any legacy
          * padded packet byte-exact instead of guessing its data count. */
         if (index == 0x20 || pos == length || compact_reg_barrier(index))
            return false;
         pos++;
      }
   }
   return pos == length;
}

int
parhelia_batch_compact_reglists(struct parhelia_batch *dst,
                                const struct parhelia_batch *src)
{
   struct parhelia_batch *out;
   unsigned packet_map[PH_BATCH_PACKETS];
   bool previous_compactable = false;
   int ret = 0;

   if (!dst || !src || src->command_count > PH_BATCH_DWORDS ||
       src->packet_count > PH_BATCH_PACKETS ||
       src->reloc_count > PH_BATCH_RELOCS)
      return -EINVAL;
   for (unsigned r = 0; r < src->reloc_count; r++)
      if (src->relocs[r].packet >= src->packet_count)
         return -EINVAL;
   out = malloc(sizeof(*out));
   if (!out)
      return -ENOMEM;
   out->command_count = 0;
   out->packet_count = 0;
   out->reloc_count = 0;

   /* A range-stream relocation owns the first packet of an exact direct-fetch
    * tail.  That packet and every packet after it are already barriers (stream,
    * primitive, bounds, descriptor FIFO, then 0x8f ranges), so they remain
    * byte-exact naturally.  Ordinary state packets before the tail may still
    * be joined; the relocation's packet owner is remapped below. */

   for (unsigned i = 0; i < src->packet_count; i++) {
      const struct drm_parhelia_packet *packet = &src->packets[i];
      const uint32_t *cmd;
      bool has_reloc = false, compactable, merge;

      if (packet->offset > src->command_count ||
          packet->length > src->command_count - packet->offset) {
         ret = -EINVAL;
         goto out_free;
      }
      cmd = src->commands + packet->offset;
      for (unsigned r = 0; r < src->reloc_count; r++)
         has_reloc |= src->relocs[r].packet == i;
      compactable = !has_reloc && compact_reglist_packet(cmd, packet->length);
      merge = compactable && previous_compactable && out->packet_count &&
              out->packets[out->packet_count - 1].length <=
                 PH_COMPACT_REGLIST_DWORDS &&
              packet->length - 1 <= PH_COMPACT_REGLIST_DWORDS -
                                     out->packets[out->packet_count - 1].length;

      if (merge) {
         struct drm_parhelia_packet *previous =
            &out->packets[out->packet_count - 1];
         uint32_t *previous_cmd = out->commands + previous->offset;

         memcpy(out->commands + out->command_count, cmd + 1,
                (packet->length - 1) * sizeof(cmd[0]));
         out->command_count += packet->length - 1;
         previous->length += packet->length - 1;
         previous_cmd[0] = (previous_cmd[0] & 0xffff0000u) |
                           (previous->length - 2);
         packet_map[i] = out->packet_count - 1;
      } else {
         if (out->packet_count == PH_BATCH_PACKETS ||
             packet->length > PH_BATCH_DWORDS - out->command_count) {
            ret = -E2BIG;
            goto out_free;
         }
         packet_map[i] = out->packet_count;
         out->packets[out->packet_count++] = (struct drm_parhelia_packet) {
            .offset = out->command_count,
            .length = packet->length,
         };
         memcpy(out->commands + out->command_count, cmd,
                packet->length * sizeof(cmd[0]));
         out->command_count += packet->length;
      }
      previous_compactable = compactable;
   }

   for (unsigned r = 0; r < src->reloc_count; r++) {
      out->relocs[r] = src->relocs[r];
      out->relocs[r].packet = packet_map[src->relocs[r].packet];
   }
   out->reloc_count = src->reloc_count;
   memcpy(dst->commands, out->commands,
          out->command_count * sizeof(out->commands[0]));
   memcpy(dst->packets, out->packets,
          out->packet_count * sizeof(out->packets[0]));
   memcpy(dst->relocs, out->relocs,
          out->reloc_count * sizeof(out->relocs[0]));
   dst->command_count = out->command_count;
   dst->packet_count = out->packet_count;
   dst->reloc_count = out->reloc_count;
out_free:
   free(out);
   return ret;
}

int
parhelia_wait_fence(int fd, uint32_t fence, int64_t timeout_ns,
                    uint32_t *completed)
{
   struct drm_parhelia_wait_fence wait = {
      .fence = fence,
      .timeout_ns = timeout_ns,
   };

   if (!fence)
      return 0;
   if (ioctl(fd, DRM_IOCTL_PARHELIA_WAIT_FENCE, &wait))
      return -errno;
   if (completed)
      *completed = wait.completed;
   return 0;
}

int
parhelia_batch_vram_relocs(const struct parhelia_batch *batch,
                           uint32_t vertex_handle, uint32_t vertex_offset,
                           unsigned vertex_dwords,
                           struct drm_parhelia_reloc *out,
                           unsigned out_count)
{
   uint64_t bytes, end;
   bool saw_vertex_source = false, saw_range = false;

   if (!batch || !vertex_handle || !vertex_dwords || !out ||
       out_count < batch->reloc_count || (vertex_offset & 3))
      return -EINVAL;
   bytes = (uint64_t)vertex_dwords * sizeof(uint32_t);
   if (bytes > UINT32_MAX || vertex_offset > UINT32_MAX - bytes)
      return -EINVAL;
   for (unsigned i = 0; i < batch->reloc_count; i++) {
      const struct drm_parhelia_reloc *src = &batch->relocs[i];

      out[i] = *src;
      if (src->type == PARHELIA_RELOC_RANGE_STREAM_VRAM) {
         if (saw_vertex_source || src->handle || src->delta)
            return -EINVAL;
         out[i].handle = vertex_handle;
         out[i].delta = vertex_offset;
         saw_vertex_source = true;
         saw_range = true;
         continue;
      }
      if (src->type != PARHELIA_RELOC_DRAW)
         continue;
      if (saw_range || src->handle || (src->dword & 3) || !src->delta ||
          (src->delta & 3) ||
          src->delta > PH_BATCH_MAX_DRAW_DWORDS * sizeof(uint32_t))
         return -EINVAL;
      end = (uint64_t)src->dword + src->delta;
      if (end > bytes)
         return -EINVAL;
      out[i].dword = vertex_offset + src->dword;
      out[i].handle = vertex_handle;
      out[i].type = PARHELIA_RELOC_DRAW_VRAM;
      saw_vertex_source = true;
   }
   return saw_vertex_source ? 0 : -EINVAL;
}

int
parhelia_batch_submit_vram(int fd, const struct parhelia_batch *batch,
                           uint32_t vertex_handle, uint32_t vertex_offset,
                           unsigned vertex_dwords, uint32_t *fence)
{
   struct drm_parhelia_reloc relocs[PH_BATCH_RELOCS];
   unsigned reloc_count = 0;
   int ret;

   if (!batch || !fence || batch->reloc_count > PH_BATCH_RELOCS)
      return -EINVAL;
   ret = parhelia_batch_vram_relocs(batch, vertex_handle, vertex_offset,
                                    vertex_dwords, relocs,
                                    PH_BATCH_RELOCS);
   if (ret)
      return ret;
   for (unsigned i = 0; i < batch->reloc_count; i++)
      if (relocs[i].type != PARHELIA_RELOC_CALL_BOUNDARY)
         relocs[reloc_count++] = relocs[i];
   struct drm_parhelia_submit submit = {
      .commands = (uintptr_t)batch->commands,
      .packets = (uintptr_t)batch->packets,
      .relocs = (uintptr_t)relocs,
      .command_dwords = batch->command_count,
      .packet_count = batch->packet_count,
      .reloc_count = reloc_count,
      .flags = PARHELIA_SUBMIT_FENCE,
   };
   if (ioctl(fd, DRM_IOCTL_PARHELIA_SUBMIT, &submit))
      return -errno;
   *fence = submit.fence;
   return 0;
}

int
parhelia_batch_submit_vram_async(int fd, const struct parhelia_batch *batch,
                                 uint32_t vertex_handle,
                                 uint32_t vertex_offset,
                                 unsigned vertex_dwords, uint32_t *fence)
{
   struct drm_parhelia_reloc relocs[PH_BATCH_RELOCS];
   struct drm_parhelia_submit submit;
   unsigned reloc_count = 0;
   int ret;

   if (!batch || !fence || batch->reloc_count > PH_BATCH_RELOCS)
      return -EINVAL;
   ret = parhelia_batch_vram_relocs(batch, vertex_handle, vertex_offset,
                                    vertex_dwords, relocs,
                                    PH_BATCH_RELOCS);
   if (ret)
      return ret;
   for (unsigned i = 0; i < batch->reloc_count; i++)
      if (relocs[i].type != PARHELIA_RELOC_CALL_BOUNDARY)
         relocs[reloc_count++] = relocs[i];
   submit = (struct drm_parhelia_submit) {
      .commands = (uintptr_t)batch->commands,
      .packets = (uintptr_t)batch->packets,
      .relocs = (uintptr_t)relocs,
      .command_dwords = batch->command_count,
      .packet_count = batch->packet_count,
      .reloc_count = reloc_count,
      .flags = PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC,
   };
   if (ioctl(fd, DRM_IOCTL_PARHELIA_SUBMIT, &submit))
      return -errno;
   *fence = submit.fence;
   return 0;
}

int
parhelia_batch_submit_vram_async_bounded(
   int fd, const struct parhelia_batch *batch, uint32_t vertex_handle,
   uint32_t vertex_offset, unsigned vertex_dwords, uint32_t *fence)
{
   struct drm_parhelia_reloc relocs[PH_BATCH_RELOCS];
   struct drm_parhelia_submit submit;
   int ret;

   if (!batch || !fence || batch->reloc_count > PH_BATCH_RELOCS)
      return -EINVAL;
   ret = parhelia_batch_vram_relocs(batch, vertex_handle, vertex_offset,
                                    vertex_dwords, relocs,
                                    PH_BATCH_RELOCS);
   if (ret)
      return ret;
   submit = (struct drm_parhelia_submit) {
      .commands = (uintptr_t)batch->commands,
      .packets = (uintptr_t)batch->packets,
      .relocs = (uintptr_t)relocs,
      .command_dwords = batch->command_count,
      .packet_count = batch->packet_count,
      .reloc_count = batch->reloc_count,
      .flags = PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC |
               PARHELIA_SUBMIT_CALL_BOUNDARIES,
   };
   if (ioctl(fd, DRM_IOCTL_PARHELIA_SUBMIT, &submit))
      return -errno;
   *fence = submit.fence;
   return 0;
}

/* This is intentionally not a general indexed-draw API.  It is the smallest
 * possible command stream that exercises the kernel's mode-7 relocation
 * contract: one interleaved stream, a four-vertex floor/limit and a
 * six-element GL_UNSIGNED_INT triangle list. A nonzero vertex_handle selects
 * the class-0 CREATE_VERTEX probe; zero selects the test-only sealed class-3
 * DMA transport. Keeping it separate prevents
 * experimental mode-7 assumptions from reaching Gallium's normal 0x97 path.
 */
int
parhelia_batch_mode7_screen_test(struct parhelia_batch *batch,
                                 uint32_t colour_handle,
                                 uint32_t vertex_handle,
                                 unsigned width, unsigned height,
                                 unsigned pitch, unsigned warp_entry,
                                 unsigned vertex_dwords,
                                 unsigned vertex_count,
                                 unsigned vertex_offset,
                                 unsigned index_offset,
                                 unsigned index_count)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_state state = {
      .colour = {.pitch = pitch, .fmt = 0x32},
      .screen_w = width,
      .screen_h = height,
      .prim = P3D_TRIANGLES,
      .warp_entry = warp_entry,
      .dw_per_vertex = vertex_dwords,
      .dither = 1,
   };
   struct p3d_rw prim[] = {
      /* mtx3661 selects packet/direct-fetch context mode 2; mtx3319 then
       * clears bit 18 and emits 0x0002305c.  The normal copied 0x97 path uses
       * the independently proven bit-18 form, so change it only here. */
      {.reg = P3D_R_PRIMWORD,
       .val = p3d_primword(P3D_TRIANGLES) & ~0x00040000u},
   };
   struct p3d_rw bounds[] = {
      {.reg = P3D_R_VTXCAP_HI, .val = 0},
      {.reg = P3D_R_VTXCAP, .val = vertex_count},
   };
   /* The vendor's direct-fetch emitter snapshots every stream base and
    * packed stride before it re-emits primitive, bounds and descriptors.
    * Mode 7 did not retire when this test used the normal 0x97 shortcut
    * (only stream 0 and one stride); preserve this exact single-stream form
    * for the isolated hardware test. */
   struct p3d_rw stream[] = {
      {.reg = P3D_R_STREAM0, .val = 0},
      {.reg = P3D_R_STREAM0 + 4, .val = 0},
      {.reg = P3D_R_STREAM0 + 8, .val = 0},
      {.reg = P3D_R_STREAM0 + 12, .val = 0},
      {.reg = P3D_R_STREAM0 + 16, .val = 0},
      {.reg = P3D_R_STREAM0 + 20, .val = 0},
      {.reg = P3D_R_STREAM0 + 24, .val = 0},
      {.reg = P3D_R_STREAM0 + 28, .val = 0},
      {.reg = P3D_R_STRIDE0, .val = vertex_dwords * sizeof(uint32_t)},
      {.reg = P3D_R_STRIDE0 + 4, .val = 0},
      {.reg = P3D_R_STRIDE0 + 8, .val = 0},
      {.reg = P3D_R_STRIDE0 + 12, .val = 0},
   };
   struct p3d_rw desc_reset[] = {
      {.reg = P3D_R_DESC_INDEX, .val = 0},
   };
   struct p3d_rw desc[] = {
      {.reg = P3D_R_DESC, .val = 0x20c00000u},
      {.reg = P3D_R_DESC, .val = 0xa1c50020u},
      {.reg = P3D_R_DESC, .val = 0},
   };
   unsigned stream_packet, index_packet, surface_bases = 0;

   if (!batch || !colour_handle || !width || !height ||
       width > pitch || (pitch & 63) || pitch > 0x3fff || height > 0xffff ||
       vertex_dwords != 5 || vertex_count != 4 || (vertex_offset & 3) ||
       (index_offset & 3) || index_count != 6 ||
       vertex_offset > UINT32_MAX - vertex_count * vertex_dwords * 4 ||
       index_offset < vertex_offset + vertex_count * vertex_dwords * 4)
      return -EINVAL;

   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_state(&cs, &state);
   stream_packet = cs.rel_len;
   p3d_reglist(&cs, stream, sizeof(stream) / sizeof(stream[0]));
   p3d_reglist(&cs, prim, sizeof(prim) / sizeof(prim[0]));
   p3d_reglist(&cs, bounds, sizeof(bounds) / sizeof(bounds[0]));
   p3d_reglist(&cs, desc_reset, sizeof(desc_reset) / sizeof(desc_reset[0]));
   p3d_reglist(&cs, desc, sizeof(desc) / sizeof(desc[0]));
   index_packet = cs.rel_len;
   p3d_indexpacket_u32(&cs);
   if (cs.error || cs.rel_len != index_packet + 1 ||
       !calls[stream_packet].sec_len || calls[index_packet].sec_len != 3)
      return -E2BIG;

   /* Copy the ordinary state relocation policy, but admit exactly one
    * structural mode-7 packet.  All command addresses remain zero here. */
   for (unsigned i = 0; i < cs.rel_len; i++) {
      const struct p3d_reloc *call = &calls[i];
      const uint32_t *cmd = batch->commands + call->sec_off;

      batch->packets[i] = (struct drm_parhelia_packet) {
         .offset = call->sec_off, .length = call->sec_len,
      };
      if ((cmd[0] >> 24) == 0x41)
         continue;
      if ((cmd[0] >> 24) == 0x9f) {
         if (i != index_packet || cmd[0] != 0x9f000001u || cmd[1] || cmd[2])
            return -EINVAL;
         continue;
      }
      if (cmd[0] >> 24)
         return -EINVAL;
      for (unsigned pos = 1; pos < call->sec_len;) {
         uint32_t tag = cmd[pos++];
         for (unsigned slot = 0; slot < 4; slot++) {
            unsigned index = (tag >> (slot * 8)) & 0xff;

            if (index == 0x21)
               break;
            if (pos >= call->sec_len)
               return -EINVAL;
            if (index == 0x88 || index == 0x8a || index == 0x8c ||
                index == 0x8e) {
               /* p3d_emit_state writes both target slot 0 and the inactive
                * source slot 2.  The normal builder relocates both to the
                * colour allocation, so keep this small diagnostic stream
                * structurally identical. */
               if (cmd[pos] || (index != 0x88 && index != 0x8c))
                  return -EINVAL;
               surface_bases++;
               batch->relocs[batch->reloc_count++] =
                  (struct drm_parhelia_reloc) {
                     .packet = i, .dword = pos, .handle = colour_handle,
                     .type = PARHELIA_RELOC_VRAM,
                  };
            }
            pos++;
         }
      }
   }
   if (surface_bases != 2 || batch->reloc_count + 2 > PH_BATCH_RELOCS)
      return -EINVAL;
   /* The full direct-fetch snapshot begins [header][base tag][base0...].
    * Dword 2 is its uniquely relocatable stream-0 operand. */
   if (batch->packets[stream_packet].length != 16 ||
       batch->commands[batch->packets[stream_packet].offset + 2] ||
       batch->commands[batch->packets[stream_packet].offset + 1] != 0x03020100u ||
       batch->commands[batch->packets[stream_packet].offset + 6] != 0x07060504u ||
       batch->commands[batch->packets[stream_packet].offset + 11] != 0x13121110u ||
       batch->commands[batch->packets[stream_packet].offset + 12] !=
          vertex_dwords * sizeof(uint32_t))
      return -EINVAL;
   batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
      .packet = stream_packet, .dword = 2, .handle = vertex_handle,
      .delta = vertex_offset,
      .type = vertex_handle ? PARHELIA_RELOC_STREAM_VRAM : PARHELIA_RELOC_STREAM_DMA,
   };
   batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
      .packet = index_packet, .dword = index_offset, .handle = vertex_handle,
      .delta = index_count * sizeof(uint32_t),
      .type = vertex_handle ? PARHELIA_RELOC_INDEX_VRAM : PARHELIA_RELOC_INDEX_DMA,
   };
   batch->command_count = cs.sec_len;
   batch->packet_count = cs.rel_len;
   return 0;
}

int
parhelia_batch_mode3_screen_test(struct parhelia_batch *batch,
                                 uint32_t colour_handle,
                                 uint32_t depth_handle,
                                 unsigned width, unsigned height,
                                 unsigned pitch, unsigned warp_entry,
                                 enum parhelia_mode3_range_case range_case)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_state state = {
      .colour = {.pitch = pitch, .fmt = 0x32},
      .screen_w = width, .screen_h = height, .prim = P3D_TRIANGLES,
      .warp_entry = warp_entry, .dw_per_vertex = depth_handle ? 6 : 5,
      .dither = 1, .has_depth = !!depth_handle,
      .depth_test = !!depth_handle, .depth_write = !!depth_handle,
      .depth_func = P3D_CMP_LESS,
      .depth = {.pitch = pitch, .fmt = 0x36}, .depth_slot = 1,
   };
   struct p3d_rw stream[] = {
      {.reg = P3D_R_STREAM0, .val = 0},
      {.reg = P3D_R_STREAM0 + 4, .val = 0},
      {.reg = P3D_R_STREAM0 + 8, .val = 0},
      {.reg = P3D_R_STREAM0 + 12, .val = 0},
      {.reg = P3D_R_STREAM0 + 16, .val = 0},
      {.reg = P3D_R_STREAM0 + 20, .val = 0},
      {.reg = P3D_R_STREAM0 + 24, .val = 0},
      {.reg = P3D_R_STREAM0 + 28, .val = 0},
      {.reg = P3D_R_STRIDE0, .val = depth_handle ? 24 : 20},
      {.reg = P3D_R_STRIDE0 + 4, .val = 0},
      {.reg = P3D_R_STRIDE0 + 8, .val = 0},
      {.reg = P3D_R_STRIDE0 + 12, .val = 0},
   };
   struct p3d_rw prim[] = {
      {.reg = P3D_R_PRIMWORD,
       .val = p3d_primword(P3D_TRIANGLES) & ~0x00040000u},
   };
   struct p3d_rw bounds[] = {
      {.reg = P3D_R_VTXCAP_HI, .val = 0},
      {.reg = P3D_R_VTXCAP, .val = 6},
   };
   struct p3d_rw desc_reset[] = {{.reg = P3D_R_DESC_INDEX, .val = 0}};
   struct p3d_rw desc[] = {
      {.reg = P3D_R_DESC, .val = depth_handle ? 0x21c00000u : 0x20c00000u},
      {.reg = P3D_R_DESC, .val = depth_handle ? 0xa1c50030u : 0xa1c50020u},
      {.reg = P3D_R_DESC, .val = 0},
   };
   unsigned stream_packet, range_packet, surface_bases = 0;

   if (!batch || !colour_handle || !width || !height || width > pitch ||
       (pitch & 63) || pitch > 0x3fff || height > 0xffff ||
       range_case > PARHELIA_MODE3_TWO_PACKETS ||
       (!depth_handle && range_case != PARHELIA_MODE3_ONE_RANGE))
      return -EINVAL;
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   p3d_emit_state(&cs, &state);
   stream_packet = cs.rel_len;
   p3d_reglist(&cs, stream, sizeof(stream) / sizeof(stream[0]));
   p3d_reglist(&cs, prim, sizeof(prim) / sizeof(prim[0]));
   p3d_reglist(&cs, bounds, sizeof(bounds) / sizeof(bounds[0]));
   p3d_reglist(&cs, desc_reset, sizeof(desc_reset) / sizeof(desc_reset[0]));
   p3d_reglist(&cs, desc, sizeof(desc) / sizeof(desc[0]));
   range_packet = cs.rel_len;
   if (range_case == PARHELIA_MODE3_ONE_RANGE)
      p3d_rangepacket(&cs, 0, 6);
   else if (range_case == PARHELIA_MODE3_TWO_RANGES_ONE_PACKET)
      p3d_rangepacket2(&cs, 0, 3, 3, 6);
   else {
      p3d_rangepacket(&cs, 0, 3);
      p3d_rangepacket(&cs, 3, 6);
   }
   if (cs.error || cs.rel_len != range_packet +
          (range_case == PARHELIA_MODE3_TWO_PACKETS ? 2 : 1))
      return -E2BIG;

   for (unsigned i = 0; i < cs.rel_len; i++) {
      const struct p3d_reloc *call = &calls[i];
      const uint32_t *cmd = batch->commands + call->sec_off;

      batch->packets[i] = (struct drm_parhelia_packet) {
         .offset = call->sec_off, .length = call->sec_len,
      };
      if ((cmd[0] >> 24) == 0x41)
         continue;
      if ((cmd[0] >> 24) == 0x8f) {
         if (range_case == PARHELIA_MODE3_ONE_RANGE) {
            if (i != range_packet || call->sec_len != 3 ||
                cmd[0] != 0x8f000001u || cmd[1] || cmd[2] != 6)
               return -EINVAL;
         } else if (range_case == PARHELIA_MODE3_TWO_RANGES_ONE_PACKET) {
            if (i != range_packet || call->sec_len != 5 ||
                cmd[0] != 0x8f000003u || cmd[1] || cmd[2] != 3 ||
                cmd[3] != 3 || cmd[4] != 6)
               return -EINVAL;
         } else if (i == range_packet) {
            if (call->sec_len != 3 || cmd[0] != 0x8f000001u ||
                cmd[1] || cmd[2] != 3)
               return -EINVAL;
         } else if (i == range_packet + 1) {
            if (call->sec_len != 3 || cmd[0] != 0x8f000001u ||
                cmd[1] != 3 || cmd[2] != 6)
               return -EINVAL;
         } else {
            return -EINVAL;
         }
         continue;
      }
      if (cmd[0] >> 24)
         return -EINVAL;
      for (unsigned pos = 1; pos < call->sec_len;) {
         uint32_t tag = cmd[pos++];

         for (unsigned slot = 0; slot < 4; slot++) {
            unsigned index = (tag >> (slot * 8)) & 0xff;

            if (index == 0x21)
               break;
            if (pos >= call->sec_len)
               return -EINVAL;
            if (index == 0x88 || index == 0x8a || index == 0x8c ||
                index == 0x8e) {
               if (cmd[pos] || index == 0x8e ||
                   (index == 0x8a && !depth_handle))
                  return -EINVAL;
               surface_bases++;
               batch->relocs[batch->reloc_count++] =
                  (struct drm_parhelia_reloc) {
                     .packet = i, .dword = pos,
                     .handle = index == 0x8a ? depth_handle : colour_handle,
                     .type = PARHELIA_RELOC_VRAM,
                  };
            }
            pos++;
         }
      }
   }
   if (surface_bases != (depth_handle ? 3 : 2) ||
       batch->reloc_count + 1 > PH_BATCH_RELOCS ||
       batch->packets[stream_packet].length != 16 ||
       batch->commands[batch->packets[stream_packet].offset + 1] != 0x03020100u ||
       batch->commands[batch->packets[stream_packet].offset + 2] ||
       batch->commands[batch->packets[stream_packet].offset + 6] != 0x07060504u ||
       batch->commands[batch->packets[stream_packet].offset + 11] != 0x13121110u ||
       batch->commands[batch->packets[stream_packet].offset + 12] !=
          (depth_handle ? 24 : 20))
      return -EINVAL;
   batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
      .packet = stream_packet, .dword = 2,
      .type = PARHELIA_RELOC_RANGE_STREAM_DMA,
   };
   batch->command_count = cs.sec_len;
   batch->packet_count = cs.rel_len;
   return 0;
}
