# M9 OpenGL 2.0 conformance evidence audit

This checklist prevents the preserved vendor installation from being retired
after only the ioquake3/OpenGL 1.x path has been captured.  It inventories the
hardware evidence needed for an OpenGL 2.0 and GLSL 1.10 implementation.  It
does not treat a vendor extension string, compiler acceptance, or a static
packer as a successful rendering test.

The bounded static pass in `parhelia-m9-opengl2-static-closure.md` and its
verifier closes compiler/API reachability while retaining the live-result
boundary used here.  It enumerates compiler operations, built-in semantic
records, uniform entrypoints, sampler families, Direct3D state handlers, and
all sixteen logic-op frontend codes.

`parhelia-m9-d3d-caps-audit.md` preserves the independent Windows D3D8/D3D9
capability builders.  D3D9 can publish VS/PS 3.0, predication, 24 levels of
dynamic flow control, 32 temporary registers per stage, 512 pixel-shader
slots, four render targets, vertex texture point sampling, NPOT textures, and
anisotropy 16.  D3D8 clamps the same backend to VS 1.1 and PS 1.4.  These are
valuable upper-bound and compiler-reachability evidence, but remain
declarations until their generated records and visible results are joined.

The evidence states used below are:

* **result**: a controlled draw/readback demonstrated the API result;
* **packet**: the final command, resource, or native-program delta is saved;
* **static**: a hash-pinned Linux or Windows binary establishes the path;
* **API-only**: the old ICD accepted the call, without a decisive result or
  hardware-state delta;
* **open**: the new driver still needs a named capture or implementation test.

## Core state, rasterization, and framebuffer operations

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| matrix transforms and stacks | exact world/view/projection/eight texture-matrix dispatch and compiler records; live non-symmetric texture/normal-matrix results with B/A/B context isolation | implementation replay; raw generated packet/resource join exists for directional lighting, while the other 18 T&L cases retain exact result/state evidence |
| lighting and materials | exact state/compiler records plus 19-case live B/A/B matrix for directional/point/spot attenuation, front material, local viewer, normalize/rescale, color material, two-sided/separate specular, flat provoking color, clipping, texgen and matrices; GL exposes 8 lights, D3D 12, internal list 16 | implementation replay; per-case native/resource joins for cases 1--18 are optional diagnostics after the vendor tracer wedged twice |
| texgen and fixed texture environment | exact accepted S/T/R/Q mode matrix; five distinct live texgen result hashes; eye/object planes, eight texture matrices, complete GL texenv/combine/source/operand/scale shadow, 28 D3D TSS records and D3DTOP 1..26 | legacy/combine/DOT3/crossbar/bump result matrix and native-record joins |
| viewport, depth range, scissor | result/packet for nontrivial rectangles and depth range | implementation replay |
| color/depth/stencil clear and masks | packet/static; color mask and complete depth/stencil fields | validate every advertised depth/stencil format and partial clear masks |
| FBO completeness and render-to-texture | result/packet for missing attachment, dimension mismatch, depth-only, stencil-only, color+depth, and producer-to-consumer sampling | packed color+depth+stencil combinations, deletion/rebinding, and new-driver replay |
| all eight depth comparisons; depth write independently disabled | result/packet | implementation replay |
| front/back stencil compare, reference, masks, and eight operations | static/packet; ATI separate op path captured; vendor separate-function call is broken | validate core `glStencil*Separate` behavior in the new implementation |
| culling, winding, smooth/flat shade | packet/result | implementation replay |
| point, line, triangle, strip, fan, quad, quad-strip, polygon topology | packet/result for the ordinary frontend set; D3D caps allow 1,048,575 primitives and indices | implementation replay and degenerate/batch-boundary tests |
| polygon point/line/fill, point/line width, smoothing | packet/result for representative sizes; D3D publishes maximum point size 2048 | width ranges, line stipple, point attenuation, and exact coverage rules |
| polygon offset | enable packet/static; D3D slope and units handlers carry raw floats through `0x104267/0x1042a9` | join OpenGL factor/units to the generated depth equation and visible quantized result |
| alpha test, blending, separate RGB/alpha, min/max/subtract/reverse subtract | enable/result/static factor and equation maps | join generated native state to visible destination-sensitive results |
| all logic operations | static frontend: all sixteen GL enums map to exact ICD internal codes; XOR packet/result | join the other fifteen internal codes to final native state/program and visible results |
| fog, explicit fog coordinate, clip planes, two-sided lighting | API/result plus exact fog/clip/light state and generated compiler records; non-symmetric clip and two-sided lighting are live-closed in B/A/B contexts | remaining fog-coordinate result variants and implementation replay |
| multisample and sample coverage | API-only on an unsampled visual; Windows config stores internal sample type 8 and reports eight quality levels; the format-table constructor stores nonmaskable masks and one GetDriverInfo path recomputes type 8; `ad[20:0]` is backed allocation bytes/32; reserved DP2 opcode 5 alone reaches the auxiliary 1..31 dispatcher, whose state 25 stores the float reset to `1.0f` and converted to `af[8:0]=0x100`; the other `af` sources are exact; zero mask suppresses output; GL query post-scaling is exact | identify auxiliary state 25's public meaning and the runtime format-query branch; capture sampled allocation growth, fragment layout, positions/quality patterns, nonzero mask/invert, resolve, presentation, and physical query-counter unit |
| occlusion query | result/packet for 16384 visible, 0 hidden, 8192 half-hidden, object reuse, and pre-finish availability zero; fixed selector pair 0/1, separate relocated begin/end resources, modulo-32 result, and both-snapshot retirement are exact; no slot allocator exists | physical counter overflow, snapshot word 1, ordering stress, raw/MSAA-scaled behavior, and implementation replay |
| draw buffers/MRT | exact Windows fan-out for four descriptor pairs, relocations, format classes, masks/blend and common TaskMgr render-write fences; masks 0/1 complete and RT0 writes; mask 2 and no-read mask 15 finish before Linux wedges in timeout-free resource-close retirement | recover native output export and each RT's job-BO membership, then prove a decoded RT1 job, common fence writeback, independent readback and bounded close; expose RT0 only |

