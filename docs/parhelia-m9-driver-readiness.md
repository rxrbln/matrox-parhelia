# M9 kernel and Mesa driver readiness

The saved M9148 vendor stack now supplies a hardware reference for the first
Linux driver, rather than only a static design. Captures cover command
submission, a clear and triangle, all ordinary GL 1.x topologies, canonical
vertex repacking, program-pair selection, stage constants, fixed state, broad
texture targets and formats, FBO/query behavior, special GLSL results, and two
simultaneous direct-GLX clients. The remaining work divides into implementation
validation and a smaller set of hard native-record, blocked-layout, MRT,
multisample, GigaColor, sRGB, and recovery boundaries.

Confidence labels in the tables are:

* **closed**: sufficient static and/or captured evidence exists to implement;
* **captured**: the vendor behavior is preserved but the new driver still has
  to reproduce it;
* **validation**: the implementation rule is known and needs a hardware test;
* **capture**: one named live differential is still required;
* **unexposed**: the saved Linux ICD does not advertise the feature.

## First useful driver boundary

The following pieces no longer depend on another vendor capture:

* primary DMA32 command transport through exclusive interval `0x400/0x404`,
  typed relocations, the expanded writeback footer, 32-bit fence rollover, and
  fail-closed timeout handling;
* linear scanout and rendering allocations, color/depth descriptors, viewport,
  scissor, rectangle clear, and the two-pass preprocessing/raster draw;
* points, lines, line strips/loops, triangles, strips/fans, quads, quad strips,
  polygons, and H03/H06/H07 indexed/nonindexed launches;
* CPU repacking into four captured canonical vertex layouts and the exact
  generated-stream producer/consumer joins;
* depth test with writes independently enabled or disabled, one- and two-sided
  stencil packing, alpha test, blending, color masks, cull/fill/shade compiler
  inputs, and the fixed-state differential corpus. `HZFlags` statically closes
  the optional `ba/bb` plane as format-`0x0b` hierarchical-Z metadata at
  `ceil(width/8) x ceil(height/4)`, including conservative initialization,
  clear/write invalidation, owner/alias lifetime, and joint resource tracking;
  ordinary direct hardware depth does not require it, and default enablement
  remains gated only by a bounded rejection-correctness test;
* RGB565, RGBA4, RGB5A1, RGB8, RGBA8, L8, A8, LA8, I8, D16 and D32 textures,
  odd 1-D/2-D dimensions, full mip chains and packed tails, sampler state,
  four live sampler selectors, subimage/copy coherency, and the class-1
  render-to-sample dependency through index `43`.  The exact 40-bit PTE
  grammar and all class-3 calculator bodies/dispatches have deterministic
  vectors; all non-32-bit aperture permutation families have native-checked
  formulas, and cube-face/volume-Z base rules are exact. The mapped index-`78`
  image is repeated PTE-shaped metadata rather than linear texels; its live
  table-root and physical-page ownership remain unjoined;
* separate index-`69` external fixed-fragment-database and `6c` linked resources, atomic vertex plus
  fragment A/B/A selection, live stage-constant ranges, and the invariant
  nature of the 80-slot `8f/90..9f` store;
* context lifetime rules and a successful two-process test in which both
  direct contexts alternated distinct programs/colors and read back the
  expected result without state leakage.

These facts are enough for a guarded clear/triangle Gallium path and for the
fixed-function subset needed by ioquake3. The programmable transport boundary
is now closed as one opaque linked VS/FS pair selected atomically through
`6c`; there are no public native start/count or per-stage entry fields to fill.
The private whole-pipeline allocation, key cache, copy-on-write validation and
publication order are exact. Arbitrary GLSL remains unsafe until a new compiler
can construct the bytes inside that linked pair and place semantic control-flow
targets.

The first DRM implementation should continue to omit the dormant auxiliary
`0x500..0x524` requester. Ordinary GL uses the primary `0x400/0x404` path;
the five hardware-owned auxiliary words and a forced-wrap test affect only a
later streaming path. Render completion is the coherent memory fence, not an
IRQ. The documented IRQ handler should enable the four vblank sources and
leave unproved service/fault sources masked.

