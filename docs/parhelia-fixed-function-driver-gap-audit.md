# Parhelia fixed-function hardware gap audit

This audit compares the saved vendor Linux/Windows implementation with the
Mesa Parhelia Gallium driver after Mesa commit `e4a6857`.  It separates state
that already runs on the GPU, state currently evaluated by Gallium's software
draw frontend, and hardware paths whose packet or WARP details are now exact
enough to implement.  It does not infer support from extension names alone.

The saved vendor binaries and captures establish two distinct programmable
blocks:

* WARP transforms vertices, evaluates fixed-function lighting, fog, texgen,
  point parameters, and user clip distances, then writes rasterizer inputs.
* The fragment block has four texture-address stages and five ALU instruction
  slots.  Fixed-function texture environments are compiled into this block;
  they are not a separate legacy blend unit.

The open driver currently runs the vertex shader through Gallium `draw`, so
transformed and clipped window-space vertices reach `ph_stage_tri()` and
`ph_stage_line()`.  Production draws bind one of four small WARP passthrough
programs (`warp_passthrough`, `warp_textured`, `warp_textured_multitex`, or
`warp_replace`).  This can provide correct software transform, lighting,
texgen, and clipping, but it does not use the vendor's native WARP variants.
The fragment-side NIR recognizer is considerably narrower and can silently
select a simpler texture environment for algebra it has not classified.

## Current support versus vendor hardware

| GL state | Vendor implementation | Current Mesa path | Remaining work |
|---|---|---|---|
| Single texture REPLACE/MODULATE/DECAL | Fragment ALU program | Exact narrow NIR shapes select native ALU state | Match complete expression dependencies, formats, alpha, and constants |
| Two texture MODULATE/ADD and unit-1 REPLACE | Serial fragment ALU program | Exact instruction-count shapes select native state | Replace count-only matching with data-flow matching |
| Texture env ADD/BLEND | Native fragment ALU | Packet emitter has encodings, but single-unit NIR selection and environment-color plumbing are incomplete | Add exact NIR patterns and upload `c0` |
| COMBINE and DOT3 | Native fragment ALU compiler | Missing | Add a small fixed-function fragment compiler |
| Fog | WARP fog fragment plus `0x12d4..0x12dc` and a lookup table | Raster fog registers are forced off; no native WARP fog variant | Preserve software fog output or link a native fog variant |
| Lighting | Generated WARP program, with directional/point/spot loops and material epilogues | Gallium draw frontend evaluates the vertex shader; only a diagnostic one-light WARP program exists | Build a legal clean-room WARP linker/compiler and cache variants |
| Texgen and texture matrices | Generated WARP fragments per unit and coordinate | Gallium draw frontend | Link native texgen fragments after the WARP compiler exists |
| User clip planes | Six WARP plane fragments and six constant records | Gallium draw clips in software through `draw_set_clip_state()` | Keep software clipping until the plane constant consumer and output descriptor are closed |
| Aliased unit-width GL_LINES | Native line primitive | Native as of `e4a6857` for untextured, unsmoothed, rounded-to-one width | Add textured/smooth/wide line paths |
| Points | Native point primitive plus point-size WARP/raster state | Point callback discards | Implement only after point output and size state are tested together |
| Alpha test | `0x12e4` | Native | Retain exact reference rounding and format behavior |
| Depth test/write | `0x12c8`, `0x12d0` | Native | No fixed-function gap for the represented depth state |
| Stencil | `0x12c8..0x12d0` | State is neither represented nor emitted; stencil reference callback is a no-op | Add exact fields and tests; packet encoding is complete below |
| Scissor | `0x1328`, `0x132c`, DWGCTL bit 23 | Callback is a no-op | Store scissor and emit inclusive-min/exclusive-max state |
| Blend factors/equation/constants | Native blender | Common factors and shared equation exist in the reference builder; context selection and constants are incomplete | Map equations and constant factors, store blend color |
| Polygon mode/offset, smooth lines/points | Native raster and/or vendor CPU expansion | No complete native path | Add after the core Quake 3 fixed-function subset |

