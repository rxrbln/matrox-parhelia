<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 varying transport: how interpolated values reach the raster stage

This note closes how the M-series carries interpolated values from the
preprocessing (vertex) stage to the raster (fragment) stage, why the
clean-room driver's second preprocessing draw delivered nothing on hardware,
and what the vendor's own ceiling is.

Labels: **STATIC** is read out of a vendor binary at a named address;
**CAPTURED** is read out of a saved vendor command stream or the runtime
values a saved fixture recorded; **DERIVED** is a join of those, with the
join given; **MEASURED** is an earlier hardware result, cited by its note;
**OPEN** is undecided.  Nothing in this note was run on hardware when it was
written; section 11 records the two runs that since settled its experiments 1
and 3.

Re-derivable with:

```sh
S="captures/m9x-vendor-glsl-programs-20260915
   captures/m9x-vendor-glamor-shaders-20260915
   captures/m9x-vendor-gl2ext-2d-video-modes-20260915
   captures/m9x-vendor-glamor-blend-20260915
   captures/m9x-linux-gl2-special-20260914
   captures/m9x-linux-five-sampler-20260914
   captures/m9x-linux-ff-tnl-20260914
   captures/m9x-vendor-openbits-20260917"
python3 tools/m9-varying-transport-evidence.py --chain  $S   # the pass grammar
python3 tools/m9-varying-transport-evidence.py --record $S   # the record encoding
python3 tools/m9-varying-transport-evidence.py --raster $S   # what raster reads
python3 tools/m9-vertex-output-target-evidence.py \
        captures/m9x-vendor-openbits-20260917/cap-attribs_14   # the clean witness
```

The binary reads below are `MSerX/MxMGLX.dll` from
`xddm32_210_00_105_se_u_nowhql/`, the M-series OpenGL ICD, image base
`0x10000000`, `.text` at file offset `0x1000`.

## 0. Result, in one paragraph

An interpolated value reaches the raster stage as **its own flat array, one
16-byte entry a vertex, bound as a raster stream slot** — never as a field of
a shared record.  Everything upstream of that is a *preprocessing chain*: one
or more draws that hand values to each other through a **packed intermediate
record**, addressed by dword and read back through the route bindings at
`0x74 + k`.  A draw states its place in that chain in **index `ef` bits 11:9**
and that field moves with the clip-code enable at bit 15; 628 of 628 captured
preprocessing passes obey it with no exception.  The clean-room driver's
extra preprocessing draw told the engine "the first draw of a chain" and then
left the fused pass saying "the only draw of a chain", which is not a state
the grammar has — the same class of failure the raster splitter had before
`ef` bits 10:9 were emitted (MEASURED,
`docs/parhelia-m9-fragment-pass-hardware.md` section 1).  The vendor's own
ceiling is **14 varyings**, a compare against `0xe` in the GLSL linker.

## 1. There are two different records; keep them apart

Earlier notes use "the record" for two objects that share only their
descriptor encoding.

* the **inter-pass record** of a split *fragment* program: one entry a
  fragment, written by one raster pass and read by the next, four dwords a
  hand-over, the object `docs/parhelia-m9-fragment-pass-hardware.md` and
  `docs/parhelia-m9-record-bandwidth.md` are about;
* the **intermediate record** of a *preprocessing* chain: one entry a vertex,
  written by one preprocessing draw and read by a later one through `0x74`.

Both are named by the same descriptor word (section 3), and the driver's
`m9_record_desc()` / `m9_record_route()` already build it.  Neither is what
the raster stage reads for an interpolated input: that is always a separate
16-byte-stride stream (section 5).

## 2. What one raster pass can consume (CAPTURED)

Unchanged from `docs/parhelia-m9-raster-stream-limit-re.md`, which MEASURED
six four-component inputs in one pass: the binding limit is index `22`
bits 11:8, the declaration word count minus one, so at most **16 declaration
words**.  The corpus holds 814 raster passes, 556 of which carry a
declaration of their own rather than inheriting one, and they add the
per-word cost that note did not separate out:

| declared as | words | occurrences |
|---|---|---|
| rasterizer position, attribute 0, `x,y,z,w` | 1 | 556 |
| clip position, attribute 13, `x,y,z,w` | 1 | 556 |
| clip code, attribute 12, `-,-,-,x` | 1 | 556 |
| an interpolated input, attributes 1..4 and 9..10, `x,y,-,-` **plus** `-,-,z,w` | 2 | 227 + 73 |
| a sampler coordinate, attributes 5..8, `x,y,z,w` or `x,y,z,-` or `x,y,-,-` | 1 | 13 + 16 + 74 |

so with the captured pair form the fixed trio costs three words and thirteen
are left: six interpolated inputs (12 words) and one sampler coordinate, or
fewer inputs and more coordinates.  **No captured pass ever declares an
interpolated input in the single `0688` word that attributes 0, 5, 6 and 13
use** — and the hardware takes it anyway, one word an input, MEASURED in
section 11.  It does not double the ceiling: six is the interpolation register
file, not the declaration budget.

## 3. The intermediate record: descriptor and routes (CAPTURED)

A preprocessing draw binds up to four output targets at `b0, b2, b4, b6`,
each an address at the even index and a descriptor at the odd one, plus the
clip code at `b8` (`docs/parhelia-m9-vertex-output-target-re.md` section 1).
A target descriptor is either the constant `0x68002000`, meaning a separate
flat stream view, or a record descriptor:

```text
bits 31:25  0x77         format class
bits 20:19  form         1 = two dwords (0xee09), 3 = four dwords (0xee19)
bit  16     1
bits 15:8   dword offset of this target's first dword inside the record
bits  7:0   record stride in dwords
```

which is exactly `m9_record_desc()` in `m9_draw_params.c`.  The route binding
a later draw reads it back with is `0x74 + k` for the selector word, `0x78`
for the record's base address and `0x79` for a descriptor of the same shape:

```text
route word  bits  7:0   0x0c for a value route, 0x10 + 4k for a coordinate route
            bits 15:8   record dword feeding component 1
            bits 23:16  record dword feeding component 2
            bits 31:24  record dword feeding component 3
```

component 0 comes from the descriptor's own offset field.  The program then
reads route `k` as the register pair `(0x00 + 4k, 0x02 + 4k)`, low register
components 0 and 1, high register components 2 and 3.

### The record is packed by dword, not by slot

The witness is `captures/m9x-vendor-openbits-20260917/cap-attribs_14`, a
fourteen-attribute fixture whose preprocessing chain is two draws.  Its first
draw binds three targets into **one** ten-dword record:

```text
seq 26 c3=04 ef=00280200 88=0300fff8 22=0008040e
   target b0 = 10ec1000 desc ee19020a     form 3, dwords 2..5
   target b2 = 10ec1000 desc ee19060a     form 3, dwords 6..9
   target b4 = 10ec1000 desc ee09000a     form 1, dwords 0..1
```

and the second reads them back:

```text
seq 26 c3=26 ef=00288e00 88=3f703f3f 22=00080c0e
   route 74 = 0302010c base 10ec1000 desc ee19000a    components 0,1,2,3
   route 75 = 0908050c base 10ec1000 desc ee19040a    components 4,5,8,9
   route 76 = 0000070c base 10ec1000 desc ee09060a    components 6,7
```

Three facts fall straight out, and each refutes a "four-dword slots" reading:
a target may start at **any** dword (2, 6 and 0 here, not multiples of four);
a target may be **two** dwords wide (`b4`, form 1); and the stride is the
exact sum of the packed fields, `2 + 4 + 4 = 10`.  The same ten-dword shape
carries two `vec2` texture coordinates in 54 glamor composite passes
(`composite-stub-texture-texture-normal`, targets at dwords 0 and 4, routes
gathering `4,5,8,9` and `6,7,8,9`).

### Observed ranges

Across 628 preprocessing passes the record descriptor takes 35 distinct
values: form 1 and form 3, dword offsets `0, 2, 4, 6, 8, 10, 12, 16, 20, 24,
28, 32, 36, 40, 44, 48, 52, 56` and strides `4, 10, 12, 16, 20, 60`.  The
widest is `vs-varyings-max`, fifteen four-dword fields in a sixty-dword
record.  Both fields are one byte wide in the driver's existing reading
(`M9_RECORD_MAX_DWORDS` is `0xff`); the captures prove offset ≥ 56 and
stride ≥ 60 and say nothing above that (**OPEN**).

## 4. A preprocessing draw states its place in the chain: `ef` bits 11:9

