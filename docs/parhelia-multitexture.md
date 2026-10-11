# Multitexture on Parhelia-512 — two texture units in one pass

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

What it takes to bind a **second** texture unit and combine the two samples in
one draw — the case ioquake3's OpenGL1 renderer needs for
`diffuse x lightmap` via `GL_ARB_multitexture`. Extends
`docs/parhelia-textured-draw.md` (single unit) and corrects/completes
`docs/parhelia-fragment-isa.md` §11 and §12.

Evidence tags: **[HW]** measured in a live ring or ICD-record capture,
**[BIN]** disassembly with symbol + offset, **[INF]** inferred, **[OPEN]** not
resolved.

Binaries, all clean-room read as machine code only:
`KRN` = `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (unstripped
relocatable, `objdump -dr` names every call target and `.data` table),
`GL` = `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0`,
`HAL` = `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` (has a real
`.dynsym`, so its texture code carries **C++ names**),
`PARD` = `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`.

Captures: `captures/gl-matrix/*-secs.txt` (DMA ring),
`captures/glsweep-0528-prims-formats.txt` (ICD → kernel record stream, carries
the fragment-program blob verbatim), `captures/live-ring-0528/decoded.txt`.

---

## 0. Answers in one page

| question | answer |
|---|---|
| how many texture units | **4** [HW][BIN], three independent binaries and the wire |
| how unit 1 is addressed | `0x11c0`-`0x11e4` is **one windowed register file**; the write to **`0x1200 + unit*4`** selects the bank. Program unit 1 by writing `0x1204` and then the same `0x11dc/0x11e0/0x11e4/0x11c0/0x11c4/...` again [HW] |
| 2-unit vertex | `x,y, r,g,b, s0,t0, s1,t1` — 9 dwords, stride 36 |
| 2-unit draw header | **`0x97100001`** |
| texcoord enables | `0x1110` bit `8+set`; two sets → `| 0x300` [BIN] |
| WARP outputs | set *i* → output `o(3+i)`; `0x1118 = 0x00006543` already says so, unchanged [HW][BIN] |
| diffuse x lightmap | 4 instructions in 5 slots: TA(u0) slot 0, TA(u1) slot 2, `MUL r0,t0,v0` slot 4, `MUL r0,t1,r0` slot 8, pad slot 10 |

The P690 texture-address source selector is now settled by live hardware:
logical unit 1 follows PARD's physical-pipe permutation and selects pipe 2 in
`0x1264/0x1260` — §6.1. Everything else below is either measured or derived
from a named function.

---

## 1. Four units, and how unit 1 is addressed

### 1.1 The sampler window is banked by `0x1200 + unit*4`  **[HW]**

`docs/parhelia-fragment-isa.md` §11.1 called this "INFERRED". It is now
**measured**. `captures/gl-matrix/base-secs.txt`, ring group `1950cd12`, is one
36-dword DMA section that programs all four units back to back:

```
$ tools/decode-gl-secs.py --ring 1950cd12 captures/gl-matrix/base-secs.txt
=== SEC 1950cd12 (36 dw)
   1200 = 00040101      <- unit 0 selected
   11dc = 00000000 / 11e0 = 32020001 / 11e4 = 00010001 / 11c0 = 0 / 11c4 = 0
   1204 = 00040102      <- unit 1 selected
   11dc = 00000000 / 11e0 = 32020001 / 11e4 = 00010001 / 11c0 = 0 / 11c4 = 0
   1208 = 00040104      <- unit 2
   ... same window again ...
   120c = 00040108      <- unit 3
   ... same window again ...
```

`captures/live-ring-0528/decoded.txt:53,54,62,63,89,90,98,99` is the same
pattern from a completely different run (the X server's own 3D init), with unit
type `4` (2D) and `5` (2D projective).

There is exactly **one** address for each of `0x11c0`-`0x11e4` in the whole
engine, and four units are given four independent descriptions through it — so
the write to `0x1200 + unit*4` is the bank selector, in the same DMA packet or
immediately before. There is no `0x11c0 + unit*stride`.

**Three binaries emit exactly that order, and one of them says so by name.**

* `HAL CDPSTextureEngine::UpdateHardware` (`mtx1196` @0x6ffb0) **[BIN]** —
  loops `u = 0..3` over a 0x28-byte software stage struct and appends
  `(register, value)` pairs in this order, per unit:

  ```
  0x1200 + u*4   struct+0x7c, with the low nibble forced to 1<<u
                 (0x6fff2: |= 8 for u==3, 0x70010: |= 1 for u==0,
                  0x70230: |= 2 for u==1, 0x70251: |= 4 for u==2)
  0x11e8         struct+0x80        (only if struct+0x78 != 0)
  0x11c0         struct+0x88
  0x11c4         struct+0x8c
  0x11c8         0
  0x11cc         0xffffffff
  0x11d0 / 0x11d4 / 0x11d8   0
  0x11dc         struct+0x90
  0x11e0         struct+0x94
  0x11e4         struct+0x98
  ```

  then `add $0x28,%rsi ; inc %r8d ; cmp $0x3,%r8d ; jbe` — **four units, whole
  window per unit**.

* `KRN mtx1381` @0x3f680, the emitter the Linux ICD path actually uses
  **[BIN]** — dirty-bit driven, **three bits per unit** (`ecx = count*3;
  ebx = (1<<ecx)-1`), stepping a 0x38-byte per-unit struct at `state+0x1c`:

  ```
  bit0 -> tag {0x1200+u*4, 0x11dc, 0x11e0, 0x11e4}   (0x3f7b9, idx = (0x200+u*4)>>2)
  bit1 -> tag {0x11c0, 0x11c4, 0x11d4, 0x11d8}       (0x3f7fa)
  bit2 -> tag {0x11cc, 0x11d0, 0x11c8, 0x11e8|PAD}   (0x3f7ad / 0x3f826)
  ```

  byte for byte the tags in the textured capture
  (`docs/parhelia-textured-draw.md` §2.1). Per-unit struct:
  `+0x04` = `0x1200+u*4`, `+0x08` = `0x11e8`, `+0x0c` = `0x11c0`,
  `+0x14` = `0x11c4`, `+0x18` = `0x11c8`, `+0x1c` = `0x11cc`, `+0x20` = `0x11d0`,
  `+0x24` = `0x11d4`, `+0x28` = `0x11d8`, `+0x2c` = `0x11dc`, `+0x30` = `0x11e0`,
  `+0x34` = `0x11e4`. Four structs, at `state+0x1c/+0x54/+0x8c/+0xc4`
  (`0x3f8c0`-`0x3f8d5` clears up to `state+0xc4`).

* `KRN mtx792` @0x69790 and `PARD 0x16fe50` — the same window, same order
  (already in `docs/parhelia-fragment-isa.md` §11.1).

**Consequence for a driver:** to bind two textures, emit the block twice.

```
0x121c = 0                                   ordering rule, once, before the units
0x1200 = <unit 0 control>                    selects bank 0
0x11dc/0x11e0/0x11e4 = base / w|fmt / h|1    unit 0 surface
0x11c0/0x11c4        = filter / wrap         unit 0 sampler
0x11cc/0x11d0/0x11c8/0x11d4/0x11d8           unit 0 border etc.
0x1204 = <unit 1 control>                    selects bank 1
0x11dc/0x11e0/0x11e4 = base / w|fmt / h|1    unit 1 surface   <- different values
0x11c0/0x11c4        = filter / wrap         unit 1 sampler   <- may differ
0x11cc/0x11d0/0x11c8/0x11d4/0x11d8           unit 1
0x1208 = 0 ; 0x120c = 0                      units 2, 3 off
```

`0x1200 + unit*4` **must be in the same packet as, or before, the window
writes** — both emitters put it in the same packet [BIN][HW].

### 1.2 The trailing `0x1200..0x120c` packet is table-driven  **[BIN][HW]**

`KRN mtx82` @0x407b0 initialises three tag tables indexed by the **number of
active units**. (For `movl $imm,disp(%rip)` the relocation addend is 8 short of
the datum, so the true table base is `.data+0xc10`, entries every 4 bytes.)

```
tagA[count] : count 0 -> 0x83828180   {0x1200, 0x1204, 0x1208, 0x120c}
              count 1 -> 0x83828120   {DMAPAD, 0x1204, 0x1208, 0x120c}
              count 2 -> 0x83822020   {DMAPAD, DMAPAD, 0x1208, 0x120c}
              count 3 -> 0x83202020
              count 4 -> 0x20202020
```

used at `KRN mtx1381` @0x3faf0 with the four data words
`state[+0x20], [+0x58], [+0x90], [+0xc8]` — i.e. the four units' `0x1200`
values, the first `count` of them swallowed by the pads because the per-unit
loop already emitted them. `tagA[1]` is exactly the trailing packet of the
single-texture capture, `0x1204 = 0x0001000c / 0x1208 = 0 / 0x120c = 0`
**[HW]**.

**So with two active units the tail packet is `tagA[2]`: only `0x1208` and
`0x120c` (both 0).** Two more tables, `tagB[count]` at `.data+0xbf0` and
`tagC[count]` at `.data+0xbd0`, carry the `0x11e8` variant used when
`dev[0x2440] & 0x7fc00` is set; on the 0x0528 that bit is clear and `0x11e8` is
never written (`0x3f753`: the register becomes DMAPAD `0x1080` otherwise)
**[BIN][HW]**.

### 1.3 `0x1200 + unit*4` for two units

Field layout is `docs/parhelia-fragment-isa.md` §11.2, unchanged. Only the
low nibble — the **texture-pipe allocation** — needs a decision, and the
vendors do not agree, which is itself the useful finding:

| source | 1 unit | 2 units | 4 units |
|---|---|---|---|
| Linux ICD **[HW]** | `0x3` (+ a *disabled* unit 1 holding `0xc`) | not captured | `0x1`, `0x2`, `0x4`, `0x8` |
| `HAL` / `KRN mtx398` @0x329d9 **[BIN][HW]** | `0x1` | `0x1`, `0x2` | `0x1`, `0x2`, `0x4`, `0x8` |
| `PARD` table `0x1d3ac0` **[BIN]** (5 rows x 4, `1 << row[count*4+u]`) | `0x1` | `0x1`, `0x4` | `0x1`, `0x2`, `0x4`, `0x8` |

`PARD .data 0x1d3ac0` re-dumped verbatim: `{0,1,2,3}{0,1,2,3}{0,2,1,3}{0,1,3,2}{0,1,2,3}`,
and the matching per-count union `PARD 0x1d3b10 = {0, 0x1000, 0x5000, 0xb000,
0xf000}` (mask << 12) confirms one pipe per unit on Windows.

So the nibble is a **partition of four texture pipes among the active units**;
the union does *not* have to be `0xf` (Windows uses `0x1` alone for one unit).
The Linux ICD's `0x3` + parked `0xc` is one legal partition, `0x1` + `0x2` is
another, both attested on this silicon.

**Use for two units** — mirroring the ICD's own "<= 2 stages" split:

```
0x1200 = 0x00040103     unit 0: [3:0]=0x3 pipes 0+1, [11:8]=1, [18:16]=4 (2D)
0x1204 = 0x0004010c     unit 1: [3:0]=0xc pipes 2+3, [11:8]=1, [18:16]=4
0x1208 = 0x00000000     unit 2 off
0x120c = 0x00000000     unit 3 off
```

**[INF]** for the exact nibbles (`0x1` / `0x2` is the documented fallback,
`HAL`'s own choice for two units); **[HW]** for everything else in the words.

### 1.4 What is *not* constrained

* **Formats and filters are per unit.** The whole surface + sampler window is
  banked, so unit 0 can be `0x32` ARGB8888 bilinear repeat while unit 1 is
  `0x11` L8 nearest clamp. Four different windows in one section, §1.1 [HW].
* **Sizes:** max 2048 in every dimension and power-of-two required
  (`docs/parhelia-gl-limits.md` §*, `GL 0x13df0b`, size check at `GL` "dim <=
  2048 and (dim-1)&dim == 0"). ioquake3's 128x128 lightmaps and POT textures
  are fine.
* **Units:** `GL_MAX_TEXTURE_UNITS_ARB = 4` (`GL 0x13c67b`), D3D
  `MaxTextureBlendStages = MaxSimultaneousTextures = 4` (`PARD .data 0x1d1e50`),
  and the hardware loops in `HAL`/`KRN` are all `u = 0..3` **[BIN]**.
* Extensions the vendor ICD advertises, so all of this is exercised silicon:
  `GL_ARB_multitexture`, `GL_ARB_texture_env_add`, `GL_EXT_texture_env_add`,
  `GL_ARB_texture_env_combine`, `GL_ARB_texture_env_dot3`. No
  `GL_ARB_texture_env_crossbar` — `GL_TEXTUREn` is rejected as a `GL_COMBINE`
  source (`docs/parhelia-fragment-isa.md` §12.2.3).

---

## 2. The vertex and WARP side for two coordinate sets

### 2.1 Layout and draw header

One texcoord set is `x,y, r,g,b, s,t` = 7 dwords, header `0x970c0001`
(`docs/parhelia-textured-draw.md` §4.1, [HW]). The header rule is
`0x97000001 | ((dwordsPerVertex - 1) << 17)`, so:

```
struct vertex { float x, y, r, g, b, s0, t0, s1, t1; };   /* 9 dwords, 36 bytes */

