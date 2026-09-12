Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 fragment combiner — the encoding a compiler needs

`parhelia-fragment-isa.md` is the long-form reverse engineering of the
fragment pipeline.  This is the compiler-facing extract: the complete field
layout, the register, modifier, replicate and opcode tables, the slot walk and
the limits, with each fact tagged.

Evidence tags:

* **PROVEN(hw)** — observed on our P690, or on the 0528 with a vendor ring
  capture of the same instruction.
* **PROVEN(enc)** — confirmed by re-encoding: `comb_program_selftest` in
  `parhelia_drm_test.c` builds a `p3d_comb_program` for each of the five
  GL_TEXTURE_ENV modes and requires the emitted batch to be **byte-identical**
  to the one the hardware-proven fixed-function texenv emitter produces.  All
  five match, so every field those five use is confirmed.
* **BIN** — read out of a vendor binary at the quoted address.
* **INF** — derived, not executed.
* **OPEN** — no writer found anywhere; leave zero.

Binaries: `PARD` = `MTXPARD.dll` (D3D, image base 0x10000), `PSER` =
`mtxpserd.dll`, `KRN` = `mtx_parhelia.o`, `GL` = the OpenGL ICD.

---

## 1. The two register pairs

A slot co-issues an **RGB** operation and an **alpha** operation.  Each half
has its own source word and its own opcode word; there is no write mask,
because the pipe *is* the register pair you write.

| register | half | contents |
|---|---|---|
| 0x1264 | RGB | slot address + the three source operands |
| 0x126c | RGB | opcode, replicates, scale, destination, saturate |
| 0x1260 | alpha | the same source layout with the slot field zero |
| 0x1268 | alpha | the same opcode layout, replicate bits 2/5/8 forced to zero |

### 1.1 Source word  [PROVEN(enc)]

```
 31    28 27       23 22    19 18       14 13    10 9         5 4      0
+--------+-----------+--------+-----------+--------+-----------+--------+
|  Cmod  |   Creg    |  Bmod  |   Breg    |  Amod  |   Areg    |  slot  |
+--------+-----------+--------+-----------+--------+-----------+--------+
```
`word = slot | Areg<<5 | Amod<<10 | Breg<<14 | Bmod<<19 | Creg<<23 | Cmod<<28`.
The slot address is OR-ed in at emission and is zero in the alpha word.
[BIN `PARD 0x182190/0x183be0`, `KRN 0x6b170`; all six vendor packers mask with
`0xffffc01f`]

### 1.2 Opcode word  [PROVEN(enc)]

```
 31    25 24  23    20 19   17 16 15 14        9 8    6 5    3 2    0
+--------+---+--------+------+-----+-----------+------+------+------+
|  OPEN  |sat|  dreg  |scale | OPEN|  opcode   | Crep | Brep | Arep |
+--------+---+--------+------+-----+-----------+------+------+------+
```
`word = opcode<<9 | Arep | Brep<<3 | Crep<<6 | scale<<17 | dreg<<20 | sat<<24`.

Bits 15, 16 and 25..31 have **no writer in any of the four binaries**, yet
every packer deliberately preserves them (`KRN 0x6b20e` masks byte 2 with
`0xf`, `0x6b21a` with `0xf1`).  They are real fields; leave them zero. [BIN,
negative result]

The alpha packers clear bits 2, 5 and 8 (`mask 0x124`), i.e. the alpha half
can select a channel but not "replicate alpha" — which is the natural read
there anyway, so a `.aaaa` swizzle needs no special case in a compiler.
[BIN `KRN 0x6a3ac/0x6a493/0x6a582`; PROVEN(enc) by the DECAL match]

---

## 2. Registers  [BIN `PARD .data 0x1d7a78`; PROVEN(hw) for 0, 4, 0x10]

| code | name | meaning |
|---|---|---|
| 0x00–0x03 | `t0`–`t3` | texture **pipe** output — a physical pipe, not a logical unit |
| 0x04 | `r0` | temporary 0, and the register the fragment colour is read from |
| 0x05 | `r1` | temporary 1 |
| 0x0e | `c0` | this instruction's first constant (0x1278 / 0x127c) |
| 0x0f | `c1` | this instruction's second constant (0x1280 / 0x1284) |
| 0x10 | `v0` | interpolated colour 0 |
| 0x11 | `v1` | interpolated colour 1 |

There is **no literal 0 or 1 selector**: a constant is the only way to get a
literal, unlike the vertex engine's swizzle codes.  The destination field is
four bits wide, so `c0`, `c1`, `v0` and `v1` are structurally read-only.

---

## 3. Modifiers  [BIN `PARD .data 0x1d78f8`; PROVEN(enc) for 0 and 0xf]

