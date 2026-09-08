# Parhelia 3D pipeline state — consolidated RE (2026-09)

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** The `reg = 0x1000 + slot*4` rule is refuted (`0x12a0` is named in no vendor binary).
> ## STALE IN TWO SPECIFIC PLACES — both would break a draw
>
> This file elevates itself over others ("where the two disagree, this wins")
> while its own appendix admits part of its register list is mis-sliced payload.
> Two concrete hazards:
> * **Do not replay the `0x1100-0x117c = 0xffffffff` "attribute file init"
>   after binding.** `0x1100` is the WARP entry point; replaying the sweep
>   overwrites your entry with a bogus value.
> * **Do not program `0x1298` as draw state.** It is `GL_POINT_SIZE_MAX`, and
>   the address itself is refuted — the 2048.0 is a clamp folded into block
>   `0x173`.


Provenance: reversed from `matroxdriver-x86_64-cvs-20090511/opengl6/libGL.so.1.3.0`
(gl64.asm), the win32 D3D driver `MTXPARD.DLL` (pard.asm), and the Linux X HAL
`mtx_drv.so` (mtx.asm). Complements docs/parhelia-gl-driver.md,
parhelia-fragment-isa.md, parhelia-vertex-isa.md, parhelia-3d-driving.md and the
WARP microcode/draw-submission RE (see [[parhelia-warp-draw-protocol]]).
Tags: [C]=confirmed from disasm, [P]=probable, [CAP]=capture-only.

## 1. Fragment combiner [SUPERSEDED]

**This section was wrong in several fields and has been replaced by
`docs/parhelia-fragment-isa.md` (revision 2026-09-05).** Read that file; do not
use the bit tables that used to be here. Short summary of the corrected model:

- 10 instruction slots = 5 combiner stages x {colour, alpha}; the slot address
  lives in `0x1264[4:0]` and the driver's instruction list maps to slots
  `0,2,4,8,10, 1,3,5,9,11`. Instruction count is padded to 5 or 10.
- `0x1264` = slot[4:0] + **three** source operands, each 5-bit register selector
  + 4-bit modifier, at [13:5], [22:14], [31:23]. `0x07038000` is not a control
  base, it is just "srcB = c1, srcC = c0".
- `0x126c` = three 3-bit channel-replicate fields [2:0]/[5:3]/[8:6], opcode
  [14:9], **result scale** [19:17] as a signed shift (0 x1, 1 x2, 2 x4, 3 x8,
  7 /2, 6 /4 — not a write mask), destination register [23:20], saturate [24].
- `0x1278`-`0x1284` are the two general **constant registers** c0 and c1, four
  signed Q1.10 (x1024) components each — not a dedicated colour matrix.
- `0x1260`/`0x1268` are the **alpha** instruction of the same slot (identical
  layout); stage-flag bit0 = "separate alpha present". Texture-address stages
  are a separate 4-entry descriptor array, and their opcodes are still unknown.
- Opcode bases (`0x126c[14:9]`): MUL 0x0000, MOV 0x0400, SUB 0x0600, MAD 0x0800,
  LRP 0x0a00, ADD 0x0c00, DP3 0x1000, DP4 0x2000, CND 0x4c00, **CMV 0x5c00**,
  CMP 0x7c00. Operand roles: MAD = A*C+B, MUL = A*C, ADD = A+B, SUB = A-B,
  DP3/DP4 = dot(A,C), CND/CMV/CMP select between B and C on A.
- `0x00400803` is MAD with source-A replicate = red, not a "colour-matrix
  multiply" opcode.

## 2. Viewport / scissor — conflict RESOLVED [C]