draw packet:  0x97100001   <vtxStartByte>   <vtxEndByte>      (end exclusive)
```

`(9-1) << 17 = 0x00100000` — cross-checks against the two measured headers,
`0x97080001` for 5 dwords and `0x970c0001` for 7 **[HW]**.

### 2.2 The `0x10b8` fetch descriptors  **[HW][BIN]**

Descriptor fields (`KRN mtx2714` @0xb7b1, and §4.3 of the textured-draw doc):
`[2:0]` stream, `[11:4]` offset in **dwords**, `[19:16]` destination WARP input
register, `[25:22]` component mask, `[29:27] = 4` (ICD attribute), `[31]` last.

```
0x10b4 = 0x00000000                      write index
0x10b8 = 0x20c00000    position x,y   dw 0 -> v0    mask xy
0x10b8 = 0x21c50020    colour r,g,b   dw 2 -> v5    mask xyz
0x10b8 = 0x20c70050    texcoord0 s,t  dw 5 -> v7    mask xy    (last flag CLEARED)
0x10b8 = 0xa0c80070    texcoord1 s,t  dw 7 -> v8    mask xy    (last)
0x10b8 = 0x00000000    terminator
```

Only two things change from the single-texture stream: the texcoord-0
descriptor loses bit 31 (`0xa0c70050` -> `0x20c70050`) and a new one is added.
`0xa0c80070` = `last | kind 4 | mask xy | v8 | dword 7`, built by the same
formula that reproduces all three measured descriptors exactly. **[INF]** from
a **[BIN]**-confirmed packer; the vendor's own vertex input register map is
`v0` position, `v5` diffuse, `v7..v10` texcoord 0..3
(`docs/parhelia-warp-programs-decoded.md:200`), so set 1 is `v8`.

### 2.3 `0x1110` — the texcoord-set enables  **[BIN]**, was [INF]

`KRN mtx3400` @0x43c46 emits `0x1110/0x1114/0x1118` in one packet
(tag `0x21464544` = indices 0x44, 0x45, 0x46) with
`0x1110 = state[0x61c] | state[0x608]`.

`KRN mtx2301` @0x87d80 builds `state[0x608]`, and this settles the field:

```
87d4d: edx = state[0x610]              ; the 0x1118 value
87d80: ecx = edx & 0xf                 ; nibble i = WARP output register for set i
87d88: r9d = 1 << ecx
87d8b: test r9d, [ctx+0x7a4]           ; does the WARP program write that output?
87d94: ecx = 8 + i ; edi &= ~(1 << ecx); [ctx+0x3e8] = i+1   ; if not, clear the bit
87dba: edx >>= 4 ; i++                 ; next set
87dc7: eax = (base & ~0x0000f000) | edi
```

and `87e0a` tests `0x1110 & 0x0f00` as one field. So:

**`0x1110` bit `8 + set` enables texture-coordinate set `set`, sets 0..3 in
bits [11:8]**, and the kernel *clears* a set's bit when the bound WARP program
does not write the output register that `0x1118`'s nibble names for it.
A second, independent corroboration: `KRN mtx1495` @0x882b9 tests
`((0x1110 & 0x3c0) >> 8) <= 1`, i.e. "set 1 not enabled".

Measured values that fit: `0x00000003` untextured, `0x00000103` one
`GL_MODULATE` texture, `0x00000101` one `GL_REPLACE` texture (bit 1 = diffuse
drops out because the combiner stops reading `v0`), `0x00000f27` in the X
server's own 3D context = all four sets **[HW]**.

```
two units, GL_MODULATE on both:   0x1110 = 0x00000303
two units, unit 0 GL_REPLACE:     0x1110 = 0x00000301
0x1114 = 0x00287210   unchanged
0x1118 = 0x00006543   unchanged  (nibble i = (i & 7) + 3, KRN mtx3654 @0x8c9e8)
```

`0x1118` already routes unit *i* to WARP output `o(3+i)` for all four units, so
**it does not change when a second unit is enabled** — the same reason it did
not change when the first one was.

### 2.4 The WARP program

Add one pass-through per set. `tools/parhelia-warpasm.py --gen passthrough
--tex 2` already emits it:

```
  0: 00083e00 00440000 b89a2249 | add o0, v0, v0.0000
  1: 08087ebf 00440215 b89a2249 | add o1, v5, r1.0000 | mov o11, v0
  2: 0078023f 00440200 b01a21a2 | mov o3, v7            <- texcoord set 0
  3: 0088024f 00440200 b01a21a2 | mov o4, v8            <- texcoord set 1
  4: 00000200 00040000 14c00000 | end
  5: 00080200 00440000 301a21a2 | wra
