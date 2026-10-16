# M9 OpenGL 2.0 compatibility fixed-function static closure

This audit closes the fixed-function state which was underrepresented in the
M9 OpenGL 2.0 audit.  It uses the saved M-Series Windows ICD and miniport, the
independent Linux binary, and the normalized live captures already in this
tree.  It does not borrow register meanings from the older Parhelia 0x0528
driver: M9 has a generated-program path and a different texture architecture.

Reproduce the binary and saved-live checks from the repository root:

```sh
python3 tools/m9-fixed-function-static-evidence.py
python3 tools/m9-fixed-function-static-evidence.py --json
```

The verifier pins these inputs:

| image | SHA-256 |
|---|---|
| `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| Linux X 1.15 `m9x_drv.so` | `15ff4ad6e450c2d5a27c3d114011014e8aaf97851d107a070f31baa26eafc7e5` |

Addresses are preferred-image virtual addresses.  `[BIN]` means bytes or data
flow in a pinned binary, `[STR]` is weaker diagnostic/name evidence, `[LIVE]`
is an existing saved capture or result, and `[BOUNDARY]` names a fact which
static analysis cannot establish safely.

## Architecture and implementation split

The M9 vendor stack has three different fixed-function mechanisms.  They must
remain separate in a new driver.

| mechanism | evidence | appropriate new-driver route |
|---|---|---|
| direct raster/depth/stencil state | MxMD render-state handlers and normalized command writes | emit the proven packet fields directly |
| transform, lighting, texgen, fog, points, texture environment and some alpha/blend work | complete GL compatibility-state dictionary, generated compiler inputs, native program linker and resource uploads | lower Mesa fixed function to the existing M9 vertex/fragment compiler |
| selection, feedback, evaluator tessellation and exceptional primitive/pixel cases | API/callback code and explicit software T&L/clipping paths | execute API semantics in Mesa/Draw, then submit ordinary generated vertices where useful |

The Windows ICD names distinct `GLSendStateHardwareTnL`,
`GLSendStateSoftwareTnL`, `GLPrimitiveSoftwareClipping`, and
`GLPrimitiveSoftwareVertexFlat` implementations [STR].  This is direct proof
that a recognized state is not automatically a hardware path.  Conversely,
the named fixed-state constants and shader compiler do not mean a new native
opcode exists for every GL operation.  The safe implementation is fixed-state
lowering through the proven compiler and packet backend, with Draw as a
semantic fallback.

The MxMD routine at `0x1454a0` builds a 57-entry fixed fragment database used
by vertex-declaration conversion.  It is not a library of transform, lighting,
or texture-environment programs.  Assigning those fragments to GL fixed state
would be an unsupported shortcut.

## Complete transform and lighting state

### Matrices and transforms

DP2 opcode 36 is installed at `0xdd9fb` and enters `0xe0d30`.  Each input
record is exactly `0x44` bytes: a 32-bit transform selector followed by sixteen
floats.  It calls `0x109c60`, whose selector table is at `0x109db4` and whose
target table is at `0x109d98` [BIN].

| transform selectors | destination class | dirty state |
|---|---|---|
| `1`, `0x100..0x11f` | world matrix `n`, 64-byte stride | group `0x08`, per-world mask, serial `+0x48` |
| `2` | view matrix | group `0x10`, serial `+0x4c` |
| `3` | projection matrix | group `0x20`, serial `+0x50` |
| `4..6` | legacy world matrices 1..3 | same world group/mask |
| `7` | distinct matrix pointer at transform state `+0x0c` | group `0x40`, serial `+0x54`; public semantic remains unnamed |
| `16..23` | eight texture matrices, 64-byte stride | group `0x80`, serial `+0x58` |
| `8..15` | rejected/default route | no matrix mutation |

The GL compiler state dictionary at `0x10570670..0x10572050` independently
creates model-view, projection, MVP, texture and program matrices, including
inverse, transpose, and inverse-transpose forms [BIN].  The compiler preamble
also exposes `gl_NormalMatrix` and `gl_NormalScale`.  This closes the values
needed by vertex-program lowering.  It does not identify a separate matrix
packet; final fixed GL transforms use the generated program and constant
resource path.

### Material, color material, normalization and lights

DP2 opcode 33 enters `0xdfab0`.  Its count advances by `count * 68` bytes and
copies one exact D3DMATERIAL7 to `p251c+0x18`, then sets dirty bit `0x200`
[BIN].  The GL state dictionary has front and back ambient, diffuse, specular,
emission, and shininess records.  Front and back are therefore independent
compiler inputs rather than aliases.

The direct D3D render-state shadow has these exact fields [BIN]:

| state | shadow |
|---|---|
| `LIGHTING` | `p251c+0xc0` bit 0; dirty group `0x01` |
| `AMBIENT` | RGBA floats at `p251c+0x08..+0x14`; bit 6 marks zero ambient; dirty `0x04` |
| `COLORVERTEX` | `p251c+0xc0` bit 12; dirty `0x100` |
| `LOCALVIEWER` | bit 7; dirty `0x08` |
| `NORMALIZENORMALS` | bit 8; dirty `0x10` |
| diffuse material source | bits `16:15`; dirty `0x100` |
| specular material source | bits `18:17`; dirty `0x100` |
| ambient material source | bits `20:19`; dirty `0x100` |
| emissive material source | bits `22:21`; dirty `0x100` |
| vertex blend | raw value at `p2518+0x1d0`; dirty `0x800` |
| indexed vertex blend | `p2518+0x1a8` bit 0; dirty `0x800` |
| tween factor | float at `p2518+0x1a4`; dirty `0x800` |

DP2 opcode 34 enters `0xdfb00`.  Its eight-byte header is `{index,type}`:
type 0 enables through `0x1082f0`, type 1 disables through `0x1084a0`, and
type 2 consumes a following `0x68`-byte D3DLIGHT7 and calls `0x108100`
[BIN].  The allocated internal light record is `0x80` bytes:

| internal bytes | source |
|---|---|
| `+0x00` | light type |
| `+0x04..+0x30` | diffuse, specular, ambient RGBA |
| `+0x34..+0x40` | position xyz plus generated `w=1` |
| `+0x44..+0x50` | direction xyz plus generated `w=1` |
| `+0x54,+0x58` | range, falloff |
| `+0x5c,+0x60,+0x64` | constant, linear, quadratic attenuation |
| `+0x68,+0x6c` | theta, phi |
| `+0x70,+0x74,+0x78,+0x7c` | dirty groups, version, ID, derived classification |

The active record vector is bounded at 16 by `cmp count,0x10` at `0x108443`.
D3D advertises 12 active lights, while the saved GL stack reports eight.  The
GL compiler dictionary creates ambient, diffuse, specular, position,
attenuation, spot direction, and half-vector records for eight lights, plus
front/back light products and light-model ambient/front/back scene colors
[BIN, LIVE].  The new GL driver should expose eight until a boundary test
proves a larger GL-compatible allocation.

There is no D3D rescale-normal state.  `GL_EXT_rescale_normal` is advertised
by both saved GL binaries, and `gl_NormalScale` is a compiler built-in [BIN].
Rescaling is therefore generated vertex-program work.  Normalization may be
represented by the D3D shadow or generated code, but a from-scratch Mesa path
should use one shader lowering for both so that normal matrices, rescale,
normalize, color material and two-sided lighting share one state key.

Two-sided GL lighting is supported at the compiler-input level by distinct
front/back materials, products, scene colors, primary colors and secondary
colors.  `GL_EXT_separate_specular_color` is advertised [BIN].
`D3DRS_SPECULARENABLE` writes `p2520+0x18` bit 1, but that D3D enable is not by
itself the GL separate-specular semantic.  The exact front/back selection and
secondary-color native program are still a generated-program boundary.

## Texgen, texture matrices, fog, clip planes and points

The GL texgen validator at `0x100c1340` accepts coordinates S/T/R/Q
(`0x2000..0x2003`) and the following mode matrix [BIN]:

| mode | legal generated coordinates |
|---|---|
| `GL_EYE_LINEAR` (`0x2400`) | S, T, R, Q |
| `GL_OBJECT_LINEAR` (`0x2401`) | S, T, R, Q |
| `GL_SPHERE_MAP` (`0x2402`) | S, T |
| `GL_NORMAL_MAP` (`0x8511`) | S, T, R |
| `GL_REFLECTION_MAP` (`0x8512`) | S, T, R |

The compiler dictionary creates eye and object planes for all four coordinates
on each of eight logical texture-coordinate sets.  The ICD also advertises
`GL_NV_texgen_reflection`.  Together with the eight texture-matrix records,
this is sufficient to implement texgen in the generated vertex program.
There is no closed direct texgen packet.

Fog state is exact in the D3D shadow [BIN]:

| state | shadow |
|---|---|
| enable | `p2510+0x2c` bit 0 |
| range fog | bit 1 |
| table mode | bits `4:3` |
| vertex mode | bits `6:5` |
| color | `p2510+0x18` |
| start, end, density | floats at `+0x1c,+0x20,+0x24` |

The GL dictionary adds `fog_color` and `fog_params`, and the compiler preamble
has the standard color, density, start, end, and scale members.  Both saved GL
binaries advertise fog coordinates.  Linear, exponential, exponential-squared,
vertex and explicit-coordinate variants should lower through one generated
fragment/vertex state key.  The native generated programs for each variant
have not yet been joined to those fields.

DP2 opcode 44 enters `0xdfbb0` with 20-byte `{index,vec4}` records and calls
`0x109a00`.  Indices must be below six; the plane is copied at 16-byte stride
from `p2518+0x84`, the version at `+0x7c` increments, and dirty bit `0x1000`
is set.  All-zero planes also update an internal active mask.  D3D clip-plane
enable state 152 uses `0x10cf30`; D3D clipping state 136 writes `p2528+4` bit
3.  The GL dictionary creates six `clip_plane_N` constants and saved GL reports
six planes [BIN, LIVE].  Because the ICD has an explicit software-clipping
class, the static record does not prove every user-plane case stays on the GPU.

Point state is also exact [BIN]:

| state | shadow |
|---|---|
| size, minimum, maximum | floats at `p2528+0x08,+0x0c,+0x10` |
| attenuation A, B, C | floats at `+0x14,+0x18,+0x1c` |
| point sprite | `p2528+0x48` bit 8 |
| point scale | `p2528+0x48` bit 10 |

`point_size` and `point_attenuation` are compiler constants, and point
parameters and point sprites are advertised.  Saved limits are aliased
`1..2048` and smooth `0..32`; live GL_POINTS uses extent word `f4=0x20` for
the captured size-32 case.  Point attenuation, fade threshold, sprite
coordinate replacement and smooth coverage still need visible results joined
to generated state.  The pinned GLSL compilers lack `gl_PointCoord`, so fixed
point-sprite support must not be used as proof of the GLSL built-in.

## Fixed texture environment and DOT3

MxMD's texture-stage dispatcher is called at `0xd0976`.  `0x105340` selects
one of 28 logical stage records from `ctx+0x2520+0x3c` with a `0x60` stride.
Its exact public state map is [BIN]:

| TSS | handler/action |
|---:|---|
| 1, 4 | color/alpha operation |
| 2, 3, 5, 6 | color/alpha ARG1 and ARG2 |
| 7..10 | bump matrix 00, 01, 10, 11 |
| 11 | texture coordinate index and generation kind |
| 12..21, 25 | delegate to sampler-state dispatcher `0x105850` |
| 22, 23 | bump luminance scale and offset |
| 24 | texture-transform flags |
| 26, 27 | color/alpha ARG0 |
| 28 | result argument |
| 32 | constant color |

Each record has this exact semantic shadow [BIN]:

| bytes/bits | meaning |
|---|---|
| `+0x30[4:0]` | color operation |
| `+0x30[10:5],[16:11],[22:17]` | color ARG0, ARG1, ARG2 |
| `+0x30[25:23]` | result argument |
| `+0x34[4:0]` | alpha operation |
| `+0x34[10:5],[16:11],[22:17]` | alpha ARG0, ARG1, ARG2 |
| `+0x08,+0x10,+0x0c,+0x14` | bump matrix 00, 01, 10, 11 floats |
| `+0x18,+0x1c` | bump luminance scale, offset floats |
| `+0x20..+0x2c` | constant expanded to R, G, B, A floats divided by 255 |
| `+0x38[2:0]` | texture transform coordinate count |
| `+0x38[3]` | projected transform |
| `+0x38[6:4]` | generated-coordinate kind |

The low three bits of each texture-coordinate source are also packed into a
three-bit-per-stage 64-bit table at `p2520+0x48` by `0x109850`.

The GL ICD keeps its separate per-unit compatibility shadow at unit stride
`0xa444` [BIN]:

| field | packed bits |
|---|---|
| environment mode | `unit+0xa434[2:0]` |
| RGB / alpha combine operation | `unit+0xa434[6:3]` / `[9:7]` |
| RGB sources 0..2 | `unit+0xa438[2:0],[5:3],[8:6]` |
| RGB operands 0..2 | `unit+0xa438[10:9],[12:11],[14:13]` |
| alpha sources 0..2 | `unit+0xa438[17:15],[20:18],[23:21]` |
| alpha operands 0..2 | `unit+0xa438[24],[25],[26]` |
| RGB / alpha scale | `unit+0xa438[28:27]` / `[30:29]` |
| environment color | `unit+0xa388..+0xa394` RGBA floats |

Getter `0x100aaa70` decodes the legacy modes MODULATE, DECAL, BLEND, ADD,
COMBINE and REPLACE.  It decodes combine operations MODULATE, ADD,
ADD_SIGNED, INTERPOLATE, SUBTRACT, DOT3_RGB and DOT3_RGBA, including the EXT
DOT3 enum spellings.  The pname selector at `0x100ab284` covers
`COMBINE_ALPHA`, RGB scale, `SOURCE0..2_RGB`, `SOURCE0..2_ALPHA`,
`OPERAND0..2_RGB`, and `OPERAND0..2_ALPHA`; alpha scale has its legacy getter
route.  The driver advertises add, combine, DOT3, and crossbar in both saved GL
binaries [BIN].

D3D `TextureOpCaps=0x03feffff` independently includes every defined D3DTOP
value 1 through 26: DISABLE, both SELECTARG operations, MODULATE/2X/4X, ADD,
ADDSIGNED/2X, SUBTRACT, ADDSMOOTH, all diffuse/texture/factor/current-alpha
blend forms, PREMODULATE, the four modulate-add forms, both bump-map forms,
DOTPRODUCT3, MULTIPLYADD, and LERP [BIN].

These two complete semantic shadows prove that Mesa can represent the entire
fixed texture environment before lowering.  They do not close the GL-shadow
to D3DTOP mapping or the final generated native fragment program.  Crossbar is
especially sensitive: the GL source fields are only three bits and the final
physical sampler assignment must be observed rather than inferred.  M9 has
four complete fragment sampler descriptor blocks even though the D3D cap says
eight blend stages/textures; saved GL correctly reports four fixed texture
units and eight texture coordinates.  A fifth logical fragment sampler was
observed lowering by multipass in the current capture set.  The from-scratch
driver should keep four hardware fragment samplers and use an explicit
multipass or rejection policy outside that bound.

## Rasterization and fragment operations

The direct and generated portions can now be classified without guessing:

| feature | recovered path | remaining boundary |
|---|---|---|
| flat shade | saved direct draw has `22=0x00006503` and bit 31 in two index-23 words; ICD has software-flat vertex path | provoking-vertex convention and clipped primitive cases |
| polygon fill/line/point | direct index-22 topology and index-f0/f4 extent words captured | exact edge/diamond coverage rules |
| wide lines | width 3 captured with `f0=0xcc008400,f4=3` | fractional widths and X/Y-major coverage |
| line smooth | saved line case adds `ef=0x60000000` | coverage/alpha correctness and interaction with stipple |
| line stipple | GL API and warning exist, but D3D rejects `LINEPATTERN` and `STIPPLEENABLE` | exact generated/texture state and repeat/phase |
| polygon stipple | ICD says line and polygon stipple consume an available texture unit | 32x32 bit order, origin, clipping and sampler reservation |
| polygon offset | enable produces `ef=0x04000000` | numeric factor/units/clamp equation and fill/line/point isolation |
| point smooth | point coverage changes index c1/ef in saved cases | exact subpixel coverage and alpha result |

The MxMD state map explicitly rejects `LINEPATTERN`, `STIPPLEENABLE`,
`EDGEANTIALIAS`, and `ANTIALIASEDLINEENABLE`, although 32 independent
stipple-row setters at `0x10475d..0x104951` store a pattern [BIN].  The GL
paths are separately gated by `OGL\WideLines`, `OGL\SmoothWideLines`, and
`OGL\LineStipple`, whose saved defaults are zero.  Those dormant paths must
remain experimental until captures establish their final resources and
coverage.

Alpha test, blend, logic operation, dithering and color masks divide as follows:

| feature | exact closure |
|---|---|
| alpha test | lowerer `0x170800`; compare selector and alpha reference are generated fragment-program state; saved resource carries `ref*256` |
| blend | per-render-target lowerer `0x170ff0`; all D3D factors 1..21 and min/max/add/subtract/reverse-subtract branches recovered; constant color expands to A,R,G,B inputs |
| separate alpha blend | direct enable reaches index-88 bit 7; RGB/alpha factors and equations still join through generated program/resource state |
| logic op | all sixteen GL enums map to exact internal values; XOR has saved `88=0x01180000,ef=0x000000aa`; the other final codes need result/delta captures |
| color mask | compatible formats repeat R/G/B/A as `4444/2222/1111/8888` into index-ab low 16; incompatible cases enter compiler fallback `0x173ee0` |
| dither | state 26 writes `p2514+0x0c` bit 0 and dirties group 2; no final packet or visible result is isolated |

Depth, stencil, cull, scissor, shade, fill, color mask, direct blend enable and
polygon topology already have normalized command evidence in
`parhelia-m9-fixed-state-capture.md`.  Their state belongs to each Gallium
context.  A context switch must re-emit all dirty direct state and bind the
correct generated program/constants; sharing one vendor-style global shadow
between GL clients would violate state isolation.

## Pixels, evaluators, selection, feedback and display lists

The ICD exposes genuinely accelerated pixel routes, with explicit exclusion
conditions [STR]:

| API family | accelerated evidence | forced alternate path |
|---|---|---|
| DrawPixels | `FastDrawPixels` uses a secondary C3D context and allocated/locked resources | active query, texturing, GLSL, unsupported or out-of-range transfer state, allocation/lock failure |
| depth/stencil DrawPixels | `DrawPixelsDepthStencilFromTexture` allocates and locks a texture resource | allocation/lock failure |
| ReadPixels | `FastReadPixels` calls the driver | clone mirror, required conversion, non-32-bit source, nonidentity transfer |
| CopyPixels color | `FastCopyPixelsColor` allocates a resource | query, texturing, GLSL, transfer, overlap or allocation failure |
| Bitmap | separate fast path | active shaders/texturing and multimonitor cases |

Compiler constructor `0x1068abd7..0x1068ac40` registers exact internal
pack/unpack operations `itof=117`, `ftoi=118`, `skipPixels=119`,
`readInput=120`, `writePixel=121`, `bitmapLSB=122`, `bitmapMSB=123`,
`writeOutput=124`, and `readPixel=125` [BIN].  This proves that some general
pixel transfer is shader driven; it does not prove all rejected fast cases run
on the CPU.  Pixel zoom has a public frontend, but its coordinate transform,
negative zoom orientation, clipping and final draw path are unjoined.

Display-list state and callbacks are CPU-side, while
`Mesh::MakeResident` and `Execute DisplayList as Resident` prove that compiled
geometry can be stored as a resident GPU mesh [STR].  The ICD explicitly does
not support three 3-D texture update calls inside a display list.  A new Mesa
driver should rely on Mesa display-list semantics and cache ordinary driver
draws only when useful.

Selection, feedback and evaluator entrypoints are present.  The evaluator code
has explicit frontend checks and rejects MAP1_INDEX and MAP2_INDEX [STR].  No
M9 command class or render-state record for selection names, feedback tokens,
or evaluator polynomials was found.  These are API semantics, so Mesa/Draw
should evaluate/tessellate and produce selection/feedback results on the CPU.
Resulting render-mode geometry can still use the ordinary GPU vertex path.
Saved limits are evaluator order 8, display-list nesting 64, name stack 64 and
pixel-map table 256 [LIVE].

## Smallest remaining conformance capture set

Static work has reached the point where more broad disassembly adds names but
cannot prove visible GL results.  Five normalized fixtures close the remaining
hardware joins with little target time:

1. `m9-ff-tnl`: one submission per state delta and a small readback grid for
   directional/point/spot light, local viewer, normal/rescale, color material,
   two-sided and separate specular, flat provoking vertex, one user clip plane,
   all five texgen modes, and a nonidentity texture matrix.  Save command and
   selected `0x21000` resource after every delta.
2. `m9-ff-texenv`: four colored 2x2 textures and deterministic primary,
   constant and previous colors.  Step through every legacy mode, combine
   operation, ARG0/1/2 complement/alpha operand, 1x/2x/4x scale, DOT3,
   bump-map state and crossbar source.  Save both output pixels and native
   resource/program deltas.
3. `m9-ff-raster`: one horizontal, vertical and diagonal primitive per wide,
   smooth and stippled-line case; one polygon stipple; positive and negative
   polygon offset; dither on/off into a low-bit target; both winding orders for
   flat shade.  Save the target, not only command words.
4. `m9-ff-pixels`: Draw/Copy/Read/Bitmap with identity transfer, one supported
   conversion, overlap, positive/fractional/negative pixel zoom and one
   depth/stencil DrawPixels.  Record which fast-path diagnostic fires and save
   command/resource/output.
5. `m9-ff-software-semantics`: result-only evaluator, selection, feedback and
   display-list cases.  This needs no packet discovery; it verifies Mesa's CPU
   semantics and that emitted geometry does not corrupt subsequent GPU state.

The first fixture should be split only if the generated vertex resource cannot
be snapshotted between deltas.  Each fixture must use non-symmetric values so
transpose, front/back, source ordering and component swizzles cannot pass by
accident.  Run every GPU fixture interleaved between two contexts to make state
isolation part of the same evidence rather than a later repair.

Until those fixtures pass, the safe advertised implementation is: eight GL
lights, six clip planes, four fixed fragment texture units, eight texture
coordinate sets, generated GPU programs for ordinary fixed transform/lighting/
fog/texgen/texture environment, direct packets for already captured raster and
fragment state, and Mesa/Draw for semantic or explicitly exceptional paths.
