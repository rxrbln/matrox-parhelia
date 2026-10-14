/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* DRM adapter for René Rebe's recovered command builder. The scratch primary
 * CALLs are metadata only; the kernel constructs the executable primary span.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
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
      if (reloc->type == PH_RELOC_STREAM_BASE) {
         if (reloc->delta < vertex_begin || reloc->delta >= vertex_end)
            return -EINVAL;
         dst->relocs[dst->reloc_count].delta -= vertex_begin;
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

static int compact_transform = -1;

void
parhelia_batch_set_compact_transform(bool enable)
{
   compact_transform = enable;
}

static bool
compact_transform_packets(void)
{
   if (compact_transform < 0) {
      const char *value = getenv("PARHELIA_COMPACT_TRANSFORM");

      compact_transform = !value || strcmp(value, "0");
   }
   return compact_transform;
}

static int batch_finish(struct parhelia_batch *batch, const struct p3d_cs *cs,
                        const struct p3d_reloc *calls, uint32_t colour_handle,
                        uint32_t depth_handle,
                        const uint32_t *texture_handles,
                        const uint32_t *texture_offsets,
                        uint32_t source_handle,
                        unsigned triangle_count,
                        unsigned vertex_dwords,
                        enum p3d_prim prim,
                        unsigned vertices_per_packet,
                        unsigned total_vertices,
                        bool bound_ranges,
                        unsigned indexed_vertices);

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

/* Split 2-D fills into the vendor's column strips; the screen enables it per
 * chip family, PARHELIA_FILL_STRIPS=0|1 overrides. */
static int ph_fill_strips_family;

void
parhelia_batch_set_fill_strips(bool enable)
{
   ph_fill_strips_family = enable;
}

static bool
ph_fill_strips(void)
{
   const char *v = getenv("PARHELIA_FILL_STRIPS");

   return v ? strcmp(v, "0") != 0 : ph_fill_strips_family;
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
   if (ph_fill_strips() && width > 32) {
      /* The P-series vendor fill (mtxpserd.dll 0x112480..0x112630): a first
       * strip to the next 32-pixel boundary, then full-height 32-pixel
       * strips refired by DSTXY and LEN alone.  On the P690 a 1280x1024
       * fill runs at 676 instead of 482 Mpixel/s (32 bpp), 1311 instead of
       * 771 (16 bpp), pixel-exact. */
      unsigned sx = x, sw = 32 - (x & 31);

      p3d_emit_clear(&cs, &surface, value, write_mask, sx, y, sw, height);
      for (sx += sw; sx < x + width; sx += sw) {
         struct p3d_rw rw[2];

         sw = x + width - sx < 32u ? x + width - sx : 32u;
         rw[0].reg = 0x1308; rw[0].val = (y << 16) | (sx & 0xffffu);
         rw[1].reg = 0x138c; rw[1].val = (height << 16) | sw;
         p3d_reglist(&cs, rw, 2);
      }
   } else {
      p3d_emit_clear(&cs, &surface, value, write_mask, x, y, width, height);
   }
   return batch_finish(batch, &cs, calls, handle, 0, NULL, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false, 0);
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
       control != P3D_ZINIT_LOAD && control != P3D_ZINIT_RESET &&
       control != P3D_ZINIT_LOAD_ORIGINAL &&
       control != P3D_ZINIT_RESET_ORIGINAL)
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
   return batch_finish(batch, &cs, calls, 0, depth_handle, NULL, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false, 0);
}

/* The complete hardware clear: arm the surface, then load the value.  Both
 * commands travel in one batch so nothing can bind another depth surface in
 * between. */
static int
batch_zclear(struct parhelia_batch *batch, uint32_t colour_handle,
             const struct p3d_surface *colour, uint32_t depth_handle,
             unsigned pitch, unsigned format, uint32_t value, uint32_t load)
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
   if (colour) {
      const struct p3d_rw rw[2] = {
         {P3D_R_SURF0_BASE, 0}, {P3D_R_SURF0_BASE + 4, p3d_surface_cfg(colour)},
      };

      p3d_reglist(&cs, rw, 2);
   }
   p3d_emit_depth_control(&cs, &surface, 1, P3D_ZTRACK_ON, false, 0,
                          P3D_DWGCTL_SOLID);
   p3d_emit_depth_control(&cs, &surface, 1, load, true, value,
                          P3D_DWGCTL_SOLID);
   return batch_finish(batch, &cs, calls, colour_handle, depth_handle, NULL,
                       NULL, 0, 0, 0, P3D_TRIANGLES, 0, 0, false, 0);
}

int
parhelia_batch_zclear(struct parhelia_batch *batch, uint32_t depth_handle,
                      unsigned pitch, unsigned format, uint32_t value)
{
   return batch_zclear(batch, 0, NULL, depth_handle, pitch, format, value,
                       P3D_ZINIT_LOAD);
}

/* The original family's form: its own load word, and the colour target
 * bound in slot 0 first as the vendor rebinds both slots before the load -
 * its Z unit addresses depth relative to the colour target, and the kernel
 * pins a depth surface into that window only for such a pair. */
int
parhelia_batch_zclear_original(struct parhelia_batch *batch,
                             uint32_t colour_handle, unsigned colour_pitch,
                             unsigned colour_format, uint32_t depth_handle,
                             unsigned pitch, unsigned format, uint32_t value)
{
   const struct p3d_surface colour = {.pitch = colour_pitch,
                                      .fmt = colour_format};

   if (!colour_handle || !colour_pitch ||
       !p3d_surface_format_bytes(colour_format))
      return -EINVAL;
   return batch_zclear(batch, colour_handle, &colour, depth_handle, pitch,
                       format, value, P3D_ZINIT_LOAD_ORIGINAL);
}

/* The Z-init load with an explicit DWGCTL word: 0x208 uncompressed, 0x008
 * with Z compression engaged (mtxpserd.dll 0x9eff0). */
int
parhelia_batch_zclear_word(struct parhelia_batch *batch, uint32_t depth_handle,
                           unsigned pitch, unsigned format, uint32_t value,
                           uint32_t word)
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
   p3d_emit_depth_control(&cs, &surface, 1, word, true, value,
                          P3D_DWGCTL_SOLID);
   return batch_finish(batch, &cs, calls, 0, depth_handle, NULL, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false, 0);
}

