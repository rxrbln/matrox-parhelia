<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# M9 native vertex backend: what it compiles, and on what evidence

`src/gallium/drivers/m9x/m9_vs_compile.c` turns a NIR vertex shader into the
head of a native preprocessing program.  The instruction set it emits is the
one `docs/parhelia-m9-native-vertex-isa.md` decodes; this document records the
compiler's own contracts, what each form rests on, and what is not covered.

## Result

The driver runs the compiled program on the GPU: section 8 records the
integration, what it measures and what it still cannot do.

Twenty-nine vertex shaders, glmark2's `bump-poly.vert` among them, compile
and verify offline (OFFLINE, `m9-vs-compile-test`), six of them with a
coordinate stream (section 4a) compiled beside the default layout and checked
stream by stream -- four with the coefficients in the job's constant span and
two with them in the source record.  `mvp * position` with a
colour varying compiles to **36 slots, the same length the vendor compiler
emits for it**, with the transform in the vendor's own eight instructions.

## 1. What the compiler produces, and what it appends

The compiled head does three things: it reads the declared attribute
registers, it computes the clip position into `r00`/`r01`, and it computes one
varying into `r02` (x,y) and `r13` (z,w).  `m9_vs_emit()` then appends slots
14..38 of `m9_passthrough_c2_program` **verbatim** and the stop pair.

That tail is the reciprocal of `w`, the perspective divide, the viewport scale
and bias, the depth range and the clip code.  It is copied rather than
re-derived for two reasons the reverse engineering settled: it is the same 25
slots in every captured single-pass program, and one of its instructions
(the three-source `op2 = 0x00` form at W1 `0x8c01cd10`) is undecoded.

Its interface to the head is exactly:

| register | holds |
|---|---|
| `r00` | clip x, clip y |
| `r01` | clip z, clip w |
| `r02` | varying x, y -- the tail moves it to the output register `0x11` |
| `r13` | varying z, w -- an output register the head writes itself, as the vendor does |

`r03` is reserved: nothing in the vendor program writes it.  The original
reason -- that the partial dot names it in its `c` field -- is wrong: `c` is a
constant pair index (`parhelia-m9-native-vertex-isa.md` section 3), so `r03` is
probably free, but that is UNTESTED and it stays reserved.  Everything the
tail writes -- `r04`..`r0f`, `r19`, `r1b`..`r1d`, `r21` -- is free for the head,
which runs first.  The temporary pool was `r04`..`r12` and `r14`..`r21`, 29
registers; `0x22` upward is not CAPTURED as writable and nothing here widens
the pool on a guess.

**Since 2026-10-02 the pool is 23 registers.**  `0x11`, `0x15`, `0x17`, `0x19`,
`0x1b` and `0x1f` read back wrong when used as temporaries (MEASURED by
steering a reciprocal across the pool: seeded in any of those six it is wrong,
in `0x12`, `0x14`, `0x16`, `0x18`, `0x1a`, `0x1c`..`0x1e` or `0x20` it is right;
padding slots after the write does not help, so it is not latency).  The
mechanism is OPEN.  DERIVED hint: they are odd registers from `0x11` up, `0x11`
is the varying x,y output the tail fills, and the vendor uses `0x11`/`0x15`
only as partial-dot results read within two slots -- consistent with output or
accumulator registers that do not hold a value.  Not fitting yet: `0x13` (an
output) works as a temporary, and `0x1d` is in the tail's write set.  A probe
writing known values to each odd register and reading both the register and
the varying output would settle it and may return registers to the pool.

Two more 2026-10-02 corrections (MEASURED): the partial dot subtracts
`k[c].s0` (it now names a zero literal pair), and `frcp`, `frsq`, `fsqrt` and
the `fdiv` divisor read source component 0 in every lane, so each component
runs its own chain.  All three were invisible offline because the simulator
modelled them the same wrong way; it is corrected (`divide-vector`,
`rsq-sqrt-vector`, `dot-varying` cases).  71 glsl `vs-*` piglit tests failed
only on the native path; 73 of the 85 failing ones pass now.