| code | operation | GL token |
|---|---|---|
| 0 | `x` | — |
| 1 | `-x` | `GL_ARG_NEGATE_MTX` |
| 2 | `x - 0.5` | `GL_ARG_BIAS_MTX` |
| 3 | `-(x - 0.5)` | `GL_ARG_BIAS_NEGATE_MTX` |
| 4 | `2x - 1` | `GL_ARG_SIGNED_MTX` |
| 5 | `-(2x - 1)` | `GL_ARG_SIGNED_NEGATE_MTX` |
| 6–0x0e | unused by every vendor driver | OPEN |
| 0x0f | `1 - x` | `GL_ARG_INVERT_MTX` |

Gotcha [BIN `PARD 0x1816f0`]: for a **texture** source with a modifier other
than 0 or 1 the vendor compiler does not use the ALU bias/scale — it clears
bit 4 of that unit's sampler state instead.  Whether codes 2–5 also work on a
texture source through the ALU is OPEN.

### 3.1 Replicates  [BIN `PARD 0x1839f0`]

| code | meaning | tag |
|---|---|---|
| 0 | natural per-channel read | PROVEN(hw) |
| 1 | replicate `.b` | BIN |
| 2 | replicate `.g` | BIN |
| 3 | replicate `.r` | **PROVEN(hw)** — `0x1268 = 0x01400403` feeds `t1.r` into fragment alpha and drives both alpha test and SRC_ALPHA blending on the P690 |
| 4 | replicate `.a` | **PROVEN(enc)** — GL_DECAL's `LRP r0, TEX, PREV, TEX.a` is `0x126c = 0x01400b00`, i.e. `Crep = 4` |
| 5–7 | never emitted | OPEN |

Structure [BIN `PARD 0x183cfa`]: `rep = (alpha_flag & 1) << 2 | (channel & 3)`
with channel 0 = none, 1 = b, 2 = g, 3 = r.

---

## 4. Opcodes, `0x126c[14:9]`

Three byte-identical tables in three independent binaries [BIN `PARD .data
0x1d77f0` and `0x1d6740`, `KRN mtx1291 .data vma 0x15e40`].

| value | operation | tag |
|---|---|---|
| 0x00 | `MUL` — `A * C` | PROVEN(enc) |
| 0x02 | `MOV` — `A` | PROVEN(enc) |
| 0x03 | `SUB` — `A - B` | BIN |
| 0x04 | `MAD` — `A * C + B` | PROVEN(hw) in PARD's video path |
| 0x05 | `LRP` — `A * C + B * (1 - C)` | PROVEN(enc), twice: DECAL and BLEND |
| 0x06 | `ADD` — `A + B` | PROVEN(enc) |
| 0x08 | `DP3` — `dot3(A, C)`, broadcast | PROVEN(hw) |
| 0x10 | `DP4` — `dot4(A, C)`, broadcast | BIN |
| 0x26 | `CND` — `A > 0.5 ? B : C` | BIN |
| 0x2e | `CMV` | BIN |
| 0x3e | `CMP` — `A >= 0 ? B : C` | BIN |

**The operand names are positional**, i.e. `MUL r0, X, Y, Z` means A = X,
B = Y, C = Z, and each opcode reads only the operands its expression names.
Getting this wrong is the easiest way to write a plausible but wrong
compiler, which is why the five-mode equivalence test exists.

Field structure [INF; do not synthesise unlisted encodings]:

```
bits[11:9]  ALU function: 0 MUL, 2 MOV, 3 SUB, 4 MAD, 5 LRP, 6 ADD
bit 12      3-component sum   (MUL | bit12 = DP3)
bit 13      4-component sum   (MUL | bit13 = DP4)
bit 14      conditional select (ADD-base | bit14 = CND, +12 = CMV, +12+13 = CMP)
```

Operand liveness, as the vendor driver itself decodes it [BIN `PARD
0x182c6a..0x182cdb`] — useful for dead-source elimination:

```
A : always live
B : live if (word & 0x0200) or (word & 0x0800)
C : live if !(word & 0x0400)
all three: if (word & 0x7000) >= 0x4000
```

`DP3`/`DP4` write all four channels, so a slot carrying one must carry the
same opcode in **both** halves [BIN `GL 0x1b67d6`]; the driver's compiler
enforces that when it zips the two halves.

---

## 5. Scale, saturate, destination

`scale` = `0x126c[19:17]`, a signed 3-bit shift count [BIN `GL 0x1b3bb0` plus
its jump table]:

| value | result | GL token |
|---|---|---|
| 0 | x1 | `GL_NONE_MTX` |
| 1 | x2 | `GL_SCALE_X2_MTX` |
| 2 | x4 | `GL_SCALE_X4_MTX` |
| 3 | x8 | `GL_SCALE_X8_MTX` |
| 4, 5 | unused | OPEN |
| 6 | /4 | `GL_SCALE_D4_MTX` |
| 7 | /2 | `GL_SCALE_D2_MTX` |