## OpenGL 2.0 compatibility fixed function

`parhelia-m9-fixed-function-static-closure.md` closes the complete software
state before native-program selection: world/view/projection and eight texture
matrices; front/back materials; light, color-material, normalize/rescale and
local-viewer inputs; six clip planes; all texgen modes; fog and point fields;
the GL texenv/combine source/operand/scale shadow; 28 D3D texture-stage records
and all D3DTOP operations 1 through 26.  GL should expose eight lights even
though D3D publishes 12 and the internal record list holds 16.

Direct depth/stencil/raster state uses captured packets.  Transform, lighting,
texgen, fog, points, texenv and some fragment state use generated programs.
The 19-case live T&L matrix closes directional/point/spot lighting, materials,
normal handling, two-sided/separate-specular behavior, clipping, five texgen
modes, and texture/normal matrices with B/A/B context isolation.  Directional
lighting also has the complete command/generated-instruction/resource join.
Cases 1--18 retain exact result and state evidence; repeated vendor tracing
wedged before their individual packet joins, so further broad tracing is not a
bring-up prerequisite.
Selection, feedback and evaluator semantics belong in Mesa/Draw; resident
display-list meshes are an optional cache.  Fast pixel paths exist with exact
fallback conditions and shader pack/unpack built-ins, but their conversion,
zoom and overlap result matrix is still live work.  The five bounded fixtures
at the end of the static-closure document are the remaining compatibility
capture set.

## OpenGL 2.0 shader coverage

The live compiler is OpenGL 2.0 / GLSL 1.10. Its advertised limits are 16
vertex attributes, 4 vertex samplers, 16 fragment samplers, 20 combined
samplers, 512 uniform components in each stage, 60 varying floats, and 4 draw
buffers.  Static emission proves four full sampler slots and four generic
descriptor/route operands at `74..77`; D3D separately declares four
point-filtered vertex samplers.  A live shader with five dependent fragment
textures produces the exact result through four scratch render-target passes
and one final pass in one submission, proving compiler scheduling beyond the
physical slot count.  The table distinguishes compiler acceptance from a
native encoding or an implemented scheduling limit.

