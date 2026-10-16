# M9 OpenGL 2.0 and GLSL 1.10 bounded static closure

This note applies the conformance checklist to the pinned M9 Windows and Linux
compiler images.  It records what static analysis closes exactly and names the
smallest live experiment wherever the API result or native packed form cannot
be recovered from code alone.  It does not treat a string, accepted enum, or
Direct3D state shadow as proof of a conforming OpenGL result.

Run the evidence check from the repository root:

```sh
python3 tools/m9-opengl2-static-closure-evidence.py
python3 tools/m9-opengl2-static-closure-evidence.py --json
```

The checked images are:

| image | SHA-256 |
|---|---|
| `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| Linux 1.15 `m9x_drv.so` | `15ff4ad6e450c2d5a27c3d114011014e8aaf97851d107a070f31baa26eafc7e5` |

Addresses below are preferred-image virtual addresses. `[BIN]` is an exact
binary fact, `[DER]` a deterministic decode, and `[LIVE]` an experiment which
cannot be replaced by more disassembly.

## Compiler operation inventory

MxMD converter `0x1a40b0` has a complete 55-entry Iris dispatch table at
`0x1a5a3c`.  Its complete base vocabulary is [BIN]:

```text
00 NOP   01 MOV   02 ADD   03 SUB   04 MAD   05 MUL   06 RCP   07 RSQ
08 DP3   09 DP4   10 MIN   11 MAX   12 EXP   13 LOG   16 LRP   17 FRC
18 M4x4  19 M4x3  20 M3x4 21 M3x3 22 M3x2 23 CALL
30 POW   31 SGN   32 ABS   33 NRM   34 SINCOS
41 BREAK 45 KILL  46 TEXLD 48 CMP  49 DSX   50 DSY
51 TEXLDD 52 SETP 53 TEXLDL 54 TEXLDB 55 TEXLDP
```

Base values `14,15,24..29,35..40,42..44,47` reach the formatter's explicit
unsupported/reserved path.  The compiler frontend also names this extended
integer vocabulary [BIN]:

```text
101 FTOI 102 FTOU 103 IADD 104 signed comparisons 106 IMAD 107 IMAX
108 IMIN 109 IMUL 10a INEG 10b ISHL 10c ISHR 10d ITOF 10e NOT
10f OR 110 UDIV 111 UMOD 112 unsigned comparisons 114 UMUL 115 UMAD
116 UMAX 117 UMIN 118 USHR 119 UTOF 11a XOR 11b ICMP 11d SIN 11e COS
8001 CEIL 8002 TRUNC 8003 FLOOR 8004 DP2 8005 M2x2
```

GLSL 1.10 exposes float, int, and bool language operations, but the extended
names are frontend IR.  They do not prove native integer arithmetic.  The safe
new-driver rule is to lower GLSL 1.10 integer and boolean work into proven
float/CMP/SETP forms, and reject a form whose native sequence is not in the
recorded corpus.  A direct native integer capability must not be advertised.

The recovered exact lowerings are:

| language family | exact static route | remaining boundary |
|---|---|---|
| add/sub/mul/mad/min/max, DP3/DP4, reciprocal/rsqrt, exp/log, fraction/floor, abs/sign | ordinary dispatch plus recovered native packer | accept only native combinations in the 91-case evidence corpus |
| `mat4x4`, `mat4x3`, `mat3x4`, `mat3x3`, `mat3x2` | wrappers `0x1c74c0/0x1c7690/0x1c7820/0x1c79f0/0x1c7b80`; exact 4/3 DP4 and 4/3/2 DP3 rows; alias adds a final MOV | none for lowering; one live matrix regression |
| relational operators, equality, boolean selection | six conditions GT/EQ/GE/LT/NE/LE; CMP/SETP and frontend SLT/SGE routes | native nonzero predicate values and all six visible outcomes [LIVE] |
| discard | `KILL 45 -> backend 65 -> 0x1c82d0`, pairwise `xy`/`zw` comparison records | conditional and unconditional packed slots plus helper coverage [LIVE] |
| derivatives | `DSX 49 -> 91/0x88`, `DSY 50 -> 92/0x89`, common `0x1c9300`; canonical packed streams and 104 operand/control variants exact | helper-lane edge behavior on the new driver [VALIDATE] |
| calls, branches, loops, break/continue | CALL and BREAK dispatch plus conditional/predicate stack are exact frontend paths | target, backedge, reconvergence, folding/unroll policy and nesting limits [LIVE] |
| texture lookup | common `0x1bda50` with one-hot ordinary/bias/projected/LOD/gradient modes | packed mode, stage resource, and result route for special forms [LIVE] |

The ARB parser separately accepts `KIL, TXB, TXP, TEX, SWZ, MAD, LRP, CMP,
XPD, SUB, SLT, SGE, MUL, MIN, MAX, DST, DPH, DP4, DP3, ADD, POW, SCS, SIN,
RSQ, RCP, LG2, EX2, COS, MOV, LIT, FRC, FLR, ABS`.  This independently bounds
the public assembly frontend, while the Iris table above bounds the backend
converter.  Neither list is a native opcode enum.

## Types, operands, declarations, and arrays

The compiler operand descriptor has exact fields [BIN]:

| dword-0 field | role |
|---|---|
| bits `4:0` | register file |
| bits `10:6` | operand form |
| bits `12:11` | relative-address component |
| bits `16:13` | component enables |
| bits `24:17` | swizzle |
| bits `27:25` | scalar/vector/matrix/structure/array/label class |
| bits `30:28` | array element class |

This closes representation of scalar, vector, matrix, structure and array
objects, direct and relative addressing, swizzles, write masks, and aggregate
element type.  Exact register-file codes are `r=0`, `v=1`, `c=2`, `a=3`,
`o=6`, `ci=7`, `s=10`, `cb=14`, system input `17`, literal `18`, and internal
`rb/ri=30/31`.  Destinations accept only `r,a,o,p,rb,ri`.  The final physical
varying selector is assigned through explicit input/output link tables rather
than using the logical source index directly.

The Direct3D declaration parser `0x10f060 -> 0x10b690`, append helper
`0xa9820`, and fixed conversion selector `0x13b2b0` close the input conversion
families [BIN]:

| declaration types | selector family |
|---|---|
| `UBYTE4, SHORT2, SHORT4, UBYTE4N, SHORT2N, SHORT4N, USHORT4N, UDEC3` | packed conversion |
| `FLOAT3, FLOAT4, D3DCOLOR, USHORT2N, DEC3N` | float/color/special conversion |
| `FLOAT1, FLOAT2, FLOAT16_2`, default | generic conversion |
| `FLOAT16_4` | higher format-bit case; must not alias low-nibble `FLOAT1` |

The OpenGL ICD contains reachable entrypoint vocabulary for current generic
attributes with 1--4 components, float/double/short inputs, all signed and
unsigned four-component vector forms, and normalized signed/unsigned vector
forms, plus `glVertexAttribPointer`.  This closes API conversion coverage, not
zero-copy hardware layouts.  Mesa may CPU-repack all of them into a captured
canonical float layout first.  Disabled current values, stride/offset
boundaries and generic attribute zero aliasing remain new-driver tests.

Array type and relative-index representation are static facts.  Runtime array
bounds, nested structure/array layout, array-of-matrix uniform updates and
aliasing need compile/link tests at sizes `N` and `N+1`; no allocation byte
count should be exposed as a GLSL limit.

## Built-ins and result routes

Compiler preamble constructor `0x10689b20..0x1068a07c` creates these exact
records through common helper `0x10681b90` [BIN]:

| built-in | components | logical semantic | type/class argument | presence mask |
|---|---:|---:|---:|---:|
| `gl_FrontFacing` | 1 | `0x10` | 3 | `0x01` |
| `gl_FragCoord` | 4 | `0x11` | 1 | `0x02` |
| `gl_FragColor` | 4 | `0x12` | 1 | `0x04` |
| `gl_FragDepth` | 1 | `0x13` | 1 | `0x08` |
| `gl_Position` | 4 | `0x0d` | 1 | `0x10` |
| `gl_PointSize` | 1 | `0x0e` | 1 | `0x20` |
| `gl_ClipVertex` | 4 | `0x0f` | 1 | `0x40` |

`gl_FragData` is constructed separately at `0x1068ace0`; it is a runtime-sized
array keyed by the supplied `gl_MaxDrawBuffers`, and its symbol is installed at
`0x1068ad7a`.  Static validation rejects simultaneous assignment to
`gl_FragColor` and `gl_FragData`, and requires a fragment shader either to
discard or to write one of those color results.  Linker/output lowering and the
four color surface pairs establish four logical targets.

The table closes symbol presence, legal stage class and logical semantic ID.
It does not close the final native output field.  The exact irreducible tests
are:

The same pinned preamble names the complete fixed-function compatibility
surface consumed by this compiler [BIN]: vertex inputs `gl_Vertex`,
`gl_Normal`, `gl_Color`, `gl_SecondaryColor`, and `gl_MultiTexCoord0..7`;
vertex outputs/front-back varyings `gl_FrontColor`, `gl_BackColor`,
`gl_FrontSecondaryColor`, `gl_BackSecondaryColor`, `gl_TexCoord`, and
`gl_FogFragCoord`; matrix uniforms and their inverse/transpose variants for
model-view, projection, model-view-projection and texture matrices; normal
matrix/scale; clip planes; eye/object texture planes; texture environment
color; depth range, fog, point, material, light source/product and light model
structures.  The verifier checks 66 distinct built-in names.  These names and
generated declarations close frontend availability; their fixed-function
lowering still uses the captured generated-state/program path and must not be
treated as new native ISA operations.

| result/input | shortest decisive fixture |
|---|---|
| `gl_FragDepth` | two sentinel depths, then a later depth test which selects exactly one; save packed program and depth BO |
| `gl_FrontFacing` | opposite-winding triangles with culling disabled and two output colors |
| `gl_PointSize` | vertex shader sizes 1 and 7 with measured coverage and saved generated streams |
| `gl_ClipVertex` | one primitive crossing one enabled user plane, then a sign-reversed control |
| `gl_Position` | nonidentity position transform with viewport/readback |
| `gl_FragData[0..3]` | four unique attachment colors and readback of every attachment |

`gl_PointCoord` is absent from both the pinned Windows compiler and the
independent Linux compiler image.  This is a conclusive bound on these builds,
not proof that hardware cannot generate point coordinates.  The live compiler
probe decides whether it is rejected, accepted through generated extension
source, or supplied only by a fixed point-sprite path.  Keep the GLSL built-in
and corresponding cap disabled unless that probe succeeds.

## Uniform types and update transport

The ICD carries all 19 OpenGL 2.0 uniform update families: scalar/vector
`glUniform[1-4]f[v]`, scalar/vector `glUniform[1-4]i[v]`, and
`glUniformMatrix[2-4]fv`.  The concrete context table at `0x107f4a70` maps the
float vector slots `+0x4f8..+0x504` to `0x100b9f10..0x100b9f70`; the common
linked-program updater is `0x10559510` [BIN].

Locations select 64-byte metadata records.  Each record carries independent
vertex and fragment native indices.  Updates compare bytes, stage changed
values in tightly packed vec4 ranges, then drain exact 16-byte range records
through private opcode `0x65` for vertex and `0x66` for fragment.  Vertex
ranges always precede fragment ranges; each private batch contains at most 300
records.  The independent Linux implementation has the same record and order.

This closes stage isolation, float/int/bool/sampler setter routing, vector
counts, arrays as consecutive locations, matrix API coverage, unchanged-value
suppression and partial dirty ranges.  It does not prove the compiler's final
representation for every int/bool conversion or matrix transpose case.
Conformance still needs visible vertex and fragment results for float, int,
bool, sampler, mat2/mat3/mat4, an array partial update, a relinked location, and
the required transpose rejection.

The advertised limits are exact frontend limits: 512 vertex and fragment
uniform components, 16 generic attributes, 60 varying floats, four vertex
samplers, sixteen fragment samplers, twenty combined samplers, and four draw
buffers.  Internal `0x7c00` allocation metrics and 256/224/0x2bb backend bank
sizes are private capacities.  Each public limit requires a compile/link test
at exactly `N` and `N+1` before the new compiler reports it.

## Textures, samplers, and formats

The compiler constructs all GLSL 1.10 sampler types used by this stack:
`sampler1D`, `sampler2D`, `sampler3D`, `samplerCube`, `sampler1DShadow`,
`sampler2DShadow`, `sampler2DRect`, and `sampler2DRectShadow`.  Its prototypes
cover ordinary, projected, bias and explicit-LOD functions for 1-D/2-D/3-D;
ordinary and explicit LOD cube functions; ordinary/projected rectangle
functions; and ordinary/projected/LOD shadow functions [BIN].  Explicit
gradient is represented by the backend `TEXLDD` route, although it is not a
GLSL 1.10 public builtin in the advertised extension set.

The static format and layout table closes these families [BIN][DER]:

| family | exact evidence | exposure boundary |
|---|---|---|
| normalized/color | RGB/XRGB `0x64`, ARGB8 `0x66`, RGB565/XRGB1555 `0x4e`, ARGB1555 `0x4a`, ARGB4444 `0x46`, alpha/luminance families `0x30/0x31/0x33/0x50/0x51` | upload/sample swizzle and normalization per public GL format |
| 10-bit | A2B10G10R10 `0x68`, A2R10G10B10 `0x6a` | sentinel upload/render/sample/readback |
| vector/signed | exact V/U/Q/W descriptor codes `0x55/0x59/0x65/0x6d/0x71/0x79` | public normalization/result matrix |
| float | R/RG/RGBA 16-bit `0x54/0x74/0x3c`; 32-bit `0x75/0x3b/0x34` | live ICD does not advertise texture-float; filter/render tests required |
| depth | D16 `0x5c`, D32 `0x7c`, D15S1 `0x5d`, D24 family `0x7d` | raw depth sample, shadow compare, FBO producer and packed stencil legality |
| DXT | DXT1 `0x10`, DXT2/3 `0x15`, DXT4/5 `0x17` plus exact block/tail offsets | live ICD does not advertise S3TC; DXT1/DXT5 edge-block samples |
| YUV | UYVY/YUY2 `0x58` | plane order, chroma siting and conversion result; no GL exposure |

One root relocation plus descriptors `78/79` describes a texture; LOD bounds
are in `7d`.  The ordinary per-level recurrence, 32x16 packed mip tail, and
compressed offsets are exact.  NPOT, cube, 3-D and rectangle constructors are
real, while face orientation, volume slice addressing, unnormalized rectangle
coordinates and odd-size mip results remain targeted live tests.

Shadow compare is sampler state: enable joins backend bit 26 and compare codes
are `NEVER,LESS,EQUAL,LEQUAL,GREATER,NOTEQUAL,GEQUAL,ALWAYS ->
0,1,3,2,6,4,5,7`.  A GLSL shadow sample must still join this state to a
pass/fail output channel.

Render-target sRGB is a reachable D3D state: selector 194 uses handler
`0x1037e7`, sets frontend bit 11 and dirty group `0x10000`; output-zero compiler
gate `0x137242` inserts the native conversion suffix built at `0x174ea0`, and
the clear path has a corresponding RT0 conversion condition.  D3D9 maps
public `D3DSAMP_SRGBTEXTURE` to legacy HAL/DDI state 29; both pixel/fixed and
vertex sampler dispatchers store its Boolean at sampler `+0x46` bit 5, and
state blocks plus normalized queries preserve it.  Neither pinned final native
lowerer consumes bit 5, so execution remains unjoined.  The apparent sampler
state-11 entry at `0x105c04` is unreachable legacy table residue: live state 11
is `TEXCOORDINDEX`, and compiler `0x17f91d` consumes its three-bit field as a
coordinate-record index unrelated to sRGB.  Neither pinned Linux compiler
advertises an sRGB texture extension, so keep sampler sRGB disabled until a
derived-record consumer or explicit shader lowering is joined and validated.

## Core fragment and raster state

The Direct3D state dispatcher at `0x103370`, selector table `0x104bf4`, and
handler table `0x104990` give exact reachability for the M9 backend [BIN].

| OpenGL family | exact recovered state | remaining result check |
|---|---|---|
| depth | enable/write/eight comparisons and independent write disable join final `a8`; live bits `4:2` are decoded below | replay every advertised depth format |
| stencil | handlers 52--59 and 185--189; both faces, refs/masks and all eight operations join `a8..aa`; live final fields are decoded below | core separate front/back visible outcome |
| blend | source/destination handlers, equations add/subtract/reverse/min/max, constant factor, separate alpha handlers 206--209, four target masks | destination-sensitive MIN/MAX/reverse and independent alpha outcomes |
| alpha test | enable/ref/eight comparisons | visible boundary values |
| color masks/MRT | handlers 168 and 190--192 plus four surface pairs `b0..b7` | four distinct outputs and partial masks |
| multisample | handlers 161/162 and final `ad/af` emitter | sampled allocation, pattern, mask, resolve and coverage invert |
| polygon offset | slope handler `0x104267`, units handler `0x1042a9`; raw floats stored at state `+0x20/+0x24`, shared dirty bit `0x1000` | OpenGL factor/units to final generated depth equation and visible result |
| points | size/min/max, sprite enable and attenuation A/B/C handlers `0x10454b..0x104634` | GLSL size, coord replacement and measured coverage |

Two code layers must remain explicit.  The Windows Direct3D frontend remapper
uses comparison codes `NEVER,LESS,EQUAL,LEQUAL,GREATER,NOTEQUAL,GEQUAL,ALWAYS
-> 0,1,3,4,6,5,7,2` and operation codes
`KEEP,ZERO,REPLACE,INCRSAT,DECRSAT,INVERT,INCR_WRAP,DECR_WRAP ->
0,3,1,6,7,2,4,5`.  These are compiler/front-end codes, not final packet bits.

The live GL command matrix in commit `dc180f4` closes the actual `a8` fields:
depth comparison bits `4:2` and stencil comparison bits `9:7` both use
`NEVER,LESS,EQUAL,LEQUAL,GREATER,NOTEQUAL,GEQUAL,ALWAYS ->
0,2,1,3,4,6,5,7`.  Each stencil operation field uses
`KEEP,ZERO,REPLACE,INVERT,INCR_WRAP,DECR_WRAP,INCRSAT,DECRSAT ->
0,3,1,2,4,5,6,7`.  The fixed-state lowerer performs the required translation;
a new driver must emit the live final codes rather than the Direct3D frontend
values.

`glLogicOp` is now statically closed through the ICD frontend.  Validator
`0x100ae440` accepts exactly `0x1500..0x150f`, calls mapper `0x1005fa00`, and
the 16-entry table at `0x1005fb48` yields [BIN][DER]:

| GL op | internal code | GL op | internal code |
|---|---:|---|---:|
| CLEAR | 1 | NOR | 2 |
| AND | 9 | EQUIV | 10 |
| AND_REVERSE | 5 | INVERT | 6 |
| COPY | 13 | OR_REVERSE | 14 |
| AND_INVERTED | 3 | COPY_INVERTED | 4 |
| NOOP | 11 | OR_INVERTED | 12 |
| XOR | 7 | NAND | 8 |
| OR | 15 | SET | 16 |

Every handler stores the original GL enum at state `+0x4c` and the internal
code at `+0x50`.  This proves all sixteen API routes.  Only XOR currently has a
final packet/result capture; the downstream internal-code-to-native program or
state mapping for the other fifteen is still a compact live matrix, not a safe
guessed ROP3 table.

## Queries, framebuffer objects, MRT, and capability masking

Four color attachment address/descriptor pairs `b0/b1..b6/b7`, one depth pair
`b8/b9`, and the `HZFlags`-controlled hierarchical-Z metadata pair `ba/bb`,
target masks, the fragment output declaration lowerer `0x181fa0`, and surface
emitter `0xfb920` are exact.  Allocator `0x54be0` gates 16/32/64 stored-bit
depth classes with `HZFlags` bits 0/1/2 and creates internal format `0x0b` at
`ceil(width/8) x ceil(height/4)`; binder `0xaaa7d` installs selector 12 in
`ba/bb`.  The vendor initializes and conservatively invalidates the entire
auxiliary plane with word `0x007ff000`; a full depth clear seeds it from the
clear value, a partial clear invalidates the whole plane, and uncoupled writes
set a retryable refresh marker.  Creator-owned lifetime, non-owning clones,
and joint parent/auxiliary resource traversal are also exact.  This permits a
byte-faithful implementation behind a validation flag; ordinary hardware
depth through `b8/b9` remains the fallback.  The saved FBO render-to-texture
capture closes ordinary producer/consumer ordering.
The static pieces suffice for one linear RGBA8 FBO.  Four-target exposure still
requires a unique output per attachment, completeness failures, partial masks,
and readback.

Occlusion query create/begin/end/result paths and the relocation-backed
snapshot are statically exact.  Counter visibility, availability ordering,
slot reuse/wrap, 32-bit wrap and multisample scaling are inherently dynamic
and remain experimental.

The Windows backend accepts more state than the live Linux ICD advertises.
Examples include RT sRGB, float/YUV/DXT formats, anisotropy and multisample
state.  Registry `PixelShader=0x300`, `VertexShader=0x300`, `ShaderOpt=0x23`,
`Queries=3` and `AAMode=0x80000842` values are configuration inputs, not caps.
No static consumer has been joined which turns those defaults into a complete
OpenGL capability mask.  The authoritative exposure policy is therefore the
intersection of:

1. the live extension/limit inventory;
2. a reachable M9 packer/emitter;
3. a complete allocation, synchronization and result contract;
4. a new-driver replay test.

This explains apparent D3D/OpenGL differences without assuming hidden hardware
support or deliberately disabled hardware.  A state handler proves that the
Windows frontend can carry the state; the public cap still depends on format,
surface and result behavior.

## Exact remaining live experiments

| gap | why static analysis stops | minimal experiment |
|---|---|---|
| built-in native routes | compiler semantic IDs stop before packed stage/result ownership | one isolated result fixture per row in the built-in table |
| int/bool conversions and arrays | frontend types and updater exist, final lowering/range behavior is data dependent | vertex and fragment sentinels, partial array update, nested index and relink |
| shader limits | public values and private allocator bounds differ | compile/link at each advertised `N` and `N+1` |
| all logic results | all frontend codes are exact; only XOR reaches a saved final packet/result | 16 destination/source sentinel draws with full state/program delta |
| polygon offset | raw factor and units are exact; generated equation and depth quantization are not | positive/negative factor and units on sloped and coplanar primitives |
| sampler sRGB | RT write path exists; mapped sampler state 29 is live, stored and queryable, but no consumer of its `+0x46` bit 5 is joined in the pinned final lowerers; state 11 is only a TEXCOORDINDEX negative control | retain explicit shader lowering as the implementable path unless a later derived-record consumer is found; validate decode and filtering with off/on result fixtures |
| point inputs/results | PointCoord absent statically; point setup exists | compile acceptance plus two sizes and coordinate-colored point sprite |
| MRT/query/MSAA | packet structure exists; results depend on allocation and counters | four-target readback, visible/hidden query, sampled edge/mask/resolve |
| special shader native forms | DDX/DDY, seven-mask KIL, nine-slot TEXLDD, ten metadata and eight shadow-compare forms are native-checked; B/P/L linked resources, unrecorded predicates, semantic control-flow placement and pair-byte serialization remain | construct one bounded new linked pair, then validate LOD/bias/projection/vertex-texture, predicate, branch/loop and stop behavior |

These are evidence boundaries, not reasons to delay the ordinary OpenGL 1.x
path or the guarded canonical OpenGL 2.0 forms already captured.  Unsupported
forms should fail compilation or remain unadvertised until their row closes.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