int
parhelia_batch_zcontrol(struct parhelia_batch *batch, uint32_t depth_handle,
                        unsigned pitch, unsigned format, const uint32_t *words,
                        unsigned count)
{
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_surface surface = {.pitch = pitch, .fmt = format};

   if (!batch || !depth_handle || !pitch || (pitch & 63) || pitch > 0x3fff ||
       !p3d_surface_format_bytes(format) || !count || count > 8)
      return -EINVAL;
   batch->command_count = 0;
   batch->packet_count = 0;
   batch->reloc_count = 0;
   p3d_cs_init(&cs, batch->commands, PH_BATCH_LEGACY_DWORDS, ring,
               PH_BATCH_PACKETS * 5, calls, PH_BATCH_PACKETS);
   for (unsigned i = 0; i < count; i++)
      p3d_emit_depth_control(&cs, &surface, 1, words[i], false, 0,
                             P3D_DWGCTL_SOLID);
   return batch_finish(batch, &cs, calls, 0, depth_handle, NULL, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false, 0);
}

/* Drop the tracked tile state after it has been written out. */
static int
batch_zdiscard(struct parhelia_batch *batch, uint32_t depth_handle,
               unsigned pitch, unsigned format, uint32_t reset)
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
   p3d_emit_depth_control(&cs, &surface, 1, reset, false, 0,
                          P3D_DWGCTL_SOLID);
   p3d_emit_depth_control(&cs, &surface, 1, P3D_ZTRACK_OFF, false, 0,
                          P3D_DWGCTL_SOLID);
   return batch_finish(batch, &cs, calls, 0, depth_handle, NULL, NULL, 0, 0, 0,
                       P3D_TRIANGLES, 0, 0, false, 0);
}

int
parhelia_batch_zdiscard(struct parhelia_batch *batch, uint32_t depth_handle,
                        unsigned pitch, unsigned format)
{
   return batch_zdiscard(batch, depth_handle, pitch, format, P3D_ZINIT_RESET);
}

int
parhelia_batch_zdiscard_original(struct parhelia_batch *batch,
                                 uint32_t depth_handle, unsigned pitch,
                                 unsigned format)
{
   return batch_zdiscard(batch, depth_handle, pitch, format,
                         P3D_ZINIT_RESET_ORIGINAL);
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
   return batch_finish(batch, &cs, calls, dst_handle, 0, NULL, NULL,
                       src_handle, 0, 0, P3D_TRIANGLES, 0, 0, false, 0);
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
            uint32_t depth_handle,
            const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS],
            const struct p3d_state *input, unsigned triangle_count,
            unsigned vertices)
{
   const unsigned units = input ? p3d_state_units(input) : 0;
   bool units_ok = true;
   struct p3d_reloc calls[PH_BATCH_PACKETS];
   uint32_t ring[PH_BATCH_PACKETS * 5];
   struct p3d_cs cs;
   struct p3d_state state;
   unsigned per_packet;
   int ret;

   /* a handle exactly for every active unit, at relocatable base zero */
   for (unsigned u = 0; input && u < P3D_MAX_TEXTURE_UNITS; u++)
      units_ok &= !!texture_handles[u] == (u < units) &&
                  (u < units || !p3d_state_unit(input, u)) &&
                  (u >= units || !p3d_state_unit(input, u)->base);
   /* shared sets: every unit's coordinate map nibble names a carried set */
   if (input && input->coord_sets)
      for (unsigned u = 0; u < units; u++) {
         const unsigned o = (input->coord_map >> (4 * u)) & 0xfu;

         units_ok &= o >= 3 && o < 3 + input->coord_sets;
      }
   if (!batch || !input || !colour_handle || !triangle_count || !units_ok ||
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
	       (input->second_texcoord_probe &&
	        (!input->tex || input->tex1 || input->has_depth ||
	         input->vertex_alpha || input->transform)) ||
	       (input->dw_per_vertex != 5u + !!p3d_position_has_z(input) +
	        !!input->oow +
	        2 * (p3d_state_coord_sets(input) +
	             !!input->second_texcoord_probe) +
	        !!input->vertex_alpha +
	        (input->vertex_colour1 ? 3u + !!(input->vertex_colour1 & 2) +
	                                 !!(input->vertex_colour1 & 4) : 0u) +
	        2u * (input->texcoord_q && !input->texcoord3) -
	        (input->packed_colour ? 2u + !!input->vertex_alpha : 0u)) ||
	       /* one packed colour; the second colour stays float */
	       (input->packed_colour &&
	        (input->vertex_colour1 || input->texcoord3)) ||
	       /* The projective set is measured for one 2-D unit or volume. */
	       (input->texcoord_q &&
	        (!input->tex || input->tex1 ||
	         input->vertex_alpha || input->vertex_colour1 ||
	         input->second_texcoord_probe || input->pos_desc ||
	         input->sampled_alpha_companion ||
	         input->tex->coord_mode != (input->texcoord3 ?
	                                    P3D_TEX_COORD_3D_PROJ :
	                                    P3D_TEX_COORD_2D_PROJ) ||
	         (input->texcoord3 &&
	          input->tex->type != P3D_TEX_TYPE_VOLUME))) ||
	       /* 1/w follows z; the second colour and the three-coordinate
	        * and probe layouts do not carry it. */
	       (input->oow && ((input->vertex_colour1 && !input->tex) ||
	                       input->texcoord3 ||
	                       input->second_texcoord_probe)) ||
	       /* The second colour rides untextured draws and one-set textured
	        * draws of the original family. */
	       (input->vertex_colour1 &&
	        (input->vertex_alpha ||
	         (input->tex && (input->tex1 || input->texcoord3 ||
	                         input->single_texture_pipe ||
	                         p3d_state_coord_sets(input) != 1)))) ||
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
	            input->blend_eq == P3D_BEQ_ADD &&
	            !input->blend_eq_alpha_separate)))) ||
	       /* RGBA fetch is restricted to the verified one-texture MODULATE
	        * source-alpha ADD form.  The depth+RGBA alpha-test combination is
	        * deliberately test-only: it proves the descriptor and texture-alpha
	        * route without exposing a Mesa production path. */
	       /* Untextured RGBA is the measured x,y,z,r,g,b,a stream only. */
	       (input->vertex_alpha && !input->tex &&
	        (!p3d_position_has_z(input) || input->depth_vertex_alpha ||
	         input->pos_desc || input->col_desc)) ||
	       (input->vertex_alpha && input->tex && (input->tex1 ||
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
	                                 input->blend_eq != P3D_BEQ_ADD ||
	                                 input->blend_eq_alpha_separate)))) ||
	       (!input->vertex_alpha &&
	        (input->depth_vertex_alpha || input->depth_vertex_alpha_add ||
	         input->depth_alpha_test_probe)) ||
	       (input->depth_vertex_alpha_add &&
	        (!input->depth_vertex_alpha || input->depth_alpha_test_probe)) ||
       /* The constants may come from the fixed-function matrix or from a
        * compiled program's own uniform block; either satisfies transform. */
       (input->transform &&
        ((!input->mvp && !(input->vs_const && input->vs_const_vec4s)) ||
         !input->const_loader_entry)) ||
       /* Constants without a transform: the loader has to be bound and the
        * wanted blocks named, since the default mask asks for the viewport
        * pair a non-transforming program never set. */
       (!input->transform && input->const_loader_entry &&
        (!input->const_mask ||
         (!(input->vs_const && input->vs_const_vec4s) &&
          !(input->aux_const && input->aux_const_vec4s)))) ||
       (input->aux_const && input->aux_const_vec4s > 63) ||
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
   if (state.indexed_vertices)
      p3d_emit_indexed(&cs, &state, triangle_count);
   else if (state.bound_ranges)
      p3d_emit_bound_ranges(&cs, &state, triangle_count);
   else
      p3d_emit_draws(&cs, &state, triangle_count);
   uint32_t texture_offsets[P3D_MAX_TEXTURE_UNITS] = { 0 };

   for (unsigned u = 0; u < units; u++)
      texture_offsets[u] = p3d_state_unit(&state, u)->reloc_offset;

   ret = batch_finish(batch, &cs, calls, colour_handle,
                      state.has_depth ? depth_handle : 0,
                      texture_handles, texture_offsets, 0,
                      triangle_count, state.dw_per_vertex, state.prim,
                      per_packet, vertices, !!state.bound_ranges,
                      state.indexed_vertices);
   /* Transform batches compact too (PARHELIA_COMPACT_TRANSFORM=0 keeps
    * their exact packet cadence for diagnosis). */
   if (!ret && (!state.transform || compact_transform_packets()) &&
       compact_state_packets_enabled())
      ret = parhelia_batch_compact_reglists(batch, batch);
   return ret;
}

