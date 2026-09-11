# Parhelia hardware OpenGL feature implementation audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is the implementation checklist for the Mesa Parhelia driver as inspected
on 2026-09-11.  It reconciles every hardware-facing OpenGL feature described by
the documents in this directory with the driver in
`../src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/parhelia`.
The three principal source files had SHA-256 prefixes `644bd1a1` (context),
`07e17ddc` (screen), and `4a5ece5b` (state emitter).  Later changes must re-audit
the affected rows rather than assuming this snapshot still applies.  This
snapshot includes Mesa commits `e4a6857` (unit lines), `83c89f6` (2x
anisotropy), `78a7bd5` (stencil, scissor, and blend state), and `eb7c52e`
(batched consecutive lines).

Status has a deliberately strict meaning:

* **I+T**: Mesa reaches the hardware implementation by default and a retained
  open-driver hardware result exercises the feature.
* **I+G**: useful code exists, but it is unreachable, opt-in, restricted to one
  workload, or lacks a retained hardware result.
* **M**: the documented hardware path is absent.  A Mesa software transform or
  primitive decomposition does not count as hardware implementation.
* **C**: current Mesa advertises or accepts more than the emitter implements, or
  implements a materially narrower/different contract.  This is a correctness
  contradiction, not merely an opportunity for acceleration.

Vendor captures prove silicon and packet behavior; they do not by themselves
make an **I+T** Mesa row.  The historical overview is also not authoritative
where newer documents correct it: the correction table explicitly retracts old
scissor, viewport, point-size and polygon-offset claims
(`parhelia-raster-clip.md:967-984`), and the state-slot namespace correction
invalidates register addresses derived directly from region-2 indices
(`parhelia-gl-feature-map.md:30-52`).

## Geometry and rasterization

