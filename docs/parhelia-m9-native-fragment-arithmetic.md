<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 native fragment arithmetic: rounding, compare, select and the
# special-function chains

This note records the arithmetic half of the M9 fragment instruction set, as
established on September 17, 2026 while bringing the clean-room `m9x` Gallium
fragment backend up to glmark2.  It continues
`parhelia-m9-native-fragment-program-re.md`, which covers where the program
lives and how a slot is laid out, and it is the reference the backend's
`m9_fs_compile.c` implements.

Labels: **MEASURED** is an M9 (`102b:0540`) result read back from the
framebuffer and compared against a CPU reference for every one of 76800
pixels, through the `m9-mesa-glsl` fixtures; **CAPTURED** is read from the
vendor programs in `m9_fs_corpus.h`, generated from
`captures/m9x-vendor-glsl-programs-20260915`; **DERIVED** is inferred from
those and not separately confirmed; **UNDECODED** is named where the words
are known but their meaning is not.

## Result

Every arithmetic form below is now compiled by the backend and measured
against the C library on hardware.  Two facts are worth stating on their own
because they are reusable and were each the cause of a silent wrong result:

1. **Every operand slot has its own two-bit scalar select in W1** — a at
   bits 9:8, b at 7:6, c at 5:4 (CAPTURED, from the bases of `L_SEL`,
   `L_MUL_RR`, `L_MIN_RC` and the rounding forms).  A captured base word
   already carries identity (`1`) in exactly the slots that instruction uses,
   so a base must never be OR'd with a select the base itself sets.  The
   multiply-add encoded none of its selects; an operand that asked for the
   second scalar silently read the first.
2. **`op0` in W0 is the destination scalar mask** — `0x1c` writes both
   scalars, `0x1d` the first, `0x1e` the second (CAPTURED, and relied on by
   every chain below).  Together with (1) this is what lets one instruction
   read one scalar of one register and write one scalar of another.

## 1. The rounding classes (MEASURED)

Three one-slot forms, all the move family with W1[13:12] cleared, source in
W0's c field, the operand select at bit 4:

| form  | W1 base      | evidence |
|-------|--------------|----------|
| fract | `0xa2800f00` | fs-fract |
| floor | `0xa2800cc0` | fs-floor, fs-fragcoord, fs-mod, ff-alpha-test |
| ceil  | `0xa2800c00` | fs-ceil  |

`fs-floor` and `fs-ceil` are byte-identical programs apart from this one
word, which is what pins the pair; `fs-fract` differs from the plain move in
W1[13:12] alone.  The backend reproduces all three vendor programs **exactly**,
and the `fract`, `floor` and `ceil` fixtures — each scaling the varying so the
quad crosses several step boundaries — match `floorf`/`ceilf` for every pixel.

No fourth member of the family was found.  `trunc` is therefore not emitted;
synthesise it from a compare and a select where it is needed.

## 2. The compare unit (MEASURED)

UNIT 1, `op2 = 0x00`, `e = 0x05`, W1 base `0x90000000`.  It takes one register
and one constant pair — and, as every UNIT 1 instruction does, takes the
constant's **pair index in W0's c field**, not in `e`.  It writes 1.0 or 0.0.

W0's `cls` field is the predicate:

| cls | predicate | first evidence |
|-----|-----------|----------------|
| 0 | `<`  | fs-discard-cond, fs-if-varying |
| 1 | `>=` | fs-step, which is `step(u, v1)` |
| 2 | `<=` | fs-min, fs-clamp lower bound |
| 3 | `>`  | fs-ternary, `gl_TexCoord[0].y > 0.5` |
| 4 | `==` | fs-pow and fs-sqrt zero guards |
| 5 | `!=` | fs-ternary's select on the result |

Bit 0 is the complement, so a negated comparison is the same instruction in
class `cls ^ 1`.  **All six were measured**, one fixture each, with the whole
varying ramp on one side of a per-channel threshold so that a class off by one
shows as an inverted channel rather than a few edge pixels; `==` and `!=`
compare a floored ramp against an integer so the test is exact.

Two registers cannot be compared directly.  The backend compares their
difference against zero, which is exact and uses only the form above.

## 3. Select (MEASURED)

`op2 = 0x1a`, W1 base `0xaf400c00` for a register condition and `0xa3400c00`
for a constant one, computing `dst = (a <cls> 0) ? b : c` per scalar.

**Only class 4 works.**  It is the only one captured — fs-sqrt's `sqrt(0)`
guard and fs-exp's lane merge — and class 5, which would have read more
naturally for a 0/1 condition, returns the false value for every fragment on
this hardware.  So a condition is tested against zero with `==` and the false
value is taken first.  There is also a register/constant form, `op2 = 0x1b`,
captured in fs-ternary and fs-pow, where class 5 *is* used; the two opcodes
evidently do not share a class table, and no attempt was made to use 0x1b.

A condition is a value holding 1.0 or 0.0, so `&&` is a product, `||` is the
larger of the two and `!` is a negate plus one — all measured together in one
fixture with each operator in its own channel.