```

`--gen transform --tex 2` for the MVP-transforming variant. Program buffer
address must stay a multiple of 12 (`docs/parhelia-minimal-draw-recipe.md`
§3.3). Because §2.3's kernel rule keys `0x1110` off the program's output-write
mask, **`o4` must actually be written or set 1 is silently dropped** — the one
real coupling between the WARP program and multitexture.

---

## 3. The fragment combiner for two units

### 3.1 How the program is laid out

Recap of the settled machinery, all needed below:

* Instruction index -> hardware slot walk
  `0,1,2,3,4,5,6,7,8,9 -> 0,2,4,8,10,1,3,5,9,11`. Re-dumped verbatim from
  `KRN .rodata+0x1ae0` = `{0,2,4,8,10,1,3,5,9,11}` **[BIN]**, read at
  `KRN mtx1381` @0x3f9e5 and `mtx2501` @0x40750.
* Instruction count is padded to **5** (<= 5) or **10**; pad slots are
  `0x1264 = 0x80 | slot`, `0x126c = 0x00400400` (`MOV r0, r0`). The pad template
  is the literal 24-byte entry `KRN mtx3535` = `{0x00000080, 7,7,7,
  0x00400400, 7}` **[BIN]**.
* Live texture-address (TA) stages are emitted **first** and consume
  instruction indices, then the ALU instructions
  (`KRN mtx2132` @0x6e1d0: loop 1 walks `blob+0x5d4 + i*0x24`, skipping
  `desc+0x00 == 0`, and bumps the shared counter `ctx+0x24`; the ALU loop
  starts at 0x6e372) **[BIN]**, and the wire agrees — a one-texture draw emits
  a slot-0 TA word and puts its single ALU instruction in slot **2** **[HW]**.
* Which units get a live TA stage: `GL 0x10da50` — for every source register
  code of every emitted instruction, `if (code <= 3) TA[code].opcode = 1`
  (`movl $0x1,0x5d4(%rdi,%rax,4)` with `rax = code*9`) **[BIN]**. So
  **referencing `t1` in the combiner is what creates unit 1's TA stage.**
* Word packing (`docs/parhelia-fragment-isa.md` §2/§3, table `KRN mtx1291` /
  `PARD 0x1d77f0`):

  ```
  0x1264 = slot | Areg<<5 | Amod<<10 | Breg<<14 | Bmod<<19 | Creg<<23 | Cmod<<28
  0x126c = opcodeBase | Arep | Brep<<3 | Crep<<6 | scale<<17 | dreg<<20 | sat<<24
  opcodeBase: MOV 0x0400  ADD 0x0c00  SUB 0x0600  MUL 0x0000  DP3 0x1000
              DP4 0x2000  MAD 0x0800  LRP 0x0a00  CND 0x4c00  CMV 0x5c00  CMP 0x7c00
  operand map: MOV A<-s0 | ADD/SUB A<-s0,B<-s1 | MUL/DP3/DP4 A<-s0,C<-s1
               MAD A<-s1,B<-s2,C<-s0 | LRP/CND/CMV/CMP A<-s0,B<-s1,C<-s2
  registers:   t0..t3 = 0..3, r0 = 4, r1 = 5, c0 = 0x0e, c1 = 0x0f, v0 = 0x10, v1 = 0x11
  ```

* The **alpha pair is always written** by the emitter the ICD uses:
  `0x1260 = 0x1264 with bits [4:0] cleared` (the slot is merged into `0x1264`
  only, `KRN mtx530` @0x69313) and `0x1268 = 0x126c & ~0x124` (alpha replicate
  has no third bit, `KRN 0x6a3ac`) — or, when the instruction has a *separate*
  alpha op, that op packed the same way. Both forms are on the wire **[HW]**.

### 3.2 Instruction count and the two count-dependent registers

Two units, `GL_MODULATE` on both, RGBA textures:

```
index 0  TA stage, unit 0      -> slot 0
index 1  TA stage, unit 1      -> slot 2
index 2  MUL_sat r0, t0, v0    -> slot 4
index 3  MUL_sat r0, t1, r0    -> slot 8
index 4  pad MOV r0, r0        -> slot 10
```

4 instructions -> padded to 5 -> still <= 5, and still <= 2 texture stages, so
**neither `0x1300` nor `0x12f4` changes**:

```
0x1300 = 0x00a60ec4     bit21 (<= 2 texture stages) set, bit19 (6..10 instrs) clear
0x12f4 = 0x42000000     guard band 32.0f, <= 2 stages
0x11e8 = not written    (dev[0x2440] & 0x7fc00 clear on the 0x0528)
```

(`KRN mtx1389` @0x257bb / @0x2575e for the two `0x1300` bits **[BIN]**;
`0x12f4` from `PARD` and the wire **[BIN][HW]**.) Three or four units would set
`0x1300` bit 21 clear and `0x12f4 = 0x41f80000`.

### 3.3 The diffuse x lightmap program

Chaining rule, from the generator itself (`GL 0x10ddd0` loops units 0..3;
`TEX = [state+0x674] =` the unit index used **directly as the source register
code**; `PREV = [state+0x678] =` `0x10` (`v0`) for the first emitted
instruction and `4` (`r0`) thereafter — `GL 0x10e010 movl $0x4,0x678(%rbp)`)
**[BIN]**, and `GL 0x10d280` (MODULATE) writing
`record[0x00] = 3 (MUL)`, `record[0x10] = TEX`, `record[0x1c] = PREV`,
`record[0x34] = 0` (co-issue) **[BIN]**:

```
unit 0 GL_MODULATE :  MUL r0, t0, v0        ; texture x iterated colour
unit 1 GL_MODULATE :  MUL r0, t1, r0        ; x lightmap
```

Packed, with the slot walk of §3.1:

```
slot 0    texture-address stage, unit 0                        [HW]
   0x1264 = 0x00004000     0x1260 = 0x00004000
   0x126c = 0x00000c00     0x1268 = 0x00000c00
   0x1278 = 0x127c = 0x1280 = 0x1284 = 0

