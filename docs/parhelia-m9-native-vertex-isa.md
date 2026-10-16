<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 native vertex program: entry, attributes, the transform constants and
# the epilogue

This note records the vertex half of the M9 (`102b:0540`) native program: how
the preprocessing pass enters it, how source attributes reach it, where the
transform matrix and the viewport live in the constant span, and what the
program does after the divide.  It continues
`parhelia-m9-native-fragment-program-re.md` (where the program lives) and
`parhelia-m9-native-fragment-arithmetic.md` (the slot encoding), and it is the
reference for putting transform and lighting on the hardware instead of the
Gallium draw module.

Labels: **MEASURED** is an M9148 result read back from the colour surface and
compared against a CPU reference for every pixel, through
`tools/m9-user-vertex-probe.c` on September 17, 2026; **CAPTURED** is read from
the vendor programs and command streams in
`captures/m9x-vendor-glsl-programs-20260915`, decoded with
`tools/m9-vertex-program-extract.py`; **DERIVED** is inferred from those and
not separately confirmed; **UNDECODED** is named where the words are known but
their meaning is not.

## Result

The clean-room driver already runs a real hardware transform.  What
`m9_passthrough.h` calls `m9_passthrough_c2_program` is not a pass-through: its
36 vertex slots are the vendor's full transform — a 4x4 matrix multiply, a
reciprocal of `w`, the perspective divide, the viewport and depth scale, and a
guard-band clip code — and `m9_draw_params.c` already fills its constants with
`glOrtho(0, W, 0, H, -1, 1)`.  The CPU pre-transforms vertices into that same
screen space, so the hardware transform is currently doing an identity's worth
of work.

Everything needed to stop doing that is below and measured: replacing the nine
matrix pairs of the preprocessing constant span with an application
modelview-projection matrix and feeding object-space positions produces
correct geometry, including perspective, with no change to the program itself.

## 1. Entry and addressing (CAPTURED)

The `c2` image holds `[fragment slots][vertex slots]`; index `c3` is the entry
slot of the pass.  A preprocessing pass enters at its vertex program, a raster
pass at its fragment program.  The vendor's first vertex program therefore
starts at slot 4 in every one-fragment-program case, and the program runs to
the shared stop pair `ffff801c a01fc00f`.

The vendor splits a shader across **one to nine preprocessing passes**
(`vs-varyings-max` uses nine), each with its own entry, its own source
declaration and its own constant span; a later pass reads the previous pass's
results as low-numbered registers.  The driver's program is the single-pass
shape: one pass, four output targets, one constant span.  Only the single-pass
shape is documented here; the inter-pass register contract is **UNDECODED**.

Constants of a pass are the 8-byte pairs between `8d` and `8e`, with
`8e - 8d == 8 * ((8c >> 8) & 0x7f)` (CAPTURED, without exception across the
corpus).  A constant index `e` or a constant index in the `c` field is a pair
index **relative to that pass's own `8d`**.  In the shipping stream the raster
span is pairs 0..1 of the same arena and the preprocessing span starts 16 bytes
later, which is why `m9_passthrough_system_320x240` begins with the two raster
pairs: preprocessing pair *p* is prefix dwords `4 + 2p` and `5 + 2p`.

## 2. Attributes (MEASURED)

The preprocessing source declaration is the index-`23` list.  Each word is

```text
dword << 24 | attr << 20 | stream << 16 | s0 | s1 << 3 | s2 << 6 | s3 << 9
```

Bits 19:16 are the **source stream index** (DECODED 2026-09-17).  That is what
the byte at 23:16 walking `10, 11, 22, 21, 33, 34, 45, 46, 97, 98, a9, aa`
means: it is `(attr, stream)` = (1,0)(1,1)(2,2)(2,1)(3,3)(3,4)(4,5)(4,6)
(9,7)(9,8)(10,9)(10,10).  The vendor packs the used components of two different
GL attributes into one attribute register pair, giving each half its own
stream.  The raster builder in `m9_draw_params.c` already writes this same
`attr << 20 | stream << 16` form.

A pass takes **up to sixteen** declaration words, not eight: index `22` bits
11:8 hold the count minus one, a four-bit field.  Measured word counts across
the captures: N=3 gives 5, N=4 gives 6, N=6 gives 8, N=8 gives 10, and N=14
gives 5 plus 13 **across two preprocessing passes** -- several "blocks" are
several passes, not one block splitting.