int
parhelia_batch_state(struct parhelia_batch *batch, uint32_t colour_handle,
                     uint32_t depth_handle, const struct p3d_state *state,
                     unsigned triangle_count)
{
   static const uint32_t none[P3D_MAX_TEXTURE_UNITS];

   return batch_state(batch, colour_handle, depth_handle, none, state,
                      triangle_count, 0);
}

int
parhelia_batch_state_run_units(struct parhelia_batch *batch,
                               uint32_t colour_handle, uint32_t depth_handle,
                               const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS],
                               const struct p3d_state *state,
                               unsigned vertices)
{
   unsigned per_packet;

   if (!state)
      return -EINVAL;
   per_packet = state->vertices_per_packet ? state->vertices_per_packet :
                p3d_vertices_per_primitive(state->prim);
   if (!per_packet || !vertices)
      return -EINVAL;
   return batch_state(batch, colour_handle, depth_handle, texture_handles,
                      state, packet_count(state->prim, per_packet, vertices),
                      vertices);
}

/* The state prefix of a batch - everything before its draw packets, after
 * compaction - depends only on the state and the objects it names, and
 * ioquake3 re-emits a few hundred distinct states per frame.  Build each
 * once: a hit copies the prefix and appends the draw packets and their
 * DRAW relocations exactly as batch_finish() does.  The key is the state
 * with every pointer replaced by the data it points at, compared in full. */
struct state_cache_entry {
   uint64_t hash;
   uint8_t *key;
   size_t key_len;
   uint32_t *commands;
   struct drm_parhelia_packet *packets;
   struct drm_parhelia_reloc *relocs;
   unsigned command_count, packet_count, reloc_count;
   int mvp_offset;               /* the matrix's dwords in commands, or -1 */
   int calls;                    /* packed CALLs of the batch alone, or -1 */
};

struct parhelia_state_cache {
   struct state_cache_entry **slots;
   unsigned cap, count;
   uint8_t *key;                 /* the key being looked up */
   size_t key_len, key_cap;
   bool verify;
   unsigned long hits, misses;
   int last_calls;               /* the last batch's entry calls, or -1 */
};

/* Entries before the cache is flushed and refilled. */
#define STATE_CACHE_MAX_ENTRIES 4096u

struct parhelia_state_cache *
parhelia_state_cache_create(void)
{
   struct parhelia_state_cache *c = calloc(1, sizeof(*c));
   const char *verify = getenv("PARHELIA_STATE_CACHE_VERIFY");

   if (c)
      c->verify = verify && strcmp(verify, "0");
   return c;
}

static void
state_cache_entry_free(struct state_cache_entry *e)
{
   if (!e)
      return;
   free(e->key);
   free(e->commands);
   free(e->packets);
   free(e->relocs);
   free(e);
}

static void
state_cache_flush(struct parhelia_state_cache *c)
{
   for (unsigned i = 0; i < c->cap; i++) {
      state_cache_entry_free(c->slots[i]);
      c->slots[i] = NULL;
   }
   c->count = 0;
}

void
parhelia_state_cache_destroy(struct parhelia_state_cache *c)
{
   if (!c)
      return;
   if (c->verify)
      fprintf(stderr, "parhelia: state cache %lu hits, %lu misses, %u entries\n",
              c->hits, c->misses, c->count);
   state_cache_flush(c);
   free(c->slots);
   free(c->key);
   free(c);
}

static bool
state_key_put(struct parhelia_state_cache *c, const void *data, size_t len)
{
   if (c->key_len + len > c->key_cap) {
      size_t cap = c->key_cap ? c->key_cap : 1024;
      uint8_t *grown;

      while (cap < c->key_len + len)
         cap *= 2;
      grown = realloc(c->key, cap);
      if (!grown)
         return false;
      c->key = grown;
      c->key_cap = cap;
   }
   memcpy(c->key + c->key_len, data, len);
   c->key_len += len;
   return true;
}

