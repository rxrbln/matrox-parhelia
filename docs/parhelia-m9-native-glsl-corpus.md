<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9148 native GLSL programs from the vendor ICD

This note is the vendor-side ground truth for a NIR-to-native backend in the
clean-room m9x Mesa driver.  The captures cover 112 GLSL 1.10 and
fixed-function cases in `captures/m9x-vendor-glsl-programs-20260915`.  For each
case they give the complete bound `c2` program image, the draw passes with
their `c3` entries, the constant spans with values joined to uniforms and
literals, and a full readback checked against a reference evaluation.

Labels:
* **CAPTURED**: read from those vendor captures.
* **MEASURED**: vendor readback.
* **STATIC**: from earlier binary notes.
* **DERIVED**: a conclusion that follows from several captured facts; the
  reasoning is given.

Field names follow `tools/m9-native-disasm.py`.  Each slot is `W0 W1`:

```text
W0: a[2:0]=31:29  b=28:22  c=21:15  d=14:8  cls=7:5  op0=4:0
W1: hi=31:28  op2=25:21  e=20:14  a[6:3]=3:0  lo=7:4
```

`--` in a listing means the field value is 0x7f (unused operand).
`tools/m9-glsl-program-extract.py CASE` prints any case listed below.

## 1. Image layout, entries and passes

* **Image layout (CAPTURED).** One `c2` image holds the fragment program(s)
  first, then the vertex program(s).  Raster draws bind `c3` = fragment
  entry; preprocessing draws bind `c3` = vertex entry.  A plain GLSL pair
  uses three preprocessing draws: the application vertex program, then two
  fixed stages of 7 and 25 slots.  With a 4-slot fragment program the
  entries are `4, 0x13, 0x1a` (`fs-mul`).  Fixed-function drawing uses one
  to three preprocessing entries (`cases.md`).
* **Multiple fragment programs (CAPTURED).** Several fragment programs can
  sit in one image, one per raster draw:
  * texture sampling with a computed coordinate uses two raster draws
    (section 6), with entries `0` and `4`/`7`;
  * 14 varyings split the fragment work into three raster draws, with entries
    `0, 9, 0x22` (`vs-varyings-max`).  No pass interpolates more than 6
    varying slots.
* **Stop slot (CAPTURED).** The only stop word present is
  `ffff801c a01fc00f`.  The compiler endpoint `fffc7e1d a2803f0f` occurs 0
  times in 112 images.
  * Vertex programs end with the stop slot.
  * Fragment programs shorter than 4 slots are padded with it
    (`fs-color`: 2 slots + 2 stops).
  * 53 of 83 `fs-*` fragment programs have no stop slot before the next
    entry (`fs-mul`: 4 operation slots, then the vertex program at slot 4).
    The captures do not show what bounds such a program in the raster pass
    (section 13).
* **c2 addresses (CAPTURED).** Every case bound one `c2` address for both
  frames.  The "two alternating C2 allocations" seen with glxgears did not
  occur for these single-program clients.

## 2. Selector classes

### Fragment stage (CAPTURED census of 121 fragment programs)

| selector | role | evidence |
|---|---|---|
| `op2=0x10`, `a=0x40+8k` / `0x42+8k`, `c=0x44+8k` / `0x45+8k`, `b=0x79` | varying slot `k` (0..5), xy / zw; `W1[3:0] = 8+k` | `pr-two-texcoords` k=0,1; `vs-varyings-4` k=0..3; `vs-varyings-max` k=0..5 |
| `0x00/0x02` (first texture), `0x04/0x06` (second) as sources | sampled texel xy / zw | `fs-tex2d`, `fs-tex2d-two` |
| `0x7a` | `gl_FragCoord` source | `fs-fragcoord`, `pr-fragcoord-300` |
| `0x7b` (field `b`) | facing source, used with a constant (-0.5, 0.5) | `fs-frontfacing` |
| `d=0x2d` | colour output: `op0=1d` writes R,G; `op0=1e` writes B,A | all cases |
| `d=0x21` | `gl_FragDepth` output | `fs-fragdepth`, `fs-fragdepth-const` |
| `d=0x40` with `op2=0x1e` | KIL (discard) | discard and alpha-test cases |
| `d=0x00..0x0f, 0x11, 0x13..0x1f` | temporaries (destinations seen: 0x00–0x0f, 0x11, 0x13–0x1f) | census |
| `e` | constant entry index in constant-reading forms | section 3 |