**Attributes 5..8 are the four texture coordinate sets**, so the general
attribute numbering runs 1..4 and then resumes at 9..10.  That is why the
register rule below stops being usable after four: `0x46 + 8*(a - 1)` reaches
`0x86` at a=9, which does not fit the seven-bit operand fields.

where `dword` is the offset of the element inside the source record, `attr` the
destination attribute, and `s0..s3` are four component selectors: `0..3` pick
x,y,z,w of the record element, `4` is the literal 0.0, `5` the literal 1.0 and
`6` leaves that slot to the paired word.  Two words describe one attribute:
slots 0,1 fill its **first** register and slots 2,3 its **second**.  The list
ends with `00004b08` (attr 0), and index `22` bits 11:8 carry the word count
minus one, the preprocessing pass exactly as the raster pass.  Index `10` is
the record span in bytes.

**Attribute *a* is register pair `0x46 + 8 * (a - 1)` and the next register.**
CAPTURED across the corpus: `vs-uniform-color` declares one attribute and reads
`46 47`; `vs-ftransform` two and reads `46 47 4e 4f`; `vs-lighting` three and
reads `46 47 4e 4f 56 57`; `fs-color` four and reads through `5e 5f`.
MEASURED for attributes 1 and 2: with a third attribute declared and the second
carrying a decoy colour, patching the two colour moves of the vendor program to
read `0x4e/0x4f` returns the decoy for 100 % of 19827 interior pixels, and the
position transform reading `0x46/0x47` is what the whole of section 3 measures.
Attribute 3 is CAPTURED only.  The same probe declaring three attributes and
reading `0x56/0x57` draws the right geometry (21095 pixels, no stray) but
returns neither the third attribute's colour nor the decoy — pixels such as
`00aa5600` and `54540000`, with the alpha the declaration's literal 1.0 should
have pinned also wrong.  So either a third preprocessing attribute needs
something the probe does not set, or its registers are not `0x56/0x57` in a
three-attribute declaration.  **UNDECODED**; a vendor capture with three
non-zero vertex inputs whose values are distinguishable, or a sweep of the
register over a declared range, would settle it.

