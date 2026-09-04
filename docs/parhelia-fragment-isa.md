# Parhelia-512 fragment / pixel-shader ISA

Reverse-engineering of the Parhelia **pixel (fragment) combiner** — the block the
Windows D3D driver targets when it compiles ps.1.x, the block the Linux GL ICD
exposes as `GL_MTX_fragment_shader`, and the block the X driver's
`CDPSPixelShader` class uses for video colour conversion / procamp.
Counterpart of `parhelia-vertex-isa.md` (WARP microcode, vertex side — out of
scope here).

**Revision 2026-09-05: this file was substantially rewritten.** The 2026-09
version had the register set right but several field assignments wrong; the
corrections are listed in §12. Do not carry the old bit tables forward.

Sources actually read for this revision:

| tag | binary | notes |
|---|---|---|
| `[PARD]` | `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | PE i386 Windows D3D driver, advertises `D3DPS_VERSION(1,3)` = `0xFFFF0103` (`.text` 0x86c90). Disassembly: `scratchpad/pard.asm`, `objdump -d -M intel`. Section map: `.text` VMA 0x10300 / file 0x300, `.rdata` VMA 0x19b700 / file 0x18b700, `.data` VMA 0x19fb00 / file 0x18fb00. |
| `[HAL]` | `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | x86-64, exported `CDPSPixelShader::*` symbols. Disassembly: `scratchpad/mtx_drv.asm`. |
| `[GL]` | `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` | stripped ICD. `scratchpad/libGL64.asm`. |
| `[HDR]` | `opengl6/include/glext.h` | ships the whole `GL_MTX_fragment_shader` token block. |

Every claim below is tagged **CONFIRMED** (read out of a binary, and where
possible corroborated by a second, independent binary) or **INFERRED**.
Things that could not be pinned down are listed as such in §13 rather than
guessed.

Decoder: `tools/parhelia-fragdis.py` (§11).

---

## 1. Machine model

**CONFIRMED.** The combiner is a fixed list of instruction slots, each slot
programmed through a small window of engine registers. Engine register =
`0x1000 + index*4`, reached as (register-index, value) dword pairs in the DMA
command stream (see §10).

Per slot the driver writes at most eight registers:

| register | role |
|---|---|
| `0x1260` | texture-address instruction — source/control word |
| `0x1268` | texture-address instruction — destination/opcode word |
| `0x1264` | ALU instruction — source/control word, **and carries the slot address** |
| `0x126c` | ALU instruction — destination/opcode word |
| `0x1278`, `0x127c` | stage constant **c0** (4 × signed 11-bit) |
| `0x1280`, `0x1284` | stage constant **c1** (4 × signed 11-bit) |

`0x1260`/`0x1268` are believed to use the *same* bit layout as
`0x1264`/`0x126c` (**INFERRED**). The evidence is `PARD 0x170a10`, which
maintains a shadow copy of every slot and, when the slot has no texture-address
instruction (flag bit0 clear), substitutes the ALU pair with bits 2, 5 and 8
cleared — `and eax,0xfffffedb` @0x170abc, i.e. `& ~0x124`, exactly the three
alpha-replicate bits of a `0x126c`-shaped word. The mask is applied bluntly to
both words of the pair, so it argues for a shared layout without proving it.
No program in either binary puts a non-trivial instruction in `0x1260`/`0x1268`
(§9.4).

### 1.1 Slot addressing and the 5-stage / 2-pipe organisation

`0x1264[4:0]` is the slot address; it is merged in at upload time, not at
compile time (**CONFIRMED**: `HAL CDPSPixelShader::UpdateHardware` @0x6d896
`edx &= ~0x1f; edx |= slot`; `PARD 0x1707b0` @0x1707b3 does the identical
XOR-merge; all four packers mask `0x1264` with `0xffffc01f` and never touch
bits [4:0]).

The driver keeps a flat array of **10 instruction slots**, 36 bytes each
(**CONFIRMED**: `PARD` ctor 0x170860 fills `edi = 0xa` entries at stride 0x24;
`HAL BuildShader` 0x6e620 pads the same 10-entry array). The emitter walks that
array and writes each entry to a hardware slot in this order
(**CONFIRMED**, `PARD 0x170980`, the `esi` sequence with its `esi==6||esi==7 →
esi+=2` and `esi==12 → esi=1` fixups):

```
instruction index  0  1  2  3  4   5  6  7  8  9
hardware slot      0  2  4  8 10   1  3  5  9 11
```

Slots **6 and 7 are never used**. Read as `slot = 2*stage + pipe`, this is:

```
combiner stage   0  1  2  (3 reserved)  4  5
colour pipe      0  2  4       6        8  10
alpha  pipe      1  3  5       7        9  11
```