static bool
state_key_texture(struct parhelia_state_cache *c, const struct p3d_texture *t)
{
   struct p3d_texture copy;
   const uint8_t present = !!t;

   if (!state_key_put(c, &present, 1))
      return false;
   if (!t)
      return true;
   copy = *t;
   copy.aux = NULL;
   return state_key_put(c, &copy, sizeof(copy)) &&
          (!t->aux || state_key_put(c, t->aux, 5 * sizeof(uint32_t)));
}

static bool
state_key_floats(struct parhelia_state_cache *c, const float *v, size_t n)
{
   const uint8_t present = !!v;

   return state_key_put(c, &present, 1) &&
          (!v || state_key_put(c, v, n * sizeof(float)));
}

/* The fixed-function matrix reaches the stream only verbatim, as c0..c3 of
 * the constant group's first load (p3d_emit_constant_group()), so it stays
 * out of the key and is written into the cached prefix instead. */
static bool
state_mvp_patchable(const struct p3d_state *st)
{
   const unsigned mask = st->const_mask ? st->const_mask : 7u;

   return st->mvp && (mask & 1u) &&
          !(st->vs_const && st->vs_const_vec4s && st->vs_const_vec4s <= 63);
}

static bool
state_key_build(struct parhelia_state_cache *c, const struct p3d_state *input,
                uint32_t colour_handle, uint32_t depth_handle,
                const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS])
{
   struct p3d_state s = *input;
   const uint8_t fog = !!input->fog_table, comb = !!input->comb;

   s.fence = 0;
   s.fog_table = NULL;
   s.mvp = s.vs_const = s.aux_const = NULL;
   s.comb = NULL;
   s.tex = s.tex1 = s.tex2 = s.tex3 = NULL;
   c->key_len = 0;
   return state_key_put(c, &s, sizeof(s)) &&
          state_key_put(c, &colour_handle, sizeof(colour_handle)) &&
          state_key_put(c, &depth_handle, sizeof(depth_handle)) &&
          state_key_put(c, texture_handles,
                        P3D_MAX_TEXTURE_UNITS * sizeof(uint32_t)) &&
          state_key_put(c, &fog, 1) &&
          (!fog || state_key_put(c, input->fog_table, 64 * sizeof(uint32_t))) &&
          (state_mvp_patchable(input) ?
           state_key_put(c, &(const uint8_t){ 2 }, 1) :
           state_key_floats(c, input->mvp, 16)) &&
          state_key_floats(c, input->vs_const, input->vs_const_vec4s * 4u) &&
          state_key_floats(c, input->aux_const, input->aux_const_vec4s * 4u) &&
          state_key_put(c, &comb, 1) &&
          (!comb || (state_key_put(c, &input->comb->count,
                                   sizeof(input->comb->count)) &&
                     state_key_put(c, input->comb->inst,
                                   (input->comb->count <
                                    P3D_COMB_MAX_INSTRUCTIONS ?
                                    input->comb->count :
                                    P3D_COMB_MAX_INSTRUCTIONS) *
                                   sizeof(input->comb->inst[0])))) &&
          state_key_texture(c, input->tex) &&
          state_key_texture(c, input->tex1) &&
          state_key_texture(c, input->tex2) &&
          state_key_texture(c, input->tex3);
}

/* Four independent lanes over the ~1 KiB key: one multiply chain was a
 * quarter of a cache hit's time. */
static uint64_t
state_key_hash(const uint8_t *key, size_t len)
{
   uint64_t h[4] = {
      0x9e3779b97f4a7c15ull ^ len, 0xc2b2ae3d27d4eb4full,
      0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull,
   };
   uint64_t r;
   size_t i = 0;

   for (; i + 32 <= len; i += 32) {
      for (unsigned l = 0; l < 4; l++) {
         uint64_t w;

         memcpy(&w, key + i + l * 8, 8);
         h[l] = (h[l] ^ w) * 0xff51afd7ed558ccdull;
         h[l] ^= h[l] >> 32;
      }
   }
   r = h[0] ^ (h[1] * 0x9e3779b97f4a7c15ull) ^
       (h[2] * 0xc2b2ae3d27d4eb4full) ^ (h[3] * 0x165667b19e3779f9ull);
   for (; i < len; i++)
      r = (r ^ key[i]) * 0x100000001b3ull;
   return r ^ (r >> 29);
}

static struct state_cache_entry **
state_cache_find(struct parhelia_state_cache *c, uint64_t hash)
{
   unsigned i = (unsigned)hash & (c->cap - 1);

   while (c->slots[i]) {
      struct state_cache_entry *e = c->slots[i];

      if (e->hash == hash && e->key_len == c->key_len &&
          !memcmp(e->key, c->key, c->key_len))
         return &c->slots[i];
      i = (i + 1) & (c->cap - 1);
   }
   return &c->slots[i];
}

static bool
state_cache_grow(struct parhelia_state_cache *c)
{
   unsigned cap = c->cap ? c->cap * 2 : 256;
   struct state_cache_entry **old = c->slots;
   unsigned old_cap = c->cap;

   c->slots = calloc(cap, sizeof(*c->slots));
   if (!c->slots) {
      c->slots = old;
      return false;
   }
   c->cap = cap;
   for (unsigned i = 0; i < old_cap; i++) {
      unsigned j;

      if (!old[i])
         continue;
      j = (unsigned)old[i]->hash & (cap - 1);
      while (c->slots[j])
         j = (j + 1) & (cap - 1);
      c->slots[j] = old[i];
   }
   free(old);
   return true;
}

/* The prefix of a freshly built batch: its packets before the first draw,
 * provided the rest is exactly @draws DRAW-relocated draw packets. */
