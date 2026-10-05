# Parhelia hardware OpenGL feature implementation audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Reconciled with code on 2026-10-05: shared Mesa **9369ebc**, shared DRM
**f288453**; isolated Mesa **1f5318b** (deployed isolated prefix SHA65008177...), outer NIR **e1817b2**, actual DRM
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
| S3TC | Original BC1 storage supports 20 measured base shapes covering all 211 opaque nonlightmap Quake3 benchmark candidates. 39,201 raw addresses and 79 added native pixel checks pass; exact app RGBA encoding and bilinear/trilinear repeat filtering are independently verified. Opt-in compressed GL1 reaches 46.2 FPS in the earlier 1024x768 windowed run and 54.1 FPS in a new fullscreen run with performanceDRM12d2626/cache/async scanout; these are distinct configurations, not an isolated speedup comparison. A textured gameplay capture is inspected. Anisotropy2 axis-aligned2:1 minification atLOD1 has vendor/native proof; broader perspective/diagonal footprints remain open. BC2/BC3 allocation/upload and complete mip chains now support six measured 2-D shapes in ad6f680: 64x32, 64x128, 128x32, 128x128, 32x128 and 32x32. All 4,606 captured addresses and 130 native GL checks pass, including alpha, tail updates and mip rebasing; the integrated build also passes. Bilinear LOD1 and trilinear LOD1.5 sampled alpha also pass 24 native checks over 35,840 pixels, including repeat boundaries (25747ef). Other shapes, broader filtering footprints, encoders and layered textures remain open. Native S3TC remains gated by PARHELIA_S3TC. |
| Transfers, copy/blit, render-to-texture and mixed-size FBOs | Functional paths are implemented, with a substantial native acceleration gap: `ph_blit` uses CPU conversion; equal-size colour `ph_copy_texture_box` waits for async work, downloads shadows, stages texels and uploads the destination. The imported-resource type2-to-type1 linear resolve uses GPU BITBLT. Native equal-format level0 colour/depth copies now pass on exact0528/64x32 tiled owned render allocations in57a7867, including subsequentdraw/sample/readback and outside-box preservation. Main39b0e2e additionally samples owned RGBA8 render storage directly on exact0528/64x32 with matching render/sampler pitch, level0 and up to two physical units in native colour shaders. Three changing1024x768 generations are byte-identical to forced-shadow controls (nearest/linear/nearest); nearest is analytically exact, while both linear paths retain the same maximum5-LSB discrepancy. Blend, CPU alpha/lookup, depth, feedback and other formats remain on the copy path; four-unit coverage and additive ONE/ONE native consumers now pass six distinct-source images (max1LSB), including retained-source lifetime. Production1f5318b is deployed (SHA65008177...); complete CPU/state and shader selftests and unchanged Piglit colour-matrix pass. It has no measured performance claim. Scaling, other layouts/chips and subresource coverage remain open. GL2 profiling identified a costly screen-depth readback; the app now omits it when no effect consumes it. |
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
5. **Fragment/compiler and glamor coverage.** Main82dcff9 adds bounded generic identity-domain coordinate linking and componentwise vector reciprocal/division/sqrt; native saturated PASS and clipped PLANE proofs pass with zero pixel mismatches. Quake3 GL2 uniform-selected output-alpha discard is implemented in088b5ee;48native cutoff/depth/blend cases pass with preservedRGBA. OuterNIRe1817b2 prevents split colour-vector positions. Default GL2 startup still requests a real five-sampler LIGHTMAP+DELUXE+NORMAL+SPECULAR variant against four hardware units. With explicit normal/specular/deluxe mapping off and cg_shadows=1, basic GL2 startup now passes on hardware after app commits4ec640b/5007a5f defer unused projected-shadow/postprocess shaders until first use. Full original optional shaders are retained and compile normally when requested. Actual1024x768 basic gameplay is visually verified; exhaustive pixel fidelity and full optional lighting remain open. One basic-profile1024x768 windowed timedemo now completes387frames/20.5sec/18.8FPS on actual0f7d564 with main d00d0b2/app e7321f0, clearing the GL2>15 threshold in that profile. App e7321f0 now skips unused screen-depth copy/HDR-depth conversion while retaining depth prepass, MSAA resolve and active SSAO/sunlight consumers; focused native image/profile checks pass. The earlier pre-depth-copy-fix GL2timedemo exceeded90sec without an FPS result; no numerical gain baseline is inferred. Runtime uniform-bounded colour linkage and full RGBA GPU outputs remain major performance work.  0a47bf5 adds GPU direction lookup ramps for the proven single-unit direction-cube route, preserving VS normalization and clipped fallbacks. Native varying, normalized and clipped pixel checks pass. Current remaining transform gaps include secondary
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