## 4. min and max (CAPTURED, and a lowering)

`L_MIN_RC`/`L_MAX_RC` are W0 `cls` 2 and 3 with W1 base `0xa3200d50`.  Every
class-2/3 instruction in all 3605 slots of the corpus has the **same register
in both the b and the c field** and a constant index in `e`; the sources
confirm it, always a varying against a uniform.  There is no captured
register-register min or max anywhere.

The backend therefore lowers `min(a, b)` to `a + min(b - a, 0)` and `max` to
`a + max(b - a, 0)`, which is exact for all finite values and built only from
captured forms.  That is what took the `satmin` fixture from every pixel wrong
to none.

## 5. The special-function chains (MEASURED, algebra UNDECODED)

exp2, log2, rcp and rsq are **scalar** chains: a seed instruction followed by a
polynomial or a Newton refinement.  The backend reproduces the vendor's wiring
and coefficients with its own registers, because what the shared step
instruction `op2 = 0x13` computes for arbitrary operands is **not decoded** —
only that the captured sequence evaluates the function.

```
exp2   seed 0x9e851d40 (scalar select at bit 4)      fs-exp
       poly 0x982082d0, constant index in W0's c
       step 0xa2600cf0 twice, constant index in e
       mul  0xac2fc0c0
       coefficients 0x3af71c15/0x3c130514, 0x3d64b436/0x3e75ea9e,
                    0x3f317271/0x3f800000

log2   mov  0xa28ffc00 first, because the seed reads scalar 0 only
       seed 0x96852ec0                               fs-log
       poly 0x944041e0, constant index in W0's c
       step 0xa26008a0 three times
       mad  0xac4fcf00
       coefficients 0xbc188576/0x3d553bd2, 0xbe0cd266/0x3e77ab67,
                    0xbeb1d173/0x3ef51608, 0xbf389e52/0x3fb8aa10

rcp    seed 0xae2ff000 (scalar select at bit 8)      fs-smoothstep
       then twice: residual 0x8c26ac10 with W0 cls 5, step 0xae600300
       against the constant pair (0, 2)

rsq    mov  0xa28ffc00, then seed 0x9e26b090 with W0 cls 5 and a constant
       pair of (+inf, -inf) in W0's c              fs-sqrt
       then twice: mul 0xac2fcd00, then step 0xa2603f00 with W0 cls 7
       against the constant pair (0, 3)
```

Two things the captures only imply, each of which produced a wrong result
until it was got right:

- **The rcp residual reads the argument a second time**, so it must repeat the
  scalar select the seed took.  Without that, the components that live in the
  second scalar of a register came out of the chain for the first.
- **The rsq seed writes a pair whose second scalar every refinement needs
  again.**  Each step therefore copies that scalar across first (a move with
  `op0 = 0x1e`) and writes only the first scalar itself.  Overwriting both
  gave a result that was out by roughly a square root.

`pow` is `exp2(y * log2(x))`, which is what the vendor does too — fs-pow is
literally the log2 chain, a multiply and the exp2 chain, plus a zero guard.
There is no `pow` instruction.  `sqrt` is `x * rsq(x)` with the captured zero
guard, since `rsq(0)` is an infinity.

Measured against the C library, each for all 76800 pixels:
`exp2(-v*3)`, `log2(v*3+1)`, `pow(v*0.9+0.1, 2.5)`,
`pow(floor(v*4)*0.25, 2.5)`, `0.25/(v*0.75+0.25)`, `inversesqrt(v*3+1)`,
`sqrt(v)` and `normalize(v-0.5)`.

## 6. gl_FragCoord (MEASURED)

The raster engine publishes the fragment position in **register `0x7a`**, which
holds the column and the row, and **`0x7b`**, which holds z.  `r88` bit 19
declares that the program reads them; nothing else in the raster words, the
declaration, the strides or `r22` changes, which was established by decoding
the full command stream of `pr-fragcoord-300` against `fs-const` — the two
raster passes differ only in `r88`, the constant span and the program.

The backend floors `0x7a` and biases it by `(0.5, 0.5)`, because the screen
advertises a half-integer pixel centre.  It applies **no flip**: st's own
`nir_lower_wpos_ytransform` does whatever flip the shader asked for, and the
vendor's additional multiply-add against `(0, height - 1)` belongs to its own
lower-left convention.  Applying both gave a y that tracked x exactly.

## 7. Dot products (CAPTURED, and a lowering)

`op2 = 0x0e` is a four-component dot and there is a two-component partial at
UNIT 3 `op2 = 0x10`; fs-dot3 and fs-dot4 differ only in the second instruction.
Both forms take a **constant** operand per half, so a shader that dots two
computed vectors — which is what every lit shader does — cannot use them.  The
backend expands those into multiplies and adds instead, and leaves the native
forms for the one-uniform case.

## 8. Why `fs-log` and `fs-pow` still differ from the capture

The chains are right; the **layout** around them is not, and it is worth
saying exactly what the remaining difference is, because a plausible-looking
account of it has already been proposed and is wrong.

