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
#include <string.h>

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
   /* An original-family level that starts in the right half of its tile. */
   bool half;
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
   /* Levels the texture unit addresses as stored; 0 when all of them. */
   unsigned sampled_levels;
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

/* S3TC storage is tiled like the uncompressed texture order: 64x32-texel
 * tiles of 512-byte units 32 texels wide (8x8 DXT1 blocks or 8x4 DXT3/5
 * blocks, blocks row-major inside a unit), units left-right then top-down
 * inside a tile.  Tile rows advance by pitch/64 tiles - at a 32-texel pitch
 * two rows share one tile - and every odd tile row swaps the unit columns
 * (texel x bit 5 ^ texel y bit 5).  Measured on the P690 with
 * parhelia-drm-test --texture-dxt-probe PROBE_LINEAR=1 PROBE_MAP=1. */
static inline size_t
ph_dxt_block_offset(unsigned pitch, unsigned bx, unsigned by,
                    unsigned block_bytes)
{
   const unsigned unit_rows = 512u / (8u * block_bytes);
   const unsigned tile_bytes = 1024u * (8u / unit_rows);
   const unsigned ty = by >> 3;
   const size_t tile = ((size_t)ty * pitch >> 6) + (bx >> 4);
   const unsigned ux = ((bx >> 3) & 1u) ^ (ty & 1u);
   const unsigned uy = (by & 7u) / unit_rows;

   return tile * tile_bytes + (uy * 2u + ux) * 512u +
          (size_t)(by % unit_rows) * (8u * block_bytes) +
          (size_t)(bx & 7u) * block_bytes;
}

/* Bytes a compressed level of @pitch texels and @height rows occupies: its
 * 32-row bands of tiles (see ph_dxt_block_offset), two bands sharing a tile
 * at a 32-texel pitch. */
static inline size_t
ph_dxt_level_size(unsigned pitch, unsigned height, unsigned block_bytes)
{
   const unsigned tile_bytes = block_bytes == 8 ? 1024u : 2048u;
   const size_t bands = (height + 31) / 32;

   return ((bands * pitch + 63) >> 6) * tile_bytes;
}

/* A compressed chain as the P690's texture unit derives it from the base
 * (parhelia-drm-test --texture-dxt-probe PROBE_LEVELS=1 PROBE_LINEAR=1, both
 * block sizes, 16x16 to 512x512 and 8:1 rectangles: exact).  A
 * non-power-of-two base is addressed exactly at the next power-of-two pitch,
 * but its chain packs levels by the unit columns they use, which is not
 * measured yet: such a chain is stored here and sampled at level 0 only.  Levels follow one
 * another, each at pitch max(32, next power of two of its width).  The first
 * level no larger than 16x16 starts the tail 512 bytes into the previous
 * level's tile (texel x 32), or at 0 as the base; the levels after it sit at
 * texels (+16,0), (+24,0), (+16,+8) and (+24,+8) of the tail level. */