Varying slots are assigned in the order the fragment program uses them,
whatever the varying is.  `fs-color` (`gl_Color`), `fs-varying` (custom) and
`fs-texcoord` (`gl_TexCoord[0]`) have byte-identical `c2` images, fragment
and vertex programs included.  Their command streams differ only in
`ab/ac` and the `1b` fence word (CAPTURED).  How a varying is routed to a
slot is therefore not in `c2` (section 13).

**Registers (CAPTURED example, DERIVED model).**

`fs-const` builds `vec4(0.375, 0.625, 0.875, 0.9375)`:

```text
0 ffff801e a280330f  d=00 op0=1e <- e0 (0.625)
1 ffff931d a280f30f  d=13 op0=1d <- e3 (0.875)
2 ffff911d a280b30f  d=11 op0=1d <- e2 (0.375)
3 ffc9811d a28fff0f  d=01 op0=1d <- 13
4 ffff811e e280730f  d=01 op0=1e <- e1 (0.9375)
5 ffc8801d a28fff0f  d=00 op0=1d <- 11
6 ffc0ad1e a29fff1f  B,A <- 01
7 ffc02d1d a29fff1f  R,G <- 00
```

It reads back `96,159,223,239` (MEASURED).  Temporary 00 must therefore hold
(0.375, 0.625) and 01 hold (0.875, 0.9375).

DERIVED from that: each register index holds two scalars.  For temporaries,
`op0=1d` writes the first scalar, `1e` the second, and `1c` both (`fs-mad`:
`5e62811c` interpolates z,w into 01).  A vec4 occupies an xy register and a
zw register.  `W1[4]` (`lo` bit 0) is 1 in every two-scalar write seen and 0
in single-scalar constant MOVs.

The vendor writes temporaries 0x00 and 0x01 with these forms.  The lockup
recorded in `parhelia-m9-native-fragment-program-re.md` used
`ffff801c a280331f` / `ffff811c a280731f`: `op0=1c` with `W1[4]=1` on a
constant source, a combination absent from the corpus.  The program also
sat at slot 40 after the vertex program.

### Vertex stage (CAPTURED)

| selector | role | evidence |
|---|---|---|
| `0x4e/0x4f` | `gl_Vertex` xy / zw | `vs-vertex-direct`, `vs-uniform-mat` (matrix rows applied to b=4e/4f) |
| `0x46/0x47` | `gl_Color` or a linked user attribute | `vs-attrib` (`a` bound to location 6) has the same `c2` as `vs-vertex-direct` |
| `d=0x11/0x13` | varying slot 0 output xy / zw | `vs-uniform-color`: `ffff919c a280331f` (11 <- e0 = u.xy), `ffff939c a280731f` (13 <- e1 = u.zw) |
| `d=0x15/0x17`, `0x19/0x1b` | further output pairs | `vs-varyings-2`, `vs-varyings-max`; the slot for each pair is not proven |
| `d=0x21` | position result of the fixed tail | every vertex program |
| `0x56/0x57` | read only in `vs-lighting`, whose remaining inputs are `gl_Normal` and `L`; `gl_Normal` is not proven | `vs-lighting` |

## 3. Constant layout (CAPTURED)

* **Entries.** Entries are two float32s (8 bytes) in `[8d & ~7, 8e & ~7)`.
  Their count is `8c[14:8]`.  The low bit of `8d/8e` is a tag.
* **Placement.** The fragment span starts at the arena base (`40c3a000`) and
  the preprocessing span follows directly.
* **Literals.** A literal scalar takes one entry `(v, v)`, e.g. `fs-floor`
  (0.25, 0.25), (4, 4).
