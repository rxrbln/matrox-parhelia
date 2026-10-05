# Parhelia hardware OpenGL feature implementation audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Reconciled with code on 2026-10-05: shared Mesa **9369ebc**, shared DRM
**f288453**; isolated Mesa **25747ef** (main deployed production **1d6ffc0**; ad6f680 verified in a separate prefix), outer NIR **e1817b2**, actual DRM
**0f7d564**. Performance DRM **12d2626** has passed isolated hardware tests
and was restored afterward. It is not the currently loaded kernel.
The latest GPU-gap report records exact source, binary and configuration evidence.
The shared and experimental stacks must not be treated as the same deployment.

This replaces the [September 11 inventory](parhelia-hw-gl-feature-audit-20260911.md).
Its 64-row percentages and line references describe that old snapshot; they
are not current coverage figures. Functional GL support, native acceleration,
and retained hardware proof are separate claims. This inventory is not a
conformance result.


## GPU affine texture link checkpoint: dac942a, 2026-10-04

The isolated gpu-features branch links affine fragment texture coordinates
into private WARP vertex programs (o3/o4). Fragment-consumed lane masks let
packed lighting plus raw coordinates share a generic varying without requiring
unused coordinate lanes to fit the clamped color carrier. Two taps of one
texture are independently composed. Clipped and rejected native draws retain
CPU coordinate/varying fixups; original application NIR remains unchanged.

Hardware proof: ten frames per single/two-tap case at 1024x768, FAA=0,
662592 pixels per frame with zero mismatches, and linked GPU draw markers.
Additional two-tap 1408-triangle grids pass native, clipped (657024 pixels),
forced-software and disabled-GPU-multitexture cases. All compiler selftests
and seven focused Piglit regressions pass. The glmark texture profile shows
one compiled GPU transform draw/frame, with software tails for clipping.

One short matched texture scene scores 219 before / 227 after; this is a
ballpark result, not proof of a stable speedup. One new short regression pass:
Quake3 387 frames / 13.6 seconds / 28.4 FPS; glmark build/shading/bump
62/24/230 FPS (three-second scenes). Earlier measurements used different
lengths and are not a matched comparison. No repeated benchmark campaign.
Future runner defaults to one pass per latest user instruction.

Limits: at most two coordinate units, affine varying expressions, smooth
interpolation, complete single-block VS outputs. Uniform-dependent coordinates are added by 17f8248 (UBO0, constant offsets).
Secondary generic color routing, ramps, nonlinear expressions and wider shader
coverage remain gaps. Native indexed/strided fetch remains unproven. The
small smooth-point coverage candidate stays preserved on faa-textured and is
not deployed. This milestone adds no FAA work.

Deployed binary SHA-256:
`cdf00a996b6509294fb812d539a98db7a9baaf626f861db6e984d3311ddd0e68`.
Prefix `/root/parhelia-mesa-gpu-features`; DRM/boot unchanged.


## Combined uniform and user-plane checkpoint, 2026-10-04

Main GPU feature HEAD57739f6; production implementation54c83b6. Runtime UBO0
fragment uniforms with constant offsets now drive up to two affine GPU
texture coordinates (17f8248). Constants are refreshed per draw; same-program
changing-uniform simulation and hardware oracles pass. Nonlinear expressions,
more texture units and lookup ramps remain gaps.

54c83b6 preserves GPU transforms when eight affine bounding-box corners prove
that every enabled user plane leaves the draw unchanged, with a floating-point
margin. Crossings/outside, separate ClipVertex/ClipDistance, nonaffine positions
and ambiguous boundaries keep software clipping. This is not native clipping.
The independent geometry fixture checks thirty1024x768 frames with zero
mismatches and native plane markers. The combined build additionally passes
full selftests, ten changing-uniform two-tapgrid frames and ten changing-plane
fixed-function frames, allFAA0 with zero mismatches. No repeated benchmarks.

Combined deployment /root/parhelia-mesa-gpu-features, binarySHA256
`04c0f01f7f44b38280ede21b9816f531daf5786642531514d0c10cf72ddd0a57`.
DRM0f7d564/targetboot unchanged; sharedstack unchanged.