so **five usable combiner stages, each with a colour instruction and an alpha
instruction**; instructions 0-4 are the colour pipe, 5-9 the alpha pipe.
(**INFERRED** from the slot map plus the padding rule below; the even/odd =
colour/alpha reading is the only one that explains both.) Why the stage at
slots 6/7 is skipped is **not known**.

The instruction count is padded to **5 or 10** before upload — never anything
else (**CONFIRMED**, `PARD 0x1708d0`: `count>10 → 10`, else `count<=5 → 5` else
`10`; `HAL 0x6e667`: `>10 → 10`, `<6 → 5`, else `10`). Pad slots are filled with
`0x1264 = 0x00000080`, `0x126c = 0x00400400`, i.e. `MOV r0, r0`
(**CONFIRMED**, both binaries write those two literals).

### 1.1a Colour / alpha pairing and co-issue

There is **no co-issue bit in the instruction words**. The colour and alpha
instruction of a combiner stage are two *separate* slots that happen to share a
stage number, and they are addressed purely by the slot field. Consistent with
that:

* the D3D `D3DSI_COISSUE` flag (`0x40000000`) is never tested anywhere in
  MTXPARD's shader compiler (`0x17xxxx`-`0x18xxxx`); **CONFIRMED** by scanning
  every occurrence of that immediate in `.text`;
* the GL API exposes the split explicitly as `glFragProcRGBAOp{1,2,3}MTX` versus
  `glFragProcAlphaOp{1,2,3}MTX` over one shared opcode/selector encoder
  (`GL 0x1b60e0` / `0x1b64e0`, both calling `0x1b3a50`);
* no field in `0x126c` selects a write mask (§8 shows bits [19:17] are the
  result scale).

The practical rule for a code generator: emit your RGB instructions as list
entries 0-4 and your alpha instructions as entries 5-9, then apply the slot map
of §1.1. Pairing constraints *within* a stage (whether the colour and alpha
instruction of the same stage must agree on anything) are **not known**.

### 1.2 Per-slot flag byte

The 36-byte software slot record is `{+0x00 flags, +0x04 0x1264, +0x08 0x126c,
+0x0c 0x1260, +0x10 0x1268, +0x14..+0x20 constants}` in `PARD`; in `HAL` the
same record sits at `this + 0xc + i*36` with fields shifted by 0xc. The flag
byte selects which optional registers are emitted (**CONFIRMED**,
`PARD 0x1707b0` / `HAL CDPSPixelShader::UpdateHardware` 0x6d830):

| flag bit | emits |
|---|---|
| bit0 | `0x1260`, `0x1268` (texture-address instruction present) |
| bit1 | `0x1278`, `0x127c` (constant c0 used) — **INFERRED** |
| bit2 | `0x1280`, `0x1284` (constant c1 used) — **INFERRED** |
| always | `0x1264`, `0x126c` |

Both drivers test `flags & 6` and then emit all four constant registers, so the
individual bit meanings are inferred from usage: `HAL
BuildColorConvertShader` starts a stage with flags = 6 while it reads both c0
(srcC = 0xE) and c1 (srcB = 0xF), then clears bit2 for the follow-up stage at
the same moment it stops using c1 (`and BYTE PTR [...],0xfb` @0x6dee1).

---

## 2. `0x1264` — source / control word  **CONFIRMED**

```
 31    28 27       23 22    19 18       14 13    10 9         5 4      0
+--------+-----------+--------+-----------+--------+-----------+--------+
| Cmod   |  Creg     | Bmod   |   Breg    | Amod   |   Areg    |  slot  |
+--------+-----------+--------+-----------+--------+-----------+--------+
```

* `slot`   [4:0]   hardware slot address, merged at upload (§1.1)
* `Areg`   [9:5]   source A register selector (§4)
* `Amod`   [13:10] source A modifier (§5)
* `Breg`   [18:14] source B register selector
* `Bmod`   [22:19] source B modifier
* `Creg`   [27:23] source C register selector
* `Cmod`   [31:28] source C modifier

Evidence, `PARD 0x182190` (the ps.1.x instruction packer):

```
182295: and edx,0xffffc01f ; or edx,(mod&0xf)<<5|(reg&0x1f) <<5   ; source A
1822eb: and ecx,0xff803fff ; or edx,(mod&0xf)<<5|(reg&0x1f) <<14  ; source B
182349: and eax,0x007fffff ; or ecx,(mod    )<<5|(reg&0x1f) <<23  ; source C
```