* **Uniform vectors.** A vec4 takes two entries `(x, y)`, `(z, w)`; a float
  uniform takes `(f, 0)` (`fs-loop-break`).
* **Matrices.** A matrix column takes the same xy/zw pair form.  `fs-mat4`
  stores column 3 first (`e0 = m[12], m[13]`, `e1 = m[14], m[15]`), then
  columns 2, 1, 0.
* **Entry order** follows first use in the compiled code, not declaration
  order:
  * `fs-swizzle` (`u.wzyx`) stores `(u.z, u.w)` before `(u.x, u.y)`;
  * `fs-vec-mat4` interleaves columns.
* **Fixed fragment tail.** Every fragment span ends with `(0, 1), (1, 0.5)`.
  The compare forms of if/else, discard and alpha test add `(2.00000024, 1)`
  (`fs-ternary` and `fs-step` do not); loops add counter literals such as
  `(3, 3)`, `(1, 1)`, `(0, 0)` and set `8c[24]` (`01500a00`).
* **Preprocessing span.** The application vertex constants come first, then
  13 fixed entries for the 128x128 viewport:

```text
(64,-64) (0.5,1) (64,64) (0.5,128) (0,2) (-16352,16351) (0,0) (1,0.5)
(inf,-inf) (1,238.984375) (1,-238.984375) (127,128) (1.25,1.5)
```

  64 is half the viewport size.  The role of the other fixed entries is not
  decoded.  A backend can copy them per viewport as a template, but must
  check them against a second window size first (section 13).

DERIVED: the entry order is the compiler's choice.  A backend may use any
order that its `e` operands match; the vendor order is not a hardware
requirement.

## 4. Arithmetic templates (CAPTURED, readback MEASURED)

All sequences are the complete fragment program of the named case.  The
varying setup is always the two interpolation slots
`5e62801c ee0ffc18` (zw -> 00) and `1e621a1c ae0ffc18` (xy -> 1a), with other
destinations as listed.

```text
fs-uniform   ffffad1e e280731f  B,A <- e1         (u.zw)
             ffffad1d a280331f  R,G <- e0         (u.xy)
fs-mul       e03fad1e a020414f  op2=01 B,A <- 00 * e1
             e6bfad1d a020014f  op2=01 R,G <- 1a * e0
fs-add       1fc0ad1e bc8fc110  op2=04 B,A <- 00 + e1(c=01)
             5fc02d1d bc8fc113  op2=04 R,G <- 1a + e0(c=00)
fs-sub       ffc02d1e a0804ddf  op2=04 B,A <- e1 - 00   (u - v1)
             ffcd2d1d a0800ddf  op2=04 R,G <- e0 - 1a
fs-min       e0002d5e a3204d5f  op2=19 cls=2 B,A <- min(00, e1)
             e68d2d5d a3200d5f  op2=19 cls=2 R,G <- min(1a, e0)
fs-max       e0002d7e / e68d2d7d  same W1, cls=3
fs-neg       ffc0931c a2afff1f  op2=15 13 <- -01
             ffc0111c e2afff1f  op2=15 11 <- -00
             then ADD 7fc0821c bc8fc112 / 3fc02d1d bc8fc112, output ffc12d1e a29fff1f
fs-swizzle   ffffad1e e280732f  B,A <- e1 (u.w,u.z stored as (z,w), swap W1[5])
             ffffad1d a280332f  R,G <- e0
fs-step      e000ad3e 9001414f  op2=00 cls=1 hi=9  B,A <- step(e1, 00)
             e6802d3d 9001414f  R,G <- step(e0, 1a)
```

`fs-mad` (`v1 * u + w`) and `pr-temp-chain` (`t = c*2; t = t - 0.5`) are 7
slots: two interpolations, two MUL into temps `13/11`, two ADD, and output
`ffc12d1e a29fff1f`.

Longer built-ins are exact, verified sequences in the fixture:

| case | slots |
|---|---:|
| `fs-dot3` | 8 |
| `fs-dot4` | 9 |
| `fs-normalize` | 18 |
| `fs-length` | 18 |
| `fs-distance` | 19 |
| `fs-cross` | 13 |
| `fs-div` | 25 |
| `fs-floor` | 9 |
| `fs-ceil` | 9 |
| `fs-fract` | 7 |
| `fs-mod` | 30 |
| `fs-sign` | 11 |
| `fs-abs` | 7 |
| `fs-clamp` | 7 |
| `fs-mix` | 7 |
| `fs-exp` | 32 |
| `fs-log` | 33 |
| `fs-pow` | 64 |
| `fs-sqrt` | 35 |
| `fs-rsq` | 32 |
| `fs-sin` | 21 |
| `fs-smoothstep` | 41 |
| `fs-mat2` | 7 |
| `fs-mat3` | 11 |
| `fs-mat4` | 11 |
| `fs-vec-mat4` | 13 |

**op2 values (CAPTURED usage, not global mnemonics).**

| op2 | cls | role |
|---|---|---|
| 0x01 | | multiply |
| 0x04 | | add, subtract with the negate bit |
| 0x14 | | move |
| 0x15 | | negated move |
| 0x10 | | interpolation, and a DP-style form in `fs-dot3`/`fs-vec-mat4` |
| 0x19 | 2 / 3 | min / max |
| 0x00 | 1 | step |
| 0x00 | 5 | compare for if / discard |
| 0x00 | 3 | compare for the ternary |
| 0x1b | | select |
| 0x1e | | KIL |
| 0x02, 0x0d, 0x0e, 0x11, 0x13, 0x1a | | seen in dot, normalize, division, matrix and derivative sequences; semantics not isolated |

## 5. Outputs

* **Colour (CAPTURED).** Colour is written to `d=0x2d` in both halves.  A
  program either computes into temporaries and ends with
  `ffc{src}2d1d a29fff1f` (R,G) / `ffc{src}ad1e a29fff1f` (B,A), or writes the
  last operation straight into 0x2d (`fs-mul`, `fs-add`).
* **Depth (CAPTURED, MEASURED).** `gl_FragDepth` is written to `d=0x21`.
  `fs-fragdepth-const` writes it with
  `a54aa11d a3204f52` (op2=19) from temp 15 = e0 (0.25).  The depth
  readback is 0.25 at all sample points.  `fs-fragdepth`
  (`gl_FragDepth = gl_TexCoord[0].x`) reads back exactly `(x + 0.5) / 128`.
* **Selector census (CAPTURED).** No program writes 0x58 or 0x78.

## 6. Texture sampling (CAPTURED)

* **Direct sampling.** There is no texture-sample instruction in `c2`.  With a
  varying coordinate, the sampled texel is already present when the fragment
  program starts: in 0x00/0x02 for the first bound texture and 0x04/0x06 for
  the second.

  ```text
  fs-tex2d        ffc02d1d e29fff1f  R,G <- 00
                  ffc12d1e a29fff1f  B,A <- 02
  pr-tex-mul-color  interp 5e62841c ee003c18 (zw->04), 1e621a1c ae003c18 (xy->1a)
                  413fad1e ac200140  B,A <- 02 * 04
                  06bfad1d ac200140  R,G <- 00 * 1a
  fs-tex2d-two    c0bfad1e ec200140  B,A <- 06 * 02
                  803fad1d ac200140  R,G <- 04 * 00
  ```

* **Texture unit.** The unit is not encoded in the program.  `fs-tex2d` (unit
  0, 4x4) and `fs-tex2d-unit1` (unit 1, 2x2) have identical `c2`.  Their
  streams differ only in the bound-texture packet `78/79/7a`
  (`79=c8010004/7a=00010004` vs `c8010002/00010002`), apart from `ab/ac` and
  the fence.  `fs-tex2d-two` adds a second `78/79/7a` packet and changes `88`
  (`0010ffe3` -> `0030ff8f`) and `22` (`2303` -> `2403`).
* **Sampler word.** `7c` is `02409200` for 2-D and cube sampling and
  `12409200` for the 3-D and rectangle cases.  Plain draws bind `7c=00000200`.