| GLSL/ARB family | evidence now | driver status | smallest remaining check |
|---|---|---|---|
| MOV, ADD, SUB, MUL, MAD, DP3/4, MIN/MAX, RCP/RSQ, EXP2/LOG2, FLOOR | executable hash-pinned lowerer fixtures plus live compile/link/draw | **closed** for canonical forms | expand only when a new modifier/result class is needed |
| absolute/negate, swizzle, write masks, scalar predicates | 91-case native lowering matrix and live draw | **closed** for recorded forms | reject unrecorded combinations |
| matrices M4x4/M4x3/M3x4/M3x3/M3x2 | exact static expansion to 4/3 DP4 and 4/3/2 DP3 rows, including alias MOV | **closed** | one `mat4*vec4` regression is sufficient |
| KIL/discard | dedicated pairwise comparison lowerer, exact seven-mask native basis, all three kill/result finalizer paths, plus live conditional and unconditional discard results/resources | canonical packed forms **closed** | extend the native matrix before accepting unrecorded predicate/control combinations |
| DDX/DDY | backend selectors 91/92, isolated live `dFdx`/`dFdy` results/resources, exact canonical packed streams, and a 104-case mask/swizzle/modifier/predicate/control basis | **closed** for recorded native forms | validate helper-lane behavior at primitive and viewport edges on the new driver |
| TEXLD ordinary 2D | static lowerer, live single-level and mipmapped texture draws | **closed** for captured 2D forms | none for initial path |
| TEXLDB / projected TEXLD | exact one-hot lowerer modes plus live bias and projected result/resources | **captured** | name the final native mode field |
| TEXLDL / TEXLDD | vertex explicit LOD has a live result/resource; TEXLDD has an exact nine-slot native expansion; fragment explicit LOD and gradient are rejected because the required extension is absent | vertex **captured**; fragment **unexposed** | join the vertex LOD resource selector; retain TEXLDD for compiler tests |
| cube texture | all six faces and orientation probes pass; descriptor/resource dimension and exact `level_offset + face * face_size` base are captured/static | **captured** | mip/seam/incomplete cases and validate static face bases against decoded physical pages |
| shadow comparison | exact sampler enable/eight compare codes plus live raw 0.25/0.75 and LEQUAL fail/pass results | **captured** for the tested D24/nearest route | filtering, result-channel variants, and FBO producer dependency |
| vertex texture fetch | four-sampler compiler limit plus live vertex `texture2DLod` result/resource | **captured** | native instruction and vertex sampler join |
| SLT/SGE and comparisons | CMP/SETP frontend plus live ARBfp SLT/SGE zero/one results | **captured** for the recorded pair | exact native predicate fields and all relational outcomes |
| constant/uniform branches | live constant/uniform results plus exact ELSE builder, two packed templates, physical selector allocation and native field placement | **captured**, graph reconvergence open | distinguish folded/native cases and construct reconvergence graph |
| fixed/dynamic loops, BREAK/CALL | live fixed/uniform loops; exact loop/break builder joins, BREAK templates, CALL/LABEL/RET graph edges, CALLNZ macro, paired physical-selector allocation for `94/b4/c4`, ordered substitution, and final W0/W1 placement | physical fixup and transport lifetime **closed** for pinned families | implement graph construction/scheduling for arbitrary targets, backedges, calls, nesting and reconvergence |
| uniforms/constants | vertex and fragment A/B/A sentinels identify live-component ranges in the 0x21000 resource | **closed** for external range path | mode-2 inline constant bank remains optional |
| program entry/resource ownership | `69` binds the external fixed-fragment database, with address translation unresolved; `6c` atomically selects an opaque tagged linked VS/FS pair; private 0x3158-byte pipeline allocation, ordered key reuse and copy-on-write publication are exact; fixed-chain-only starts/counts are separately exact | transport/lifetime contract **closed** | construct the internal bytes of a new linked pair |
| termination | count-delimited compiler stream, endpoint selector `0x7e`, and result-only, kill-only and combined finalizer paths are exact | **validation** | determine endpoint-only versus full-terminal stop on generated programs; terminal variants do not encode MRT target index |

`tools/m9-gl2-compiler-control-evidence.py` pins the MxMD and MxMGLX hashes,
the complete 55-entry compiler dispatch, special lowerer call targets, matrix
call counts, derivative marker selector, and extension-token inventory. Its
JSON output is the compact machine-readable static matrix.

## Vendor extension and hardware-feature coverage

This table uses the exact live inventory in
`captures/m9x-linux-gl2-context-20260914/glxinfo-l.txt`. A name embedded in
`MxMGLX.dll` is only binary vocabulary: it is
not marked advertised unless the running M9148 ICD returned it. This matters
for S3TC and texture-float, whose names and hardware formats exist but which
the live Linux ICD did not advertise.