Vendor emulator fixtures establish packet-generation ground truth, not actual
GPU retirement: registered system-memory indexed DP2 still emits97080001;
RGB/DXT1/3/5 sampler state differs only in texture format/layout (plus DMA
addresses); signed slope/units offset emits c178 + mode86/d7 and resets97.
Native compressed sampling, indexed fetch and full native offset remain open.
Parallel agents continue these major gaps, serialized by the established
hardwarelock. All routineperformance checks default to one short run.

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
| S3TC | Original BC1 storage supports 20 measured base shapes covering all 211 opaque nonlightmap Quake3 benchmark candidates. 39,201 raw addresses and 79 added native pixel checks pass; exact app RGBA encoding and bilinear/trilinear repeat filtering are independently verified. One opt-in compressed 1024x768 windowed GL1 run reaches 46.2 FPS, with an inspected gameplay capture. Anisotropy2 axis-aligned2:1 minification atLOD1 has vendor/native proof; broader perspective/diagonal footprints remain open. BC2/BC3 allocation/upload and complete mip chains now support six measured 2-D shapes in ad6f680: 64x32, 64x128, 128x32, 128x128, 32x128 and 32x32. All 4,606 captured addresses and 130 native GL checks pass, including alpha, tail updates and mip rebasing; the integrated build also passes. Bilinear LOD1 and trilinear LOD1.5 sampled alpha also pass 24 native checks over 35,840 pixels, including repeat boundaries (25747ef). Other shapes, broader filtering footprints, encoders and layered textures remain open. Native S3TC remains gated by PARHELIA_S3TC. |
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
   Main work remains FAA off; the active goal now requests occasional FAA correctness/performance checks. Current Mesa867b45a predecessor457a116 passes ten textured depth-occlusion frames at1024x768 on actual0f7d564. Combined performance-kernel FAA proof remains pending. A suitable explicit
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
5. **Fragment/compiler and glamor coverage.** Main82dcff9 adds bounded generic identity-domain coordinate linking and componentwise vector reciprocal/division/sqrt; native saturated PASS and clipped PLANE proofs pass with zero pixel mismatches. Quake3 GL2 uniform-selected output-alpha discard is implemented in088b5ee;48native cutoff/depth/blend cases pass with preservedRGBA. OuterNIRe1817b2 prevents split colour-vector positions. Default GL2 startup still requests a real five-sampler LIGHTMAP+DELUXE+NORMAL+SPECULAR variant against four hardware units. With explicit normal/specular/deluxe mapping off and cg_shadows=1, basic GL2 startup now passes on hardware after app commits4ec640b/5007a5f defer unused projected-shadow/postprocess shaders until first use. Full original optional shaders are retained and compile normally when requested. Gameplay pixel fidelity, GL2 FPS and full optional lighting support remain open.  0a47bf5 adds GPU direction lookup ramps for the proven single-unit direction-cube route, preserving VS normalization and clipped fallbacks. Native varying, normalized and clipped pixel checks pass. Current remaining transform gaps include secondary
   generic routing, nonlinear texture coordinates and
   unsupported shader expressions. dac942a now links up to two affine
   texture coordinates into GPU vertex programs; clipped draws retain CPU
   fixups. Single/two-tap pixel and fallback checks pass. c45282b now links a proven
   bounded generic color varying to WARP o1, including normalized matrix
   lighting and GPU saturation. Matrix-lighting pixel checks pass for ten
   frames on native, forced-CPU and clipped paths. Generic1 and unsupported
   varying domains remain gaps; dac942a proves only fragment-consumed color
   lanes so unused raw texture coordinates need no clamped-color bound. 4538ca2 enables the affine bounding-box
   clip gate by default, avoiding repeated CPU position interpretation while
   retaining GPU transforms; native shading improved from 8 to 27 FPS in
   diagnostic runs. The latest 20-second run gives 26 FPS at 1024x768.
   Native shading still submits about three times as many triangles as the
   CPU route; indexed/strided reuse remains a performance priority.
   See ../captures/gpu-gaps-20261004/report.md. Reproduce remaining text/tile,
   FBO readback and unsupported-shader failures against the current compiler;
   historical reports alone do not establish current failures. Expand lowering
   where the combiner contract supports it.
6. **Native acceleration.** Native polygon offset, crossing user clip planes, direct indexed
   and strided fetch, native point-size and topology paths remain work. Conservative all-inside user-plane GPU acceptance is implemented and verified. Packed 8-bit vertex-colour fetch is default for verified FAA-off0528 fixed-functionRGBA/BGRA8 inputs in1d6ffc0.48nativechecks plus12gradient/BGRA/far-clipping and32RGB-above-alpha cases pass; GPU transforms retained. One Q3run31.6FPS reports~18% fewer vertexDWORDs, without an isolated speedup claim. Float/compiled/current-colour paths retain existing layouts; packedFAA remains experimental. Existing ordinaryFAA textured depth smoke passes after promotion. GPU
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
9. **Hardware proof gates.** BC2/BC3 shapes beyond the six verified 2-D chains, compressed filtering beyond the measured bilinear/trilinear cases, complete S3TC filter coverage, GigaColor render/sampling and
   system-memory texture fetch remain unproven. Keep unsupported/gated rather
   than equating decoded vendor structures with working Mesa support.

10. **Presentation and submission performance.** Original-family scanout placement, genuine async flips, final framebuffer/controller identity and pixel checks now pass, including generic unreachable FB rejection. Native scanout remains opt-in; actual restored X uses PageFlip=false. Performance kernel977a1e5 and Mesa0fde61f negotiate16384DWORD owned payloads with independent4096DWORD fetch bounds; sync/async dense/sparse pixel checks pass. One fullscreen FAAoff GL1 run32.4FPS reduces submissions80→35/frame. Kernel12d2626 allows opt-in scalar register caching plus adjacent packet packing; host sanitizer contracts, fetch/scanout/rejection and one Q3run pass, omitting177730/191312 eligible state packets across the workflow. Its33.7FPS is not an isolated speedup because app settings differ. Current restored0f7d564 uses the Mesa4096/221 fallback. Fullscreen native-BC1 performance, cache-default adoption and FAA throughput remain separate checks. Combined-kernel ordinary FAA async textured-depth regression now passes10 frames at1024x768 with228855 interior pixels/frame; packed-FAA stays experimental/off. Preserve completion ordering for CPU access and Present consumers.

The default-off native-offset candidate d6df48e retired safely but failed six of seventeen depth-effect cases on 0528: nonzero slope/units behaved as no offset. Known-good DRM0f7d564 and X were restored. Vendor c178/mode86/d7 captures establish packet generation only; do not enable native GL offset from them. Q3's offset fallback is one draw/eighteen vertices per frame, so major performance work takes precedence.

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