* **Computed coordinates.** A computed coordinate (`texture2DProj` with vec3,
  bias, cube, 3-D, rectangle, `shadow2D`, swizzled coordinate) makes the
  vendor emit two raster draws in one submission.
  * Draw 1 (`c3=0`, `7c=00000200`, `88=0110fffc`) runs a program that writes
    the coordinate into 0x11/0x13 and never writes 0x2d:

    ```text
    fs-tex2d-bias  1e62111c ee0ffc18  11 <- interp xy
                   ffc8931c a28fff1f  13 <- 11
                   ffff911e a280330f  11.second <- e0 (bias 1.0)
                   ffff801c a01fc00f
    ```

  * Draw 2 (`c3=4` or `7`, `7c=02409200`) runs the plain texture program
    (`R,G <- 00`, `B,A <- 02`).
  * **Draw 1 renders into an intermediate surface (CAPTURED, 2026-09-17).**
    It binds `b0` to the transient arena rather than to the colour target
    (`b0=10ec1000 b1=ee190004`), which is why it writes `0x11`/`0x13` and not
    `0x2d`: those are the output register pairs of bound target 0.  Draw 2
    binds that surface back through the descriptor-only route `74`/`78`/`79`
    and its unit samples at the record.  Registers do not survive the draw
    boundary.  The full contract, including the producer and consumer
    descriptors and route words of all seven cases, is in
    `parhelia-m9-fragment-pass-splitting.md`.
  * `fs-texcube` computes `(1, t*2-1, s*2-1)` in its 7-slot first program.
  * `fs-shadow2d`'s second program writes both output halves from 00 with
    `ffc02d1e e6803b2f` / `ffc02d1d a29fff3f`.  Its W1 differs from the plain
    texture program (`e29fff1f` / `a29fff1f`); the field is not decoded.
* **Readback (MEASURED).** Readbacks match the reference for 2-D, unit 1,
  proj3/proj4, bias, modulate, two samplers, swizzle, cube, 3-D, rectangle
  and shadow.

## 7. Discard, compare, select, loops (CAPTURED)

```text
fs-discard-all      ...  bfffc0bc afdfc302  KIL (cls=5 op2=1e d=40) on temp 15
fs-discard-cond     15 slots: compare 3 slots (ffc182bc 9000d03f, e541131d 9001400f,
                    7fc111bc 8800cc12), select e4c091bc 5360000f,
                    ADD 3fc8801e ec8fcc32, KIL 1fffc0bc afdfc300, outputs
pr-discard-color    14 slots, same structure on gl_Color.r (e041137d 9001400f cls=3)
ff-alpha-test       e07f851d e04043cf, ffc2851d a29fcccf, output, KIL e17fc05c a3c0000f
                    constants (128,128) (0.5,255) (2.00000024,1)
fs-if-uniform       16 slots: compare, then conditional MOVs with W1 hi=4
                    (ffff811c 4280731f, ffff801c 4280331f) and outputs
fs-ternary          compare e041117d 900140cf (cls=3), select
                    e44002bc a3604d1f / e44d2dbe a3600d1f (op2=1b) into 02 and 0x2d
fs-loop-fixed/-uniform  44 slots with the same layout for a literal and a uniform bound:
                    control words 01000000 01501000, ffffffff 03900800,
                    fffffffe 00403e00, 00000005 05cf8000, ..., ffffffee 04c00085,
                    three zero slots, then outputs and four stop slots
fs-loop-break       50 slots, adds 3ffffff8 00400800 and e1bf861f a2d1305f
```

* **Loop form.** Loops are not unrolled.  They contain control slots whose
  `W0` is not the operand form (e.g. `ffffffff`, `00000005`).  These carry
  counters and relative targets.  The static control-flow note
  (`parhelia-m9-native-control-flow-re.md`) has BREAK/ELSE templates of the
  same family.