**Real scissor/viewport clip = 0x1280 / 0x1284** (indices 0xa0/0xa1). libGL
emits them via DMA header 0x2121a1a0 (idx a0→0x1280, a1→0x1284; two 0x21 pads),
two lazy-flush emitters gl64 0x1c0453 / 0x1c4efe. Packing per reg = `(Y<<16)|X`.
Coordinate space = **NDC × ~1024 signed fixed-point**, clamped to guardband
[-1024.0 (0xc4800000), +1023.0 (0x447fc000)]; viewport scale/bias folded in on
the CPU via mulss (ctx+0xa068). NOT screen pixels.

Conflict roots (both earlier claims were context-confusions):
- 0x1280/0x1284 are a **format-multiplexed vertex-parameter register file**:
  clip-rect in the viewport-flush path, packed diffuse colour in the colour-draw
  path (MTXPARD 0x42c86), colour-matrix coeffs in the combiner path (§1). Same
  regs, different context — not a contradiction.
- 0x12c0/0x12c4/0x12c8 = 0xffffffff/0x7fffffff/0 are clip-plane/guardband LIMIT
  constants (max/disabled), not the scissor.

**Separate device/window clip in SCREEN PIXELS = 0x1304 / 0x1308** (Matrox
CXLEFT/CXRIGHT/YTOP/YBOT lineage), packed `(Y<<16)|X`, set on RT/viewport change
(MTXPARD 0x1305xx). [P] this is the coarse pixel scissor; 0x1280/0x1284 the fine
guardband clip. (2D engine also uses 0x1328/0x132C SCISSOR gated by DWGCTL bit23,
per docs/parhelia-2d-accel-complete.md.)

## 3. Guardband / viewport transform / primitives [C]
- Guardband 0x12f4: 31.0 (0x41f80000) untextured/≤2 stages; 15.0 (0x41700000)
  textured/>2 stages; default 32.0 (0x42000000). Distinct from the ±1024 clip
  clamp. (MTXPARD 0x113abf select, 0x1704f4 32/16 alt.)
- Viewport transform + glDepthRange are **CPU-applied** (software T&L, MTXPARD
  0x113729-0x113871 per-vertex fadds/fildl/fstps + matmul 0x10d860). No dedicated
  scale/bias engine registers.
- **Triangle-list is the only HW primitive** (DWGCTL 0x1300 low byte 0x07;
  kernel writes 0xc07, pard 0x307/0x707). Strips/fans/lines/points are
  CPU-decomposed to triangles.

## 4. Constants / matrices [P]
- Generic indexed constant window: reg = 0x1200 + slot*4 (MTXPARD 0x474300,
  type-gated (*obj & 0x70000)==0x10000).
- A 10-register transform/constant block 0x11c0-0x11e4 uploaded from a struct
  (MTXPARD 0x16fe9c-0x16ff33).
- key=0x20 [slot][16 floats] constant packet + MVP slot→register mapping: [CAP].
  Given CPU T&L (§3), the MVP appears CPU-applied; HW fixed-function-T&L with VS
  constants is not proven.

## 5. Texturing / blend / depth / stencil — register map (offsets known;
exact bit encodings partly [CAP])

Known register offsets (from docs/parhelia-gl-driver.md + this push):
- Texture base 0x11dc (VRAM byte offset), control 0x11e4; texture pipes 0x1200-
  0x120c: active 0x00040101, idle 0x00010000 (per stage).
- Colormask 0x12a0 (4-bit RGBA write-enable; 0 blanks output).
- Depth test enable 0x101c; alpha test 0x103c / func 0x1064.
- Blend equation 0x12ac (ADD/SUB/REVSUB/MIN/MAX = 1..5).
- Blend-factor enum→hw code (from parhelia-gl-driver.md): ZERO=1 ONE=2
  SRC_COLOR=3 1-SRC_COLOR=4 SRC_ALPHA=5 1-SRC_ALPHA=6 DST_ALPHA=7 1-DST_ALPHA=8
  DST_COLOR=9 1-DST_COLOR=0xa SRC_ALPHA_SAT=0xb CONST_COLOR=0xe 1-CONST_COLOR=0xf
  CONST_ALPHA=0x10 1-CONST_ALPHA=0x11. Factor bank = WriteBuffer key 0x5, whose
  absolute base is resolved at libGL runtime dispatch — [CAP] the base.
