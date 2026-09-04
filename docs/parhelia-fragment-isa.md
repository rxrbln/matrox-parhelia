# Parhelia-512 fragment / pixel-shader ISA

Reverse-engineering of the Parhelia **pixel (fragment) combiner** — the block the
Windows D3D driver targets when it compiles ps.1.x, the block the Linux GL ICD
exposes as `GL_MTX_fragment_shader`, and the block the X driver's
`CDPSPixelShader` class uses for video colour conversion / procamp.
Counterpart of `parhelia-vertex-isa.md` (WARP microcode, vertex side — out of
scope here).

**Revision 2026-09-05: this file was substantially rewritten.** The 2026-09
version had the register set right but several field assignments wrong; the
corrections are listed in §14. Do not carry the old bit tables forward.

Sources actually read for this revision:

| tag | binary | notes |
|---|---|---|
| `[PARD]` | `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | PE i386 Windows D3D driver, advertises `D3DPS_VERSION(1,3)` = `0xFFFF0103` (`.text` 0x86c90). Disassembly: `scratchpad/pard.asm`, `objdump -d -M intel`. Section map: `.text` VMA 0x10300 / file 0x300, `.rdata` VMA 0x19b700 / file 0x18b700, `.data` VMA 0x19fb00 / file 0x18fb00. |
| `[HAL]` | `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | x86-64, exported `CDPSPixelShader::*` symbols. Disassembly: `scratchpad/mtx_drv.asm`. |
| `[GL]` | `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` | stripped ICD. `scratchpad/libGL64.asm`. |
| `[KRN]` | `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | x86-64 relocatable, **not stripped** (`mtxNNNN` symbols). The DMA emitter `mtx530` @0x691f0 and the two instruction packers `mtx1653` @0x6b170 / `mtx2619` @0x6a200. Disassembly: `scratchpad/mtxk.asm`. |
| `[HDR]` | `opengl6/include/glext.h` | ships the whole `GL_MTX_fragment_shader` token block. |

Every claim below is tagged **CONFIRMED** (read out of a binary, and where
possible corroborated by a second, independent binary) or **INFERRED**.
Things that could not be pinned down are listed as such in §15 rather than
guessed.

Decoder: `tools/parhelia-fragdis.py` (§13).

---

## 1. Machine model

**CONFIRMED.** The combiner is a fixed list of instruction slots, each slot
programmed through a small window of engine registers. Engine register =
`0x1000 + index*4`, reached as (register-index, value) dword pairs in the DMA
command stream (see §10).

Per slot the driver writes at most eight registers:

| register | role |
|---|---|
| `0x1264` | **RGB** instruction — source/control word, **and carries the slot address** |
| `0x126c` | **RGB** instruction — destination/opcode word |
| `0x1260` | **alpha** instruction — source/control word |
| `0x1268` | **alpha** instruction — destination/opcode word |
| `0x1278`, `0x127c` | stage constant **c0** (4 × signed 11-bit) |
| `0x1280`, `0x1284` | stage constant **c1** (4 × signed 11-bit) |

**`0x1260`/`0x1268` are the alpha half of the same instruction, with the same
bit layout as `0x1264`/`0x126c`** (**CONFIRMED**). Two independent proofs:

* The kernel DMA emitter `KRN mtx530` @0x691f0 tests the slot's flag bit0 and
  either emits the header `0x9a989b99` = `{0x1264, 0x126c, 0x1260, 0x1268}`
  (separate alpha instruction) or the header `0x20209b99` =
  `{0x1264, 0x126c, DMAPAD, DMAPAD}` (@0x69250 / @0x6933e) — the *same four*
  record dwords are written either way; when bit0 is clear the alpha pair is
  simply discarded into the pad register.
* `PARD 0x170a10` builds the same shadow and, when bit0 is clear, fills the
  alpha slot with the **RGB pair masked by `& ~0x124`** (`and eax,0xfffffedb`
  @0x170abc) — i.e. the RGB instruction with the three *alpha-replicate* bits
  cleared. That is exactly the right default for an alpha pipe, and makes no
  sense for anything else.

The kernel has two instruction packers with identical structure, `mtx1653`
@0x6b170 and `mtx2619` @0x6a200, one per half.

### 1.1 Slot addressing

`0x1264[4:0]` is the slot address; it is merged in at upload time, not at
compile time (**CONFIRMED**: `HAL CDPSPixelShader::UpdateHardware` @0x6d896
`edx &= ~0x1f; edx |= slot`; `PARD 0x1707b0` @0x1707b3 does the identical
XOR-merge; `KRN mtx530` @0x6930c does the same; all six packers mask `0x1264`
with `0xffffc01f` and never touch bits [4:0]).

The driver keeps a flat array of **10 instruction slots**, 36 bytes each
(**CONFIRMED**: `PARD` ctor 0x170860 fills `edi = 0xa` entries at stride 0x24;
`HAL BuildShader` 0x6e620 pads the same 10-entry array; libGL's uploaded blob
has 10 records of 0x94 bytes, §12.2.2). The emitter walks that array and writes
each entry to a hardware slot in this order (**CONFIRMED** in all three, e.g.
`PARD 0x170980` and `KRN 0x692df`-`0x69313`, with the `esi==6||esi==7 → +=2`
and `esi==12 → 1` fixups):

```
instruction index  0  1  2  3  4   5  6  7  8  9
hardware slot      0  2  4  8 10   1  3  5  9 11
```

Twelve slot addresses exist; **6 and 7 are never used**, and the walk visits the
even addresses first and the odd ones second. Why 6/7 are skipped, and why the
order is even-then-odd rather than sequential, is **not known** — a plausible
reading is five hardware shader units addressed in two passes. It is *not* the
colour/alpha split; that is carried by the register pair (§1.1a).

The instruction count is padded to **5 or 10** before upload — never anything
else (**CONFIRMED**, `PARD 0x1708d0`: `count>10 → 10`, else `count<=5 → 5` else
`10`; `HAL 0x6e667`: `>10 → 10`, `<6 → 5`, else `10`; `KRN 0x692bd` the same).
More than 5 instructions additionally sets pipeline bit `0x80000` (`KRN
0x2576b`). Pad slots are filled with `0x1264 = 0x00000080`,
`0x126c = 0x00400400`, i.e. `MOV r0, r0` (**CONFIRMED**, all three binaries
write those two literals; `KRN mtx2628` @0x69080).

The GL caps agree exactly: `GL_MAX_FP_OPS_MTX = 10`, `GL_OPT_FP_OPS_MTX = 5`
(§9.3).

### 1.1a Colour / alpha pairing and co-issue

**RGB and alpha are genuinely co-issued inside one slot.** A slot holds two
complete instructions: the RGB one in `0x1264`/`0x126c` and the alpha one in
`0x1260`/`0x1268`. There is no write mask anywhere in the encoding — the pipe
is the register pair you write.

* If a program has no separate alpha instruction for a slot, the driver clears
  flag bit0 and the alpha pair is not emitted at all (`KRN mtx530` @0x6933e).
  `PARD 0x170a10` shows the implied default: **the RGB instruction with the
  alpha-replicate bits cleared**.
* The alpha packer writes only the low **two** bits of each replicate field,
  forcing bits 2/5/8 to zero (`KRN 0x6a3ac`, `0x6a493`, `0x6a582`) — alpha
  replicate is meaningless in the alpha pipe. Everything else (opcode, sources,
  modifiers, scale, saturate, destination) is encoded identically.
* libGL matches: one 0x94-byte instruction record holds both halves, the RGB
  op's decoded opcode at `record+0xe0` and the alpha op's at `record+0x118`
  (`GL 0x1b67cf`, `0x1b67d6`, `0x1b688e`), with a separate-alpha flag at
  `record+0x34` (`KRN mtx2132` @0x6e3d4 `cmp BYTE PTR [r13+0x34],1; cmove` —
  when clear, the RGB record is handed to *both* packers).
* The D3D `D3DSI_COISSUE` flag (`0x40000000`) is never tested in MTXPARD's
  shader compiler (`0x17xxxx`-`0x18xxxx`); MTXPARD reaches the same structure
  through its IR instead. **CONFIRMED** by scanning every occurrence of that
  immediate.

One pairing constraint is visible on the GL side: for `DP3`/`DP4`
(`GL 0x1b67bb`) the alpha op is merged into the current record only if its
opcode **equals** the RGB op's (`cmp edx,[record+0xe0]`, `GL 0x1b67d6`) — dot
products write all four channels, so that is the expected restriction. The
general rule for the other opcodes was not traced.

This also resolves the arithmetic that looked wrong before: 10 slots × 2 pipes,
one shared instruction counter, `GL_MAX_FP_OPS_MTX = 10` — all consistent.
The even/odd slot addressing of §1.1 is **not** the colour/alpha split; it is
the driver walking 12 slot addresses in two passes.

### 1.2 Per-slot flag byte

The 36-byte software slot record is `{+0x00 flags, +0x04 0x1264, +0x08 0x126c,
+0x0c 0x1260, +0x10 0x1268, +0x14..+0x20 constants}` in `PARD`; in `HAL` the
same record sits at `this + 0xc + i*36` with fields shifted by 0xc. The flag
byte selects which optional registers are emitted (**CONFIRMED**,
`PARD 0x1707b0` / `HAL CDPSPixelShader::UpdateHardware` 0x6d830):

| flag bit | emits |
|---|---|
| bit0 | `0x1260`, `0x1268` — a **separate alpha instruction** is present |
| bit1 | `0x1278`, `0x127c` (constant c0 used) — **INFERRED** |
| bit2 | `0x1280`, `0x1284` (constant c1 used) — **INFERRED** |
| always | `0x1264`, `0x126c` |

`KRN mtx530` tests bit0 at @0x6931f and bits[2:1] at @0x69272, matching
`PARD 0x1707b0` and `HAL` exactly.

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
| 7 | `LRP` | s0 | s1 | s2 | `0x0000_0a00` | `A*C + B*(1-C)` |
| 8 | `CND` | s0 | s1 | s2 | `0x0000_4c00` | `A > 0.5 ? B : C` |
| 9 | `CMV` | s0 | s1 | s2 | `0x0000_5c00` | conditional move |
| 10 | `CMP` | s0 | s1 | s2 | `0x0000_7c00` | `A >= 0 ? B : C` |

`CMV = 0x5c00` closes the last open item of the previous revision (it was
`[CAP]` there); it is absent from the D3D tables because ps.1.x has no such
token, but the X driver emits it (`HAL 0x6e083` `or r12d,0x5c00`).

The **kernel HAL carries a third, byte-for-byte identical copy** — the exported
object `mtx1291` at `.data + 0x1260`, 264 bytes = 11 entries of 24, read at
`KRN 0x6a238`-`0x6a25d` (relocations `mtx1291+0`, `+0x10`, `+0x14` pick out the
same three fields). Re-dumped and diffed against `PARD 0x1d77f0`: identical.
Three independent tables in three independent binaries agreeing on both the
bases and the slot permutation is why this table is tagged CONFIRMED rather than
probable.

### 7.4 Hardware operand roles  **INFERRED (from the slot-assignment tables)**

The three tables agree on a consistent algebra: **A and C are the multiplier
inputs, A and B are the adder inputs, A is the predicate**. `MAD = A*C + B`,
`MUL = A*C` (B idle), `ADD = A + B` (C idle), `SUB = A - B`,
**`LRP = A*C + B*(1-C)`** (MAD with an implicit complement on B), dot products
fold `A*C` across 3 or 4 channels, and the conditional ops select between B and
C on A. The D3D `mad d,s0,s1,s2` mapping `A←s1, B←s2, C←s0` and `mul d,s0,s1`
mapping `A←s0, C←s1` are the direct evidence; the LRP form is independently
pinned three ways by the GL fixed-function lowering (§12.2.5), which uses it for
`GL_BLEND`, `GL_DECAL` and `GL_COMBINE INTERPOLATE` and matches the GL spec in
all three cases.

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

## 8. Result scale and saturate  **CONFIRMED**

`0x126c[19:17]` is a **signed 3-bit shift count** applied to the result:

| field | result | GL token |
|---|---|---|
| 0 | ×1 | `GL_NONE_MTX` (0x8ACB) |
| 1 | ×2 | `GL_SCALE_X2_MTX` (0x8ACC) |
| 2 | ×4 | `GL_SCALE_X4_MTX` (0x8ACD) |
| 3 | ×8 | `GL_SCALE_X8_MTX` (0x8ACE) |
| 7 | ÷2 | `GL_SCALE_D2_MTX` (0x8ACF) |
| 6 | ÷4 | `GL_SCALE_D4_MTX` (0x8AD0) |

Read out of libGL's `GL_SCALE_MTX` mapper `GL 0x1b3bb0` and its `.rodata` jump
table at `0x36ec20` (six entries, base enum 0x8ACB, resolving to `0x1b3c05`→0,
`0x1b3c12`→1, `0x1b3c20`→2, `0x1b3c30`→3, `0x1b3c50`→7, `0x1b3c40`→6). The
`{6, 7}` pair for `÷4`/`÷2` is what makes it a two's-complement shift count.

Corroborated from three more places:
* the D3D packer feeds the field from the D3D destination-shift token through
  `PARD 0x1d7938 = {0,1,2,3,0,...}` (`PARD 0x181f8e`, `0x18399e`), so ps.1.x
  `_x2/_x4/_x8` reach it and `_d2/_d4` are dropped;
* the kernel packer copies the IR's scale byte verbatim —
  `KRN 0x6a2a4`: `movzx edx,[r8+0x8]; and edx,7; add edx,edx` merged into
  byte `+0xe` of the word, i.e. `<<17` (**CONFIRMED**);
* the X driver's colour-convert shader needs ×4 to undo its ×1024 coefficients
  and emits exactly that — `HAL 0x6dfce`: `or ebx,0x40001; and ebx,0xff05fffd;
  ... or ebx,r12d` leaves bit18 set, bits 17/19 clear ⇒ field = 2 = ×4 (and
  bit0 set ⇒ source-A replicate = blue).

`0x126c[24]` = saturate, one bit. In the D3D path it comes from the destination
modifier (`setne`, `PARD 0x181fa7`); in the kernel path from the IR's saturate
byte (`KRN 0x6a2b6`-`0x6a2c7`). libGL's fixed-function generator initialises it
to **1 and never clears it**, so the fixed-function pipeline always saturates.
This matches the GL API split: `GL_SCALE_MTX` (a value) and `GL_SATURATE_MTX`
(a boolean) **[HDR]**.

Real evidence: `PARD 0xb28fb` emits `0x126c = 0x00420800` (`MAD_x2`) and
`PARD 0xb2d26` emits `0x00560800` (`MAD_x8` into `r1`); the identity pad emits
`0x00400400` (scale 0).

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

* 10 usable instruction slots out of 12 addresses (§1.1).
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

### 9.4 The alpha instruction and the texture-address stages

`0x1260`/`0x1268` are the **alpha** half of the slot (§1.1a), not a
texture-address instruction. That corrects the previous revision.

The texture-address stages are a **separate, 4-entry array**, not part of the
instruction list. libGL's uploaded program blob is 0x664 bytes and is laid out
(**CONFIRMED**, `GL` ctor 0x10d1c6-0x10d211, memcpy length at `0x1137d9`):

```
+0x000  u32   instruction count
+0x004  10 x 0x94  instruction records                      (ends 0x5cc)
+0x5cc  u8    0
+0x5d0  u32   4  = number of texture-address stages
+0x5d4  4 x 0x24   texture-address stage descriptors        (ends 0x664)
```

Four descriptors — matching `GL_MAX_TA_OPS_MTX = 4` (§9.3). The fixed-function
generator only ever sets `descriptor = 1` for each texture unit an instruction
actually reads (`GL 0x10da50`, driven from `0x10df65`-`0x10e161`; the alpha half
is scanned only when `record+0x34 != 0`), i.e. a plain "sample this unit".

**Which engine register a texture-address descriptor is written to, and the
encoding of the `GL_OP_TEX*` opcodes** (`GL_OP_TEX_NOP/TEX/TEX_COORD`,
`TEXKILL_GE/LT`, `TEXBEM`, `TEXDEP_AR/GB/RGB`, `TEXDOT3_COORD/TEXDOT3`,
`TEXM3X2/_DEPTH`, `TEXM3X3/_COORD/_REFLECT`, 0x8A8E-0x8A9D **[HDR]**, plus
`GL_TEXBEM_MATRIX_MTX` 0x8A9E) **could not be determined**. No path in any of
the four binaries emits a non-trivial one: MTXPARD's ps.1.3 table routes every
D3DSIO t-op (0x40-0x57) to the fallback `MOV` row, and libGL's fixed-function
path only ever writes the value 1. See §15.

---

## 10. How a program reaches the chip  **CONFIRMED**

The 3D command stream is a sequence of variable-length packets:

```
header dword = four register indices, one per byte, consumed LSB first
               (register = 0x1000 + index*4)