| # | hardware GL feature | status | current Mesa evidence and remaining contract | documentation evidence |
|---:|---|:---:|---|---|
| 1 | triangle lists, Gouraud interpolation, tiled color output | **I+T** | `parhelia_context.c:3247-3487` packs transformed triangles and `:3028-3034` submits the hardware batch. | An open-driver stream and WARP program match the vendor result (`parhelia-3d-integrated.md:383-405`); Gouraud and full-scene depth coverage are hardware checked (`parhelia-gears-test.md:34-58`). |
| 2 | flat shading | **I+G** | `parhelia_context.c:2761-2763` forwards `flatshade`; `reference/p3d_state.c:401-404` emits the flat bit.  No retained current-Mesa pixel test isolates flat provoking-color behavior. | Hardware state is documented at `parhelia-gl-feature-map.md:149-167`; quad decomposition has provoking-vertex constraints (`parhelia-vertex-submission.md:239-269`). |
| 3 | native points, point size, point parameters/sprites | **M** | The point callback still discards every point (`parhelia_context.c:3489-3490`). | Native point state and sizes are real hardware (`parhelia-raster-clip.md:893-909`); the vendor point draw is one vertex (`parhelia-engine-context-bringup.md:5610-5632`). |
| 4 | native lines, line strip and line loop | **I+G** | Native untextured, aliased, unit-width `GL_LINES` now emits the vendor primitive with two transformed vertices (`parhelia_context.c:3492-3618`).  The exact class, primitive word and relocations have an offline self-test (`parhelia_drm_test.c:1098-1146`); textured/smooth/wide lines, strips/loops and a retained open-driver live result remain absent. | Native line modes and the loop closing segment are decoded at `parhelia-vertex-submission.md:191-203`; open hardware rendered lines and strips (`parhelia-3d-integrated.md:339-353`). |
| 5 | triangle strips, fans, polygons, quads and quad strips | **I+G** | Mesa draw decomposes these to triangles before `ph_stage_tri`; the hardware emitter contains topology codes (`reference/p3d_state.c:19-39`) but current Mesa always sets `P3D_TRIANGLES` (`parhelia_context.c:2756-2760`). | All modes are hardware-observed (`parhelia-3d-integrated.md:339-350`); exact native/decomposition rules are at `parhelia-vertex-submission.md:175-209`. |
| 6 | hardware indexed draws | **I+G** | Gallium maps indices into software draw and expands them (`parhelia_context.c:3651-3696`).  The direct P690 ABI remains disabled and needs a guarded fast path. | Four native packet forms and eight fetch streams are proven from three vendor codebases (`parhelia-indexed-draws.md:15-35`); current-ABI limits are recorded at `parhelia-p690-native-indexed-draw-feasibility.md:8-12,68-76,121-176`. |
| 7 | direct strided/interleaved vertex-array fetch | **I+G** | A packed hardware vertex ring exists, but arbitrary Gallium streams are first mapped through software draw (`parhelia_context.c:3664-3691`). | Hardware supports eight base/stride streams (`parhelia-indexed-draws.md:17-25,499-516`). |
| 8 | face culling and `glFrontFace` | **M** | Rasterizer state is given to software draw (`parhelia_context.c:3727-3738`); `p3d_state.c` has a cull field but `ph_submit_vertices` never assigns it. | The complete `NONE/CW/CCW` hardware table is confirmed at `parhelia-raster-clip.md:693-745`. |
| 9 | front/back polygon mode | **M** | The emitter has no front/back fill-state plumbing.  Point mode is discarded, while line mode can only fall into the restricted unit-width untextured path from row 4. | Independent face registers and enums are decoded at `parhelia-raster-clip.md:758-785`. |
| 10 | polygon offset fill/line/point | **M** | No depth-bias fields reach `p3d_state`; any draw-module adjustment is software. | Five hardware state values and raw factor/units behavior are established at `parhelia-raster-clip.md:791-847`. |
| 11 | scissor | **I+G** | Mesa retains the Gallium rectangle, clamps it to the target, and emits hardware min/max corners (`parhelia_context.c:997-1003,4129-4138`; `reference/p3d_state.c:406-416`).  Exact register values have an offline self-test (`parhelia_drm_test.c:1150-1191`), but no retained open-driver live result exercises this new path. | Hardware corners and DWGCTL enable are explicit at `parhelia-raster-clip.md:988-995`; a direct standalone test clipped 15,140 pixels to 11,909 (`parhelia-gl-features.md:174-182`). |
| 12 | hardware viewport and depth-range transform | **M** | Viewport and clip state go only to Mesa draw (`parhelia_context.c:4113-4128`); submitted vertices are already transformed. | Hardware consumes a viewport rectangle and z scale/bias (`parhelia-raster-clip.md:248-352,996-1001`). |
| 13 | guard-band/frustum clipping | **I+G** | The emitter writes one fixed `0x12f4=32.0` (`reference/p3d_state.c:603-608`); capability-dependent 32/16 behavior, stage count, and CPU fallback are not implemented. | The hardware rules and required selection are summarized at `parhelia-raster-clip.md:380-480,1002-1004`. |
| 14 | six user clip planes | **M** | Clip planes are consumed by Mesa draw (`parhelia_context.c:4127-4128`), not uploaded to WARP/setup state. | Dedicated plane records and the six-plane hardware path are confirmed at `parhelia-raster-clip.md:483-550`. |
| 15 | wide lines | **M** | The native line callback explicitly rejects widths of 1.5 or greater (`parhelia_context.c:3520-3525`). | The vendor CPU expansion is fully decoded (`parhelia-raster-clip.md:911-963`); only unit-width lines are native. |
| 16 | point, line and polygon smoothing/stipple | **M** | Sample mask and polygon stipple are no-ops (`parhelia_context.c:4159-4163,4959-4960`); the new line path explicitly excludes smoothing. | Point smoothing has real registers, line smoothing uses geometry, and polygon smoothing is ignored by the vendor (`parhelia-antialiasing.md:684-703,729-739`). |

## Transform, lighting and shaders