10. **Presentation and submission performance.** Original-family scanout placement, genuine async flips, final framebuffer/controller identity and pixel checks now pass, including generic unreachable FB rejection. Native scanout remains opt-in; actual restored X uses PageFlip=false. Performance kernel977a1e5 and Mesa0fde61f negotiate16384DWORD owned payloads with independent4096DWORD fetch bounds; sync/async dense/sparse pixel checks pass. One fullscreen FAAoff GL1 run32.4FPS reduces submissions80→35/frame. Kernel12d2626 allows opt-in scalar register caching plus adjacent packet packing; host sanitizer contracts, fetch/scanout/rejection and one Q3run pass, omitting177730/191312 eligible state packets across the workflow. Its33.7FPS is not an isolated speedup because app settings differ. Current restored0f7d564 uses the Mesa4096/221 fallback. Fullscreen native-BC1 performance now has a single54.1FPS run at1024x768 with explicitcompression1/nativeS3TC1/aniso0 and Mesa5461138. Cache-default adoption and FAA throughput remain separate checks. Combined-kernel ordinary FAA async textured-depth regression now passes10 frames at1024x768 with228855 interior pixels/frame; packed-FAA stays experimental/off. Preserve completion ordering for CPU access and Present consumers.

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

Compiled GPU lighting input stream5461138 defaults to6DWORD instead of8 within the proved original0528/FAAoff/single-raster-set/RGB scope, retaining all23WARP instructions. Native lighting/clipping/consumed-ST fallback and integrated10frame pixel proof pass. One short1024glmark build51FPS vs earlier33–34 ballpark; no Quake3 gain established. Opt out with PARHELIA_TCL_COMPACT_INPUTS=0.

General affine RGB matrix lowering d00d0b2 collects shared sampled-channel atoms into three native MAD slots. Full selftests, unchanged Piglit colour-matrix, five changing-uniform native cases (327680pixels/zero mismatches/channel error at most2), and four neighbouring dot/swizzle Piglit cases pass. Signed coefficients, offsets, sampled alpha and immutable source textures are covered; unsupported uniform magnitudes remain rejected.

Original inline32 indexed-draw RE checkpoint: fresh Windows writer execution
proves exact8b0a0005/sixDWORD indices and65535+3 index splitting; this is
CPU command construction, not GPU retirement. Candidate kernel a934e0d
(on performance12d2626) adds default-off query36/reloc11 diagnostic with
synchronous kernel-owned copied vertices, at most6indices, strict bounds
and stream/primitive/descriptor cleanup. Its build and78 recognizer
mutation cases pass. Native class3 owned-inline32 retirement/depth interlock now passes three1024x768 scenes: near/far both orders and shared four-vertex rectangle, each zero colour/depth mismatches including subsequent97cleanup draw. Main diagnostic kernelfc378ed remains defaultoff; production97drawing stays unchanged. Repeated native index packets now pass: kernel11fd0fe/fixturee2084e2,24small packets share one4/6vertex stream and match dense144vertex controls in all three1024x768 colour/depth scenes, including97cleanup. Gallium indexed reuse, other fetch layouts and async integration remain open. No native indexed
capability or performance claim follows from this candidate.

### Indexed reuse preparation (2026-10-05)

