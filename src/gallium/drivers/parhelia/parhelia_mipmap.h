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
   /* Storage one image of this level occupies.  A cube map's six faces are
    * this many bytes apart; a volume's slices are interleaved instead. */
   size_t bytes;
   /* A P690 cube map's tail level: the six faces interleave in the volume
    * order at the tail position. */
   bool face_tail;
};

struct ph_mip_layout {
   struct ph_mip_level level[PH_MAX_MIP_LEVELS];
   size_t size;
   unsigned levels;
   /* Slices of a volume image, or 6 for a cube map; 1 for a plain 2-D or
    * 1-D image.  The texture-address unit derives a slice's own address from
    * the descriptor's count in 0x11e4[27:16]. */
   unsigned slices;
   /* The six faces of a cube map are whole level images laid out
    * face-minor inside a level (vendor mtx3887, .text+0x27cb3), where a
    * volume interleaves its slices inside one storage tile. */
   bool cube;
   /* Bytes per texel of the sampler storage this chain describes. */
   unsigned cpp;
   /* Bytes per 4x4 block when the storage is S3TC; zero otherwise. */
   unsigned block_bytes;
   /* The measured P690 8/16-bit tile order backs this chain. */
   bool p690_narrow;
   bool tiled;
   bool native_64x16_32;
   /* The original Parhelia's 32-bit sampler order: 64x32-texel tiles. */
   bool native_64x32;
   /* PARHELIA_CUBE_PROBE: every level and face row-linear, back to back, so
    * a sampled texel names the address the unit read. */
   bool probe_linear;
   size_t tail_base;
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

/* Bytes a tiled level of @rows rows occupies, that is where the hardware puts
 * the next level: the descriptor carries one base and derives every level
 * itself.  A level ends where its next row band would start - whole tiles
 * plus, when that band begins mid-tile, only the 512-byte granule the x5
 * parity bit leaves free there, because the following level's own parity puts
 * it in the half-tile this level does not use.  Rounding up to a whole tile
 * instead displaces every level from 1 on whenever the pitch is not a
 * multiple of 64.  Measured with parhelia-drm-test --mip-address-probe over
 * pitches 64/96/128/160/224 and heights 16/32/40/48/64: all 25 agree.  For a
 * pitch that is a multiple of 64 this is exactly pitch * rows * cpp. */
static inline size_t
ph_tiled_level_bytes(unsigned pitch, unsigned rows, unsigned tile_h,
                     size_t tile_bytes)
{
   const unsigned trows = rows >= tile_h ? rows / tile_h : 1u;
   const size_t texels = (size_t)trows * pitch;

   return texels / 64 * tile_bytes + (texels % 64 ? tile_bytes / 8 : 0);
}

/* Byte offset of half tile @h and back.  The two 32-column halves of a 64x16
 * tile interleave every 512 bytes, so a half tile is not contiguous and only
 * this index advances linearly across a chain. */
static inline size_t
ph_half_to_byte(size_t h)
{
   return (h >> 1) * 4096 + (h & 1) * 512;
}

static inline size_t
ph_byte_to_half(size_t bytes)
{
   return (bytes >> 12) * 2 + ((bytes >> 9) & 1);
}

/* Storage a tiled level actually reaches.  Its last band spills into the
 * half-tile the next level occupies, so the allocation must cover whole tiles
 * even though the next level starts earlier. */
static inline size_t
ph_tiled_level_reach(unsigned pitch, unsigned rows, unsigned tile_h,
                     size_t tile_bytes)
{
   const unsigned trows = rows >= tile_h ? rows / tile_h : 1u;

   return (((size_t)trows * pitch + 63) / 64) * tile_bytes;
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
 * levels from 16x16 through 1x1 share one 32x16 block.  The slot is a plain
 * count of tail levels, not a function of the level's size: the vendor chain
 * builder mtx325 records the first level with w <= 16, h <= 16 and d <= 31 at
 * .text+0x28c43 and indexes the three tables with level - that index at
 * .text+0x28a6d..0x28aad. */
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
   size_t next = 0, tail_base = 0, reach = 0, next_half = 0;
   /* The P690 32-bit chain advances in half tiles: a level begins where the
    * previous one ends, which is a whole tile only when its row bands happen
    * to end on one.  Measured over pitches 64..224 and heights 16..65 with
    * parhelia-drm-test --mip-address-probe. */
   const bool halves = p690 && tiled && cpp == 4;
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
      unsigned pitch, rows, level_align, pack_align;
      size_t bytes = (size_t)32 * (tiled ? 32 : 16) * cpp;

      level->width = width;
      level->height = height;
      /* The address unit rounds the descriptor's width up to 32 texels, not
       * to the next power of two: parhelia-drm-test --mip-address-probe 96 64
       * and 129 65 fetch tile rows 96/64 and 160/64 apart.  Every earlier
       * measurement used a power-of-two width, where the two agree. */
      pitch = ph_mip_align(width, 32);
      pitch = pitch < 32 ? 32 : pitch;
      level->pitch = pitch;
      level_align = tiled ? 0x1000 : 0x40;
      /* A level begins exactly where the previous one ends: the measured
       * bases of 96x40 and 129x65 advance by the level size alone, with no
       * tile rounding, which would swallow the 512-byte granule a mid-tile
       * row band leaves.  Only the shared tail block starts on a whole
       * tile. */
      pack_align = p690 && tiled && cpp == 4 ? 0x200u : level_align;

      if ((width <= 16 || (p690 && tiled && width <= 32)) && height <= 16) {
         unsigned x = 0, y = 0;

         if (!tail_started) {
            tail_base = halves ? ph_half_to_byte(next_half) :
                        ph_mip_align((unsigned)next, level_align);
            /* The shared tail block is one whole 64x16 tile. */
            next_half += 2;
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
         /* Every P690 level quantises to 16 rows, at a 32-texel pitch too:
          * the 24x10 level of 96x40 is followed 0x800 bytes on, one 32x16
          * block, not the 32-row 0x1000 the 64x32 orders reserve. */
         rows = !tiled ? height :
                p690 ? ph_mip_align(height, 16) : ph_mip_align(height, 32);
         bytes = (size_t)pitch * rows * cpp;
         /* A 32-texel pitch addresses 32x16-texel 2 KiB blocks row-major and
          * needs no such correction; the 64-wide tile orders do. */
         if (tiled && !halves && pitch >= 64)
            bytes = ph_tiled_level_bytes(pitch, rows, 32u, 2048u * cpp);
         if (halves) {
            level->offset = ph_half_to_byte(next_half);
            next_half += (size_t)(rows / 16) * (pitch / 32);
            bytes = ph_half_to_byte(next_half) - level->offset;
         } else {
            level->offset = ph_mip_align((unsigned)next, pack_align);
         }
         next = level->offset + bytes;
      }
      level->bytes = bytes;
      /* A level's last band spills into the half tile the next level starts
       * in, so the allocation covers whole tiles either way. */
      if (halves && (next_half + 1) / 2 * 4096 > reach)
         reach = (next_half + 1) / 2 * 4096;
      if (next > reach)
         reach = next;

      width = width > 1 ? width >> 1 : 1;
      height = height > 1 ? height >> 1 : 1;
   }
   out->size = ph_mip_align((unsigned)reach, tiled ? 0x2000 : 0x200);
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
ph_mip_log2(unsigned value)
{
   unsigned bits = 0;

   while ((1u << bits) < value)
      bits++;
   return bits;
}

/* Byte offset of texel (x,y) of slice @z in the measured P690 volume order
 * (parhelia-drm-test --texture-volume-probe; see
 * docs/parhelia-p690-volume-cube-targets.md).  It is built from:
 *
 *   bits 0..6  a 512-byte granule of 32x4 texels, the same seven low bits
 *              the plain 2-D order uses;
 *   bit 7      the slice index's low bit, exclusive-ORed with the 32-wide
 *              half selector x5 ^ trow the 64x16 texture tile already uses;
 *   bits 8,9   y2 and y3, the four granules of a 32x16 sub-tile;
 *   bits 10..  (s * @slices + z) >> 1, where s is the 32x16 sub-tile index
 *              with its bit 0 replaced by that same half selector.
 *
 * So the slices of one granule are consecutive: a volume is neither a stack
 * of whole 2-D images nor the vendor's generic whole-block interleave.  For a
 * power-of-two depth the last field is the XOR-linear (s << log2(D)-1) |
 * z >> 1; a depth of 3 or 5 multiplies, measured at 16x64x3, 64x32x3 and
 * 32x48x5 tile corner by tile corner.  @pitch is the descriptor width
 * rounded up to 32 (measured at 32 and 64). */
static inline size_t
ph_volume_texel_offset(unsigned x, unsigned y, unsigned z, unsigned pitch,
                       unsigned slices, unsigned cpp)
{
   const unsigned half = ((x >> 5) ^ (y >> 4)) & 1u;
   const unsigned tile = (y >> 4) * (pitch >> 5) + (x >> 5);
   const unsigned granule = (x & 7u) | ((y & 1u) << 3) |
                            (((x >> 3) & 3u) << 4) | (((y >> 1) & 1u) << 6);
   const size_t slot = (size_t)((tile & ~1u) | half) * slices + z;

   return (granule | (((z & 1u) ^ half) << 7) | (((y >> 2) & 3u) << 8) |
           ((slot >> 1) << 10)) * cpp;
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