slot 2    texture-address stage, unit 1                        [INF] - see §4.3
   0x1264 = 0x00004002     0x1260 = 0x00004000
   0x126c = 0x00100c00     0x1268 = 0x00100c00
   0x1278 = 0x127c = 0x1280 = 0x1284 = 0

slot 4    MUL_sat r0, t0, v0                                   [HW] value, [INF] slot
   0x1264 = 0x08000004     0x1260 = 0x08000000
   0x126c = 0x01400000     0x1268 = 0x01400000

slot 8    MUL_sat r0, t1, r0                                   [BIN]
   0x1264 = 0x02000028     0x1260 = 0x02000000
   0x126c = 0x01400000     0x1268 = 0x01400000

slot 10   pad MOV r0, r0
   0x1264 = 0x0000008a     0x126c = 0x00400400
```

Derivations: slot-4 word is the measured single-texture `MUL r0, t0, v0`
(`0x08000002` at slot 2) with the slot renumbered — `Areg = t0 = 0`,
`Creg = v0 = 0x10` -> `0x10 << 23 = 0x08000000`. Slot-8 word is
`Areg = t1 = 1` -> `1 << 5 = 0x20`, `Creg = r0 = 4` -> `4 << 23 = 0x02000000`,
`dreg = r0 = 4` -> `0x00400000`, `sat` -> `0x01000000`.

**If the second unit uses `GL_ADD` instead** (ioquake3 does this for some
shader stages): `ADD r0, t1, r0` -> `Areg = t1`, `Breg = r0` ->
`0x1264 = 0x00010020 | slot`, `0x126c = 0x01400c00`.

---

## 4. texenv modes — measured, not guessed

### 4.1 Method

`captures/glsweep-0528-prims-formats.txt` is a live 0x0528 capture whose
**region 2 is the ICD -> kernel record stream**, and it contains the 0x664-byte
fragment-program blob verbatim for five texenv modes
(`e_replace`, `e_decal`, `e_blend`, `e_add`, `e_combine_dot3` — the ops of
`tools/parhelia-texsweep.c`). The blob layout is
`docs/parhelia-fragment-isa.md` §12.2.2: `+0x000` instruction count,
`+0x004` ten 0x94-byte records (`+0x00` RGB sub-instruction, `+0x34` separate-
alpha flag, `+0x38` ALPHA sub-instruction, `+0x6c/0x70` c0, `+0x80/0x84` c1),
`+0x5cc` hints-valid, `+0x5d0 = 4`, `+0x5d4` four 0x24-byte TA descriptors;
sub-instruction `{opcode, dst, scale, sat, 3 x (code, mod, replicate)}`.

Each blob was decoded and re-packed with §3.1's rules. **The method validates
itself**: `e_replace` re-packs to `0x1264 = 0x00000002`,
`0x126c = 0x01400400`, byte for byte the values measured on the DMA ring for
`GL_REPLACE` (`captures/gl-matrix/texrepl-secs.txt` ring `0x1962`) **[HW]**.

All five blobs carry `count = 1`, `TA[0].opcode = 1`, `TA[1..3].opcode = 0`,
i.e. one live TA stage -> the ALU instruction lands at instruction index 1 =
**slot 2**, which is again what the wire shows.

### 4.2 The table

Values below are for the **slot the instruction actually occupies**; `S` is
that slot number, OR-ed into `0x1264[4:0]`. Textures are `GL_RGBA` (the sweep's
own); the format-dependent variants are noted after.

| `glTexEnvi` mode | pipe | instruction | `0x1264` | `0x126c` |
|---|---|---|---|---|
| **`GL_REPLACE`** | rgb | `MOV_sat r0, TEX` | `0x00000000\|S` | `0x01400400` |
| | alpha | co-issued (RGBA) | `0x00000000` | `0x01400400` |
| **`GL_MODULATE`** | rgb | `MUL_sat r0, TEX, PREV` | `0x08000000\|S` | `0x01400000` |
| | alpha | co-issued (RGBA) | `0x08000000` | `0x01400000` |
| **`GL_ADD`** | rgb | `ADD_sat r0, TEX, PREV` | `0x00040000\|S` | `0x01400c00` |
| | alpha | **separate** `MUL_sat r0, TEX, PREV` | `0x08000000` | `0x01400000` |
| **`GL_DECAL`** | rgb | `LRP_sat r0, TEX, PREV, TEX.a` | `0x00040000\|S` | `0x01400b00` |
| | alpha | **separate** `MOV_sat r0, PREV` | `0x00000200` | `0x01400400` |
| **`GL_BLEND`** | rgb | `LRP_sat r0, c0, PREV, TEX` | `0x000401c0\|S` | `0x01400a00` |
| | alpha | **separate** `MUL_sat r0, TEX, PREV` | `0x08000000` | `0x01400000` |
| **`GL_COMBINE` `GL_DOT3_RGB`** | rgb | `DP3_sat r0, bx2(TEX), bx2(PREV)` | `0x48001000\|S` | `0x01401000` |
| | alpha | **separate** `MUL_sat r0, TEX, PREV` | `0x08000000` | `0x01400000` |

`TEX` is the unit's texture register `t<unit>`, `PREV` is `v0` for the first
emitted instruction and `r0` afterwards, so **on unit 1 replace
`Areg 0 -> 1` (`+0x20` in `0x1264`) and `Creg/Breg v0(0x10) -> r0(4)`**:

```
GL_ADD      on unit 1 :  0x1264 = 0x00010020|S   0x126c = 0x01400c00
GL_DECAL    on unit 1 :  0x1264 = 0x00810020|S   0x126c = 0x01400b00
                         (A = t1, B = r0 << 14, C = t1 << 23, Crep .a)