## Fragment texture environment and DOT3

The vendor texture-environment state word is fully decoded.  Its low three
bits select MODULATE=1, DECAL=2, BLEND=3, ADD=4, REPLACE=5, COMBINE=6.
COMBINE RGB uses REPLACE=1, MODULATE=2, ADD=3, ADD_SIGNED=4, SUBTRACT=5,
INTERPOLATE=6, DOT3_RGB=7, DOT3_RGBA=8, DOT3_RGB_EXT=9, and
DOT3_RGBA_EXT=10.  RGB sources are TEXTURE=0, CONSTANT=1, PRIMARY=2, and
PREVIOUS=3; operands are SRC_COLOR=0, ONE_MINUS_SRC_COLOR=1, SRC_ALPHA=2,
and ONE_MINUS_SRC_ALPHA=3.  Alpha has the same four sources, six combine
operations through INTERPOLATE, and non-inverted/inverted alpha operands.

The hardware compiler maps sources as follows:

| GL source | Fragment register code |
|---|---:|
| texture for the active unit | 0..3 |
| environment constant `c0` | `0x0e` |
| primary color `v0` | `0x10` |
| previous result `r0` | 4; first stage uses `v0` |

The exact ALU operation fields are MOV=`0x0400`, ADD=`0x0c00`,
SUB=`0x0600`, MUL=`0x0000`, DP3=`0x1000`, DP4=`0x2000`, MAD=`0x0800`,
LRP=`0x0a00`, CND=`0x4c00`, CMV=`0x5c00`, and CMP=`0x7c00`.  Ordinary and
inverted source modifiers are 0 and `0x0f`.  ADD_SIGNED rewrites source zero
to bias=2 or bias-negate=3.  DOT3 rewrites both operands to signed=4 or
signed-negate=5, which implements `2*x-1`.  DOT3_RGBA makes the instruction
write all four channels.  EXT DOT3 forces scale x1; the ARB forms honor the
requested x1/x2/x4 scale, encoded as 0/1/2 in `0x126c[19:17]`.

The vendor emits these per-unit programs:

* REPLACE uses MOV, preserving prior RGB or alpha when the texture base format
  does not supply that component.
* MODULATE uses MUL for RGBA, with a split RGB MOV/alpha MUL for alpha-only
  textures.
* DECAL uses `LRP(texture, previous, texture.a)` and preserves prior alpha.
* BLEND uses `LRP(c0, previous, texture)` and usually multiplies alpha.
* ADD adds RGB and usually multiplies alpha.
* COMBINE builds separate RGB and alpha instructions from the decoded sources,
  modifiers, replication, and scale.

`p3d_emit_texenv()` already emits the five non-COMBINE families, including
environment-color registers, but `ph_nir_texenv()` recognizes only one sample
plus exact REPLACE/MODULATE/DECAL instruction counts.  `ph_nir_serial_op()`
recognizes two-unit MODULATE/ADD in the same count-based way.  Counting ALU
instructions is unsafe for fog, alpha fixups, or any shader containing unrelated
arithmetic because it does not prove that the exported color depends on the
matched operations.

An actionable implementation is a small data-flow compiler for the fixed GL1
NIR subset:

1. Start at the color output and recursively match texture samples, primary
   color, constants, multiply, add, subtract, and `flrp`; reject any unmatched
   input or side effect.
2. Preserve RGB and alpha dependencies separately.  Include texture base format
   in the key so the GL rules above select MOV versus the alpha operation.
3. Lower each enabled unit to one RGB and one alpha record with the exact source
   modifiers and channel replication.  Expose a list of records rather than
   forcing the result back into `enum p3d_texenv`.
4. Upload each referenced environment color to `c0`, encode the x1/x2/x4 scale,
   and pad to the hardware's five-instruction quantum.
5. Add byte-exact tests for every mode, all four operand forms, ADD_SIGNED,
   DOT3_RGB/RGBA and ARB/EXT scale behavior.  Add rejection tests where the
   final color has extra NIR arithmetic.

