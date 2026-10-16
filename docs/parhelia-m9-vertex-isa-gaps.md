<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 vertex instruction set: what is missing, what is only missing from the
# compiler, and what the hardware does not have

`docs/parhelia-m9-native-vertex-isa.md` records the M9 (`102b:0540`) vertex
program the driver runs; `docs/parhelia-m9-native-vertex-backend.md` records
what `m9_vs_compile.c` compiles into it.  This note answers the separate
question of what the backend still **refuses**, and for each refusal says
whether an instruction exists, whether the compiler simply never learned a
lowering the fragment backend already has, or whether the hardware lacks the
operation outright.

It was opened because ioquake3's opengl2 renderer is almost entirely on the
CPU vertex path: of its fifteen vertex programs, eight were refused on `fsin`,
two on more than four vertex attributes, and five compiled natively (MEASURED
by the caller of this work).  Sections 1.5, 2 and 9 record what happened when
the offline answers were put on the engine.

Labels: **STATIC** is read from a named address of a named binary, with the
bytes; **CAPTURED** is read from a saved vendor command stream; **DERIVED** is
a join of those, with the join given; **MEASURED** is a hardware result;
**OPEN** names what is not settled and the one experiment that would settle it.

Run `python3 tools/m9-vertex-isa-gap-evidence.py` (48 checks) to re-derive every STATIC,
CAPTURED and DERIVED claim below.  It ends with negative controls that must
find a planted fault, so a pass cannot be a stale one.  The vendor package is
not in the repository, so pass `--dll` when `xddm32_*/MSerX/MxMD.dll` lives
elsewhere; without it the static half reports itself unverified rather than
quietly passing.

## Result, in one table

| refused | verdict | where it stands |
|---|---|---|
| `fsin`, `fcos` | **no instruction exists**; the vendor lowers it | implemented, **on by default**; MEASURED on the M9128, worst error 7.085e-05 (§1.5).  ioquake3's eight are no longer refused for it, and are refused for the program length instead (§1.6) |
| `flog2`, and so `fpow` | an instruction exists on the fragment unit; not seen on the vertex unit | OPEN, one hardware probe (§4) |
| `iand`, `ior`, `inot`, `b2f16` | **no integer ALU exists**; they are float lowerings | implemented and on by default, but they drew a **wrong picture** until §8; MEASURED exact since |
| `flrp` | a float lowering, and the driver's own NIR options already lower it | nothing to do (§5) |
| register-register dot | **an instruction exists and already runs**, `op2 = 0x0d` | DERIVED, not yet used by either backend (§6) |
| `fcsel_gt` | the select's own comparison class would give it | **refused today**, and `x > 0.0 ? a : b` is a common shape (§9) |

**What running it on the engine found.**  Everything above had been verified
offline only.  The sine was right the first time the card drew it; the
Boolean combinators were not, and neither were `min` and `max` of two
registers, because those stand on a subtract whose two operand fields the
vertex backend had the wrong way round (§8).  Offline evidence could not see
it: the gate's simulator held the same belief as the compiler.

Three further findings are recorded because they change what the existing
notes say: several forms the backend note calls "untested on the vertex unit"
are in fact CAPTURED there (§6); `min` against a constant, which no capture
states on the vertex unit, is MEASURED there (§6); and the offline gate used
to compile NIR that differs from what the driver hands the backend, which is
now fixed (§7).

## 1. There is no sine and no cosine (STATIC, CAPTURED, DERIVED)

### 1.1 The vendor's compiler keeps sine coefficients, not a sine opcode

`MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`, carries one
constant pool for the shared shader compiler.  Addresses below are file offsets
with the image virtual address beside them; every section of this PE sits
`0x10000` above its file offset, image base included.  **STATIC**:

| file offset | image VA | words | what they are |
|---|---|---|---|
| `0x1faed8` | `0x20aed8` | `3af71c15 3c130514 3d64b436 3e75ea9e 3f317271 3f800000` | the exp2 polynomial |
| `0x1faef8` | `0x20aef8` | `bc188576 3d553bd2 be0cd266 3e77ab67 beb1d173 3ef51608 bf389e52 3fb8aa10` | the log2 polynomial |
| `0x1faf18` | `0x20af18` | `b5c08e8a 35902424 39881741 b8ca0212 bcaaa5d9 3b882268 3effffc7 bdaa9f95` | **the sine polynomial** |
| `0x1faf38` | `0x20af38` | `00000000 3f7ffea6` | its linear term |
| `0x1faf48` | `0x20af48` | `40490fdb 40c90fdb 3e22f983 3f000000` | pi, 2pi, 1/2pi, 0.5 |

The first two rows are exactly the coefficients
`docs/parhelia-m9-native-fragment-arithmetic.md` measured on hardware for the
exp2 and log2 chains.  The sine rows begin at the byte the log2 row ends on
(**STATIC**: `0x1faef8 + 8 words = 0x1faf18`), so the sine coefficients are in
the same pool, reached the same way.  Each row is handed out by a constant-id
dispatcher at `0x180e20 .. 0x18142c`, one arm per pair, which loads the pool
address into `eax`: `0x181419` for exp2, `0x181400` for log2, `0x1813ce` for
the sine polynomial, `0x180f69` for its linear term and `0x180f8d` for the
reduction row (**STATIC**, each address is the `mov $ADDR,%eax` of that arm).
Each of the four distinctive sine coefficients occurs exactly once in the whole
image, so the pool is the only place the compiler keeps them.