Main6a2cb92/42d5b2c adds a bounded owned indexed emitter and a raw FLOAT3
XYZ/RGB tuple cache. Cache groups preserve triangle order and bit-exact inputs,
with at most682 owned vertices and942 indices per group. Resource generations,
source indices, offsets, strides and translation keys invalidate the cache;
user/volatile data, unsupported formats and ambiguous same-buffer per-element
strides fall back. The independent CPU oracle passes, including2100-index
multi-group cases and degenerate triangles. This is preparation only: Gallium
selection/submission and VBO pixel validation remain pending, and production
remains39b0e2e. No app performance benefit is claimed yet.

The guarded Gallium candidate now passes a native VBO pipeline proof on the
query36-enabled temporary kernel. Ten1024x768 frames each check662592 pixels
with zero error and hashes identical to the dense control. Eight lighting
frames retain23 GPU instructions, update normal-buffer contents and change
live matrix uniforms, with ordinary draws interleaved; two final ordinary
RGBA frames check cleanup. Logs confirm80 successful owned indexed submissions,
not only selection before potential fallback. The workflow restores actual
0f7d564/X1920. The guarded Gallium implementation is integrated as553ef26; production remains39b0e2e pending the combined build. No performance claim yet.

Main8289aa9 extends proven direct render sampling to all four physical units
and retains extra sampler resources across deferred batches. Six1024x768 image
checks pass over three changing generations, including a pending unit3 partial
producer and deletion of source attachments/views before readback. Maximum
channel error is1 against the analytical weighted sum (tolerance3). Distinct
BO traces confirm all four units consume real render storage. Blend, CPU alpha,
lookup and depth-sampling gates remain; no performance measurement is claimed.

### Programmable FLOAT4 colour and constant-bank legalization

Main dfc53c6 integrates measured FLOAT4 fetch/export and live per-draw range
proof behind default-off PARHELIA_TCL_FLOAT4. Native144-case evidence covers
independent RGBA, all texture-alpha codes, discard, depth/blend and changing
uniforms that select GPU -> conservative fallback -> GPU without relinking.
The actual vendor compiler rejects MAD operands using two distinct constant
vectors; accepted split MUL/ADD and temporary-materialized MAD controls explain
the earlier zero-output failure. Lowering now materializes the second constant
vector. Main6eb2921 explicitly rejects mixed FLOAT4/indexed and packed layouts;
full integrated build, CPU/state and shader selftests pass. Compact RGB and
packed byte layouts are preserved. Combined native integration and two-ST
validation are in progress; production remains39b0e2e. No FPS gain is claimed.

Latest combined deployment:1f5318b/SHA65008177... is installed in the isolated
production prefix. Full merged build and CPU/state+shader suites pass; combined
single-ST FLOAT4 integration passes144 cases. Main1f5318b admits direct render
sampling for exactONE/ONE RGB+alpha ADD consumers, leaving other blends on
fallback. The integrated additive native gate passes six1024x768 images,
maximum channel error1, with boot unchanged. Previous production39b0e2e notes
above describe earlier checkpoints. Two-ST uniform-range fallback still loses
alpha and is under investigation; valid two-ST GPU cases have separate proof.

Two-ST FLOAT4 follow-up:96 valid-range cases pass with a patterned second
texture and independent UV2, confirming the11-DWORD descriptor and both UV
inputs. Uniform-range fallback still loses alpha; its failed test is retained
and complete transition support remains unproven. Native depth sampling also
remains open: a fresh D24S8 TEXTURE|ZBUFFER vendor fixture returns success from
creation and both callbacks but emits no sampler descriptor or draw. Callback
success alone is not evidence of a supported depth-sampler format; D16/D24X8
and private vendor paths are being investigated before hardware probing.

Async owned indexed candidate proof: a separate default-off kernel gate and
GETPARAM37 admit the exact0528 owned stream into fence-retained async slots.
Ten1024x768 frames match the synchronous indexed control with zero analytical
error and identical hashes. Logs prove80 async submissions with successful
returns and nonzero fences, with no synchronous fallback. Client vertex-buffer
reuse, changing uniforms and ordinary draw interleave are covered. Existing
index/state/bounds validation remains; the workflow restores actual0f7d564/X1920.
The candidate is not the loaded kernel, and no FPS improvement is inferred.