* **Conditional marks.** `W1` hi values 4 and 5 appear only in conditional
  programs (census over all 112 images):
  * hi=4 in `fs-if-uniform`, `fs-if-varying`, `vs-if` and `vs-loop`;
  * hi=5 in `fs-discard-*`, `pr-discard-color`, `fs-loop-*` and `vs-loop`.

  Plain programs have exactly one slot with `W1` bit 30 set: 112 of 121
  fragment and 308 of 310 vertex programs.  The discard, if and loop programs
  mark 2–4 slots.  The meaning of both fields is not decoded.

## 8. Fragment built-ins and derivatives (CAPTURED)

```text
fs-fragcoord   fffd001c a28fccdf  00 <- 7a (gl_FragCoord)
               fec2931d b041015f  op2=02 with b=7b
fs-frontfacing fec2111d f041003f  op2=02 11 <- 7b, e4 (-0.5, 0.5)
               e44181bc b360811f, e440adbe b360011f  select u : w
fs-dfdx        1e62001d ae0ffc08, ffff9100 a00fc00f x2, 1fff821d 8c020c00 (DDX), ...
fs-dfdy        ..., 1fff831d 8c024f30 (DDY: W1 bit 14 set)
```

The DDX/DDY result slots have the family of the STATIC lowerer templates
(`dfff801c 8c020d10` / `8c024d10`), with W1 bit 14 as the axis bit.

## 9. Vertex stage (CAPTURED)

* **Fixed tail.** Application vertex programs end with a fixed tail:
  viewport, guard band and position into 0x21.  `vs-vertex-direct` slots
  22..31 are `ffffa11c a281f30f` ... `bfffa11c ac81c1c3`, and the same words
  close `vs-attrib`, `vs-uniform-color` and `vs-uniform-mat`, only
  re-indexed to their constant entries.
* **Split passes.** Many varyings split the vertex work into several
  preprocessing draws: 4 for `vs-varyings-4`, 9 for `vs-varyings-max`.
* **Varying limits.** 14 vec4 varyings link (`GL_MAX_VARYING_FLOATS = 60`).
* **Vertex texture fetch.** `vs-texlod` uses a separate preprocessing draw
  (`c3=8`, `7c=00009200`).  Its readback is black (MEASURED, same as the
  2026-09-14 special capture).

## 10. Fixed-function join (CAPTURED)

* **cls bit.** Fixed-function programs use the GLSL slot forms with `W0` bit 7
  set (cls 4), e.g. `ff-color` `5e62ad9e ee003c18 / 1e622d9d ae003c18`.  The
  GLSL `fs-color` has `5e62ad1e / 1e622d1d`.  The readbacks are identical.
* **Texture environments.** `ff-tex-modulate` equals `pr-tex-mul-color` with
  cls 4; `ff-tex-replace` is the MOV from 00/02.  `ff-tex-decal` (8 slots),
  `ff-tex-blend` (8, env colour in e0/e1) and `ff-tex-add` (6) are exact
  sequences in the fixture.
* **Fog.** Fog interpolates varying slot 3
  (`5e6e8c9c ae1ffc1b`, a=0x5a c=0x5d).  Linear, EXP and EXP2 fog produce
  the same 7-slot fragment program, raster `88=0910df10` and 13 raster
  constants.  The fog mode differs only in the preprocessing program and its
  `88` (`0700fff8` linear, `0700ffe0` EXP/EXP2).
* **Alpha test.** Alpha test appends the compare and KIL sequence of section 7.

## 11. Measured anomalies (MEASURED)

| case | reference | readback |
|---|---|---|
| `fs-dfdy` `vec4(0.25, dFdy(t)*64, 0.5, 1)` | G = 128 | G = 0 |
| `fs-fwidth` | 128, 128 | 128, 127 (matches) |
| `fs-fragcoord` B = `gl_FragCoord.z` (z=0 geometry, depth test on) | 128 | 0 |
| `vs-texlod` vertex `texture2DLod` | texel colours | 0,0,0,255 |

A negative `dFdy` clamped to 0 is consistent with the `fs-fwidth` match, but
the sign is not measured.  In `fs-fragcoord` the depth buffer holds 0.5 at
every sample point, the correct window z, so only the `gl_FragCoord.z` value
seen by the program reads 0.