A polynomial the compiler must upload is not an instruction the silicon runs.

### 1.2 The vendor's own program confirms it (CAPTURED)

`captures/m9x-vendor-glsl-programs-20260915/fs-sin` is `gl_FragColor =
sin(v1)`, a four-component sine, and its whole fragment program is **21
slots**.  Decoded, the 21 are, in order (CAPTURED, and asserted by the tool):

```text
 0  1   op2=0x10   interpolate v1 into r00 and r01
 2  3   op2=0x02   t = v * (1/2pi) + 0.5                  pairs 8 and 9
 4  5   op2=0x14   t = fract(t)                           the measured fract class
 6  7   op2=0x02   x = t * 2pi + (-pi)                    pairs 6 and 7
 8  9   op2=0x01   y = x * x
10..15  op2=0x02   three Horner steps over y              pairs 4, 5, 3, 2
16 17   op2=0x0c cls 7   S = y * t3 + k1                  pair 1
18 19   op2=0x0c cls 0   result = 2 * x * S - x           pair 0
20      op2=0x14   move the second lane to the output
```

The eleven constant pairs the case binds are the pool's own rows plus pi, 2pi
and 1/2pi, in the pool's order (CAPTURED, asserted pair by pair).  Each pair
holds **two different coefficients**; the Horner chain's constant selects all
read the pair's **second** float, and the first floats are a second, unused
half whose values are exactly `0.5 * sinc(x/2)` — the cosine partner of a
shared sine-and-cosine table.

Nothing in those 21 slots could be a sine instruction: the argument is reduced
with a `fract` against uploaded constants and the answer is a polynomial in
`x * x`.

### 1.3 The arithmetic, folded (DERIVED)

Reading the chain with the second float of each pair gives, with `y = x * x`
and `x` the reduced argument in `[-pi, pi)`:

```text
t3 = ((1.07393544e-06 * y - 9.63249622e-05) * y + 0.00415449217) * y
     - 0.0833121911
S  = y * t3 + 0.999979377            op2 = 0x0c class 7
sin(x) = 2 * x * S - x               op2 = 0x0c class 0
```

which folds to `sin(x) = x * P(y)` with

```text
P = 2.14787088e-06 y^4 - 1.92649924e-04 y^3 + 8.30898434e-03 y^2
    - 0.166624382 y + 0.999958754
```

**DERIVED, and the join is numeric**: evaluated against the C library over
200001 points of `[-8pi, 8pi]`, including the reduction, the worst absolute
error is **6.925e-05**.  A minimax fit landing that close is not a coincidence,
and the negative controls show a one per cent change in a single coefficient
takes the error to 5.2e-02 and the wrong reduction phase to 1.4e+00.

What this does **not** settle: the exact semantics of `op2 = 0x0c`.  Class 0
must compute `2ab - a + k` for the program to be a sine, and class 7 must
compute `ab + k`; both readings are forced by the requirement that the result
be `sin`, not read off a second instance, because `op2 = 0x0c` occurs **only in
fs-sin** in the whole corpus — 4 slots, 0 of them in any vertex program
(CAPTURED).  It is **OPEN** whether class 0's doubling is a property of the
class or of the opcode.  Nothing here depends on the answer: the folded
polynomial needs no such instruction.

### 1.4 What this means for the vertex unit

No captured vertex program contains a sine, and `op2 = 0x0c` appears in no
vertex program.  But none of the folded form's steps needs it: a multiply-add
against two constants, a `fract`, a multiply-add, a square and a Horner chain
are all forms the vertex backend already emits.  **So `fsin` and `fcos` are a
lowering, not an encoding to find, and the recommendation is to lower them.**

`m9_vs_compile.c` now does, behind `M9X_VS_SINCOS`, **default on**:
`emit_sincos()` builds the vendor's reduction and the folded polynomial from
`emit_mad`, `emit_unary(VS_FRACT)`, `emit_mul` and `emit_add`.  `cos(x)` is
`sin(x + pi/2)`, which the phase of the reduction carries (0.75 in place of
0.5), so it costs the same.  A scalar sine is 12 slots for both scalars of a
register.  Offline (`m9-vs-compile-test`): `sincos` (a deformVertexes-shaped
scalar sine and cosine) compiles to 65 slots and `sincos-vec4` to 84, both
verified against the NIR reference — which evaluates the true `sinf` — within
the polynomial's own 1.5e-4 bound, and `sincos-default-off` is still refused
with the reason `fsin` when the knob is unset.

**The knob is on by default since the engine ran it** (§1.5).  It still
refuses when set to 0, which is how a fixture says which path drew a picture.

### 1.5 What the engine made of it (MEASURED)

`tools/m9-mesa-glsl.c` carries three cases.  The target is cut into 40x30
cells of 8x8 pixels and each cell carries its own argument **in a vertex
attribute** -- not a uniform, which the backend would fold on the host with
the C library's own sine -- so one draw sweeps 1200 arguments and every pixel
names the one that drew it.  1152 cells step across `[-4pi, 4pi]` and the
last 48 are the reduction's edges one by one: every quarter-turn from
`-4pi` to `4pi`, each approached from either side, zero, `1e-7`, and `+-20`
and `+-50`, where the reduction's own multiply starts losing bits.