A register is reclaimed when nothing live holds it: a NIR definition dies
past its last use, and an *intermediate* of the instruction being emitted
dies when that instruction ends.  The second half of that rule was missing
until 2026-09-20 -- every intermediate stayed live for the whole program,
so a register that had once carried one was never handed out again.  The
allocator ran out at 28 of its 29 registers held by dead intermediates on
glmark2's `buffer` vertex shader (four clip-space transforms and three edge
distances), which is what `c->temp_base` fixes: only a temporary raised
past the mark the current instruction started at is live.  OFFLINE over the
whole `m9-vs-compile-test` corpus the change costs one slot in one case
(`five-varyings`, 46+20 to 46+21) and every case still verifies against the
NIR reference.

## 2. Constants

The tail names constant pairs 9..21 by absolute index, so user constants take
pairs 0..8 and then 22 upward: `m9_vs_const_pair()` is that map, and
`m9_vs_const_count()` reports the span the pass needs, never fewer than the 22
pairs the tail occupies.  The driver keeps filling pairs 9..21 from
`m9_draw_params_init()`, which already computes exactly those words.

A constant pair is either two literals or two constant-buffer components; a
uniform paired with a literal zero keeps the pair addressable through the
`M9_VS_UNIFORM_ZERO` offset, which is what a three-component dot against a
uniform vector needs.

## 3. The value model

A native register holds two scalars and every instruction carries a
destination scalar mask, so a vector value occupies one register per component
pair -- the same model the fragment backend uses, because the two units share
the instruction encoding field for field.

Each register scalar records *what it holds*: a component of a declared
attribute, a component of a NIR definition, a literal, a constant-buffer
component, or an intermediate of the instruction being emitted.  An operand is
then a register whose two scalars are the two source components in some order,
found by scanning; the two-bit operand swizzle names the order (bit 1 is the
source scalar of result scalar 0).  When no register carries them, up to two
moves gather them into a fresh one.

Registers are reclaimed by liveness: a definition's last use is the
instruction index computed in a backward pass, and a register is free once no
scalar it holds is still live.  A definition folded into a multiply-add, and
one that only feeds a vector or move, extends its sources' liveness to its own
consumer instead.

The attribute declaration pads a short input with the GL default `(0, 0, 0, 1)`,
which makes `vec4(attribute.xyz, 1.0)` a register pair the program already
has -- no instruction, and the four-row transform reads it directly.

## 4. Two things the backend does that Mesa does not

**Multiply-add folding.**  Mesa has no pass that fuses `fadd(fmul(a, b), c)`
into `ffma`; drivers are expected to do it.  The hardware's multiply-add takes
its multiplier from a constant pair, which is exactly what a matrix column is,
so the backend folds a product whose result feeds nothing else and whose
constant side is the same operand on every lane.  `mat4 * vec4` -- which GLSL
gives as a linear combination of columns, not as dot products -- therefore
compiles to one multiply and three multiply-adds per lane, the vendor's eight.

**Coordinate placement and scaling.**  See section 4a; it is the second
thing Mesa does not do for a driver, and it is what lets a textured draw run
its transform on the engine at all.

**Output pinning.**  When both scalars of an output register come from one
lane that a single instruction writes whole, that instruction writes the
output register itself and the two moves disappear.  A lane written by a dot
product, a reciprocal or a reciprocal square root is excluded: those forms
write the first scalar only.

## 4a. Output streams, and the coordinate one

`m9_vs_compile()` takes the streams the program must write -- `struct
m9_vs_outputs`, one entry an output target -- rather than deriving them from
the varyings the shader stores.  The layout belongs to the (vertex shader,
fragment variant) pair, because it is the fragment variant that says which
varyings the raster pass interpolates and which pair of which varying each
sampler reads, so the shader keeps one compiled program per layout
(`struct m9_vs_variant`).  Passing NULL asks for the old shape, one varying
stream a stored varying in store order, which is what the offline corpus and
the pass-through colour image use.

A **varying stream** is what it always was: the four components of the
varying into the target's register pair, low register components 0 and 1.

A **coordinate stream** carries the two scalars a sampler reads.  The CPU
used to produce it per vertex in `m9_scale_coords` -- pack the pair down out
of the varying, fold the sampler's affine transform in, divide by a
projector, scale to the render target's addressed span -- and the program now
does all of it but the divide, in one instruction:

```text
low  = (V[c], V[c+1]) * k_scale + k_bias      VS_MAD_RKK, CAPTURED s26
high = low                                    VS_MOV, so the stream entry
                                              carries no stale register
```

`c` is wherever the linker put the pair, including the straddling `c = 1`
where the two scalars sit in different registers and the operand builder's
two gather moves fetch them.

**Where the coefficients come from decides whether two draws can share a
job.**  As constant pairs of their own kind (`M9_VS_CONST_COORD`,
resolved per job by `m9_vs_resolve_consts()`) they are part of the job's
constant span, and `m9_job_reserve()` ends a job when that span changes --
so a scene that samples render targets of different addressed spans got one
job a draw.  glmark2's `desktop:shadow` is exactly that scene and it cost
4 fps.

So a stream whose layout says `coeff_record` takes them from a
**source-record slot** instead: one more declared attribute, four floats the
record writer puts in every record of the draw
(`struct m9_vs_source_attr::value`, filled from `m9_vs_coord_coefficients()`
in `m9_native_vs_records()`), and two register instructions in place of the
constant multiply-add:

```text
t    = (V[c], V[c+1]) * A[0..1]               VS_MUL_RR, CAPTURED s23
low  = t + A[2..3]                            VS_ADD_RR
high = low                                    VS_MOV
```

Records are per draw already, so a draw with its own span appends records to
the pending job rather than ending it.  The slot is spent whenever one of
the four attributes is free -- not when the coefficients happen to differ,
because whether they differ is a property of the draw and a layout that
followed it would recompile, and split the job, every time a scene mixed a
padded render target with an ordinary texture.  A shader that already
declares four attributes keeps the constant route.

A coordinate stream therefore costs one multiply-add, one move and two
constant pairs, or one multiply, one add, one move and one record slot.

What stays refused, each with its own reason: a **projective** coordinate,
whose per-vertex divide is wrong in 75373 of 76800 pixels (MEASURED, fixture
`projtexz`) and which a two-scalar stream cannot express; a **shadow or
volume** fetch, which reads a third interpolated component; a coordinate an
earlier raster pass produced; and a fragment program split across raster
passes.

**A coordinate inside a varying the pass already interpolates costs no
stream at all.**  The raster declaration word's low twelve bits are four
component selectors, so the sampler's attribute can be declared over the
varying's own stream slot reading components `c` and `c + 1`
(`m9_coord_select()`), and the layout drops the coordinate stream entirely.
It applies where the coefficients are the identity -- a shared stream
carries the varying's own values, unscaled -- which is the common shape and
glmark2's `texture` among it.  MEASURED on the M9128: six textured fixtures
exact either way, and `texture` native at the draw module's own frame rate
(`docs/parhelia-m9-raster-stream-limit-re.md`).  `M9X_COORD_SHARE=0` puts
the separate stream back.

`M9X_VS_TEXTURE=0` puts every sampling draw back in the draw module, which
is the A/B arm of this change rather than a gate.  `M9X_VS_COORD_PAIR=1`
lifts the one gate that is left: a coordinate stream that cannot share --
because it scales, or because its varying is not one the pass
interpolates -- takes the second output target beside another stream, and
that shape costs frame rate on the one benchmark measured with it
(`docs/parhelia-m9-glmark2-status.md`).

## 5. Instruction forms and their provenance

Reconstructed byte for byte from `m9_passthrough_c2_program`, so **CAPTURED in
the vertex unit**:

| form | op2 | slot |
|---|---|---|
| move register | 0x14 | s16 |
| move constant | 0x14 | s29 |
| multiply register by constant | 0x01 | s30 |
| multiply register by register | 0x01 | s23 |
| multiply-add, both addend and multiplier constant | 0x02 | s26 |
| partial dot, subtracting `k[c].s0` | 0x10 | s5 |
| dot completion | 0x0e | s7, s12 |
| compare against a constant pair | 0x00, UNIT 1 | s14 |
| reciprocal seed, residual, Newton step | 0x11, 0x01 cls 5, 0x13 | s15, s17, s18 |