- S8Z24 depth surface base/format = **slot 1, `0x1228`/`0x122c`** (not slot 2),
  format enum **0x36** = 32 bpp. See `parhelia-engine-context-bringup.md` §7.

RESIDUAL (the RE agents for this section died on API errors before deepening):
the concrete texture FORMAT enums (RGBA8/RGB565/… → hw code), texture dim/log2
register + wrap (repeat/clamp/mirror) + min/mag/mip filter bit encodings, and the
D3D render-state → register bit encodings. Strong anchor for the latter, found
before the trace timed out: **a flat render-state array at MTXPARD
stateobj+0x4a4 (53 dwords, zeroed by ctor 0x86760)** — the per-D3DRS values are
written into this array and flushed to the 0x10xx/0x12xx registers; tracing the
per-token writers off 0x86760 is the way to recover the blend/depth/stencil bit
encodings. Left as the next RE step.

## Appendix: LIVE-CAPTURED draw state (ground truth) [CAP]

> **Superseded in part by [[parhelia-gl-feature-map]]** (per-GL-feature sweep,
> same board).  That capture shows the 136KB mapping is a *record* stream, not a
> flat pair stream, so part of the register list below is mis-sliced payload;
> it also contradicts §2 (scissor/viewport), §3 (primitive decomposition, CPU
> viewport transform) and §5 (0x1024, 0x1230).

Everything above is static RE. This appendix is what a **real vendor GL draw
actually programmed**, captured on hardware 2026-09-04 (0x0528, PCI 256MB board,
vendor 2009 stack). Where the two disagree, this wins.

**Platform caveat:** captured on a **PCI/PCI-X** board (no AGP/PCIe capability).
The register protocol is bus-agnostic, the bus addresses are not — see
docs/parhelia-3d-driving.md "Address-space caveat".

### Submission (mmiotrace, captures/mmio-draw-0528*.txt)
Per draw: arm `0x10b0=0x10`, `0x12b8=0x00070601`, `0x300=0x1f0b0140`, `0x384=0`
(DMAPAD, not a soft-trap)
(soft-trap), `0x308=0x83`; submit `0x404` = command-buffer bus address, then
`0x408 = end|3` repeatedly (end advances 0x20/packet); `0x314=1` kicks; poll
`0x8f0`/`0x450`. The **0x91c FIFO carries the WARP microcode ROM once at init
only** — it is NOT the per-draw geometry path.

### Command buffer (captures/cmdbuf-draw-0528-*.bin/-decoded.txt)
Captured by diffing the `/dev/matrox/mtx0` mapping between `glBegin` and
`glFlush`: 1539 bytes changed in the 136KB ring mapping, a 1109-dword span. The
content decodes as a stream of **(register-index, value) dword pairs**,
index = `(reg-0x1000)/4` — the same indexed register file the ring consumes.

State for one Gouraud triangle, first-seen order:

```
0x1034=1  0x1020  0x10d4=1  0x1030  0x105c=2  0x1098  0x126c=0
0x1298=0x45000000            ; 2048.0f  viewport/scale
0x1100-0x117c = 0xffffffff   ; whole vertex/attribute register file initialised
0x1118=1
0x1048=0x3f800000 (1.0f)  0x1200=0x80  0x1004=0x3f800000  0x1014=2  0x104c=1
0x10d8=1  0x1230=1  0x1090=1  0x108c=1  0x1084=0x18  0x1018=2  0x101c=1
0x1024=2  0x1028=2  0x102c=2  0x1088=2  0x109c=1  0x1094=6  0x1080=6  0x1058=1
```