* `vssin` writes `sin` and `cos` into two channels and is checked pixel by
  pixel against the C library.
* `vssinerr` subtracts the C library's answer **in the shader**, from a second
  attribute component, and multiplies what is left by 1024, so an 8-bit
  channel resolves the error to `1/(255*1024)` = 3.8e-06.  The readback then
  states the error rather than passing a window.
* `vssinerr:cal` offsets that reference by 1e-4.  It is the control that says
  an error of that size would have been seen.

MEASURED on the M9128, 2026-09-20, one process a case, `M9X_DEBUG=1`:

```text
vssin      M9X_VS_SINCOS=1   0 of 76800 pixels differ: PASS   m9x vs: native
vssin      M9X_VS_SINCOS=0   0 of 76800 pixels differ: PASS   m9x vs: fsin
vssinerr   M9X_VS_SINCOS=1   worst sin 7.085e-05, cos 7.085e-05,
                             mean -3.4e-08 / -2.8e-07, step 3.8e-06
vssinerr   M9X_VS_SINCOS=0   worst sin 1.915e-06, cos 1.915e-06, mean 0
vssinerr:cal                 worst 1.704e-04, mean -9.99e-05 / -1.00e-04
```

Read in order: the picture is exact on both paths and the debug line says
which one drew it, so the fixture distinguishes them rather than passing
either way.  The **worst error on the engine is 7.085e-05**, and the mean is
under 3e-07, so there is no systematic bias.  The offline figure for the same
polynomial is 6.925e-05, over a denser sweep of a wider range, so the engine's
own rounding is worth a couple of parts in 1e-06 and no more -- the claim the
offline work made holds on the silicon.

The `M9X_VS_SINCOS=0` arm is the instrument's own noise floor: the draw module
computes the sine on the CPU and comes back one quantisation step from the
reference, 1.915e-06, which is as close as this fixture can see.  The engine's
7.085e-05 is thirty-seven of those steps, so it is a measurement and not the
readback's rounding.  The calibration arm moves the mean by exactly the 1e-4
planted in it, which says an error that size would have been seen.

`vssinerr`'s bound in the fixture is 3e-4 plus the planted offset; the
offline gate carries the same two shapes as `vssin-fixture` and
`vssinerr-fixture`, so the gate and the card compile the same text.

### 1.6 ioquake3: the sine is accepted, and the next wall is the program length

This work was opened for ioquake3's opengl2 renderer, so that is where it is
read.  MEASURED on the M9128, `bench` timedemo at 1280x1024, `cl_renderer
opengl2`, one X server an arm, on a freshly rebooted card:

| arm | timedemo | native | refused for the sine | refused for the length | refused for the attributes |
|---|---:|---:|---:|---:|---:|
| `M9X_VS_SINCOS=0` | 70.4 fps | 5 | **8** | 0 | 2 |
| `M9X_VS_SINCOS=1` | 70.5 fps | 5 | 0 | **8** | 2 |

**The same eight programs.**  With the sine accepted they compile, and the
backend then refuses them for being longer than `M9X_VS_MAX_SLOTS`, which is
128 by default.  A scalar sine is 12 slots and the `deformVertexes` shaders
carry several, so the sine is what pushed them over.

The frame rate is therefore **unchanged**: 70.5 against 70.4, and the
steady-state phase profile is the same to the hundredth of a millisecond --
`draw 1.67 emit 1.52 ioctl 5.23` at 5.80 ms/frame against
`draw 1.67 emit 1.52 ioctl 5.23` at 5.87.  Nothing moved onto the engine, so
nothing could move.

**Lifting the bound did not work, and §1.7 says why it could not have.**
`M9X_VS_MAX_SLOTS=4096` with the sine on lets all eight run, and the engine
refuses them:

```text
m9x: two-pass job failed (Input/output error); context lost
```

after which the game renders nothing and reports 380.5 fps of it -- the
profile for those frames is `native 0 draw module 0` with every phase at
0.00 ms, which is what an empty frame costs.  That run left **one kernel
stream timeout**.

What the eight cost is now recorded.  MEASURED 2026-09-20 on the M9128, the
same timedemo, `M9X_DEBUG=1`; the backend's debug line states the compiled
length and the constant span of every program it compiles, refused or not:

| programs | slots | constant pairs | of which uniform |
|---:|---:|---:|---:|
| 4 | 138 | 68, 69, 69, 69 | 44 |
| 1 | 146 | 70 | 45 |
| 1 | 180 | 74 | 47 |
| 1 | 180 | 75 | 47 |
| 1 | 206 | 82 | 54 |

Read against §1.7: every one of them is far inside what the engine will run
for length -- it runs a **5161-slot** program exactly -- and every one of
them is past the **63 constant pairs** the engine's span will take.  Seven of
the eight are inside the raised length bound and are refused for the span
alone; only the 206-slot one is past both.  So the 4096 run handed the engine
five spans of 68 to 82 pairs, which is the configuration §1.7 stops the card
with on purpose.

