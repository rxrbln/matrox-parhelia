# CPU alpha-cutout performance audit

This review covers the opt-in `PARHELIA_CPU_ALPHA_CUTOUT` fallback in the Mesa Parhelia driver. It does not alter driver or target state.

## Finding

`ph_stage_tri()` calls `ph_use_cpu_alpha_cutout()` for each frontend triangle (`parhelia_context.c:1643`). After strict state checks, the predicate unconditionally calls `ph_resource_upload_tiled()` and `ph_texture_base_hash()` (`:641-644`). The upload returns cheaply while current, but the hash walks every base-level texel and four bytes per texel (`:553-578`). Thus each candidate has O(texture_width * texture_height) work per triangle. The observed Q3 cutout images are 8x16 and 32x64; a repeated 32x64 signature is 2048 texels / 8192 byte-mixes per triangle before rasterization.

The rasterizer has a separate and larger cost. Every accepted triangle scans its full screen-space bounding rectangle (`:1071-1115`). Covered pixels recompute three floating edge tests, perspective interpolation, bilinear sampling, tiled depth access, and tiled colour access. `ph_cpu_sample_rgba()` repeats the four tiled texel addresses and repeat modulo operations for every component (`:970-985`), or 16 tiled-address calculations per shaded pixel. Large foliage or portal triangles will remain CPU-limited after selector caching.

## Minimal selector cache

Use the context-local alpha-blend cache form (`parhelia_context.c:137-143`, `:718-735`) for cutout: cache resource identity, native GEM handle, content generation, base hash, and whether it matches the two proven cutout hashes. Call `ph_resource_upload_tiled(texture)` first and hash only if that tuple differs. Retain the exact hash comparison. A one-entry cache removes repeat hashing for the same texture; two direct entries also cover alternation between the two Q3 images.

## Correct invalidation boundary

Dirty flags alone are not a correct cache key. `ph_unmap()` writes both linear and tiled images then leaves `linear_dirty = false`; with a linear shadow it leaves `tiled_dirty = false` too (`parhelia_context.c:2611-2651`). The existing blend cache's `linear_dirty || tiled_dirty` test (`:721-735`) can therefore reuse a stale signature after a normal `texture_map(... PIPE_MAP_WRITE)` upload.

Add `uint64_t content_generation` to `struct parhelia_resource` (`parhelia_internal.h:21-48`) and increment it after every successful content-changing operation. Required hooks are texture `PIPE_MAP_WRITE` unmap (`context.c:2611-2651`), `ph_copy_texture_box()` destination writes (`:2387-2418`), `ph_clear_surface()` (`:2267-2278`), CPU/GPU Present destination writes (`:403-442`), and CPU or GPU triangle destination writes when a render target may later be sampled (`:1248-1265`, `:1598-1601`). Cache both cutout and blend signatures against `(resource pointer, handle, content_generation)`. To make recreated resources lifetime-safe, use a monotonic creation serial as well, or clear cache slots when sampler views rebind.

## Safe next optimization after caching

If profiling still identifies CPU cutout as material, hoist top-left flags and edge gradients from the inner loops in `ph_cpu_cutout_triangle()` (`:1077-1090`) and increment edge values across each scanline. In `ph_cpu_sample_rgba()`, compute four wrapped coordinates and tiled texel pointers once, then interpolate all four channels. These preserve sampling and depth rules, but require a pixel-reference test before deployment.