The order of the two registers follows the declaration, not a fixed rule: the
shipping declaration writes `(z,w)` into the first and `(x,y)` into the second
(`m9_vertex_decl_init`'s "position slots are z,w | x,y"), while `vs-attrib` and
`vs-vertex-direct` declare `(x,y)` first.  The transform reads whichever
register the declaration filled.

Attributes 1..4 are CAPTURED from the vendor programs.  Reading register `0x5e`
while only three attributes were declared **locked the engine** (the submit returned `-EIO`,
then every later submit `-EBUSY`, and a module reload did not recover it — only
a reboot did), so an undeclared attribute register must never be read.  The
upper limit is **UNDECODED**: `0x46 + 8 * (a - 1)` reaches `0x7e/0x7f` at
`a = 6`, and `0x7f` is the "no operand" encoding in every captured slot, so at
most five or six attributes can be addressed; a capture of a vendor shader with
five or more vertex inputs would settle it.

One measured limit of the shipping command stream, not of the hardware: its
preprocessing pass has room for eight index-`23` words, so three attributes
(seven words) fit and four (nine words) do not.  A vendor capture with more
attributes shows the block is not fixed at eight words — it grows and splits
into several terminated blocks (`parhelia-m9-openbits-gl-capture.md`).

**Update (2026-09-17, MEASURED on the vendor stack.)**  Fifteen simultaneous
vertex attributes work: `gl_Vertex` plus fourteen generic ones, each checked
per attribute and per value over 64 cells.  The sixteenth fails to link even
though `MAX_VERTEX_ATTRIBS` is 16.  So attribute 3 and every attribute up to 15
is verified; the earlier probe failure at `0x56/0x57` was a probe problem, not
a hardware limit.  Two ICD mechanics matter when writing such a probe:
`glBindAttribLocation` crashes the vendor ICD, and generic attribute arrays
must live in buffer objects.  The mapping from declaration entry to attribute
register is still UNDECODED; dumping the compiled C2 program alongside the
index-`23` words for several attribute counts would close it.

## 3. The transform (MEASURED)

Four rows, each a two-component partial dot followed by a completion that adds
the other half.  With `A` the attribute's first register and `B` its second —
`(z,w)` and `(x,y)` under the shipping declaration — the slots of
`m9_passthrough_c2_program` are

| slot | op2 | b | c | d | op0 | e | computes |
|---|---|---|---|---|---|---|---|
| 5  | 0x10 | `0x47` B | `0x03` | `0x11` | 1d | 8 | partial of clip.z |
| 7  | 0x0e | `0x46` A | `0x11` | `0x01` | 1d | 7 | clip.z |
| 8  | 0x10 | `0x47` B | `0x03` | `0x11` | 1d | 5 | partial of clip.x |
| 9  | 0x10 | `0x47` B | `0x03` | `0x15` | 1e | 6 | partial of clip.w |
| 10 | 0x0e | `0x46` A | `0x11` | `0x00` | 1d | 4 | clip.x |
| 11 | 0x10 | `0x47` B | `0x03` | `0x11` | 1e | 2 | partial of clip.y |
| 12 | 0x0e | `0x46` A | `0x15` | `0x01` | 1e | 1 | clip.w |
| 13 | 0x0e | `0x46` A | `0x11` | `0x00` | 1e | 0 | clip.y |

`op0` is the destination scalar mask of the fragment note (`1d` first scalar,
`1e` second), so `r00 = (clip.x, clip.y)` and `r01 = (clip.z, clip.w)`.  W1 is
`0xbe000d0f | e << 14` for the partial and `0xadc00c0f | e << 14` for the
completion, `0xadc00c3f | e << 14` when the completion takes the partial's
second scalar (operand swizzle 3 at bits 5:4, exactly the encoding of the
fragment note).  **Corrected 2026-10-02 (MEASURED):** the partial's `c` field
is a constant pair index, not a register: it computes `d.s0 = b . k[e] - k[c].s0`
(probes with binary-weighted operands; a pair holding 0.25 subtracts exactly
0.25).  The vendor's `c = 0x03` works only because its pair 3 holds zero; a
compiled program must name a zero literal pair there.  The partial results
land in `0x11` and `0x15` and are read by the completion within one or two
slots -- the only vendor use of those registers in this program.

Both dot operands are read whole, so the operand swizzle is 0 on the `b` field
of all four.  `e` names a constant pair, and that pair is a **row half**:

```text
clip.x = A . k5 + B . k4      k5 = (m[0][0], m[0][1])   k4 = (m[0][2], m[0][3])
clip.y = A . k2 + B . k0      k2 = (m[1][0], m[1][1])   k0 = (m[1][2], m[1][3])
clip.z = A . k8 + B . k7      k8 = (m[2][0], m[2][1])   k7 = (m[2][2], m[2][3])
clip.w = A . k6 + B . k1      k6 = (m[3][0], m[3][1])   k1 = (m[3][2], m[3][3])
```

reading `m` as row major with `clip = m * object`, and `X . K` as
`X.s0 * K.s0 + X.s1 * K.s1`.  **Pair 3 is not referenced by any slot** and the
vendor leaves a copy of the w row's tail there; the driver may write anything.

The map onto `m9_passthrough_system_320x240` (prefix dword `4 + 2p` for pair
`p`) and onto what `m9_draw_params.c` patches today:

| pair | prefix dwords | shipping value at W x H | matrix element |
|---:|---|---|---|
| 0 | 4, 5   | `(0, -1)`            | m[1][2], m[1][3] |
| 1 | 6, 7   | `(0, 1)`             | m[3][2], m[3][3] |
| 2 | 8, 9   | `(0, 2/H)` (`sys 9`) | m[1][0], m[1][1] |
| 3 | 10, 11 | `(0, 1)`             | unused |
| 4 | 12, 13 | `(0, -1)`            | m[0][2], m[0][3] |
| 5 | 14, 15 | `(2/W, 0)` (`sys 14`)| m[0][0], m[0][1] |
| 6 | 16, 17 | `(0, 0)`             | m[3][0], m[3][1] |
| 7 | 18, 19 | `(-1, 0)`            | m[2][2], m[2][3] |
| 8 | 20, 21 | `(0, 0)`             | m[2][0], m[2][1] |
| 9 | 22, 23 | `(W/2, -H/2)`        | viewport scale x, y |
| 10 | 24, 25 | `(0.5, 1)`          | depth scale, unused |
| 11 | 26, 27 | `(W/2, H/2)`        | viewport bias x, y |
| 12 | 28, 29 | `(0.5, H)`          | depth bias, unused |
| 13 | 30, 31 | `(0, 2)`            | Newton step of the reciprocal |
| 14 | 32, 33 | `(-16352, 16351)`   | UNDECODED, size invariant |
| 15 | 34, 35 | `(0, 0)`            | UNDECODED |
| 16 | 36, 37 | `(1, 0.5)`          | clip-code seed and DP3 weight |
| 17 | 38, 39 | `(+inf, -inf)`      | the finiteness compares |
| 18 | 40, 41 | `(1, 15359/(W/2) - 1)` | guard band, x |
| 19 | 42, 43 | `(1, -(15359/(H/2) - 1))` | guard band, y |
| 20 | 44, 45 | `(127, 128)`        | UNDECODED |
| 21 | 46, 47 | `(1.25, 1.5)`       | clip-code alternative |

Read as a matrix, the shipping constants are exactly
`glOrtho(0, W, 0, H, -1, 1)`: `x' = 2x/W - w`, `y' = 2y/H - w`, `z' = -z`,
`w' = w`.  The same 13-pair viewport-and-clip block, in the same order, appears
in every captured vendor program (`vs-vertex-direct` at 128x128 has
`(64,-64) (0.5,1) (64,64) (0.5,128) (0,2) (-16352,16351) (0,0) (1,0.5)
(inf,-inf) (1,238.98) (1,-238.98) (127,128) (1.25,1.5)`), which is what pins
pairs 9..21 independently of the driver's own code.

Only pairs 0..8 differ between vendor cases, and the layout above reproduces
`vs-mvp`'s eight-pair explicit `gl_ModelViewProjectionMatrix * gl_Vertex` form
as well, which the vendor compiles from multiply and multiply-add
(`op2 = 0x01` and `0x02`) instead of the dot pair; both produce the same eight
row halves.

### Measurement

`tools/m9-user-vertex-probe.c` rebuilds the verified two-pass stream, keeps
the vendor `c2` image byte for byte, writes object-space vertices into the
source buffer and writes its own matrix into pairs 0..8.  For every variant a
CPU reference applies the same matrix, divides by `w` and applies the same
viewport, and every pixel at least one pixel inside a predicted triangle is
compared against the barycentric interpolation of the three corner colours.
Three triangles, distinct corner colours, 320x240:

| variant | matrix supplied | interior pixels | matched |
|---|---|---:|---|
| ortho-screen | the shipping ortho, screen-space source | 19827 | 100 % |
| identity | identity — object coordinates are clip coordinates | 19827 | 100 % |
| affine | rotate 0.6 rad, anisotropic scale, translate | 8461 | 100 % |
| perspective | non-constant w row (0.25, 0.18, 0, 1.30) | 10295 | 100 % |
| float4-zw | four-component source, x and y take object z, w takes z | 7949 | 100 % |
| composed | ortho times a scale-and-bias, as a matrix product | 11563 | 100 % |

No variant wrote a pixel outside its predicted triangles.  The `perspective`
variant is the one that proves the divide: a screen-linear interpolation model
matches only 3.8 %, 50.5 % and 79.6 % of the three triangles while the
perspective-correct model matches 100 %, so the reciprocal chain computes a
real `1/w` and the raster engine interpolates with it.

The z row reaches only the depth unit, so it was measured by clipping:
with `m[2] = (0,0,0,+2)` and `(0,0,0,-2)` — clip.z at `+2w` and `-2w` — the
draw writes **zero** pixels, and with `(0,0,0,+0.5)` it writes the same 21106
pixels as the identity variant.  Near and far clipping against `±w` therefore
happens, and pairs 7 and 8 are the z row.

## 4. The rest of the program

Slot numbers are of `m9_passthrough_c2_program`; the same slots in the same
order appear in every captured single-pass vendor program, with only register
numbers and constant indices shifted.

**Reciprocal of w (CAPTURED, and MEASURED through `perspective`).**  Slot 15 is
the `op2 = 0x11` seed on `r01` with the second-scalar select, slots 17/18 and
20/22 are two Newton refinements built from the `op2 = 0x01` residual with W0
class 5 (`8c26af10`) and the `op2 = 0x13` step against constant pair 13
`(0, 2)` — the same reciprocal the fragment note records for `fs-smoothstep`,
with the same words.  The result is `r04`.

**Divide and viewport (CAPTURED).**  Slot 23 multiplies `r00` (clip.x, clip.y)
by `r04`, slot 24 multiplies clip.z by it; slot 26 is a multiply-add
`r06 = ndc.xy * k9 + k11` — and this is the one place where a
three-operand instruction takes **two** constants, the multiplier in `e` and
the addend in W0's `c` field.  Slot 27 is the same form for depth,
`r1b.s0 = ndc.z * k10 + k12`, so depth lands in `[0,1]`.

**Outputs (CAPTURED).**  Registers `0x11`, `0x13`, `0x15`, `0x17`, `0x19`,
`0x1b` are written and never read again; so is `0x21`.  In the driver's
four-target configuration they carry

```text
0x11, 0x13  the varying (colour x,y then z,w)        -> b0 -> raster stream 0
0x15, 0x17  the clip position                        -> b2 -> raster stream 3
0x19, 0x1b  screen x,y then depth and 1/w            -> b4 -> raster stream 1
0x21        the clip code                            -> b8 -> raster stream 2
```

matching the kernel's relocation table (`b0,b2,b4,b8` to transient 0,3,1,2 and
raster inputs `00..03` to transient 0,1,2,3) and the raster declaration
`80100d88 801006b6 00010688 00c231b6 00d30688`, whose attribute 0 is the
rasterizer position, attribute 12 takes one component from the six-byte stream
and attribute 13 is the clip position.  The general rule is **UNDECODED**: in
the vendor's multi-pass shape the same screen position is written to
`0x15/0x17` when the pass binds fewer targets, so the register-to-target
binding depends on the pass's target set, which no capture isolates.  A
capture of a two-target preprocessing pass with a known output would settle it.