The **`0x1100-0x117c=0xffffffff` attribute-file init** and the **`0x1298` scale**
are the state a from-scratch draw is most likely to be missing.

### Vertex format
`[x, y, r, g, b]` — 2 position floats + 3 colour floats, **stride 20 bytes**.
z and w are pipeline constants (w=1.0 const, z compared against 0.0), not
streamed. See docs/parhelia-vertex-isa.md.

### Per-feature mapping — method
Re-run the same PRE/POST command-buffer diff around each op in the `gltest.c`
sweep (clear, flat tri, gouraud, quad, lines, depth, blend, texture) and diff the
resulting buffers against each other; the delta is that feature's state. This
turns the [C]/[P] encodings above into confirmed values.

## Depth buffer: working, measured 2026-09-08

The Z buffer draws correctly from our own C-generated stream.  Configuration,
all confirmed against the vendor's own 3D draw span (`captures/gl-matrix/
depth-secs.txt`, section `195204ea`, which binds both surfaces):

| register | value | meaning |
|---|---|---|
| `0x1220`/`0x1224` | base, `32020780` | colour target, format 0x32, type 2, pitch in **pixels** |
| `0x1228`/`0x122c` | base, `36020780` | depth surface, format **0x36** (S8Z24), type 2, same pitch |
| `0x12c8` | `0x001000e0 \| func<<1 \| enable` | depth test; LESS = func 2 -> `...e5` |
| `0x12d0` | `0x000000ff \| 0x100` | bit 8 = depth write enable |

Slot 1 is the depth slot; slots 2 and 3 are not.  Binding the Z surface into
slot 2 or 3 leaves the depth unit reading whatever slot 1 last held, which
rejects roughly half the fragments and writes no Z at all - a failure that
looks like a broken compare op rather than a mis-bound surface.

### Measured behaviour

Triangle of ~86500 pixels, depth surface 1024x768 at a base well clear of the
colour target:

| case | result |
|---|---|
| Z cleared far (`0x00ffffff`), LESS, z=0.8 triangle | 86593 px drawn - full coverage |
| Z cleared near (`0x00000000`), LESS, same triangle | 0 px - every fragment rejected |
| Z cleared far, func 5, same triangle | 0 px - func 5 is a GREATER-family op |
| far triangle then near triangle | near fully drawn, far reduced to its unoccluded part |
| near triangle then far triangle | identical near coverage, far rejected in the overlap |

The written Z is the expected `0.8 * 2^24 = 0x00cccccd`, so Z24 is plain
unsigned normalised: far is `0x00ffffff`, near is `0x00000000`.

### Depth writes cover ~850 more pixels than colour writes - SOLVED

Originally measured as 87426 Z pixels against 86577 colour pixels for the same
triangle, and guessed at as an edge rule.  It was neither an edge rule nor a
depth/colour difference: the draw was simply not draining, and the colour path
lost more of the tail than the depth path did.  With the end-of-draw pipeline
drain (see "The missing sliver" below) the two footprints are **identical** -
87500 colour pixels and 87500 Z pixels.

### Surface aliasing corrupts both buffers

The depth surface base is honoured - moving it by 0x400000 moves the written Z
by exactly 0x400000.  But a depth base placed near the colour target through
the same pool aliases onto it, and then each buffer ends up holding a mixture
of colour and Z:

| depth base | colour buffer | depth buffer |
|---|---|---|
| `0x02400000` (near the colour target at `0x02000000`) | 14442 red + 57894 Z | 23475 Z + 2842 red |
| `0x03400000` (well clear) | 86529 red, 45 Z | 86593 Z, 0 red |

Colour pixels appearing inside the depth surface is the diagnostic: it can only
happen if the two overlap.  A driver must allocate the two surfaces from
disjoint ranges; the symptom otherwise reads as a broken depth test.

### Clear

`p3d_emit_clear()` emits the 2D solid fill into the same command stream: bind
the surface into both slot 0 and slot 1, `0x12c0 = 0xffffffff`, `DWGCTL = 0xc0`,
the value in `0x1314`, `0x1304 = 0`, `0x1308` = destination x/y and `0x138c` =
height/width last, which is what fires the blit.  Because the fill overwrites
slots 0 and 1, the clear must be emitted **before** `p3d_emit_state()`.

A depth surface clears exactly like a colour one - the fill is byte-level over
the same tiling.  Round the height up to a multiple of 32: a tiled target only
takes complete 32-row tile rows.

Verified by poisoning the colour buffer green and the Z buffer to near, then
submitting one stream that clears and draws: the green is gone, Z reads back
`0x00ffffff` outside the triangle, and the triangle draws - which it could not
do at all had the depth clear not landed.

## Alpha test and blending, implemented 2026-09-08

Both were decoded from the capture and cross-checked against
`docs/parhelia-blend-alpha-encoding.md`, which derives the same layouts from the
vendor's Linux kernel object and Windows driver.  `src/parhelia3d` now emits
both, and reproduces the vendor's captured register values bit for bit.

### Compare functions are a three-bit mask, shared by depth and alpha

`0x12c8` (depth) and `0x12e4` (alpha) use the same field: bit 0 enables the
test, bits `[3:1]` select it, and the selector is a **mask** - bit 0 EQUAL,
bit 1 LESS, bit 2 GREATER:

| function | code |
|---|---|
| NEVER | 0 |
| EQUAL | 1 |
| LESS | 2 |
| LEQUAL | 3 |
| GREATER | 4 |
| GEQUAL | 5 |
| NOTEQUAL | 6 |
| ALWAYS | 7 |

Three independent captures agree: `GL_LESS` gives `...e5`, `GL_GEQUAL` gives
`...eb` (= GREATER|EQUAL), and `glAlphaFunc(GL_GREATER, 0.5)` gives `0x8009`
(= GREATER, enabled).  A disabled test carries ALWAYS.  This **corrects** the
earlier note that the numbering was D3D's CMPFUNC enum - under D3D numbering 5
would be GREATER, and the mask reading is the one that fits all three captures
and the alpha register at the same time.

`0x12e4` bits `[15:8]` are the alpha reference scaled to 0..255: GL's 0.5 was
captured as `0x80`.

### `0x12e8` BLENDCTL

`[0]` enable, `[4:1]` srcRGB, `[8:5]` dstRGB, `[12:9]` srcAlpha, `[16:13]`
dstAlpha, `[19:17]` and `[22:20]` the blend equation - written twice from one
value, so there is a single equation for both pipes and
`GL_EXT_blend_equation_separate` cannot be exposed.  Bit 23 marks the dither
unit engaged and bit 24 disables it.  Blending is only honoured when DWGCTL's
ROP nibble `[7:4]` is `0xc` (GXcopy); the 3D baseline `0x1300 = 0x00a60ec4`
already satisfies that.

The factor nibbles differ per pipe: a colour factor on the RGB pipe becomes the
matching alpha factor on the alpha pipe (`SRC_COLOR 0x4` -> `SRC_ALPHA 0x6`), so
each GL factor carries both codes.  ZERO 0/0, ONE 1/1, SRC_ALPHA 6/6,
ONE_MINUS_SRC_ALPHA 7/7, DST_COLOR 8/0xa, and so on.

Verification: our encoder emits `0x00800202` with blending off and `0x0080eced`
for `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` - both exactly the
values the vendor capture carries.

### Hardware verification of the alpha test

Flat red triangle, ~86585 px, black background, each run carrying an
alpha-test-off positive control:

| alpha test | red pixels |
|---|---|
| off | 86585 |
| GREATER, ref `0x00` | 86593 |
| GREATER, ref `0x80` | 86585 |
| GREATER, ref `0xff` | **0** |
| LESS, ref `0xff` | **0** |

The test accepts and rejects on both compare directions and honours the
reference, so the encoding is confirmed on hardware.  GREATER `0xff` and LESS
`0xff` both rejecting everything pins the pipeline's constant fragment alpha at
exactly 255 - our vertices carry no alpha component yet, so the interpolator
supplies 1.0.

### Hardware verification of blending

Flat red triangle over a **green** background, so a blended result cannot be
confused with an unblended one.  437703 green + 86585 triangle = 524288, the
whole scanned region, in every row:

| mode | `0x12e8` | result |
|---|---|---|
| off | `00800202` | 86585 red |
| `ONE, ONE` (additive) | `00802223` | **86585 yellow**, 0 red - red + green |
| `ZERO, ONE` | `00802021` | **524288 green** - destination preserved exactly |
| `DST_COLOR, ZERO` (modulate) | `00801411` | **86585 black** - red x green |
| `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` | `0080eced` | 86585 red, same as off |

Additive, modulate and keep-destination are independent of the fragment alpha,
so they prove the blender ran even though the alpha is a constant 1.0; the
SRC_ALPHA row matching the unblended control is exactly what alpha = 1.0
predicts.  Every result matches the factor table's prediction, confirming both
the RGB and the alpha nibble of each factor.

### Per-vertex alpha

A three-component colour attribute makes the interpolator supply alpha = 1.0,
which is why the alpha test above could only be probed at its extremes.  Widen
the colour descriptor's component mask `[25:22]` from `0111` to `1111` and the
vertex carries a real alpha: layout x,y,z,r,g,b,a, seven dwords per vertex, so
the draw header becomes `0x970c0001`.

With alpha = 0.5 at every vertex, over a green background:

| state | result |
|---|---|
| blending off | 86568 red - the wider vertex still draws correctly |
| `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` | 43311 px `0x7f8000` + 43265 px `0x807f00` = 86576, the whole triangle |
| alpha test GREATER, ref `0x40` | 86576 red - passes |
| alpha test GREATER, ref `0xc0` | 0 - all 524288 pixels stay green |

`0x7f8000` and `0x807f00` are the two roundings of 127.5, which is exactly
`0.5 * red + 0.5 * green`.  The split into two values in near-equal numbers is
the dither unit (`0x12e8` bit 23) resolving the half-LSB; `0x808000` and
`0x7f7f00` never appear, so the dither is anti-correlated between the two
channels rather than independent per channel.

Per-vertex alpha therefore interpolates and drives both the alpha test and the
blender.

## Clipping and lighting, 2026-09-08

Both became possible once the constant file worked
(`docs/parhelia-constant-file-blocker.md`).

### Clipping

An NDC triangle far outside the view volume - `(-2,-2) (2,-2) (0,2)`, whose
unclipped screen vertices are `(-512,1152) (1536,1152) (512,-384)` - rasterises
to **688096 px** against a hand-computed visible area of **687872**, a 0.03%
difference.  Edges are straight to the screen boundary, the colour gradient
continues from the unclipped vertices rather than restarting, and there is no
wraparound.  Viewport clipping needs no extra state: the scissor and guard band
the library already emits are enough.

### Lighting

`p3d_warp_lighting()` is a fixed-function directional light: `max(0, N.L)` per
vertex times the material colour, interpolated by the setup unit.  The normal
rides in the colour attribute `v5`, the light direction is `c4` and the
material `c5`, both loaded in the same block as the MVP.

Measured with `L = (0,0,1)`, white material, and three vertex normals:

| vertex normal | predicted N.L | predicted grey | measured |
|---|---|---|---|
| `(0,0,1)` | 1.000 | 255 | **255** (`#ffffff`) |
| `(0.707,0,0.707)` | 0.707 | 180 | **180** (`#b4b4b4`) |
| `(1,0,0)` | 0.000 | 0 | 16 (`#101010`, one pixel inside the corner) |

