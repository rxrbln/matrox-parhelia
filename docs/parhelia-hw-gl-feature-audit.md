# Parhelia hardware OpenGL feature implementation audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Reconciled with code on 2026-10-04: shared Mesa **9369ebc**, shared DRM
**f288453**; isolated Mesa **4aa7fa7**, DRM **0f7d564**. Diagnostics **8719c74**
include phase statistics, overlapping transform-fixup causes and default-off
metadata observation (no observation-mode hardware result yet).
The shared and experimental stacks must not be treated as the same deployment.

This replaces the [September 11 inventory](parhelia-hw-gl-feature-audit-20260911.md).
Its 64-row percentages and line references describe that old snapshot; they
are not current coverage figures. Functional GL support, native acceleration,
and retained hardware proof are separate claims. This inventory is not a
conformance result.

## Implemented paths and remaining limits

| Feature family | Current code and limits |
|---|---|
| Triangle rasterization, culling, scissor, polygon modes | Implemented. Nontriangle and unfilled modes may use draw-module decomposition. |
| Points, lines, smoothing and stipple | Native unit primitives plus triangle expansion for wider/textured cases. Point-coordinate and polygon-stipple lowering now exist; native hardware point-size and strip/loop acceleration remain work. |
| Matrix transform, perspective and viewport | GPU WARP path exists and is preferred; unsupported programs and clipping can fall back to Mesa draw. They are no longer missing features. |
| Programmable vertex shaders and lighting | WARP compiler, program upload/cache and constant loading exist; generated lighting can run there. Full vendor 12-light-store coverage is not established. |
| Fragment programs and texenv | Data-flow compiler replaces the old fixed-shape matcher. Combiner-expressible NIR, lowering and preshaders cover many more programs; arbitrary fragment ISA/GLSL execution is not established. Unsupported programs remain a coverage gap. 54d121b adds scalar/vec2/vec3 stores, verified with executable WARP simulation. |
| Secondary color, fog and texgen | Secondary color is packed; shader lowering can cover fog/texgen expressions. Dedicated vendor fixed-function paths and complete mode coverage remain unproven. |
| Color/depth/stencil, clears, blending and logic operations | Implemented, including masked/scissored clears, separate blend equations and logic-op lowering. Complete stencil/two-sided-operation coverage still needs targeted hardware validation. |
| Texture dimensionality and size | 1-D, 2-D, cube and volume resource/view paths exist; cube/volume have measured default paths on P690 and 0528. Maximum 2-D extent is 2048. |
| Mipmaps, filtering and wrap | Mip chains, trilinear filtering, border/mirrored wrapping and LOD state exist. The original-family LOD-bias issue below remains. |
| Multitexture | Original family exposes up to four hardware units; LX is limited to two. Coverage depends on compiler lowering and chip capabilities. |
| Narrow texture formats | Chip-dependent format routes exist; format support must be read from ph_is_format_supported, not the old blanket missing-format list. |
| S3TC | Upload/sampling code exists behind PARHELIA_S3TC; successful native hardware retirement remains unproven. Keep gated. |
| Transfers, copy/blit, render-to-texture and mixed-size FBOs | Implemented. Broader format/subresource and shader workloads still need coverage tests. |
| VRAM eviction and restoration | parhelia_ttm_move is installed; aperture resource/page copies and eviction/restore counters exist. The old patch-only TODO is superseded. |
| Cursor placement/upload | Cursor uses parhelia_vram_bar_addr and original-family aperture writes. The old direct-flat-BAR claim is superseded; broader placement/display stress remains validation work. |
| FAA-16x | Implemented and hardware-tested on isolated original-family branch, default off. Textures, ordinary depth/write modes, private depth and stencil preservation work in tested cases. Not merged into shared Mesa/DRM. See below. |

Source entry points: Mesa parhelia_screen.c (caps/formats),
parhelia_context.c (compiler, draw, state and FAA), parhelia_batch.c and
reference/p3d_state.c (packets); DRM parhelia_drv.c, parhelia_gem.c,
parhelia_cursor.c and parhelia_submit.c. Commit references above pin this audit
without relying on obsolete source line numbers.

Current user priority: implement/test hardware GPU features and optimize
Quake 3/glmark2 with FAA **off** until further notice. Piglit must also use
FAA off. Occasional standalone FAA smoke checks are allowed. FAA
implementation/performance/control work below is deferred. Prioritize major
hardware gaps and performance; defer small conformance/pixel details. Retain
correct features, cleanups and optimizations even when benchmark FPS is equal.

## Latest FAA-off hardware checkpoint

11ded06 fixes fast depth-clear state surviving context destruction and owns
its depth resource across framebuffer unbind. On the original 0528 board,
three old-build context-owner/scissored-clear pairs all failed the second
test; all three fixed-build pairs passed. Seven focused shader, lighting,
texgen and depth-clear Piglit tests then passed, with PARHELIA_FAA=0.
Concurrent-context cache ownership still needs separate coverage. The isolated
stack remains deployed; this does not constitute a shared-stack merge.