The vendor does not advertise texture-env crossbar and rejects `GL_TEXTUREn` as
a COMBINE source.  The open compiler should keep the same restriction until an
independent hardware result proves otherwise.

## Fog

The raster fog registers are exact:

* `0x12d4`: enable bit 0, mode in bits 2:1, auxiliary flag bit 4, and packed
  B/G/R color in bits 13:6, 22:15, and 31:24.
* `0x12d8` and `0x12dc`: 24-bit fixed values derived from fog parameters.
* The table upload is 64 dwords carried by sixteen `0xb8b8b8b8` records through
  the `0x12e0` table port.

The WARP registry contains fog fragments `0x58..0x64`.  The generator uses
mode 3 for LINEAR, 1 for EXP, 2 for EXP2, and 0 for pass-through.  Variants
select radial versus depth fog and computed versus explicit fog coordinates;
the explicit form reads `r11.x`, and parameter registers include `r116/r117`.
Current `p3d_emit_state()` explicitly writes `0x12d4=0`, `0x12d8=0`, and
`0x12dc=0x00ffffff`.

There are two valid implementation strategies.  The smaller one keeps fog in
Gallium's software vertex/fragment lowering and makes the fragment classifier
prove that the final output includes that fog result before selecting hardware
texenv.  The faster path adds fog to a clean-room WARP program linker, uploads
the parameters/table, and sets the raster fog state.  Mixing a native texenv
program with an ignored software fog expression produces the wrong color and
must be rejected.

## Lighting, texgen, and clip planes

The vendor fixed-function WARP generator links a transform body, fog, texture
coordinates, point size, user clip planes, lighting, and an output terminator.
The state key contains transform mode, counts of spot/point/directional lights,
the enabled user-plane count, and the two-sided-lighting flag.

The decoded registry is broad enough to guide a clean-room implementation:

* Fog is `0x58..0x64`.
* Texgen/texture-matrix fragments are `0x6f..0xa6`, with default-fill/output
  fragments at `0xa7..0xaf`.  Four 28-entry tables select raw texcoord, object
  position, eye position, or reflection/sphere vector and optional dp4 against
  texture-matrix rows `r89..r108`.
* One through six clip-plane prefixes are `0xb0..0xb5`, followed by `0xb6`.
  They dot transformed position `r2` with constants `r12..r17`.
* Directional-light fragments are `0x16b..0x182`, point lights
  `0x183..0x1a6`, spot lights `0x1a7..0x1d6`, and material/color epilogues
  `0x1d7..0x21e`.  Each light occupies seven vec4 constants.

This is a larger project than adding register state.  A legal implementation
must encode the documented WARP ISA and reproduce the algorithms; it must not
copy vendor microcode.  Build a linker that chooses clean-room fragments,
patches register/constant operands, validates instruction and register limits,
uploads once, and caches by the complete fixed-function key.  Keep the current
software-draw implementation as the correctness fallback for every rejected
variant.

User clip planes already reach `draw_set_clip_state()`, so software clipping is
the appropriate current behavior.  Native plane records are known, but the
complete plane-constant consumer/output binding has not been verified in the
open submission path.  Emitting raw plane constants before that is closed risks
clipping every primitive.

## Point and line rasterization

Basic primitive words are vendor-proven.  With the open driver's direct-fetch
bit set, points use `0x00060020`, lines use `0x0006103d`, and triangles use
`0x0006305c`.  The matching raster class changes PIPECTL and the low DWGCTL
nibble.  A DRAW relocation spans one, two, or three vertices respectively.

Mesa commit `e4a6857` implements untextured, unsmoothed aliased lines whose
width rounds to one.  Gallium `draw` has already transformed and clipped the
endpoints, so the driver flushes any triangle aggregation, selects P3D_LINES,
uses the window-space passthrough WARP entry, emits one two-vertex `0x97`
packet per segment, and updates color/depth resource generations.  The offline
selftest checks `0x10b0=0x0006103d`, the line PIPECTL class, DWGCTL primitive
nibble 5, and 40-byte relocation ranges for two five-dword vertices.  Gallium
line-strip and line-loop decomposition can feed this callback one segment at a
time; a target regression should confirm the closing loop segment.

