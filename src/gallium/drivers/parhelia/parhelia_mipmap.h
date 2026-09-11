/* SPDX-License-Identifier: MIT */
/*
 * Layout of the hardware's contiguous 32-bit 2D mip chain.  This is kept
 * separate from the render-target layout: texture layout 2 has 32x16
 * addressing and a 32-row allocation quantum, while layout 0 is linear.
 */
#ifndef PARHELIA_MIPMAP_H
#define PARHELIA_MIPMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PH_MAX_MIP_LEVELS 12

/* P690's second texture-address unit corrupts images below 64 pixels in
 * either dimension.  Repeat padding must end on a whole logical period so
 * scaled, unbounded GL_REPEAT coordinates remain exact across the physical
 * texture boundary.  Clamp padding repeats the edge texel over the same
 * extent. */
static inline unsigned
ph_alpha_companion_extent(unsigned logical)
{
   if (!logical || logical >= 64)
      return logical;
   return ((64 + logical - 1) / logical) * logical;
}

static inline unsigned
ph_alpha_companion_texel(unsigned physical, unsigned logical, bool repeat)
{
   if (!logical)
      return 0;
   return repeat ? physical % logical :
                   (physical < logical ? physical : logical - 1);
}

static inline float
ph_alpha_companion_scale(unsigned logical, unsigned physical)
{
   return physical ? (float)logical / (float)physical : 0.0f;
}

struct ph_mip_level {
   size_t offset;
   unsigned width, height, pitch;
   unsigned tail_x, tail_y;
};

struct ph_mip_layout {
   struct ph_mip_level level[PH_MAX_MIP_LEVELS];
   size_t size;
   unsigned levels;
   bool tiled;
   bool native_64x16_32;
};

static inline unsigned
ph_mip_align(unsigned value, unsigned alignment)
{
   return (value + alignment - 1) & ~(alignment - 1);
}

static inline unsigned
ph_mip_next_pot(unsigned value)
{
   unsigned result = 1;

   while (result < value && result < (1u << 31))
      result <<= 1;
   return result;
}

static inline unsigned
ph_mip_max_levels(unsigned width, unsigned height)
{
   unsigned levels = 1;

   while ((width > 1 || height > 1) && levels < PH_MAX_MIP_LEVELS) {
      width = width > 1 ? width >> 1 : 1;
      height = height > 1 ? height >> 1 : 1;
      levels++;
   }
   return levels;
}

/* The tail table is the kernel HAL's mtx3067/mtx516/mtx1834 data: all
 * levels from 16x16 through 1x1 share one 32x16 block. */
static inline void
ph_mip_tail_position(unsigned tail, unsigned *x, unsigned *y)
{
   static const unsigned tail_x[] = { 0, 16, 24, 16, 24 };
   static const unsigned tail_y[] = { 0, 0, 0, 8, 8 };

   *x = tail < 5 ? tail_x[tail] : 0;
   *y = tail < 5 ? tail_y[tail] : 0;
}

/* Returns false for chains the fixed table cannot represent.  `tiled` is
 * hardware texture layout 2; it is intentionally not the display/RT tile
 * layout. */
static inline bool
ph_mip_layout_init(struct ph_mip_layout *out, unsigned width, unsigned height,
                   unsigned requested_levels, bool tiled)
{
   size_t next = 0, tail_base = 0;
   unsigned tail = 0;
   bool tail_started = false;

   if (!out || !width || !height || !requested_levels ||
       requested_levels > PH_MAX_MIP_LEVELS ||
       requested_levels > ph_mip_max_levels(width, height))
      return false;
   *out = (struct ph_mip_layout){ .levels = requested_levels, .tiled = tiled };

   for (unsigned i = 0; i < requested_levels; i++) {
      struct ph_mip_level *level = &out->level[i];
      unsigned pitch, rows, level_align;
      size_t bytes;

      level->width = width;
      level->height = height;
      pitch = tiled ? ph_mip_next_pot(width) : ph_mip_align(width, 32);
      pitch = pitch < 32 ? 32 : pitch;
      level->pitch = pitch;
      level_align = tiled ? 0x1000 : 0x40;

      if (width <= 16 && height <= 16) {
         unsigned x, y;

         if (tail >= 5)
            return false;
         ph_mip_tail_position(tail++, &x, &y);
         if (!tail_started) {
            tail_base = ph_mip_align((unsigned)next, level_align);
            bytes = (size_t)32 * (tiled ? 32 : 16) * 4;
            next = tail_base + bytes;
            tail_started = true;
         }
         level->offset = tail_base;
         level->tail_x = x;
         level->tail_y = y;
      } else {
         rows = tiled ? ph_mip_align(height, 32) : height;
         bytes = (size_t)pitch * rows * 4;
         level->offset = ph_mip_align((unsigned)next, level_align);
         next = level->offset + bytes;
      }

      width = width > 1 ? width >> 1 : 1;
      height = height > 1 ? height >> 1 : 1;
   }
   out->size = ph_mip_align((unsigned)next, tiled ? 0x2000 : 0x200);
   return out->size != 0;
}

static inline unsigned
ph_mip_ready_levels(uint16_t defined, unsigned levels)
{
   unsigned ready = 0;

   while (ready < levels && (defined & (1u << ready)))
      ready++;
   return ready;
}

#endif
