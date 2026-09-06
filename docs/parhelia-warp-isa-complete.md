Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# Parhelia WARP vertex ISA — the complete encoding, from the vendor's own assembler

This document closes the open items of `docs/parhelia-vertex-isa.md` PART 2 and
`docs/parhelia-warp-binding.md` by reading the **DirectX vs_1_x → WARP
assembler inside the Windows D3D driver** `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`
(PE, image base 0x10000; file offset = VA − 0x10000 for `.text`/`.data`).
That assembler is the one place in the whole vendor stack that *encodes* WARP
instructions from scratch, field by field, so it settles the meaning of every
bit-field the kernel's fragment patcher only ever *patched*.

Evidence tags: **[BIN]** = read out of a binary at the quoted VA/offset;
**[INF]** = inferred from [BIN] facts, not itself read from code; **[OPEN]** =
not determined. Nothing here has been executed on hardware.

Binaries used: `MTXPARD.dll` (D3D), `kernel/mtx_parhelia.o` (fragment bytes only,
by ELF symbol), `MTXOGL.dll` (only to confirm no second assembler exists).
No proprietary C source was read.

---

## 0. Headline

* The WARP instruction is a **dual-issue bundle**: a *primary* op (opcode in
  `w2[30:23]`, up to three sources, writes destination port **B**) and an
  optional *secondary* op (4-bit opcode in `w1[12:9]`, reads **src2**, writes
  destination port **A**) execute in the same 96-bit word. [BIN, emit routine
  `0x192da0`]
* Every register operand carries a **2-bit register-file code** next to its
  4-bit index. Source files: `0 = input v#`, `1 = temp r#`, `2 = latch T#`,
  `3 = constant c#`. Destination files: `0 = latch/address T#`, `1 = temp r#`,
  `2 = output o#`, `3 = constant c#`. The former unknown flags `f0a`, `f0bc`,
  `f0e`, `f1a`, `f1c` are exactly these file codes. [BIN `0x192980`,
  `0x1929d0`, `0x192b70`, `0x192c30`, `0x192d10`, tables `0x1dd16c`/`0x1dd194`]
* There is **one shared 12-bit swizzle field** for src1/src2
  (`{w2[9:0], w1[31:30]}`) and `w2[22]` says which of the two it belongs to;
  src0's swizzle is `w2[21:10]`. Swizzle codes: `0-3 = x y z w`, `4 = 0.0`,
  `5 = 1.0`, `7 = own component (identity)`, `6 = [OPEN]`. [BIN `0x192a20`,
  `0x192a80`, `0x190f70`, dst-expansion at `0x1916ef`]
* Relative addressing: `w1[29:27]` = which operand is indexed (`4/5/6 = src0/1/2`,
  `7 = port-A destination`), `w1[13]` = index source (`1` = the program's `a0`,
  written by the `mova` op; `0` = a driver-preset base register selected by
  `w1[26:25]`). [BIN `0x192b70..0x192d10`, `0x1913f4`, `0x19144d`]
* Register indices above 15 use `exthi = w1[16:14]` + `extsel = w1[19:17]`
  (one operand per instruction), giving 7-bit `r#/c#` (0..127). Constants
  `c128..c255` are reached through base register 2. [BIN, same routines]
* The D3D opcode → WARP table is a 34-case switch at `0x191626` (index table
  `0x192850`, targets `0x1927c8`); the complete table is in §5.
* D3D output mapping: `oD0 → o1`, `oD1 → o2`, `oT0..7 → o3..o10`, `oFog → latch
  T0 then o10.w`, `oPos → r14`, `oPts → r15` (the driver epilogue turns r14 into
  the screen-space `o0`). Port-B index `0 = position` is confirmed by the
  kernel's own output-format tails, which end in `add o0 ← r7`. [BIN `0x190e60`,
  `0x1926fa`, `mtx1661`]

---

## 1. Where the assembler is

| VA (MTXPARD.dll) | role |
|---|---|
| `0x171a90` | D3DVSD declaration parser (`token >> 29` switch, table `0x1720dc`) |
| `0x1903d0` / `0x190260` | vs token stream validation / pre-pass |
| `0x190e60` | D3D **destination** token → (type, index, writemask) |
| `0x191370` | D3D **source** token → (type, index, negate, 12-bit swizzle, relative) |
| `0x191490` | per-instruction compiler: fills a descriptor, then the **D3D-opcode switch at `0x19161f`** |
| `0x192af0` | descriptor init (0x84 bytes; see §3.1) |
| `0x192920` | descriptor completion from the WARP opcode-info table (`0x198030` → `0x1dd510`) |
| `0x1928e0` | NOP template `{0x00000200, 0x00040000, 0x00000000}` |
| `0x192900` / `0x1928c0` | "wra" template `{0x00080200, 0x00440000, 0xb01a21a2}` (opcode 0x60, f2b=1, identity swizzles) |
| `0x192980` | encode port-B destination |
| `0x1929d0` | encode port-A destination |
| `0x192a20` / `0x192a80` | encode the shared src1 / src2 swizzle |
| `0x192b70` / `0x192c30` / `0x192d10` | encode src0 / src1 / src2 |
| `0x192da0` | **emit**: descriptor → 3 dwords (primary or secondary path) |
| `0x192f60` / `0x192f90` | emit/init a *pair* of descriptors into one instruction (dual issue) |
| `0x18a460` | program finaliser: prologue/epilogue (clip planes, tex-gen, fog, point size), END |
| `0x18d670` | program builder: allocates `(n+0x6c)` instructions pre-filled with the wra template, runs `0x18a460` |
| `0x79e00..0x7a0fa` | Windows twin of the kernel field patcher `mtx3558` (same field kinds; cross-check §2) |
| `0x1cdc30` / `0x1ddf68` | D3D opcode *validation* table (mnemonic, expansion count, token length, masks) |

