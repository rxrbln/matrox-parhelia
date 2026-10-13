/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef PARHELIA_BATCH_H
#define PARHELIA_BATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "parhelia_drm.h"
#include "reference/parhelia3d.h"

#define PH_BATCH_LEGACY_PACKETS 221
#define PH_BATCH_PACKETS 1024
#define PH_BATCH_LEGACY_DWORDS 4096
#define PH_BATCH_DWORDS 16384
#define PH_BATCH_VERTEX_DWORDS (PH_BATCH_PACKETS * 3 * 10)
#define PH_BATCH_LEGACY_VERTEX_DWORDS (PH_BATCH_LEGACY_PACKETS * 3 * 10)
#define PH_BATCH_MAX_DRAW_DWORDS 4096
#define PH_BATCH_STATE_PACKET_RESERVE 64
#define PH_BATCH_MAX_DRAWS (PH_BATCH_PACKETS - PH_BATCH_STATE_PACKET_RESERVE)
/* Vertices one draw packet may consume; the vendor's own packets hold 24. */
#define PH_BATCH_MAX_PACKET_VERTICES 64u
#define PH_BATCH_RELOCS (PH_BATCH_PACKETS + 4)
/* The P690 primary ring has 224 CALL slots.  A normal submission consumes
 * three of them for its opening, EOB and terminal drain blocks. */
#define PH_BATCH_HW_CALLS 224

struct parhelia_batch_limits {
   unsigned command_dwords;
   unsigned packets;
   unsigned relocs;
   unsigned vertex_dwords;
};

struct parhelia_batch_mark {
   unsigned command_dwords;
   unsigned packets;
   unsigned relocs;
   unsigned vertex_dwords;
};

struct parhelia_batch;

/* A report of zero represents an unavailable or invalid GET_PARAM result.
 * Capacities above the legacy ABI are enabled only after CREATE_VERTEX has
 * produced a mapped buffer on the new dedicated-DMA kernel path. */
void parhelia_batch_negotiate_limits(uint64_t reported_packets,
                                     bool vertex_buffer_mapped,
                                     struct parhelia_batch_limits *limits);
int parhelia_batch_slice(struct parhelia_batch *dst,
                         const struct parhelia_batch *src,
                         const struct parhelia_batch_mark *begin,
                         const struct parhelia_batch_mark *end);
/* Predict the kernel execution-plan size after either its proven 0x97/0x8f
 * packing or capability-gated generic adjacent-packet packing.  The half-open
 * packet range is treated as one normal submit, including synthetic CALLs. */
int parhelia_batch_packed_call_count(const struct parhelia_batch *batch,
                                     unsigned first_packet,
                                     unsigned end_packet,
                                     bool pack_adjacent_packets,
                                     unsigned *calls);

/* No physical addresses or MMIO callbacks cross this interface. */
struct parhelia_batch {
   uint32_t commands[PH_BATCH_DWORDS];
   struct drm_parhelia_packet packets[PH_BATCH_PACKETS];
   struct drm_parhelia_reloc relocs[PH_BATCH_RELOCS];
   uint32_t command_count, packet_count, reloc_count;
};

/* Initial path: untextured window-space triangles; kernel-owned passthrough. */
int parhelia_batch_clear(struct parhelia_batch *batch, uint32_t colour_handle,
                        unsigned width, unsigned height, unsigned pitch,
                        uint32_t colour);
int parhelia_batch_clear_surface(struct parhelia_batch *batch, uint32_t handle,
                                 unsigned width, unsigned height,
                                 unsigned pitch, unsigned format,
                                 uint32_t value);
/* Solid fill of [x,x+width) x [y,y+height) inside a surface_width by
 * tile-row-aligned surface_height allocation.  Destination bits outside
 * write_mask are preserved by the GPU. */
int parhelia_batch_clear_rect_surface(struct parhelia_batch *batch,
                                      uint32_t handle, unsigned surface_width,
                                      unsigned surface_height, unsigned pitch,
                                      unsigned format, uint32_t value,
                                      uint32_t write_mask, unsigned x,
                                      unsigned y, unsigned width,
                                      unsigned height);
/* One vendor Z control command (a P3D_ZTRACK_ or P3D_ZINIT_ word) against the
 * depth surface bound in slot @slot.  @load writes the Z-init value first;
 * @resume is the draw word DWGCTL is left holding. */
int parhelia_batch_depth_control(struct parhelia_batch *batch,
                                 uint32_t depth_handle, unsigned pitch,
                                 unsigned format, unsigned slot,
                                 uint32_t control, bool load, uint32_t value,
                                 uint32_t resume);