| feature / public path | live M9 ICD | hardware and capture evidence | driver boundary |
|---|---|---|---|
| `GL_EXT_framebuffer_object` | advertised | missing/dimension errors, depth-only, stencil-only, RGBA8+depth, render-to-sample dependency, and readback pass; distinct color+depth+stencil is rejected | **captured** for tested linear attachments; packed depth-stencil and lifecycle replay remain |
| `GL_ARB_draw_buffers` / MRT | advertised, max 4 | exact four-target descriptor/reloc/mask/blend; Windows RT1..3 use selectors 3..5 and join RT0's render-write submission value/class; 2026-09-17 live GL: one, two, three and four attachments all render and read back exactly per target (64/128/192/256 cell checks, zero mismatches), survive a framebuffer destroy/rebuild, and emit one address/descriptor pair per target in `b0/b1`..`b6/b7`; the earlier wedge was a no-read mask-15 case in Linux's timeout-free resource-close poll | **safe when every target is read back**; a written-but-never-read target is still the open risk. Verify the output-to-pair mapping per driver: the vendor's pairs appear in reverse attachment order. See `parhelia-m9-openbits-gl-capture.md` |
| `GL_ARB_occlusion_query` | advertised, 32-bit counter | exact fixed selector pair 0/1, modulo-32 result, begin/end retirement path, and live 16384/8192/0 visible/partial/hidden counts with asynchronous availability and reuse | **captured** for ordinary single-sample use; physical overflow, second snapshot word, and MSAA raw/scaled behavior remain |
| `GL_ARB_multisample` / FAA | advertised; current GLX visuals report zero sample buffers | Windows selects type 8/eight quality levels; `ad[20:0]` is allocation bytes/32; `af[8:0]` comes from reserved DP2 opcode 5 auxiliary state 25 with reset `1.0f -> 0x100`; the remaining `af` sources and GL query post-scaling are exact; zero mask suppresses color | **capture** auxiliary state 25's public meaning, allocation growth/layout, sample positions/pattern, nonzero masks, resolve, present and physical counter unit |
| 10-bit GigaColor (`A2B10G10R10`, `A2R10G10B10`) | no dedicated GL extension observed | exact codes `0x68/0x6a`, masks, sampler/RT descriptors and rounded native clear packers; Linux `GL_RGB10_A2` explicitly downgrades to `GL_RGBA8` | Linux route **not native**; capture gated 10-bit render/blend/sample/backing/readback and matching visual before exposure |
| framebuffer/texture sRGB | not advertised | D3D RT0 `SRGBWRITEENABLE` is live and emits an exact 25-slot suffix; clear has the exact CPU encode curve; mapped sampler state 29 is live, stored and queryable in both sampler paths, but its native decode consumer remains unjoined | **unexposed**; capture RT encode/blend/readback and join or capture sampler decode before enabling either capability; CRT gamma LUT remains separate |
| NPOT 2D | advertised | complete 13x7/6x3/3x1/1x1 chain, base/max selection, and defined incomplete-texture behavior are captured | **captured**; automatic generation remains |
| rectangle textures | ARB/EXT/NV names advertised | unnormalized sampling passes; level 1, repeat, and mip filters produce required errors | **captured** |
| cube textures | ARB/EXT advertised | all six face/orientation tests pass with exact descriptor/resource dimension and static per-mip/per-face base formula | **captured**; seams, mips, and physical-page validation remain |
| 3D textures | `GL_EXT_texture3D` advertised | 7x5x4 slices and R repeat pass; exact volume descriptor, full-depth mip recurrence, and calculator-Z convention | **captured**; volume mips and physical-page validation remain |
| depth texture and shadow | depth/shadow extensions advertised | raw 0.25/0.75 plus LEQUAL fail/pass results and exact descriptor/control delta | **captured** for D24 nearest; filtering/FBO dependency remain |
| S3TC/DXT1-5 | not advertised by live ICD | hardware codes/layouts are static; direct DXT1/DXT5 tokens reject, and the core compressed-format query reports four but leaves its output array untouched | **unexposed** pending a valid public token and result capture |
| VBO | advertised | indexed H06/H07 and CPU canonical repacking are captured | **closed** with CPU repack; server-side zero-copy layout is optional |
| PBO | not advertised and no `GL_ARB_pixel_buffer_object` token in pinned ICD | no M9-specific asynchronous pixel-transfer join | **unexposed**; Gallium staging copies may still back core transfers |
| separate/two-sided stencil | `GL_ATI_separate_stencil` advertised; `GL_EXT_stencil_two_side` absent | exact face controls and live separate-op packet; the vendor separate-function call rejects valid LESS/GREATER enums | implement correct core behavior from final state; do not copy the vendor API bug |
| point sprites | `GL_ARB_point_sprite` advertised | point topology/expansion and real GL_POINTS result captured; `gl_PointCoord` is rejected by the GLSL frontend | fixed-function coord replacement/attenuation and exact coverage remain |
| fog coordinates | `GL_EXT_fog_coord` advertised | fog modes and explicit fog-coordinate result/state are captured | **captured** for representative modes; implementation replay remains |
| blend add/subtract/reverse/min/max | minmax/subtract/separate extensions advertised | separate factors/equations and representative destination-sensitive results are captured; static mapping covers operations 1..5 | complete visible equation/factor matrix before broad exposure |
| anisotropic filtering | not advertised by the live ICD (`max=0`) | exact hardware anisotropy classes 1..16 exist in the Windows sampler packer | **unexposed/capture** before enabling |
| float textures | not advertised | R16F/RG16F/RGBA16F and 32-bit format codes exist | **unexposed/capture** format upload/sample and filter restrictions |