97371 pixels in 1667 distinct greys - a smooth interpolated ramp.

### Two WARP encoding rules that cost a day

Both assemble cleanly and both silently draw **nothing**:

1. **Only one constant per instruction bundle.**  `dp3 r3, v5, c4 | mov T1, c9`
   reads two constants in one bundle.  Keep `mov T1, c9` paired with a primary
   that reads no constant, exactly as the vendor does.
2. **A constant reads correctly only in the operand positions the vendor uses**
   - `dp4`/`dp3` src1 and `mad` src0.  `add o1, c0, r1.0000` runs and draws,
   but the colour that comes out is `#ffff80`, not c0's `(1,0,0,0)`.

Neither is a program-length limit: the working program plus one extra
instruction draws normally, which is the control that separated the two.

## Transform, clipping and lighting in ONE draw

Each of the three was proven separately above.  This is the combined case - a
single draw, single stream, single WARP program, with the depth buffer also
bound and testing:

* **Transform**: MVP with a translation of +0.25 in x, so the NDC triangle
  `(-2,-2) (2,-2) (0,2)` lands at screen `(-384,1152) (1664,1152) (640,-384)`.
* **Clipping**: those vertices are outside the viewport on all four sides.
* **Lighting**: vertex normals `(0,0,1)`, `(1,0,0)`, `(0.707,0,0.707)` against
  `L = (0,0,1)`, so the unclipped vertex greys would be 255, 0 and 180.