Cross-checks: `HAL BuildPassThroughShader` @0x6d7f6 writes only
`(input<<5) & 0x3e0`; `PARD 0x1816f0` reads the A and B modifier fields back as
`(w>>10)&0xf` and `(w>>19)&0xf`; the ps.1.4 packer `PARD 0x183be0` uses identical masks
(`0x183c9d`, `0x183cec`, `0x183d55`).

---

## 3. `0x126c` — destination / opcode word  **CONFIRMED except where noted**

```
 31    25 24  23    20 19   17 16 15 14        9 8    6 5    3 2    0
+--------+---+--------+------+-----+-----------+------+------+------+
|  ????  |sat|  dreg  |scale |?????|  opcode   | Crep | Brep | Arep |
+--------+---+--------+------+-----+-----------+------+------+------+
```

* `Arep`/`Brep`/`Crep` [2:0]/[5:3]/[8:6] — per-source channel replicate (§6)
* `opcode`  [14:9]  — ALU function (§7)
* bits 15,16 — **unaccounted for**; no writer found
* `scale`   [19:17] — result scale (§8)
* `dreg`    [23:20] — destination register (§4, 4-bit field so only `r0`/`r1`
  and the texture units are addressable)
* `sat`     [24]    — saturate result to [0,1]
* bits 25-31 — **unaccounted for**; no writer found

Evidence, `PARD 0x182190`:

```
18224b: and ecx,0xfe01ffff ; or (((mod&1)<<4 | reg&0xf) <<3 | comp&7) <<17
1822a8: 0x126c[1:0] = srcA replicate   (and eax,0x03)
182306: 0x126c[4:3] = srcB replicate   (and eax,0x18)
18235c: 0x126c[7:6] = srcC replicate   (and ecx,0xc0)
```

The third bit of each replicate field (bits 2, 5, 8 — mask `0x124`) is written
only by the ps.1.4 packer, `PARD 0x183cfa`:
`0x126c[2:0] = (alphaFlag & 1) << 2 | channel & 3`.

Cross-checks: `HAL BuildPassThroughShader` @0x6d7f3/0x6d809 writes
`((output<<20) & 0xf00000) | 0x400`; `HAL BuildColorConvertShader` @0x6dda1
writes `((output<<20) & 0xf00000) | 0x803` (MAD with source-A replicate = red).

---

## 4. Source / destination register selectors  **CONFIRMED**

Table `PARD .data 0x1d7a78`, indexed `regType*16 + regNum`, and its exact
inverse `0x1d7978` (pairs of `{regType, regNum}` indexed by hardware code):

| code | name | D3D | GL (`glext.h`) |
|---|---|---|---|
| 0x00 | `t0` | `t0` | `GL_TEXTURE0_ARB` |
| 0x01 | `t1` | `t1` | `GL_TEXTURE1_ARB` |
| 0x02 | `t2` | `t2` | `GL_TEXTURE2_ARB` |
| 0x03 | `t3` | `t3` | `GL_TEXTURE3_ARB` |
| 0x04 | `r0` | `r0` | `GL_TEMPORARY0_MTX` == `GL_OUTPUT_COLOR_MTX` |
| 0x05 | `r1` | `r1` | `GL_TEMPORARY1_MTX` |
| 0x06-0x0D | — | — | unused; **0x0D is the "no destination" sentinel** (`PARD 0x181fc7`, `0x1839d7`) |
| 0x0E | `c0` | first constant referenced by the instruction | registered invariant |
| 0x0F | `c1` | second constant referenced | registered invariant |
| 0x10 | `v0` | `v0` | `GL_COLOR0_MTX` (iterated diffuse) |
| 0x11 | `v1` | `v1` | `GL_COLOR1_MTX` (iterated specular) |

`r0` is both temp0 and the **program result register** — the fragment colour is
read out of `r0`.

The destination field is only 4 bits (`0x126c[23:20]`), so the writable set is
codes 0-4 plus 5; `c0`/`c1`/`v0`/`v1` are read-only, which follows directly
from the field width. **CONFIRMED** by the field mask `0xf00000` in both
binaries.

---

## 5. Source modifiers  **CONFIRMED**

4-bit field per source, table `PARD .data 0x1d78f8` = `{0,1,2,3,4,5,0xF,0,...}`
indexed by the D3D `D3DSPSM_*` code:

| code | operation | D3D | GL (`glext.h`) |
|---|---|---|---|
| 0 | `x` | `_none` | — |
| 1 | `-x` | `_neg` | `GL_ARG_NEGATE_MTX` |
| 2 | `x - 0.5` | `_bias` | `GL_ARG_BIAS_MTX` |
| 3 | `-(x - 0.5)` | `_bias _neg` | `GL_ARG_BIAS_NEGATE_MTX` |
| 4 | `2x - 1` | `_bx2` (sign) | `GL_ARG_SIGNED_MTX` |
| 5 | `-(2x - 1)` | `_bx2 _neg` | `GL_ARG_SIGNED_NEGATE_MTX` |
| 0xF | `1 - x` | `1-` (complement) | `GL_ARG_INVERT_MTX` |