## Textures, samplers, and pixel transfer

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| 1-D and 2-D integer/normalized formats | result/packet/readback for odd 1-D, RGB565, RGBA4, RGB5A1, RGB8, RGBA8, L8, A8, LA8, I8, D16 and D32; exact descriptor codes and allocation alignments | remaining upload types, component conversions, filtering, renderability, and implementation replay |
| NPOT 2-D | result/packet for a complete 13x7, 6x3, 3x1, 1x1 chain, including exact logical dimensions | implementation replay and incomplete-chain validation |
| mipmaps and generated mipmaps | result/packet for complete odd chains, base/max selection, defined incomplete-texture behavior, tails, signed bias and clamped LOD; D3D autogen opcode 89, filter mapping, and surface-vmethod dispatch are exact | join the autogen vmethod to its layout walk, result, and fence |
| nearest/linear and six minification filters | packet | visible LOD/footprint validation |
| S/T wrap modes and border color | packet plus result for repeat and a nontrivial border color; mirror/edge selectors are static/packet | 1-D behavior and exact border-color packing |
| LOD bias, min LOD, max LOD | packet/result for negative bias, both signed saturation limits, min 3, and max 2 | implementation replay |
| cube maps | result/packet for all six faces and orientation checks; exact physical base `allocation + level_offset + face*face_size` | seams, mip selection, incomplete cube behavior, and decoded-page validation |
| 3-D textures | result/packet for a 7x5x4 volume, all slices, and R repeat; exact full-depth level-offset recurrence with Z passed through the address calculator | volume mip results, edge coordinates, and decoded-page validation |
| rectangle textures | result/packet for unnormalized coordinates; level 1, repeat, and mip filter produce the required errors | implementation replay |
| depth textures and shadow compare | result/packet for raw D24 0.25/0.75, LEQUAL fail/pass, and exact D16/D32 gradients; static eight compare codes | result-channel/filter matrix, other compare functions, and FBO producer dependency |
| texture compression | generic extension advertised; static DXT1-5 packers; Linux S3TC absent and direct DXT1/DXT5 uploads return `GL_INVALID_ENUM` | sentinel-filled core-format query; keep DXT disabled unless a returned public format passes upload/sample/readback |
| texture objects, subimages, copies, priorities/residency semantics | live update-then-sample and framebuffer-copy-then-sample ordering, exact transfer/draw stream boundary, stable descriptors, and ordinary static lifecycle | overlapping copies, subimage/mip boundaries, deletion while bound, priorities/residency, and implementation replay |
| pixel-store, Draw/Copy/ReadPixels and bitmap | fast GPU path diagnostics and exclusions, shader pack/unpack operations 117..125, basic readback | alignment/row-length/skip/type conversions, overlap, zoom/orientation, depth/stencil pixels, and fast-versus-fallback results; performance is secondary |
| 10-bit GigaColor | exact native codes `0x68/0x6a`, channel masks, sampler/RT construction and rounded D3DCOLOR clear packers; Linux `RGB10_A2` explicitly reports actual `GL_RGBA8` and 8-bit quantization | native Windows/gated render, blend, sample, raw-backing/readback capture, plus separate depth-30 scanout/LUT/dither validation |
| float, signed, packed, depth/stencil, YUV, DXT, anisotropic, and sRGB formats | exact D3D map for 46 formats and 13 rejections; native codes/classes/bit sizes; exact 52-row format-operation baseline plus removal-only restrictions; live RT0 sRGB-write state lowers to an exact 25-slot suffix and clear uses the exact piecewise encode; D3D9 sampler state 29 is live for pixel and vertex samplers, stores `+0x46` bit 5 and survives state blocks/query normalization, but the pinned final native lowerers do not consume it; state 11 is the separate TEXCOORDINDEX control | keep unvalidated families gated until upload/sample/filter/render/blend/copy/readback behavior is proved; join a derived-record consumer or use explicit shader lowering for sRGB read |