| # | hardware GL feature | status | current Mesa evidence and remaining contract | documentation evidence |
|---:|---|:---:|---|---|
| 17 | fixed-function matrix transform and perspective divide in WARP | **M** | Vertex shaders execute in Mesa draw (`parhelia_context.c:3837-3848`); normal submits select canned pass-through/texture WARP entries and never set `state.transform`. | Open hardware executed MVP, perspective divide and viewport in WARP (`parhelia-gears-test.md:18-32`). |
| 18 | fixed-function lighting (up to 12 lights) | **M** | No light/material state is lowered into `p3d_state`; Mesa draw executes the generated GL vertex shader. | The hardware 12-light store is confirmed (`parhelia-vertex-pipeline.md:603-627`), and one-light WARP output has pixel-level tests (`parhelia-gears-test.md:18-58`). |
| 19 | hardware fog: vertex/table/range/W/Z and fog coordinate | **M** | No fog state appears in `ph_submit_vertices`; fragment-shader inspection only classifies texture/color use. | Hardware fog caps and the fog-coordinate source switch are documented at `parhelia-gl-limits.md:599,802`; fog is graded hardware in `parhelia-3d-coverage.md:63-64`. |
| 20 | texgen, reflection/normal map generation | **M** | No texgen lowering or WARP variant selection exists. | The hardware texgen emitter and reflection/normal modes are decoded at `parhelia-gl-limits.md:803`. |
| 21 | secondary/specular color and separate specular color | **M** | `ph_stage_tri` retrieves only `COLOR[0]` (`parhelia_context.c:3251-3253`); no second color is packed. | One vertex slot serves secondary color/specular/fog and hardware color sum (`parhelia-gl-limits.md:790,825-847`). |
| 22 | programmable WARP vertex shader / `GL_EXT_vertex_shader` class | **M** | Mesa accepts NIR VS solely because software draw executes it (`parhelia_context.c:3837-3848`).  No user program is compiled or uploaded to WARP. | The Linux ICD contains a complete but fenced-off implementation (`parhelia-feature-exposure.md:37-64`); the hardware/driver exposes vs_1_1-class limits (`parhelia-gl-limits.md:591-593,667-761`). |
| 23 | arbitrary fragment combiner / GLSL 1.20 | **C** | Screen caps claim GLSL 1.20 (`parhelia_screen.c:922-924`), but `ph_create_fs` merely pattern-matches a few fixed NIR shapes (`parhelia_context.c:3850-4049`) and never compiles arbitrary ALU to the fragment ISA. | The complete ps_1.3-class ISA and MTX compiler route are documented at `parhelia-gl-limits.md:593-594,792`; `parhelia-pixel-pipeline-complete.md:57-236` gives the instruction encoding. |
| 24 | fixed-function fragment `REPLACE` and `MODULATE` | **I+T** | Exact NIR forms select the emitter programs (`parhelia_context.c:3935-3984,4027-4030`); texture and replace WARP entries are kernel-gated at context creation (`:4845-4882`). | Both programs and register differences are captured (`parhelia-multitexture.md:447-532`); the current texture path is hardware-proven (`parhelia-engine-context-bringup.md:5810-5833`). |
| 25 | fixed-function fragment `DECAL` | **I+G** | The emitter has DECAL and exact NIR FLRP detection (`reference/p3d_state.c:358-364`; `parhelia_context.c:3935-3984`), but no retained current-Mesa target result isolates it. | The packed DECAL sequence is decoded at `parhelia-multitexture.md:447-532,567-599`. |
| 26 | fixed-function fragment `ADD`, `BLEND`, COMBINE and DOT3 | **C** | The emitter encodes ADD/BLEND (`reference/p3d_state.c:320-363`), but single-unit NIR recognition does not select ADD/BLEND or upload env color; COMBINE/DOT3 are absent.  The advertised fixed-function contract is therefore broader than lowering. | All six texenv families and their hardware programs are documented (`parhelia-multitexture.md:582-588`; `parhelia-gl-limits.md:775-777`). |

## Fragment back end and render targets