**The clip code (DERIVED, arithmetic UNDECODED).**  Slots 29..38 with 14 and
34 build `r21`.  Slot 29 seeds it from pair 16 `(1, 0.5)`.  Slots 30 and 31
multiply the broadcast `w` by the guard-band pairs 18 and 19, giving the
per-axis limits `(w, w * gx)` and `(w, -w * gy)` — `gx` and `gy` are exactly
`M9_GUARD_BAND / (W/2) - 1`, which `m9_draw_params.c` already computes.  Slots
14 and 34 are the fragment note's compare unit (UNIT 1, `op2 = 0x00`,
`e = 0x05`, class 2 `<=`) against pair 17 `(+inf, -inf)`, one for the clip
`xy` and one for the clip `zw`, so they are finiteness tests.  Slots 32, 33 and
35 are a three-source form at `op2 = 0x00` with W1 `0x8c01cd10`, classes 0, 1
and 2, whose arithmetic is **UNDECODED**; slot 36 is a two-component dot of the
two compare results plus a constant, the `op2 = 0x0d` register-register dot
`docs/parhelia-m9-vertex-isa-gaps.md` section 6 decodes (it was called a DP3
here before that form was read); slot 37 the fragment note's select (`op2 = 0x1a`, register condition,
class 0) between `r0d` and pair 21 `(1.25, 1.5)`, and slot 38 an `op2 = 0x04`
write into `r21`.