## 12. Rules for a NIR-to-native backend

These rules are supported directly by the captures; the derived rules are
marked.

1. **Image and entries.** Put the fragment program(s) first and the vertex
   program(s) after, and bind raster and preprocessing `c3` to their entries.
   Emit the stop `ffff801c a01fc00f`; the clean-room probe measured that it
   ends a fragment program.  Never emit `fffc7e1d a2803f0f`.
2. **Varyings.** Interpolate each varying into temporaries with the
   `op2=0x10` template (a = 0x40+8k / 0x42+8k, b = 0x79, c = 0x44+8k /
   0x45+8k, `W1[3:0] = 8+k`).  Assign slots in fragment use order.  Keep six
   or fewer slots per raster pass; the vendor splits beyond that.
3. **Colour outputs.** Write colour to 0x2d with `op0=1d` (R,G) and `op0=1e`
   (B,A), covering both halves.
4. **Temporaries.** Use the vendor MOV forms: `W1[4]=0` single-scalar
   constant MOVs, `op0=1c`/`W1[4]=1` two-scalar copies.  Do not emit the
   lockup combination from section 2.
5. **Constants.** Build the fragment span as 8-byte entries.  Uniform vectors
   and matrix columns are xy/zw pairs, literal scalars `(v, v)`.  Append
   `(0, 1), (1, 0.5)`, and `(2.00000024, 1)` when compares are present.  Set
   `8c[14:8]` to the entry count and `8e - 8d = 8 * count`.
6. **Textures.** Bind them through the texture packets (`78/79/7a`, `7c`)
   and read texels from 0x00/0x02 (0x04/0x06).  A computed coordinate needs
   a preceding raster draw whose program writes 0x11/0x13 (DERIVED from the
   seven two-pass cases).
7. **Other forms.** Use the section 7 templates for discard, compare/select
   and loops, and the section 4 templates for arithmetic.  Compare every
   generated program against the fixture with
   `tools/m9-glsl-program-extract.py`.

## 13. Open items and exact follow-up captures

1. **Program bound without a stop slot.** Replay the vendor `fs-mul` image on
   the clean-room driver, first unchanged, then with a stop slot at 4 and the
   vertex program moved to 5.  Compare the readback and engine status.
2. **Varying routing.** Find the linker's VS-output-to-slot mapping (`6c`
   record, `23` declarations).  Capture a two-varying case with
   `v0 = u, v1 = w` written in the VS, and an FS writing `v0` to R,G and `v1`
   to B,A.  Run it once as declared and once with the varying declaration
   order swapped.  Dump the `6c` record at 0x200 bytes and the transient
   pre-pass output views (`m9-vendor-vram-dump.py --exec`) during the hold.
3. **cls 0 vs 4.** Draw `ff-color` and `fs-color` with an out-of-range colour
   (vertex colour from a uniform of 2.0 and -1.0) and compare readbacks.
4. **Signs and depth.** For the `dFdy` sign, capture `vec4(-dFdy(t)*64)`.
   For `gl_FragCoord.z`, capture geometry at z=0.5 with `glDepthRange(0.25,
   0.75)`.
5. **Fixed preprocessing entries.** Repeat `fs-color` at 64x64 and 256x256 to
   separate the viewport-dependent entries from constants.
6. **Operation semantics.** The operand roles of op2 0x02/0x0d/0x0e/0x11/
   0x13/0x1a are fixed only through the listed sequences.  Joining them to
   the lowerer corpus (`parhelia-m9-native-lower-runtime.md`) needs
   one-operation GLSL cases with a swizzle per operand.

## Reproduction

```sh
sh tools/m9x-glsl-program-corpus.sh OUT [CASE...]      # vendor host via ssh
python3 tools/m9-glsl-program-extract.py --summary \
    captures/m9x-vendor-glsl-programs-20260915
python3 tools/m9-glsl-program-extract.py \
    captures/m9x-vendor-glsl-programs-20260915/fs-const
```