then one data dword per index, up to four
index 0x20 (register 0x1080) = DMAPAD  : consumes a data dword and discards it
index 0x21 (register 0x1084) = TERMINATE: ends the packet; the next dword in
                                          the stream is a new header
```

The authoritative spec is the kernel's own software interpreter of the stream,
`KRN mtx2434` @0x398e0 — see `docs/parhelia-kernel-abi.md` §6.2, which decoded
it exactly. Both idioms occur in practice: the kernel emitter always writes a
full five-dword packet and pads unused slots with `0x20` (so the extra dwords
are consumed and thrown away), while MTXPARD emits short packets terminated by
`0x21`.

Corroborated by the hand-built shader streams in MTXPARD `.text` 0xb2799 ff.,
whose packet boundaries only line up under the terminate rule (header
`0x21219a98` at +0x3c is followed by exactly two data dwords before the next
header at +0x48; the block at 0xb2638 is 4 + 4 + 5 = 13 dwords, matching its
`add eax,0xd`), and by the kernel combiner emitter `KRN mtx530` @0x691f0, which
writes four data dwords after the header `0x20209b99` and lets the two DMAPADs
swallow the unused alpha pair. Frequently used register indices: `0x98`→0x1260,
`0x99`→0x1264, `0x9a`→0x1268, `0x9b`→0x126c, `0x9e`→0x1278, `0x9f`→0x127c,
`0xa0`→0x1280, `0xa1`→0x1284, `0x77`→0x11dc, `0x79`→0x11e4, `0x80`→0x1200.

### 10.1 A complete real program

MTXPARD `.text` 0xb2799 (the video/procamp path), literal dwords straight out of
the binary, three RGB instructions in slots 0, 2, 4:

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

All three are RGB instructions; the packets carry no
`0x1260`/`0x1268`, so the alpha pipe runs the same instruction with
alpha-replicate disabled (§1.1a).

and MTXPARD `.text` 0xb264f, a single-stage brightness offset:

```
0x1264 = 0x00038000   0x126c = 0x00400c00   ->  ADD r0, t0, c0
```

Both were decoded blind by the tool and match the shape of the source code that
built them, which is the practical validation of §2, §3, §7 and §8.

---

## 11. Texture sampler state

The combiner reads four texture units; this is the state that feeds them. All
addresses are MTXPARD VMA. All of it is **CONFIRMED** unless marked otherwise.

### 11.1 Emission order and the unit latch

`0x16fe50` is the per-unit emitter, driven from the texture-engine
`UpdateHardware` at `0x170450` (which loops `unit = 0 .. count-1`, then writes
`0x11e8` and the guardband `0x12f4`). The per-unit software stage struct is
0x48 bytes at `engine + 8 + unit*0x48` (`0x16fce0`). Register writes go out
through the same `(register, value)` appender used by the combiner
(`0x170e10`, capped at 0x12b pairs), and the **register offsets are pushed in
full** (`push 0x11dc`), not as indices.

| struct off | register | role |
|---|---|---|
| +0x00 | **`0x1200 + unit*4`** | unit control (§11.2) |
| +0x04 | `0x11c0` | address / wrap modes (§11.3) |
| +0x0c | `0x11c4` | filter word (§11.4) |
| +0x10 | `0x11c8` | always 0 |
| +0x14 | `0x11cc` | border colour, in the texture's own format |
| +0x18 | `0x11d0` | border-colour mask |
| +0x1c, +0x20 | `0x11d4`, `0x11d8` | always 0 |
| +0x24 | `0x11dc` | texture base address, VRAM byte offset |
| +0x28 | `0x11e0` | pitch + format + tiling (§11.5) |
| +0x2c | `0x11e4` | height |

**Only `0x1200` is per-unit addressed.** `0x11c0`-`0x11e4` carry no unit index,
and the emitter writes `0x1200 + unit*4` **first**, then the whole
`0x11c0`-`0x11e4` block, once per unit. The `0x1200+unit*4` write therefore
looks like the unit selector that latches the following window. **INFERRED**,
but a driver must reproduce the order regardless.

If a unit is disabled (`0x1200[18:16] == 1`) the emitter writes only
`0x1200 + unit*4` and stops (`0x16fe5d`, `cmp ecx,0x10000`).

### 11.2 `0x1200 + unit*4` — unit control

| bits | meaning |
|---|---|
| [3:0] | one-hot texture-pipe select (1, 2, 4, 8) — `0x1052e7` `1 << tbl0x1d3ac0[cnt*4+unit]`; idle defaults `0x00010001/2/4/8` for units 0-3 at `0x15a2d5` |
| [11:8] | constant `0x1` on every path (`or ...,0x40100`) |
| [18:16] | texture type / enable: `1` = disabled, `4`-`7` = enabled; bit16 is a projective ("divide by q") sub-flag that an errata path rewrites 5→4 and 7→6 (`0x2c900`) |
| [23:19] | 5-bit filter code in the DDraw-side texture-object representation (`0xa2915`: `obj[+4] << 0x13`, mask `0xf80000`; `0xa293e` forces `0x480000` = 9). Codes 2, 3, 9, 0xa, 0xe trigger a hardware-errata workaround (`0x2cea0`) |
| [24], [28:27] | writable (mask `0xe404f1f1`), purpose unknown |

Known-good values: active `0x00040101`, idle `0x00010000`.

Related: `engine[+4] = tbl0x1d3b10[count] = {0, 0x1000, 0x5000, 0xb000,
0xf000}` — the one-hot pipe-enable mask shifted left 12.

### 11.3 `0x11c0` — address / wrap modes

```
0x11c0 = 0x1000                 ; bit12 always set
       | (Uaddr & 3) << 0       ; bits [1:0]
       | (Vaddr & 3) << 3       ; bits [4:3]
       | (Waddr & 3) << 6       ; bits [7:6]