Two things about it are MEASURED.  **Exceeding the guard band does not cull.**
A matrix putting one vertex of each triangle at NDC y ~ 290, three times the
guard-band limit of 94.99 that pair 18 encodes at 320x240, still rasterizes the
on-screen part: 5280 pixels written, none outside the predicted triangle, and
100 % of 4998 interior pixels carrying the right interpolated colour.  A
control at NDC y ~ 87, inside the limit, covers the same pixels — the two
slivers differ only far above the viewport — but its colours are wrong
(`bcf80000`, `bef40000`, `ffffff00` where the clipped variant gives `ffbf6011`,
`ff8c9210`), reproducibly over four runs.  Coverage is therefore right either
way and only interpolation degrades, which is consistent with the band existing
so that setup runs unclipped; the band the vendor picks is evidently wider than
the precision of a triangle whose apex is ten thousand rows outside the screen.
Near and far clipping does cull, as section 3 records.  A replacement program
must still produce a sensible `r21`, but the clip code does not gate ordinary
geometry.

## 5. How much of the instruction set is shared with the fragment unit

Shared, byte for byte (CAPTURED): the two-dword slot with `a` split across
W0[31:29] and W1[3:0], `b` at W0[28:22], `c` at W0[21:15], `d` at W0[14:8],
`cls` at W0[7:5], `op0` at W0[4:0], `op2` at W1[25:21], the constant index at
W1[20:14]; the per-operand two-bit swizzles at W1[9:8], [7:6], [5:4] with 1 the
identity; `op0` as the destination scalar mask; the stop pair
`ffff801c a01fc00f`; the move form `op2 = 0x14`; multiply `0x01` and
multiply-add `0x02`; the compare unit `op2 = 0x00` at W1 `0x90000000` with the
class as predicate and the constant pair index in W0's `c`; the select
`op2 = 0x1a`; the dot forms `0x0e` and `0x10`; and the reciprocal seed `0x11`
plus the Newton step `0x13` with its `(0, 2)` pair.  Every one of these appears
in the vertex programs with the same encoding the fragment backend already
emits.