| # | hardware GL feature | status | current Mesa evidence and remaining contract | documentation evidence |
|---:|---|:---:|---|---|
| 27 | ARGB/XRGB 32-bit tiled render target and render-to-texture | **I+T** | Format support, native/share resources and color binding are present (`parhelia_screen.c:145-174,317-539`; `parhelia_context.c:2753-2760`). | Native target tiling is hardware solved (`parhelia-3d-integrated.md:357-381`); owned render targets pass the integrated path (`parhelia-own-render-target.md:1-80`). |
| 28 | Z24 depth attachment, clear, compare functions and write mask | **I+T** | Z24/S8 formats are accepted (`parhelia_screen.c:162-165`); compare/write state is emitted (`parhelia_context.c:963-975,2863-2871`; `reference/p3d_state.c:430-455`). | Open-driver depth is measured (`parhelia-3d-state-complete.md:177-232`) and full-scene coverage agrees within 0.008% (`parhelia-gears-test.md:34-58`). |
| 29 | stencil clear | **I+G** | `ph_clear` packs depth and stencil together (`parhelia_context.c:4242-4262`), but the callback ignores stencil write masks and no retained open-driver result isolates stencil clear. | S8Z24 is the established depth/stencil format (`parhelia-gigacolor-faa-hardware-status.md:45-48`). |
| 30 | stencil test, compare, ref/read/write masks, ops and wrap | **I+G** | The front-face Gallium stencil state and dynamic reference are lowered into compare, three operations, read/write masks and the shared S8Z24 packet (`parhelia_context.c:928-975,3716-3726,4149-4157`; `reference/p3d_state.c:430-455`).  The exact register words pass an offline self-test (`parhelia_drm_test.c:1150-1191`); two-sided stencil and a retained live result remain absent. | The contiguous hardware state and wrap operations are decoded (`parhelia-pixel-pipeline-complete.md:392-528`; `parhelia-gl-limits.md:581-582,789`). |
| 31 | alpha test from vertex alpha | **I+T** | All compare functions and 8-bit reference are translated (`parhelia_context.c:883-891,958-962`) and emitted (`reference/p3d_state.c:418-428`). | The P690 vertex-alpha cutout controls color and depth correctly (`parhelia-p690-rgb-companion-alpha-cutout.md:154-164`). |
| 32 | P690 sampled-texture alpha test and source-alpha blend | **I+G** | A unit-1 RGB companion route exists but requires `PARHELIA_NATIVE_SAMPLED_ALPHA=cutout|blend|all` (`parhelia_context.c:1356-1432,4841-4868`).  It is deliberately excluded from default production selection after live spatial/VRAM failures. | The ordinary one-unit route is explicitly unproven (`parhelia-p690-q3-sampled-alpha-final-audit.md:6-16,64-99`); the companion probe proves only the uniform sample and records the missing spatial proof (`parhelia-p690-native-alpha-live-failure.md:62-110`). |
| 33 | ADD blending with ordinary factors | **I+T** | Factor mapping and enable are emitted (`parhelia_context.c:893-914,978-991`; `reference/p3d_state.c:75-92`). | Depth/vertex-alpha source-over and additive forms pass hardware probes (`parhelia-q3-ui-additive-blend.md:37-73`). |
| 34 | independent RGB/alpha blend factors | **I+G** | Mesa lowers the four Gallium factors into the hardware RGB and alpha fields (`parhelia_context.c:978-991`; `reference/p3d_state.c:75-92`).  The complete emitter self-test passes offline, but no retained live result isolates unequal RGB/alpha factors. | Four independent hardware factor slots are confirmed (`parhelia-gl-limits.md:602,785`). |
| 35 | SUBTRACT, REVERSE_SUBTRACT, MIN and MAX blend equations | **I+G** | Gallium equations now reach the shared hardware equation field; MIN/MAX select the required canonical factors (`parhelia_context.c:916-926,978-991`; `reference/p3d_state.c:75-92`).  Exact ADD/SUBTRACT/MIN words pass offline tests (`parhelia_drm_test.c:855-882`), but the new equations lack retained live results. | Hardware equations are confirmed from the vendor emitter (`parhelia-gl-limits.md:786`; `parhelia-fragment-ops.md:729-738`). |
| 36 | constant blend color/factors | **I+G** | All constant factor cases, dynamic blend-color state, and the two packed 10-bit register values are connected (`parhelia_context.c:893-949,978-991,4139-4147`; `reference/p3d_state.c:418-428`).  Exact words pass offline tests (`parhelia_drm_test.c:855-882,1150-1191`), but no retained live result exercises them. | The hardware opcode/register route is confirmed (`parhelia-gl-limits.md:601,787`; `parhelia-blend-alpha-encoding.md:6-12`). |
| 37 | logic operations | **M** | No logic-op state or ROP translation exists. | The hardware state-slot route and 16 operations are documented at `parhelia-gl-limits.md:788`. |
| 38 | per-channel color write mask | **I+T** | Gallium masks become four emitter fields (`parhelia_context.c:992-995`; `reference/p3d_state.c:594-597`). | The byte mask is capture-confirmed (`parhelia-3d-coverage.md:58`; `parhelia-pixel-pipeline-complete.md:530-551`). |
| 39 | dithering | **I+G** | Gallium dither reaches the blend word (`parhelia_context.c:2762`; `reference/p3d_state.c:75-92`), but no retained current-Mesa visual test isolates on/off behavior or format gates. | Dither is part of the decoded BLENDCTL contract (`parhelia-blend-alpha-encoding.md:6-12`). |
| 40 | masked/scissored color, depth and stencil clears | **C** | `ph_clear` ignores `color_mask`, `stencil_mask`, and the supplied scissor (`parhelia_context.c:4242-4251`) and fills the whole selected surface. | The vendor clear record carries mask and rectangle (`parhelia-gl-feature-map.md:136-147`). |
| 41 | GigaColor X2R10G10B10 render target/scanout | **M** | Format support exposes no tested 10-bit target and the draw emitter hard-codes color format `0x32` (`parhelia_context.c:2756-2760`). | `0x34` X2R10G10B10 is binary-proven but still needs a P690 3-D target test (`parhelia-gigacolor-faa-hardware-status.md:19-62`). |