```
(`0x1052f0`-`0x105339`, write mask `0xffffff24` at `0x15a386`.) Each axis is a
2-bit mode; the filler drives U and V from one internal 3-valued mode:
0 → `0` (default, repeat), 1 → `0b10`, 2 → `0b01`. **Which of 1 and 2 is clamp
and which is mirror is not determined.**

### 11.4 `0x11c4` — filter word

Write mask `0xfffffe49`; every path found writes the constant `0x49`:

```
[2:0]   filter field A   } all three = 1 in every program found
[5:3]   filter field B   }
[8:6]   filter field C   }
[10:9]  border-colour enable (cleared when a border colour is programmed, 0x105378)
```

**Which field is min / mag / mip, and the filter enum values, are NOT
determined** — both stage fillers (`0x1051b0`, `0x15a2d0`) emit the literal
`0x49`. This **corrects `docs/parhelia-gl-driver.md`**, which claimed
`0x11e8 = min[7:0]/mag[15:8]/mip[23:16]`; `0x11e8` is not a filter register
(§11.6).

### 11.5 `0x11dc` / `0x11e0` / `0x11e4` — surface descriptor

Consumed by helpers as a `{base, ctrl, height}` triple (`0x111150`, `0xa3b40`).

| reg | bits | meaning |
|---|---|---|
| `0x11dc` | [31:0] | base, VRAM byte offset (from `texObj[+0x12c]`, `0x1051e4`) |
| | [1:0] | flag bits — `0xa3f18` tests `& 3` on this dword in a block-compressed path. **INFERRED** |
| `0x11e0` | [13:0] | pitch/width in **pixels**, aligned to 32 (`0xa3b10` returns 0x20) |
| | [18:16] | tiling / memory-layout code, values 0-5 (`0xa3f27`, `& 0x70000`) |
| | [29:24] | **format code**, 6 bits (`0xa3b40`, `& 0x3f000000`) |
| | [31:30] | unknown |
| `0x11e4` | [13:0] | height in pixels |
| | [16:14], bit16 forced set | extra field (`0x10eb47` `or eax,0x10000`, mask `0xf001c000`) |
| | [31:28] | unknown |

Texel address: `base + y * align(width,32) * bpp/8 + x * bpp/8`.

### 11.6 `0x11e8` — texture-cache partition, not a filter register

Table `0x1d43f0` indexed `[activeStageCount*4 + unit]` (`0x1704bf`):

```
count 0 : 0, 0, 0, 0
count 1 : 0x00500000 x4
count 2 : 0x00500000 x4
count 3 : 0x01600000, 0x01600000, 0x00400000, 0x00400000
count 4 : 0x01600000 x4
```

Guardband `0x12f4` is set alongside: `0x41f80000` (31.0) for more than two
stages, `0x42000000` (32.0) otherwise — agreeing with the existing notes in
`parhelia-3d-state-complete.md` §3.

### 11.7 Format encodings

**D3DFORMAT → internal format id** (`0x83af0`, jump table `0x83c48`, byte index
`0x83cac`, 62 entries indexed `fmt-20`):

| D3DFMT | id | | D3DFMT | id |
|---|---|---|---|---|
| A8R8G8B8 (21) | 0x0b | | A8 (28) | 0x1f |
| X8R8G8B8 (22) / R8G8B8 (20) | 0x0c | | A2B10G10R10 (31) | 0x0d |
| R5G6B5 (23) | 0x0a | | A8B8G8R8 (32) | 0x0e |
| X1R5G5B5 (24) | 0x09 | | A8P8 (40) / P8 (41) | 0x10 |
| A1R5G5B5 (25) | 0x08 | | L8 (50) | 0x1d |
| A4R4G4B4 (26) | 0x06 | | A8L8 (51) | 0x20 |
| X4R4G4B4 (30) | 0x07 | | V8U8 (60) | 0x21 |
| L6V5U5 (61) | 0x22 | | X8L8V8U8 (62) | 0x23 |
| V16U16 (64) | 0x25 | | L16 (81) | 0x21 |
| D16 (80) | 0x11 | | D15S1 (73) | 0x12 |
| D24S8 (75) | 0x13 | | D24X8 (77) | 0x14 |
| D24X4S4 (79) | 0x15 | | | |

Rejected as unsupported: R3G3B2, A8R3G3B2, X8B8G8R8, G16R16, A2R10G10B10,
A16B16G16R16, A4L4, Q8W8V8U8, D16_LOCKABLE, D32.

**FOURCC → internal id** (`0x83f80`): DXT1 = 1, DXT2 = 2, DXT3 = 3, DXT4 = 4,
DXT5 = 5, YV12 = 0x27, NV12 = 0x28, UYVY = 0x29, YUY2 = 0x2a, MTXY = 0x2b,
MTX0 = 0x2c, ICD0 = 0x2d.

**Hardware format code (`0x11e0[29:24]`) → bits per texel** (`0xa3b40`):

| code | bpp |
|---|---|
| 0x00, 0x02, 0x04, 0x05 | 1 |
| 0x08 | 4 |
| 0x09-0x0c, 0x10-0x12 | 8 |
| 0x20-0x22, 0x24-0x28, 0x2a-0x2e | 16 |
| 0x30, 0x32-0x34, 0x36, 0x38, 0x3a, 0x3b | 32 |
| anything else | invalid |

The high nibble is the size class (0x1x = 8bpp, 0x2x = 16bpp, 0x3x = 32bpp),
corroborated by `0x8c778` which ORs `0x10000000/0x20000000/0x30000000` for
1/2/≥3-byte elements, and by `0x10eb71` (`or ...,0x32000000`) for a 32-bpp ARGB
surface.

**The internal-format-id → 6-bit hardware code table was not found.** Both
halves of the mapping are confirmed; the join is not. Note also that
`parhelia-gl-driver.md`'s "DXT = 0x2e" is suspect: `0xa3b40` classifies 0x2e as
16 bpp, not 4.

### 11.8 D3D sampler state, driver side

`SetTextureStageState` is `0xaee40` with a 28-entry jump table at `0xaf46c`;
the per-stage D3D block is 0x9c bytes at `renderer+0x54 + 0xedc + stage*0x9c`
(`0x13ec33`, stage ≤ 12), dirty bits in `block[+0]`. Everything sampler-ish is
packed into `block[+0x10]`:

| bits | D3DTSS | dirty bit |
|---|---|---|
| [2:0] | TEXCOORDINDEX | 0x4000 |
| [4:3] | TEXCOORDINDEX TCI flags | 0x80000 |
| [7:5] / [10:8] / [13:11] | ADDRESSU / V / W | 0x8000 |
| [16:14] / [19:17] / [22:20] | MAGFILTER / MINFILTER / MIPFILTER | 0x20000 |
| [24:23] | MAXANISOTROPY (2 bits) | 0x20000 |
| [27:25], ≥28 | TEXTURETRANSFORMFLAGS, PROJECTED | 0x80000 |

`BORDERCOLOR` → `[+0x14]`, `MIPMAPLODBIAS` → `[+0x18]` (float),
`MAXMIPLEVEL` → `[+0x1c]`, the colour/alpha op and argument set →
`[+0x2c]`-`[+0x4c]`, BUMPENVMAT → `[+0x60]`ff.

On the older DDI (`device[+0x48] < 4`) the filter enums are normalised to DX8
`D3DTEXF_*` numbering through `0x1d1f8c` = `{0,1,2,4,5,3}` (mag),
`0x1d1fa4` = `{0,1,2,3}` (min), `0x1d1fb4` = `{0,0,1,2}` (mip).

**The step that turns these 3-bit D3D fields into the hardware bits of §11.3 /
§11.4 was not located.**

### 11.9 Mip selection is done on the CPU  **INFERRED (strongly)**

No LOD-bias or mip-level register write exists anywhere in the traced paths.
Instead two near-identical loops (`0x1110b0`-`0x111385`, `0x1559a0`-`0x155c52`)
walk primitives × 4 texture stages, compute the chosen level's address as

```
levelAddr = base + sum over levels l of  (bpp/8) * align(pitch_l, 32) * height_l
```

and store it into the shadow slot for `0x11dc`, marking it dirty. So the
hardware sees a single surface, and the driver reprograms its base per draw.
An open driver can do the same, which sidesteps needing the mip registers at
all.

### 11.10 A known-good complete unit state

Captured from the internal textured-blit setup (`0x10ea10`-`0x10ecbe`):

```
0x1200 = 0x00040101     0x11e8 = 0x00500000     0x11c0 = 0x00001000
0x11c4 = 0x00000049     0x11c8 = 0x00000000     0x11cc = 0xffffffff
0x11d0 = 0x00000000     0x11d4 = 0x00000000     0x11d8 = 0x00000000
0x11dc = <VRAM base>    0x11e0 = pitch | 0x32000000
0x11e4 = height | 0x10000                       0x12f4 = 0x41f80000
```

Also unresolved here: the role of `0x11c8`, `0x11d4`, `0x11d8` (always 0), and
whether the `0x1200+unit*4` write really latches the window.

## 12. Lowering the fixed-function texture environment

An open driver needs the fixed-function texture environment long before it needs
a shader compiler, so this is the part to implement first.

### 12.1 The D3D side (MTXPARD), for reference  **CONFIRMED**

MTXPARD does not have a separate fixed-function path: it lowers the D3D texture
stage states into the *same* IR that ps.1.x compiles to, then runs the packers of
§2/§3 over it.

* The compiler object is reset by `PARD 0x1764d0`, which lays out **4 texture
  stage descriptors of 0x12c bytes at `+0xc20`**, **4 texture-unit states of 8
  bytes at `+0x10f0`** (the same `ctx[+0xc] + unit*8 + 0x10f0` the source
  decoders resolve texture registers to, §5) and **10 IR instruction records of
  0x154 bytes at `+0x12a4`** (`mov ebx,0xa`). Ten again — the same pool the
  ps.1.x path uses.
* The `D3DTSS_COLOROP` / `ALPHAOP` lowering starts at `PARD 0x1766d5`. It
  validates `COLOROP <= 0x1a` (`D3DTOP_LERP`) and each argument's low nibble
  `<= 5` (`D3DTA_` selector) with high nibble `<= 0x30` (the `COMPLEMENT` /
  `ALPHAREPLICATE` flags), with a special case for ops 0x18/0x1b
  (`DOTPRODUCT3` / beyond) when a device flag is set, and dispatches
  `D3DTOP_BUMPENVMAP`/`BUMPENVMAPLUMINANCE` (0x16/0x17) to `0x1769e0`.
* Note the shape of a D3D texture argument: `arg & 0x0f` is the source
  (`DIFFUSE 0`, `CURRENT 1`, `TEXTURE 2`, `TFACTOR 3`, `SPECULAR 4`, `TEMP 5`),
  `arg & 0x10` is `D3DTA_COMPLEMENT` and `arg & 0x20` is
  `D3DTA_ALPHAREPLICATE` — which map onto exactly the two hardware fields of
  §5 (modifier `0xF` = complement) and §6 (replicate `4` = alpha). The
  fixed-function environment is therefore a near-direct fit to this hardware,
  with no lowering tricks needed for the argument modifiers.

The full per-`D3DTOP` instruction sequence was not transcribed; the function is
large and the OpenGL side below is the one an open driver actually needs.

### 12.2 The OpenGL side (libGL) — what an open driver should implement first

All VMAs below are in `opengl6/lib64/libGL.so.1.3.0`. Everything in this
subsection is **CONFIRMED** from disassembly unless marked otherwise.

#### 12.2.1 Entry points

| VMA | role |
|---|---|
| `0x1a8060/0x1a80d0/0x1a8130/0x1a8190` | `glTexEnvf/fv/i/iv` — dispatch thunks only |
| `0x12e750` | the real integer `TexEnv` setter (accepts `GL_TEXTURE_ENV` 0x2300 and `GL_TEXTURE_FILTER_CONTROL` 0x8500) |
| `0x12f3e0` | float/`fv` variant, also `GL_TEXTURE_ENV_COLOR` |
| `0x114810` | draw-time cache-key builder, 5 dwords at `hw+0x768` |
| `0x113700` | program cache (20-byte key) + upload |
| **`0x10ddd0`** | **the fixed-function fragment-program generator** |
| `0x10dcc0` | `GL_COMBINE` decoder |
| `0x10db20` | `GL_COMBINE` instruction emitter |
| `0x10da70` | `GL_SRCn`/`GL_OPERANDn` → {register code, modifier, replicate} |
| `0x10e330` / `0x10e390` | sub-instruction / record initialiser |

The whole fixed-function path runs only when no `GL_MTX` shader is bound
(`cmp BYTE PTR [rbx+0xa528],0` @0x114eda).

#### 12.2.2 The uploaded program blob and the instruction record

Blob = **0x664 bytes**, uploaded verbatim as command 0x45 (`memcpy` length at
`0x1137d9`); command 0x46 re-uploads it with an id; command 0x48 pushes one
stage's `c0` (`{u32 0; u32 instrIdx; float envcol[4]}`, `0x11386c`).

```
+0x000  u32        instruction count (max 10)
+0x004  10 x 0x94  instruction records
+0x5cc  u8         0
+0x5d0  u32        4   = texture-address stage count
+0x5d4  4 x 0x24   texture-address stage descriptors
```

Instruction record, 0x94 bytes:

```
+0x00  RGB sub-instruction (0x34 bytes)
+0x34  u8   1 = separate ALPHA instruction, 0 = the RGB instruction writes RGBA
+0x38  ALPHA sub-instruction (0x34 bytes)
+0x6c  u8   this stage reads c0
+0x70  4 x float  c0 (= GL_TEXTURE_ENV_COLOR)
+0x80  u8   this stage reads c1
+0x84  4 x float  c1 (constructor-initialised to 0.5,0.5,0.5,0.5)
```

Sub-instruction, 0x34 bytes, identical for both halves — this maps 1:1 onto
§2/§3/§5/§6/§8:

```
+0x00 u32  opcode        0 MOV 1 ADD 2 SUB 3 MUL 4 DP3 5 DP4 6 MAD 7 LRP 8 CND 9 CMV 10 CMP
+0x04 u32  destination register code   (always 4 = r0 in the fixed-function path)
+0x08 u32  result scale               (§8 encoding)
+0x0c u8   saturate                   (initialised to 1, never cleared here)
+0x10 u32  src0 code   +0x14 modifier   +0x18 replicate
+0x1c u32  src1 code   +0x20 modifier   +0x24 replicate
+0x28 u32  src2 code   +0x2c modifier   +0x30 replicate
        code 0xff = source unused