CAPTURED, 628 of 628 passes over six capture sets, produced by
`tools/m9-varying-transport-evidence.py --chain`:

| place in chain | `ef[10:9]` | `ef[11]` | `ef[15]` | route-fed | passes |
|---|---|---|---|---|---|
| the only draw | 0 | 1 | 1 | no | 170 |
| the first of several | 1 | 0 | 0 | no | 151 |
| a middle one, source-record-fed | 2 | 0 | 0 | no | 4 |
| a middle one, route-fed | 3 | 0 | 0 | yes | 152 |
| the last of several | 3 | 1 | 1 | yes | 151 |

There is no sixth row.  Read as fields: **bits 10:9 name where the draw's
vertex inputs come from** — 1 opens a chain on the source record, 2 continues
it on the source record, 3 reads the intermediate record through `0x74`, 0 is
a draw that is the whole chain — and **bit 11 marks the draw that produces
the final vertex outputs**, which is the same draw that enables the clip code
at bit 15.  The two never occur apart.  "Route-fed" here means a route word
other than the idle `0x00000100` *with a non-zero stride* in its descriptor;
108 single-draw passes carry the degenerate `0x0000010c` with
`0x79 = 0xee090000`, stride zero, which feeds nothing.

This is the preprocessing counterpart of the raster half's `ef` bits 10:9,
whose absence MEASURED-ly stopped the engine until
`docs/parhelia-m9-fragment-pass-hardware.md` section 1 emitted it.

### Why the clean-room driver's extra preprocessing draw delivered nothing

MEASURED (commit `cb65eafc`, appendix of
`docs/parhelia-m9-vertex-output-target-re.md`): every draw whose fragment
shader read a varying the extra preprocessing draw wrote rendered nothing and
came back `EBUSY` with the context lost.

The chain role below is **not** what was missing: emitting it changed nothing
on hardware (MEASURED, section 11).  The reading of `ef` bits 11:9 stands as a
capture survey; what it does not do is make the driver's chain-opening draw
deliver.

DERIVED: `m9_draw.c` gave the extra draw `ef = 0x00000200 | targets`, chain
place 1, correctly — and left the fused pass at the constant `0x00288800`,
chain place 0 with bit 11 and bit 15, which says *the only draw of a chain*.
A chain that opens and never closes is not one of the five rows above.  The
vendor's value for a fused-shaped final draw behind one other preprocessing
draw is **`0x00288e00`** — three targets, the clip code, place 3 with bit 11
— and `cap-attribs_14`'s second draw carries exactly that word, with exactly
the same three bound targets and clip code the m9x fused pass binds.  The fix
is one term; section 7.

Two hypotheses the same measurement already refuted — index `0x88`'s
register-pair mask, and the barrier between the two draws — are recorded in
that appendix and are not revisited here.

## 5. The raster stage never reads the intermediate record (CAPTURED)

`tools/m9-varying-transport-evidence.py --raster` over 814 raster passes
finds nine distinct stream-stride tuples and **every stride in every one of
them is `0x10`**, except the clip code's `6` and a trailing zero pad:

```text
  10 6 10                                  468
  10 10 6 10                               272
  10 10 10 6 10                             38
  10 10 10 10 10 6 10                        1
  10 10 10 10 10 10 10 6 10                  2
  ...
```

So an interpolated input is always a flat 16-byte-per-vertex array of its
own, and every multi-draw preprocessing chain ends in a draw that
*de-interleaves*: it reads the packed record through `0x74` and writes each
value out to its own `0x68002000` stream view.  `vs-varyings-4` is the
clearest case — its third draw is nine slots long and is a pure gather,
`r11 = mov r00`, `r13 = mov r02` … `r1f = mov r0e`, four inputs into four
targets — and its raster pass then binds four separate addresses
`113e8000 113db000 113ce000 113c1000` at stride `0x10`.

This **refutes** the lead recorded at the end of
`docs/parhelia-m9-vertex-output-target-re.md`, that the raster stream builder
needs to express "several slots bound to one object at different offsets with
a common stride".  It does not: the vendor never asks the raster stage to do
that.  Whether the hardware *could* — the stream stride is a real 16-bit byte
stride, proven by the clip code's 6 against everything else's 16 — is
**OPEN** and is one of the two experiments in section 8.