The second opcode switch at `0x196c6f` (function `0x196a10`, cases for op−1) is
a pre-pass over the same token stream that rewrites texture-coordinate output
instructions (`0x60000000` dst) and builds the same descriptors; its per-case
bodies were not decoded. [BIN location, OPEN semantics]

---

## 2. Bit-field map — every field named

`w0`, `w1`, `w2` are the three little-endian dwords of one instruction. The
"kind" column is the kernel patcher's field id (`mtx3558`, `.rodata+0x2eb0`);
the Windows patcher at `0x79e00` writes the identical masks (`0x40`, `0x180`,
`0x1e00`, `0x2000`, `0x1c000`, `0x3`, `0x300000`, `0x1c00000`, `0x6000000`,
`0x38000000`, `{w1[31:30],w2[9:0]}`, `0x3ffc00`, `0x400000`, `0x7f800000`,
bit 31, `0x3c00`, `0x3c000`, `0x40000`, `0x80000`, `0x1f00000`, `w0[31:27]`,
`w1[6:2]`, imm32) — the two OS drivers agree on the map. [BIN]

| bits | old name | meaning | evidence |
|---|---|---|---|
| `w0[3:0]` | wmask0 | port-A write mask (xyzw = 1,2,4,8); 0 = no port-A write | [BIN 0x1929d0] |
| `w0[7:4]` | dst0 | port-A destination index (4 bits) | [BIN 0x1929d0] |
| `w0[9:8]` | f0a | **port-A destination register file** (§3.3) | [BIN 0x1929d0] |
| `w0[13:10]` | wmask1 | port-B write mask; 0 = no port-B write | [BIN 0x192980] |
| `w0[17:14]` | dst1 | port-B destination index | [BIN 0x192980] |
| `w0[19:18]` | f0bc | **port-B destination register file** | [BIN 0x192980] |
| `w0[23:20]` | src2 idx | source-2 index (low 4 bits) | [BIN 0x192d10] |
| `w0[24]` | src2 bit4 | **src2 negate** | [BIN 0x192d10] |
| `w0[26:25]` | f0e | **src2 register file** | [BIN 0x192d10] |
| `w0[30:27]` | src1 idx | source-1 index | [BIN 0x192c30] |
| `w0[31]` | src1 bit4 | **src1 negate** | [BIN 0x192c30] |
| `w1[1:0]` | f1a | **src1 register file** | [BIN 0x192c30] |
| `w1[5:2]` | src0 idx | source-0 index | [BIN 0x192b70] |
| `w1[6]` | src0 bit4 | **src0 negate** | [BIN 0x192b70] |
| `w1[8:7]` | f1c | **src0 register file** | [BIN 0x192b70] |
| `w1[12:9]` | f1d | **secondary (port-A) opcode**, 0 = none (§4.2) | [BIN 0x192ee4] |
| `w1[13]` | dpgA2 | **index-register select**: 1 = `a0` (latch written by `mova`), 0 = preset base register `w1[26:25]` | [BIN 0x192df4 + 0x19144d] |
| `w1[16:14]` | exthi | high 3 bits of the one extended operand index | [BIN 0x192be8] |
| `w1[19:17]` | extsel | which operand is extended: 0/1/2 = src0/1/2 (only these three are ever written) | [BIN 0x192be8/0x192ca3/0x192d80] |
| `w1[21:20]` | dpgA0 | never written by the assembler; kernel patch kind 0x13; seen = 1 in `mtx3888`/`mtx3559` together with `rel=5.1` | [OPEN] |
| `w1[24:22]` | f1g | relative/extension control for **port B / src1**: 7 = port-B destination is relative; becomes 2 when an extended source is used and it was 0 (src0) or 1 (src1); template value 1 | [BIN 0x1929bc, 0x192c12, 0x192cdc] |
| `w1[26:25]` | dpgA1 | **relative-address component / base-register select** (`a0.{x,y,z,w}` when `w1[13]=1`; base register 0..3 when `w1[13]=0`) | [BIN 0x192bcc] |
| `w1[29:27]` | f1i | **relative operand select**: 4 = src0, 5 = src1, 6 = src2, 7 = port-A dst; becomes 2 when an extended src0/src1 is used and no operand is relative | [BIN 0x192bcc..0x192d76, 0x192a09] |
| `{w2[9:0], w1[31:30]}` | swzA | **shared 12-bit swizzle**: src1 when `w2[22]=0`, src2 when `w2[22]=1`; 3 bits per component, comp c at `[3c+2:3c]`, identity 0x688 | [BIN 0x192a20/0x192a80] |
| `w2[21:10]` | swzB | **src0 swizzle** | [BIN 0x192bb5] |
| `w2[22]` | f2a | **shared-swizzle owner**: 0 = src1, 1 = src2 (also 1 on immediate-carrying control ops, where the field is not a swizzle) | [BIN 0x192aa7] |
| `w2[30:23]` | opcode | primary opcode | [BIN 0x192dc9] |
| `w2[31]` | f2b | descriptor byte 0, **1 on every arithmetic instruction**, 0 on NOP/END/control | [BIN 0x192de3] |
| `{w2[21:0], w1[31:22]}` | imm32 | immediate for control-class opcodes (< 0x60) | [BIN kernel kind 0x23, Windows 0x7a0d8] |