/* The P690 hardware depth clear: mark every tile of @depth_handle as holding
 * @value without writing memory.  The value is materialised by the Z unit on
 * first access; parhelia_batch_zdiscard() drops what is left. */
int parhelia_batch_zclear(struct parhelia_batch *batch, uint32_t depth_handle,
                          unsigned pitch, unsigned format, uint32_t value);
int parhelia_batch_zclear_original(struct parhelia_batch *batch,
                                 uint32_t colour_handle, unsigned colour_pitch,
                                 unsigned colour_format, uint32_t depth_handle,
                                 unsigned pitch, unsigned format,
                                 uint32_t value);
int parhelia_batch_zdiscard(struct parhelia_batch *batch,
                            uint32_t depth_handle, unsigned pitch,
                            unsigned format);
int parhelia_batch_zdiscard_original(struct parhelia_batch *batch,
                                     uint32_t depth_handle, unsigned pitch,
                                     unsigned format);
int parhelia_batch_zclear_word(struct parhelia_batch *batch,
                               uint32_t depth_handle, unsigned pitch,
                               unsigned format, uint32_t value, uint32_t word);
void parhelia_batch_set_fill_strips(bool enable);
/* One-shot Z-unit control words for @depth_handle, in order (probes). */
int parhelia_batch_zcontrol(struct parhelia_batch *batch,
                            uint32_t depth_handle, unsigned pitch,
                            unsigned format, const uint32_t *words,
                            unsigned count);
int parhelia_batch_blit(struct parhelia_batch *batch,
                        uint32_t dst_handle, unsigned dst_pitch,
                        unsigned dst_type, unsigned dst_x, unsigned dst_y,
                        uint32_t src_handle, unsigned src_pitch,
                        unsigned src_type, unsigned src_x, unsigned src_y,
                        unsigned width, unsigned height);
/* The same copy with explicit surface format codes; the pitches are in
 * pixels of that format and both ends must be the same size. */
int parhelia_batch_blit_format(struct parhelia_batch *batch,
                               uint32_t dst_handle, unsigned dst_pitch,
                               unsigned dst_format, unsigned dst_type,
                               unsigned dst_x, unsigned dst_y,
                               uint32_t src_handle, unsigned src_pitch,
                               unsigned src_format, unsigned src_type,
                               unsigned src_x, unsigned src_y,
                               unsigned width, unsigned height);
int parhelia_batch_triangles(struct parhelia_batch *batch,
                            uint32_t colour_handle, uint32_t warp_entry,
                            unsigned width, unsigned height, unsigned pitch,
                            unsigned triangle_count);
/* Reference fixed-function state, including the gears xyz/normal layout,
 * grouped constants and Z24. Surface bases must be zero; handles are resolved
 * by the kernel. One triangle per DRAW preserves the example's CALL
 * boundaries; parhelia_batch_state_run() draws longer runs per packet.
 */
int parhelia_batch_state(struct parhelia_batch *batch,
                        uint32_t colour_handle, uint32_t depth_handle,
                        const struct p3d_state *state, unsigned triangle_count);
/* As above, with one local-VRAM texture backing the state->tex descriptor.
 * The texture base remains zero in userspace and is relocated by the kernel.
 */
int parhelia_batch_state_textured(struct parhelia_batch *batch,
                                 uint32_t colour_handle,
                                 uint32_t depth_handle,
                                 uint32_t texture_handle,
                                 const struct p3d_state *state,
                                 unsigned triangle_count);
/* Two local-VRAM textures backing state->tex and state->tex1.  Each surface
 * base remains zero in userspace and is independently relocated by the
 * kernel.  The vertex stream must use the 9- or 10-dword two-coordinate-set
 * layout described by the p3d state. */
int parhelia_batch_state_multitextured(struct parhelia_batch *batch,
                                      uint32_t colour_handle,
                                      uint32_t depth_handle,
                                      uint32_t texture0_handle,
                                      uint32_t texture1_handle,
                                      const struct p3d_state *state,
                                      unsigned triangle_count);
/* One local-VRAM texture per active unit of state (tex .. tex3), each handle
 * relocated into its unit's surface base. */
int parhelia_batch_state_units(struct parhelia_batch *batch,
                               uint32_t colour_handle, uint32_t depth_handle,
                               const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS],
                               const struct p3d_state *state,
                               unsigned triangle_count);
/* Draw a run of @vertices vertices of state->prim, at most
 * state->vertices_per_packet of them per draw packet, with either texture
 * handle zero when that unit is unused.  Only the last packet may be short. */
int parhelia_batch_state_run(struct parhelia_batch *batch,
                             uint32_t colour_handle, uint32_t depth_handle,
                             uint32_t texture0_handle,
                             uint32_t texture1_handle,
                             const struct p3d_state *state, unsigned vertices);