GL_MODULATE on unit 1 :  0x1264 = 0x02000020|S   0x126c = 0x01400000
```

`GL_BLEND` additionally reads `c0` = `GL_TEXTURE_ENV_COLOR`: set slot flag bit
1 and emit `0x1278`/`0x127c` with the constant (11-bit Q1.10 per
`docs/parhelia-fragment-isa.md` §9.1). It is the only mode of the six that
needs a constant.

Field checks for the two new opcodes, by hand:
`GL_ADD` = `ADD` base `0x0c00`, `A <- s0 = t0` (0), `B <- s1 = v0` ->
`0x10 << 14 = 0x00040000`, `dreg r0 << 20 | sat << 24 = 0x01400000`.
`GL_DECAL` = `LRP` base `0x0a00`, `A <- s0 = t0`, `B <- s1 = v0`,
`C <- s2 = t0` with **replicate `.a` = 4** -> `Crep 4 << 6 = 0x100`, giving
`0x0b00`. `LRP = A*C + B*(1-C)` = `Ct*At + Cp*(1-At)` — the GL DECAL equation,
which is the third independent confirmation of the LRP operand order
(`docs/parhelia-fragment-isa.md` §7.4).

**Format-dependent packet results** **[CAP]**:
with `GL_RGB` / `GL_LUMINANCE` (no alpha channel) `GL_REPLACE` and
`GL_MODULATE` stop co-issuing and emit a separate alpha `MOV r0.a, PREV`
(`0x1260 = 0x00000200`, `0x1268 = 0x01400400`); `GL_ADD` and `GL_BLEND`
co-issue only for `GL_INTENSITY`; `GL_DECAL` never co-issues. ioquake3's
textures are `GL_RGBA`/`GL_RGB`, so the co-issued rows above apply to the RGBA
case and the split ones to RGB.

`MTXOGL.dll:0x1010d532` is not the claimed GL_RGBA compiler branch; it is a
pixel conversion loop.  The distinction here is measured in archive packets,
not inferred from that invalid location.  See
`parhelia-p690-native-alpha-static-recheck.md`.

---

## 5. Complete recipe delta — one texture to two

Against `docs/parhelia-textured-draw.md` §5, everything not listed is
unchanged.

```
0x1110       0x00000103  -> 0x00000303        texcoord sets 0 and 1
0x10b8 [3]   0xa0c70050  -> 0x20c70050        texcoord0, last flag cleared
0x10b8 [4]   (absent)    -> 0xa0c80070        texcoord1 -> v8, dword 7, last
0x10b8 [5]   0x00000000                       terminator

