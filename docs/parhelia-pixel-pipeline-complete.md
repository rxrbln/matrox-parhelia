# Parhelia-512 pixel pipeline — the complete register model, from the Windows assembler

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

What the fragment side of a Parhelia-512 (PCI 0x0528) draw needs, end to end:
the ps.1.x → combiner assembler, the texture-stage state → sampler registers,
the fixed-function raster/ROP registers, and three concrete, ordered register
sets (flat, Gouraud, textured). Everything below was read out of binaries;
nothing was taken from vendor C source.

Tags on every claim:

* **[BIN binary offset]** — read out of the instruction stream or a data table.
  `PARD` = `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` (i386 PE, VMA;
  `.text` VMA 0x10300 / file 0x300, `.data` VMA 0x19fb00 / file 0x18fb00, so
  file = VMA − 0x10000 for both), `OGL` = `MtxPar/MTXOGL.dll` (VMA), `KRN` =
  `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (`.text` offset).
  `CAP` = `captures/live-ring-0528/decoded.txt` (vendor kernel's real DMA
  stream, 0x0528 board), `REC` = the ICD→kernel record streams
  `captures/gltri-{flat,gouraud,textured}-0528-trace.txt` (`tools/parhelia-recdec.py`).
* **[INF]** — inferred; the reasoning is given next to it.
* Cross-references to `docs/parhelia-fragment-isa.md` (FI), `-fragment-ops.md`
  (FO), `-texture-layout.md` (TL), `-rendertarget.md` (RT), `-raster-clip.md`
  (RC), `-slot-translation.md` (ST), `-draw-builders-kernel.md` (DBK) are given
  where a field was already established there; this file does not repeat their
  derivations, only their results, and corrects them where the Windows
  binaries disagree.

The single most important structural fact, established here: **MTXPARD keeps
three small hardware-shadow objects for the fragment back end and flushes them
as fixed DMA header packets whose index bytes are exactly the register groups
the Linux kernel writes in the live capture** — so the Windows and Linux stacks
were built from one register description, and the two binaries can be read
against each other field by field.

---

## 0. Register-index namespace

Engine register = `0x1000 + index*4`; DMA packets carry four index bytes per
header dword, LSB first; index `0x21` terminates, `0x20` (`0x1080`) swallows a
data dword. [BIN PARD 0x918ed–0x9192c: `and edi,0xff; lea edi,[edi*4+0x1000]`,
window clamp `0x1000 ≤ reg < 0x1400` at 0x91a42/0x91a4a] [BIN KRN `mtx2434`
0x398e0, see `docs/parhelia-kernel-abi.md` §6.2]. Every header immediate quoted
below decodes under this rule, e.g. `0xbbbcb9ba` = `{0x12e8, 0x12e4, 0x12f0, 0x12ec}`.

MTXPARD has two ways of getting a register out: the `(reg, value)` list
appender `0x170e10` (pushes the full register offset, list capped at 0x12b
pairs) [BIN PARD 0x170e10–0x170e45], used by the combiner and texture engines
and by the internal textured-rectangle path; and hand-built header packets
for the fragment back end (§3.1).

---

## 1. ps.1.x → combiner assembler (MTXPARD)

### 1.1 Token flow

`D3DPS_VERSION(1,3)` is advertised [BIN PARD 0x86c90, `D3DCAPS8+0x570 =
0xFFFF0103`, `MaxPixelShaderValue = 1.0f` at +0x574]. The instruction
dispatcher is `0x188b48` [BIN]:

```
188b48  eax = token & 0xffff
        ≤ 0x09  → jump table 0x188db0 (ALU)
        == 0x12 → LRP handler 0x1866a0
        0x40..0x61 → (op-0x40) jump table 0x188dd8 (texture-address / DEF / PHASE)
        else    → 0x188d52  (reject: RCP, RSQ, EXPP, LOGP, TEXDEPTH, BEM, TEXM3x3DIFF …)