CAPTURED in a vertex program as well, which
`docs/parhelia-m9-vertex-isa-gaps.md` section 6 establishes over the whole
corpus: multiply-add over three registers (`op2 = 0x02`, `e = 0x3f`),
**maximum** (`op2 = 0x19` class 3), **negate** (`op2 = 0x15`), **select**
(`op2 = 0x1a` class 4) and the **reciprocal-square-root chain** (seed
`op2 = 0x11` class 5, step `op2 = 0x13` class 7), all in `vs-lighting`,
`vs-math`, `vs-builtin-light`, `ff-fog-exp` and `ff-fog-exp2`.

Captured in no vertex program, but **MEASURED on the vertex unit** by
`tools/m9-mesa-glsl.c`'s `vsprim:*` probes on 2026-09-20: add and subtract of
two registers (`vsprim:sub`), **minimum** (`op2 = 0x19` class 2) and maximum
against a constant (`vsprim:minmax`), each exact in all 1200 cells.  Saturate
and fract/floor/ceil remain stated by no capture; `fract` is exercised by
every vertex sine and measured exact through it
(`docs/parhelia-m9-vertex-isa-gaps.md` section 1.5).

This paragraph used to argue that because the backend had emitted these forms
on the default path since 2026-09-17, "a form that did nothing would have
shown as a wrong scene rather than silently".  That was wrong, and the
subtract is the counter-example: its two operand fields were swapped for days
without a wrong scene, because nothing on the default path reached one.  The
form computes **the `a` operand minus the `c` one**; a comparison against a
register, the complement of a condition and both halves of a two-register
minimum or maximum were all inverted by it, and each of those is rare in the
vertex programs that were running.  `docs/parhelia-m9-vertex-isa-gaps.md`
section 8 has the measurement.

Absolute value is a compare, a negate and a select, and square root is the
argument times its reciprocal square root with the vendor's zero guard --
both the shapes the fragment backend already uses.

## 6. Limits

* **Four attributes**, the coefficient slot of a record-routed coordinate
  stream (section 4a) counted among them.  Index `22` carries the declaration word count minus one
  in bits 11:8 -- four bits, so **sixteen words per pass**, not eight.  Four
  attributes are CAPTURED in one pass: every `fs-*` case of the vendor corpus
  declares four in nine index-`23` words over one 64-byte record and its vertex
  program reads `46 47 4e 4f 56 57 5e 5f`.  Five is refused for a decoded
  reason, not a guessed one: the vendor's attribute numbering **skips 5..8**,
  which are the four texture coordinate sets, and resumes at 9, so the register
  rule `0x46 + 8*(a - 1)` reaches `0x86` -- which does not fit the seven-bit
  `b`/`c`/`d` operand fields.  The linear rule provably stops at four.  (The
  vendor stack does run fifteen attributes, so beyond four it must use another
  mechanism; no capture isolates it.)
* **Two output streams.**  The fused pass binds output targets 0 and 1 -- the
  second is the clip position's, which the raster stage does not need
  (MEASURED, `parhelia-m9-varying-transport-re.md` section 13) -- so one
  preprocessing draw carries two.  A stream is either a varying the raster
  pass interpolates or a texture coordinate (section 4a), and a varying read
  both ways costs two.  A third needs the second preprocessing draw, which
  has never delivered a stream on this board.
* **One basic block.**  Control flow is refused.
* **No sine or cosine by default.**  Neither has an instruction: the vendor
  lowers both, and `docs/parhelia-m9-vertex-isa-gaps.md` section 1 carries the
  coefficient table it lowers them with, read out of `MSerX/MxMD.dll`.  The
  same reduction and polynomial are rebuilt here from forms the backend
  already emits, behind `M9X_VS_SINCOS`, **default off** until the engine has
  run the sequence.
* **No integer arithmetic**, and none exists; `iand`, `ior` and `inot` over a
  one-bit definition are accepted as the float lowerings the fragment backend
  uses, and a 32-bit bitwise operation is refused.