**The picture.**  MEASURED, and not from the frame rate: the demo was played
in real time on both arms and frozen with `SIGSTOP`, and the screen read
through the host aperture with the scanout descriptor
(`tools/m9-x-scanout-compare`), not through a glamor readback.  The gameplay
frame shows the level, its statues, its sky, the floor inlay and the weapon
in hand, all correct, on both arms, and the two differ only where the demo
has advanced one tick between them -- the HUD ammo counter reads 108 against
107 and two moving items have moved.  Every static surface is identical.
The sine changes no pixel here, because no program that uses it reaches the
engine.

A second route exists and was considered: Mesa's own
`nir_shader_compiler_options::lower_sincos` expands `fsin` and `fcos` into
`ffract`, `fmul`, `fsub`, `fabs` and `ffma`, all of which the backend already
accepts, so that one flag in `m9_vs_nir_options` would have needed no backend
code.  Its approximation is about sixteen times coarser (worst error near
1.1e-3 against 6.9e-5), which is why the vendor's own polynomial was
implemented instead.

This note used to say that flag "would unblock ioquake3".  It would not, and
neither does the backend's own sequence: §1.6 measures the eight programs
being refused for their length the moment the sine stops refusing them, and
Mesa's expansion is longer than the backend's twelve slots, so it would reach
that bound sooner.

### 1.7 The wall is the constant span, not the program length (MEASURED)

Two synthetic programs settle it, on the pattern `fschain:N`/`fsconst:N` used
for the raster stage.  `vschain:N` and `vsconst:N` in `tools/m9-mesa-glsl.c`
rotate a pair of values N times **in the vertex shader** and interpolate the
result; a rotation is linear, so the reference is the same per-pixel one the
fragment chain uses.  `vschain` cycles three angles and holds the span at 27
pairs while the length grows; `vsconst` gives every step its own angle, so
the span grows with the length.  Whichever fails first names the resource.
MEASURED on the M9128, one process a case, a known-good native-vertex fixture
(`vssin`) run after every case:

| case | slots | span | result |
|---|---:|---:|---|
| `vschain:64` | 361 | 27 | 0 of 76800 differ |
| `vschain:256` | 1321 | 27 | 0 of 76800 differ |
| `vschain:512` | 2601 | 27 | 0 of 76800 differ |
| `vschain:1024` | **5161** | 27 | **0 of 76800 differ** |
| `vsconst:16` | 121 | 53 | 0 of 76800 differ |
| `vsconst:20` | 141 | 61 | 0 of 76800 differ |
| `vsconst:21` | 146 | **63** | **0 of 76800 differ** |
| `vsconst:22` | 151 | **65** | **76800 of 76800 differ, kernel stream timeout** |
| `vshalf:21` | 149 | 65 | 76800 of 76800 differ, kernel stream timeout |

**There is no length ceiling within reach.**  A 5161-slot program renders
every pixel exactly.  There is a **span ceiling between 63 and 65 pairs**, and
it is sharp: 63 is exact and 65 stops the engine, reproduced on two programs
of different length, each on a freshly rebooted card.

**And it is the span the job hands the engine, not the constants the program
names.**  `M9X_VS_CONST_PAD=K` appends K unnamed pairs to the span, which
changes index `8c`'s count and index `8e`'s end address and nothing else.
MEASURED with `vschain:8` -- 81 slots naming 27 pairs, the same program in
every arm:

| pad | span | result |
|---:|---:|---|
| 0 | 27 | 0 of 76800 differ |
| 20 | 47 | 0 of 76800 differ |
| 36 | **63** | **0 of 76800 differ** |
| 38 | **65** | **76800 of 76800 differ, kernel stream timeout** |

So the resource is the span the engine loads from `8d..8e`.  Neither the
field nor the layout explains it: `8c` carries the count in eight bits, and
the span may run to pair 93 before it reaches the linked record at `0x300`.
The raster stage's own ceiling, measured earlier between 56 and 68 pairs, is
consistent with the same 64-pair file (DERIVED; the raster bracket was never
narrowed).  `M9_VS_MAX_CONST_PAIRS` is **63**, the highest measured-good
count, and `M9X_VS_MAX_CONST_PAIRS` is the knob over it.

### 1.8 What a long program costs, and where the length bound belongs

The length bound is a **cost** bound, and it now has numbers.  `vsperf:N` in
`tools/m9-mesa-glsl.c` runs the same chain over 20000 vertices a draw, sixty
draws with a `glFinish()` around them, on a two-row strip of slivers so the
raster stage has almost nothing to do.  MEASURED on the M9128, five processes
a point where a point is quoted twice:

| slots | native ms/draw | draw module ms/draw |
|---:|---:|---:|
| 111 | 0.697 | 0.862 |
| 141 | 0.789 | — |
| 161 | 0.849 | 0.930 |
| 181 | 0.910 (x5 identical) | 0.983 |
| 186 | 0.925 (x5 identical) | — |
| **191** | **1.78 - 2.06** | — |
| 196 | 1.83 - 2.10 | — |
| 201 | 1.87 | 1.297 |
| 681 | 5.466 | 2.028 |

**A step of about two, between 186 and 191 slots.**  It is repeatable to the
millisecond below the step and reproduces above it, and it is a property of
the vertex program rather than of the image it sits in: with a four-step
fragment chain in front of it in the same `c2` image the step lands between
**188 and 193** slots -- 188 costs 1.290 ms and 193 costs 2.14 - 2.38 -- so
the fragment program in front moves it by less than the instrument's own
granularity.  Below the step native beats the draw module at every length
measured; above it native loses outright (201 slots: 1.87 against 1.30 ms).