`sat` = bit 24.  libGL's fixed-function generator sets it and never clears
it, so the fixed-function pipeline always saturates [BIN `GL 0x10e330`,
PROVEN(hw) in every captured program]; the driver's compiler does the same.

`dreg` = bits [23:20], four bits, so only `t0`–`t3`, `r0` and `r1` are
writable.  `r0` is the fragment colour.  [PROVEN(hw): `dst = 4` in all three
captured program blobs]

---

## 6. Slots, ordering and the quantum

| rule | tag |
|---|---|
| Ten instruction slots; instruction index → hardware slot address is the fixed walk `{0, 2, 4, 8, 10, 1, 3, 5, 9, 11}` | [BIN `KRN .rodata+0x1ae0` verbatim, same walk in `PARD 0x170980`]; PROVEN(enc) for the first five |
| Slot addresses 6 and 7 are never used | BIN, all three binaries |
| Structure: `slot = 2 * pair + pass` with pair ∈ {0,1,2,4,5} — five ALUs in two passes, pair 3 skipped for no documented reason | INF |
| The instruction count is padded to exactly **5 or 10**, never anything else | [BIN `PARD 0x1708d0`, `KRN 0x692bd`] |
| More than 5 instructions additionally sets `0x1300` bit 19 and halves the fill rate | [BIN `KRN 0x2576b`, `PSER 0x177230`] — the driver stays at five and does not set that bit |
| The pad instruction is `MOV r0, r0`: `0x1264 = 0x80 \| slot`, `0x126c = 0x00400400`, no alpha pair.  **The padding is the terminator** — there is no slot-count register | PROVEN(hw) on the ring, PROVEN(enc) |
| A later slot reads an earlier slot's result with no latency requirement | PROVEN(hw): the two-unit MODULATE path is `MUL r0, t0, v0` in slot 4 then `MUL r0, t1, r0` in slot 8 |

### 6.1 Slot 0 is not a texture-address instruction

The word previously described as a "TA stage" is an ordinary combiner `ADD`
inserted when a stage runs on a ganged pipe mask; it sums the partial results
of the two ganged pipes, each of which returns zero for the fragments it does
not own.  [BIN `PSER 0x1829d0 → 0x183830`, `KRN mtx1484`; PROVEN(hw) on the
vendor ring and in our own ganged-pipe route]

| mask | merge | 0x1264 | 0x126c |
|---|---|---|---|
| 3 (pipes 0+1) | `ADD t0, t0, t1` | 0x00004000 | 0x00000c00 |
| 0xc (pipes 2+3) | `ADD t2, t2, t3` | 0x0000c040 | 0x00200c00 |
| **5 (the P690 gang, pipes 0+2)** | `ADD t0, t0, t2` | 0x00008000 | 0x00000c00 |
| 0xf, anisotropic | `ADD_d2` pairs | 0x4000 / 0xc040 | 0x0e0c00 / 0x2e0c00 |

Merges are inserted at the head of the list, so they take slots 0 (then 2),
and each costs one of the ten slots.  A stage on a single pipe gets no merge.

### 6.2 Which register a stage reads  [PROVEN(hw)]

`t_k` is **physical pipe k's** output.  A logical unit's register code is the
first pipe of its pipe mask; there is no register anywhere in 0x1100..0x13ff
that remaps logical sources to physical pipes.  On the P690 two-unit route
logical unit 1 sits on physical pipe 2, which is why the second stage's
source must be `t2`: probing `Areg = 1` returned black, `Areg = 2` returned
exactly the expected red (96 edge-convention mismatches out of 18,336 covered
pixels).

---

## 7. Constants

| item | value | tag |
|---|---|---|
| Selectors | exactly two, `c0 = 0x0e` and `c1 = 0x0f`, allocated **per instruction** | [BIN `PARD 0x1805d0`] |
| Registers | `c0` → 0x1278 (x, y) + 0x127c (z, w); `c1` → 0x1280 + 0x1284 | [BIN `HAL 0x6dd9a`] |
| Packing | two signed 11-bit values per dword, at bits [10:0] and [26:16] | PROVEN(enc) |
| Format | signed **Q1.10**, range [-1.0, +0.999] | BIN + the D3D cap `MaxPixelShaderValue = 1.0` |
| Host conversion | `v = (int)truncf(f * 1024); clamp to [-1024, 1023]` — round **toward zero** | [BIN `KRN mtx2132`]; PROVEN(enc) |
| Component order | **0x1278 = R \| A<<16, 0x127c = B \| G<<16** | **PROVEN(hw)**, see below |
| Total | two vec4s per slot; an API-level constant file of up to 32 `def c#` is folded down at compile time.  **There is no other constant path to the chip.** | [BIN `PARD 0x180770`] |