4aa7fa7 accounts for the existing two-triangle packets when limiting batch
size. Points and lines keep one primitive per packet; staging, vertex bounds
and the hardware packet shape are unchanged. At 1024x768 with FAA off, a
20-second shading comparison reduces draw batches from 92 to 64 and submits
from 92 to 65 per frame, with 14348 triangles and 258264 vertex dwords in both.
FPS is 26 versus 27. Three repeats of the four selected five-second scenes
give shading medians 25 versus 26; Quake 3 medians are 28.5 versus 28.3 FPS,
so no Quake 3 speedup is claimed. A 1408-triangle analytic mesh passes ten
frames on native, CPU-control and clipped paths. Large point/line checks also
pass. Native indexed reuse remains a separate gap.

A smooth-point coverage discrepancy occurs on both old and new builds. The
zero-coverage discard candidate is retained but incomplete: it currently
refuses the coverage shader and takes the aliased-disc fallback. It is not in
the deployed 4aa7fa7 checkpoint. The new fractional-alpha fixture must request
an alpha framebuffer; that refined check has not run. This pixel-detail work
is deferred under the user's current priority.

The occasional 1024x768 FAA off/on occlusion smoke passes ten frames per arm
(228855 interior pixels/frame); the on arm confirms 20 FAA batches and ten
resolves. Benchmarks and Piglit remain FAA off.

## Feature and bug TODOs

1. **Deferred: FAA performance and application control.** At 1024x768, three matched
   runs give median normal 28.3 versus FAA 24.5 FPS; linear decoding then
   gives 24.6 FPS in all three FAA runs. Nine ten-frame analytical cases pass.
   Resolve/metadata lifecycle optimization is deferred until the user resumes FAA work. A suitable explicit
   GL control is requested but not implemented; current control is
   PARHELIA_FAA=1. Sample buffers/masks and full MSAA semantics are not exposed.
   Alpha test, application stencil, partial color masks, logic ops, other
   target formats and special multipass/primitive paths can hand over to
   ordinary rendering. Full application-wide FAA coverage is not established.
   See [FAA RE](parhelia-faa-original-re.md), sections 14.6–14.7, and
   [retained performance results](../captures/faa-perf-20261004/report.md).
2. **P690 sampled alpha.** Native companion route stays opt-in because spatial
   failures remain. Production micro/two-pass/CPU routes work but cost time.
   See parhelia-p690-q3-sampled-alpha-final-audit.md.
3. **Original-family LOD bias.** Bias encoding exists, but magnified raw LOD
   clamps at -4 and the GL LOD-bias test still fails. This is a correctness gap,
   not a missing LOD-field implementation (parhelia_screen.c comments).
4. **P690 larger primitive packets.** More than the safe two-primitive default
   can wedge. Do not turn packet-size experiments into a default merely from
   vendor similarity. See parhelia-p690-multi-primitive-packet-hang.md.
5. **Fragment/compiler and glamor coverage.** Current glmark2 evidence
   identifies texture-tap/varying-route fixups, bump lookup ramps, and shading
   generic-output routing as GPU-transform gaps. c45282b now links a proven
   bounded generic color varying to WARP o1, including normalized matrix
   lighting and GPU saturation. Matrix-lighting pixel checks pass for ten
   frames on native, forced-CPU and clipped paths. Generic1 and unbounded
   varying domains remain gaps. 4538ca2 enables the affine bounding-box
   clip gate by default, avoiding repeated CPU position interpretation while
   retaining GPU transforms; native shading improved from 8 to 27 FPS in
   diagnostic runs. The latest 20-second run gives 26 FPS at 1024x768.
   Native shading still submits about three times as many triangles as the
   CPU route; indexed/strided reuse remains a performance priority.
   See ../captures/gpu-gaps-20261004/report.md. Reproduce remaining text/tile,
   FBO readback and unsupported-shader failures against the current compiler;
   historical reports alone do not establish current failures. Expand lowering
   where the combiner contract supports it.
6. **Native acceleration.** Polygon offset, user clip planes, direct indexed
   and strided fetch, native point-size and topology paths remain work. GPU
   transforms already exist. Prefer GPU paths; retain correct improvements
   even if one benchmark shows no immediate gain. See
   parhelia-polygon-offset-hardware-re.md and
   parhelia-p690-native-indexed-draw-feasibility.md.
7. **Kernel geometry validation.** Audit fill/blit destination extents against
   relocated BO bounds beyond register/relocation validation. The historical
   claim of an exploitable out-of-bounds write needs a current reproducer;
   do not treat that claim as fresh test evidence.
8. **Recovery and display coverage.** No validated general engine-reset path;
   continue isolated bounded tests. Secondary-head/multimonitor and cursor
   placement stress need current hardware proof.
9. **Hardware proof gates.** Native S3TC, GigaColor render/sampling and
   system-memory texture fetch remain unproven. Keep unsupported/gated rather
   than equating decoded vendor structures with working Mesa support.

## Historical reports requiring fresh reproduction

The old NPOT/odd-height bilinear edge discrepancy, scanout LUT-bypass
candidate, 0528 tile-layout hypothesis, and glamor readback reports remain
historical leads. Current mip/layout/display changes mean they are neither
confirmed current bugs nor proven fixed by this audit. Preserve their original
notes and rerun a focused test before assigning status.

## Evidence boundaries

Occlusion queries have no driver callbacks and no recovered usable hardware
counter contract; the negative vendor audit is not an implementation recipe
(parhelia-occlusion-query-re.md). Skinning/palette and displacement remain
vendor capability/RE leads, not ready implementation tasks. Native sRGB decode
and framebuffer encode are not established hardware features. No percentage
completion score is assigned to these unequal and partially validated families.