```

Cross-confirmed twice: `InitSubRecord` `0x10e330` writes exactly
`{0, 4, 0, 1, 0xff,0,0, 0xff,0,0, 0xff,0,0}`, and the `GL_MTX` shader API builds
the *same* struct in its scratch at `shader+0xa8` and calls the very same
initialiser (`0x1b6482`). `glFragProcRGBAOp*` copies 0x34 bytes to
`shader+0xe0+i*0x94+0x00`, `glFragProcAlphaOp*` to `+0x38` and sets `+0x34 = 1`.

The fixed-function path never emits `r1` (5), `v1` (0x11), or the opcodes
MAD/DP4/CND/CMV/CMP.

#### 12.2.3 Texture-environment state word

All of `GL_TEXTURE_ENV` for unit *u* lives in one dword at
`TEU(u)+0x8`, where `TEU(u) = state + 0xb10 + u*0xb28`:

| bits | field | encoding |
|---|---|---|
| [2:0] | `TEXTURE_ENV_MODE` | MODULATE 1, DECAL 2, BLEND 3, ADD 4, REPLACE 5, COMBINE 6 (default 1) |
| [6:3] | `COMBINE_RGB` | REPLACE 1, MODULATE 2, ADD 3, ADD_SIGNED 4, SUBTRACT 5, INTERPOLATE 6, DOT3_RGB 7, DOT3_RGBA 8, DOT3_RGB_EXT 9, DOT3_RGBA_EXT 10 (default 2) |
| [8:7] [10:9] [12:11] | `SRC0/1/2_RGB` | TEXTURE 0, CONSTANT 1, PRIMARY_COLOR 2, PREVIOUS 3 (defaults 0, 3, 1) |
| [14:13] [16:15] [18:17] | `OPERAND0/1/2_RGB` | SRC_COLOR 0, ONE_MINUS_SRC_COLOR 1, SRC_ALPHA 2, ONE_MINUS_SRC_ALPHA 3 (defaults 0, 0, 2) |
| [21:19] | `COMBINE_ALPHA` | REPLACE 1 … INTERPOLATE 6 (default 2) |
| [23:22] [25:24] [27:26] | `SRC0/1/2_ALPHA` | as above (defaults 0, 3, 1) |
| [28] [29] [30] | `OPERAND0/1/2_ALPHA` | SRC_ALPHA 0, ONE_MINUS_SRC_ALPHA 1 |

`GL_TEXTURE_ENV_COLOR` is 4 floats at `TEU+0xa70`; `GL_RGB_SCALE` /
`GL_ALPHA_SCALE` are floats at `TEU+0xb20` / `+0xb24` and the setter accepts
**only {1, 2, 4}** (`cmp ecx,4; ja reject; mov eax,1; shl rax,cl; test al,0x16`
@0x12efec) — so ×8 and the ÷2/÷4 codes of §8 are unreachable from GL.

`GL_TEXTUREn` is rejected as a `GL_COMBINE` source, hence no
`ARB_texture_env_crossbar`.

#### 12.2.4 Chaining across units

`0x10ddd0` loops units 0..3, skipping a unit unless one of its
`TEXTURE_1D/2D/3D/CUBE` enables is set *and* the texture is complete
(`0x1541d0`). It keeps two working values:

* `TEX` = the unit index, used **directly as the source register code** 0..3;
* `PREV` = `4` (`r0`) if any instruction has already been emitted, else `0x10`
  (`v0`, the iterated primary colour) — or `0x0f` (`c1` = 0.5) when the
  generator's `useC1Flag` is set.

One instruction record per enabled unit, in ascending unit order, destination
always `r0`. Three `GL_MODULATE` units therefore give:

```
MUL r0, t0, v0      ; record+0x34 = 0 -> writes RGBA
MUL r0, t1, r0
MUL r0, t2, r0
```

Zero enabled units gives a single `MOV r0, v0` (RGBA).

Mode dispatch is a jump table at `.rodata 0x36bac4` indexed by
`state & 7` — there is **no data-driven stage template**, the six builders are
code:

| mode | builder |
|---|---|
| MODULATE | `0x10d280` |
| DECAL | `0x10d3d0` |
| BLEND | `0x10d710` |
| ADD | `0x10d900` |
| REPLACE | `0x10d460` |
| COMBINE | `0x10dcc0` |

#### 12.2.5 Per-mode instruction sequences

`fmt` is the texture's base internal format. Result scale is 0 (×1) for **all**
non-COMBINE modes — none of the five builders touches `+0x08`.

**GL_REPLACE** (`0x10d460`)

| | |
|---|---|
| RGB, `fmt != GL_ALPHA` | `MOV r0, TEX` |
| RGB, `fmt == GL_ALPHA` | `MOV r0, PREV` |
| alpha, `GL_ALPHA` | separate: `MOV r0.a, TEX` |
| alpha, `GL_RGB` / `GL_LUMINANCE` | separate: `MOV r0.a, PREV` |
| alpha, `GL_RGBA` / `GL_LUMINANCE_ALPHA` / `GL_INTENSITY` | co-issue, `record+0x34 = 0` |

**GL_MODULATE** (`0x10d280`)

* `fmt != GL_ALPHA`: one co-issued RGBA instruction `MUL r0, TEX, PREV`
  (`record+0x34 = 0`).
* `fmt == GL_ALPHA`: RGB `MOV r0, PREV`, alpha `MUL r0.a, TEX, PREV`.

**GL_DECAL** (`0x10d3d0`, no format test)

```
RGB   : LRP r0, TEX, PREV, TEX.a      ; src2 replicate = 4 (alpha)
alpha : MOV r0.a, PREV
```

**GL_BLEND** (`0x10d710`)

```
RGB   : fmt == GL_ALPHA ->  MOV r0, PREV
        else            ->  LRP r0, c0, PREV, TEX      ; record+0x6c = 1 (reads c0)