static struct state_cache_entry *
state_cache_capture(const struct parhelia_batch *batch, unsigned draws)
{
   struct state_cache_entry *e;
   unsigned first, relocs = 0;

   if (!draws || batch->packet_count < draws)
      return NULL;
   first = batch->packet_count - draws;
   for (unsigned i = first; i < batch->packet_count; i++)
      if (batch->packets[i].length != 3 ||
          (batch->commands[batch->packets[i].offset] >> 24) != 0x97 ||
          batch->packets[i].offset != batch->packets[first].offset + 3 * (i - first))
         return NULL;
   if (batch->packets[first].offset + 3 * draws != batch->command_count)
      return NULL;
   while (relocs < batch->reloc_count && batch->relocs[relocs].packet < first)
      relocs++;
   if (batch->reloc_count - relocs != draws)
      return NULL;
   for (unsigned i = relocs; i < batch->reloc_count; i++)
      if (batch->relocs[i].type != PARHELIA_RELOC_DRAW ||
          batch->relocs[i].packet != first + (i - relocs))
         return NULL;
   e = calloc(1, sizeof(*e));
   if (!e)
      return NULL;
   e->command_count = batch->packets[first].offset;
   e->packet_count = first;
   e->reloc_count = relocs;
   e->commands = malloc((e->command_count + 1) * sizeof(uint32_t));
   e->packets = malloc((first + 1) * sizeof(*e->packets));
   e->relocs = malloc((relocs + 1) * sizeof(*e->relocs));
   if (!e->commands || !e->packets || !e->relocs) {
      state_cache_entry_free(e);
      return NULL;
   }
   memcpy(e->commands, batch->commands, e->command_count * sizeof(uint32_t));
   memcpy(e->packets, batch->packets, first * sizeof(*e->packets));
   memcpy(e->relocs, batch->relocs, relocs * sizeof(*e->relocs));
   e->mvp_offset = -1;
   return e;
}

/* The one constant load at slot 0 holding @mvp verbatim, or -1. */
static int
state_cache_find_mvp(const struct state_cache_entry *e, const float *mvp)
{
   int found = -1;

   for (unsigned i = 0; i < e->packet_count; i++) {
      const uint32_t *cmd = e->commands + e->packets[i].offset;
      unsigned pos = 0;

      if ((cmd[0] >> 24) != 0x41)
         continue;
      while (pos + 5 <= e->packets[i].length) {
         const unsigned words = (cmd[pos] & 0xffff) + 2;

         if (words > e->packets[i].length - pos)
            return -1;
         if (cmd[pos + 2] == 0 && cmd[pos + 1] >= 4 &&
             !memcmp(&cmd[pos + 5], mvp, 16 * sizeof(float))) {
            if (found >= 0)
               return -1;
            found = (int)(e->packets[i].offset + pos + 5);
         }
         pos += words;
      }
   }
   return found;
}

/* The cached prefix plus the draw packets batch_finish() would emit. */
static int
state_cache_replay(struct parhelia_batch *batch,
                   const struct state_cache_entry *e,
                   const struct p3d_state *state, unsigned draws,
                   unsigned per_packet, unsigned vertices)
{
   const uint32_t header = 0x97000000u |
                           (((state->dw_per_vertex - 1) & 0x3f) << 17) | 1u;
   unsigned consumed = 0;

   if (e->command_count + 3 * draws > PH_BATCH_DWORDS ||
       e->packet_count + draws > PH_BATCH_PACKETS ||
       e->reloc_count + draws > PH_BATCH_RELOCS)
      return -E2BIG;
   memcpy(batch->commands, e->commands, e->command_count * sizeof(uint32_t));
   if (e->mvp_offset >= 0)
      memcpy(batch->commands + e->mvp_offset, state->mvp, 16 * sizeof(float));
   memcpy(batch->packets, e->packets, e->packet_count * sizeof(*e->packets));
   memcpy(batch->relocs, e->relocs, e->reloc_count * sizeof(*e->relocs));
   batch->command_count = e->command_count;
   batch->packet_count = e->packet_count;
   batch->reloc_count = e->reloc_count;
   for (unsigned k = 0; k < draws; k++) {
      const unsigned run = packet_run(state->prim, per_packet,
                                      vertices - consumed);
      uint32_t *cmd = batch->commands + batch->command_count;

      cmd[0] = header;
      cmd[1] = 0;
      cmd[2] = 0;
      batch->packets[batch->packet_count] = (struct drm_parhelia_packet) {
         .offset = batch->command_count, .length = 3,
      };
      batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
         .packet = batch->packet_count,
         .dword = consumed * state->dw_per_vertex * 4,
         .delta = run * state->dw_per_vertex * 4,
         .type = PARHELIA_RELOC_DRAW,
      };
      batch->packet_count++;
      batch->command_count += 3;
      consumed += run;
   }
   return 0;
}

int
parhelia_batch_state_run_units_cached(struct parhelia_state_cache *cache,
                                      struct parhelia_batch *batch,
                                      uint32_t colour_handle,
                                      uint32_t depth_handle,
                                      const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS],
                                      const struct p3d_state *state,
                                      unsigned vertices)
{
   struct state_cache_entry **slot, *e;
   unsigned per_packet, draws;
   uint64_t hash;
   int ret;

   if (cache)
      cache->last_calls = -1;
   if (!cache || !state || state->bound_ranges || state->indexed_vertices)
      return parhelia_batch_state_run_units(batch, colour_handle, depth_handle,
                                            texture_handles, state, vertices);
   per_packet = state->vertices_per_packet ? state->vertices_per_packet :
                p3d_vertices_per_primitive(state->prim);
   if (!per_packet || !vertices || !state->dw_per_vertex)
      return -EINVAL;
   draws = packet_count(state->prim, per_packet, vertices);
   if ((cache->count + 1) * 2 > cache->cap && !state_cache_grow(cache))
      return parhelia_batch_state_run_units(batch, colour_handle, depth_handle,
                                            texture_handles, state, vertices);
   if (!state_key_build(cache, state, colour_handle, depth_handle,
                        texture_handles))
      return parhelia_batch_state_run_units(batch, colour_handle, depth_handle,
                                            texture_handles, state, vertices);
   hash = state_key_hash(cache->key, cache->key_len);
   slot = state_cache_find(cache, hash);
   if (cache->verify && !((cache->hits + cache->misses + 1) % 20000))
      fprintf(stderr, "parhelia: state cache %lu hits, %lu misses, %u entries\n",
              cache->hits, cache->misses, cache->count);
   if (*slot) {
      cache->hits++;
      ret = state_cache_replay(batch, *slot, state, draws, per_packet,
                               vertices);
      cache->last_calls = ret ? -1 : (*slot)->calls;
      if (ret || !cache->verify)
         return ret;
      /* PARHELIA_STATE_CACHE_VERIFY=1: rebuild and compare every hit */
      {
         struct parhelia_batch *check = malloc(sizeof(*check));

         if (check &&
             !parhelia_batch_state_run_units(check, colour_handle,
                                             depth_handle, texture_handles,
                                             state, vertices) &&
             (check->command_count != batch->command_count ||
              check->packet_count != batch->packet_count ||
              check->reloc_count != batch->reloc_count ||
              memcmp(check->commands, batch->commands,
                     batch->command_count * sizeof(uint32_t)) ||
              memcmp(check->packets, batch->packets,
                     batch->packet_count * sizeof(batch->packets[0])) ||
              memcmp(check->relocs, batch->relocs,
                     batch->reloc_count * sizeof(batch->relocs[0]))))
            fprintf(stderr, "parhelia: state cache mismatch after %lu hits\n",
                    cache->hits);
         free(check);
      }
      return 0;
   }
   cache->misses++;
   ret = parhelia_batch_state_run_units(batch, colour_handle, depth_handle,
                                        texture_handles, state, vertices);
   if (ret)
      return ret;
   if (cache->count >= STATE_CACHE_MAX_ENTRIES) {
      state_cache_flush(cache);
      slot = state_cache_find(cache, hash);
   }
   e = state_cache_capture(batch, draws);
   if (!e)
      return 0;
   if (state_mvp_patchable(state)) {
      e->mvp_offset = state_cache_find_mvp(e, state->mvp);
      if (e->mvp_offset < 0) {
         state_cache_entry_free(e);
         return 0;
      }
   }
   /* a longer draw run may take one more CALL than the captured one */
   {
      unsigned calls;

      e->calls = parhelia_batch_packed_call_count(batch, 0,
                                                  batch->packet_count, true,
                                                  &calls) ? -1 :
                 (int)calls + 1;
   }
   e->hash = hash;
   e->key = malloc(cache->key_len);
   if (!e->key) {
      state_cache_entry_free(e);
      return 0;
   }
   memcpy(e->key, cache->key, cache->key_len);
   e->key_len = cache->key_len;
   *slot = e;
   cache->count++;
   cache->last_calls = e->calls;
   return 0;
}