* **No logarithm or power.**  `exp2` is CAPTURED on the vertex unit -- the
  vendor's own `ff-fog-exp` and `ff-fog-exp2` vertex programs carry the seed,
  polynomial, two steps and scale with the same words, the same operand wiring
  and the same three coefficient pairs the fragment unit uses -- and the backend
  emits it.  `log2` appears in no captured vertex program, and the fragment
  unit's own log2 chain cannot be validated offline (its polynomial slots write
  the first scalar while their chained operand's swizzle reads the second), so
  `log2` and `pow` stay refused rather than ported on a guess.
* A shader outside any of this fails with a reason and the draw module runs it,
  which is what the driver does today.

The point-size store the compatibility linker adds to every shader is accepted
and ignored when it writes the constant 1.0, and refused otherwise: the
preprocessing pass has no point-size output.

## 7. How it is verified

`m9-vs-compile-test` compiles each case with Mesa's GLSL compiler, lowers it
the way st/mesa hands a vertex shader to the driver, and then compares two
evaluations of the same random inputs:

1. the NIR the backend consumed, interpreted with Mesa's own constant folder;
2. the emitted program, run through a simulator that decodes the instruction
   words the backend wrote -- the attribute declaration included, so the
   selectors are checked as well.

A case that names an output layout is compiled a second time with it and
every stream of that program is checked against the same reference: a
varying stream against the varying, a coordinate stream against
`varying[component..component+1] * scale + bias` with the coefficients the
driver would resolve, whichever route carries them.

Until 2026-09-19 the coordinate cases checked **nothing of the kind**: the
`COORD_STREAM()` initialiser named four of `struct m9_vs_output`'s five
fields, so `true` landed in `pass` and `coordinate` stayed false, and each
of them compiled an ordinary varying stream -- no case produced an
`M9_VS_CONST_COORD` pair at all.  The gate was green because it was
verifying the wrong program.  Corrected, the four cases compile real
coordinate streams and two more cover the record route.

The simulator treats the reciprocal and reciprocal-square-root seeds as exact.
Newton refinement from an exact seed is a fixed point, so the chains still have
to carry their operands through the right registers to agree; what is not
checked is the hardware's approximation itself.

```sh
ninja -C objs-m9x-llvm src/gallium/drivers/m9x/m9-vs-compile-test
./objs-m9x-llvm/src/gallium/drivers/m9x/m9-vs-compile-test
# M9_VS_DUMP=1 prints the NIR, M9_VS_PROGRAM=1 the emitted slots,
# M9_VS_TRACE=1 every register the simulator writes
```

## 8. Integration: what the driver does with it

Since 2026-09-17 the driver runs the compiled program on the GPU.  The
decision is per draw, taken in `m9_native_vs_draw()` before the draw module is
entered; when any part of it says no the draw falls through to the draw
module unchanged.  `M9X_NATIVE_VS=0` disables the path outright.

### 8.1 The predicate

In the order it is evaluated, cheapest first, because a draw that falls back
must not pay for the decision:

1. a compiled program exists, and declares at most `M9_VS_HW_ATTRIBUTES`
   attributes (section 8.6);
2. the bound fragment shader samples no texture and reads at most one
   varying -- read straight from its `shader_info`, before any variant of it
   is compiled;
3. one instance, no primitive restart, a topology the raster pass expresses
   with the provoking vertex the hardware uses (`m9_native_topology()`), and
   a rasterizer state the pass carries: no discard, user clip plane, half-z
   clip, disabled depth clip, polygon offset, polygon stipple, non-fill
   polygon mode, and no wide, smooth or stippled line;
4. every declared attribute resolves to a `pipe_vertex_element` over a
   linear, non-instanced source whose whole vertex range lies inside the
   bound buffers (`m9_vs_source_layout_init()` and `m9_vs_source_in_range()`,
   which report the reason they refuse);
5. after the job key is built: the fragment variant reads no texture, and its
   one varying is the one the vertex program writes.

The vertex element that feeds attribute *i* is **not** its NIR
`driver_location`: `st_atom_array` builds one element per input the shader
reads, in `inputs_read` order, so it is `util_bitcount64(inputs_read &
BITFIELD64_MASK(location))`.  `m9_vs_attr_element()` is that map.

### 8.2 What a native job changes