alpha : fmt == GL_INTENSITY -> co-issue (record+0x34 = 0)
        else                -> MUL r0.a, TEX, PREV
```

**GL_ADD** (`0x10d900`)

```
RGB   : ADD r0, TEX, PREV
alpha : fmt == GL_INTENSITY -> co-issue
        else                -> MUL r0.a, TEX, PREV
```

**LRP semantics, pinned three independent ways** — `dst = src0*src2 +
src1*(1-src2)`:

* `GL_BLEND`: `LRP(c0, prev, tex)` = `Cp*(1-Ct) + Cc*Ct` ✓ GL spec
* `GL_DECAL`: `LRP(tex, prev, tex.a)` = `Ct*At + Cp*(1-At)` ✓ GL spec
* `GL_COMBINE` `INTERPOLATE`: `Arg0*Arg2 + Arg1*(1-Arg2)` ✓ GL spec

In hardware slot terms (§7.4) that is `A*C + B*(1-C)` — `MAD` with an implicit
complement on `B`, consistent with the slot permutation of the opcode tables.

#### 12.2.6 GL_COMBINE

The decoder `0x10dcc0` unpacks the state word and calls the emitter twice, once
for the RGB sub-instruction with `GL_RGB_SCALE` and once for the alpha
sub-instruction with `GL_ALPHA_SCALE`.

Source/operand mapping (`0x10da70`):

| GL source | register code |
|---|---|
| `GL_TEXTURE` | the unit index (0..3) |
| `GL_CONSTANT` | 0x0E (`c0`) |
| `GL_PRIMARY_COLOR` | 0x10 (`v0`) |
| `GL_PREVIOUS` | 4 (`r0`), or 0x10 (`v0`) on the first instruction |

| GL operand | modifier (§5) | replicate (§6) |
|---|---|---|
| `GL_SRC_COLOR` | 0 | 0 |
| `GL_ONE_MINUS_SRC_COLOR` | 0x0F | 0 |
| `GL_SRC_ALPHA` | 0 | 4 |
| `GL_ONE_MINUS_SRC_ALPHA` | 0x0F | 4 |

Function mapping (jump table `.rodata 0x36ba98`, 11 entries):

| `COMBINE_*` | opcode | notes |
|---|---|---|
| REPLACE | `MOV` | src1/src2 = unused |
| MODULATE | `MUL` | src2 unused |
| ADD | `ADD` | src2 unused |
| ADD_SIGNED | `ADD` | src0 modifier rewritten 0→2 (bias), 0x0F→3 (bias-negate) — `A+B-0.5` |
| SUBTRACT | `SUB` | src2 unused |
| INTERPOLATE | `LRP` | all three sources |
| DOT3_RGB | `DP3` | src0 *and* src1 modifiers rewritten to 4 (signed) or 5 (signed-negate) — `2x-1` |
| DOT3_RGBA | `DP3` | as above **plus** `record+0x34 = 0` so the result writes RGBA |
| DOT3_RGB_EXT / DOT3_RGBA_EXT | same | but the result scale is forced to ×1 |

The two rewrites are exactly right: `ADD_SIGNED` needs `x-0.5`, and an inverted
operand needs `(1-x)-0.5 = 0.5-x`; `DOT3` needs `2x-1`, and an inverted operand
`1-2x`. The reason libGL keeps four DOT3 codes is that the ARB tokens honour
`GL_RGB_SCALE` while the EXT tokens must ignore it.

Scale emission (`0x10dbe2`): compare the float scale against 1.0 → 0, 2.0 → 1,
otherwise → 2. So only ×1/×2/×4 ever reach `0x126c[19:17]` from GL.

#### 12.2.7 Extensions advertised

Present in the extension string at `.data 0x66e060`:
`GL_ARB_texture_env_combine`, `GL_ARB_texture_env_dot3`,
`GL_ARB_texture_env_add`, the three `GL_EXT_` equivalents,
`GL_MTX_fragment_shader`, `GL_ARB_multitexture`, `GL_ARB_texture_cube_map`,
`GL_NV_texgen_reflection`.

Absent (zero occurrences in the binary): `GL_NV_register_combiners`,
`GL_NV_texture_env_combine4`, `GL_ATI_fragment_shader`,
`GL_ARB_texture_env_crossbar`.

#### 12.2.8 Two loose ends on the GL side

* `useC1Flag` — `0x113700` stores it in bit 7 of `hw+0x77a` (part of the cache
  key) and `0x10ddd0` turns it into "the first stage reads `c1` = 0.5 instead of
  `v0`". Its caller computes it as `(ebp>>10)&1` @0xc51bb from a state mask; the
  GL state it corresponds to was **not identified**. Other callers pass 0.
* **A likely vendor bug**: the env-colour map is *written* indexed by texture
  unit (`mov [rbp+rbx*4+0x664],r12d` @0x10e11a) but *read* indexed by
  instruction ordinal (`node+0x38+i*4` @0x113853). With a lower-numbered unit
  disabled, the `c0` upload (command 0x48) for a `GL_BLEND` or `GL_CONSTANT`
  stage is skipped. Worth reproducing on hardware before copying the logic.

## 13. `tools/parhelia-fragdis.py`

GPL-2, `Copyright (c) 2026 Rene Rebe <rene@exactco.de>`.

```
parhelia-fragdis.py --selftest       # decode the real programs harvested above
parhelia-fragdis.py --words 0x1264=0x0703c000,0x126c=0x00400803
parhelia-fragdis.py --stream cmdbuf.bin
```

`--stream` implements the §10 packet framing (terminate on index 0x21, swallow
a dword on index 0x20), filters out everything that is not a combiner register,
splits the writes into slots and prints each one with its symbolic RGB and alpha
instructions, their algebraic expansion, the decoded Q1.10 constants and the raw
register values. When a slot carries no `0x1260`/`0x1268` it says so explicitly
rather than inventing an alpha instruction.

It is deliberately conservative: unknown modifier, replicate, scale or opcode
fields print as `{modX}` / `{repX}` / `_scaleX` / `OP_xxxx` rather than being
guessed at.

Sample output for the video shader of §10.1:

```
slot 0 (program instruction 0)
    rgb  MAD        r0, t0, c1, c0
           ; r0 = t0*c0+c1
      ; no separate alpha instruction: the alpha pipe runs the rgb one with
        alpha-replicate disabled
    const c0 = [+0.7998 +0.6992 +0.5996 +0.0000]
    const c1 = [+0.0996 -0.0234 +0.2998 +0.0000]