int
parhelia_state_cache_last_calls(const struct parhelia_state_cache *cache)
{
   return cache ? cache->last_calls : -1;
}

int
parhelia_batch_state_run(struct parhelia_batch *batch, uint32_t colour_handle,
                         uint32_t depth_handle, uint32_t texture0_handle,
                         uint32_t texture1_handle,
                         const struct p3d_state *state, unsigned vertices)
{
   return parhelia_batch_state_run_units(batch, colour_handle, depth_handle,
      (const uint32_t[P3D_MAX_TEXTURE_UNITS]){
         texture0_handle, texture1_handle },
      state, vertices);
}

int
parhelia_batch_state_textured(struct parhelia_batch *batch,
                              uint32_t colour_handle, uint32_t depth_handle,
                              uint32_t texture_handle,
                              const struct p3d_state *state,
                              unsigned triangle_count)
{
   return batch_state(batch, colour_handle, depth_handle,
                      (const uint32_t[P3D_MAX_TEXTURE_UNITS]){ texture_handle },
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
   return batch_state(batch, colour_handle, depth_handle,
                      (const uint32_t[P3D_MAX_TEXTURE_UNITS]){
                         texture0_handle, texture1_handle },
                      state, triangle_count, 0);
}

int
parhelia_batch_state_units(struct parhelia_batch *batch,
                           uint32_t colour_handle, uint32_t depth_handle,
                           const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS],
                           const struct p3d_state *state,
                           unsigned triangle_count)
{
   return batch_state(batch, colour_handle, depth_handle, texture_handles,
                      state, triangle_count, 0);
}