## 6. The vendor's ceiling is fourteen varyings (STATIC)

`MxMGLX.dll`, the GLSL linker's varying pass at `0x1055db40`.  It opens by
clearing a map and a high-water mark:

```text
0x1055db4b  8d8ff45b0000   lea  ecx, [edi + 0x5bf4]
0x1055db51  8d87bc5b0000   lea  eax, [edi + 0x5bbc]
0x1055db60  c700ffffffff   mov  dword [eax], 0xffffffff
0x1055db66  83c004         add  eax, 4
0x1055db69  3bc1           cmp  eax, ecx
0x1055db6b  75f3           jne  0x1055db60
0x1055db74  c70100000000   mov  dword [ecx], 0
```

`0x5bf4 - 0x5bbc` is `0x38`, so the map is **14 dwords**.  For each varying
the fragment shader reads it finds the vertex shader's declaration, checks
type and size, and then checks the allocation:

```text
0x1055dcef  8b10           mov  edx, dword [eax]        ; the varying's base slot
0x1055dcf1  035500         add  edx, dword [ebp]        ; + its size in slots
0x1055dcf4  83fa0e         cmp  edx, 0xe                ; 14
0x1055dcf7  0f8777010000   ja   0x1055de74              ; -> "Not enough resources
                                                        ;     to allocate varyings."
```

and then writes the vertex-to-fragment slot map one slot at a time,
`map[fs_base + k] = vs_base + k`:

```text
0x1055dd3d  8b4c242c       mov  ecx, dword [esp + 0x2c]
0x1055dd41  8b11           mov  edx, dword [ecx]
0x1055dd43  8b07           mov  eax, dword [edi]
0x1055dd4d  899481bc5b..   mov  dword [ecx + eax*4 + 0x5bbc], edx
```

The unit is a four-component slot: `vs-varyings-max` declares fourteen
`vec4` varyings and links with an empty log (CAPTURED, its `case.json`), and
fifteen would not fit the map.  The ICD nevertheless advertises
`GL_MAX_VARYING_FLOATS = 60`, fifteen slots (CAPTURED, the same field in all
112 `case.json` files of the GLSL corpus, and already recorded in
`docs/parhelia-m9-opengl2-static-closure.md`): **the advertised limit is one
slot above what the linker will allocate.**  The ICD builds its GLSL preamble
from a limits structure whose field order the emitter at `0x10689080` fixes —
`+0x00` lights, `+0x04` clip planes, `+0x08` texture units, `+0x0c` texture
coords, `+0x10` vertex attribs, `+0x14` vertex uniform components, `+0x18`
varying floats, `+0x1c` vertex texture image units, `+0x20` combined, `+0x24`
texture image units, `+0x28` fragment uniform components, `+0x2c` draw
buffers — but the structure is filled at run time and the values are not
static constants in the image (**OPEN**).

Fourteen is also what the transport does: `vs-varyings-max`'s preprocessing
chain is nine draws through a sixty-dword record, and its raster half is
three passes carrying 2, 6 and 6 inputs.

The Parhelia ICD `MtxPar/MTXOGL.dll` contains none of this — no
`gl_MaxVaryingFloats` preamble, no varying linker, no
`GL_ARB_shading_language_100` in its extension string (STATIC).  The varying
transport decoded here is M-series only, so a Parhelia cross-check cannot
separate family-specific fields from general ones for this feature.

## 7. What changed in the driver

One term, in `m9_draw.c`'s preprocessing block, gated behind the same
`M9X_VS_VARYINGS` knob that already defaults to one varying, so no default
behaviour moves:

* `m9_preprocess_ef()` takes the chain role instead of hardcoding place 1;
* the fused pass emits `0x00280000 | clip code | M9_PRE_ROLE_LAST` when an
  extra preprocessing draw precedes it and `… | M9_PRE_ROLE_ONLY` when it
  does not — `0x00288e00` and `0x00288800`, both CAPTURED, the first as
  `cap-attribs_14` seq 26 `c3=26` and the second as every single-draw pass;
* `m9_batch_test.c`'s `test_vs_extra_preprocess` asserts the fused pass moves
  from one to the other when the extra draw appears.

`m9-batch-test`, `m9-vs-compile-test` (23/23 compiled and verified, 2/2
refused) and `m9-fs-compile-test` (182 corpus shaders, 157 supported,
required exact 23/23) all pass (OFFLINE).