A compiler must therefore treat constants as a per-instruction resource, and
a value outside [-1, 1) cannot be expressed at all.

### 7.1 The component order is not (r, g) then (b, a)  [PROVEN(hw)]

The natural reading of the two registers is red and green in the first,
blue and alpha in the second.  That is wrong.  Compiling the GLSL shader
`gl_FragColor = texture2D(s, c) * constant` and rendering it on the P690
returned the constant's **alpha** in the green lane, with red and blue exact
to the last bit.  Swapping the two high halves - red and alpha in 0x1278,
blue and green in 0x127c - makes that shader and
`texture2D(s, c) * gl_Color + constant` match the GL specification exactly.

This also means the fixed-function `GL_TEXTURE_ENV_COLOR` packing in the
Gallium driver (`env_colour_rg` / `env_colour_ba`) has the same swap to make:
GL_BLEND has never rendered correctly on this hardware.

Test: `tools/parhelia-gl-combiner-test.c`, checks "glsl uniform constant
times texture" and "glsl mad: texture * colour + constant".

---

## 8. Limits

| limit | value | tag |
|---|---|---|
| `GL_MAX_FP_OPS_MTX` | 10 instruction slots | BIN `GL` |
| `GL_OPT_FP_OPS_MTX` | 5 — the free quantum | BIN `GL` |
| texture-address stages | 4, sharing the same ten-slot budget | BIN `KRN mtx2132` |
| texture pipes on P690 | 4 physical, all addressable | BIN |
| writable registers | 2 (`r0`, `r1`); `r0` is the output | field width |
| interpolated colours | 2 (`v0`, `v1`) | BIN `GL` |
| P690 ganging | only the (0,2) pair; masks 3, 0xc and 0xf point at an all-zero cost table on this class and the vendor never programs them | BIN |
| P690 multi-stage routes | 2 stages → pipes {0,2}; 3 → {0,1,3}; 4 → {0,1,2,3} | BIN |

---

## 9. What the driver implements

`reference/parhelia3d.h` carries this as `struct p3d_comb_program` and
`reference/p3d_state.c` encodes it.  `parhelia_fs_compile.c` is the NIR front
end.  Current capability, with the five-instruction quantum and the pipe
merges taken off the top:

| draw | merge slots | ALU instructions available |
|---|---|---|
| untextured | 0 | 5 |
| one texture | 1 | 4 |
| two textures | 2 | 3 |

Going to ten needs `0x1300` bit 19 and costs half the fill rate; it is the
obvious next extension and is untested.

## 10. Measured coverage

`tools/parhelia-gl-combiner-test` compares the rendered result of 21 shapes
with the value the GL specification requires.  On the P690, with the driver's
instruction-shape matcher (`PARHELIA_FS_COMPILER` unset) **18 of 21 fail**;
with the data-flow compiler **9 fail**.

Exact with the compiler: GL_REPLACE, GL_MODULATE, GL_ADD, a two-unit
MODULATE chain, an untextured constant colour, and seven GLSL 1.20 shaders -
multiply, add, `1 - x`, an alpha replicate, an rgb/alpha split, a uniform
constant and a multiply-add.

Still wrong, precisely characterised:

* **LRP loses the green lane of a replicated source.**  `LRP r0, TEX, PREV,
  TEX.a` returns `PREV.g`, i.e. the C operand's green reads as zero, while
  its red and blue are exact.  The same alpha replicate is exact in a MUL, so
  this is specific to LRP's use of C.  It costs GL_DECAL, GL_BLEND and
  `mix()`.
* **A second texture unit reads black** through a compiled program, although
  the fixed-function two-unit path is exact.  The P690 places logical unit 1
  on physical pipe 2 and the merge instruction in slot 2 leaves the result in
  `t1`; the compiled program's register choice or its slot offset is wrong.
* The vertex colour of an **untextured** draw comes out white.  That predates
  the compiler: the driver selects the WARP replace program, which forces the
  vertex colour to 1.
* A shader the compiler cannot express still takes a hardware path and
  renders the raw texture instead of falling back to the draw module.  The
  fallback predicate has to consider the compiler's verdict.

## 11. Still open

* `0x126c` bits 15, 16 and 25..31: real fields, no writer found.
* Modifier codes 6..0x0e, replicate codes 5..7, opcode bits[11:9] values 1
  and 7, scale values 4 and 5.
* Why slot addresses 6 and 7 are skipped and why the walk is even-then-odd.
* The execution order of instructions 6..10 relative to slot address.
* Pairing constraints between the RGB and alpha halves for opcodes other than
  DP3/DP4.
* The texture-address descriptor's engine register (leading candidate
  `0x1200 + unit*4` bits [23:19]); nothing is implemented against it.