Codes 6-0xE are not produced by either driver. Note the ordering of
`GL_ARG_NEGATE_MTX` … `GL_ARG_SIGNED_NEGATE_MTX` (0x8AB5-0x8AB9) matches
hardware codes 1-5 one-for-one **[HDR]**, which independently corroborates the
D3D-derived table.

`_bias` / `_bx2` on a **texture** source is not done in the ALU: the compiler
instead flips a bit in that texture unit's sampler state
(**CONFIRMED**, `PARD 0x1816f0`: for each source whose kind field says "texture"
and whose modifier is neither 0 nor 1, `*texState &= ~0x10`). That is the
mechanism behind `GL_TEX_UNSIGNED/SIGNED/BIASED_MTX` (0x8AC0-0x8AC2) **[HDR]**.

---

## 6. Source channel replicate  **CONFIRMED**

3-bit field per source in `0x126c`. Derived from the D3D source swizzle byte
(`PARD 0x1839f0`, the ps.1.4 decoder, which is the only place that emits the
top bit):

| field | meaning | from D3D swizzle |
|---|---|---|
| 0 | no replicate (natural per-channel read) | `0xE4` (identity) and anything else |
| 1 | replicate `.b` | `0xAA` |
| 2 | replicate `.g` | `0x55` |
| 3 | replicate `.r` | `0x00` |
| 4 | replicate `.a` | `0xFF` |

Codes 5-7 unknown. `GL_REPLICATE_MTX` with `GL_RED/GREEN/BLUE/ALPHA_MTX`
(0x8ABA-0x8ABD) **[HDR]** exposes exactly these four choices.

The `.r/.g/.b` replicate is what the video colour-space-conversion shader uses:
`HAL BuildColorConvertShader` emits three stages whose `0x126c` low bits are
3, 2, 1 (@0x6dda1 `or 0x803`, @0x6df03 `(x & ~3) | 2`, and the analogous `| 1`)
— one stage per input channel.

---

## 7. Opcode table  **CONFIRMED**

`0x126c[14:9]`. The tables below are read verbatim out of MTXPARD `.data`.
Both are 24-byte-per-entry arrays of
`{0x1264 base, srcA←, srcB←, srcC←, 0x126c base, dest-gate}`, where the three
middle dwords say **which source operand of the source language feeds hardware
slot A, B, C** (value 7 = slot unused) and the last dword gates destination
decoding (7 = no destination).

### 7.1 ps.1.x (D3DSIO opcode) → hardware — table `0x1d6f98`, used by `PARD 0x182190`

Any opcode not listed falls through to the default row
`{0x1264=0, srcs=7,7,7, 0x126c=0x400, no dest}` — i.e. a bare `MOV`.

| D3DSIO | op | A← | B← | C← | `0x126c` base |
|---|---|---|---|---|---|
| 0x00 | NOP | – | – | – | `0x00400400` (with `0x1264 = 0x80`: `MOV r0,r0`) |
| 0x01 | MOV | s0 | – | – | `0x0400` |
| 0x02 | ADD | s0 | s1 | – | `0x0c00` |
| 0x03 | SUB | s0 | s1 | – | `0x0600` |
| 0x04 | MAD | s1 | s2 | s0 | `0x0800` |
| 0x05 | MUL | s0 | – | s1 | `0x0000` |
| 0x08 | DP3 | – | – | – | `0x1000` — see note |
| 0x09 | DP4 | s0 | – | s1 | `0x2000` |
| 0x12 | LRP | s1 | s2 | s0 | `0x0a00` |
| 0x50 | CND | s0 | s1 | s2 | `0x4c00` |
| 0x58 | CMP | s0 | s1 | s2 | `0x7c00` |

**Note on DP3:** in this table the DP3 row's three source columns are all 7
(no operand mapping) while the `0x126c` base is correct. The ps.1.4 table
(§7.2) has the correct `A←s0, C←s1`. This looks like a genuine defect in the
ps.1.x path of the shipped driver rather than a different encoding; an open
driver should use the ps.1.4 mapping. **CONFIRMED** (both tables re-dumped from
file offsets 0x1c6f98 and 0x1c6740).

### 7.2 ps.1.4 variant — table `0x1d6740`, used by `PARD 0x183be0`

Identical to §7.1 except `DP3 = {A←s0, C←s1}`. Everything else matches.

### 7.3 GL / internal opcode → hardware — table `0x1d77f0`, used by `PARD 0x182390`