Corrections to `parhelia-vertex-isa.md`: the naming "swzA = src0 / swzB = src1"
is swapped (see above); `w1[13]`, `w1[26:25]` are relative-addressing fields,
not port-A page bits (the self-test sweep `mtx2785` enumerates destination
*files* and relative modes — instr 4/5 write `T0/T1` with secondary op 9, instr
10.. write `c0..` with `rel=7`); `f0a = 2` is not "port A idle" but the wra
template's default file code with mask 0. [BIN]

### 2.1 The encoder, as pseudo-code [BIN, transcribed from 0x192980..0x192d9b]

```
FILE_DST[type] = {0:1, 1:1, 2:2, 3:3, 4:0, 5:0, 6:1}        ; table 0x1dd16c
FILE_SRC[type] = {0:1, 1:0, 2:2, 3:3, 4:2, 5:1, 6:0}        ; table 0x1dd194
                     ; type: 0 temp r#, 1 input v#, 2 output o#, 3 const c#,
                     ;       4 latch T#, 5 address a0 (= latch 1), 6 unused

set_dstB(t,i,m,rel): w0 = (w0 & 0xfff003ff) | (FILE_DST[t]<<18) | (i<<14) | (m<<10)
                     if rel: w1 |= 0x1c00000                     ; f1g = 7
set_dstA(t,i,m,rel): w0 = (w0 & 0xfffffc00) | (FILE_DST[t]<<8) | (i<<4) | m
                     if rel: w1 |= 0x38000000                    ; f1i = 7
swz_shared(s, owner2):                                           ; 0x192a20 / 0x192a80
     if s != 0x688: w1 = (w1 & 0x3fffffff) | (s<<30)
                    w2 = (w2 & (owner2 ? 0xfffffc00 : 0xffbffc00)) | (s>>2) | (owner2 ? 0x400000 : 0)
     else if (w2 & 0x400000) == owner2<<22: w1 &= 0x3fffffff; w2 = (w2 & 0xfffffda2) | 0x1a2
set_src0(t,i,neg,s,rel,rc):
     w1 = (w1 & 0xfffffe03) | (FILE_SRC[t]<<7) | (neg<<6) | ((i&15)<<2)
     w2 = (w2 & ~0x3ffc00) | (s<<10)
     if rel: w1 = (w1 & 0xe1ffffff) | ((0x10 | rc)<<25)         ; f1i=4, comp=rc
     if i >= 16: w1 = (w1 & 0xfff03fff) | ((i & 0x70)<<10)       ; exthi, extsel=0
                 if (w1 & 0x38000000)==0: w1 = (w1 & 0xd7ffffff) | 0x10000000   ; f1i=2
                 if (w1 & 0x01c00000)==0: w1 = (w1 & 0xfebfffff) | 0x00800000   ; f1g=2
set_src1(t,i,neg,s,rel,rc):
     w1 = (w1 & ~3) | FILE_SRC[t];  w0 = (w0 & 0x07ffffff) | (neg<<31) | ((i&15)<<27)
     swz_shared(s, 0)
     if rel: w1 = (w1 & 0xe9ffffff) | ((0x14 | rc)<<25)         ; f1i=5
     if i >= 16: w1 = (w1 & 0xfff23fff) | (((i & 0x70) | 0x80)<<10)   ; exthi, extsel=1
                 if (w1 & 0x38000000)==0x08000000: f1i=2;  if (w1 & 0x1c00000)==0x400000: f1g=2
set_src2(t,i,neg,s,rel,rc):
     w0 = (w0 & 0xf80fffff) | (FILE_SRC[t]<<25) | (neg<<24) | ((i&15)<<20)
     swz_shared(s, 1)
     if rel: w1 = (w1 & 0xf1ffffff) | ((0x18 | rc)<<25)         ; f1i=6
     if i >= 16: w1 = (w1 & 0xfff43fff) | (((i & 0x70) | 0x100)<<10)  ; exthi, extsel=2

emit(desc, ins):                                                 ; 0x192da0
     if desc.mode==0 and desc.op < 0x60: ins = NOP; w2 |= op<<23; return   ; control class
     w2 = (w2 & 0x7fffffff) | (desc.f2b<<31)
     if desc.anyRelative: w1 = (w1 & ~0x2000) | (desc.idxsel<<13)       ; default idxsel=1 (a0)
     if desc.mode==0:  w2 = (w2 & ~0x7f800000) | (op<<23)
                       set_dstB(dst); set_src0(slot0); set_src1(slot1); set_src2(slot2)
     else:             w1 = (w1 & ~0x1e00) | (op<<9)               ; secondary op
                       set_dstA(dst); set_src2(slot0)               ; secondary reads src2
```