glmark2's `buffer` scene is the same statement at scene scale.  MEASURED at
1920x1200, one X server, one deployed library, `M9X_PROFILE=200`:

| arm | ms/frame | draw | emit | ioctl | vertex path |
|---|---:|---:|---:|---:|---|
| `M9X_VS_MAX_SLOTS=128` | 6.40 | 2.96 | 1.92 | 4.09 | draw module 200 |
| `M9X_VS_MAX_SLOTS=4096` | 10.03 | **0.00** | **0.00** | 4.98 | native 200 |

The host half goes to **zero** -- 4.88 ms of transform and emit a frame -- and
the frame still gets **3.6 ms slower**, so the 299-slot program costs the
engine more than the whole host transform it replaces.  That is what the
quarter of the frame rate the `buffer` rows lost was.

`M9X_VS_MAX_SLOTS` is therefore **188**, the highest length measured to run
below the step, in both instruments.  It is not a correctness bound -- §1.7
runs 5161 slots exactly -- and it is not a cost model either, because a
length is only a proxy for what a program costs; it is the measured point
where the engine stops being the cheaper place to run one.

### 1.9 What ioquake3 still needs (OPEN, and it is not the length)

With the bound at 188 and the span bound at 63, the timedemo tally reads:

```text
5 m9x vs: native
7 m9x vs: the vertex program's constant span is past M9X_VS_MAX_CONST_PAIRS
1 m9x vs: the compiled vertex program is past M9X_VS_MAX_SLOTS
2 m9x vs: more than four vertex attributes
```