| quantity | predicted | measured |
|---|---|---|
| visible area | 663552 px | 662528 px (0.15% low, one scanline) |
| brightness at (10,700) | 221 | **221** |
| brightness at (500,400) | 171 | **171** |
| brightness at (1000,700) | 98 | **98** |

The three sample points are chosen far apart inside the clipped region and are
computed from the barycentric interpolation of `max(0, N.L)` at the transformed
vertex positions.  Matching all three exactly to the LSB is only possible if
the transform, the clip and the lighting are simultaneously correct - a wrong
matrix moves the barycentric weights, a wrong clip changes which pixels exist,
and wrong lighting changes the ramp.

The visible grey range is 94..227 rather than 0..255 because the fully lit and
fully unlit vertices are both off-screen and clipped away - itself a check that
the clip is doing what is claimed.

Reproduce with:

```
p3d_selftest --depth --zbase 0x3400000 --posdesc 0x21c00000 \
             --coldesc 0xa1c50030 --warpentry --light --translate 0.25 0 \
             --emit TCL
parhelia-3dtri --warpstore --warpidx 32 --warpbind 2 loader.warp.hex 0 \
               --warpprog light4-prog.hex --rrfence auto \
               --ringreplay TCL-{ring,sec}.hex TCL-reloc.txt --ringvtx TCL-verts.hex
```