Every D3D instruction starts from the **wra template** (the instruction buffer
is pre-filled with it at `0x18d712`): `w0=0x00080200 w1=0x00440000
w2=0xb01a21a2` = primary opcode 0x60 (no-op arithmetic), `f2b=1`, both file
codes 2 with masks 0, `extsel=2` (inert), `f1g=1`, identity swizzles. The
program ends with `END = 00000200 00040000 14c00000` (`0x18d5b2`: NOP template,
`w2 = (w2 & 0x94ffffff) | 0x14c00000`). [BIN]

---

## 3. Register files

### 3.1 The compiler's descriptor (0x84 bytes, init at 0x192af0) [BIN]

`+0x00 f2b(=1) | +0x0c opcode(=0x60) | +0x10 mode (0 primary, 1 secondary) |
+0x14 hasDst | +0x18 dstType | +0x1c dstIdx | +0x20 dstMask | +0x24 dstRel |
+0x28 nsrc | three 0x18-byte slots at +0x2c/+0x44/+0x5c {used, type, idx, neg,
swz(=0x688), rel} | +0x74 anyRel | +0x78 (=5) | +0x7c idxsel(=1 → w1[13]) |
+0x80 relComp (→ w1[26:25])`. `0x192920` fills `hasDst`/`nsrc` from the
opcode-info record (§4).

### 3.2 D3D register → WARP operand [BIN 0x190e60 (dst), 0x191370 (src)]

| D3D | type | index | WARP encoding |
|---|---|---|---|
| `r#` (regtype 0) | 0 | # | src file 1 / dst file 1, idx # |
| `v#` (regtype 1) | 1 | # | src file 0, idx # (source only) |
| `c#` (regtype 2) | 3 | # (13 bits kept) | src file 3; `#<16` direct, `16..127` via exthi, `128..255` via base register 2 (§3.5) |
| `a0` (regtype 3) | 5 | 1 | dst file 0, idx 1 (= latch T1), written by secondary op 9 (`mova`) |
| `oPos` (rastout 0) | 0 | 0xe | **temp r14** (the epilogue derives `o0` from it) |
| `oFog` (rastout 1) | 4 | 0, mask .x | latch T0; the compiler appends `secondary op 7: o10.w ← T0.(0,0,0,x)` (`0x1926fa`) |
| `oPts` (rastout 2) | 0 | 0xf, mask .x | temp r15 |
| `oD#` (attrout, regtype 5) | 2 | #+1 | dst file 2, **o1 = colour0, o2 = colour1** |
| `oT#` (texcrdout, regtype 6) | 2 | #+3 | dst file 2, **o3..o10 = texcoord 0..7** |
| internal `T#` (regtype 7 / type 4) | 4 | # | dst file 0 / src file 2 — the latch file used as scratch by the compiler (T0 for fog/clamp, T1 for pow/exp/log temporaries) |

Source negate = D3D modifier field `== 0x1000000` (`D3DSPSM_NEG`). Relative
flag = D3D token bit 13. Write mask = token bits 19:16. The 8-bit D3D swizzle
(2 bits/comp) is widened to 12 bits (3 bits/comp) at `0x1913f4..0x191433`
(`c0 | c1<<3 | c2<<6 | c3<<9`, identity `0xE4 → 0x688`). [BIN]

### 3.3 File codes as seen by the hardware [BIN tables + kernel fragments]

| code | as a source (`f1c`/`f1a`/`f0e`) | as a destination (`f0a`/`f0bc`) |
|---|---|---|
| 0 | input `v#` (vertex fetch) | latch/address file: `T0`, `T1 (= a0)`, … |
| 1 | temp `r#` | temp `r#` |
| 2 | latch `T#` | output `o#` (12 slots: 0 position, 1/2 colour, 3..10 texcoord, 11 seen written by the kernel tails, 10.w = fog) |
| 3 | constant `c#` | constant `c#` (the self-test writes `c0..` through port A this way) |

Consequences: port A (secondary op) can target temps, latches *and* outputs
(the compiler writes `o10.w` and `o1` through port A); port B can target temps,
latches and outputs (the vendor dp4 blocks write `r3` / `r5` through port B).
Both ports carry a file code, so the "9-bit port-A address" reading in
`parhelia-vertex-isa.md` §11 is withdrawn; the self-test's 384 readback entries
are 24 (file, relative-mode) combinations × 16. [INF from the BIN map]

### 3.4 Input registers (vertex fetch) [BIN 0x171a90, tables 0x19e7ec..0x19e8e0]