* **Source records.**  `m9_vs_source_records()` writes the raw attributes
  instead of the draw module's transformed position and colour.  The job key
  carries the program, so a job never mixes the two record shapes, and
  `ctx->record_floats` is the stride of the pending job rather than a
  constant.
* **Declaration.**  `m9_vs_decl_words()` supplies the index-`23` words and
  index `10` the record span; both used to be the pass-through's four words
  and fixed stride.  `m9_preprocess_control()` computes index `22` from the
  word count, which reproduces the captured `0008040e` at five words.
* **Constant span.**  `m9_vs_const_span()` places the resolved user constants
  at their pairs, leaving 9..21 as `m9_draw_params_init()` set them.  Indices
  `8c`, `8d` and `8e` are computed from the pair count -- they were the
  captured 22-pair constants, which would have silently truncated the span of
  any program needing pairs 22 and up.
* **Viewport.**  The appended tail applies constant pairs 9..12 to the divided
  position.  The draw module applied the pipe viewport itself and those pairs
  were always the target's own; a native pass needs the viewport actually in
  force, so `m9_draw_params_viewport()` rewrites them per job from
  `pipe_viewport_state`.  Without this every FBO render is upside down,
  because the state tracker flips the viewport for a window framebuffer and
  not for an FBO.
* **c2 image.**  `m9_vs_c2_image()` assembles the fragment slots with the
  compiled vertex program behind them.  One image per fragment variant, keyed
  by a never-reused serial on the vertex state, so a stale pointer cannot
  match; the pass-through fragment image gets its own.
* **Varying.**  The preprocessing pass writes one varying, to transient view
  0.  `m9_varying_from_pass()` routes the fragment program's varying there
  whatever its slot, instead of only `VARYING_SLOT_COL0`, and no CPU stream
  is written at all.

### 8.3 Results (MEASURED, M9148, 2026-09-17; superseded for the textured
scenes by `docs/parhelia-m9-glmark2-status.md`'s eighth standing)

`tools/m9-mesa-vs.c` renders each case into a 320x240 RGBA8 FBO and compares
every pixel a full pixel inside a predicted triangle against a CPU reference
that applies the same transform, divides by w and interpolates
perspective-correctly.  A pixel any triangle might or might not cover is not
predicted and not checked, so no edge rule is under test.

14 of 14 cases pass with the path on and with it off: `inline mvp persp scale
flat strip indexed attr3 attr4 attr4b vbo viewport depth cull`.  Eleven of
them run natively; `attr3`, `attr4` and `attr4b` fall back for section 8.6.
Worst per-pixel error 1.14/255.  `persp` carries a real `w` row, so the
reciprocal chain and perspective-correct interpolation are under test.

The fragment corpus `tools/m9-mesa-glsl.c` is 90 of 90 with the path on and
with it off, no kernel stream timeout either way.  That run is what justified
making the path the default; the knob remains for A/B.

glmark2, 800x600 windowed on the glamor server, off -> on:

| scene | off | on | |
|---|---:|---:|---:|
| build use-vbo=false | 227 | 508 | +124 % |
| build use-vbo=true | 254 | 565 | +122 % |
| shading gouraud | 138 | 334 | +142 % |
| shading blinn-phong-inf | 141 | 296 | +110 % |
| bump high-poly | 42 | 111 | +164 % |
| conditionals 0/5 | 604 | 818 | +35 % |
| conditionals 0/0 | 652 | 816 | +25 % |
| function low | 506 | 604 | +19 % |
| ideas | 242 | 219 | -10 % |
| pulsar | 855 | 814 | -5 % |
| texture (all three) | ~1050 | ~1040 | -1 % |
| glmark2 score | 512 | 568 | +11 % |

ioquake3 timedemo at 640x480: 115.2 fps off, 117.0 fps on (387 frames both).
Every Quake III draw samples a texture, so the path does not apply there and
the number only shows that the predicate costs nothing measurable.

The two regressions are the cost of the design rather than noise, and both
runs reproduced them.  A native job's constant span is the vertex program's
uniforms, so a draw that changes them ends the job; `ideas` and `pulsar` give
each object its own transform, which is one job per object where the draw
module needed none.  Carrying several spans in one job is what would fix it,
and the pass binds one.

