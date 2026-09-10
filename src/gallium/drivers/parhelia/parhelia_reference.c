/* SPDX-License-Identifier: GPL-2.0-only */
/* Memory/submission adapter for the complete, original C gears example. */
#include <errno.h>
#include <string.h>
#include "parhelia_batch.h"

int
parhelia_batch_reference(struct parhelia_batch *batch, const struct p3d_cs *src,
                        const struct parhelia_reference_bindings *b,
                        unsigned vertex_dwords)
{
   unsigned end = 0, draws = 0, fences = 0, vertex_binds = 0, loader_binds = 0;
   if (!batch || !src || !b || src->error || !src->sec || !src->rel ||
       !src->sec_len || src->sec_len > PH_BATCH_DWORDS ||
       !src->rel_len || src->rel_len > PH_BATCH_PACKETS + 3 ||
       !b->colour_handle || !b->depth_handle || !b->vertex_entry || !b->loader_entry ||
       !b->colour_base || !b->depth_base || b->colour_base == b->depth_base ||
       !vertex_dwords || vertex_dwords > 4096 || vertex_dwords % 18)
      return -EINVAL;
   memset(batch, 0, sizeof(*batch));
   for (unsigned i = 0; i < src->rel_len; i++) {
      const struct p3d_reloc *call = &src->rel[i];
      unsigned n = call->sec_len;
      if (call->sec_off != end || n < 2 || n > src->sec_len - end)
         return -EINVAL;
      const uint32_t *in = src->sec + end;
      end += n;
      if (n == 5 && in[0] == 3 && in[1] == 0x21afafdd && !in[2] &&
          in[3] == (in[4] | 1) && !(in[4] & 7)) {
         if (fences || draws || call->is_draw)
            return -EINVAL;
         fences = 1;
         continue;
      }
      if (n == 3 && in[0] == 1 && in[1] == 0x212121ae && in[2] == 0x70601) {
         if (fences != 1 || !draws || call->is_draw)
            return -EINVAL;
         fences = 3;
         continue;
      }
      if (n == 6 && in[0] == 4 && in[1] == 0xc0afafc0 && in[2] == 0x307 &&
          in[3] == (in[4] | 1) && !(in[4] & 7)) {
         if (fences != 3 || i + 1 != src->rel_len || call->is_draw)
            return -EINVAL;
         fences = 7;
         continue;
      }
      if (fences & 2 || batch->packet_count == PH_BATCH_PACKETS ||
          batch->command_count + n > PH_BATCH_DWORDS)
         return -E2BIG;
      unsigned packet = batch->packet_count++;
      uint32_t *out = batch->commands + batch->command_count;
      batch->packets[packet] = (struct drm_parhelia_packet){batch->command_count, n};
      batch->command_count += n;
      memcpy(out, in, n * sizeof(*out));
      if (in[0] == 0x970a0001 && n == 3) {
         if (!call->is_draw || fences != 1 || in[1] || in[2] ||
             (draws + 1) * 18 > vertex_dwords || batch->reloc_count >= PH_BATCH_PACKETS + 4)
            return -EINVAL;
         batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc){
            .packet = packet, .dword = draws++ * 72, .delta = 72, .type = PARHELIA_RELOC_DRAW,
         };
      } else if ((in[0] >> 24) == 0x41) {
         unsigned pos = 0;
         if (call->is_draw || fences || !loader_binds)
            return -EINVAL;
         while (pos < n) {
            if (n - pos < 5)
               return -EINVAL;
            unsigned vectors = in[pos + 1], words = vectors * 4 + 5;
            if (!vectors || vectors > 63 || words > n - pos ||
                in[pos] != (0x41000000u | vectors << 17 | (words - 2)) ||
                in[pos + 2] > 384 - vectors || in[pos + 3] != 1 || in[pos + 4])
               return -EINVAL;
            pos += words;
         }
      } else if (!(in[0] >> 24) && in[0] == n - 2 && !call->is_draw) {
         unsigned pos = 1;
         while (pos < n) {
            uint32_t tag = in[pos++];
            for (unsigned slot = 0; slot < 4; slot++) {
               unsigned index = tag >> (slot * 8) & 255;
               if (index == 0x21)
                  break;
               if (pos >= n || (index > 0xdf && index != 0xe3) ||
                   index == 0xaf || index == 0x24 || index == 0x25 ||
                   index == 0x28 || index == 0x29)
                  return -EINVAL;
               if (index >= 0x88 && index <= 0x8e && !(index & 1)) {
                  uint32_t handle;
                  if (in[pos] == b->depth_base)
                     handle = b->depth_handle;
                  else if (in[pos] == b->colour_base || !in[pos])
                     handle = b->colour_handle;
                  else
                     return -EINVAL;
                  if (batch->reloc_count >= PH_BATCH_PACKETS + 4)
                     return -E2BIG;
                  batch->relocs[batch->reloc_count++] = (struct drm_parhelia_reloc){
                     .packet = packet, .dword = pos, .handle = handle, .type = PARHELIA_RELOC_VRAM,
                  };
                  out[pos] = 0;
               } else if (index == 0x40 || index == 0x41) {
                  if (in[pos] == 0xdeade000) {
                     out[pos] = b->vertex_entry;
                     vertex_binds++;
                  } else if (index == 0x40 && in[pos] == 0xdeade002) {
                     out[pos] = b->loader_entry;
                     loader_binds++;
                  } else {
                     return -EINVAL;
                  }
               }
               pos++;
            }
         }
      } else {
         return -EINVAL;
      }
   }
   return end == src->sec_len && fences == 7 && draws * 18 == vertex_dwords &&
          vertex_binds == 2 && loader_binds == 1 ? 0 : -EINVAL;
}