With a user vertex shader, **hardware attribute slot = D3D `v#`** and the
instruction reads it as source file 0, index `#`. The declaration parser builds
24 eight-byte fetch descriptors at `ctx+0x108 + slot*8`: `[7:0]` byte offset in
the stream (running sum of `0x19e7ec[type]` dwords ×4), `[10:8]` stream,
`[14:11]` semantic class (`0x19e830`), `[19:15]` format (`0x19e8e0[type + 8*mode]`),
`[23:20]` component count (`0x19e80c`), `[27:24]` type class (`0x19e8bc`),
bit 30 = enabled. `D3DVSD_SKIP` adds to the stream offset; `D3DVSD_STREAM`
selects `[esi+0xc8+stream*4]`. For fixed-function (FVF) declarations the D3D
register is first remapped through `0x19e878`: position→0, blendweight→2,
blendindices→3, normal→4, psize→7, diffuse→8, specular→9, tex0..7→10..17.
The kernel's own fixed-function fragments use the classic D3D numbering
directly (`v0` position, `v3` normal, `v5` diffuse, `v6` specular,
`v7..v14` texcoords — e.g. `mtx737`: `add o1 ← v5`, `add o2 ← v6`;
`mtx1621`: `mov o3..o6 ← v7..v10`). How these descriptors reach MMIO
`0x1110/0x1114/0x1118` and `0x10b8` is not visible in MTXPARD (it goes
through MTXPARM.sys) — [OPEN]; see `parhelia-warp-binding.md` §1 for the
register side.

### 3.5 Constant registers [BIN + INF]

* `c0..c15`: direct 4-bit index, file 3. `c16..c127`: 4-bit index + `exthi`,
  `extsel` = the operand (one extended operand per instruction). [BIN]
* `c128..c255`: `0x191370` forces `rel=1, w1[13]=0, w1[26:25]=2` and encodes
  the low 7 bits — i.e. **base register 2 + (n & 0x7f)**. `mindex` (D3D 0x62)
  emits an extra `add` (0x71) for indices ≥ 0x80 for the same reason. The
  declaration parser accepts `D3DVSD_CONST` addresses up to 0x100. [BIN]
* `c[a0.x + n]`: `rel=1, w1[13]=1, w1[26:25]=0`, `f1i = 4/5/6` for the operand. [BIN]
* Fixed-function code (Windows generator `0x18a460`, kernel fragments) uses
  `w1[13]=0` with base register 0 or 1 (`relc`), e.g. `dp4 r2.x ← r3, c[105+base0]`
  (`mtx999`, matrix-palette entry 15×7 = 105, inside the palette block
  `0x000..0x0df` of `parhelia-block-state-map.md`), so **base 0 = 0** and the
  direct index space is the block-file index space [INF, strong]. The D3D path
  needs **base 2 = 128**. Base 1's value and how the bases are programmed
  are [OPEN] (candidates: fixed `128×k`, or written through the `0x910/0x938`
  port / a block load).
* Block loads (opcode 0x41, `first`, `count`) therefore address the same
  file: for the minimal program in §6 load the 4 matrix rows at `first=0,
  count=4`; the kernel places its projection at `0x149..0x14c` and reads it
  through an index register loaded from a constant (`mtx3945`/`mtx3201`
  instr 2: `add T1 ← c[1+base1]`), which is why those fragments never name
  0x149 directly. [BIN fragments, INF mapping]

---

## 4. WARP opcodes

### 4.1 The opcode-info table (0x1dd510, 2×256 pointers → 16-byte records) [BIN]

Record: `{u8 class, u8 dstMask, u16 read[w,z,y,x] (nibble per source), u8 hasDst
@+0xa, u8 nsrc @+0xb, u8 unit @+0xc}`. Only these primary opcodes exist:

| op | nsrc | reads (per dest comp) | unit | identity (see §5 for the D3D proof) |
|---|---|---|---|---|
| 0x60 | 0 | — | — | arithmetic no-op; carrier for a secondary op ("wra") [BIN] |
| 0x61 | 2 | componentwise | 7 | **min** [BIN] |
| 0x62 | 2 | componentwise | 7 | **max** [BIN] |
| 0x63 | 2 | componentwise | 7 | **slt** [BIN] |
| 0x64 | 2 | componentwise | 7 | compare used as `T1 = op64(r14.wwww, 0000)` in the epilogue (w-sign test) [INF: sle/seq family] |
| 0x65 | 2 | componentwise | 7 | [OPEN] compare family |
| 0x66 | 2 | componentwise | 7 | **sge** [BIN] |
| 0x67, 0x68 | 2 | componentwise | 7 | [OPEN] compare family |
| 0x69 | 1 | componentwise | 7 | [OPEN] |
| 0x6d | 1 | componentwise | 7 | primary-form **mov** (`o10.x ← r0` clip result, `0x18ab75`) [INF] |
| 0x71 | 2 | componentwise | 4 | **add** [BIN] |
| 0x72 | 2 | componentwise | 4 | used as `T1 = op72(T1, T1.0000)` in kernel tails [OPEN, add/sub variant] |
| 0x73 | 2 | componentwise | 4 | **sub** (`T1.w = op73(T1.w, floor·k)` fraction steps in the fog code; first op of every local-light fragment) [INF, strong] |
| 0x75 | 2 | componentwise | 3 | **mul** [BIN] |
| 0x76 | 3 | componentwise | 3 | **mad** [BIN] |
| 0x77 | 2 | all 4 | 3 | **dp4** [BIN] |
| 0x78 | 2 | xyz | 3 | **dp3** [BIN] |
| 0x79 | 2 | all 4 | 3 | dp4 variant: `r0 = op79(r14, c[plane])` per user clip plane, result to `o10.x` [INF: dp4 + clip flag] |
| 0x7a | 2 | all 4 | 3 | dp4 variant used as `r0 = op7a(T1.x000, T1.1000)` [OPEN] |
| 0x89 | 2 | `s0.w × s1.c` | 1 | **scalar × vector** (`o4.xyz = c5 × T0.w`) [INF, strong] |
| 0x8a | 2 | `s0.w × s1.c` | 1 | scalar × vector, used right after `T.w = dp3(T,T)` to normalise → `s1 × rsq(s0.w)` [INF] |
| 0x8b, 0x8c, 0x8d | 1 | `s.w` only | 1 | scalar transcendental (`T1 = op8b(T1.000#)` on a squared length → rsq/rcp) [INF] |
| 0x91 | 1 | y,z,x read `s.w`; w reads nothing (=1) | 2 | **exp** (D3D expp layout: x=2^floor, y=frac, z=2^x, w=1) [BIN] |
| 0x92 | 1 | same | 2 | **log** (D3D logp layout, z = log2) [BIN] |
| 0x93 | 2 | z ← `s0.y, s1.w`; y ← `s0.x` | 2 | **pow / lit2**: `mpow` uses `.z = s0.y ^ s1.w` [BIN] |
| 0x94 | 2 | z ← `s0.x, s0.y, s1.w`; y ← `s0.x` | 2 | lit with the x>0 clamp [INF] |