Different: the sources and the destinations.  A fragment program interpolates
from `0x40 + 8k + 2*lane` with the position register `0x79` and writes the
output register `0x2d`; a vertex program reads attributes from
`0x46 + 8*(a-1)` and writes the output registers `0x11 .. 0x1b` and `0x21`.
The vertex unit additionally uses the three-source `op2 = 0x00` form at W1
`0x8c01cd10` and the `op2 = 0x04` write, neither of which occurs in any
captured fragment program.

## 6. What a NIR-to-native vertex backend would still need

1. The **output contract** for a pass that binds a number of targets other
   than the driver's four (section 4), and the register contract between
   passes, which the vendor uses for every shader with more than four output
   vectors.  Both are UNDECODED.
2. A rule for **appending the epilogue** to a compiled user program.  The
   epilogue can be copied verbatim — it is the same 25 slots in every capture —
   but its constant indices and its temporaries must be relocated around the
   user program's, and one of its instructions is undecoded, so it cannot be
   re-derived, only moved.
3. The **safe temporary range**.  The vendor writes `0x00..0x21` freely in
   vertex programs, unlike the fragment unit where class-0 destinations
   `0x00/0x01` lock the engine; that difference is CAPTURED, not measured.
4. ~~Whether the vertex unit has the fragment unit's **special-function
   chains**~~ — settled in `docs/parhelia-m9-vertex-isa-gaps.md` section 6:
   `exp2` and the **reciprocal-square-root chain** are both CAPTURED on the
   vertex unit (the rsq seed `op2 = 0x11` class 5 and its step `op2 = 0x13`
   class 7, in `vs-lighting`, `vs-math` and `vs-builtin-light`, which is the
   shape those two cases carry).  `log2` is CAPTURED in fragment programs
   only and stays OPEN, with the probe that would settle it in section 4 of
   that note.  There is **no sine and no cosine instruction** at all: the
   vendor lowers both to a `fract` range reduction and a polynomial whose
   coefficients sit in its compiler's constant pool.

None of that is needed for the immediate win, which is section 3 alone.

## Reproduction

```sh
# host: decode the vendor vertex programs and their declarations
python3 tools/m9-vertex-program-extract.py --summary \
    captures/m9x-vendor-glsl-programs-20260915
python3 tools/m9-vertex-program-extract.py \
    captures/m9x-vendor-glsl-programs-20260915/vs-ftransform

# target, next to m9-user-triangle-data.h, inside flock /root/tmp/m9-hw.lock
gcc -O2 -Wall -I/root/parhelia-cleanroom/include -o m9-user-vertex-probe \
    m9-user-vertex-probe.c -lm
./m9-user-vertex-probe     # every variant, or name them on the command line
```

The probe only ever reads registers of attributes it declared.  Reading an
undeclared attribute register locks the engine until the machine is rebooted,
which is how `0x5e` and `0x66` were ruled out, so do not widen its sweep.