Exactly 11 entries, in the same order as the `GL_OP_*_MTX` tokens
(0x8AA0-0x8AAA) and as the internal opcode numbers libGL's mapper at
`GL 0x1b3a50` produces. **This is the authoritative op → `0x126c` base table for
an open driver.**

| internal | `GL_OP_*_MTX` | A← | B← | C← | `0x126c` base | hardware function |
|---|---|---|---|---|---|---|
| 0 | `MOV` | s0 | – | – | `0x0000_0400` | `A` |
| 1 | `ADD` | s0 | s1 | – | `0x0000_0c00` | `A + B` |
| 2 | `SUB` | s0 | s1 | – | `0x0000_0600` | `A - B` |
| 3 | `MUL` | s0 | – | s1 | `0x0000_0000` | `A * C` |
| 4 | `DP3` | s0 | – | s1 | `0x0000_1000` | `dot3(A, C)` |
| 5 | `DP4` | s0 | – | s1 | `0x0000_2000` | `dot4(A, C)` |
| 6 | `MAD` | s1 | s2 | s0 | `0x0000_0800` | `A * C + B` |
| 7 | `LRP` | s0 | s1 | s2 | `0x0000_0a00` | `B + C*(A - B)` |
| 8 | `CND` | s0 | s1 | s2 | `0x0000_4c00` | `A > 0.5 ? B : C` |
| 9 | `CMV` | s0 | s1 | s2 | `0x0000_5c00` | conditional move |
| 10 | `CMP` | s0 | s1 | s2 | `0x0000_7c00` | `A >= 0 ? B : C` |

`CMV = 0x5c00` closes the last open item of the previous revision (it was
`[CAP]` there); it is absent from the D3D tables because ps.1.x has no such
token.

### 7.4 Hardware operand roles  **INFERRED (from the slot-assignment tables)**

The three tables agree on a consistent algebra: **A and C are the multiplier
inputs, A and B are the adder inputs, A is the predicate**. `MAD = A*C + B`,
`MUL = A*C` (B idle), `ADD = A + B` (C idle), `SUB = A - B`, dot products fold
`A*C` across 3 or 4 channels, and the conditional ops select between B and C on
A. The D3D `mad d,s0,s1,s2` mapping `A←s1, B←s2, C←s0` and `mul d,s0,s1`
mapping `A←s0, C←s1` are the direct evidence.

### 7.5 Opcode field structure  **INFERRED**

The 6-bit values are not arbitrary:

```
bits [11:9] ALU function: 0 MUL, 2 MOV, 3 SUB, 4 MAD, 5 LRP, 6 ADD  (1, 7 unknown)
bit  12     3-component sum   (MUL + bit12 = DP3)
bit  13     4-component sum   (MUL + bit13 = DP4)
bit  14     conditional select (ADD-coded base 0x0c00 + bit14 = CND;
                                + bit12 = CMV; + bit12 + bit13 = CMP)
```

Only the flat values in §7.3 are CONFIRMED; treat the decomposition as a
plausibility aid, not as a licence to synthesise unlisted encodings.

However, the driver itself decodes operand liveness straight out of these bits,
which is strong independent evidence that the field really is structured that
way. `PARD 0x182c60` answers "does this instruction read register code *n*?" by
examining `0x1264`'s three selector fields under exactly this rule
(**CONFIRMED**, @0x182c6a-0x182cdb):

```
source A : always live
source B : live if opcode bit 9 or opcode bit 11 is set
source C : live if opcode bit 10 is clear
all three: if (opcode & 0x7000) >= 0x4000   (the CND/CMV/CMP family)
```

Applied to the eleven bases in §7.3 this reproduces the A/B/C columns of all
three tables exactly.

---

## 8. Result scale and saturate  **CONFIRMED for codes 0-3, INFERRED for 4-5**

`0x126c[19:17]`, fed through table `PARD 0x1d7938 = {0,1,2,3,0,0,...}` indexed
by the D3D destination shift field (`PARD 0x181f8e`, `0x18399e`):

| field | result |
|---|---|
| 0 | ×1 |
| 1 | ×2 (`_x2`) |
| 2 | ×4 (`_x4`) |
| 3 | ×8 (`_x8`) |
| 4, 5 | presumably ÷2, ÷4 — `GL_SCALE_D2_MTX`/`GL_SCALE_D4_MTX` exist **[HDR]** but the D3D driver never emits them (its table clamps everything ≥ 4 to 0) |

`0x126c[24]` = saturate, a single bit from the D3D destination-modifier field
(`setne`, `PARD 0x181fa7`). This matches the GL API split exactly: `GL_SCALE_MTX`
(a value) and `GL_SATURATE_MTX` (a boolean) **[HDR]**, and it is why a program
using both `_x2` and `_sat` needs no extra instruction.