Opcodes `< 0x60` are the control class (branch 0x23/0x46, END 0x29, NOP 0x00,
immediates) and are emitted through the NOP template with only the opcode set;
they have no operand fields. [BIN 0x192db2]

### 4.2 Secondary (port-A) opcodes, `w1[12:9]` [BIN table mode 1 + switch]

| op | nsrc | identity |
|---|---|---|
| 0 | — | none |
| 1 | 1 | **mov** (D3D mov, minsert, mextract, mswizzle) |
| 2, 3 | 1 | [OPEN] (class 0) |
| 4 | 1 | **frc** |
| 5 | 1 | **floor** (mfloor; mround = add 0.5 then op 5) |
| 6 | 1 | [OPEN] |
| 7 | 1 | mov variant used for `o10.w ← fog` [INF: saturating/clamped mov] |
| 8 | 0 | writes a destination with no source [OPEN: constant/zero write] |
| 9 | 1 | **mova** (mov to `a0` = latch 1; mindex) |
| 0xa, 0xb | 1 | [OPEN] |
| 0xc | 1 (`.w`) | **rcp** |
| 0xd | 1 (`.w`) | **rsq** |
| 0xe | 1 | **lit** (z reads x,y,w; y reads x) |

The secondary op reads **src2** (`w0[26:20]`, shared swizzle with `w2[22]=1`)
and writes port A. A primary op that itself uses three sources (mad) therefore
cannot be paired with a secondary op that needs a source. [BIN 0x192f22]

---

## 5. D3D vs_1_x opcode → WARP (the switch at 0x19161f) [BIN]

`index = (token & 0xffff) − 1`, table `0x192850[0x63]`, targets `0x1927c8`.
"P" = primary opcode, "S" = secondary opcode.