## Buffers, arrays, and draw submission

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| client arrays and sixteen generic attributes | static declaration grammar, complete ICD generic-attribute entrypoint vocabulary, four canonical live layouts, and D3D declarations for normalized short/ushort/ubyte and float16 types | visible conversion matrix, stride/offset, disabled current value, and alias rule |
| VBO creation/update/map/unmap | advertised; vendor resource/fence lifecycle and captured VBO draw path; exact 40-bit PTE grammar, all 35 class-3 calculator routes/352 vectors, and all 14 non-32-bit aperture formulas over 104,144 native comparisons; the index-`78` image is PTE-shaped metadata rather than texels, but its table-root and physical-page ownership are unjoined | orphaning, partial updates, map failure, deletion while referenced, context sharing, and one root-plus-decoded-pages capture |
| 16/32-bit element buffers and draw ranges | packet/static H06/H07 paths | base/range boundaries, restart absence, mixed index sizes, and zero-count draws |
| immediate mode and display lists | live fixed-function workloads | state capture/replay and resource lifetime in the Mesa implementation |
| evaluators, selection, feedback, display-list semantics | API frontends and explicit software paths; resident GPU mesh cache exists for compiled geometry | Mesa/Draw CPU semantic tests followed by ordinary GPU-draw state-isolation checks |
| multiple contexts/share groups | result/packet for simultaneous A/B/A/B direct contexts | shared texture/VBO/program deletion and concurrent update ordering |
| command completion and errors | coherent fence/static lifetime path | bounded wrap, rollover, OOM, invalid command rejection, hang/device-loss behavior |

## GLSL 1.10 language and programmable pipeline

Compiler acceptance is insufficient here.  Each native family needs its
instruction bytes, stage ownership, constants/resources, immutable linked-pair
construction, and a visible result joined in the same fixture.  The external
programmable path exposes no public per-stage entry/count or end-PC fields;
those fields belong only to the separate fixed-chain inline path.