```

ALU handlers, dumped from the table at 0x188db0 [BIN file 0x178db0]:

| D3DSIO | handler | D3DSIO | handler |
|---|---|---|---|
| 0x00 NOP | 0x188b72 (skip) | 0x05 MUL | 0x185d30 |
| 0x01 MOV | 0x184f30 | 0x08 DP3 | 0x1860b0 |
| 0x02 ADD | 0x185190 | 0x09 DP4 | 0x186330 |
| 0x03 SUB | 0x185510 | 0x12 LRP | 0x1866a0 |
| 0x04 MAD | 0x185890 | 0x50 CND / 0x58 CMP | 0x186b40 / 0x186fb0 (via the 0x188dd8 table, entries 0x10 and 0x18) |
| 0x06, 0x07 | 0x188d52 reject | | |

Texture-address handlers (0x188dd8, 25 entries) are as listed in FI §9.5.8
[BIN, re-dumped: 0x187450 TEXCOORD, 0x1874e0 TEXKILL, 0x187570 TEX, 0x187600
TEXBEM, 0x187720 TEXBEML, 0x187840 TEXREG2AR, 0x187960 TEXREG2GB, 0x187a80
TEXM3x2PAD, 0x187bb0 TEXM3x2TEX, 0x187cf0 TEXM3x3PAD, 0x187e20 TEXM3x3TEX,
0x187f70 TEXM3x3SPEC, 0x188130 TEXM3x3VSPEC, 0x188280 DEF, 0x188350
TEXREG2RGB, 0x188470 TEXDP3TEX, 0x188590 TEXDP3, 0x1886b0 TEXM3x3, 0x188800
TEXM3x2DEPTH, 0x188940 PHASE]. Texture-address ops never reach the ALU packer;
they become the 4-entry texture-address descriptor array (FI §9.5), whose
hardware opcode is looked up as `[D3DSIO*4 + 0x1d4388]` with the opcode
clamped to `[0x40, 0x61]` [BIN PARD 0x1790ba–0x1790fb] — i.e. the table's
live entries start at VMA 0x1d4488 (index 0x40), which is why a dump starting
at 0x1d4388 shows unrelated data.

Each ALU handler allocates one **IR record** (10 max, 0x154 bytes each at
`compiler+0x12a4` [BIN PARD 0x1764d0 `mov ebx,0xa`; bound check `cmp
[ebp+0x12a0],0xa` at 0x176cff]). The IR is then lowered to the 36-byte
hardware slot record `{+0 flags, +4 0x1264, +8 0x126c, +0xc 0x1260, +0x10
0x1268, +0x14.. c0/c1}` by one of three packers with identical output format:

| packer | input | table | used for |
|---|---|---|---|
| 0x182190 | ps.1.x IR (D3DSIO opcode + token-decoded operands) | 0x1d6f98 (0x59 rows) | D3D ps_1_1..1_3 [BIN] |
| 0x183be0 | ps.1.4 IR | 0x1d6740 | D3D ps_1_4 [BIN] |
| 0x182390 | GL sub-instruction record (0x34 bytes, §2.2) | 0x1d77f0 (11 rows) | the MTXOGL/libGL fragment-program blob [BIN] |

### 1.2 Instruction words

`0x1264` (RGB source word; `0x1260` is the identically laid-out alpha source
word) [BIN PARD 0x182190 packer: 0x182295 `and edx,0xffffc01f` A-field, 0x1822eb
`and ecx,0xff803fff` B-field, 0x182349 `and eax,0x7fffff` C-field; GL packer
0x1824f4/0x1825b7/0x18265f use the same shifts 5/14/23]:

```
bits  4:0   slot address (merged at upload: 0x1707b3 XOR-merge, mask 0xffffc01f)
bits  9:5   src A register code       bits 13:10  src A modifier
bits 18:14  src B register code       bits 22:19  src B modifier
bits 27:23  src C register code       bits 31:28  src C modifier
```

`0x126c` (RGB destination/opcode word; `0x1268` the alpha twin) [BIN PARD
0x18224b `and ecx,0xfe01ffff; or (((sat&1)<<4 | dst&0xf)<<3 | scale&7)<<17`;
0x1822a8/0x182306/0x18235c replicate fields `&3`, `<<3 &0x18`, `<<6 &0xc0`;
KRN `mtx1653` 0x6b204–0x6b237 writes the same byte offsets]:

```
bits  2:0   src A replicate    bits 5:3 src B replicate    bits 8:6 src C replicate
bits 14:9   opcode (§1.3)      bits 15,16 preserved, never written
bits 19:17  result scale (signed 3-bit shift: 0 x1, 1 x2, 2 x4, 3 x8, 7 /2, 6 /4)
bits 23:20  destination register code
bit  24     saturate
bits 31:25  preserved, never written
```

Register codes (table `0x1d7a78`, indexed `regType*16 + regNum`, and its
inverse `0x1d7978`, both re-dumped [BIN file 0x1c7a78 / 0x1c7978]):

| code | register | D3D type/num |
|---|---|---|
| 0x00–0x03 | t0–t3 (texture) | type 3, 0–3 |
| 0x04 | r0 = temp0 = **program result** | type 0, 0 |
| 0x05 | r1 | type 0, 1 |
| 0x0d | "no destination" sentinel [BIN PARD 0x181fc7] | — |
| 0x0e / 0x0f | c0 / c1 (per-slot constants) | type 2, allocated per instruction by 0x180770 (max 2, `cmp ecx,2`) |
| 0x10 / 0x11 | v0 diffuse / v1 specular | type 1, 0–1 |

Source modifier table `0x1d78f8` = `{0,1,2,3,4,5,0xf,0,…}` indexed by the D3D
`D3DSPSM` code [BIN file 0x1c78f8]: `0 x`, `1 −x`, `2 x−0.5`, `3 −(x−0.5)`,
`4 2x−1`, `5 −(2x−1)`, `0xf 1−x`. `_bias`/`_bx2` on a **texture** source is not
done in the ALU: the post-pass 0x1816f0 clears bit 4 of that texture unit's
state (`and [unitState],0xffffffef`) when the source kind is "texture" and the
modifier is neither 0 nor 1 [BIN PARD 0x1816f0–0x181764] — the hardware
sampler then delivers a signed/biased texel (GL `GL_TEX_SIGNED/BIASED_MTX`).

Result-shift table `0x1d7938` = `{0,1,2,3,0,…,6,7}` indexed by the D3D
destination-shift nibble [BIN file 0x1c7938]: `_x2 → 1`, `_x4 → 2`, `_x8 → 3`,
`_d2 → 7`, `_d4 → 6` (the last two sit at table index 0xe/0xf where D3D encodes
−2/−1). Saturate = `dstToken & D3DSPDM_SATURATE != 0` [BIN PARD 0x181fa7 `setne`].

Replicate, ps.1.x packer [BIN PARD 0x18212d–0x182182]: swizzle byte `0x00 → 3
(.r)`, `0x55 → 2 (.g)`, `0xaa → 1 (.b)`, anything else (`0xe4`, `0xff`) → 0.
The alpha-replicate code **4** is written only by the ps.1.4 packer
(`0x126c[2:0] = (alphaFlag&1)<<2 | ch&3` [BIN PARD 0x183cfa]) and by the GL
path (record replicate field 4 for `GL_SRC_ALPHA`, FI §12.2.6); in ps.1.x the
`.a` replicate on a *constant* source is folded before packing (0x182029:
swizzle `0xe4` on a constant is rewritten to `0xff`) [BIN].

### 1.3 Opcode → encoding table

Read verbatim out of `.data` [BIN file 0x1c6f98 (ps.1.x), 0x1c77f0 (GL/IR)],
24 bytes per row `{0x1264 base, A←, B←, C←, 0x126c base, dest gate}`; 7 = slot
unused. Hardware operand roles: **A·C multiply, A+B add, A predicate**
(FI §7.4, confirmed by the liveness decoder 0x182c60: B live if opcode bit 9|11,
C live if bit 10 clear, all three for `(op & 0x7000) >= 0x4000`).

| ps.1.x mnemonic | D3DSIO | A ← | B ← | C ← | `0x126c[14:9]` | function |
|---|---|---|---|---|---|---|
| nop / pad | 0x00 | – | – | – | `0x400` (with `0x1264=0x80`: `MOV r0,r0`) | identity pad |
| mov d, s0 | 0x01 | s0 | – | – | `0x0400` | A |
| add d, s0, s1 | 0x02 | s0 | s1 | – | `0x0c00` | A + B |
| sub d, s0, s1 | 0x03 | s0 | s1 | – | `0x0600` | A − B |
| mad d, s0, s1, s2 | 0x04 | s1 | s2 | s0 | `0x0800` | A·C + B |
| mul d, s0, s1 | 0x05 | s0 | – | s1 | `0x0000` | A·C |
| dp3 d, s0, s1 | 0x08 | s0 | – | s1 | `0x1000` | dot3(A,C) — the ps.1.x row has 7,7,7 (defect); use the ps.1.4/GL mapping |
| dp4 d, s0, s1 | 0x09 | s0 | – | s1 | `0x2000` | dot4(A,C) |
| lrp d, s0, s1, s2 | 0x12 | s1 | s2 | s0 | `0x0a00` | A·C + B·(1−C) = s1·s0 + s2·(1−s0) |
| cnd d, s0, s1, s2 | 0x50 | s0 | s1 | s2 | `0x4c00` | A > 0.5 ? B : C |
| cmp d, s0, s1, s2 | 0x58 | s0 | s1 | s2 | `0x7c00` | A ≥ 0 ? B : C |
| (GL only) cmv | – | s0 | s1 | s2 | `0x5c00` | conditional move (X driver emits it) |

GL/IR opcode numbering used by `0x182390` and by the MTXOGL/libGL record:
`0 MOV 1 ADD 2 SUB 3 MUL 4 DP3 5 DP4 6 MAD 7 LRP 8 CND 9 CMV 10 CMP` [BIN file
0x1c77f0, identical to KRN `mtx1291`].

Worked encodings (values that appear in the binaries and on the wire):

| instruction | `0x1264` | `0x126c` | where |
|---|---|---|---|
| `MOV r0, v0` (slot 0, sat) | `0x00000200` | `0x01400400` | CAP flat draw |
| `MUL r0, t0, v0` (slot 0, sat) | `0x08000000` | `0x01400000` | REC textured (record `MUL t0,v0`, §5.3) |
| `MAD r0, t0, c1, c0` | `0x0703c000` | `0x00400800` | PARD 0xb2799 |
| `MAD_x2 r0, t0, c1, r0` (slot 4) | `0x0203c004` | `0x00420800` | PARD 0xb28fb |
| `ADD r0, t0, c0` | `0x00038000` | `0x00400c00` | PARD 0xb264f |
| pad `MOV r0, r0` (slot n) | `0x00000080 \| n` | `0x00400400` | CAP slots 2,4,8,10 |

### 1.4 Chaining across slots, co-issue, padding, constants

* **One ps instruction = one slot, in program order.** Instruction *i* goes to
  hardware slot `{0,2,4,8,10, 1,3,5,9,11}[i]` [BIN PARD 0x170980 emitter,
  KRN 0x692df]; temporaries are the real registers r0/r1 (codes 4/5), so a
  chain like `mul r1,t0,v0 / add r0,r1,t1` needs no renaming: slot 0 writes
  code 5, slot 2 reads code 5. The result the blender sees is **r0 of the last
  slot that wrote it** [INF from r0 = output code 4 and the absence of any
  "output slot" field].
* Count is padded to 5 or 10 (`>10 → 10; ≤5 → 5; else 10`) [BIN PARD 0x1708d0],
  pad slots = `0x1264 = 0x80|slot, 0x126c = 0x00400400`; more than five
  instructions sets pipeline bit `0x80000` [BIN KRN 0x2576b].
* **Co-issue.** A slot carries an RGB pair and an alpha pair. `D3DSI_COISSUE`
  is never tested [BIN, scan]; instead the IR merges an `.rgb`-masked and an
  `.a`-masked instruction into one record, flag bit 0 = "separate alpha
  present" selects whether `0x1260/0x1268` are emitted. When absent, the RGB
  words are reused with the alpha-replicate bits cleared (`and eax,0xfffffedb`
  [BIN PARD 0x170abc]); the kernel simply discards them into DMAPAD (header
  `0x20209b99` vs `0x9a989b99`) [BIN KRN 0x69250/0x6933e].
* Constants: c0 → `0x1278/0x127c`, c1 → `0x1280/0x1284`, each register two
  signed Q1.10 values at [10:0] and [26:16]; `v = trunc(f*1024)` clamped to
  [−1024, 1023] [BIN KRN 0x6e4d6; PARD 0xb2390 `round(v*(1<<0xa))`]. Flag bits
  1/2 gate the two uploads [BIN PARD 0x1707b0].
* Emission per slot (D3D and X): `0x1264, 0x126c [, 0x1260, 0x1268] [, 0x1278,
  0x127c, 0x1280, 0x1284]` [BIN PARD 0x1707b0]. On the wire (kernel) the first
  slot is `{0x1264,0x1260,0x126c,0x1268,0x1278,0x127c,0x1280,0x1284}` and pads
  are `{0x1264,0x126c}` pairs [CAP lines 360–361].

---

## 2. Texture-stage state → registers

### 2.1 D3D texture-stage state (MTXPARD, `SetTextureStageState` 0xaee40)

Per-stage block = 0x9c bytes (`stage*0x9c + 0xedc` in the device object)
[BIN PARD 0x16a987, 0x16b01b]. Dispatch `jmp [ (TSS-1)*4 + 0xaf46c ]`, 28 entries
[BIN file 0x9f46c], decoded:

| D3DTSS | block field | dirty bit in block[0] |
|---|---|---|
| 1 COLOROP / 26 COLORARG0 / 2 COLORARG1 / 3 COLORARG2 | +0x2c / +0x30 / +0x34 / +0x38 | 0x1000 |
| 4 ALPHAOP / 27 ALPHAARG0 / 5 ALPHAARG1 / 6 ALPHAARG2 / 28 RESULTARG | +0x3c / +0x40 / +0x44 / +0x48 / +0x4c | 0x1000 |
| 7–10 BUMPENVMAT00/01/10/11 | +0x60..+0x6c (float) | 0x2000 |
| 22 BUMPENVLSCALE / 23 BUMPENVLOFFSET | +0x70 / +0x74 (float, never read) | 0x2000 |
| 11 TEXCOORDINDEX | +0x10 [2:0] index, [4:3] TCI flags (`value>>13`) | 0x4000 / 0x80000 |
| 12 ADDRESS (both) / 13 ADDRESSU / 14 ADDRESSV / 25 ADDRESSW | +0x10 [7:5] & [10:8] / [7:5] / [10:8] / [13:11] | 0x8000 |
| 15 BORDERCOLOR | +0x14 | 0x10000 |
| 16 MAGFILTER / 17 MINFILTER / 18 MIPFILTER | +0x10 [16:14] / [19:17] / [22:20] (old DDI values normalised through 0x1d1f8c `{0,1,2,4,5,3}`, 0x1d1fa4 `{0,1,2,3}`, 0x1d1fb4 `{0,0,1,2}`) | 0x20000 |
| 19 MIPMAPLODBIAS / 20 MAXMIPLEVEL | +0x18 (float) / +0x1c | 0x40000 |
| 21 MAXANISOTROPY | +0x10 [24:23] | 0x20000 |
| 24 TEXTURETRANSFORMFLAGS | +0x10 [27:25] count, bit 28 = PROJECTED (`value & 0x100`) | 0x80000 |

The stage unpacker 0xac180/0xac240 expands the packed word into the compiler's
per-stage descriptor and translates the D3D filter enums [BIN PARD 0xac25d–0xac294,
tables at file 0x1c1f44]:

| D3DTEXF | MAG 0x1d1f44 | MIN 0x1d1f5c | MIP 0x1d1f74 |
|---|---|---|---|
| 0 NONE | 1 | 1 | 1 |
| 1 POINT | 1 | 1 | 2 |
| 2 LINEAR | 2 | 2 | 3 |
| 3 ANISOTROPIC | 5 | 3 | 1 |
| 4 FLATCUBIC | 3 | 1 | 1 |
| 5 GAUSSIANCUBIC | 4 | 1 | 1 |

These are the *internal* filter ids; the join to the hardware `0x11c4` fields is
the kernel's hint tables (§2.3).

### 2.2 The MTXOGL side (texenv)

`glTexEnv` in MTXOGL is the same code as the Linux ICD: the setter at
`OGL 0x103ae130` accepts `GL_TEXTURE_ENV (0x2300)` and `GL_TEXTURE_FILTER_CONTROL
(0x8500)`, keeps the per-unit state at `ctx + unit*0x8c8 + 0x8b8` with
`TEXTURE_ENV_MODE` in bits [2:0] as `MODULATE 1, DECAL 2, BLEND 3, ADD 4,
REPLACE 5, COMBINE 6` [BIN OGL 0x103ae1c4–0x103ae32b], `COMBINE_RGB` as
`REPLACE 1, MODULATE 2, ADD 3, ADD_SIGNED 4, SUBTRACT 5, INTERPOLATE 6, DOT3_RGB
7 …` [BIN OGL 0x103ae3d2–0x103ae48f], and a second packed copy of the mode at
`+0x8c0[13:11]` (`REPLACE 5, BLEND 3, ADD 2, MODULATE 1, DECAL/COMBINE 0`)
[BIN OGL 0x103ae33e]. The uploaded program is the same 0x664-byte blob of
0x94-byte instruction records (`imul …,0x94` at OGL 0x1030f84f–0x10312684,
`[+0x5cc]` hints-valid byte at OGL 0x1004d7e4, `+0x678`-stride record at OGL
0x100041f6) [BIN]. MTXPARD packs that blob with `0x182390` (§1.1), so the
Windows GL lowering is bit-identical to FI §12.2 — one instruction per enabled
unit, destination r0, PREV = v0 for the first unit then r0:

| GL_TEXTURE_ENV_MODE | RGB | alpha |
|---|---|---|
| REPLACE | `MOV r0, tN` | separate `MOV r0.a, PREV` (RGB tex) / co-issue (RGBA tex) |
| MODULATE | `MUL r0, tN, PREV` co-issued (all but GL_ALPHA textures) | — |
| DECAL | `LRP r0, tN, PREV, tN.a` | `MOV r0.a, PREV` |
| BLEND | `LRP r0, c0(envcolour), PREV, tN` | `MUL r0.a, tN, PREV` |
| ADD | `ADD r0, tN, PREV` | `MUL r0.a, tN, PREV` |
| COMBINE | per §1.3 table, operands `GL_TEXTURE → unit`, `GL_CONSTANT → c0`, `GL_PRIMARY_COLOR → v0`, `GL_PREVIOUS → r0/v0`; `ONE_MINUS_* → mod 0xf`, `*_ALPHA → rep 4` | same |

The GL sub-instruction record consumed by `0x182390` [BIN PARD 0x1823b2–0x1826a6]:
`+0 opcode, +4 dst code, +8 scale, +0xc sat, +0x10/+0x14/+0x18 src0 code/mod/rep,
+0x1c.. src1, +0x28.. src2` (0xff = unused). The packer looks the destination
and each source up in the inverse table (`0x1d7978`, type 3 num < 4 ⇒ texture
unit state pointer, `|= 8` = "unit written/read by the program") — this is the
only place a program *binds* a texture unit: reading `tN` marks unit N.

### 2.3 Sampler window `0x1200+4u`, `0x11c0..0x11e8`

Emission order per unit (both stacks): `0x1200+4u` first, then `0x11c0, 0x11c4,
0x11c8, 0x11cc, 0x11d0, 0x11d4, 0x11d8, 0x11dc, 0x11e0, 0x11e4` from a 0x48-byte
struct at `engine+8+u*0x48` [BIN PARD 0x16fe50–0x16ff3a; KRN `mtx792` 0x69790
headers `0x72717000/0x76757473/0x20797877`]. A unit whose `0x1200[18:16] == 1`
gets only the `0x1200+4u` write [BIN PARD 0x16fe5d `cmp ecx,0x10000`].

**`0x1200 + 4u` — unit control** [BIN PARD 0x1052e7–0x105307, 0x15a372–0x15a380,
0x165360–0x1653fc]:

```
[3:0]   one-hot texture-pipe select (1<<pipe; pipe = tbl0x1d3ac0[count*4+u] = u)
[11:8]  0x1 on every enabled path (the literal 0x40101 / 0x40100)
[18:16] 1 = unit disabled, 4 = enabled 2D (0x40000); bit 16 = projective sub-flag
        (errata path 0x2c900 rewrites 5→4, 7→6)