## Exact remaining blocker ledger

| remaining gap | consequence | smallest closing experiment |
|---|---|---|
| programmable linked-pair bytes inside heterogeneous `6c` records | private construction, validation, ordered key reuse and copy-on-write publication are exact, but arbitrary newly compiled GLSL still cannot serialize the internal record safely | recover or replace the opaque byte-layout builder while retaining the proven lifetime policy |
| special shader packed forms and semantic control flow | KIL, DDX/DDY and TEXLDD canonical forms are exact; ten texture metadata flags are recorded; ELSE/BREAK/BREAKP templates, parser-to-builder joins, `94/b4/c4` physical selector allocation, ordered substitution and final native selector bits are exact; B/P/L texture distinction, remaining predicates, and arbitrary branch graph construction/scheduling are not all implemented | implement graph targets/backedges/calls/reconvergence and linked-resource texture controls; reject unrecorded combinations |
| four-target MRT export and retirement | descriptor/reloc plus Windows RT0..3 job/fence membership are exact, but native output export and Linux per-RT job-BO membership are unjoined; Linux close can poll a nonretiring fence forever | decode one RT1 job through export, Linux write list and common writeback, then independently verify readback/unbind/delete/context/fd close with bounded recovery |
| multisample/FAA allocation, pattern, mask and resolve | no sample buffers and no FAA cap | acquire a vendor multisample visual/surface, then clear/edge/mask/query differentials |
| native 10-bit render and sample round trip | Linux GL downgrades RGB10_A2 to RGBA8, so GigaColor formats cannot be advertised | enable the gated Windows path and capture A2B10/A2R10 clear, draw, sample, raw storage and presentation with sentinel channels |
| sRGB write/sample conversion | RT0 write compiler and clear encode are exact; sampler decode is not joined; output LUT semantics stay separate | validate the 25-slot RT suffix through blend/readback and locate a real sampler decode path before enabling either capability |
| query counter publication and multisample scaling | fixed selector-pair reuse, software modulo-32 arithmetic, ordinary visible/hidden/partial counts, and snapshot retirement are closed | cross physical `0xffffffff`, identify snapshot word 1, and compare raw versus ICD-scaled results on a sampled surface |
| auxiliary requester wrap and five hardware-owned words | optional streaming path remains absent | one forced auxiliary wrap with four timestamped descriptor snapshots |
| transparent 3D reset and fault IRQ | raw/W1C/mask roles and ordering are exact, but no saved consumer names a requester/3D fault and no bounded reset entry exists; timeout recovery remains fail-closed | controlled bad address/opcode tests with complete pre-ack snapshots |
| class-3 physical layout and backing join | all 35 dispatch/body routes and 352 golden vectors are pinned; all 14 non-32-bit aperture formulas match native code over 104,144 comparisons; cube-face and volume-Z base math is exact | capture `request+0x90`, the table root, and decoded coordinate-coded physical pages together to join index `78` ownership and validate the formulas on hardware |

The vendor capture gates are now separate from new-driver validation. The new
kernel/Mesa stack still has to replay a no-op fence, clear, triangle, mipmapped
texture, FBO chain, and two-client workload before default enablement. Those
tests validate implementation; they are no longer requests for missing vendor
behavior.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