### 8.4 The record carries four components per attribute

Every attribute occupies a whole 16-byte slot and the record writer fills the
components the shader does not read with the GL default `(0, 0, 0, 1)`, so
the declaration selects only components 0..3.  Two reasons:

* every captured vendor record places its attributes at dword offsets 0, 4, 8
  and 12 of a 64-byte record, so 16-byte slots are the captured shape and a
  tighter packing is untested;
* the compiled head reuses the padded scalars as the literals `0.0` and `1.0`
  (section 3), and with the record carrying them that reuse rests on data the
  driver wrote rather than on declaration selectors `4` and `5`, which appear
  in a captured *declaration* but which no captured vertex program is known to
  read.

Packing the record tighter is an untested optimisation, not a known-good one.

### 8.5 A literal argument is folded, not run through the chain

The offline case `literal-xy` -- `gl_Position = vec4(pos.x / 160.0 - 1.0,
...)`, which Mesa leaves as `fdiv` by a constant -- compiled to the captured
reciprocal chain over a materialised literal and produced a result exactly
four times too large, in the simulator and on the hardware alike (all 74892
checked pixels wrong).  A reciprocal over a *register* operand is correct
(`divide` offline, `persp` on hardware), so the defect is specific to a
compile-time literal reaching `emit_rcp()`.

`build_alu()` now folds a literal argument of `frcp`, `frsq`, `fsqrt` and
`fexp2`, and a literal divisor of `fdiv`, at compile time.  That is correct by
construction, removes six slots, and puts the wrong path out of reach of
GLSL.  **The defect inside `emit_rcp()` itself is not root-caused**; a probe
that materialises a literal into a register and runs the chain over it is
what would settle it.

### 8.6 Four attributes reach the hardware, and how the third was fixed

`M9_VS_HW_ATTRIBUTES` is **four**.  A third and a fourth attribute used to
draw from neither their own value nor a decoy -- `m9-mesa-vs attr3`, `attr4`
and `attr4b` missed every one of 67156 checked pixels -- and the register
rule `0x46 + 8 * (a - 1)` was right all along: the pass stated a **fixed
count of two** in index `0x88` bits 27:24.  With that count driven from the
declaration all four attributes read exactly (MEASURED on the M9128:
`attr4-r5e` is 100 % of every triangle interior and the three lower
registers each hold their own attribute's decoy;
`docs/parhelia-m9-vertex-output-target-re.md` carries the run).

Five is refused for a decoded reason: the vendor's attribute numbering skips
5..8, the four texture coordinate sets, and resumes at 9, whose register
`0x46 + 64` does not fit the seven-bit operand fields.  The vendor stack does
run fifteen attributes, so beyond four it uses another mechanism; no capture
isolates it.

### 8.7 What the path still cannot do

* **Two output streams**, so a shader whose raster pass reads three -- two
  varyings and a coordinate, say -- falls back.  A third needs the second
  preprocessing draw, and that draw has never delivered a stream on this
  board (`parhelia-m9-varying-transport-re.md` section 12).
* **A projective texture coordinate** and a shadow or volume fetch's third
  component (section 4a).
* **A coordinate stream beside another stream**, which needs the second
  output target and which `M9X_VS_COORD_PAIR=1` allows, and a **split
  fragment program**, which the engine draws nothing from (section 4a).  A
  coordinate that is its pass's only stream, and one that shares an
  interpolated varying's stream through the declaration's selectors, are
  both on by default.
* **One basic block**, the backend's own limit.  A branch or a loop the
  bound of which is a *uniform* is not that case: the value is fixed by the
  time the draw is submitted, so the program is specialised on it and folds
  or unrolls to one block.  That is what takes glmark2's three `loop`
  benchmarks onto the engine -- `vertex-loop` and `vertex-uniform` both
  default to true there -- and `parhelia-m9-uniform-specialisation.md`
  carries it for both stages.
* Instanced draws, primitive restart, user clip planes, polygon offset,
  polygon or line stipple, wide or smooth lines, non-fill polygon modes and
  vertex-stage sampling all fall back.