| D3D op | mnemonic | WARP emission |
|---|---|---|
| 0x01 | mov | S1 (dst a0: S9 mova) |
| 0x02 | add | P 0x71 |
| 0x03 | sub | default case → **nothing emitted** (the pre-pass is expected to lower it; a from-scratch assembler must use add + negate) |
| 0x04 | mad | P 0x76 |
| 0x05 | mul | P 0x75 |
| 0x06 | rcp | S 0xc |
| 0x07 | rsq | S 0xd |
| 0x08 | dp3 | P 0x78 |
| 0x09 | dp4 | P 0x77 |
| 0x0a | min | P 0x61 |
| 0x0b | max | P 0x62 |
| 0x0c | slt | P 0x63 |
| 0x0d | sge | P 0x66 |
| 0x0e / 0x4e | exp / expp | P 0x91 (one instruction; the validation table's "12" is a software cost) |
| 0x0f / 0x4f | log / logp | P 0x92 |
| 0x10 | lit | S 0xe |
| 0x11 | dst | P 0x75 with `src0.swz = (1, y, z, 1) = 0xa8d`, `src1.swz = (1, y, 1, w) = 0x74d` (`0x1916ef`) |
| 0x12 | lrp | default → nothing |
| 0x13 / 0x61 | frc / mfrac | S 4 |
| 0x14 / 0x60 | m4x4 / mmult | 4 × P 0x77, masks x,y,z,w, src1 index +0..3, each emitted only if the dst mask has that bit |
| 0x15 | m4x3 | 3 × P 0x77 |
| 0x16 | m3x4 | 4 × P 0x78 |
| 0x17 | m3x3 | 3 × P 0x78 |
| 0x18 | m3x2 | 2 × P 0x78 |
| 0x55 | mpow | P 0x93 if dst writes .z, else P 0x93 → T1 then S1 `.zzzz` |
| 0x56 | mclamp | P 0x62 max → T0; P 0x61 min |
| 0x57 | mcross | P 0x75 (yzx·zxy) → T; P 0x76 mad with negated product |
| 0x58 | mround | P 0x71 add (constant 0.5 from a c# or T0); S 5 |
| 0x59 | mfloor | S 5 |
| 0x5a / 0x5b | minsert / mextract | S 1 |
| 0x5c | mswizzle | S 1 with the 3-bit codes (`0x190f70`: sel 0-3→x..w, 4→0.0, 5→1.0, 0x10-0x13 → negated mov, 0x20→1.0) |
| 0x5d | mwrmask | nothing |
| 0x5e | mlog | P 0x92 (.z) [+ S1 `.zzzz`] |
| 0x5f | mexp | P 0x91 (.z) [+ S1 `.zzzz`] |
| 0x62 | mindex | S 5 floor → T0; P 0x75; [P 0x71 if const ≥ 0x80]; S 9 mova; 4 × S 1 relative movs |
| 0x63 | mdp3 | P 0x78 |
| any write to `oFog` | — | appended `S 7: o10.w ← T0.(0,0,0,x)` (swizzle 0x124) |

Multi-instruction expansions place the primary and the following secondary op
in **separate** descriptors; the builder (`0x192f60`) then pairs two adjacent
compatible descriptors into one bundle. [BIN]

---

## 6. Two verified programs (assembler output, dword lists)

Both are produced by applying §2.1 to the wra template exactly as
`0x191490`/`0x192da0` do; every field was then re-decoded and cross-checked
against vendor fragments that carry the same operation. [BIN encoding, INF that
the hardware executes the bundle as the vendor's own equivalent fragments do]

### 6.1 Transform + colour pass-through

D3D: `dp4 oPos.x, v0, c0 ; dp4 oPos.y, v0, c1 ; dp4 oPos.z, v0, c2 ;
dp4 oPos.w, v0, c3 ; mov oD0, v1`

What the vendor assembler emits (`oPos → r14`, `oD0 → o1`):

```
00078600 00440003 bb9a21a2   ; dp4  B:r14.x  ← v0.xyzw, c0.xyzw
08078a00 00440003 bb9a21a2   ; dp4  B:r14.y  ← v0.xyzw, c1.xyzw
10079200 00440003 bb9a21a2   ; dp4  B:r14.z  ← v0.xyzw, c2.xyzw
1807a200 00440003 bb9a21a2   ; dp4  B:r14.w  ← v0.xyzw, c3.xyzw
0018021f 00440200 b01a21a2   ; mov  A:o1.xyzw ← v1.xyzw      (secondary op 1)
00000200 00040000 14c00000   ; END
```

Field check of the first word: `w0 = 0x00078600` → `wmask1 = 1`, `dst1 = 0xe`,
`f0bc = 1 (temp)`, `src1 idx = 0`; `w1 = 0x00440003` → `f1a = 3 (const)`,
`src0 = v0` (`f1c = 0`), `extsel = 2`, `f1g = 1`; `w2 = 0xbb9a21a2` →
`opcode 0x77`, `f2b = 1`, both swizzles 0x688. The vendor's own dp4 rows use
the identical `w2` and the identical `w0` shape (`mtx999`: `48048600 2803808f
bb9a21a2` = `dp4 r2.x ← r3, c105`; `mtx1621`: `1874e23f 28022303 bb9a21a2`,
which additionally pairs a secondary `mov o3 ← v7` and reads `c3` relative to
`a0`). [BIN]

For a driver that writes the **output slot directly** (no r14 epilogue),
replace the destinations by `o0` (file 2, index 0):

```
00080600 00440003 bb9a21a2   ; dp4  B:o0.x ← v0, c0
08080a00 00440003 bb9a21a2   ; dp4  B:o0.y ← v0, c1
10081200 00440003 bb9a21a2   ; dp4  B:o0.z ← v0, c2
18082200 00440003 bb9a21a2   ; dp4  B:o0.w ← v0, c3
0018021f 00440200 b01a21a2   ; mov  A:o1   ← v1
00000200 00040000 14c00000   ; END
```

Constants: the matrix rows are read as `c0..c3` = constant-store entries
0..3 → **block load `first = 0x000, count = 4`** (rows in the order the dp4s
read them; D3D convention is row-vector × column-major-uploaded matrix, so
upload the transposed matrix exactly as a D3D application would for
`dp4 ... c0..c3`). The viewport/setup constants the *rasteriser* needs
(`0x160/0x161`, `0x172..0x17e`) are unaffected by this program and still
required (`parhelia-block-state-map.md`). Whether the setup unit accepts a
clip-space `o0` or requires the screen-space post-processing that the vendor's
output tails perform (`mtx1844`/`mtx270`/`mtx3888` → `op8a`, `floor`, then
`o0`) is [OPEN]; the vendor never writes clip space to `o0`.

### 6.2 Pure pass-through

D3D: `mov oPos, v0 ; mov oD0, v1`

Vendor assembler output (`oPos → r14`):

```
000801ef 00440200 b01a21a2   ; mov A:r14 ← v0     (f0a = 1 temp, dst0 = 0xe, src2 = v0)
0018021f 00440200 b01a21a2   ; mov A:o1  ← v1
00000200 00040000 14c00000   ; END
```

Direct-output form (pre-transformed vertices, `o0` written straight):

```
0008020f 00440200 b01a21a2   ; mov A:o0 ← v0
0018021f 00440200 b01a21a2   ; mov A:o1 ← v1
00000200 00040000 14c00000   ; END
```

Two secondary moves cannot share one bundle; to fold both into one word use
the vendor's idiom of a primary `add o0 ← v0 + 0` (as `mtx1661: 38083e00
0044009d b89a2249` = `add o0 ← r7 + r7.0000`) paired with the secondary
`mov o1 ← v1`. All dwords in this section were round-tripped through a decoder
built from §2. [BIN]

Cross-check with the kernel's canned colour fragment `mtx737` instr 2
`0808be00 00440019 b89a2249` = `add B:o2 ← v6 + r1.0000`: `w0[19:18]=2`
(output), `dst1 = 2`, `wmask1 = 0xf`, `src0 = v6` (`w1[8:7]=0`,
`w1[5:2]=6`), `src1 = r1` with the shared swizzle `{w2[9:0]=0x249, w1[31:30]=0}
= 0x924 = (0,0,0,0)`: the vendor adds a zero through swizzle code 4 exactly as
§2 predicts. [BIN]

---

## 7. What the driver epilogue does with `r14` (for completeness) [BIN 0x18a460]

When `ctx+0x40 == 1` (a user shader is bound) the finaliser appends, before
END: a w-sign test `T1 = op64(r14.wwww, 0)`; `r0 = op7a(T1.x000, T1.1000)`;
per enabled user clip plane `i` (bits of `ctx+0x1c`) `r0 = op79(r14, c[base1 + 15 + 12i])`
and `o10.x ← r0` (primary mov 0x6d); optional texture-coordinate generation
(`o3..o8 = r3/r8 × c[base1+0x1f/0x40]`, projected texcoords through
`dp3`/`op8a`); fog (`0x18c477..0x18cb37`: `mul`, `floor`, `max`, `min`,
`op89`, `sub`); point size (`r15.x ← c[base1+0x12].w`, `o9 = mad(r15.x,
c[base1+0x13], r7.xyxy)`, distance attenuation via `dp3`, `op8b`, `max`,
`mul`, `add`). The position divide/viewport step itself is not in MTXPARD's
generator for this mode; the Linux kernel's output-format tails
(`mtx1844`, `mtx2735`, `mtx3398`, `mtx2752`, `mtx270`, `mtx3512`, `mtx3888`,
`mtx3559`, `mtx1661`) are the vendor code that produces `o0`, and they all go
through latch `T1` (`add T1 ← …`, `op8a T1 ← r8, T1`, `min/max` against
`c[base1+19]`, then `add o0 ← r7` / `mov A:o0 ← T1`). Reimplementing that tail
from these fragments is the remaining step for a first hardware triangle.

---

## 8. Corrections to earlier documents

| doc | claim | correction |
|---|---|---|
| `parhelia-vertex-isa.md` §2 | `swzA` = src0, `swzB` = src1 | `w2[21:10]` = src0; `{w2[9:0],w1[31:30]}` = src1 or src2 selected by `w2[22]` |
| §11 | port-A destination is a 9-bit address `{w1[13],w1[26:25],w1[21:20],w0[7:4]}` | port-A destination = file code `w0[9:8]` + 4-bit index; `w1[13]`/`w1[26:25]` are index-register selects |
| §2 | `f0a=2` = port A idle, `f1c=0` = port B idle | file codes; "idle" is mask 0 |
| §6 | 0x76 = "add.a", 0x71 = "add.e" | 0x76 = **mad** (3 sources), 0x71 = add; the `AddAfs/AddEfs` words are `mad o1 ← v6.xyz0, c88, o0` / `add o1 ← o0, v6.xyz0` |
| §6 | "no IR→hardware opcode table exists" | true for a *table*; the mapping is the switch at MTXPARD `0x19161f` (§5) |
| §14 | `w1[12:9]`, `w1[24:22]`, `w1[29:27]`, `w2[22]`, `w2[31]`, `w0[19:18]`, `w0[26:25]`, `w1[1:0]` unknown | all named in §2 |
| `parhelia-warp-binding.md` attempt 5 | generated dp4 "writes a259..a262" | those words write `r3` through port B and `o3..o6` through port A (the paired texcoord moves) |

## 9. Still open

* Exact meaning of primary 0x64/0x65/0x67/0x68/0x69/0x72/0x7a, 0x8b–0x8d
  ordering (rcp/rsq/?), secondary 2/3/6/8/0xa/0xb, swizzle code 6, `w1[21:20]`.
* Values of base registers 1..3 and how they are set (§3.5).
* Whether `o0` may hold clip-space coordinates (§6.1) — decides if the
  from-scratch driver needs the output tail of §7.
* The route from the 24 fetch descriptors (§3.4) to `0x1110..0x1118`/`0x10b8`.
* The `0x196a10` pre-pass cases and the sub/lrp lowering.

All of these are one register-readback experiment each with the
`0x910/0x914/0x938` port described in `parhelia-vertex-isa.md` §11.
