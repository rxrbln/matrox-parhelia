/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Row copies between a linear host image and a tiled Parhelia image.
 *
 * Every Parhelia texture and surface order keeps the lowest x bits of a texel
 * in the lowest address bits: 8 texels (32 bytes) of a 32-bit row are one
 * contiguous run, 4 or 8 of a narrow one.  A row is therefore copied run by
 * run, with one address computation per run instead of one per texel.  The
 * run length is measured from the order itself, so an order without such
 * runs (the P690's 32-texel compact level) falls back to single texels.
 */
#ifndef PARHELIA_TILE_COPY_H
#define PARHELIA_TILE_COPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util/u_endian.h"

/* Byte offset of texel (x,y) in the tiled image described by @ctx. */
typedef size_t (*ph_texel_offset_fn)(const void *ctx, unsigned x, unsigned y);

/* Byte i of a converted 32-bit texel is byte src[i] of the original, or
 * 0xff when src[i] is 0xff. */
struct ph_swizzle {
   uint8_t src[4];
};

#define PH_RUN_BYTES 64

static inline void
ph_swizzle_texels(uint8_t *dst, const uint8_t *src, unsigned n,
                  const struct ph_swizzle *sw)
{
#if UTIL_ARCH_LITTLE_ENDIAN
   uint32_t fill = 0, shift[4], keep[4];

   for (unsigned i = 0; i < 4; i++) {
      keep[i] = sw->src[i] < 4 ? 0xffu : 0;
      shift[i] = sw->src[i] < 4 ? 8 * sw->src[i] : 0;
      if (!keep[i])
         fill |= 0xffu << (8 * i);
   }
   for (unsigned t = 0; t < n; t++) {
      uint32_t v, o = fill;

      memcpy(&v, src + 4 * t, 4);
      o |= ((v >> shift[0]) & keep[0]) |
           ((v >> shift[1]) & keep[1]) << 8 |
           ((v >> shift[2]) & keep[2]) << 16 |
           ((v >> shift[3]) & keep[3]) << 24;
      memcpy(dst + 4 * t, &o, 4);
   }
#else
   for (unsigned t = 0; t < n; t++)
      for (unsigned i = 0; i < 4; i++)
         dst[4 * t + i] = sw->src[i] < 4 ? src[4 * t + sw->src[i]] : 0xff;
#endif
}

/* The reverse of @to: byte to->src[i] of the result is byte i, and bytes no
 * entry names read 0xff.  A later entry wins, as in a per-byte loop. */
static inline void
ph_swizzle_inverse(struct ph_swizzle *inv, const struct ph_swizzle *to)
{
   memset(inv->src, 0xff, sizeof(inv->src));
   for (unsigned i = 0; i < 4; i++)
      if (to->src[i] < 4)
         inv->src[to->src[i]] = i;
}

/* Longest power-of-two run, up to PH_RUN_BYTES, that the order keeps
 * contiguous, measured on the first whole aligned run of the row. */
static inline unsigned
ph_tile_row_run(ph_texel_offset_fn offset, const void *ctx, unsigned x0,
                unsigned y, unsigned w, unsigned cpp)
{
   unsigned run;

   for (run = PH_RUN_BYTES / cpp; run > 1; run >>= 1) {
      const unsigned xa = (x0 + run - 1) & ~(run - 1);
      size_t base;
      unsigned i;

      if (xa + run > x0 + w)
         continue;
      base = offset(ctx, xa, y);
      for (i = 1; i < run; i++)
         if (offset(ctx, xa + i, y) != base + (size_t)i * cpp)
            break;
      if (i == run)
         return run;
   }
   return 1;
}

/* Copy texels [x0, x0 + w) of row @y between @host (linear) and the tiled
 * image at @base of @size bytes, converting 32-bit texels by @sw when set.
 * Texels the order places outside the image are skipped; returns how many. */
static inline unsigned
ph_tile_row_copy(uint8_t *base, size_t size, ph_texel_offset_fn offset,
                 const void *ctx, unsigned x0, unsigned y, unsigned w,
                 uint8_t *host, bool to_tiled, const struct ph_swizzle *sw,
                 unsigned cpp)
{
   const unsigned run = ph_tile_row_run(offset, ctx, x0, y, w, cpp);
   const bool convert = sw && cpp == 4;
   uint8_t buf[PH_RUN_BYTES];
   unsigned x = x0, skipped = 0;

   while (x < x0 + w) {
      unsigned n = run - (x & (run - 1));
      size_t off = offset(ctx, x, y);
      uint8_t *h = host + (size_t)(x - x0) * cpp;

      if (n > x0 + w - x)
         n = x0 + w - x;
      /* A run is exact when its last texel follows on from its first. */
      if (n > 1 && (offset(ctx, x + n - 1, y) != off + (size_t)(n - 1) * cpp ||
                    off + (size_t)n * cpp > size))
         n = 1;
      if (off + (size_t)n * cpp > size) {
         skipped++;
         x++;
         continue;
      }
      if (to_tiled) {
         if (convert) {
            ph_swizzle_texels(buf, h, n, sw);
            memcpy(base + off, buf, (size_t)n * 4);
         } else {
            memcpy(base + off, h, (size_t)n * cpp);
         }
      } else {
         if (convert) {
            memcpy(buf, base + off, (size_t)n * 4);
            ph_swizzle_texels(h, buf, n, sw);
         } else {
            memcpy(h, base + off, (size_t)n * cpp);
         }
      }
      x += n;
   }
   return skipped;
}

#endif