[23:19] 5-bit code that the errata scan 0x2cea0 tests for {2,3,9,0xa,0xe}
        (these are the hardware texture-address opcodes TEX_COORD, TEXKILL, TEXDOT3,
        TEXM3X3_COORD, TEXM3X2_DEPTH — FI §9.5.7) [INF that this field is the TA opcode]
[24],[28:27] writable (mask 0xe404f1f1), unknown
```
Values: enabled 2D `0x00040101`; idle units 1–3 `0x00010002/4/8`; "no texture
bound" written by the kernel `0x1200 = 0x0001000f, 0x1204..0x120c = 0` [CAP line 359].

**`0x11c0` — address modes** [BIN PARD 0x1052f0–0x105339, 0x165401–0x165439]:
`0x1000 | U[1:0] | V[4:3] | W[7:6]`; the internal 3-valued mode maps
`1 → U=V=0b10`, `2 → U=V=0b01`, `0 → 0` (repeat). Which of `0b01`/`0b10` is
CLAMP and which MIRROR is not decided by either binary [INF: D3DTADDRESS_MIRROR=2
and CLAMP=3 are handled by the same packer, so mode 1 ≈ D3DTADDRESS 2 (mirror)
→ 0b10 and mode 2 ≈ clamp → 0b01 is the natural reading; verify on hardware].

**`0x11c4` — filter word**: three 3-bit fields `[2:0]`, `[5:3]`, `[8:6]`, plus
`[10:9]` border-colour enable; every MTXPARD filler writes the literal `0x49`
[BIN PARD 0x105344, 0x15a397, 0x165446]. The field values are the kernel hint
tables `mtx1864/mtx2559/mtx1674 = {0,2,2,3}/{0,2,2,3}/{1,1,2,2}` [BIN KRN
0x70cde–0x70d0d]: **nearest = 0x040 (0,0,1), bilinear = 0x052 (2,2,1),
trilinear = 0x092 (2,2,2), anisotropic = 0x09b (3,3,2)**; field enum `0 nearest,
2 linear, 3 anisotropic`, third field `1 no mip, 2 mip-linear`. Which of the
first two fields is MIN and which MAG is undetermined (the two tables are
identical) [INF: D3D MAG 0x1d1f44 → first field, MIN → second, by table order].

**`0x11c8`, `0x11d4`, `0x11d8`** always 0 [BIN all three fillers].
**`0x11cc` / `0x11d0`** border colour in the texture's channel layout / its
mask (packer 0x84060, TL §8.3); default `0xffffffff / 0` [BIN PARD 0x105381].

**`0x11dc`** texture base, VRAM byte offset (bits [1:0] = memory-space flags)
[BIN PARD 0x1051e4]. **`0x11e0`** = `width_px (aligned 32) [13:0] | tiling[18:16]
| format[29:24]`, **`0x11e4`** = `height[13:0] | depth[27:16] (≥1)` [BIN PARD
0xa40f0, 0xa4091; TL §2]. Format codes: `0x32` ARGB8888 (the only one the GL
stack uses), `0x28` 565/4444, `0x26` 1555, `0x34` 2:10:10:10, `0x22` a 16-bpp
code forced by the internal 16-bit blit filler (`and 0xe2ffffff | 0x22000000`
[BIN PARD 0x165486]). Bits-per-texel by code: TL §4.2. Tiling 0 = linear,
texel at `base + y*align(w,32)*bpt + x*bpt` (TL §1, hardware-proven).

**`0x11e8`** — texture-cache partition per active-stage count, table
0x1d43f0 `[count*4+u]` [BIN file 0x1c43f0]: count 0 → 0; 1,2 → `0x00500000`
×4; 3 → `0x01600000, 0x01600000, 0x00400000, 0x00400000`; 4 → `0x01600000` ×4.
Written once after the unit loop, together with the guard band (§3.7).

**`0x121c`** — written `0` immediately before the unit block by the kernel
[CAP line 358]; MTXPARD has no writer [BIN, immediate scan]. [INF: a
texture-engine reset/latch that must be 0.]

**Surface descriptor words** (`0x1224/0x122c/0x1234/0x123c`, RT §1.2):
`pitch_px[13:0] | type[18:16] | swap[23] | format[29:24] | tiling[31:30]`.
Captured: `0x32020780` = 1920 px, type 2, ARGB8888 (colour target, slot 0 base
`0x02000000`); `0x36020780` = same pitch, format 0x36 on slot 1 (base
`0x01000000`) [CAP line 353] — [INF: the S8Z24 depth surface, code 0x36 in the
32-bpp class]; `0x30030780` type 3 format 0x30 on slot 1 during the clear
blits [CAP line 295]; `0x32020001` (pitch 1) is the priming placeholder.

**Sampler → WARP texcoord binding.** The combiner reads `tN` from unit N; the
unit samples the texcoord set the WARP iterated for it. The set index comes
from the TA record (`+0x08[6:4] = D3DTSS_TEXCOORDINDEX & 7` [BIN PARD 0x17ab19])
and on the vertex side from the `0x10b8` attribute descriptor (§5.4). The
texcoord itself is a WARP port-B output (a `bN`, DBK/`parhelia-warp-fragments.md`
§1) — which `bN` carries texcoord 0 is [INF] b2 (position b0, colour b1,
then texcoords in attribute order).

---

## 3. Raster / ROP state

### 3.1 MTXPARD's three fragment-back-end shadow objects — the register groups

MTXPARD keeps the fragment back end in three small objects, each with a
pointer to a dirty word at `+0` and register images after it, and flushes them
with fixed headers [BIN PARD 0xc9250–0xc9324, 0xc9330–0xc948d, 0xc9160–0xc9246]:

| dirty bit | header | registers ← object fields |
|---|---|---|
| `0x20000000` | `0xbebfcbca` | `0x1328 ← +0x04`, `0x132c ← +0x08`, `0x12fc ← +0x1c`, `0x12f8 ← +0x20` |
| `0x40000000` | `0xbbbcb9ba` | `0x12e8 ← +0x0c`, `0x12e4 ← +0x18`, `0x12f0 ← +0x14`, `0x12ec ← +0x10` |
| `0x04000000` | `0x21b4b3b2` | `0x12c8 ← +0x0c`, `0x12cc ← +0x10`, `0x12d0 ← +0x14` |
| `0x08000000` | `0x2121b1b0` | `0x12c0 ← +0x04`, `0x12c4 ← +0x08` |
| `0x01000000` | `0x21b7b6b5` | `0x12d4 ← +0x04`, `0x12d8 ← +0x08`, `0x12dc ← +0x0c`, then if `+0x04 & 1` and `+0x04 & 6`: a 0x4f block of 16 × (`0xb8b8b8b8`, 4 dwords from `+0x10`) = 64 writes to `0x12e0` (fog table port) |

Those are the exact groups the kernel writes in the capture:
`reglist(6): 0x1328 0x132c 0x12fc 0x12f8`, `reglist(6): 0x12e8 0x12e4 0x12f0 0x12ec`,
`reglist(8): 0x12c8 0x12cc 0x12d0 0x12c0 0x12c4`, `reglist(5): 0x12d4 0x12d8 0x12dc`
[CAP lines 322–325]. The engine-common object (ctor 0xc9490) holds `+0x04
DWGCTL = 0x00360004`, `+0x08 PLNWT = 0xffffffff`, `+0x0c 0x12f4 = 32.0f`, four
surface descriptors `{0, 0x32020000}` at `+0x34`, and `+0x54 = 0x3fff8842`
(`0x3ff00842 | 0xf8000` unless chip cap byte `[dev+0xab50] & 0x10`, then
`& 0xfff07fff`) [BIN PARD 0xc94bb–0xc953f] — all four values are literally in
the capture (`0x1300=00360004`, `0x1324=ffffffff`, `0x12f4=42000000`,
`0x1224=32020000`, `0x12b0=3fff8842`).

The D3D render states feed those objects through the packers in §3.2–3.6. The
SetRenderState dispatcher is `0xad444: movzx edx,[rs-1 + 0xae618]; jmp
[edx*4 + 0xae430]` [BIN]; the handlers store into packed device-object words
(dump of the whole map: ZENABLE→`dev+0xd90`, ZWRITEENABLE→`0xd94`, ZFUNC→`0xd9c`,
STENCILENABLE/FAIL/ZFAIL/PASS/FUNC/REF/MASK/WRITEMASK→`0xda0..0xdbc`,
ALPHATESTENABLE→`0xd1c`, ALPHAREF→`0xd20`, ALPHAFUNC→`0xd24`, ALPHABLENDENABLE→
`0xd28`, BLENDOP→`0xd2c`, SRCBLEND→`0xd30`+`0xd34`, DESTBLEND→`0xd38`+`0xd3c`,
DITHERENABLE→`0xd08`, COLORWRITEENABLE→`0xd0c`, ROP2→`0xd14`, FOG*→`0xe68..0xe80`,
FILLMODE→`0x1f3c[5:4]`, SHADEMODE→`0x1f3c[7:6]`, CULLMODE→`0x1f3c[9:8]`,
ZBIAS/DEPTHBIAS/SLOPESCALEDEPTHBIAS→`0x2104/0x2100` raw floats) [BIN PARD
0xad452–0xae417].

### 3.2 `0x12e8` — blend control

Packer [BIN PARD 0x9c5d3–0x9c795], factor translator `0x9a5f0` (jump table
0x9a7d4, `D3DBLEND-1`) returning `(rgbCode, alphaCode)`, equation translator
`0x9a8f0` (table 0x9a928):

```
bit  0      blend enable (ALPHABLENDENABLE, but forced 0 when the factors are the
            identity src=ONE/dst=ZERO with op ADD/SUB (0x9c5de), and only honoured
            when DWGCTL's BOP nibble is 0xC/COPY (0x9c617))
bits 4:1    src RGB factor        bits 8:5    dst RGB factor
bits 12:9   src alpha factor      bits 16:13  dst alpha factor
bits 19:17  equation              bits 22:20  equation (same value written twice)
bit  23     set when DITHERENABLE, or with bit 24 when dither is off; both clear
            for render-target internal formats {0x0f, 0x10, 0x1e, 0x2f} (0x9c396)
```

Factor codes (D3DBLEND → rgb / alpha-pipe code) [BIN PARD 0x9a616–0x9a7c5]:

| D3DBLEND | rgb | alpha | D3DBLEND | rgb | alpha |
|---|---|---|---|---|---|
| 1 ZERO | 0 | 0 | 9 DESTCOLOR | 8 | 0xa |
| 2 ONE | 1 | 1 | 10 INVDESTCOLOR | 9 | 0xb |
| 3 SRCCOLOR | 4 | 6 | 11 SRCALPHASAT | 0xc | 1 |
| 4 INVSRCCOLOR | 5 | 7 | 12 BOTHSRCALPHA | src 6 / dst 7 (caller 0x9c6db) | |
| 5 SRCALPHA | 6 | 6 | 13 BOTHINVSRCALPHA | src 7 / dst 6 | |
| 6 INVSRCALPHA | 7 | 7 | 14 BLENDFACTOR | 2 | 0xe |
| 7 DESTALPHA | 0xa | 0xa; RT without alpha: rgb 0xa or 1, alpha 1 | 15 INVBLENDFACTOR | 3 | 0xf |
| 8 INVDESTALPHA | 0xb | 0xb; RT without alpha: rgb 0xb or 0, alpha 0 | 16 / 17 | 0xe / 0xf | 0xe / 0xf |

i.e. the hardware factor enum is `0 ZERO 1 ONE 2 CONST_COLOR 3 INV_CONST_COLOR
4 SRC_COLOR 5 INV_SRC_COLOR 6 SRC_ALPHA 7 INV_SRC_ALPHA 8 DST_COLOR 9 INV_DST_COLOR
0xa DST_ALPHA 0xb INV_DST_ALPHA 0xc SRC_ALPHA_SAT 0xe CONST_ALPHA 0xf INV_CONST_ALPHA`
— identical to the kernel's (ST). Equation: `ADD 0, SUBTRACT 1, REVSUBTRACT 2,
MIN 5, MAX 6` [BIN PARD 0x9a901–0x9a921]; for MIN/MAX all four factors are
forced to ONE [BIN PARD 0x9c72a]. Capture: `0x12e8 = 0x00800202` = disabled,
src ONE / dst ZERO on both pipes, ADD, dither on [CAP line 323].

`0x12ec / 0x12f0` — blend-factor colour, four 10-bit values (`float*1023`,
[BIN PARD 0x9c7b0–0x9c7f4, constant 0x19c9b8 = 1023.0]): `0x12ec = (c0<<16)|c1`,
`0x12f0 = (c2<<16)|c3` from the D3D BLENDFACTOR floats at `dev+0xd44..` [INF:
component order R,G,B,A in that struct]. The kernel calls them "blend constant,
10-bit" (ST).

### 3.3 `0x12e4` — alpha test

[BIN PARD 0x9c3e8–0x9c462, compare translator 0x9a980 table 0x9a9d0]:

```
bit  0      enable (ALPHATESTENABLE, or forced when both dev+0xd50 and +0xd54 are set —
            the texture-alpha/colour-key path 0x17bbb0)
bits 3:1    compare function
bits 15:8   reference, 8 bits (ALPHAREF)
bits 31:24  0xff written by MTXPARD (mask); the kernel leaves 0
```
Compare-function code (D3DCMP → hw) = predicate mask **bit0 EQUAL, bit1 LESS,
bit2 GREATER**: `NEVER 0, LESS 2, EQUAL 1, LESSEQUAL 3, GREATER 4, NOTEQUAL 6,
GREATEREQUAL 5, ALWAYS 7` [BIN PARD 0x9a991–0x9a9c6]. Capture `0x12e4 = 0xe`
= off, ALWAYS, ref 0 [CAP]. (The ICD-level 8-bit `round(ref*255+0.5)` of FO §6
lands in bits [15:8].)

### 3.4 `0x12c8 / 0x12cc / 0x12d0` — depth and stencil

[BIN PARD 0x9f072–0x9f417, stencil-op translator 0x9a9f0 table 0x9aa40,
stencil-bits setter 0x9a940]:

```
0x12c8  bit 0      depth test enable (ZENABLE == D3DZB_TRUE; W-buffer path forces func ALWAYS)
        bits 3:1   depth compare function (code as §3.3)
        bit 4      stencil enable
        bits 7:5   stencil compare function
        bits 10:8  stencil fail op    bits 13:11 stencil z-fail op    bits 16:14 stencil pass op
        bits 20:17 stencil bit count (min(n,8); 8 for S8Z24)  — 0x9a940 writes it into
                   the big shadow's +0xb4 and sets the same 0x04000000 dirty bit
0x12cc  bits 7:0   stencil compare mask      bits 15:8  stencil reference
0x12d0  bits 7:0   stencil write mask        bit 8      depth write enable (ZWRITEENABLE)
```
Stencil-op code: `KEEP 0, REPLACE 1, INVERT 2, ZERO 3, INCR(wrap) 4, DECR(wrap) 5,
INCRSAT 6, DECRSAT 7` [BIN PARD 0x9aa01–0x9aa39]. The three byte fields are
shifted right by `[state+8]` = `8 − stencilBits` before storing [BIN PARD
0x9f3ed–0x9f412]; for the 4-bit-stencil format (internal id 0x15) values > 0x7f
are rescaled [BIN PARD 0x9f24c–0x9f3de]. Capture: `0x12c8 = 0x001000e4` =
test off, LESS, stencil off, stencil func ALWAYS, 8 stencil bits; `0x12cc =
0xff` mask 0xff ref 0; `0x12d0 = 0xff` write mask 0xff, depth write 0 [CAP line 324].
Depth-format id → stencil bits: `D16 0, D15S1 1, D24S8 8, D24X8 0, id 0x15 → 7,
0x16 → 1, 0x17 → 4, 0x18 → 2, 0x19 → 3, 0x1a → 5, 0x1b → 6` [BIN PARD 0x9aa60
table 0x9aab8] [INF that the value is a bit count for ids ≥ 0x15].

`0x12c0 / 0x12c4` — written from D3D device fields `0x20b0/0x20b4`
(0xaecd0/0xab2f0/0xab320 set them and raise the `0x08000000` dirty bit); init
values `0xffffffff / 0x7fffffff`, per draw `0x80000000 / 0x7fffffff` [CAP].
[INF: Z-range clip limits (min/max depth as unsigned 32-bit), "disabled" = full range.]

### 3.5 `0x12fc`, `0x12f8`, `0x1328/0x132c`, `0x1324`, `0x1300`

* **`0x1328` = `(ymin<<16)|xmin`, `0x132c` = `(ymax<<16)|xmax`**, clamped to
  `0xffe`/`0xfff` [BIN PARD 0x9d040–0x9d0a4] — settles RC §0's open order
  (min first). DWGCTL bit 23 enables it. Capture: `0x132c = 0x00f00140`
  (240 × 320 window) [CAP].
* **`0x12fc`** — raster mode word, RS object `+0x1c` [BIN PARD 0xa0490–0xa0681]:
  bits [1:0] = cull select (`and eax,0xfffffffc` = cull off; cleared when the
  primitive has no facing), bit 2 (0x4), bit 3 (0x8, set/cleared from a
  device query 0xa05b6), bit 4 (0x10, 0xa058b) [BIN as bits, meanings INF].
  Capture values: `0x18004014` (kernel init / priming) and `0x0bb80024` per draw
  [CAP]; MTXPARD's clear path writes it from a mode bitfield (RT §2.3). Not
  fully decoded: an open driver should replay `0x0bb80024` [INF].
* **`0x12f8`** — `0x007fe000` per draw = the vendor's `reg3DThresholdAA`
  (FO §10 quotes the parameter) [CAP]; `0x02800000` during priming.
* **`0x1324` PLNWT** — bit-plane write mask (`0xffffffff` = all); the D3D
  COLORWRITEENABLE mask is expanded per format into it [BIN PARD 0x113a0b
  emitter, RT §3.1].
* **`0x12b0`** — `0x3fff8842` (§3.1) [BIN], purpose unknown [INF: raster
  setup configuration; replay verbatim].
* **`0x1314 / 0x1318`** FCOL/BCOL = 0 for 3D [CAP].

### 3.6 `0x12d4 / 0x12d8 / 0x12dc` — fog

FOG object [BIN PARD 0x12d5a0–0x12db99]:

```
0x12d4  bit 0      fog enable            bits 2:1  mode (0x2 / 0x4 written from the table/linear
                                          selection at 0x12db21/0x12db49), bit 4 flag (0x12db69)
        bits 13:6  fog colour B          bits 22:15 fog colour G        bits 31:24 fog colour R
        (from FOGCOLOR: `(b<<0 | g<<9)<<6`, `byte3 = r` — 0x12d5e0–0x12d60c)
0x12d8  fog start/scale, 24-bit fixed point (`float * 16777215.0`, const 0x19e278) — 0x12daa9
0x12dc  fog end,   24-bit fixed point;  0x00ffffff when fog is off — 0x12db05
0x12e0  fog-table port: 64 dwords (16 headers 0xb8b8b8b8) when table fog is selected
```
Capture (fog off): `0x12d4 = 0, 0x12d8 = 0, 0x12dc = 0x00ffffff` [CAP line 325].
[INF: the exact meaning of `0x12d8/0x12dc` as start/end vs scale/bias — the
code computes both from FOGSTART/FOGEND with a 1/(end−start) division at
0xaaf50.]

### 3.7 `0x12f4` guard band and `0x11e8`

`0x12f4 = 32.0f` for ≤ 2 active texture stages, `16.0f` for more [BIN PARD
0x16fe0d–0x16fe31, 0x1704f4–0x170509]; a later fixup makes it 31.0/15.0 (RC §6).
Capture: `0x42000000` = 32.0 [CAP]. `0x11e8` per §2.3.

### 3.8 DWGCTL `0x1300` — 3D forms

The 2D field map (opcode [3:0], BOP [7:4], src slot [9:8], dst slot [11:10],
clip enable bit 23) is in `docs/parhelia-2d-accel-complete.md` §4. For 3D,
MTXPARD assembles it as [BIN PARD 0xab910–0xab967]:

```
DWGCTL = base(opcode | BOP<<4 from ROP2 → 0x9a820, = ROP2-1, COPY = 0xC)
       | 0x00860e00                       (constant: bits 9,10,11, 17,18, 23)
       | ((surf >> 25) & 0xf)             (from the render-target descriptor word)
       | (surf & 0x20000000 ? 0x400000 : 0)
       | (cfg & 0x20 ? 0x80000 : 0) | (cfg & 0x40 ? 0x200000 : 0)   (cfg = [ctxobj+0x5c])
```

Observed 3D forms: **`0x00a60ec4`** = opcode 4, BOP COPY, the 0x860e00 constant
(bits 9-11 = "src 2 / dst 3" in 2D terms, [INF] a 3D-mode encoding, not slot
numbers), bit 21 (`cfg & 0x40`), clip enable — the vendor kernel's real draw
[CAP line 356]; `0x000000c4` = the depth clear rectangle (RT §2.3);
`0x001000f4` / `0x005000f5` = the two pipeline-priming pseudo-draws
(opcode 4/5, BOP 0xf, bits 20 / 20+22) [BIN PARD 0x2e832, 0x2ea32; CAP];
`0x00360004` = idle/default (0xc94bb); `0x0707` = soft-trap (2D doc).
Shade mode: D3DSHADEMODE is kept at `0x1f3c[7:6]` and the ICD sends slot 0x09
(1 flat, 2 Gouraud), but no register write derived from it was found in either
binary; the vendor implements GL_FLAT by writing the provoking colour into all
three vertices (REC: the flat vertices carry identical colours) — **the
hardware always interpolates** [INF from that observation].

---

## 4. Enum summary (hardware codes)

| enum | code → meaning | source |
|---|---|---|
| compare (depth/stencil/alpha) | `0 NEVER 1 EQ 2 LT 3 LE 4 GT 5 GE 6 NE 7 ALWAYS` | [BIN PARD 0x9a980]; KRN `mtx1401/mtx1344` (ST) |
| stencil op | `0 KEEP 1 REPLACE 2 INVERT 3 ZERO 4 INCR 5 DECR 6 INCRSAT 7 DECRSAT` | [BIN PARD 0x9a9f0]; KRN (ST) |
| blend factor | §3.2 table | [BIN PARD 0x9a5f0]; KRN (ST) |
| blend equation | `0 ADD 1 SUB 2 REVSUB 5 MIN 6 MAX` | [BIN PARD 0x9a8f0]; KRN (ST) |
| ROP (DWGCTL BOP) | 4-bit truth table, `0xC` COPY | [BIN PARD 0x9a820] |
| cull (ICD slot 0x16) | `1 none 2 CW 3 CCW` (D3DCULL verbatim) | RC §9 |

---

## 5. The three register sets

Conventions: DMA index form is `reg = 0x1000 + idx*4`; "static" registers are
those the capture shows written once at context bring-up and left alone.
Surface bases and pitches are the capture's 1920×1200×32 desktop; substitute
your own. The colour target is descriptor slot 0 (`0x1220/0x1224`), depth
slot 1/2 (§2.3, RT §1.3).

### 5.1 (a) Flat colour triangle — reproduces `captures/live-ring-0528/decoded.txt`

Static context (from the kernel init and the two priming pseudo-draws
`0x1000f4/0x5000f5`, which the kernel replays every frame [CAP lines 226–242]
and which MTXPARD emits identically [BIN PARD 0x2e760/0x2e960]; an open driver
should keep them until proven unnecessary):

```
0x1300 = 0x00000307 ; 0x1300 = 0x00360004        idle DWGCTL                       [CAP 197-198]
0x1328 = 0          ; 0x132c = 0                                                    [CAP]
0x12fc = 0x18004014 ; 0x12f8 = 0x02800000                                          [CAP 179]
0x12e8 = 0 ; 0x12e4 = 0 ; 0x12f0 = 0 ; 0x12ec = 0                                   [CAP 180]
0x12c8 = 0 ; 0x12cc = 0 ; 0x12d0 = 0 ; 0x12c0 = 0x80000000 ; 0x12c4 = 0x7fffffff  [CAP 181]
0x12d4 = 0 ; 0x12d8 = 0 ; 0x12dc = 0x00ffffff                                     [CAP 182]
0x1120..0x112c = 0,0,0,1.0f ; 0x1110 = 0xf27 ; 0x1114 = 0x287210 ; 0x1118 = 0x6543 [CAP 183-184]
0x10b0 = 0x60 ; 0x1000..0x101c, 0x1040..0x104c = 0 ; 0x1064 = 0xffffffff ; 0x1060 = 0 ; 0x10b4 = 0
0x1220 = 0 ; 0x1224 = 0x32020000 ; 0x1230 = 0 ; 0x1234 = 0x32020000 ; slots 1,3 idem
0x1318 = 0 ; 0x1314 = 0 ; 0x1324 = 0xffffffff
0x1300 = 0x707 ; 0x12b0 = 0x3fff8842                                               [CAP 197]
0x1308 = 0 ; 0x1304 = 0 ; 0x130c = 0 ; 0x12f4 = 0x42000000 ; 0x121c = 0 ; 0x1374 = 0
0x1200 = 0x00010001 + full 0x11c0..0x11e4 block (0x11c4 = 0xff000000, 0x11cc = 0xffffffff,
        0x11e0 = 0x32020001, 0x11e4 = 0x00010001, rest 0), same for 0x1204/0x1208/0x120c
        with pipe bits 2/4/8                                                         [CAP 202-213]
5 combiner pads: 0x1264 = 0x80/0x82/0x84/0x88/0x8a, 0x1260 = 0x80, 0x126c = 0x1268 = 0x00400400,
        constants 0                                                                  [CAP 214-218]
priming pseudo-draw ×2: 0x1300 = 0x001000f4 (then 0x005000f5), 0x1328 = 0, 0x132c = 0x10001,
        0x12c8 = 0, 0x12e4 = 1, 0x12fc = 0, 0x1220 = 0, 0x1224 = 0x32020000,
        units 0x1200 = 0x00040101 (0x00050101 on the second) .. 0x120c = 0x00040108,
        each with 0x11dc = 0, 0x11e0 = 0x32020001, 0x11e4 = 0x10001, 0x11c0 = 0, 0x11c4 = 0,
        then the 0x1164..0x1198 vertex planes (a 1-pixel legacy DDA draw)            [CAP 226-242]
```

Per draw, in the captured order [CAP lines 320–361, 363–367]; sources per §3:

```
0x1300 = 0x00000307 ; 0x1300 = 0x000005c0           soft-trap + previous DWGCTL replay
0x1300 = 4 ; 0x1300 = 0x407 ; 0x12bc = fence+1 ; 0x12bc = fence ; 0x1300 = 0x5c0
0x1328 = 0x00000000                                  scissor min (y<<16|x)          §3.5
0x132c = 0x00f00140                                  scissor max 240<<16 | 320       §3.5
0x12fc = 0x0bb80024                                  raster mode                     §3.5
0x12f8 = 0x007fe000                                  AA threshold                    §3.5
0x12e8 = 0x00800202                                  blend off, ONE/ZERO, ADD, dither §3.2
0x12e4 = 0x0000000e                                  alpha test off, ALWAYS, ref 0   §3.3
0x12f0 = 0 ; 0x12ec = 0                              blend constant                  §3.2
0x12c8 = 0x001000e4                                  ztest off, LESS, stencil off, 8 bits §3.4
0x12cc = 0x000000ff ; 0x12d0 = 0x000000ff            stencil masks, zwrite 0         §3.4
0x12c0 = 0x80000000 ; 0x12c4 = 0x7fffffff            z limits                        §3.4
0x12d4 = 0 ; 0x12d8 = 0 ; 0x12dc = 0x00ffffff        fog off                         §3.6
0x1000 = 0 ; 0x1040 = 4 ; 0x10b0 = 0x0002305c        vertex-fetch preamble, triangles (DBK)
0x10b4 = 0 ; 0x10b8 = 0x83c00000                     attribute port reset (DBK)
0x1374 = 0 ; 0x10f0 = 0x20000028 ×2 ; 0x1100 = entry  WARP bind (parhelia-warp-binding.md)
   …block loads 0x41…, 0x1130 = 0x0700300c / 0x0200300c, 0x1120..0x112c, 0x1110 = 3,
   0x1114 = 0x287210, 0x1118 = 0x6543, stage entries 0x10c0..0x10d8, 0x111c = 0xfdb
0x10b0 = 0x0006305c
0x1000 = 2 ; 0x1004..0x101c = 0 ; 0x1040 = 4 ; 0x1044..0x104c = 0
0x1064 = 0 ; 0x1060 = 0x000280cc                     vertex-stream config (DBK)
0x10b4 = 0
0x10b8 = 0x20c00000 ; 0x10b8 = 0xa1c50020 ; 0x10b8 = 0 ; 0x10b8 = 0   attribute descriptors §5.4
0x1220 = 0x02000000 ; 0x1224 = 0x32020780            colour target (slot 0)         §2.3
0x1230 = 0 ; 0x1234 = 0x32020000                     slot 2 unbound
0x1228 = 0x01000000 ; 0x122c = 0x36020780            slot 1 = depth surface [INF]   §2.3
0x1324 = 0xffffffff                                  PLNWT
0x1300 = 0x707 ; 0x12bc = fence pair ; 0x12b0 = 0x3fff8842
0x1300 = 0x00a60ec4                                  3D DWGCTL                       §3.8
0x12f4 = 0x42000000                                  guard band 32.0                 §3.7
0x121c = 0 ; 0x1374 = 0
0x1200 = 0x0001000f ; 0x1204 = 0 ; 0x1208 = 0 ; 0x120c = 0    no texture           §2.3
0x1264 = 0x00000200 ; 0x1260 = 0x00000200            slot 0: MOV r0, v0 (rgb + alpha) §1.3
0x126c = 0x01400400 ; 0x1268 = 0x01400400            dst r0, saturate
0x1278 = 0x127c = 0x1280 = 0x1284 = 0                c0, c1
0x1264 = 0x82 ; 0x126c = 0x00400400  (×4 for slots 2,4,8,10)   pads
0x1000 = 3 ; 0x1040 = 4
DRAW packet 0x97080001 <start> <end>                 (parhelia-draw-recipe.md, DBK)
0x1094 = 2 ; 0x1090 = write-back address             fence
0x1300 = 0x307 ; 0x12bc pair ; 0x1300 = 0x00a60ec4   post-draw replay
```

Vertex: `{x, y, r, g, b}` floats, 20-byte stride, colour replicated in all three
vertices [REC flat]; the WARP program is the kernel's generated kind-3 program
(entry `0x08a57183` etc. in the capture; `docs/parhelia-warp-binding.md`).

### 5.2 (b) Gouraud

**Identical register set to (a).** [REC: the Gouraud and flat record streams
differ in exactly one slot, `key 0x0c id 0x09` (1 → absent/2), the key-0x46
program object is the same `MOV r0, v0`, and the key-0x05 WARP bind is the same
serial (`(0x13,1)(0x14,2)`); the vertex ring differs only in the colour values
`(1,0,0),(0,1,0),(0,0,1)` — `docs/parhelia-shading-texturing-capture.md`.]

What must be true for the colour to interpolate:

* Vertex `{x, y, r, g, b}`, stride 20; the ICD supplies no alpha and no z.
* Attribute descriptors on the `0x10b8` port [BIN KRN `mtx2714` 0xb6f0+0xbb, DBK]:
  `d = stream[2:0] | dwordOffset[11:4] | A[16:12] | B[19:17] | C[25:22] | D[29:27] | last[31]`.
  Position = `0x20c00000` (offset 0, A=0, C=0b0011 = xy). **Colour =
  `0xa1c50020`** = offset 2 dwords, A=5, B=2, C=0b0111 (three components),
  last. So the colour attribute is declared as slot A=5, type B=2, 3 comps.
* WARP output: the setup unit reads position from port-B register b0 and the
  iterated primary colour (what the combiner sees as `v0`, code 0x10) from **b1**
  [INF: b0 = position is established in `parhelia-warp-output-b0` (memory) and
  the `bN = rN` port-B file in `parhelia-warp-fragments.md` §1; b1 as the
  diffuse colour is the next slot and is what the flat/Gouraud program's colour
  move lands in]. The generated program copies attribute A=5 to b1 as
  `(r, g, b, 1.0)` — the default attribute vector `0x1120..0x112c = 0,0,0,1.0f`
  supplies the missing alpha [CAP line 342; INF for the role].
* Combiner: `MOV r0, v0` (slot 0, `0x1264 = 0x200`, `0x126c = 0x01400400`), alpha
  pair identical; blend/alpha test off. With `glColor4f` alpha the vertex grows
  to 28 bytes and the descriptor's C becomes 0b1111 (FO §8.5 stride
  observation; C-mask reading is DBK's [INF]).

### 5.3 (c) Textured — one 2D RGBA8 texture, GL_MODULATE

Cross-checked against `captures/gltri-textured-0528-trace.txt` [REC]: vertex
`{x, y, r, g, b, s, t}` stride 28 (`key 0x08 +4 = 0x1c`), pipeline key
`0x00020010` (bit 17 = texcoord present), texture 64×64 uploaded linear ARGB8888
(`A<<24|R<<16|G<<8|B`, pitch 256 B) into the 4 MB heap, `key 0x0e` unit 0
fields `0x00 = 4` and `0x18 = 2`, `key 0x06` unit 0 field 6 = 1000, program
object item 375 (`TA[0].opcode`) = 1, combiner block 0 = `MUL t0, v0` (dword 0
= 3 (MUL), src0 code 0 (t0), src1 code 0x10 (v0)).

Register set = (a) with these replacements/additions (order as in (a)):

```
; texture unit 0 (after 0x121c = 0), replacing the 0x1200 = 0x1000f line
0x1200 = 0x00040101                     enabled 2D, pipe 0, const 0x100        §2.3 [BIN PARD 0x15a37a]
0x11c0 = 0x00001000                     wrap U/V/W = repeat                    §2.3 [BIN]
0x11c4 = 0x00000040                     nearest (0,0,no-mip); 0x52 bilinear, 0x92 trilinear
                                        (0x49 = MTXPARD literal)               §2.3 [BIN KRN tables]
0x11c8 = 0
0x11cc = 0xffffffff ; 0x11d0 = 0        border colour / mask defaults          [BIN PARD 0x105381]
0x11d4 = 0 ; 0x11d8 = 0
0x11dc = <texture VRAM byte offset>     level 0, 4-byte aligned                [BIN PARD 0x1051e4; TL]
0x11e0 = 0x32000040                     width 64 px, tiling 0, format 0x32     §2.3 [BIN PARD 0xa40f0; TL]
0x11e4 = 0x00010040                     height 64, depth 1                     §2.3 [BIN PARD 0xa4091]
0x1204 = 0x00010002 ; 0x1208 = 0x00010004 ; 0x120c = 0x00010008   units 1-3 idle [BIN PARD 0x15a2f9-0x15a322]
0x11e8 = 0x00500000                     cache partition, 1 active stage        [BIN file 0x1c43f0]
0x12f4 = 0x42000000                     ≤ 2 stages → 32.0                      §3.7
; combiner slot 0
0x1264 = 0x08000000                     A = t0 (code 0 <<5), C = v0 (0x10 <<23), slot 0
0x1260 = 0x08000000                     alpha pair co-issued (same words, rep bits 0)
0x126c = 0x01400000                     MUL (0x0000), dst r0, saturate
0x1268 = 0x01400000
0x1278..0x1284 = 0
; pads for slots 2,4,8,10 unchanged
```

Texture-address side: TA descriptor 0 = `{opcode 1 (GL_OP_TEX → hw 0), fetch=1,
nargs-1 = 0}` [REC item 375 = 1; FI §9.5]; no engine register for it has been
found in either binary (FI §9.5.7) — the unit being *enabled* (`0x1200[18:16] = 4`)
and *read by the program* (`0x182390` marks the unit state `|= 8`) is all that is
visible on the wire [BIN].

Vertex/WARP side [INF, formula from DBK]: descriptors `0x20c00000` (xy @0),
colour `0x21c50020` (offset 2, A=5, B=2, 3 comps, **not** last), texcoord
`0x80c00050 | A<<12 | B<<17` (offset 5 dwords, 2 comps, last; A/B for a
texcoord set need the ICD side); the WARP program must write the texcoord to the
port-B register the unit samples (b2 [INF]). The ICD did **not** change the WARP
program serial for texturing (`key 0x05` unchanged) [REC], so the texcoord path
is selected by the vertex format words (`0x1110/0x1114/0x1118`, the key-0x07
word) rather than by microcode.

### 5.4 Attribute-descriptor summary (`0x10b8` port)

| vertex | descriptors (`0x10b4 = 0` first) | source |
|---|---|---|
| `[x,y,r,g,b]` | `0x20c00000`, `0xa1c50020`, `0`, `0` | [CAP], [BIN KRN `mtx2714`] |
| `[x,y,z,w,u,v]` (kernel internal) | `0x23c00000`, `0xa0c00040 \| A<<12 \| B<<17` | DBK |
| kernel-internal single xyzw | `0x83c00000` | [BIN KRN `mtx693` 0x43941; PARD 0x13afc4] |

---

## 6. Corrections and open items

* **FO §2 / draw-recipe "published reg" columns**: the ICD `key 0x0c` slot ids
  (`0x07, 0x17, 0x0e, 0x34..0x3b, 0x1b, 0x0f, 0x19, 0x18`) are **not** registers
  `0x101c…`; they resolve to the words above (`0x12c8/0x12cc/0x12d0`,
  `0x12e8`, `0x12e4`). RT §0.1 and ST already said so; this file gives the
  Windows-side proof that the packed words are the same.
* **RC §0 scissor order**: `0x1328` = min corner, `0x132c` = max corner [BIN PARD 0x9d040].
* **FI §11.4** "0x49 is the only filter value": true for MTXPARD's fillers; the
  D3D MAG/MIN/MIP enums are translated to internal ids (§2.1) and MTXPARD hands
  them on in the texture-stage descriptor (0x12c bytes at `compiler+0xc20`),
  not into `0x11c4` in any traced path. The hardware values come from the
  kernel hint tables (§2.3).
* **ST 0x12e8 bit assignment**: confirmed field for field from MTXPARD, with the
  addition of bits 23/24 (dither / format-gated) and the identity-blend and
  BOP=COPY gates on bit 0.
* Open: `0x12fc` bit meanings above bit 4; `0x12b0`; `0x12c0/0x12c4` semantics;
  `0x1200[23:19]`; which `0x11c0` code is clamp vs mirror; which `0x11c4` field is
  MIN vs MAG; the fog `0x12d8/0x12dc` units; the WARP port-B slot numbers for
  colour and texcoord (b1/b2 are inferences).