```

## 14. Corrections to the previous revision

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
8. **CMV's base is `0x5c00`**, no longer a capture item (and the X driver
   emits it: `HAL 0x6e083` `or r12d,0x5c00`).
9. **"`0x1260`/`0x1268` are the texture-address instruction."** Wrong — they are
   the **alpha** instruction of the same slot (§1.1a). The stage flag bit0 is
   the *separate-alpha* flag, and the `& ~0x124` default the previous revision
   could not explain is the co-issue fallback: run the RGB instruction in the
   alpha pipe with alpha-replicate cleared. Texture-address stages are a
   separate 4-entry descriptor array (§9.4).
10. **Result-scale codes 4 and 5 for ÷2/÷4.** Wrong: the field is a signed
   3-bit shift, so ÷2 = 7 and ÷4 = 6 (§8).
11. **Packet framing.** Index 0x21 terminates a packet, but index 0x20
   (`0x1080`, DMAPAD) *does* consume a data dword and discard it. The earlier
   wording lumped them together (§10).
12. **libGL `0x36ba98` is not a "GL_MTX opcode → ALU-class" table.** It is the
   `GL_COMBINE`-function → IR-opcode jump table (§12.2.6). The ALU-class table
   in the previous revision was a misreading and has been removed.

---

## 15. What is *not* pinned down

Stated explicitly rather than guessed:

* **Texture-address opcodes and the register they are written to.** The
  descriptors exist (4 × 0x24 bytes at blob+0x5d4, §9.4) but no path in any of
  the four binaries emits a non-trivial one, and no engine register could be
  tied to them. Recovering the `GL_OP_TEX*` encodings needs a hardware capture
  of a bump-mapping or dependent-read draw.
* **`0x126c` bit 16.** No writer anywhere, yet it is deliberately *preserved*
  by every packer and by the X driver's mask `0xff05fffd` — so it is a real
  field of unknown purpose. Bit 15 and bits 25-31: no writer either.
* **Replicate codes 5-7, modifier codes 6-0xE, opcode-field values with
  bits [11:9] = 1 or 7.** Unused by every driver.
* **Why hardware slots 6 and 7 are skipped**, and why the walk order is
  even-then-odd. Every emitter — MTXPARD, the X driver and the kernel — does the
  same thing unconditionally.
* **Fog and specular combine** — where the fixed-function fog/specular add sits
  relative to the combiner was not investigated.
* **libGL's per-stage register template** (`.rodata` 0x39fc1c, **CONFIRMED**
  re-dumped) is `1374 1084 1090 1094 10b0 1104 1264 126c 1278 127c 1280 1284` —
  twelve `localReg` slots, and `0x1260`/`0x1268` are not among them, even though
  the ICD's IR clearly carries alpha sub-instructions. libGL does not contain
  the `0x1264`/`0x126c` immediates at all; it uploads the 0x664-byte blob and
  the **kernel** (`KRN mtx1653`/`mtx2619`/`mtx530`) does the packing. Why this
  template table also exists was not established.
* **libGL's `useC1Flag`** and the env-colour indexing bug (§12.2.8).
* **Sampler residuals** (details in §11): the internal-format-id → 6-bit
  hardware format code join; which of `0x11c4`'s three 3-bit fields is
  min/mag/mip and their enum values; which `0x11c0` 2-bit code is clamp versus
  mirror; where anisotropy / LOD bias / max-mip-level land (they reach no
  register in any traced path); the role of `0x11c8`, `0x11d4`, `0x11d8`;
  `0x11e0[31:30]` and `0x11e4[31:28]`/`[16:14]`.

## 16. Confidence statement

The **instruction encoding is solid**. Every field of `0x1264` and `0x126c`
except bits 15-16 and 25-31 of the latter was read out of a binary, and the
load-bearing ones (source packing, modifiers, replicate, opcode, scale,
saturate, destination) were read out of **six independent packers in four
independent binaries** — MTXPARD's ps.1.x and ps.1.4 packers, the X driver's
`CDPSPixelShader` builders, and the kernel HAL's RGB and alpha packers — which
agree bit for bit. They were then validated by blind-decoding literal shader
words those same binaries embed.

The **opcode table is complete and authoritative** for the 11 ALU operations,
including the A/B/C slot permutation, which three independent tables and the
kernel's own operand-liveness logic agree on.

The **RGB/alpha co-issue model is now settled**, not inferred: a slot holds two
complete instructions, `0x1264`/`0x126c` and `0x1260`/`0x1268`, and the
separate-alpha flag decides whether the second is emitted. This replaces the
previous revision's guesswork about write masks and pipes.

The **texture-address / dependent-read half of the ISA is still not recovered.**
The descriptors exist and the count is 4; the opcodes and their register are
unknown.

The **fixed-function lowering is fully mapped** (§12.2) and is the right place
to start an open driver: six modes, at most one instruction per texture unit,
destination always `r0`, and a `GL_COMBINE` path that maps onto the hardware
almost one-to-one.

The **sampler state** (§11) is confirmed at the register-and-layout level, with
a known-good complete unit state, but three enum tables (format code, filter,
wrap) are open; §11.9's "mip level chosen on the CPU" means an open driver can
ship without the mip registers.

**Nothing here has been validated against real silicon.** It is all static
reverse engineering of four vendor binaries plus the shipped `glext.h`. The
first hardware test should be the simplest possible one: a single slot 0
instruction `MOV r0, v0` with the alpha pair omitted, drawing a Gouraud
triangle, then the same with `0x1260`/`0x1268` carrying `MOV r0, c0` to check
that only alpha changes.