**The flagged cause is not a defect (CAPTURED).**  The log2 polynomial slots
were suspected of writing scalar 0 (`op0 = 0x1d`) while their chained `c`
operand's select is 2, which by the general rule reads scalar 1 — a step
reading a scalar its predecessor never wrote.  The vendor's own `fs-log`
does exactly the same thing: its three step slots are `a260c8a0`, `a26088a0`
and `a26048a0`, which are the backend's base word `a26008a0` with the
constant index in `e` and nothing else, `op0 = 0x1d` and `c` select 2
included.  The backend's chain is instruction for instruction the vendor's,
in opcode, operand wiring and every select; the single encoding difference in
the whole chain is the last instruction's destination mask, `0x1c` where the
vendor writes `0x1d` or `0x1e` (see below).  So the pattern cannot be the
defect, and the rule that the step class selects its `c` scalar by the bit
belonging to the destination scalar — bit 4 for a write to scalar 0, bit 5
for scalar 1 — is what fits all three chains (log2 `0x1d`/select 2, rsq
`0x1d`/select 0, exp2 `0x1e`/select 3).

**The chains are correct on hardware (MEASURED).**  Against the C library,
every one of 76800 pixels: `exp2`, `log2`, `pow`, `rcp`, `rsq`, `sqrt` and
`normalize`, 0 pixels differ, with `m9-mesa-glsl` on the M9.

**What the difference actually is.**  `fs-log` compiles to 41 slots and 12
constants against the vendor's 33 and 10, and the 8 extra slots are two
mechanisms, both layout:

1. **Two constants cannot be added in one slot.**  `log(u + 1.0)` is a
   constant plus a constant, since the vendor holds a uniform in its constant
   file too.  The vendor's `b081c10f` takes one constant index in `c` and a
   second in `e` and adds them (`0x13 = k6 + k7`).  The backend has no such
   form, so it moves 1.0 into a register first and adds register to constant:
   two extra slots, one per lane.

2. **A chain result is written to both scalars of its own register.**  The
   vendor ends each chain with `op0 = 0x1d` or `0x1e`, writing the result
   straight into the scalar of the destination pair where it belongs
   (`0x13 = (log2 z, log2 w)` from slots 10 and 28, `0x11 = (log2 x, log2 y)`
   from slots 26 and 29), so assembling the vec4 costs nothing.  The backend
   writes each chain result to both scalars of a fresh register and then
   assembles the pairs with the generic gather — a masked multiply per scalar
   and an add per lane, six slots and the two mask constants `(1,0)` and
   `(0,1)`.

   41 - 2 - 6 = 33, the vendor's count exactly.

3. **Order.**  Even with both closed, the programs would not match: the
   vendor interleaves the four chains rather than emitting them one after
   another, and the interleave is not a fixed pattern.  In `fs-log` the z
   chain occupies slots 1, 2, 3, 4, 6, 8, 10 and the w chain 5, 7, 9, 11, 12,
   14, 28 — started four slots apart, then alternating, with the w chain's
   last instruction deferred by fourteen slots; the x and y chains start two
   apart.  The four final writes come out in the order low-z, low-x, high-w,
   high-y, that is every low scalar before every high one.  That is a
   scheduler with a latency model, and reproducing `fs-log` and `fs-pow` byte
   for byte means reproducing it.  Nothing here was reverse engineered.

**`fs-pow` is 66 slots against the vendor's 64, and its guard turns out not
to be needed (MEASURED).**  The vendor guards `pow`: slots 27, 57, 59 and 60
of its program are `op2 = 0x1b`, the register/constant select with class 5,
which section 3 above records as captured but never used.  The backend emits
no guard, so `pow(0, y)` goes through `exp2(y * log2(0))` with nothing
protecting it from the infinity, and the `pow` fixture raises
`v * 0.9 + 0.1` to 2.5, so it never reached zero.  The new `pow0` fixture
closes that corner: a floor quantises the varying to 0, 0.25, 0.5 and 0.75,
so a quarter of the image has a base of exactly zero, and the result matches
`powf` for all 76800 pixels.  The unguarded chain returns the right answer
for a zero base on this hardware; whatever corner the vendor's select covers,
it is not this one.

## What is still open

- A two-constant add (`b081c10f`), and chain results written straight into
  the scalar of the pair they belong to, which together would take `fs-log`
  from 41 slots to the vendor's 33; and the vendor's chain scheduler, which
  byte-exactness needs on top of both.
- The algebra of `op2 = 0x13`, the step shared by all four chains.
- A register-register min, max or compare, and a fourth rounding class.
- Whether `op2 = 0x1a` has any usable class other than 4.
- Dependent texture reads: the raster engine fetches at an interpolated
  varying.  Seven corpus cases do sample at a computed coordinate, and every
  one of them is two raster passes — see
  `parhelia-m9-mesa-projective-textures.md`, which also measures that the
  sampler performs no divide of its own.
- `M9_FS_MAX_TEXTURES` is now 4, `glamor/xv-planar-3` binds three with a
  third `0x00700d88` declaration and `r88` bit 22 set, and the per-pass
  ceiling is still the driver's rather than the hardware's.