/* The same with one texture handle per unit, for three or four units. */
int parhelia_batch_state_run_units(struct parhelia_batch *batch,
                                   uint32_t colour_handle, uint32_t depth_handle,
                                   const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS],
                                   const struct p3d_state *state,
                                   unsigned vertices);
/* A cache of built state prefixes (see parhelia_batch.c); the cached variant
 * is byte-identical to parhelia_batch_state_run_units(). */
struct parhelia_state_cache;
struct parhelia_state_cache *parhelia_state_cache_create(void);
void parhelia_state_cache_destroy(struct parhelia_state_cache *cache);
int parhelia_batch_state_run_units_cached(struct parhelia_state_cache *cache,
                                          struct parhelia_batch *batch,
                                          uint32_t colour_handle,
                                          uint32_t depth_handle,
                                          const uint32_t texture_handles[P3D_MAX_TEXTURE_UNITS],
                                          const struct p3d_state *state,
                                          unsigned vertices);
int parhelia_batch_submit(int fd, const struct parhelia_batch *batch,
                         const void *vertices, unsigned vertex_dwords,
                         uint32_t *fence);
/* The kernel copies commands and class-3 vertices into a fenced request slot
 * before this returns.  Callers still have to retain every referenced GEM
 * resource until parhelia_wait_fence() reports the terminal DWGSYNC tag. */
int parhelia_batch_submit_async(int fd, const struct parhelia_batch *batch,
                                const void *vertices, unsigned vertex_dwords,
                                uint32_t *fence);
int parhelia_batch_submit_async_bounded(
   int fd, const struct parhelia_batch *batch, const void *vertices,
   unsigned vertex_dwords, uint32_t *fence);
/* Append one independently validated batch to another submit.  Packet command
 * offsets, packet-indexed relocations and byte-addressed DRAW relocations are
 * rebased while preserving the original packet order. */
int parhelia_batch_append(struct parhelia_batch *dst,
                          uint32_t *dst_vertices,
                          unsigned *dst_vertex_dwords,
                          const struct parhelia_batch *src,
                          const void *src_vertices,
                          unsigned src_vertex_dwords);
int parhelia_batch_append_limited(struct parhelia_batch *dst,
                                  uint32_t *dst_vertices,
                                  unsigned *dst_vertex_dwords,
                                  const struct parhelia_batch *src,
                                  const void *src_vertices,
                                  unsigned src_vertex_dwords,
                                  const struct parhelia_batch_limits *limits);
/* Append a complete logical state section and insert a metadata-only CALL
 * boundary before it.  Boundary-aware kernels use the marker only to split
 * adjacent-packet secondary spans; it emits no GPU synchronization. */
int parhelia_batch_append_call_bounded(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords);
int parhelia_batch_append_call_bounded_limited(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords,
   const struct parhelia_batch_limits *limits);
/* Extend an already queued state batch with only the DRAW tail of a second,
 * byte-identical state batch.  This retains one hardware state/fence cadence
 * and is equivalent to p3d_emit_draws() producing more consecutive draws. */
int parhelia_batch_append_same_state(struct parhelia_batch *dst,
                                     uint32_t *dst_vertices,
                                     unsigned *dst_vertex_dwords,
                                     const struct parhelia_batch *src,
                                     const void *src_vertices,
                                     unsigned src_vertex_dwords);
int parhelia_batch_append_same_state_limited(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords,
   const struct parhelia_batch_limits *limits);
/* Append a complete, possibly different state batch.  Starting with the
 * second batch, a kernel-only logical boundary asks submit to retain the
 * recovered EOB/DWGSYNC/opening cadence between the two packet sequences.
 * This marker never reaches the command stream as an address or register. */
int parhelia_batch_append_stateful(struct parhelia_batch *dst,
                                   uint32_t *dst_vertices,
                                   unsigned *dst_vertex_dwords,
                                   const struct parhelia_batch *src,
                                   const void *src_vertices,
                                   unsigned src_vertex_dwords);
int parhelia_batch_append_stateful_limited(
   struct parhelia_batch *dst, uint32_t *dst_vertices,
   unsigned *dst_vertex_dwords, const struct parhelia_batch *src,
   const void *src_vertices, unsigned src_vertex_dwords,
   const struct parhelia_batch_limits *limits);
/* Coalesce adjacent, ordinary register-list packets while preserving their
 * register-write order.  Packets which carry a relocation or touch a fetch,
 * WARP, surface, texture-bank, draw-arm, fence, or strobe register are hard
 * boundaries.  The merged packet is additionally capped at 64 dwords.  This
 * is enabled for normal window-space batches after the P690 image, depth,
 * texture, multitexture, alpha and hang gates; setting
 * PARHELIA_COMPACT_STATE_PACKETS=0 retains the byte-separated diagnostic
 * stream.  It neither drops nor caches any state write. */