Occasional FAA regression on deployed1f5318b/known-good0f7d564: ten textured
occlusion frames at1024x768 pass228855 interior pixel checks per frame, with
20 FAA draw batches and10 resolves. Packed FAA remains disabled. This checks
textured/depth correctness after the combined shader and render-sampling
changes; it is not an FPS or overhead measurement.

2026-10-05 perspective and packed-input checkpoint: main Mesa6ef7404 fixes
compiled viewport export to carry reciprocal clip W in all four position lanes.
Vendor control and independent orthographic/perspective cases pass144/144 each;
96 GPU and48 fallback cases preserve separate UVs, alpha, blend and depth.
Production deployment remains1f5318b until combined native validation.

Packed one-ST indexed candidate Mesa7191534/kernel92a5f8d passes eight1024x768
dense/native exact frame hashes and255792 analytical pixels per frame. Logs show
80 successful async submissions with nonzero fences. The last XY crossing is
inside the supported hardware guard band and remains native; it does not prove
software clipping fallback. A separate Z-crossing/rebind test is pending. The
bounded workflow restored known-good0f7d564 and X1920 without reboot. No FPS gain
is claimed; this path remains experimental and disabled by default.

Native depth storage and reconstruction diagnostics integrated through737240e:
650240 depth pixels prove B=low/G=middle/R=high bytes of surface36 storage when
sampled through colour32. Prescaled three-stage GPU arithmetic resolves high-end
clipping and demonstrates lower-byte contribution.528320 output pixels match
exactly and121920 differ by one8-bit level at rounding boundaries. This does not
prove exact24-bit depth sampling; production CPU depth fallback remains.

Q3 missing-specular-map specialization builds but both full-map and simpler
specular-enabled startup encounter the varying allocator limit before gameplay:
more value varyings than colour interpolators and coordinate units. No gameplay
or performance result is claimed for that app change.

Follow-up packed depth-clipping proof: far-Z frame7 takes two ordinary1408-triangle
software tails and frame8 returns to ten successful native async submissions.
Both images match dense controls exactly (507642 analytical pixels per route).
Main Mesa includes packed implementation/evidence throughbdb93e0; kernel gate
e2aa290 remains default-off, with known-good0f7d564 restored after testing.

One Q3 packed indexed run at1024x768 fullscreen, uncompressed textures, anisotropy
enabled and per-submit tracing:387frames/13.2seconds =29.4FPS.12654 actual async
submissions succeed. This configuration differs from the earlier compressed
54.1FPS profile and does not establish a gain/regression. No repeated benchmark.
Submission aggregation is the next performance target; preserve the feature.

Appf519e7f defers both ordinary and constant-white-specular lighting variants
until actual bind. Full-map gameplay reaches lightmap variant1 (specularMap0),
whose NIR reads four samplers and19 live value components. It exceeds the proven
physical carrier budget; the remaining issue is actual gameplay lighting, not
only eager startup compilation. Diagnostic Mesae0a924c captures rejected NIR.
Full-map GL2 remains incomplete, including additional nonlinear fragment ALU.

Real completion fences, main Mesaf6d41fe/core48cd2d9: reference-counted kernel
sequence handles implement poll/finite/infinite waits. Native swaps retain
frontend throttle=false; explicit native completion requests no longer require
an unconditional client wait. Duplicate device descriptors retain fence access
across originating context destruction. Finish hints/CPU resolves still wait;
sync-file export remains unsupported. No FPS gain is claimed.

Native testing exposed a frontend ordering defect: FenceSync flushed the pipe
while immediate-mode vertices remained buffered. Core48cd2d9 flushes those
vertices and uses st_flush to include cached bitmaps. The independent trace
checker rejects the before log and passes the fixed log: each quad precedes its
fence, three1024x768 frames have786432 exactRGB pixels each, actualzero-timeout
ETIME/finitecompletion and shared-context fence lifetime pass. Full driverCPU
selftests pass. Production Mesa has not yet been updated with these changes.