## Textures and sampling

| # | hardware GL feature | status | current Mesa evidence and remaining contract | documentation evidence |
|---:|---|:---:|---|---|
| 42 | base-level 2-D RGB/XRGB/RGBA texture upload and sampling | **I+T** | 2-D resources, tiled upload, descriptors, nearest/linear filters and texture packets are implemented (`parhelia_screen.c:317-539`; `parhelia_context.c:2623-2735,2787-2820`). | Layout-2 textured triangles are hardware-proven; P690 sampled-alpha is the separate exception (`parhelia-engine-context-bringup.md:5262-5358`; row 32). |
| 43 | one-dimensional textures | **M** | Format support accepts only BUFFER/2D/RECT and sampler views require `PIPE_TEXTURE_2D` (`parhelia_screen.c:145-174`; `parhelia_context.c:3764-3784`). | The vendor sampler supports 1-D as part of its target priority (`parhelia-gl-limits.md:778-779`). |
| 44 | three-dimensional/volume textures | **M** | Resource, format and view paths reject 3-D targets. | A real 3-D path and W wrap field are documented (`parhelia-gl-limits.md:574,591,779`). |
| 45 | cube maps | **M** | Resource, format and view paths reject cube targets. | All six faces use the hardware sampler path (`parhelia-gl-limits.md:591,778`). |
| 46 | rectangle and non-power-of-two 2-D textures | **I+G** | The screen advertises NPOT and format queries accept RECT (`parhelia_screen.c:148-174,917-925`); raw dimensions reach descriptors.  Retained tests prove several non-square layouts, but no broad current-Mesa NPOT conformance result covers wrap/filter/mipmap combinations. | Hardware NPOT is the principal vendor under-declaration (`parhelia-gl-limits.md:587-592`); sampling remained hardware-test-gated in the detailed audit (`parhelia-texture-sampling.md:878-945`). |
| 47 | 2048x2048 maximum 2-D texture | **C** | Mesa caps the size at 1024 (`parhelia_screen.c:915-924`) while the documented hardware/vendor limit is 2048. | `parhelia-gl-limits.md:569-579`; `parhelia-multitexture.md:569-588`. |
| 48 | legacy uncompressed texture formats and RGB5_A1 | **I+G** | Generic 32-bit formats are converted; B5G5R5A1 has a dedicated sampler-only path (`parhelia_screen.c:149-172`; `parhelia_context.c:2789-2799,4552-4623`).  The P690 RGB5_A1 sampled-alpha production route has not passed. | The exact A1 route failed the direct test and remains gated (`parhelia-p690-q3-rgb5a1-alpha-route.md:9-27,181-199`). |
| 49 | S3TC/DXT1/DXT3/DXT5 compressed textures | **M** | Block-compressed formats fail the generic four-byte-per-texel format test (`parhelia_screen.c:166-172`); there is no compressed upload/layout path. | Native compressed sampling and formats are established at `parhelia-gl-limits.md:579,780`. |
| 50 | nearest and bilinear min/mag filtering | **I+T** | Gallium nearest/linear map directly into the sampler descriptor (`parhelia_sampler.h:9-18`; `parhelia_context.c:2800-2805`; `reference/p3d_state.c:177-185,665-669`). | Codes are hardware-proven (`parhelia-texture-sampling.md:207-315`). |
| 51 | mipmap chains, nearest-mip and trilinear filtering | **C** | Mesa accepts mipmapped resources/views but `ph_use_mips` always returns false (`parhelia_context.c:1317-1328,3763-3784`); every draw samples level zero. | The chain, tail and filters are decoded (`parhelia-texture-sampling.md:659-867`), but the exact P690 packet stopped retiring and is intentionally disabled (`parhelia-p690-mipmap-probe.md:11-68`). |
| 52 | anisotropic filtering up to 2x | **I+G** | Gallium advertises a maximum of 2 and maps a request above 1 to filter code 3 on both texture units (`parhelia_screen.c:925-926`; `parhelia_sampler.h:9-18`; `parhelia_context.c:2800-2805,2843-2848`).  The exact `0x01c0105b` filter word has a retained self-test (`parhelia_drm_test.c:1510-1520`), but the open-driver path has no retained live-hardware result. | Both saved vendor stacks clamp/report 2, and the code-3 sampler encoding is established at `parhelia-texture-sampling.md:292-320`; `parhelia-gl-limits.md:575,781`. |
| 53 | repeat and clamp-to-edge | **I+T** | Repeat is distinct; every other Gallium wrap currently maps to clamp (`parhelia_context.c:2808-2811`). | Repeat/clamp encodings are hardware-proven (`parhelia-texture-sampling.md:361-430`). |
| 54 | clamp-to-border, mirrored repeat and independent R wrap | **C** | These modes collapse to ordinary clamp, no border color is uploaded, and only S/T are represented (`parhelia_context.c:2808-2811`; `reference/parhelia3d.h:209-220`). | Border addressing/color and the third wrap field are documented (`parhelia-gl-limits.md:782`; `parhelia-texture-sampling.md:361-443,943-990`). |
| 55 | texture LOD bias, MIN/MAX_LOD and BASE/MAX_LEVEL | **M** | No Gallium sampler/view values reach the descriptor beyond hard-wired base zero and disabled mip selection. | The hardware fields and the vendor unit>0 LOD-bias bug are documented at `parhelia-gl-limits.md:783-784`; `parhelia-texture-sampling.md:320-359,430-443,794-867`. |
| 56 | two-texture diffuse×lightmap MODULATE | **I+T** | Two units are exposed; P690 exact MODULATE uses the live-tested one-pass path and other supported shapes serialize (`parhelia_screen.c:947-962`; `parhelia_context.c:2916-3015,4900-4908`). | The physical-pipe-2 route and Q3 image comparison are hardware-proven (`parhelia-multitexture.md:637-690`). |
| 57 | four simultaneous texture units | **C** | Mesa advertises only two samplers/views (`parhelia_screen.c:947-960`) and lowering recognizes only samplers 0/1. | Four independent units are proven (`parhelia-multitexture.md:30-49,567-588`; `parhelia-gl-limits.md:576,774`). |
| 58 | partial texture updates / `glTexSubImage` | **I+G** | Staged maps preserve and upload subresources, but the P690 texture-upload ABI still copies the allocation and no retained target test covers all offsets/mip levels (`parhelia_context.c:4633-4802`). | The vendor rectangle update record and Mesa comparison are documented at `parhelia-texture-updates-small-ui.md:1-85`. |
| 59 | copy/blit and render-target sampling | **I+T** | Hardware batch blits, resolve/upload, `resource_copy_region` and `blit` are installed (`parhelia_screen.c:184-316`; `parhelia_context.c:4330-4493,4921-4923`). | Native copy/present and owned-target paths are retained in `parhelia-own-render-target.md` and `parhelia-kms-linear-frontbuffer.md`. |
| 60 | 10-bit texture sampling | **M** | No 10-bit sampler format is exposed. | The vendor evidence explicitly does not prove sampling and requires isolated testing (`parhelia-gigacolor-textures.md:59-111,141-152`). |