Real evidence for the scale field: the MTXPARD video pipeline at `.text
0xb28fb` emits `0x126c = 0x00420800` (`MAD_x2`) and `0xb2d26` emits
`0x00560800` (`MAD_x8` into `r1`); the identity/pad stage emits `0x00400400`
(scale 0).

---

## 9. Constants, register file and limits

### 9.1 Constants are per-slot, 11-bit fixed point  **CONFIRMED**

Only **two** constant selectors exist, `c0 = 0x0E` and `c1 = 0x0F`. The
compiler allocates them per instruction: `PARD 0x1805d0` walks the instruction's
constant references and assigns `code = index + 0xE`, refusing more than two
(`cmp ecx,2 / jae`).

`c0` lives in `0x1278`/`0x127c`, `c1` in `0x1280`/`0x1284`. Each register packs
**two signed 11-bit values**, at bits [10:0] and [26:16] — four components per
constant (**CONFIRMED**, `HAL BuildColorConvertShader` 0x6dd9a-0x6deb2:
`cvttss2si(coef * 1024.0) & 0x7ff` and `<<16 & 0x7ff0000`; `PARD 0xb2390` is the
same conversion, `round(value * (1 << shift))` called with `shift = 0xa`).

So combiner constants are **Q1.10 fixed point in [-1.0, +0.999]**, not floats.
An open driver must clamp and round host-side. The D3D caps corroborate the
range: `MaxPixelShaderValue = 1.0f` (`PARD 0x86c94`, `D3DCAPS8+0x574`), next to
`PixelShaderVersion = 0xFFFF0103` (ps_1_3) at `+0x570`. **CONFIRMED.**

The API-level constant file (`def c0…`, `SetPixelShaderConstant`) is kept in
driver memory — `PARD 0x180770`: `ctx[+0xc][0] + idx*24 + 0x7674`, with a 32-bit
in-use mask at `obj+0xc0`, i.e. **up to 32 API constants**, folded down to the
two hardware slots per instruction at compile time. There is no constant upload
path to the chip other than `0x1278`-`0x1284`. **CONFIRMED.**

### 9.2 Register file summary  **CONFIRMED**

| class | count | codes |
|---|---|---|
| texture | 4 | 0x00-0x03 |
| temporary | 2 (`r0` = result, `r1`) | 0x04, 0x05 |
| iterated colour | 2 | 0x10 (diffuse), 0x11 (specular) |
| constant, per instruction | 2 | 0x0E, 0x0F |

### 9.3 Instruction limits  **CONFIRMED**

* 10 instruction slots total = 5 combiner stages × {colour, alpha} (§1.1).
* Padded to 5 or 10 on upload.
* The MTXPARD IR compiler enforces the same bound: `cmp DWORD PTR
  [ebp+0x12a0],0xa` at `0x176cff`.
* libGL's colour-phase counter is bounded `<= 9` (`GL 0x1b6126`), i.e. 10
  instructions, agreeing exactly.
* The GL caps the ICD actually returns, read out of libGL's `glGetIntegerv` /
  `glGetFloatv` dispatch (**CONFIRMED**, `GL 0x13b805`-`0x13b860`, `0x13d0ec`,
  `0x13d4bb`, `0x13dac5`, `0x13c6a5`, `0x13d0f9`, and the float mirrors at
  `0x13ed08`-`0x13ed40`):

| token | value |
|---|---|
| `GL_MAX_FP_OPS_MTX` (0x8A85) | **10** (10.0f) |
| `GL_OPT_FP_OPS_MTX` (0x8A88) | **5** (5.0f) |
| `GL_MAX_TA_OPS_MTX` (0x8A82) | 4 |
| `GL_OPT_TA_OPS_MTX` (0x8A83) | 4 |
| `GL_MAX_FP_TEXTURES_MTX` (0x8A86) | 4 |
| `GL_OPT_FP_TEXTURES_MTX` (0x8A89) | 4 |
| `GL_MAX_FP_COLORS_MTX` (0x8A87) | 2 |

  The **10 / 5** pair is exactly the padding rule of §1.1 seen from the API
  side, `MAX_FP_COLORS = 2` is `v0`/`v1`, and `MAX_TA_OPS = 4` is one
  texture-address instruction per texture unit.

### 9.4 Texture-address instruction  **partly CONFIRMED**

`0x1260`/`0x1268` form a second instruction with the identical encoding, run
per slot, and are emitted only when flag bit0 is set. In real programs they
carry ordinary ALU-looking words (e.g. `0x1260 = 0x40`, `0x1268 = 0x00400400`
in the MTXPARD video shader — `MOV r0, t2`).