0x1200       0x00040103                       unit 0, 2D, pipes 0+1
  0x11dc/0x11e0/0x11e4 = TEXBASE0 / w|fmt / h|1
  0x11c0/0x11c4        = filter0 / wrap0
  0x11cc = 0xffffffff, 0x11d0 = 0x11c8 = 0x11d4 = 0x11d8 = 0
0x1204       0x0001000c  -> 0x0004010c        unit 1, 2D, pipes 2+3
  0x11dc/0x11e0/0x11e4 = TEXBASE1 / w|fmt / h|1     <- the second bank
  0x11c0/0x11c4        = filter1 / wrap1
  0x11cc = 0xffffffff, 0x11d0 = 0x11c8 = 0x11d4 = 0x11d8 = 0
0x1208 = 0 ; 0x120c = 0

combiner     2 live slots -> 4 live slots + 1 pad  (§3.3)
draw header  0x970c0001  -> 0x97100001
vertex       28 bytes    -> 36 bytes
WARP         --gen passthrough --tex 1 -> --tex 2  (adds `mov o4, v8`)
0x1300, 0x12f4, 0x121c, 0x1114, 0x1118, 0x1040, 0x10b0   unchanged
```

Ordering rules, unchanged and still real: `0x121c = 0` before the unit block;
`0x1200 + unit*4` in the same packet as (or before) that unit's window;
`0x1300` after the WARP bind; `0x10b4 = 0` before the `0x10b8` burst; texels
written before the doorbell.

---

## 6. What is PROVEN and what is not

**PROVEN [HW] or [BIN] with a named function**

* four texture units, and `0x1200 + unit*4` banking the single
  `0x11c0`-`0x11e4` window (§1.1) — a live 4-unit section plus three emitters,
  one of them named `CDPSTextureEngine::UpdateHardware`;
* the per-unit register list and its emission order, from two emitters that
  group it differently and one live capture;
* the trailing `0x1200..0x120c` tag tables indexed by active-unit count (§1.2);
* `0x1110` bit `8 + set` = texcoord-set enable, and the kernel's rule that it
  is cleared when the WARP program does not write the output `0x1118` names
  (§2.3) — this was `[INF]` before;
* the draw-header, descriptor and slot-walk formulas, each cross-checked
  against at least two measured instances;
* the texenv instruction sequences and their packed words for REPLACE,
  MODULATE, ADD, DECAL, BLEND and DOT3 (§4), from live program blobs, with
  REPLACE re-packing bit-exactly to the value on the DMA ring;
* that referencing `t1` in the combiner is what creates unit 1's TA stage
  (`GL 0x10da50`);
* limits: 2048, power-of-two, 4 units, per-unit independent format and filter.

**INFERRED**

* the exact pipe nibbles for a two-unit split (§1.3) — `0x3`/`0xc` recommended,
  `0x1`/`0x2` attested as an alternative on the same silicon;
* the texcoord-1 fetch descriptor `0xa0c80070` — built by a `[BIN]`-confirmed
  packer whose formula reproduces all three measured descriptors;
* the slot assignment for a 4-instruction program (0, 2, 4, 8 + pad 10) — the
  walk table is `[BIN]`, only its application to this program length is
  untested;
* the `GL_DECAL`/`GL_ADD` unit-1 variants, mechanical substitution of
  `TEX`/`PREV`.

### 6.1 P690 TA source follows the physical-pipe permutation

For unit 0 the texture-address slot is the constant pair
`0x1264 = 0x00004000`, `0x126c = 0x00000c00`, invariant across every textured
variant we have (`GL_MODULATE`/`GL_REPLACE`, RGBA/RGB/LUMINANCE, nearest/
linear, repeat/clamp, 2x2/8x8) **[HW]**. Read through the ALU field layout it
decodes as `dreg = t0`, `Areg = t0`, `Breg = t1`, `opcode = ADD` — arithmetic
nonsense, so at least one field is reinterpreted for TA slots.

The kernel *does* build these words — the wire proves it, and
`KRN mtx2132`/`mtx1652` build a 0x148-byte TA record per live stage carrying
the hardware TA opcode (`+0x8c`, via `mtx1411`), the channel mask
(`+0xb8`, via `mtx3585`) and **the stage index at `+0xa4`/`+0xa8`/`+0xac`**
— but the function that folds that record into a `(0x1264, 0x126c)` slot pair
was not located: `mtx1653` (the ALU packer) and `mtx1156` (the pad packer) both
write `slotRecord+0x08`/`+0x0c` from a 24-byte template
(`mtx1291`, `mtx3535`), and no third template with `0x4000`/`0xc00` exists in
`.data` (searched exhaustively), so the TA word is computed, not tabled.

**Best hypothesis, and the one to implement first:** the destination field
names the texture register the fetch lands in, so unit *u* writes `t_u`:

```
0x1264 = 0x00004000 | slot
0x126c = 0x00000c00 | (u << 20)          ; dreg = t_u
```

which is `0x00100c00` for unit 1. Reasoning: `t0..t3` are exactly the registers
the combiner reads a unit's sample from (`docs/parhelia-fragment-isa.md` §4,
and libGL uses the unit index directly as the source register code), so a fetch
for unit 1 must land in `t1`, and `0x126c[23:20]` is the only destination field
in the encoding.

**Second hypothesis:** the coordinate set also rides in `Areg`, giving
`0x1264 = 0x00004000 | (u << 5) | slot` as well.

The destination-only hypothesis has now been exercised by the isolated P690
DRM test.  With unit 0 a solid magenta texture, unit 1 a solid yellow texture,
white vertex colour, `0x1200/0x1204 = 0x00040101/0x00040104`, and the second TA
destination set to `t1` (`0x126c = 0x00100c00`), the fence completed but the
centre sample was `0x00ff00ff` instead of the two-stage MODULATE result
`0x00ff0000` (18,336 covered pixels, 18,384 mismatches) **[HW P690]**.  Thus
the destination field alone is insufficient to make unit 1 contribute.  The
first follow-up added the logical stage index to the second TA source words:

```
second TA RGB:    0x1264 = 0x00004022   /* unit 1 in Areg[9:5], slot 2 */
second TA alpha:  0x1260 = 0x00004020   /* same source, no slot bits */
second TA dst:    0x126c = 0x00100c00   /* unchanged t1 destination */
                  0x1268 = 0x00100c00