Mainf5130cb keeps tiny packed draws on normal GPU host aggregation; force-small
coverage option preserves the indexed feature. No repeated Q3 benchmark. Agent
larger-indexed-group candidate hit pre-doorbellE2BIG from the224primaryCALL limit;
packing-aware dispatch/losslesssync-rechunk is pending nativeproof. Depth-shadow
GPU production candidate remains unpromoted: multigeneration raw comparison has
severe boundary mismatches, and a subsequent boundedtest timedout. Health
classification and feedback guards are in progress; this is not a provenhang.

Original0528 raw TEXCOORD native control now validates16384 coordinate pixels
with max0.0029 RGBerror/A1. Existing compiler PASS already uses TAopcode2, so this
is coverage proof rather than a newoptimization. VendoremulatorTEXM3x3_COORD
creation/draw0 and exact emittedTAchain/payload groundtruth is captured inmain
dfd0f23; matrix-addressing native retirement and full multipasslighting remain
open.

## 2026-10-05 combined hardware/performance checkpoint

Matching uncompressed Quake3 GL1 1024x768 profile: one combined run reached
32.0 FPS (387 frames/12.1s), versus earlier 29.4 FPS (387/13.2s).
Successful native submissions fell from 12654 to 8408; total submissions from
68–69 to 58–59 per frame. Source362286a/core48cd2d9; retained evidence in
Mesa4683648. This is a combined-change result, not an isolated attribution.
The >40 FPS goal in this profile and full GL2 lighting remain open.

The earlier depth-shadow severe boundary errors are resolved in the opt-in
Z24X8 path: three generations, eighteen consumer images, exact full source
snapshots. Consumer rounding remains max1 nearest/max2 linear colour levels;
strict pixel equality is not claimed. Shader comparisons retain CPU conversion.
D24S8 follow-up has passed producer and source equality checks; final comparison
proof and integration remain pending. Neither path is promoted by this note.

Original GPU texture-address TEXDP3 arithmetic is now proven using the captured
vendor helper dimensions/filter/wrap/auxiliary state: five basis cases,
zero mismatches across15360 RGB components. Agentf529322 integrated asfaee431.
Spatial interpolation, perspective and matrix chaining remain to be proved;
this foundation does not implement full GL2 material lighting.

Combined source7e2543f/core48cd2d9 passed one ten-frame textured FAA occlusion
check at1024x768:228855 interior pixels/frame,20 draw batches,10 resolves,
depth24/stencil0. Evidence committed inMesad3aa086. Main development stays
FAA off. Production Mesa remains1f5318b and known-good DRM0f7d564; newer
features are tested in isolated prefixes. Multi-binding aggregation is underway.

Follow-up: D24S8 integrated Mesa ea8eae4 after exact three-generation
producer/source and two-filter shadow-comparison proof; bool option now honors
'=0'. Integrated full build passes. Still opt-in, same raw rounding limits.

Spatially varying TEXDP3 equal-W interpolation passed3072 components, but
unequal-W diagnostic wedged the engine; known-good reload then timed out
initializing WARP. Hardware tests held during ATX recovery. Vendor unequal-W
capture changes input FLOAT4 declaration/stride only; no evidence supports an
invented TA perspective bit. Resident WARP export/setup versus custom vendor
VS contract is under investigation; no retry of the failed packet state.

Recovery and diagnostic correction: cold ATX recovery restored known-good0f7d564
and X:1 1920x1200, boot8d9d7a3b-db70-4d7d-9870-db28b50f0feb. Integrated CPU
selftests pass (Mesacf766b7). The failed unequal-W fixture used column-major
matrix element11 in a row-major constant contract, producing out-of-frustum
vertices instead of unequal W. It therefore does not prove valid perspective
TA failure. Corrected emitted-matrix semantic checks precede the next proof.

External depth Piglit rotation (Mesaf841c6d, sourceea8eae4/core48cd2d9,
librarySHA b00e58356d8224942e98d41c82c33d8bb453066a3d7c2a326e37783349af544a):
fbo-readpixels-depth-formats PASS; fbo-depth-sample-compare fails at link on
unsupported non-colour shader output (fragment depth), before sampling.
Fragment-written depth remains a hardware/shader gap; this result does not
contradict the depth-shadow conversion fixture. FAA off, no version override.