**This was a hypothesis with a captured witness, and the hardware refused
it** — section 11.  The chain place was not the only thing missing, so
section 8's second experiment is what is next.

What is deliberately **not** changed: the extra draw still writes separate
`0x68002000` stream views rather than a packed record.  That is a deviation
from the vendor, which always writes a record from a chain-opening draw, but
it is a deviation the raster side can consume, and changing both variables at
once would make the hardware run uninformative.

## 8. The experiments worth the card time, in order

1. ~~**The chain role alone** (section 7).~~  RUN AND REFUTED, section 11:
   `M9X_VS_VARYINGS=5` still delivers nothing and now stalls the engine.
2. ~~**The vendor chain shape**, if 1 fails.~~  BUILT AND RUN, section 12.
   The head writes the packed record, a third draw at place 3 with no clip
   code gathers it through `0x74 + k` / `0x78` / `0x79`, and its program is
   byte for byte `vs-varyings-4`'s `c3=27`.  No kernel change was needed
   after all: the raster half already binds `0x78` with a TEXTURE
   relocation, which the preprocessing half can reuse.  It renders nothing.
3. ~~**The single-word interpolant declaration** (section 2).~~  RUN, section
   11.  The hardware takes the one-word form and the driver now emits it, so
   an input costs one declaration word.  It does **not** carry thirteen
   inputs: a seventh has no interpolation register.  The gain is that the
   declaration budget stops binding at all, so six interpolants and four
   texture units now fit one pass.
4. **A sampler-only varying through a coordinate attribute.**  A varying
   whose only use is a texture coordinate can be declared as attribute
   `5 + unit` in one word (CAPTURED, `fs-tex2d`), where the same value used
   arithmetically is promoted to attribute 1 and two words (CAPTURED,
   `fs-texcoord`: `gl_FragColor = gl_TexCoord[0]` declares attribute 1).
   The driver already emits the attribute-5 form for texture coordinates it
   knows about, so this is a compiler classification change, not a new
   encoding.
5. **A wider raster stream stride**, only if 2 and 3 both hold: bind several
   raster streams into one packed record at different byte offsets with a
   common stride, which the stream binding expresses today (the clip code
   already runs at stride 6) and which no vendor pass uses.  It would remove
   the de-interleaving draw altogether.

## 9. The seven questions

1. **How many interpolants can one raster pass consume, and is four floats
   the maximum record?**  Six four-component inputs per raster pass
   (MEASURED, `docs/parhelia-m9-raster-stream-limit-re.md`), bounded by the
   fragment unit's six interpolation registers — not by the 16-word
   declaration budget at `22[11:8]`, which an input costs one word of, and
   not by any record width.  The
   four-float figure in the brief belongs to the *fragment* inter-pass record
   and is not a varying limit at all; the *preprocessing* record is a packed
   array of up to 60 captured dwords with a one-byte stride field.  The
   vendor's driver-wide varying limit is 14 slots (STATIC, `cmp edx, 0xe` at
   `MxMGLX.dll` `0x1055dcf4`) while it advertises 60 floats = 15 slots
   (CAPTURED).
2. **How does the vendor emit more interpolants than one pass holds?**  Not
   (a) and not (d).  It is (b) and (c) together, one level up from where the
   brief puts them: **several preprocessing draws share one packed
   intermediate record**, each writing up to four fields of it at its own
   dword offset, and a final route-fed draw de-interleaves the record into
   one flat 16-byte-stride stream an interpolant.  The raster stage is then
   split only when the *declaration* budget runs out, at six inputs a pass —
   `vs-varyings-max`'s fourteen varyings are nine preprocessing draws and
   three raster passes.
3. **What does the preprocessing pass write, field by field?**  Target `j`
   is the register pair `(0x11 + 4j, 0x13 + 4j)`; the low register supplies
   components 0 and 1 and the high register components 2 and 3, and the
   descriptor places them at `offset .. offset + 3` dwords of the record, or
   at `offset .. offset + 1` for a form-1 target.  `vs-texlod` proves the
   order: its first draw writes `r13 = texcoord.xy` into a four-dword record
   at offset 0, and its second draw's route reads dword **2** as the sampler
   coordinate.  The `x*w`, `y*w`, `w` model is the *fragment* inter-pass
   record's and is not touched here.