## The missing sliver: a missing end-of-draw pipeline drain - SOLVED

Every draw was losing roughly 1024 pixels - about 1% - and it showed up three
different ways: a bite out of the bottom-right corner of the Gouraud and
textured triangles (933 px short of 98304), one scanline missing from the
clipped TCL draw (1024 px short of 663552), and a depth footprint ~850 pixels
larger than the colour footprint of the same triangle.

### What the loss actually looked like

Comparing the measured pixel set against a CPU rasterisation of the same
triangle:

* **zero extra pixels** in every case - the geometry is exact, nothing is drawn
  outside the triangle, so this was never a rasterisation or clipping fault;
* the missing pixels are confined to **whole 32x32 pixel sub-blocks** at the
  end of raster order.  The clipped draw lost exactly one - block (31,23), the
  last block of the last tile of the screen, 1024 of its 1024 covered pixels;
* within a tile the loss is in-tile x 32..63 for all 32 rows, i.e. a 32x32
  block, which is contiguous in *pixels* but scattered in memory - so it is a
  rasterisation quantum, not a byte-range write buffer.

A fixed ~1024-pixel quantum, always the last work in raster order, is a
pipeline that is never drained.

### The fix

The vendor drains it immediately after its draw packet, in one register list:

```
0x1300 = 0x00000307     DWGSYNC opcode (DWGCTL nibble 6/7 is the sync family)
0x12bc = tag | 1        arm the fence
0x12bc = tag            disarm
0x1300 = 0x00a60ec4     restore the 3D DWGCTL
```