int parhelia_batch_compact_reglists(struct parhelia_batch *dst,
                                    const struct parhelia_batch *src);
int parhelia_wait_fence(int fd, uint32_t fence, int64_t timeout_ns,
                        uint32_t *completed);
/* Convert normal draw relocations to a bounded CREATE_VERTEX class-3 DMA
 * ring range. This has no side effects and is also used by the packet test. */
int parhelia_batch_vram_relocs(const struct parhelia_batch *batch,
                               uint32_t vertex_handle,
                               uint32_t vertex_offset,
                               unsigned vertex_dwords,
                               struct drm_parhelia_reloc *out,
                               unsigned out_count);
/* The persistent ring is mapped dedicated DMA32 memory. The kernel verifies
 * the object and every range; callers retain parhelia_batch_submit() as
 * fallback. */
int parhelia_batch_submit_vram(int fd, const struct parhelia_batch *batch,
                               uint32_t vertex_handle,
                               uint32_t vertex_offset,
                               unsigned vertex_dwords,
                               uint32_t *fence);
/* Async form of the same bounded class-3 relocation.  The caller owns the
 * mapped subrange until the returned terminal fence has retired. */
int parhelia_batch_submit_vram_async(int fd, const struct parhelia_batch *batch,
                                     uint32_t vertex_handle,
                                     uint32_t vertex_offset,
                                     unsigned vertex_dwords,
                                     uint32_t *fence);
int parhelia_batch_submit_vram_async_bounded(
   int fd, const struct parhelia_batch *batch, uint32_t vertex_handle,
   uint32_t vertex_offset, unsigned vertex_dwords, uint32_t *fence);
/* The same with one PARHELIA_RELOC_DRAW_RANGES for every draw packet: each
 * draw's range is written into its packet.  -ENOTSUP for a batch that needs
 * the per-packet form. */
int parhelia_batch_submit_ranges_async(
   int fd, struct parhelia_batch *batch, uint32_t vertex_handle,
   uint32_t vertex_offset, unsigned vertex_dwords, bool call_boundaries,
   uint32_t *fence);

/* Deliberately narrow P690 mode-7 exercise builder.  It is for the DRM
 * screen test only: one untextured interleaved class-3 stream and one u32
 * triangle index list.  Production Gallium submission continues to use the
 * independently proven 0x97 paths until this ABI has hardware evidence. */
int parhelia_batch_mode7_screen_test(struct parhelia_batch *batch,
                                     uint32_t colour_handle,
                                     uint32_t vertex_handle,
                                     unsigned width, unsigned height,
                                     unsigned pitch, unsigned warp_entry,
                                     unsigned vertex_dwords,
                                     unsigned vertex_count,
                                     unsigned vertex_offset,
                                     unsigned index_offset,
                                     unsigned index_count);

/* Test-only P690 mode-3 probe.  It draws exactly six expanded vertices from
 * one kernel-owned class-3 stream, with optional XYZ+RGB depth state and the
 * selected 0x8f range boundary arrangement. */
enum parhelia_mode3_range_case {
   PARHELIA_MODE3_ONE_RANGE,
   PARHELIA_MODE3_TWO_RANGES_ONE_PACKET,
   PARHELIA_MODE3_TWO_PACKETS,
};
int parhelia_batch_mode3_screen_test(struct parhelia_batch *batch,
                                     uint32_t colour_handle,
                                     uint32_t depth_handle,
                                     unsigned width, unsigned height,
                                     unsigned pitch, unsigned warp_entry,
                                     enum parhelia_mode3_range_case range_case);

/* Kernel-owned tiled render allocation; separate from Xorg dumb scanout. */
int parhelia_create_render(int fd, unsigned width, unsigned height,
                           struct drm_parhelia_create_render *allocation);

/* Import the complete C example's --batchsize 1 output. Preserve its CALLs
 * and GPU colour/depth clears; replace only kernel-owned fences, program
 * placeholders, surface addresses and vertex ranges. This is a bring-up
 * adapter, not a general command-stream validation/security boundary.
 */
struct parhelia_reference_bindings {
   uint32_t colour_base, depth_base, colour_handle, depth_handle;
   uint32_t vertex_entry, loader_entry;
};
int parhelia_batch_reference(struct parhelia_batch *batch,
                            const struct p3d_cs *source,
                            const struct parhelia_reference_bindings *bindings,
                            unsigned vertex_dwords);

#endif