`GL_MAX_TA_OPS_MTX = 4` (§9.3) says there are only **four** texture-address
instructions, not ten, so the `0x1260`/`0x1268` pair is most likely addressed
per *texture unit* rather than per ALU slot — but the slot field of `0x1260` is
0 in every program found, so this could not be checked. **INFERRED.**

The dependent-read opcodes themselves — the `GL_OP_TEX*` set
(`GL_OP_TEX_NOP/TEX/TEX_COORD`, `TEXKILL_GE/LT`, `TEXBEM`, `TEXDEP_AR/GB/RGB`,
`TEXDOT3_COORD/TEXDOT3`, `TEXM3X2/_DEPTH`, `TEXM3X3/_COORD/_REFLECT`,
0x8A8E-0x8A9D **[HDR]**) — could **not** be mapped to concrete `0x1268` values:
in the D3D driver every t-op (D3DSIO 0x40-0x57) falls through to the default
`0x400` row, so MTXPARD's ps.1.3 path evidently lowers texture reads to plain
texture-unit enables rather than to texture-address instructions. The bump-env
matrix has its own token, `GL_TEXBEM_MATRIX_MTX` (0x8A9E). **Not pinned — see
§13.**

---

## 10. How a program reaches the chip  **CONFIRMED**

The 3D command stream is a sequence of variable-length packets:

```
header dword = four register indices, one per byte, little-endian
               (register = 0x1000 + index*4)
               index 0x21 (register 0x1084) is the pad/terminator:
               it and everything after it in the header consume no data
then one data dword per non-pad index
```

Independently corroborated by the libGL viewport emitter documented in
`parhelia-3d-state-complete.md` §2 (header `0x2121a1a0` = two real indices
`0xa0`/`0xa1` followed by two `0x21` pads, carrying two data dwords).

**CONFIRMED** by the hand-built shader streams in MTXPARD `.text` 0xb2799 ff.,
where the packet boundaries only line up under this rule (e.g. header
`0x21219a98` at +0x3c is followed by exactly two data dwords before the next
header at +0x48). Frequently used register indices: `0x98`→0x1260,
`0x99`→0x1264, `0x9a`→0x1268, `0x9b`→0x126c, `0x9e`→0x1278, `0x9f`→0x127c,
`0xa0`→0x1280, `0xa1`→0x1284, `0x77`→0x11dc, `0x79`→0x11e4, `0x80`→0x1200.

### 10.1 A complete real program

MTXPARD `.text` 0xb2799 (the video/procamp path), literal dwords straight out of
the binary, three colour-pipe stages at slots 0, 2, 4:

```
0x1264 = 0x0703c000   0x126c = 0x00400800   c0, c1 uploaded   ; slot 0
0x1264 = 0x0203c002   0x126c = 0x00400800   c1 uploaded       ; slot 2
0x1264 = 0x0203c004   0x126c = 0x00420800   c1 uploaded       ; slot 4
```

decoding (`tools/parhelia-fragdis.py`) to

```
slot 0:  MAD    r0, t0, c1, c0     ; r0 = t0*c0 + c1
slot 2:  MAD    r0, t0, c1, r0     ; r0 = t0*r0 + c1
slot 4:  MAD_x2 r0, t0, c1, r0     ; r0 = (t0*r0 + c1) * 2
```

and MTXPARD `.text` 0xb264f, a single-stage brightness offset:

```
0x1264 = 0x00038000   0x126c = 0x00400c00   ->  ADD r0, t0, c0
```

Both were decoded blind by the tool and match the shape of the source code that
built them, which is the practical validation of §2, §3, §7 and §8.

---

## 11. `tools/parhelia-fragdis.py`

GPL-2, `Copyright (c) 2026 Rene Rebe <rene@exactco.de>`.

```
parhelia-fragdis.py --selftest                      # decode the programs harvested above
parhelia-fragdis.py --words 0x1264=0x0703c000,0x126c=0x00400803
parhelia-fragdis.py --stream cmdbuf.bin             # walk a DMA packet stream
```

`--stream` parses the §10 packet framing, filters out everything that is not a
combiner register, splits the writes into slots and prints each stage with its
symbolic instruction, its algebraic expansion, its decoded Q1.10 constants and
the raw register values. It is deliberately conservative: unknown modifier,
replicate, scale or opcode fields are printed as `{modX}` / `{repX}` /
`_scaleX` / `OP_xxxx` rather than guessed.

---

## 12. Corrections to the previous revision

The 2026-09 version of this file and §1 of `parhelia-3d-state-complete.md` had
the right registers but several wrong fields. Superseded claims:

1. **"`0x1264` = slot + two 5-bit selectors + a `0x07038000` control base."**
   Wrong. There are **three** source operands, each 5-bit register + 4-bit
   modifier, filling bits [31:5]. `0x07038000` is not a control base — it is
   simply `srcB = 0x0F (c1)`, `srcC = 0x0E (c0)` plus a source-A modifier,
   i.e. the operand pattern of the colour-conversion shader.
2. **"`0x126c[23:20]` = output register, low bits = opcode base"** — right as
   far as it went, but the word also carries three replicate fields, a result
   scale and a saturate bit, and the opcode occupies [14:9] specifically.
3. **"`0x1278`-`0x1284` are colour-matrix coefficients."** They are the two
   general **constant registers** `c0` and `c1`, four Q1.10 components each.
   The colour-matrix use is one application of them.
4. **"`0x126c` bits [19:17] are a destination component / write mask."**
   Wrong — that field is the **result scale** (`_x2/_x4/_x8`). The colour /
   alpha split is carried by the slot address, not by a write mask.
5. **"Two writable registers, ~10 arithmetic ops, phase-gated."** The two
   writable registers is right; the "~10 ops" is exactly 10 and is structured
   as 5 stages × 2 pipes; the "two phases" are really two instruction words
   per slot (`0x1260`/`0x1268` and `0x1264`/`0x126c`), not two passes.
6. **"The final bit-packing is done by the kernel/DRM layer."** Wrong for the
   Windows stack and, on the evidence, wrong in general: MTXPARD packs the
   words in user space, `mtx_drv` packs them in the X server, and the two agree
   bit for bit. libGL simply keeps a shadow register file and its packer was
   not located; there is no reason to believe a different layout exists.
7. **"`0x126c` base `0x803` = colour-matrix multiply."** It is `MAD` (`0x800`)
   with source-A replicate = red (`0x3`).
8. **CMV's base is `0x5c00`**, no longer a capture item.

---

## 13. What is *not* pinned down

Stated explicitly rather than guessed:

* **Texture-address opcodes.** No concrete `0x1268` opcode value could be tied
  to any `GL_OP_TEX*` / D3D t-op. The shipped D3D driver routes all of them to
  the fallback `MOV` row. Recovering these needs either the libGL texture-address
  path (`GL 0x1b47c0` writes 36-byte descriptors at `shader+0x6a0`, which are
  lowered somewhere downstream) or a hardware capture of a bump-mapping draw.
* **`0x126c` bits 15, 16 and 25-31.** No writer found in either binary.
* **Replicate codes 5-7, modifier codes 6-0xE, opcode-field values with
  bits [11:9] = 1 or 7.** Unused by both drivers.
* **Why hardware slots 6 and 7 are skipped.** The emitter jumps over them
  unconditionally.
* **Result scale codes 4 and 5** (÷2, ÷4) are inferred from `GL_SCALE_D2/D4_MTX`
  only.
* **Fog and specular combine** — where the fixed-function fog/specular add sits
  relative to the five stages was not investigated.
* **libGL's per-stage register template** (`.rodata` 0x39fc1c, **CONFIRMED**
  re-dumped) is `1374 1084 1090 1094 10b0 1104 1264 126c 1278 127c 1280 1284` —
  twelve `localReg` slots, and **`0x1260`/`0x1268` are not among them**. The
  Linux ICD therefore never programs the texture-address instruction word
  through this path, which is a second reason those opcodes stayed out of reach.
  (libGL genuinely does not contain the `0x1264`/`0x126c` immediates; it emits
  `{u16 stage, u16 localReg, u32 value}` triples and resolves them through this
  table.)
* **Whether `0x1260`/`0x1268` is per-slot or per-stage.** Both are written in
  the same packet as `0x1264`/`0x126c`, but no program in the binaries uses a
  non-trivial texture-address instruction, so the addressing could not be
  exercised.

## 14. Confidence statement

The **arithmetic instruction encoding is solid**: every field of `0x1264` and
`0x126c` except bits 15-16 and 25-31 of the latter was read out of at least one
binary, and the important ones (source packing, destination, opcode) were read
out of *four* independent packers in *two* binaries that agree bit for bit, then
validated by decoding literal shader words that those same binaries embed.

The **opcode table is complete and authoritative** for the 11 ALU operations.

The **texture-address / dependent-read half of the ISA is not recovered.** The
register pair and its layout are known; the opcodes are not.

The **5-stage × 2-pipe organisation is an inference**, though the only one
consistent with the slot map, the 5/10 padding rule and the GL API's separate
`glFragProcRGBAOp*` / `glFragProcAlphaOp*` entry points. It should be the first
thing a hardware bring-up test confirms: emit `MOV r0, v0` in slot 0 and
`MOV r0, c0` in slot 1 and see whether the alpha channel follows the second.
