# P690 Q3 Mesa submission-boundary audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Question and scope

This is a source-and-capture audit of submission boundaries which can be
removed *before* the asynchronous kernel queue exists. It covers the
software-vertex Mesa driver in `mesa-26.2.2` and the retained working P690
vendor Q3 capture. It does not change driver source or the target.

The current performance evidence remains decisive: Q3 made 1,502 DRM ioctls
in 5.606960 seconds (3.732 ms per ioctl) while glxgears reaches about 273 FPS
without Q3. Therefore an optimization must remove a real synchronous submit
or a real synchronous resolve. Merely making a state setter shorter cannot
move this bottleneck.

## What is already batched correctly

`ph_stage_flush()` deliberately does **not** call `ph_submit_vertices()`
(`src/gallium/drivers/parhelia/parhelia_context.c:1791-1800`). A Gallium
`draw_flush()` after every `ph_draw_vbo()` is therefore not itself a hardware
submission boundary. The transformed triangles remain in `ctx->vertices`
until one of the conditions below or the capacity check.

The hard capacity is real. The kernel's class-3 upload is 4,096 dwords, and
the driver checks `(triangles + 1) * 3 * dpv > 4096` at
`parhelia_context.c:1698-1703`. The CPU arrays and batch builder also have a
180-triangle bound. Raising either limit alone would overflow the accepted
kernel vertex transport; it is not a batching optimization.

Likewise, the one-texture-pipe P690 diffuse-times-lightmap path issues two
independent batches in `ph_submit_vertices()` (roughly lines 1543-1610). The
second pass reads the colour result from the first and applies
`DST_COLOR,ZERO` (or `ONE,ONE`). Combining those two passes without a proven
dual-pipe program changes the image or wedges this P690, so it is not a safe
submit-count reduction.

## Existing drains, classified

| Site | Current condition | Can it be skipped? | Reason |
|---|---|---|---|
| blend, DSA and rasterizer bind | state-object pointer changes (`1880-1908`) | No, except the existing identical-pointer fast path | The pending CPU vertices have no hardware-state snapshot; the batch builder reads these objects at submit time. |
| sampler-state bind | a bound sampler pointer changes (`1915-1931`) | No, except the existing all-identical fast path | Filter, wrap and mip fields are emitted for the pending batch. |
| sampler-view bind | a view is replaced or cleared (`1965-1990`) | No, except the existing equality fast path | The texture relocation and layout are emitted at submit time. |
| fragment-shader bind | FS pointer changes (`2221-2229`) | No | It selects the WARP program, texenv and serial lightmap operation. |
| framebuffer bind | **always** (`2305-2312`) | **Yes when `util_framebuffer_state_equal(&ctx->fb, fb)` is true** | The current code drains even when every colour/depth surface, level, format, dimensions, samples, layers, resolve resource and view mask are unchanged. |
| clear / CPU copy or blit | always before the operation (`2349`, `2409`) | No | The next operation needs the preceding render result, and CPU copy reads it immediately. |
| `flush_resource` | always (`2377-2391`) | No | It makes the named resource linear and CPU-visible. |
| generic `flush` | drains plus always resolves the colour target (`2363-2374`) | **The resolve can be deferred; the drain cannot** | A generic flush establishes GPU command order. It does not itself name a CPU reader or exported linear image. |

`util_framebuffer_state_equal()` is already included through
`util/u_framebuffer.h`. Its implementation compares width, height, samples,
layers, every colour surface, Z/S surface, resolve resource, view mask and PLS
state (`src/gallium/auxiliary/util/u_framebuffer.c:49-89`). It is therefore a
stronger test than comparing only render-target handles.

The working vendor capture is consistent with keeping logical state and
completion separate: its first `glFinish` follows 17 command doorbells
(`captures/ioq3-p690-working-draw.txt:5-74`), and later drawing is followed by
explicit `glFinish` calls (`328-395`). It does not establish a vendor
equivalent for a redundant framebuffer setter, but it confirms that a GL
completion point is distinct from ordinary command submission.