Wrapping the batch fence **around** the sync opcode is the point: the fence
then waits for the pixel pipeline to empty instead of merely ordering behind
the draw.  Our streams emitted the EOBTAG and the fence pair but left DWGCTL at
its 3D value, so nothing ever forced the drain.

### Result - every count is now exact

| case | predicted | before | after |
|---|---|---|---|
| Gouraud triangle | 98304 | 97371 (933 missing) | **98304, 0 missing** |
| textured triangle | 98304 | 97371 | **98304** (73664 + 24640 dither) |
| clipped TCL draw | 663552 | 662528 (1024 missing) | **663552, 0 missing** |
| depth vs colour footprint | equal | 86577 vs 87426 | **87500 vs 87500** |

## Compare functions: the full table, measured

The three-bit mask in `0x12c8[3:1]` (bit 0 enables the test) is now measured
directly rather than inferred from two captures.  Method: sweep the low byte
`0xe0..0xef` and test each against three conditions - incoming z below the
stored value, exactly equal, and above.  The equal column needs care: draw once
to write the depth, then **clear only the colour** and draw again, so any
non-black pixel means the second draw passed.

| `0x12c8` low byte | mask `[3:1]` | less | equal | greater | function |
|---|---|---|---|---|---|
| `0xe1` | 0 | | | | NEVER |
| `0xe3` | 1 | | X | | EQUAL |
| `0xe5` | 2 | X | | | LESS |
| `0xe7` | 3 | X | X | | LEQUAL |
| `0xe9` | 4 | | | X | GREATER |
| `0xeb` | 5 | | X | X | GEQUAL |
| `0xed` | 6 | X | | X | NOTEQUAL |
| `0xef` | 7 | X | X | X | ALWAYS |

So bit 0 = EQUAL, bit 1 = LESS, bit 2 = GREATER, and every even byte disables
the test (all eight draw unconditionally).  `0xe5` and `0xeb` match the
vendor's own GL_LESS and GL_GEQUAL captures, and the other six are new.

ioquake3 needs LEQUAL (its default) and EQUAL (multi-pass shaders); both are
confirmed working.

## Colour mask, `0x1324`

One byte per channel in the render target's own order - B `[7:0]`, G `[15:8]`,
R `[23:16]`, A `[31:24]` - `0xff` to enable a channel and `0x00` to mask it.
`glColorMask(1,0,1,1)` was captured as `0xffff00ff`, and on hardware a white
triangle with green masked renders as 98304 px of `#ff00ff`, with red and green
masked as 98304 px of `#0000ff`.

## Texture filter and wrap, structured

`p3d_tex_filter()` / `p3d_tex_wrap()` build the words from
`docs/parhelia-texture-sampling.md`'s fields rather than a magic constant:

* `0x11c0` MIN `[1:0]`, MAG `[4:3]`, MIP `[7:6]`, bit 12 always set; codes
  0 nearest, 2 linear, 3 anisotropic, MIP 0 none / 1 nearest / 2 linear.
  The capture's `texlin` (GL_LINEAR both) is `0x01c01012`, which the builder
  reproduces from MIN=MAG=2.
* `0x11c4` S `[2:0]`, T `[5:3]`, R `[8:6]`, LOD min `[23:16]`, max `[31:24]`.
  REPEAT is 0 and GL_CLAMP is **6**, from the capture's `texclamp`
  (`0xf0000036`).
* `0x11e4[31:28]` is the maximum mip level.