4. **Which field states the count, which the stride?**  Count: index `ef`
   bits 19, 21 and 23 enable targets 1..3, target 0 is always on, and bit 15
   enables the clip code (`docs/parhelia-m9-vertex-output-target-re.md`
   section 2).  Stride: the target descriptor's bits 7:0, in dwords —
   **an independent field, not a function of the pass count**.  Per-target
   placement: the same descriptor's bits 15:8.  On the raster side the count
   is `22[11:8] + 1` declaration words and each stream carries its own
   16-bit byte stride at `0x10 + k/2`.
5. **Ordering and packing rules.**  Per component, at dword granularity, in
   the order the compiler chooses — `cap-attribs_14` packs a two-dword field
   at dwords 0..1 and two four-dword fields at 2..5 and 6..9 of a ten-dword
   record, and the glamor composites pack two `vec2` coordinates into dwords
   4..5 and 6..7 and share dwords 8..9 as the unread upper half of both.
   Position and `w` occupy no fixed slot of that record: the clip position,
   the viewport position and the clip code are written by the chain's final
   draw into stream views of their own, at `b2`, `b4` and `b8`, and cost the
   raster side three declaration words rather than any record space.
6. **Are texture coordinate iterators a cheaper transport?**  Yes, but only
   for values a sampler consumes.  Attributes 5..8 declare in **one**
   declaration word (`0688`, `0c88` or `0d88`) against two for a general
   interpolant, so they are half the declaration budget; they are the same
   16-byte-stride stream underneath, so they save no record bandwidth at
   all.  A value used arithmetically is promoted to attribute 1..4/9..10 and
   two words — `fs-tex2d` declares attribute 5, `fs-texcoord` declares the
   same coordinate as attribute 1 (CAPTURED).  So routing smooth user
   varyings through them is possible exactly when the fragment program only
   samples with them, which the driver can already classify.
7. **Anything that cuts the pass count.**  Three things, in order of value:
   the single-word `0688` declaration for a general interpolant, MEASURED and
   implemented (section 11), which does not raise the six-input ceiling but
   frees every declaration word a pass was spending on its inputs; the
   sampler-coordinate classification of 6, which frees one
   declaration word per texture-fed varying; and allocating the *fragment*
   inter-pass record's slots by liveness rather than one a hand-over, which
   `docs/parhelia-m9-record-bandwidth.md` already costs out and which the
   descriptor's independent offset and stride fields make expressible.

## 10. Corrections this note makes to other documents

* `docs/parhelia-m9-raster-stream-limit-re.md` section 3 described the
  intermediate record as `0xee19 | slot * 4 << 8 | total_slots * 4`, i.e. as
  an array of four-dword slots.  It is a packed array of dwords with a
  two-dword form as well as a four-dword one; corrected in place.
* `docs/parhelia-m9-vertex-output-target-re.md`'s closing lead — that the
  raster stream builder must bind several slots to one interleaved object —
  is refuted by section 5; corrected in place, with the chain-role reading
  that replaces it.
* `docs/parhelia-m9-record-bandwidth.md` treats the record stride as a
  function of the pass count.  It is an independent descriptor field, so a
  liveness allocation needs no new hardware capability; noted in place.

## 11. The two hardware runs (MEASURED 2026-09-19)

Both on the dual-DisplayPort M9128 at 192.168.1.111, module
`/root/tmp/m9x_drm.ko`, each run opened with a known-good fixture to prove the
engine alive and each stall followed by a reboot before the next measurement.

### Experiment 1: the chain role does not make the second draw deliver

`M9X_VS_VARYINGS=5`, `tools/m9-mesa-glsl.c` fixtures, one process a case, the
Mesa build of commit `8763f81` verified on the card by dumping its own
submission: the extra draw carries `ef = 0x00000200` (`M9_PRE_ROLE_FIRST`) and
the fused pass `ef = 0x00288e00` (`M9_PRE_ROLE_LAST`), which is the word
section 7 added.

| cap | case | result |
|---|---|---|
| 1 | `texture` | exact — the engine is alive |
| 1 | `varying`, `vary2`, `vary3`, `vary4`, `vary5` | all exact through the draw module |
| 5 | `varying`, `vary2` | exact; neither emits an extra draw |
| 5 | `vary3` | **every pixel wrong, kernel stream timeout** |
| 5 | `vary4` | every pixel wrong, stream timeout |
| 5 | `vary5` | the context is already gone |