```

This variant is CLI-only (`--p690-multitex-ta-unit-probe`) and patches the
already-built test batch after checking the exact baseline words.  It does not
change the reference emitter or advertise multitexture to Gallium.

That source-1 probe also completed its fence and covered the same 18,336
pixels, but returned **black** (`0x00000000`) throughout the triangle
**[HW P690]**.  The pair of results strongly suggests that this repurposed
field selects a physical texture pipe: source 0 reads the magenta unit on pipe
0, while source 1 reads an unassigned pipe.  PARD's two-unit table routes the
second logical unit to physical pipe 2 (`0x1204 = 0x00040104`), so the next
evidence-backed variant keeps that vendor route and the `t1` destination but
uses source 2:

```
second TA RGB:    0x1264 = 0x00004042   /* physical pipe 2, slot 2 */
second TA alpha:  0x1260 = 0x00004040
second TA dst:    0x126c = 0x00100c00   /* unchanged t1 destination */
                  0x1268 = 0x00100c00
```

The `--p690-multitex-ta-pipe2-probe` result was exactly red (`0x00ff0000`),
with the untouched sample still `0x00204060`, 18,336 covered pixels, only 96
edge-convention mismatches, and a completed fence **[HW P690]**.  This confirms
that TA source selection follows PARD's physical-pipe permutation.

The reference emitter consequently uses source 2 for TA1 when
`single_texture_pipe` selects the P690 route.  The same state was then tested
in ioquake3 at 640x480 with vblank disabled.  Backend time fell from 134.15 ms
(7.45 FPS) with serial lightmapping to 121.08 ms (8.26 FPS) in one pass.  A
large static crop of matched window captures differed by only 0.15 levels per
8-bit colour channel on average **[HW P690]**.

Mesa therefore enables the one-pass path by default only on PCI ID `0x2539`,
only when the kernel supplies the multitexture WARP entry, and only for the
exact two-sample MODULATE shader shape already classified as
`PH_SERIAL_MODULATE`.  `PARHELIA_P690_MULTITEXTURE=0` restores the serial path
for diagnosis.  Other shader shapes and other chips retain their existing
paths.

**The experiment that settles it in one run** — no new instrumentation needed:
run any GL program that enables two texture units on the vendor stack while
capturing the ring the way `captures/gl-matrix` was captured (a 4-line addition
to `tools/gltest.c`: `glActiveTextureARB(GL_TEXTURE1_ARB)`,
`glEnable(GL_TEXTURE_2D)`, bind a second texture, feed
`glMultiTexCoord2fARB`), then

```
tools/decode-gl-secs.py --regs 0x1264,0x126c,0x1260,0x1268,0x1200,0x1204 <cap>
```

The section will contain **four** live combiner slots; the second one is the
unit-1 TA word, and diffing it against `0x00004000`/`0x00000c00` names the
field. The same capture also fixes §1.3's pipe nibbles and §2.2's second fetch
descriptor, i.e. every remaining `[INF]` in this document.

---

## 7. Corrections to existing documents

| document | claim | correction | evidence |
|---|---|---|---|
| `parhelia-fragment-isa.md` §11.1 | "`0x1200` looks like the unit selector that latches the following window. **INFERRED**" | **measured**: one section programs four units through one window, each preceded by its own `0x1200+u*4` | [HW] `base-secs.txt` `1950cd12`, `live-ring-0528/decoded.txt:53-99` |
| `parhelia-fragment-isa.md` §11.2 | pipe field "`1 << tbl0x1d3ac0[cnt*4+unit]`" | correct for `PARD`, but **not universal**: the Linux ICD gives one active unit `0x3` and parks `0xc` on a disabled unit. The nibble is a partition, the union need not be `0xf` | [HW] + `PARD 0x1d3ac0`/`0x1d3b10` re-dumped |
| `parhelia-textured-draw.md` §2.2 | "the union over all four units must cover `0xf`" | not a hardware rule — `PARD 0x1d3b10[1] = 0x1000` (union `0x1`) and `HAL` uses `0x1`,`0x2` for two units | [BIN] |
| `parhelia-textured-draw.md` §4.5 | `0x1110` bits [11:8] = texcoord sets — **[INF]** | **confirmed [BIN]**, plus the new rule that the kernel clears a set's bit when the WARP program does not write the matching output | `KRN mtx2301` @0x87d80, `mtx1495` @0x882b9 |
| `parhelia-textured-draw.md` §3.4 | "unit 1 chains `MUL r0, t1, r0`, instruction index 2 -> slot 4, second TA stage takes index 1 -> slot 2 — untested [INF]" | the chaining and the TA-per-referenced-register rule are now [BIN] from the generator itself; the **slot numbers** are 4 and 8 (not 4), because *two* TA stages precede the two ALU instructions | `GL 0x10ddd0`, `0x10d280`, `0x10da50`; slot table `KRN .rodata+0x1ae0` |
| `parhelia-textured-draw.md` §2.5 | trailing unit words "reproduce that pair verbatim" | they come from `tagA[count]`, so the pattern changes with the unit count: `tagA[2]` writes only `0x1208`/`0x120c` | [BIN] `KRN mtx82` @0x407b0, `mtx1381` @0x3faf0 |
| `parhelia-fragment-isa.md` §12.2.5 | GL_ADD / GL_DECAL / GL_BLEND / DOT3 given as instruction sequences only | now packed to register values and validated by a REPLACE round-trip against the wire (§4.2) | [HW] `glsweep-0528-prims-formats.txt` |
| — (new) | | `0x1260` never carries the slot address; it is `0x1264` with bits [4:0] cleared, and `0x1268 = 0x126c & ~0x124` | [BIN] `KRN mtx530` @0x69313, [HW] |