MEASURED at 1280x1024, `cl_renderer opengl2`, `bench` timedemo, one X server
an arm on a freshly rebooted card: **64.3 fps in both arms**, and the
steady-state profile is `draw 1.75 emit 1.52 ioctl 5.28` at 5.99 ms/frame
against `draw 1.77 emit 1.53 ioctl 5.26` at 6.01.  Nothing moved onto the
engine, so nothing could move.  (The 70.4/70.5 fps of §1.6 was another
session: the previous library, re-run in this one, also reports 64.3, so the
six frames a second are the card's day and not the change.)

What would move them is **getting the span under 63 pairs**, and the
breakdown above says where it would have to come from: 44 to 54 of the 55 to
69 user pairs a program names are **uniforms**, not literals.  Three routes
exist and none is measured:

* the appended tail owns pairs 9..21 unconditionally, so a user program has
  9 + 42 = **51** pairs available out of 64 -- the tail's thirteen are a
  seventh of the file spent on a viewport transform;
* a pair holds two scalars and is addressed by one swizzle, `0` for a
  broadcast and `1` for the two halves.  Two broadcast constants could share
  a pair if a swizzle selects the second half alone; that encoding is **not
  captured** and guessing it is how the operand fields were got wrong before
  (§8);
* the uniforms are what they are, but ioquake3's generic opengl2 shader is
  specialised on some of them already (`M9X_VS_SPECIALISE`), and a value the
  backend can inline stops being a pair.

Until one of those lands, ioquake3's `deformVertexes` surfaces stay in the
draw module, and they stay there for the **constant span** -- not the sine,
which is accepted, and not the length, which is now inside the bound for
seven of the eight.

## 2. There is no integer ALU (STATIC, and a lowering)

Three independent statements, none of them new evidence but none of them
previously joined:

* `docs/parhelia-m9-d3d-sm23-audit.md` records that the D3D compiler's integer
  and Boolean constant files feed comparisons, address and control
  expressions, and conversions only: "There is no D3D SM3 general integer ALU
  here."
* The screen declares no integer support — `no_integers = true` in both
  `m9_vs_nir_options` and the fragment backend's options — so Mesa stores even
  an integer uniform as a float.
* The fragment backend has always built `iand`, `ior` and `inot` out of float
  arithmetic: a condition holds 1.0 or 0.0, so and is a product, or is the
  larger of the two and not is one minus.

So the vertex backend's missing `iand`/`ior`/`inot` were never an encoding
gap; they were a lowering the fragment backend had and the vertex backend did
not.  `m9_vs_compile.c` now has them, **on by default** — a product, a
maximum and a subtract, no instruction word the backend did not already emit.
Only a one-bit definition is accepted; a 32-bit bitwise operation still fails
with `integer bitwise operation`, because nothing here implements one.
`b2f16` joins `b2f32`.  Offline: `bool-ops` verifies a conjunction, a
negation and a disjunction against the NIR reference.

**That default was not earned by the offline evidence, and the card said so.**
The first time the engine drew the `vsbool` fixture -- a conjunction, a
disjunction, a negation and a nesting of all three over all eight
combinations of three conditions -- **76800 of 76800 pixels were wrong**,
while the draw module drew the same shader exactly.  The cause was not the
lowering: `or` is a maximum of two registers, a maximum of two registers is
built on a subtract, and the subtract had its two operand fields the wrong
way round (§8).  With that fixed, MEASURED on the M9128:

```text
vsbool        0 of 76800 pixels differ: PASS   m9x vs: native
vsprim:bool   0 of 76800 pixels differ: PASS   m9x vs: native
```

and the same picture from the draw module, so the default is justified
**now** and was not before.  `vsbool` reads the combinators through a select
and `vsprim:bool` reads them as values -- `float(p && q) * 0.6 + 0.2` -- which
matters, because a select only asks whether a condition is non-zero while the
lowerings need it to be exactly 1.0 or 0.0.  The model underneath is measured
too: `vsprim:cmp` puts `float(x > 0.0)`, `float(x < 0.0)`, `float(x > y)` and
`float(x >= 0.0)` in its four channels and all four are exact in all 1200
cells, so **the vertex compare does write 1.0 and 0.0**, against a constant
and against a register alike.

The general lesson is recorded in §8: a value model verified against a
simulator that shares the compiler's beliefs is not verified.

Shifts and integer compares were looked for and are not there: no captured
program of either stage contains an opcode that is not accounted for by the
float forms this note and the two ISA notes list.

## 3. `op2` census, by stage (CAPTURED)

The whole vendor corpus, 112 cases, with the shared 25-slot epilogue stripped
from every vertex program so the head's opcodes stand alone: 1248 fragment
slots and 4813 vertex slots.  The opcodes that appear in **only one** stage:

| op2 | stage | cases | what it is |
|---|---|---|---|
| `0x0c` | fragment only, 4 slots | `fs-sin` | the sine's combining step (§1.3) |
| `0x03` | fragment only, 3 slots | `fs-cross` | the cross product's step, arithmetic **OPEN** |
| `0x16` | fragment only, 1 slot | `fs-loop-break` | control flow |
| `0x1b` | fragment only, 13 slots | `fs-pow`, `fs-ternary`, `fs-discard-*` | the register/constant select |
| `0x1e` | fragment only, 7 slots | `fs-discard-*`, `ff-alpha-test` | the kill |
| `0x02` with two registers and a constant addend | fragment only, 4 slots | `fs-sin` | see §1.4 |
| `0x14` `hi=9` `W1[13:12]=2` | fragment only, 8 slots | `fs-log`, `fs-pow` | **the log2 seed** |

and the ones the vertex unit is now known to run, which §6 revisits.

## 4. `flog2` and `fpow`: OPEN, and the probe that settles them

The fragment unit's log2 is a seed instruction followed by a polynomial: the
seed splits the exponent from the mantissa, which nothing in the float-only
instruction set can do otherwise, so log2 cannot be lowered the way sine can.
`exp2`'s seed and `log2`'s seed are the **same opcode and the same `hi`
nibble** — `op2 = 0x14`, `hi = 9` — differing in `W1[13:12]`, 1 against 2.

* exp2's seed **is** CAPTURED on the vertex unit: 2 slots, in `ff-fog-exp` and
  `ff-fog-exp2`, and the backend emits it.
* log2's seed is CAPTURED **only** in fragment programs: 8 slots, `fs-log` and
  `fs-pow`, 0 vertex.

That the two seeds are neighbouring classes of one opcode makes it likely the
vertex unit has both, but likely is not evidence, and the backend note already
records that the fragment log2 chain cannot be validated offline — its
polynomial slots write the first scalar while their chained operand's swizzle
reads the second, which the simulator's model does not reproduce.  So `flog2`
and, through `exp2(y * log2(x))`, `fpow` stay **OPEN**.

**The probe.**  Extend `tools/m9-user-vertex-probe.c` with a variant that
writes the fragment note's log2 chain — `mov 0xa28ffc00`, seed `0x96852ec0`,
polynomial `0x944041e0`, three steps `0xa26008a0`, `mad 0xac4fcf00`, with the
four captured coefficient pairs — into the vertex program's head over a value
carried in an attribute, and compares the resulting geometry against a CPU
`log2f`.  One draw answers it.  Expect to have to reboot: the probe writes an
instruction class no vertex program has been seen to carry, and probing
unknown vertex encodings has locked this engine before (`0x5e`, `0x66`).  Take
both `/tmp/m9-hw.lock` and `/root/tmp/m9-hw.lock`, reload with
`KO=/root/tmp/dxtmip-m9x_drm.ko` named explicitly, and re-prove the engine
with a known-good fixture afterwards.

## 5. `flrp` needs nothing

`mix(a, b, t)` is `a + t * (b - a)`, three instructions the backend already
emits, and the fragment backend lowers it exactly so.  The vertex backend
never sees it: `m9_vs_nir_options` sets `lower_flrp32` and `lower_flrp64`, so
Mesa expands it before the backend is called.  Nothing to implement.  The
offline test compiles the driver's own options now, so it does not see
`flrp` either (§7).

## 6. What the vertex unit is now known to run

`docs/parhelia-m9-native-vertex-backend.md` section 5 lists forms "taken from
the fragment unit's measured forms ... but not captured in a vertex program",
and calls them untested on the vertex unit.  The census says four of them are
CAPTURED there after all (slot counts are vertex heads, epilogue stripped):

| form | vertex evidence |
|---|---|
| maximum, `op2 = 0x19` class 3 | 2 slots, `vs-builtin-light`, `vs-lighting` |
| negate, `op2 = 0x15` | 2 slots, `ff-fog-exp`, `ff-fog-exp2` |
| select, `op2 = 0x1a` class 4 | 6 slots, `vs-builtin-light`, `vs-lighting`, `vs-math` |
| the reciprocal-square-root chain, seed `op2 = 0x11` class 5 and step `op2 = 0x13` class 7 | 5 and 10 slots, `vs-builtin-light`, `vs-lighting`, `vs-math` |
| multiply-add over three registers, `op2 = 0x02`, `e = 0x3f` | 5 slots, the same three cases |

That closes open item 4 of the ISA note for `rsq`: the vertex unit has the
reciprocal-square-root chain, byte for byte the fragment unit's, in the
vendor's own `normalize()` programs.  **Minimum** (`op2 = 0x19` class 2) is
captured in no vertex program, but it is **MEASURED on the vertex unit**:
`vsprim:minmax` puts `min(x, 0.25)` and `max(x, 0.0)` against a constant in
two of its four channels and both were exact in all 1200 cells, while the
two-register forms in the other two channels were wrong (§8).  So class 2
runs there; what was wrong was the lowering around it, not the instruction.

### A register-register dot product exists: `op2 = 0x0d` (DERIVED)

Both backends expand a dot of two *computed* vectors into multiplies and adds,
because the captured dot forms `0x0e` and `0x10` take a constant operand per
half.  There is a third form that does not.

`op2 = 0x0d`, destination mask `0x1d` (first scalar only), with the operand
registers in `a` and `b`, `c` encoded equal to `a`, and a constant pair index
in `e` whose scalar select sits at `W1[5:4]`:

```text
dst.s0 = a.s0 * b.s0 + a.s1 * b.s1 + k
```

**DERIVED from four independent programs**, each of which needs exactly three
or four products and spends one `0x0d` plus one ordinary scalar multiply-add:

* `fs-length`, `length(v1.xyz)`: `0x0d` over one interpolated pair, then
  `op2 = 0x02` adding the square of the third component;
* `fs-normalize`, `normalize(v1.xyz)`: the same shape;
* `fs-distance`, `distance(v1, u)`: `0x0d` over one difference pair, then
  `op2 = 0x10` over the other difference pair with the first result as a
  register addend — a second, register-addend form of the same idea, whose
  single join makes it weaker evidence;
* `vs-math` and `vs-lighting`, `normalize()` in a **vertex** program:
  `0x0d` over the attribute's first register pair, then `op2 = 0x02` for the
  third component.

It is not merely available on the vertex unit, it already runs there on every
native draw: **slot 36 of the epilogue `m9_vs_emit()` copies verbatim is
`c3c70a1d eda40c01`**, an `op2 = 0x0d` with `a = r0e`, `b = r0f`, `c = r0e`
and constant pair 16 — the clip code's dot over the two compare results.  The
ISA note calls that slot "a DP3"; it is a two-component dot plus a constant,
and that note is corrected.  The census finds `0x0d` in 100 vertex head slots
across 98 cases and in 17 epilogues.

Using it would take a four-component register-register dot from seven
instructions to two.  Nothing here implements that yet, and it is not on the
path to ioquake3, so it is recorded rather than done.  What stays **OPEN** is
whether the `c` field is read at all: every captured instance has `c == a`, so
a form with `c != a` is untested and must not be emitted.

## 7. The offline gate compiled what the driver did not (FIXED)

`m9_vs_compile_test.cpp` used to build its own `nir_shader_compiler_options`
rather than use the driver's, and the two disagreed on five options that
change which opcodes reach the backend:

| option | driver | the offline test, before | effect |
|---|---|---|---|
| `lower_sincos` | unset | **set** | the test could never see `fsin` at all |
| `lower_flrp32`/`64` | set | unset | the test could see `flrp`, the driver cannot |
| `lower_fsat` | set | unset | the backend's `fsat` path is dead in the driver |
| `lower_scmp` | set | unset | different comparison shapes |
| `float_mul_add32` | unset | `has_ffma` | the test got `ffma`, the driver gets `fmul` plus `fadd` and relies on the backend's own fusion |

The first row is why a sine could pass through this gate unnoticed for as long
as it did: **the gate that is supposed to say what the backend accepts could
not, by construction, ever have presented it with a sine.**

Fixed the way the fragment side already worked: the options live beside the
backend that consumes them.  `m9_vs_nir_options` moved out of `m9_screen.c`
into `m9_vs_compile.c`, is declared in `m9_vs_compile.h`, and the gate uses
that struct.  Nothing else moved with it -- the same cases compile, verify and
are refused, with the same reasons -- so the divergence cost nothing to close.

## 8. The vertex subtract had its two operand fields swapped (MEASURED)

This is what running the offline work on the engine found, and it had been
shipping.

**The instruction.**  The register-register add, `VS_ADD_RR`
(`0xac800c00`), carries a negate at `W1[7:6]`.  The subtract form computes
**the `a` operand minus the `c` one**.  Two independent statements:

* MEASURED on the M9128 (below).
* The fragment backend, whose subtract is measured correct over the whole
  corpus, already encodes it that way: `m9_fs_compile.c` case `N_SUB` puts
  the node's *second* source in the `a` field and its first in the `c` field
  to compute `b - a`.

The vertex backend swapped the two fields, so **every** use of the form
computed `b - a` where `a - b` was meant.  Three things used it:

| use | what it became |
|---|---|
| a comparison against a register (`flt`/`fge`/`feq`/`fneu` with a non-constant operand) | the comparison inverted |
| `inot`, the complement of a condition | `a - 1`, so -1 for true; still non-zero, so a select still worked, but the value is not a condition any more |
| `min` and `max` of two registers, built as `a + minmax(b - a, 0)` | `a + minmax(a - b, 0)`, which is neither |

and `ior` is a maximum, so the Boolean combinators were wrong through the
third row rather than any fault of their own.

**The measurement.**  `tools/m9-mesa-glsl.c` `vsprim:minmax` sweeps a pair of
attribute values over the 40x30 cell grid and puts four results in the four
channels: `max(x, y)`, `min(x, y)`, `max(x, 0.0)` and `min(x, 0.25)`.
MEASURED on the M9128 before the fix:

```text
cells wrong, per channel:  max(x,y) 1172   min(x,y) 1173
                           max(x,0) 0      min(x,0.25) 0    of 1200
```

The two constant-operand channels are exact, so the `MINMAX` instruction is
right; the two register-register channels are wrong, so the construction
around it is.  Cell by cell the readback is `a + minmax(a - b, 0)` to within
the channel's own 8-bit step: x = +0.026, y = +0.172 came back 0.025 where
`max` is 0.172 and `a + max(a - b, 0)` is 0.026; x = -0.026, y = -0.310 came
back 0.250 where `max` is -0.026 and the swapped expression is 0.259.  No
other reading of the three instructions produces either number.

The same model accounts for the `vsbool` picture exactly: all eight
combinations of three conditions, four expressions each, thirty-two readings,
every one of them what the swapped subtract predicts and none what the
shader says.

**Why offline evidence could not see it.**  `m9_vs_compile_test.cpp`
simulates each emitted instruction, and its `VS_ADD_RR` held the same belief
the compiler did, so compiler and simulator agreed with each other and both
disagreed with the silicon.  The simulator now carries the measured order,
and with the old compiler restored the gate fails `bool-ops`, `prim-cmp`,
`prim-minmax` and `prim-bool` -- the negative control that says the gate can
now see this class of fault.

**Why nothing else had shown it.**  `docs/parhelia-m9-native-vertex-backend.md`
section 5 argued that add and subtract of two registers, though captured in no
vertex program, "would have shown as a wrong scene rather than silently"
because the backend had emitted them on the default path for days.  That
argument does not hold, for two reasons, and the sentence is corrected in
place there.

* The *add* had run; the *subtract* reaches the default path only through a
  comparison against a register, the complement of a condition, or a minimum
  or maximum of two computed values, and none of those is common in a vertex
  program short enough to compile here.
* Nothing that ran **checks pixels**.  glmark2 reports a frame rate and no
  picture, and the `m9-mesa-glsl` corpus, which does check every pixel, had
  no vertex-stage minimum, maximum or register comparison in it until the
  `vsprim:*` cases were added -- which is why it passed 280 of 280 with this
  defect in the library.

The four `vsprim:*` probes exist so that the next such question is one run
rather than an inference: each channel is one primitive, so a failure names
the primitive instead of the shader.

## 9. `fcsel_gt` is refused, and the shape is everywhere

Not fixed here, recorded because it is the next thing in the way.

With `has_fused_comp_and_csel` set, Mesa folds `x > 0.0 ? a : b` into
`nir_op_fcsel_gt`, and the vertex backend refuses it: `prim-cmp` and
`prim-bool` in their original select form were both refused for `fcsel_gt`,
and the driver said the same on the card (`m9x vs: fcsel_gt`).  The probes
had to be reshaped to `float(cmp) * k + c` to reach the backend at all --
which is the stricter test anyway, because a select hides a condition holding
anything other than 1.0 or 0.0.

The instruction is there.  `emit_sel` already emits `op2 = 0x1a` with a
comparison class, CAPTURED on the vertex unit at class 4, and the backend
uses class 4 (condition equals zero) for `bcsel`.  Class 3 is "greater than
zero" in the same table, which is `fcsel_gt` exactly, and class 1 is
"greater or equal", which is `fcsel_ge`.  What is missing is two cases in
`build_alu`, and a hardware run to confirm that classes 1 and 3 mean on the
vertex unit what they mean on the fragment unit.  **OPEN.**

## 10. Reproduction

```sh
# every STATIC, CAPTURED and DERIVED claim above, with its negative controls
python3 tools/m9-vertex-isa-gap-evidence.py \
    --dll xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll

# the backend, offline
/home/ai/tmp/m9x-ninja-llvm.sh src/gallium/drivers/m9x/m9-vs-compile-test
tools/m9-offline-gates.sh objs-m9x-llvm        # read the exit code, not the last line

# one case's compiled words, which is how section 8 was read
M9_VS_SLOTS=prim-minmax objs-m9x-llvm/src/gallium/drivers/m9x/m9-vs-compile-test
```

On the card, one process a case, `M9X_DEBUG=1` so the `m9x vs:` line says
which path drew the picture:

```sh
for c in vssin vssinerr vssinerr:cal vsbool \
         vsprim:sub vsprim:cmp vsprim:minmax vsprim:bool; do
        M9X_DEBUG=1 m9-mesa-glsl "$c"                  # the native path
        M9X_DEBUG=1 M9X_NATIVE_VS=0 m9-mesa-glsl "$c"  # the draw module's own
done
M9X_VS_SINCOS=0 m9-mesa-glsl vssin    # refused for fsin, same picture
```