## Other documented hardware facilities

| # | hardware GL feature | status | current Mesa evidence and remaining contract | documentation evidence |
|---:|---|:---:|---|---|
| 61 | FAA-16x / multisample fragment buffer and resolve | **M** | Format support rejects `samples>1`; sample-mask setter is a no-op (`parhelia_screen.c:145-175`; `parhelia_context.c:4159-4160`). | The fragment allocation/command exists, but key registers and P690 validation remain open; truthful policy is to expose no sample buffers (`parhelia-gigacolor-faa-hardware-status.md:64-129`; `parhelia-antialiasing.md:729-744`). |
| 62 | occlusion query | **M** | No query callbacks or counter state exist.  This is not a short-term implementation candidate because no vendor stack or hardware register contract is known. | The negative vendor-stack audit and its silicon caveat are at `parhelia-feature-exposure.md:111-135`. |
| 63 | vertex blend matrices / palette skinning | **M** | Mesa has no hardware WARP lowering for the feature. | Four blend matrices and 31 indices are documented hardware capabilities (`parhelia-gl-limits.md:598`). |
| 64 | displacement mapping | **M** | No tessellation/displacement path exists. | Evidence is Windows-only and no Linux packet recipe has been recovered (`parhelia-feature-exposure.md:66-80`). |