The vendor implements wide aliased lines as `round(width)` parallel one-pixel
lines, offset in order `0,-1,+1,-2,+2,...` along the minor axis.  Smooth lines
use perpendicular coverage geometry.  These need a draw-module decomposition
stage or explicit generated triangles/lines; selecting the line primitive alone
does not implement their coverage rules.

Points also require more than the primitive word.  The vendor has point-size
and attenuation constants around block slots `0x172/0x173`, WARP point-size
fragments, and distinct raster/output state.  Implement and hardware-gate that
state as one unit before accepting P3D_POINTS in the generic batch builder.

## Alpha, depth, stencil, and blend

All compare functions share the exact code: NEVER=0, EQUAL=1, LESS=2,
LEQUAL=3, GREATER=4, GEQUAL=5, NOTEQUAL=6, ALWAYS=7.

Alpha test is `0x12e4`: enable bit 0, compare function in bits 3:1, and the
8-bit reference in bits 15:8.  The current open builder emits this state.

Depth and stencil share:

```
0x12c8 bit 0      depth enable
        bits 3:1  depth function
        bit 4     stencil enable
        bits 7:5  stencil function
        bits 10:8 stencil fail operation
        bits 13:11 stencil depth-fail operation
        bits 16:14 stencil pass operation
        bits 20:17 stencil bit count
0x12cc bits 7:0 compare mask, bits 15:8 reference
0x12d0 bits 7:0 write mask, bit 8 depth write enable
```

Stencil operations are KEEP=0, REPLACE=1, INVERT=2, ZERO=3, INCR_WRAP=4,
DECR_WRAP=5, INCR_SAT=6, and DECR_SAT=7.  The open builder currently emits
depth enable/function/write but hardcodes `0x12cc=0xff` and has no stencil
fields.  Adding front-face stencil state, stored stencil reference, masks,
three operations, and stencil bit count is a narrow packet-only change with
byte-exact tests.  Two-sided stencil needs either the hardware face-selection
mechanism or a draw split; do not silently apply front-face state to both faces.

The reference builder already encodes common blend factors and a shared
equation.  The context still needs exact mappings for constant color/alpha and
their inverses, stored `set_blend_color` values, and the requested RGB equation.
The screen currently reports no separate RGB/alpha equation support, consistent
with the shared hardware operation.  MIN/MAX must use canonical factors because
the factor fields are irrelevant to those equations.

## Recommended implementation order

1. Finish stencil, scissor, blend equation, and blend constants.  Their packet
   fields are exact and require no WARP compiler.
2. Target-test the native line path, including strip/loop decomposition, depth,
   alpha test, blending, and clipping; add textured unit lines only after an
   exact fragment/attribute test.
3. Replace the texture-env instruction counters with a strict output data-flow
   matcher, then add ADD/BLEND and COMBINE/DOT3 records.
4. Make fog safe under the strict classifier, then add native fog when the WARP
   linker and table uploader exist.
5. Implement the clean-room WARP linker for lighting, texgen, and clip planes,
   retaining software draw as fallback.
6. Add points, wide/smooth lines, polygon offset/mode, and remaining raster
   variants with isolated packet tests and target gates.

Primary reverse-engineering references are
`parhelia-fragment-isa.md` sections 12.2.3 through 12.2.7,
`parhelia-pixel-pipeline-complete.md` sections 3.3 through 3.6,
`parhelia-warp-fragments.md` sections 3 and 4,
`parhelia-raster-clip.md` sections 9 through 11, and
`parhelia-command-construction.md`'s primitive table.  The open-driver comparison
comes from `parhelia_context.c`, `parhelia_batch.c`, and
`reference/p3d_state.c` at Mesa commit `e4a6857`.