static inline void
ph_dxt_layout_init(struct ph_mip_layout *mip, unsigned width, unsigned height,
                   unsigned levels, unsigned block_bytes)
{
   static const unsigned tail_x[] = { 0, 16, 24, 16, 24 };
   static const unsigned tail_y[] = { 0, 0, 0, 8, 8 };
   const unsigned tile_bytes = block_bytes == 8 ? 1024u : 2048u;
   size_t offset = 0, end = 0;
   int tail = -1;

   /* over the generic layout of the same image: only its levels change */
   memset(mip->level, 0, sizeof(mip->level));
   mip->levels = levels;
   mip->block_bytes = block_bytes;
   mip->sampled_levels = (width & (width - 1)) || (height & (height - 1)) ?
                         1 : 0;
   for (unsigned l = 0; l < levels; l++) {
      struct ph_mip_level *lv = &mip->level[l];
      const unsigned w = width >> l ? width >> l : 1;
      const unsigned h = height >> l ? height >> l : 1;

      lv->width = w;
      lv->height = h;
      lv->pitch = ph_mip_next_pot(w) > 32 ? ph_mip_next_pot(w) : 32;
      if (tail < 0 && w <= 16 && h <= 16) {
         tail = l;
         lv->offset = l ? mip->level[l - 1].offset + 512u : 0;
         if (!l)
            end = tile_bytes;
      } else if (tail >= 0) {
         const unsigned k = l - tail < 4u ? l - tail : 4u;

         lv->offset = mip->level[tail].offset +
                      tail_y[k] / 4 * (8u * block_bytes) +
                      tail_x[k] / 4 * block_bytes;
         lv->tail_x = tail_x[k];
         lv->tail_y = tail_y[k];
      } else {
         lv->offset = offset;
         lv->bytes = ph_dxt_level_size(lv->pitch, h, block_bytes);
         offset += lv->bytes;
         end = offset;
      }
   }
   mip->size = end;
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

/* The original Parhelia's 32-bit chain in 64x32 tiles at a 32-aligned pitch.
 * The unit steps from level to level in 32x32 cells, half a tile each: a
 * level after an odd cell count starts in the right half (ph_mip_level.half)
 * and its 64x32 order continues one cell later.  The first level no larger
 * than 32x32 starts the two-cell tail: one larger than 16x16 takes its first
 * cell and the smaller levels sit in the vendor tail slots shifted right by
 * 32; one of 16x16 or less is itself slot 0 (fixfn-cubeprobe with an
 * address fill, every level of POT chains from 1x1 to 256x256 and of NPOT
 * chains up to 255). */
static inline bool
ph_mip_layout_native_64x32(struct ph_mip_layout *mip)
{
   size_t cell = 0, tail_cell = 0;
   unsigned tail = 0, shift = 0;

   mip->native_64x32 = true;
   for (unsigned l = 0; l < mip->levels; l++) {
      struct ph_mip_level *level = &mip->level[l];
      const size_t rows = level->height > 32 ?
                          ph_mip_align(level->height, 32) / 32 : 1;

      if (level->width <= 32 && level->height <= 32) {
         if (!tail) {
            tail_cell = cell;
            cell += 2;
            shift = level->width > 16 || level->height > 16 ? 32 : 0;
         }
         if (shift && !tail) {
            level->tail_x = level->tail_y = 0;
         } else if (tail - !!shift < 5) {
            ph_mip_tail_position(tail - !!shift, &level->tail_x,
                                 &level->tail_y);
            level->tail_x += shift;
         } else {
            return false;
         }
         tail++;
         level->pitch = 64;
         level->offset = tail_cell / 2 * 0x2000;
         level->half = tail_cell & 1;
         level->bytes = 0x2000;
         continue;
      }
      level->pitch = ph_mip_align(level->width, 32);
      level->offset = cell / 2 * 0x2000;
      level->half = cell & 1;
      level->tail_x = level->tail_y = 0;
      level->bytes = rows * (level->pitch / 32) * 0x1000;
      cell += rows * (level->pitch / 32);
   }
   if ((cell + 1) / 2 * 0x2000 > mip->size)
      mip->size = (cell + 1) / 2 * 0x2000;
   return true;
}

/* Whether the unit reads every cell of a 64x32 chain apart: a level of two
 * or more cell rows with an even cell count per row that starts in a right
 * half maps two of its cells onto one (96x96 level 1, MEASURED), so such a
 * chain cannot be sampled past level 0. */
static inline bool
ph_mip_native_64x32_chain_exact(unsigned width, unsigned height,
                                unsigned levels)
{
   struct ph_mip_layout mip;

   if (!ph_mip_layout_init_cpp(&mip, width, height, levels, true, false, 4) ||
       !ph_mip_layout_native_64x32(&mip))
      return false;
   for (unsigned l = 0; l < levels; l++)
      if (mip.level[l].half && mip.level[l].height > 32 &&
          !((mip.level[l].pitch / 32) & 1))
         return false;
   return true;
}

/* The original family's cube chain (vendor mtx3887 order, MEASURED on the
 * 0528 at 32, 64 and 128): level-major with the six faces of a level in
 * consecutive 32x32 cells; from the first level of 16 or less each face
 * gets one tail cell holding that level in slot 0 and the smaller ones in
 * the vendor tail slots. */
static inline bool
ph_mip_layout_cube_64x32(struct ph_mip_layout *mip, unsigned faces)
{
   size_t cell = 0, tail_cell = 0;
   unsigned tail = 0;

   mip->native_64x32 = true;
   for (unsigned l = 0; l < mip->levels; l++) {
      struct ph_mip_level *level = &mip->level[l];

      level->tail_x = level->tail_y = 0;
      if (level->width <= 16) {
         if (!tail) {
            tail_cell = cell;
            cell += faces;
         } else if (tail >= 5) {
            return false;
         }
         ph_mip_tail_position(tail++, &level->tail_x, &level->tail_y);
         level->pitch = 32;
         level->offset = tail_cell / 2 * 0x2000;
         level->half = tail_cell & 1;
         level->bytes = 0x1000;
         continue;
      }
      level->pitch = ph_mip_align(level->width, 32);
      level->offset = cell / 2 * 0x2000;
      level->half = cell & 1;
      level->bytes = (size_t)level->pitch / 32 *
                     (ph_mip_align(level->height, 32) / 32) * 0x1000;
      cell += level->bytes / 0x1000 * faces;
   }
   mip->size = (cell + 1) / 2 * 0x2000;
   return true;
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

/* The original family's volume order: the P690 rule above on 64x32 tiles.
 * The in-tile bits are the 2-D 64x32 order's, bit 7 is z0 ^ x5 ^ trow, and
 * bits 11.. are (s * @slices + z) >> 1 with s the 32x32 cell index whose
 * bit 0 is that same half selector (--texture-volume-probe on the 0528:
 * 32x32x2, 32x32x6, 32x64x4, 64x32x3, 64x64x8 and 128x32x4, every texel). */
static inline size_t
ph_volume_texel_offset_64x32(unsigned x, unsigned y, unsigned z,
                             unsigned pitch, unsigned slices, unsigned cpp)
{
   const unsigned trow = y >> 5, iy = y & 31u;
   const unsigned half = ((x >> 5) ^ trow) & 1u;
   const unsigned cell = trow * (pitch >> 5) + (x >> 5);
   const unsigned granule = (x & 7u) | (((iy >> 1) & 1u) << 3) |
                            (((x >> 3) & 3u) << 4) | ((iy & 1u) << 6) |
                            ((iy >> 2) << 8);
   const size_t slot = (size_t)((cell & ~1u) | half) * slices + z;

   return (granule | (((z & 1u) ^ half) << 7) | ((slot >> 1) << 11)) * cpp;
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