## Small, safe source change: ignore equal framebuffer binds

Make `ph_set_framebuffer()` return before the drain when the complete Gallium
framebuffer state compares equal:

```c
static void
ph_set_framebuffer(struct pipe_context *p,
                   const struct pipe_framebuffer_state *fb)
{
   struct ph_context *ctx = ph_context(p);

   if (fb && util_framebuffer_state_equal(&ctx->fb, fb))
      return;
   ph_submit_vertices(ctx);
   util_copy_framebuffer_state(&ctx->fb, fb);
   if (fb && fb->zsbuf.texture)
      draw_set_zs_format(ctx->draw, fb->zsbuf.format);
}
```

The `fb &&` guard preserves the existing `NULL`-state behaviour:
`util_copy_framebuffer_state()` accepts `NULL` to release the old framebuffer.
For every non-null equal state this changes neither a resource reference nor
Draw's Z/S format. It is a zero-risk removal of an otherwise unconditional
batch boundary. Its expected benefit must be measured: the static code proves
the unnecessary drain, but the retained vendor capture does not quantify how
often current Mesa receives duplicate framebuffer binds from Q3.

Validation should run the existing triangle, depth, texture, alpha and Present
image checks, then log `framebuffer_equal_skips` and synchronous submits during
the same fixed Q3 demo. A duplicate-bind skip is valid only if it reduces
submits while all output hashes remain identical.

## Deferred resolve at generic `pipe_context::flush`

The current generic flush does:

1. submit pending triangles;
2. unconditionally call `ph_resource_resolve_linear()` for colour;
3. return no Gallium fence.

The resolve is a second synchronous hardware blit whenever `tiled_dirty` is
set (`parhelia_screen.c:148-176`). It is not necessary for command ordering:
the tiled render object remains authoritative. The driver already resolves at
the actual linear consumers:

* `ph_flush_resource()` for an explicitly flushed resource
  (`parhelia_context.c:2377-2391`);
* `ph_resource_get_handle()` before KMS/Present exports a linear handle
  (`parhelia_screen.c:483-512`);
* texture layout-0 sampling and CPU alpha routes before reading a linear copy
  (`parhelia_context.c:1381-1401`);
* texture-map reads (`parhelia_context.c:2623-2638`).

The small follow-up is to retain `ph_submit_vertices(ctx)` in `ph_flush()` but
remove its unconditional colour resolve. A later actual consumer performs the
existing resolve. This preserves ordering and eliminates a full-surface blit
from a generic `glFlush`/`glFinish` that is not followed by a linear consumer.

This must be gated by an image test because the current `ph_fence_finish()` is
a stub which always succeeds (`parhelia_screen.c:521-538`), and the current
`ph_flush()` also does not return a real fence. The proposed change is safe
only in the existing fully synchronous path, where `ph_submit_vertices()` has
already retired. Do not apply it to an asynchronous path until `flush` returns
a real fence and resource consumers wait for it.

Measure `generic_flush_resolves`, resolve pixel counts and DRM ioctls along
with Q3 output hashes. If Q3's WSI path calls `flush` and immediately exports
the render resource, the resolve simply moves to `get_handle`; that is still
correct but may not reduce the count. If it flushes internally between passes
without exporting, it removes one blocking blit per such call.

## Explicit non-proposals

Do not remove drains on changed texture, blend, DSA, rasterizer or fragment
shader state. Their values are consumed only when `ph_submit_vertices()`
builds the packet, so omitting the drain makes old vertices use new state.
Do not make `ph_flush_resource()` lazy: its API purpose is resource coherence.
Do not remove the two lightmap passes. None of these are redundant flushing
points.

Neither change fixes the dominant 3.7 ms ioctl retirement cost where Q3
actually changes texture/state. They reduce avoidable work while the
per-slot asynchronous queue and real Gallium fences are implemented.
