/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
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
   /* Bytes per texel of the sampler storage this chain describes. */
   unsigned cpp;
   /* Bytes per 4x4 block when the storage is S3TC; zero otherwise. */
   unsigned block_bytes;
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

/* S3TC storage is macro-tiled: one macrotile is 8x4 S3TC blocks, that is
 * 32x16 logical texels and 32 blocks, and macrotiles are row-major over a
 * pitch given in logical texels.  Recovered from the Linux uploader mtx2799
 * at .text+0x20720; see docs/parhelia-mesa-s3tc-implementation.md. */
static inline size_t
ph_dxt_block_offset(unsigned pitch, unsigned bx, unsigned by,
                    unsigned block_bytes)
{
   const unsigned macro = (by >> 2) * (pitch >> 5) + (bx >> 3);

   return (size_t)macro * (32u * block_bytes) +
          (size_t)(by & 3u) * (8u * block_bytes) +
          (size_t)(bx & 7u) * block_bytes;
}

/* A compressed level reserves 32 texel columns and, per mtx625
 * .text+0x5b8c0, 32 rows up to a height of 32 and 64 rows above it. */
static inline size_t
ph_dxt_level_size(unsigned width, unsigned height, unsigned block_bytes)
{
   const unsigned rows = height <= 32 ? 32u : 64u;
   const unsigned align = block_bytes == 8 ? 0x200u : 0x400u;

   return ph_mip_align((ph_mip_align(width, 32) / 4) *
                       (ph_mip_align(height, rows) / 4) * block_bytes, align);
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
 * layout.  `p690` selects the chain geometry measured on the P690 texture
 * unit (parhelia-drm-test --mip-address-probe): levels with a pitch of 64 or
 * more reserve 16-row multiples, the tail starts at the first level no
 * larger than 32x16, and a 32-wide tail level occupies the left half of the
 * 64x16 tail block with the smaller levels' table shifted right by 32. */
static inline bool
ph_mip_layout_init_cpp(struct ph_mip_layout *out, unsigned width,
                       unsigned height, unsigned requested_levels, bool tiled,
                       bool p690, unsigned cpp)
{
   size_t next = 0, tail_base = 0;
   unsigned tail = 0, tail_shift = 0;
   bool tail_started = false;

   if (!out || !width || !height || !requested_levels ||
       requested_levels > PH_MAX_MIP_LEVELS ||
       requested_levels > ph_mip_max_levels(width, height) ||
       (cpp != 1 && cpp != 2 && cpp != 4))
      return false;
   *out = (struct ph_mip_layout){ .levels = requested_levels, .cpp = cpp,
                                  .tiled = tiled };

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

      if ((width <= 16 || (p690 && tiled && width <= 32)) && height <= 16) {
         unsigned x = 0, y = 0;

         if (!tail_started) {
            tail_base = ph_mip_align((unsigned)next, level_align);
            bytes = (size_t)32 * (tiled ? 32 : 16) * cpp;
            next = tail_base + bytes;
            tail_started = true;
         }
         if (width > 16) {
            tail_shift = 32;
         } else {
            if (tail >= 5)
               return false;
            ph_mip_tail_position(tail++, &x, &y);
         }
         level->offset = tail_base;
         level->tail_x = width > 16 ? 0 : x + tail_shift;
         level->tail_y = y;
      } else {
         rows = !tiled ? height :
                p690 && pitch >= 64 ? ph_mip_align(height, 16) :
                ph_mip_align(height, 32);
         bytes = (size_t)pitch * rows * cpp;
         level->offset = ph_mip_align((unsigned)next, level_align);
         next = level->offset + bytes;
      }

      width = width > 1 ? width >> 1 : 1;
      height = height > 1 ? height >> 1 : 1;
   }
   out->size = ph_mip_align((unsigned)next, tiled ? 0x2000 : 0x200);
   return out->size != 0;
}

static inline bool
ph_mip_layout_init_chip(struct ph_mip_layout *out, unsigned width,
                        unsigned height, unsigned requested_levels, bool tiled,
                        bool p690)
{
   return ph_mip_layout_init_cpp(out, width, height, requested_levels, tiled,
                                 p690, 4);
}

static inline bool
ph_mip_layout_init(struct ph_mip_layout *out, unsigned width, unsigned height,
                   unsigned requested_levels, bool tiled)
{
   return ph_mip_layout_init_chip(out, width, height, requested_levels, tiled,
                                  false);
}

static inline unsigned
ph_mip_ready_levels(uint16_t defined, unsigned levels)
{
   unsigned ready = 0;

   while (ready < levels && (defined & (1u << ready)))
      ready++;
   return ready;
}

/* Translate a logical mip texel to the coordinates consumed by the P690
 * texture address unit.  Exhaustive coordinate-gradient sampling proves two
 * extra address permutations hidden by flat-color tests: native levels swap
 * the low two Y bits, while a compact 32x32 child interleaves X/Y bits before
 * the classic texture swizzle.  For the 2x2 tail slot this naturally places
 * its rows at physical Y=8 and Y=10. */
static inline bool
ph_mip_physical_coords(const struct ph_mip_layout *mip, unsigned level,
                       unsigned cpp, unsigned *x, unsigned *y,
                       unsigned *physical_pitch)
{
   const struct ph_mip_level *image = &mip->level[level];
   const bool p690 = mip->native_64x16_32 && cpp == 4;
   const bool tail = p690 && image->width <= 16 && image->height <= 16;

   if (p690 && image->pitch == 32 && !tail) {
      const unsigned logical_x = *x, logical_y = *y;

      *x = ((logical_x >> 2) & 1u) |
           (((logical_y >> 0) & 1u) << 1) |
           (((logical_x >> 3) & 1u) << 2) |
           (((logical_x >> 4) & 1u) << 3) |
           (((logical_y >> 1) & 1u) << 4);
      *y = (logical_y & ~31u) |
           ((logical_x >> 0) & 1u) |
           (((logical_x >> 1) & 1u) << 1) |
           (((logical_y >> 4) & 1u) << 2) |
           (((logical_y >> 2) & 1u) << 3) |
           (((logical_y >> 3) & 1u) << 4);
      *physical_pitch = image->pitch;
      return false;
   }

   *x += image->tail_x;
   if (p690)
      *y = (*y & ~3u) | ((*y & 1u) << 1) | ((*y & 2u) >> 1);
   *y += image->tail_y;
   *physical_pitch = tail ? 64 : image->pitch;
   return p690 && (image->pitch >= 64 || tail);
}

#endif