`vary3` was re-run alone after a reboot, behind a passing `texture`, and
failed the same way, so the result is not a side effect of an earlier stall.

The run also corrects the fixture table in
`docs/parhelia-m9-vertex-output-target-re.md`: Mesa's varying optimiser now
folds `vary3`'s third varying into the second as a swizzle, so its raster pass
declares **two** interpolants and its extra preprocessing draw binds **one**
target.  The minimal possible second preprocessing draw — a single stream
target — is therefore what fails.

**What this kills.**  The chain-role hypothesis of sections 4 and 7: `ef` bits
11:9 were not the missing term.  With `0x88`'s pair mask and the barrier
between the draws already refuted (that note's appendix), what was left is the
deviation section 7 deliberately kept: the extra draw writes separate
`0x68002000` stream views where every captured chain-opening draw writes a
packed intermediate record that a later route-fed draw de-interleaves.  That
was section 8's experiment 2, it was built and run, and it does not deliver
either -- section 12.  The driver's default stays at one varying.

### Experiment 3: the one-word declaration works, and six is the register file

`tools/m9-user-raster-streams.c`, extended past six inputs and given
`M9RS_ONEWORD` and `M9RS_ATTRS`.  Full tables in
`docs/parhelia-m9-raster-stream-limit-re.md` section 6; in short:

* one `0x0688` word an interpolant renders exactly what the `0d88`/`06b6`
  pair does, 21 of 21 sweep cases, and drops six inputs from fifteen
  declaration words to nine;
* a seventh input reads garbage whatever attribute number it is given, because
  the interpolation registers `0x40 + 8 * i` are handed out in ascending
  attribute order and there are six of them;
* index `88` bits 31:24 hold six interpolant flags, not eight: setting the
  seventh corrupts input 4 and setting all eight corrupts every input.

The driver emits the one-word form as of the Mesa commit "one declaration word
an interpolated input"; the whole `m9-mesa-glsl` suite is 211 of 211 with it.

## 12. The packed intermediate record does not deliver (MEASURED 2026-09-19)

The last untested difference between the clean-room chain and every captured
one was the shape of the hand-over.  It was built in full and run on the
M9128.  **It renders nothing, and neither do the two smaller steps toward
it.**

### What was built

`M9X_VS_RECORD` selects three shapes, all emitted only above
`M9X_VS_VARYINGS=1`:

| mode | the head's targets | between head and fused pass |
|---|---|---|
| 0 | one separate `0x68002000` stream view a varying | nothing |
| 1 | those views, with a record descriptor | nothing |
| 2 | one packed record, `ee19`, stride `4n`, offset `4k` | a route-fed gather draw |

Mode 2 is the captured chain: the gather runs at chain place 3 with neither
bit 11 nor the clip code (`ef = 0x00000600` plus its target enables), binds
one route at `0x74 + k` with the record's base at `0x78` and a descriptor of
the same shape at `0x79`, and writes each varying out to its own
`0x68002000` view, which the raster pass then reads unchanged.

Two findings came out of building it, both worth keeping whatever the result:

* **No kernel change was needed.**  Section 8 expected `m9_index_allowed()`
  to have to admit `0x74..0x79` in a preprocessing range.  It already does,
  and the raster half already binds `0x78` with a **TEXTURE** relocation, so
  the preprocessing half binds the record the same way.
* **Every captured native program marks W1 of slot `n - 4`**, the same rule
  `m9_fs_compile.c`'s `finish_slots()` applies to fragment programs, and pads
  to four slots to have one.  The fused pass inherits that mark from its
  appended tail, which sits exactly four slots from the end, so it was never
  noticed missing -- but **the tail-free second head has carried no mark
  since it was written**.  Both tail-free programs place it now.  With it,
  the four-varying gather the driver emits is byte for byte the vendor's own
  entry 39 of `vs-varyings-4`, which `m9-vs-compile-test` now asserts.

### The runs

Each step was a fresh boot, and each is one hold of the hardware lock:
a known-good `texture` fixture, then the case, then `texture` again.  Every
step's submitted stream was dumped from the card and decoded before its pixels
were believed.

| mode | `vary3` | stream timeout | the card afterwards |
|---|---|---|---|
| 0 | nothing rendered, 76800 of 76800 pixels differ | once yes, once no | stops rendering |
| 1 | nothing rendered | none logged | stops rendering |
| 2 | nothing rendered | none logged | stops rendering |

Mode 0 was run twice against the same library, each run bracketed by its own
passing `texture`, and the two disagree on the timeout alone: one logged
`M9 kernel stream timeout` and the other logged nothing, with the same
pixels and the same dead card either way.  **So a run that logs no timeout
has not therefore completed.**  Only the fixture that follows says whether
the engine still executes, which is why every step here ends with one.

`vary3` is the minimal case: Mesa folds its third varying into the second as a
swizzle, so its raster pass reads two interpolants and its extra draw binds a
**single** target.  The same fixture is exact through the draw module in the
same binary.

The mode-2 stream the card ran, decoded from its own dump:

```text
[  7] ef = 00000200   head,   c3 = 30, b1 = ee190004   (record, stride 4, offset 0)
[120] ef = 00000600   gather, c3 = 37, b1 = 68002000
      74 = 0302010c   78 = <record base>   79 = ee190004
[237] ef = 00288e00   fused,  c3 = 0d
[369] ef = 00000000   raster pass
```

which is the captured grammar in every field this note decoded.

### The conclusion

Four shapes have now been refused by the hardware: index `0x88`'s register
pair mask, the barrier between the draws, the chain role in `ef`, and the
packed record with its route-fed gather.  **The second preprocessing draw
needs something that is not in any capture this tree holds.**  Anything
further on *that* route should start from a new capture of a vendor chain
rather than from another reading of the ones here.

The second varying did not need it: section 13 carries two of them in the
**one** preprocessing draw the driver already runs.

### A note on method

The first attempt at modes 1 and 2 was run against a **stale deployment** --
a generated deploy script had not been rewritten as intended, so the target
kept an older library and reproduced the older failure exactly.  Dumping the
submitted stream is what caught it: the dump held two preprocessing draws
where the build under test emits three.  Every result above is from a run
whose stream was decoded and matched the intended shape first.  A pixel
result on this path is worth nothing without that check.

## 13. Two varyings in one draw, without a chain (MEASURED 2026-09-19)

The cap is **two**, not one, and the draw that carries them is the fused pass
the driver has always emitted -- same `ef`, same index `88`, same three bound
targets and clip code, same packet count.

Two facts make it work, both measured on the M9128 with a known-good fixture
before and after each hold:

* **A preprocessing pass may enable output target 3 or the clip code, not
  both.**  Eight configurations with `ef` bit 23 set were run -- clip code
  bound with pair masks `0x3f00`, `0xff00` and `0xffff`, index `ba` at 0, the
  clip code left unstated, the chain role forced to the captured head word,
  and all four targets written into one packed record -- and every one stalls
  the engine.  Binding `b6` without setting bit 23 does not stall.  The one
  that runs is the clip code disabled the vendor's own way, `b8 = 0`,
  `b9 = 0xb8010001`, `ba = 0`.  That is exactly what the corpus shows: none
  of the 91 captured four-target passes enables bit 15.
* **The raster stage does not need the clip position.**  Dropping attribute
  13's declaration and its stream from a three-target pass renders the same
  pixels, as does dropping the clip code at attribute 12.  556 of 556
  captured raster passes declare all three anyway.

So the spare target is **1**, not 3.  Its register pair `0x15`/`0x17` is
written by two tail slots with no reader; pointing both at the tail's own
scratch `r1c` -- one decoded field each -- frees the pair for the head, and
index `88` stays the captured `0x0300bf00` because `r1c` was already a
declared temporary.  `b2` then binds the second varying's transient view and
the raster pass declares no attribute 13.

`vary3`, whose two `vec4` varyings Mesa packs into two slots and whose
fragment shader reads every component of both in products, is pixel-exact
through this path with `M9X_VS_VARYINGS=2`.

**What this does not reach.**  A third varying still needs a second
preprocessing draw, and that draw has still never delivered a stream.  The
tail writes targets 0, 1 and 2 and the fourth cannot be enabled beside the
clip code, so **two is the ceiling of one draw**.