static int
batch_finish(struct parhelia_batch *batch, const struct p3d_cs *cs,
             const struct p3d_reloc *calls, uint32_t colour_handle,
             uint32_t depth_handle,
             const uint32_t *texture_handles,
             const uint32_t *texture_offsets,
             uint32_t source_handle,
             unsigned triangle_count,
             unsigned vertex_dwords,
             enum p3d_prim prim,
             unsigned vertices_per_packet,
             unsigned total_vertices,
             bool bound_ranges,
             unsigned indexed_vertices)
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
         if (indexed_vertices) {
            /* indices follow the batch's vertices, in packet order */
            const unsigned run = packet_run(prim, vertices_per_packet,
                                            total_vertices - consumed);

            if (call->sec_len != 3 || cmd[0] != 0x9f000001u || cmd[1] ||
                cmd[2] || consumed >= total_vertices)
               return -EINVAL;
            batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
               .packet = i,
               .dword = (indexed_vertices * vertex_dwords + consumed) * 4,
               .delta = run * 4,
               .type = PARHELIA_RELOC_DRAW,
            };
            consumed += run;
            draws++;
            continue;
         }
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
      if ((bound_ranges || indexed_vertices) && call->sec_len == 16 &&
          cmd[0] == 0xe &&
          cmd[1] == 0x03020100u && cmd[6] == 0x07060504u &&
          cmd[11] == 0x13121110u && cmd[12] == vertex_dwords * 4 &&
          cmd[2] == (indexed_vertices ? 3u : 0u) &&
          !cmd[13] && !cmd[14] && !cmd[15]) {
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
               const unsigned unit = selected_unit;

               if (index == 0x77)
                  texture_units |= 1u << unit;
               batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
                  .packet = i, .dword = pos,
                  .delta = index == 0x77 && texture_offsets ?
                           texture_offsets[unit] : 0,
                  .handle = index == 0x77 ?
                            (texture_handles ? texture_handles[unit] : 0) :
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
   for (unsigned u = 0; u < P3D_MAX_TEXTURE_UNITS; u++)
      if (!!(texture_units & (1u << u)) !=
          !!(texture_handles && texture_handles[u]))
         return -EINVAL;
   if (indexed_vertices) {
      if (stream_packet == UINT32_MAX ||
          batch->reloc_count >= PH_BATCH_RELOCS)
         return -EINVAL;
      batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc) {
         .packet = stream_packet, .dword = 2, .type = PH_RELOC_STREAM_BASE,
      };
   } else if (bound_ranges) {
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
           reloc->type != PARHELIA_RELOC_DRAW &&
           reloc->type != PH_RELOC_STREAM_BASE))
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
      if (src->relocs[i].type == PH_RELOC_STREAM_BASE)
         dst->relocs[dst->reloc_count + i].delta += vertex_base * 4;
   }
   /* the caller may have staged the vertices in place already */
   if (src_vertex_dwords && src_vertices != dst_vertices + vertex_base)
      memmove(dst_vertices + vertex_base, src_vertices,
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
   bool packet_has_reloc[PH_BATCH_PACKETS] = { false };
   bool in_place = dst == src;
   bool previous_compactable = false;
   int ret = 0;

   if (!dst || !src || src->command_count > PH_BATCH_DWORDS ||
       src->packet_count > PH_BATCH_PACKETS ||
       src->reloc_count > PH_BATCH_RELOCS)
      return -EINVAL;
   for (unsigned r = 0; r < src->reloc_count; r++) {
      if (src->relocs[r].packet >= src->packet_count)
         return -EINVAL;
      packet_has_reloc[src->relocs[r].packet] = true;
   }
   for (unsigned i = 0, end = 0; i < src->packet_count; i++) {
      const struct drm_parhelia_packet *packet = &src->packets[i];

      if (packet->offset > src->command_count ||
          packet->length > src->command_count - packet->offset)
         return -EINVAL;
      /* Compaction never grows the stream: packets in stream order can be
       * rewritten in place without overtaking unread input. */
      in_place &= packet->offset >= end;
      end = packet->offset + packet->length;
   }
   const unsigned packet_count = src->packet_count;

   out = in_place ? dst : malloc(sizeof(*out));
   if (!out)
      return -ENOMEM;
   out->command_count = 0;
   out->packet_count = 0;

   /* A range-stream relocation owns the first packet of an exact direct-fetch
    * tail.  That packet and every packet after it are already barriers (stream,
    * primitive, bounds, descriptor FIFO, then 0x8f ranges), so they remain
    * byte-exact naturally.  Ordinary state packets before the tail may still
    * be joined; the relocation's packet owner is remapped below. */

   for (unsigned i = 0; i < packet_count; i++) {
      const struct drm_parhelia_packet packet_copy = src->packets[i];
      const struct drm_parhelia_packet *packet = &packet_copy;
      const uint32_t *cmd = src->commands + packet->offset;
      bool compactable, merge;

      compactable = !packet_has_reloc[i] &&
                    compact_reglist_packet(cmd, packet->length);
      merge = compactable && previous_compactable && out->packet_count &&
              out->packets[out->packet_count - 1].length <=
                 PH_COMPACT_REGLIST_DWORDS &&
              packet->length - 1 <= PH_COMPACT_REGLIST_DWORDS -
                                     out->packets[out->packet_count - 1].length;

      if (merge) {
         struct drm_parhelia_packet *previous =
            &out->packets[out->packet_count - 1];
         uint32_t *previous_cmd = out->commands + previous->offset;

         memmove(out->commands + out->command_count, cmd + 1,
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
         memmove(out->commands + out->command_count, cmd,
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
   if (in_place)
      return 0;
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
   if (!in_place)
      free(out);
   return ret;
}

/* Registers whose repeated value can go: not a compaction barrier, not the
 * banked window at 0x11c0, not the 2-D drawing file at 0x1300 (its 0x1380
 * aliases fire, e.g. the fill strips refired by DSTXY and LEN). */
static const bool *
repeat_droppable(void)
{
   static bool table[256];
   static bool built;

   if (!built) {
      for (unsigned i = 0; i < 0xc0; i++)
         table[i] = i != 0x20 && i != 0x21 && !(i >= 0x70 && i <= 0x7f) &&
                    !compact_reg_barrier(i);
      built = true;
   }
   return table;
}

/* Forget what a verbatim register list writes; all of it when the pad
 * selector makes its framing unknown. */
static void
forget_reglist(const uint32_t *cmd, unsigned length, bool *known)
{
   for (unsigned pos = 1; pos < length;) {
      uint32_t tag = cmd[pos++];

      for (unsigned s = 0; s < 4 && pos < length; s++, tag >>= 8) {
         if ((tag & 0xff) == 0x21)
            break;
         if ((tag & 0xff) == 0x20) {
            memset(known, 0, 256 * sizeof(*known));
            return;
         }
         known[tag & 0xff] = false;
         pos++;
      }
   }
}

int
parhelia_batch_drop_repeats(struct parhelia_batch *dst,
                            const struct parhelia_batch *src,
                            unsigned *dropped)
{
   const bool *droppable = repeat_droppable();
   uint32_t shadow[256];
   bool known[256] = { false };
   bool referenced[PH_BATCH_PACKETS] = { false };
   unsigned packet_map[PH_BATCH_PACKETS];
   unsigned writes_dropped = 0;

   if (!dst || !src || dst == src || src->command_count > PH_BATCH_DWORDS ||
       src->packet_count > PH_BATCH_PACKETS ||
       src->reloc_count > PH_BATCH_RELOCS)
      return -EINVAL;
   for (unsigned r = 0; r < src->reloc_count; r++) {
      if (src->relocs[r].packet >= src->packet_count)
         return -EINVAL;
      referenced[src->relocs[r].packet] = true;
   }
   dst->command_count = dst->packet_count = 0;
   for (unsigned i = 0; i < src->packet_count; i++) {
      const struct drm_parhelia_packet *packet = &src->packets[i];
      const unsigned length = packet->length;
      const uint32_t *cmd = src->commands + packet->offset;
      uint32_t *out = dst->commands + dst->command_count;
      unsigned n = 1, packet_dropped = 0;
      bool plain = false;

      if (packet->offset > src->command_count ||
          length > src->command_count - packet->offset)
         return -EINVAL;
      packet_map[i] = dst->packet_count;
      if (length >= 2 && !referenced[i] && !(cmd[0] & 0xffff0000u) &&
          (cmd[0] & 0xffffu) + 2u == length) {
         uint32_t tag_out = 0x21212121u;
         unsigned pos = 1, tag_pos = 0, slot = 4;

         plain = true;
         while (plain && pos < length) {
            uint32_t tag = cmd[pos++];

            for (unsigned s = 0; s < 4; s++, tag >>= 8) {
               const unsigned index = tag & 0xff;
               uint32_t value;

               if (index == 0x21)
                  break;
               if (index == 0x20 || pos == length) {
                  plain = false;
                  break;
               }
               value = cmd[pos++];
               if (droppable[index] && known[index] &&
                   shadow[index] == value) {
                  packet_dropped++;
                  continue;
               }
               shadow[index] = value;
               known[index] = true;
               if (slot == 4) {
                  if (tag_pos)
                     out[tag_pos] = tag_out;
                  tag_out = 0x21212121u;
                  tag_pos = n++;
                  slot = 0;
               }
               tag_out = (tag_out & ~(0xffu << (slot * 8))) |
                         index << (slot * 8);
               slot++;
               out[n++] = value;
            }
         }
         /* the pad selector's framing is not ours to rewrite */
         if (plain) {
            if (tag_pos)
               out[tag_pos] = tag_out;
            writes_dropped += packet_dropped;
            if (n == 1)
               continue;
            out[0] = (cmd[0] & 0xffff0000u) | (n - 2);
            dst->packets[dst->packet_count++] = (struct drm_parhelia_packet) {
               .offset = dst->command_count, .length = n };
            dst->command_count += n;
            continue;
         }
      }
      /* verbatim; what it writes is no longer known */
      if (referenced[i] && length && !(cmd[0] >> 24))
         forget_reglist(cmd, length, known);
      else if (!length || !(cmd[0] >> 24))
         memset(known, 0, sizeof(known));
      else if (length && (cmd[0] >> 24) != 0x97 && (cmd[0] >> 24) != 0x8f &&
               (cmd[0] >> 24) != 0x9f &&
               /* block-file loads: no MMIO register */
               (cmd[0] >> 24) != 0x41)
         memset(known, 0, sizeof(known));
      memcpy(out, cmd, length * sizeof(cmd[0]));
      dst->packets[dst->packet_count++] = (struct drm_parhelia_packet) {
         .offset = dst->command_count, .length = length };
      dst->command_count += length;
   }
   dst->reloc_count = src->reloc_count;
   for (unsigned r = 0; r < src->reloc_count; r++) {
      dst->relocs[r] = src->relocs[r];
      dst->relocs[r].packet = packet_map[src->relocs[r].packet];
   }
   if (dropped)
      *dropped = writes_dropped;
   return 0;
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
      if (src->type == PH_RELOC_STREAM_BASE)
         return -EINVAL;
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

/* the kernel's DMA block bound on one packet */
#define PH_RUN_MAX_DWORDS 4095u

int
parhelia_batch_submit_ranges_async(
   int fd, struct parhelia_batch *batch, uint32_t vertex_handle,
   uint32_t vertex_offset, unsigned vertex_dwords, bool call_boundaries,
   bool draw_runs, uint32_t *fence)
{
   struct drm_parhelia_reloc relocs[PH_BATCH_RELOCS + 1];
   struct drm_parhelia_packet runs[PH_BATCH_PACKETS];
   uint16_t run_map[PH_BATCH_PACKETS];
   bool draw[PH_BATCH_PACKETS] = { false }, other[PH_BATCH_PACKETS] = { false };
   const struct drm_parhelia_packet *packets = batch ? batch->packets : NULL;
   struct drm_parhelia_submit submit;
   uint64_t bytes = (uint64_t)vertex_dwords * sizeof(uint32_t);
   unsigned reloc_count = 0, packet_count = batch ? batch->packet_count : 0;

   if (!batch || !fence || batch->reloc_count > PH_BATCH_RELOCS ||
       !vertex_handle || !vertex_dwords || (vertex_offset & 3) ||
       bytes > UINT32_MAX || vertex_offset > UINT32_MAX - bytes)
      return -EINVAL;
   for (unsigned i = 0; i < batch->reloc_count; i++) {
      const struct drm_parhelia_reloc *src = &batch->relocs[i];
      uint32_t *cmd;

      if (src->type == PARHELIA_RELOC_RANGE_STREAM_VRAM)
         return -ENOTSUP;
      if (src->type == PARHELIA_RELOC_CALL_BOUNDARY && !call_boundaries)
         continue;
      if (src->type == PH_RELOC_STREAM_BASE) {
         if ((src->delta & 3) || src->delta >= bytes ||
             src->packet >= batch->packet_count ||
             src->dword >= batch->packets[src->packet].length)
            return -EINVAL;
         batch->commands[batch->packets[src->packet].offset + src->dword] =
            src->delta | 3;
         continue;
      }
      if (src->type != PARHELIA_RELOC_DRAW) {
         if (src->packet < batch->packet_count)
            other[src->packet] = true;
         relocs[reloc_count++] = *src;
         continue;
      }
      if (src->handle || (src->dword & 3) || !src->delta ||
          (src->delta & 3) ||
          src->delta > PH_BATCH_MAX_DRAW_DWORDS * sizeof(uint32_t) ||
          (uint64_t)src->dword + src->delta > bytes ||
          src->packet >= batch->packet_count ||
          batch->packets[src->packet].length != 3)
         return -EINVAL;
      cmd = batch->commands + batch->packets[src->packet].offset;
      cmd[1] = src->dword;
      cmd[2] = src->dword + src->delta;
      draw[src->packet] = true;
   }
   if (draw_runs && batch->packet_count <= PH_BATCH_PACKETS) {
      unsigned head = 0;

      packet_count = 0;
      for (unsigned i = 0; i < batch->packet_count; i++) {
         const struct drm_parhelia_packet *p = &batch->packets[i];
         struct drm_parhelia_packet *run = packet_count ?
            &runs[packet_count - 1] : NULL;

         /* a draw no other relocation names joins the run before it */
         if (run && draw[head] && draw[i] && !other[i] &&
             run->offset + run->length == p->offset &&
             run->length + 3 <= PH_RUN_MAX_DWORDS &&
             batch->commands[run->offset] == batch->commands[p->offset]) {
            run->length += 3;
         } else {
            runs[packet_count++] = *p;
            head = i;
         }
         run_map[i] = packet_count - 1;
      }
      for (unsigned r = 0; r < reloc_count; r++)
         relocs[r].packet = run_map[relocs[r].packet];
      packets = runs;
   }
   relocs[reloc_count++] = (struct drm_parhelia_reloc) {
      .dword = (uint32_t)bytes,
      .handle = vertex_handle,
      .delta = vertex_offset,
      .type = PARHELIA_RELOC_DRAW_RANGES,
   };
   submit = (struct drm_parhelia_submit) {
      .commands = (uintptr_t)batch->commands,
      .packets = (uintptr_t)packets,
      .relocs = (uintptr_t)relocs,
      .command_dwords = batch->command_count,
      .packet_count = packet_count,
      .reloc_count = reloc_count,
      .flags = PARHELIA_SUBMIT_FENCE | PARHELIA_SUBMIT_ASYNC |
               (call_boundaries ? PARHELIA_SUBMIT_CALL_BOUNDARIES : 0),
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
       (index_offset & 3) || (index_count != 3 && index_count != 6) ||
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