| GLSL/program area | preserved evidence | remaining work before exposure |
|---|---|---|
| shader create/source/compile/link/validate/use/delete and info logs | live compiler matrix | negative tests, deferred deletion, relink, detach, and cross-context sharing |
| scalar/vector/matrix bool, int, float types; constructors and conversions | static aggregate/type descriptor, integer frontend vocabulary, all 19 uniform update families, and one live program combining arrays, integers, booleans, mat4, VS and FS uniforms | systematic visible-result matrix; lower unrecorded integer/bool combinations conservatively |
| arithmetic, swizzles, write masks, abs/negate, min/max, reciprocal/rsqrt, exp/log, floor | 91-case native lowerer corpus | join generated bytes to the live linked-program record and reject unrecorded native combinations |
| dot products and matrix/vector multiplication | static exact DP3/DP4 expansion; live `mat4 * vec4` result | non-square matrix variants and alias cases in live native storage |
| comparisons, boolean logic, selection | static CMP/SETP plus live ARBfp SLT/SGE and GLSL conditional results | exact native predicate fields and all six relational outcomes |
| constant/uniform conditionals | live result captures; exact ELSE builder join and two packed conditional templates | distinguish folded/native cases and resolve physical branch target/reconvergence fixups |
| fixed and uniform-bound loops, break/continue/function calls | live fixed/uniform-loop results; exact REP/LOOP/ENDLOOP, BREAK/BREAKP/BREAKC joins, BREAK templates, CALL/LABEL/RET graph edges, and CALLNZ macro expansion | resolve typed fixup serials into final targets/backedges/calls, then establish nesting and implementation limits |
| vertex/fragment uniforms and arrays | A/B/A stage-specific resource/result captures; exact limits | matrices, integer/bool uniforms, arrays, partial updates, locations after relink |
| attributes, varyings, built-ins, invariant matching | static 14-entry varying map and exact semantic records; live `gl_FrontFacing` produces distinct front/back colors; `gl_PointCoord` is rejected by the GLSL 1.10 frontend | remaining visible built-in routes, type/size mismatch failures, interpolation slots, and fixed-function point-sprite lowering |
| ordinary, projected, and bias texture lookup | live result captures plus static texture lowerer; executable minimal fixtures prove B/P/L have no slot-local delta, so their distinction belongs to linked coordinate/resource state; explicit fragment LOD/gradient correctly reject without the extension | join the B/P/L linked-resource controls without inventing local instruction bits |
| vertex explicit-LOD lookup | accepted live result capture under the four-vertex-sampler limit | native instruction and vertex sampler/resource join |
| derivatives | live `dFdx`/`dFdy` results; exact selectors, canonical native streams, and 104 mask/swizzle/modifier/predicate/control variants | validate helper-lane behavior at primitive and viewport edges on the new driver |
| discard/KIL | live conditional and unconditional discard results, exact lowerer, and exact seven-mask packed KIL basis | compose only recorded predicate/compare forms; validate helper and coverage behavior on the new driver |
| fragment depth output | live `gl_FragDepth=0.25` color/depth result plus static one-component semantic `0x13` and result-routing vocabulary | native output-route join and later-depth-test ordering replay |
| four fragment color outputs | runtime-sized `gl_FragData[gl_MaxDrawBuffers]` constructor, static target compiler/emitter, and live RT0 result; RT1/all-target attempts finish then wedge during readback/teardown | recover safe targets 1--3 routing and retirement before another result capture |
| vertex position, point size, clip vertex; fragment color/depth built-ins | exact logical semantics `0x0d..0x13`, component counts and stage presence masks; partial fixed-function/point evidence | direct GLSL result fixtures to join every logical semantic to its native packed route |
| compiler resource limits | Linux advertises 16 attributes, 4 vertex samplers, 16 fragment samplers, 20 combined, 512 uniform components per stage, 60 varying floats, and 4 draw buffers; static emission proves four full sampler slots plus four generic descriptor/route operands; a five-dependent fragment shader produces the exact result through four scratch RT passes and one final pass; active `vec4[129]` exposes a separate vendor limit bug | recover scratch allocation/lifetime and side-effect rules, then test N/N+1 boundaries; advertise only measured hardware plus implemented lowering limits |
| program transport and termination | exact two-dword slots; ordered descriptor substitution before packing; fixed-chain inline starts, 32-byte padding and 256-slot bound; exact result-only, kill-only and combined finalizer forms; live immutable linked VS/FS pair selected atomically by `0x6c`; private 0x3158-byte allocation, keyed reuse, >1023-entry reclamation, COW publication and fence retention are exact; terminal forms have no MRT-index input | recover the opaque internal pair byte serializer, place semantic control-flow targets, establish endpoint-versus-full-operation hardware stop semantics, and exercise reset behavior |

## Extension and vendor-only retention

The hardware also has paths outside the OpenGL 2.0 core contract.  Preserve
them for later Gallium capabilities without letting them inflate the core
conformance claim:

* native 10-bit GigaColor rendering and its no-page-flip presentation restriction; the Linux GL path's `RGB10_A2` alias to RGBA8 is an explicit negative result, not native 10-bit evidence;
* floating-point texture formats, YUY2/UYVY sampling/video paths, DXT1-5, and
  dormant anisotropy levels 1/4/8/16;
* render-target sRGB conversion and sampler-side execution: live D3D9 sampler
  state 29 stores and preserves an sRGB-read bit, but no consumer is joined in
  the pinned final lowerers; state 11 is a distinct TEXCOORDINDEX control;
* four-target MRT, hardware occlusion counters, and the incomplete FAA/MSAA
  path;
* the D3D9 SM3 compiler surface: predication, long dynamic flow, 32 temporary
  registers per stage, vertex texture fetch, and substantially larger declared
  shader limits than the Linux GLSL 1.10 frontend;
* point sprites, wide/smooth/stippled lines, stereo/layer planes, overlays,
  VPE/DXVA, tear-free/page-flip policy, and independent back buffers;
* conservative serialized context switching, resource fences, hardware copy,
  and the primary requester; no fast-context-switch, fast-trilinear,
  multiple-pipe, or volumetric-shadow M9 path is present in the pinned images.

The aging vendor installation can be retired only after every item in the
remaining-work column has either preserved live evidence, conclusive static
proof that no public path exists, or an explicit decision to keep the
corresponding capability disabled.  New-driver replay tests are still required
after capture; preserved vendor behavior does not prove the new kernel/Mesa
implementation.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
