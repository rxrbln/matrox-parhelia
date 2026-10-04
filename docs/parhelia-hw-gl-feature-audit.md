# Parhelia hardware OpenGL feature implementation audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Reconciled with code on 2026-10-04: shared Mesa **9369ebc**, shared DRM
**f288453**; isolated FAA Mesa **fbf2b5b**, DRM **0f7d564**. The FAA context
also has uncommitted phase-statistics and metadata-observation diagnostics.
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
| Fragment programs and texenv | Data-flow compiler replaces the old fixed-shape matcher. Combiner-expressible NIR, lowering and preshaders cover many more programs; arbitrary fragment ISA/GLSL execution is not established. Unsupported programs remain a coverage gap. |
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

## Active feature and bug TODOs

1. **FAA performance and application control.** At 1024x768, three matched
   runs give median normal 28.3 versus FAA 24.5 FPS; linear decoding then
   gives 24.6 FPS in all three FAA runs. Nine ten-frame analytical cases pass.
   Resolve/metadata lifecycle optimization remains active. A suitable explicit
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
5. **Fragment/compiler and glamor coverage.** Reproduce remaining text/tile,
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