The documentation also establishes one negative boundary outside the scored
matrix: sRGB texture decode and framebuffer encode are not documented hardware
features.  Mesa correctly rejects those formats (`parhelia_screen.c:135-143`),
consistent with both vendor GL drivers and the distinction between the scanout
gamma LUT and GL color-space conversion
(`parhelia-gamma-lut-srgb-vendor-re.md:104-158`).

## Counts and remaining room

The 64 positive hardware-feature rows classify as follows:

| status | rows | share |
|---|---:|---:|
| I+T | 12 | 18.8% |
| I+G | 19 | 29.7% |
| M | 26 | 40.6% |
| C | 7 | 10.9% |

Counting a gated/partial row as half complete gives a rough hardware-feature
coverage of **21.5/64 = 33.6%**.  This is an engineering inventory, not a GL
conformance score: rows deliberately represent user-visible feature families,
and a small state such as scissor has the same weight as a large compiler such
as programmable WARP.  The result nevertheless quantifies the opportunity:
**52/64 rows (81.3%) still require default enablement, validation,
implementation, or correction**, and 33/64 (51.6%) are wholly missing or
contradicted.  The first number includes the 19 useful-but-gated or unvalidated
rows; the second is the narrower estimate of functionality with no usable
implementation or a current correctness contradiction.

The highest-impact next implementations, ordered by benefit versus recovered
evidence, are:

1. **Masked/scissored clear.**  The ordinary draw-scissor path is now connected,
   but clear still drops masks and rectangles.  The exact vendor clear record is
   recovered, so this is the remaining high-impact scissor correctness hole.
2. **Native points and remaining line modes.**  Unit-width untextured
   `GL_LINES` is connected and offline-tested.  Points still emit no pixels;
   textured/smooth/wide lines and strips/loops remain absent despite decoded
   topology words and vendor target results.
3. **Texture completeness:** S3TC first on 0x0528-class hardware, then the
   P690 mip prerequisite, border modes and 2048 sizing.  Cube/3-D
   resources require broader resource/view work but have strong vendor recipes.
4. **Hardware raster/T&L state:** cull, polygon offset, viewport/depth range,
   clip planes, fog and lighting.  These eliminate software-draw work and expose
   capabilities already decoded in detail, but they need coordinated WARP
   program/state selection.
5. **A real fragment/WARP compiler or truthful caps.**  Until arbitrary NIR is
   lowered, GLSL 1.20 is a false capability.  A staged implementation can first
   add the documented fixed-function ADD/BLEND/COMBINE/DOT3 shapes, then expand
   to the recovered fragment ISA.

FAA, 10-bit texture sampling, native P690 mipmapping, and ordinary one-unit P690
sampled alpha should remain behind explicit hardware proof.  Their documents
record failed or incomplete target evidence, so enabling them from static packet
similarity would convert known limitations into regressions.
