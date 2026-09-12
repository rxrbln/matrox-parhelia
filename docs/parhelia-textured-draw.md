# Drawing a textured triangle on Parhelia-512

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Everything needed to turn our working Gouraud triangle
(`docs/parhelia-minimal-draw-recipe.md`) into a *textured* one: the texture
surface descriptor, the sampler window, the fragment-combiner program, the
vertex layout, and one ordered recipe.

**Sources.** The load-bearing evidence is the live capture matrix
`captures/gl-matrix/*-secs.txt` (37 variants of `tools/gltest.c`, DMA sections
decoded with the packet grammar of `docs/parhelia-fragment-isa.md` §10) plus
`captures/gltri-textured-0528-trace.txt`. Static evidence comes from
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (KRN),
`opengl6/lib64/libGL.so.1.3.0` (GL) and `xddm32_.../MTXPARD.dll` (PARD).

Evidence tags: **[HW]** measured on hardware / in a live ring capture,
**[BIN]** disassembly with symbol+offset, **[INF]** inferred, **[OPEN]** not
resolved.

The `gl-matrix` captures each contain **several GL contexts** — a run's own
pushbuffer plus the residue of earlier runs at lower ring addresses. Every
per-variant value below was taken from the section group that carries that
variant's distinguishing state (e.g. `texrepl` = ring prefix `0x1962`, the only
group anywhere with a `MOV` combiner), never from the largest section blindly.

`tools/decode-gl-secs.py` reproduces every capture-derived claim here, e.g.

```
$ tools/decode-gl-secs.py --ring 1962 --regs 0x1110,0x1264,0x126c \
      captures/gl-matrix/texrepl-secs.txt
=== SEC 19623f2e (446 dw)
   1110 = 00000101
   1264 = 00004000        \ slot 0, the texture-address stage
   126c = 00000c00        /
   1264 = 00000002        \ slot 2, MOV_sat r0, t0  (GL_REPLACE)
   126c = 01400400        /
   1264 = 00000084        \ first pad slot
   126c = 00400400        /
```

`--vertices` additionally dumps the floats a draw packet points at.

---

## 1. The texture surface

### 1.1 Three registers, no pitch register  **[HW][BIN]**

A texture image is described by exactly three registers. There is no pitch
register and no mip register.

| reg | bits | meaning |
|---|---|---|
| `0x11dc` | `[31:2]` | base, **byte** offset in the surface pool |
| | `[1:0]` | address space: `0` = local frame buffer, `2`/`3` = AGP / system |
| `0x11e0` | `[13:0]` | **width in texels, the true width** — not rounded |
| | `[18:16]` | tiling code, 0..5 (§1.3) |
| | `[29:24]` | hardware format code (§1.2) |
| | `[23:19]`, `[31:30]` | zero in every capture, **[OPEN]** |
| `0x11e4` | `[13:0]` | height in texels |
| | `[27:16]` | depth / slice count, minimum 1 |
| | `[15:14]`, `[31:28]` | zero in every capture, **[OPEN]** |

**Byte pitch is derived by the hardware**, `PARD 0xa40f0` (`GetPitchBytes`)
**[BIN]**:

```
w = 0x11e0 & 0x3fff
if (w >= 0x2000)               pitchBytes = bpt * 0x2000
else if (tiling == 0 || == 1)  pitchBytes = bpt * ((w + 31) & ~31)
else /* tiling 2..5 */         pitchBytes = bpt * next_pow2(max(w, 32))
```

Texel address for a linear (tiling 0) surface:
`base + y * pitchBytes + x * bpt`.

**The width field holds the raw width — [HW], settled.** `docs/parhelia-texture-layout.md`
§3 says the field "is the width already rounded up to a multiple of 32", which
contradicts the formula it quotes on the same page. The captures settle it: a
2×2 texture gives `0x11e0 = 0x32020002` and an 8×8 texture `0x32020008`
(`captures/gl-matrix/tex-secs.txt` ring group `0x195c`,
`tex8-secs.txt` group `0x1958`) — 2 and 8, not 32. The rounding happens inside
`GetPitchBytes`.

### 1.2 Format code — three new join-table entries  **[HW]**

`docs/parhelia-fragment-isa.md` §11.7 recovered *D3DFORMAT → internal id* and
*hardware code → bits per texel* but reported the internal-id → hardware-code
join as missing. Three entries of that join are now measured directly, by
running the same `gltest.c` triangle with three different `internalFormat`
arguments and reading `0x11e0` off the wire:

| `glTexImage2D` internalFormat | `0x11e0` | hw format code | bpt | tiling used |
|---|---|---|---|---|
| `GL_RGBA` | `0x32020002` | **`0x32`** (A8R8G8B8) | 32 | 2 |
| `GL_RGB` | `0x33020002` | **`0x33`** (X8R8G8B8) | 32 | 2 |
| `GL_LUMINANCE` | `0x11040002` | **`0x11`** (L8) | 8 | 4 |

Capture groups: `tex-secs.txt` `0x195c`, `texrgb-secs.txt` `0x1956`,
`texlum-secs.txt` `0x1952`. `1` texel row × 2 texels wide in all three.

This also **refutes** `docs/parhelia-texture-layout.md` §4.2's "the Linux GL ICD
stores 32 bits per texel for every requested internal format, it therefore only
ever needs code `0x32`": `GL_LUMINANCE` produces an 8-bpt code `0x11` surface.
The earlier reading came from `captures/tex_64x64_*.txt`, which only ever used
`GL_RGB`/`GL_RGBA`.

For a from-scratch driver, **use `0x32` (ARGB8888)**. It is the code MTXPARD's
own textured blit writes (`PARD 0x10eb71`, `or ...,0x32000000`) **[BIN]** and
the code the Linux GL path writes for `GL_RGBA` **[HW]**.

### 1.3 Tiling: textures ARE tiled in the vendor Linux GL path  **[HW]**

Every texture in every capture in the matrix carries a **non-zero** tiling code:
`2` for the 32-bpt formats, `4` for the 8-bpt one. Not one linear texture
appears.

That contradicts `docs/parhelia-texture-layout.md` §1's headline, "the layout the
vendor Linux GL stack actually uses is LINEAR, byte for byte, CONFIRMED on
hardware". That conclusion was drawn from the 4 MB region-6 heap, and §1.1 of
that document states the exact caveat that turns out to apply: the heap is a
**CPU-written staging buffer**, and the kernel `memcpy`s out of it into the real
surface (`docs/parhelia-texture-path.md`, upload record opcode `0x0a`, `KRN
mtx2222` @0x50fb0). The bytes measured there are the staging copy; `0x11dc` in
the same captures points at a *different* address, in the frame buffer
(`0x09006000`, `[1:0] = 0` = local FB).

**What a driver should do.** Two options, in order of confidence:

1. **Tiling code 0, linear.** `GetPitchBytes` handles it
   (`bpt * align(width,32)`), MTXPARD's internal textured blit uses it
   (`PARD 0x10ea10`-`0x10ecbe`: `0x11e0 = pitch | 0x32000000`, tiling field 0)
   **[BIN]**, and it needs no swizzle. Not yet exercised on hardware by us —
   **[INF]**, but it is the only mode with a known, simple layout.
2. **Tiling code 2**, matching the vendor. The layout is then the one we already
   measured for the 3D colour render target, which carries the *same* tiling
   code 2 in its own cfg word (`0x1224 = 0x32020780`, `[18:16] = 2`) **[HW]**:

   ```
   tile = 64 px wide x 32 rows = 8192 bytes at 32 bpp, tiles row-major
   addr(x,y) = base + (y>>5)*(W/64)*8192 + (x>>6)*8192
                    + ((y&31)*64 + (x&63))*4
   ```

   (`docs/parhelia-surface-address-translation.md` §5, derived to the byte from a
   1 003 356-byte measured footprint.) That the render target and the textures
   share tiling code 2 is itself new, and it **closes §5.3's `[OPEN]`** in that
   document: the tiling selector is cfg `[18:16]`, not `[31:30]`.

   One inconsistency to be aware of: `PARD 0xa3d80` returns `0x1000` (4096 B =
   32×32 texels) as the tile size for tiling 2 at 32 bpt, while the render-target
   measurement gives 8192 B / 64×32 **[HW]**. The 8192 figure agrees with the
   allocator's own alignment for the same class (`0x2000` granule,
   `GetLocalAlignmentAndGranularityREVB` `KRN .text+0x74170`) and with the
   height granularity of 32. The likeliest reading is a 4096-byte tile pair
   addressed as one 8192-byte unit — **[OPEN]**, and a second reason to start
   with tiling 0.

Base alignment: the observed texture base `0x09006000` is `0x2000`-aligned,
matching the tiling-2 allocation granule. Align a linear (tiling 0) surface to
`0x200` (`PARD 0xa3d80` arg 1) **[BIN]**.

### 1.4 Mip levels and slices

There are no mip registers. Levels live back to back in one allocation and the
driver reprograms `0x11dc`/`0x11e0`/`0x11e4` when the level changes
(`docs/parhelia-fragment-isa.md` §11.9, `docs/parhelia-texture-layout.md` §5)
**[BIN]**:

```
levelAddr(N) = base + sum_{l<N} pitchBytes(l) * height_l
```

`0x11e4[27:16]` is a slice count for volume textures; slices are consecutive.
For one 2D image, write `1 << 16`.

---

## 2. Sampler / stage binding

### 2.1 The window and the emission order  **[HW][BIN]**

Eleven registers, of which only `0x1200` carries a unit index. The captured
Linux GL emission order, from `captures/gl-matrix/tex-secs.txt` group `0x195c`,
is three packets:

```
tag 0x79787780 : 0x1200+0*4, 0x11dc, 0x11e0, 0x11e4
tag 0x76757170 : 0x11c0, 0x11c4, 0x11d4, 0x11d8
tag 0x20727473 : 0x11cc, 0x11d0, 0x11c8, DMAPAD
tag 0x83828120 : DMAPAD, 0x1204, 0x1208, 0x120c
```

`KRN mtx792` @0x69790 groups the same eleven registers differently
(`0x1200,0x11c0,0x11c4,0x11c8 / 0x11cc,0x11d0,0x11d4,0x11d8 /
0x11dc,0x11e0,0x11e4,PAD`) **[BIN]**. Two different groupings of the same
eleven writes both work, so **the only ordering rule that can be defended is
that `0x1200 + unit*4` is written first**, in the same packet or before. A
disabled unit gets `0x1200 + unit*4` and nothing else (`KRN 0x69880`,
`PARD 0x16fe5d`) **[BIN]**.

`0x121c = 0` is written immediately before the block (`KRN`) **[BIN][HW]** —
kept in the recipe as an ordering rule.

### 2.2 `0x1200 + unit*4` — unit control  **[HW][BIN]**

| bits | meaning | measured |
|---|---|---|
| `[3:0]` | one-hot texture-pipe select; the union over all four units must cover `0xf` | unit0 `0x3`, unit1 `0xc` |
| `[11:8]` | constant `1` | `1` |
| `[18:16]` | type / enable: `1` = disabled, `4` = 2D, `5` = 2D projective, `6`/`7` = 3D/cube | `4` |
| `[23:19]` | **hardware texture-address opcode** (`0` = plain `TEX`) — `docs/parhelia-texture-path.md`, proven in two binaries; `docs/parhelia-fragment-isa.md` §11.2 still calls it a "filter code" | `0` |
| `[24]`, `[28:27]` | writable, purpose unknown **[OPEN]** | `0` |

Live values, every textured variant, without exception:

```
0x1200 = 0x00040103        unit 0 active, 2D, pipes 0+1
0x1204 = 0x0001000c        unit 1 disabled, but still claiming pipes 2+3
0x1208 = 0x00000000        unit 2 off
0x120c = 0x00000000        unit 3 off
```

The pipe halves `0x3 | 0xc = 0xf` — the union rule is satisfied by unit 1's
*disabled* descriptor. Reproduce that pair verbatim; a single active unit
claiming all four pipes (`0x0004010f`) has never been observed.

### 2.3 `0x11c0` is the FILTER word — a correction  **[HW]**

`docs/parhelia-fragment-isa.md` §11.3/§11.4 name `0x11c0` "address / wrap modes"
and `0x11c4` "filter". **The live traffic has them the other way round.**

The discriminating pair of runs:

| variant | `glTexParameteri` | `0x11c0` | `0x11c4` |
|---|---|---|---|
| `tex` | `MIN=MAG=GL_NEAREST`, wrap default `GL_REPEAT` | `0x01c01000` | `0xf0000000` |
| `texlin` | `MIN=MAG=GL_LINEAR` | **`0x01c01012`** | `0xf0000000` |
| `texclamp` | `NEAREST`, `WRAP_S=WRAP_T=GL_CLAMP` | `0x01c01000` | **`0xf0000036`** |

(`tex-secs.txt` `0x195c`, `texlin-secs.txt` `0x195a`, `texclamp-secs.txt`
`0x1950` — each variant moves exactly one register, and they are different
registers.)

`0x11c0`'s delta is `0x12 = 2 | (2 << 3)`, i.e. two 3-bit fields both set to
**2**, and `2 = linear` is precisely the value in the kernel's filter hint
tables `mtx1864`/`mtx2559` = `{0,2,2,3}` (`docs/parhelia-fragment-isa.md`
§9.5.6) **[BIN]**. Nothing else in the sampler window has a "2 = linear" enum.

```
0x11c0  [2:0]   MAG filter   0 nearest, 2 linear, 3 anisotropic
        [5:3]   MIN filter   idem                              (min/mag order [INF])
        [8:6]   MIP filter   0 none, 1 no-mip, 2 mip-linear    (mtx1674 = {1,1,2,2})
        [12]    always 1
        [24:16] constant 0x1c0 in every capture                 [OPEN]

0x11c4  [2:0]   U wrap       0 = GL_REPEAT, 6 = GL_CLAMP
        [5:3]   V wrap
        [8:6]   W wrap                                          [INF, by symmetry]
        [23:16] LOD min clamp, 4.4 fixed
        [31:24] LOD max clamp, 4.4 fixed — 0xf0 = 15.0 in every capture
```

Known-good pairs: `GL_NEAREST` + `GL_REPEAT` → `0x11c0 = 0x01c01000`,
`0x11c4 = 0xf0000000`. `GL_LINEAR` → `0x11c0 = 0x01c01012`.
`GL_CLAMP` on S and T → `0x11c4 = 0xf0000036`.

The remaining wrap codes (`GL_CLAMP_TO_EDGE`, `GL_MIRRORED_REPEAT`) were not
captured — **[OPEN]**. Do not guess them; `0` (repeat) and `6` (clamp) are the
two we have.

Honest note on the conflict: §11.3/§11.4 derive their naming from MTXPARD's
struct fillers, which write the literal `0x49` into the register at struct
offset `+0x0c` on every path and therefore could never separate the two. The
struct-offset → register mapping in §11.1 is independently confirmed by
`KRN mtx792`; what was wrong is only the *label* on two of the offsets. The
live wire is authoritative.

### 2.4 The rest of the window  **[HW]**

```
0x11c8 = 0x00000000        role unknown [OPEN]
0x11cc = 0xffffffff        border colour, in the texture's own channel layout
0x11d0 = 0x00000000        border-colour mask
0x11d4 = 0x00000000        [OPEN]
0x11d8 = 0x00000000        [OPEN]
0x11e8 = not written       texture-cache partition; the Linux GL path never
                           emits it for 1..2 stages. PARD's table gives
                           0x00500000 for stage counts 1 and 2 [BIN]
0x12f4 = 0x42000000        guard band 32.0f, <= 2 stages [BIN][HW]
```

### 2.5 How many stages, and what selects them

Four texture units, four combiner texture registers `t0..t3`
(`docs/parhelia-fragment-isa.md` §4). There is **no "number of stages" register
and no "shader length" register**: a stage is active iff its `0x1200 + unit*4`
has `[18:16] != 1`, and the driver simply emits exactly 5 or 10 combiner slots.
Four values are functions of the two counts **[BIN]**:

| value | function of | rule |
|---|---|---|
| `0x11e8` | stage count | `PARD .data 0x1d43f0[count*4+unit]`; `0x00500000` for counts 1 and 2 |
| `0x12f4` | stage count | `0x42000000` (32.0) for ≤ 2 stages, `0x41f80000` (31.0) above |
| `0x1300` bit 21 | stage count | set when ≤ 2 stages are active (`KRN mtx1389` @0x257bb) |
| `0x1300` bit 19 | instruction count | set when the program has 6..10 instructions (`KRN mtx1389` @0x2575e-0x2576b) |

The recipe's `0x1300 = 0x00a60ec4` already has bit 21 set and bit 19 clear —
correct for one texture stage and a 2-instruction (5-slot) program, and
identical to the untextured value, so **`0x1300` does not change when a texture
is added** **[HW][BIN]**.

`docs/parhelia-fragment-isa.md` §1.1 calls bit 19 "pipeline bit `0x80000`"
without naming the register; it is `0x1300`. `docs/parhelia-gl-feature-map.md`'s
"reg `0x1050` = select fragment program" is **refuted** — `0x1050` is never
written in any DMA ring we have; the `(0x14, 3)` pair it was read from is an
ICD→kernel *record* slot, not `0x1000 + idx*4` **[BIN][HW]**.

---

## 3. The fragment combiner program

### 3.1 A textured program is TWO slots, not one  **[HW]** — new

`docs/parhelia-fragment-isa.md` §9.5.7 leaves open "no engine register carries a
TA descriptor". The captures answer it: **the texture-address stage is emitted
as an ordinary combiner slot**, through the same `0x1264`/`0x126c` window, and
it consumes instruction slot 0. The ALU instruction is therefore instruction
index 1, which maps to **hardware slot 2** under §1.1's
`index 0,1,2,3,4 → slot 0,2,4,8,10` walk.

That is exactly what §9.5.4 predicts from the kernel side — `KRN mtx2132`
@0x6e1d0 walks the TA array *before* the ALU array and advances the same
instruction counter `ctx+0x20` **[BIN]** — and it is what the wire shows.

The competing static reading, and why the wire wins. `KRN mtx958` @0x69650
condenses the TA descriptors into `HWREG+0x800 + i*0x38` with the count at
`HWREG+0x7ec`, and that area is emitted through the **block-load** namespace
(opcode `0x41`, `KRN mtx1342` @0x90010), not through `0x1000 + idx*4` **[BIN]**.
If that were the live route there would be a texturing-specific `0x41` packet.
There is not: the `0x41` header list is *identical* between the untextured and
the textured capture except for one material block that grows from 25 to 45
dwords because the ICD coalesced two adjacent constant runs (slot `0x134`,
5 vec4s → 10 vec4s including a 4×4 identity) **[HW]**. The path actually taken
is `KRN mtx1381` @0x3f9c9 / `mtx2501` @0x40302, whose packet header
`0x9a9b9899` = `{0x1264, 0x1260, 0x126c, 0x1268}` is byte-for-byte the tag in the
capture **[BIN][HW]**. So: two live slots, ALU at instruction index 1 — that
much is **[HW]**; that the occupant of index 0 is the texture-address stage is
**[INF]**, strongly, and is the only reading consistent with `mtx2132`.

Complete captured program for one `GL_MODULATE` 2D texture on unit 0
(`tex-secs.txt` group `0x195c`; byte-identical in `tex8`, `texlin`, `texlum`,
`texrgb`, `texclamp`):

```
slot 0   TA: plain 2D fetch, unit 0
  0x1264 = 0x00004000    0x1260 = 0x00004000
  0x126c = 0x00000c00    0x1268 = 0x00000c00
  0x1278 = 0x1280 = 0x127c = 0x1284 = 0

slot 2   ALU: MUL_sat r0, t0, v0
  0x1264 = 0x08000002    0x1260 = 0x08000000
  0x126c = 0x01400000    0x1268 = 0x01400000
  0x1278 = 0x1280 = 0x127c = 0x1284 = 0

slots 4, 8, 10   pad: MOV r0, r0
  0x1264 = 0x00000084 / 0x00000088 / 0x0000008a
  0x126c = 0x00400400
```

Five slots, i.e. the instruction count padded to 5 — the rule of
`docs/parhelia-fragment-isa.md` §1.1 (`count <= 5 -> 5`, else 10) **[BIN]**,
here confirmed on the wire with a *two*-instruction program.

**The alpha pair is always written**, even when the program has no separate
alpha instruction: the emitter substitutes the RGB pair with the three
alpha-replicate bits cleared, `0x1260 = 0x1264 & ~0x124`,
`0x1268 = 0x126c & ~0x124` (`KRN mtx3243` @0x694cf) **[BIN]**, and the wire
agrees. `docs/parhelia-fragment-isa.md` §1.1a's "the alpha pair is not emitted
at all" describes only `KRN mtx530`'s bit0-clear path; `mtx1381` / `mtx2501` —
the ones the ICD actually uses — always write all four.

**`0x1260[4:0]` is not a slot address.** The slot is merged into `0x1264` only
(`KRN mtx530` @0x69313) **[BIN]**, and both captures show it: the ALU slot has
`0x1264 = 0x08000002` against `0x1260 = 0x08000000`, and in
`captures/live-ring-0528/decoded.txt:39-43` five pad slots run
`0x1264 = 0x80, 0x82, 0x84, 0x88, 0x8a` while `0x1260` stays `0x00000080`
throughout **[HW]**.

### 3.2 The TA slot words  **[HW], decode [OPEN]**

`0x1264 = 0x00004000`, `0x126c = 0x00000c00` is **invariant across every
textured variant we have** — `GL_MODULATE` and `GL_REPLACE`, `GL_RGBA`,
`GL_RGB` and `GL_LUMINANCE`, nearest and linear, repeat and clamp, 2×2 and 8×8.
It is therefore the constant "plain `TEX` fetch from unit 0 into `t0`".

Read through the *ALU* field layout of §2/§3 — `tools/parhelia-fragdis.py`
does exactly this — it decodes as `Areg = 0`, `Breg = 1`, `opcode [14:9] = 6`,
`dreg = 0 (t0)`, no saturate, i.e. `ADD t0, t0, t1`, which is meaningless as
arithmetic. The TA slot almost
certainly re-uses the two words with a different field layout; `dreg = 0`
naming the destination texture register `t0` is the one field that lines up.
We have a single sample, so the bitfields are **[OPEN]** — copy the two
constants for unit 0, and expect a second capture (a two-unit draw) to be what
separates the fields. Two hypotheses worth testing, both **[INF]**: bit 14 of
`0x1264` is the descriptor's `+0x04` "this stage performs a texture fetch" flag
(default 1, `docs/parhelia-fragment-isa.md` §9.5.1), and `0x1264[9:5]` is the
texture unit / coordinate set — which would make a unit-1 fetch
`0x1264 = 0x00004020`.

### 3.3 `GL_MODULATE` and `GL_REPLACE`  **[HW][BIN]**

The two texenv modes differ in exactly two places, and nothing else in the whole
446-dword context changes:

| | `GL_MODULATE` | `GL_REPLACE` |
|---|---|---|
| `0x1110` | `0x00000103` | `0x00000101` |
| `0x1264` slot 2 | `0x08000002` | `0x00000002` |
| `0x1260` | `0x08000000` | `0x00000000` |
| `0x126c` slot 2 | `0x01400000` | `0x01400400` |
| `0x1268` | `0x01400000` | `0x01400400` |

(`tex-secs.txt` `0x195c` vs `texrepl-secs.txt` `0x1962`.)

`tools/parhelia-fragdis.py` decodes both correctly, unmodified, and
independently reproduces the slot ↔ instruction-index mapping:

```
$ tools/parhelia-fragdis.py --words 0x1264=0x08000002,0x126c=0x01400000,\
                                    0x1260=0x08000000,0x1268=0x01400000
slot 2 (program instruction 1)
    rgb  MUL_sat    r0, t0, v0
    a    MUL_sat    r0, t0, v0

$ tools/parhelia-fragdis.py --words 0x1264=0x00000002,0x126c=0x01400400,\
                                    0x1260=0x00000000,0x1268=0x01400400
slot 2 (program instruction 1)
    rgb  MOV_sat    r0, t0
    a    MOV_sat    r0, t0
```

so the decoder and `docs/parhelia-fragment-isa.md` §2/§3/§4/§7.3 need no
correction for the ALU slot. By hand:

```
MODULATE   0x1264 = slot 2 | Areg 0 (t0) | Creg 0x10 (v0) << 23   = 0x08000002
           0x126c = MUL base 0x0000 | dreg 4 (r0) << 20 | sat<<24 = 0x01400000
           -> MUL_sat r0, t0, v0

REPLACE    0x1264 = slot 2 | Areg 0 (t0)                          = 0x00000002
           0x126c = MOV base 0x0400 | dreg 4 (r0) << 20 | sat<<24 = 0x01400400
           -> MOV_sat r0, t0
```

which is precisely libGL's fixed-function lowering
(`docs/parhelia-fragment-isa.md` §12.2.5, generators `GL 0x10d280` MODULATE and
`GL 0x10d460` REPLACE, with `TEX = t0` and `PREV = v0` for the first unit)
**[BIN]**. The untextured baseline is the same generator with zero units:
`MOV_sat r0, v0` = `0x1264 = 0x00000200`, `0x126c = 0x01400400` **[HW]**.

**The archived program differs by the texture's base format for the alpha
half** **[CAP]**. With `GL_RGBA` (and `GL_LUMINANCE_ALPHA`, `GL_INTENSITY`) the
instruction is co-issued and the alpha pair mirrors the RGB pair, which is the
case measured above. With `GL_RGB` or `GL_LUMINANCE`, `GL_REPLACE` emits a
*separate* alpha instruction
`MOV r0.a, PREV`, giving

```
0x1264 = 0x00000000   0x126c = 0x01400400      ; rgb   MOV_sat r0, t0
0x1260 = 0x00000200   0x1268 = 0x01400400      ; alpha MOV_sat r0, v0
```

(independently recovered from the `tex_env_replace` program blob in
`captures/glsweep-0528-raw.txt`, which used `GL_RGB`) **[HW]**. `GL_MODULATE`
with `GL_ALPHA` splits the other way: RGB `MOV r0, PREV`, alpha
`MUL r0.a, TEX, PREV`.

The old static citation `MTXOGL.dll:0x1010d532` was incorrect: it is a scalar
pixel conversion loop, not a `GL_RGBA` compare or fragment compiler.  See
`parhelia-p690-native-alpha-static-recheck.md`; the packet observations above
remain valid but do not establish the missing P690 sampler-alpha route.

`docs/parhelia-gl-state-encoding.md` §4.8 predicted `0x1264 -> 0x08000000` and
`0x126c -> 0x01400000` for MODULATE. The values are right; **the slot address is
2, not 0** — because the TA stage takes slot 0.

### 3.4 Adding a second texture unit  **[INF]**

By §12.2.4 the generator chains `MUL r0, t1, r0` for unit 1, i.e. instruction
index 2 → hardware slot 4, and a second TA stage would take index 1 → slot 2,
pushing the first ALU to index 2. Untested — **[INF]**.

---

## 4. The vertex side

### 4.1 Layout: `x, y, r, g, b, s, t`  **[HW]**

Seven floats, stride 28, no `z`, no `w`. Raw bytes from
`captures/gltri-textured-0528-trace.txt:55665`
(`SUBMIT 19 REGION off=0xaff0000 span=0x40-0x90`, 21 dwords = 3 × 28 B):

```
41a00000 41a00000 3f800000 3f800000 3f800000 00000000 00000000   (20,20)   white  (0,0)
43960000 41a00000 3f800000 3f800000 3f800000 40800000 00000000   (300,20)  white  (4,0)
43200000 435c0000 3f800000 3f800000 3f800000 40000000 40800000   (160,220) white  (2,4)
```

matching `tools/gltri.c` mode 2 exactly. The Gouraud control run is 20 bytes,
`x,y,r,g,b` (`gltri-gouraud-0528-trace.txt:49473`).

The buffer holds the attribute values **as the application passed them** — the
WARP program applies the MVP and the viewport. With our own `passthrough` WARP
program (§4.4) the same buffer holds window coordinates instead; the layout is
unaffected.

An 8-dword variant exists for the software wide-line / antialiased-line path:
draw header `0x970e0001`, `x,y,z,w,r,g,b,a`, stride 32, descriptors
`0x03c00000` + `0x83c50040` (`lwidth-secs.txt`, `linesmooth-secs.txt`) **[HW]**.
That is where `z`/`w` go when a path needs them: the position attribute keeps
offset 0 and widens its component mask to `xyzw`.

### 4.2 The draw packet  **[HW]**

```
0x970c0001  <startByteAddr>  <endByteAddr>          end exclusive
```

`0x97 = 0x17 | bit31` (GL path); `[22:17] = dwordsPerVertex - 1 = 6`;
low 16 = totalDwords − 2 = 1. `end - start = 84` for one triangle.
The packet **is** the launch — there is no 3D go register.

The vertex bytes must lie **outside** the executed command range: a float such
as `20.0f = 0x41a00000` parsed as a packet header is an opcode-`0x41` block load
and locks the engine **[HW]**.

### 4.3 The vertex-fetch descriptors — field `B` resolved  **[HW][BIN]**

`0x10b4 = 0` (write index), then one dword per attribute to the `0x10b8` data
port, which auto-increments. Packed by `KRN mtx2714` @0xb7b1-0xb87b **[BIN]**:

| bits | field |
|---|---|
| `[2:0]` | stream index |
| `[11:4]` | offset within the vertex, in **dwords** |
| `[15:12]` | zero on every ICD attribute **[OPEN]** |
| **`[19:16]`** | **destination WARP vertex-input register** |
| `[25:22]` | component mask: `0b0011` xy, `0b0111` xyz, `0b1111` xyzw |
| `[26]` | zero |
| `[29:27]` | `4` = ICD attribute, `0` = kernel-built |
| `[31]` | last attribute |

`docs/parhelia-vertex-fetch-and-memory-classes.md` §3 splits those same bits as
`[16:12] = A` and `[19:17] = B` and leaves `B` `[OPEN]`. **That split is wrong**:
bit 16 and `[19:17]` are one 4-bit field, and its values are exactly the WARP
input registers the vendor's vertex program reads — `v0` position, `v5` diffuse,
`v7..v10` texcoord 0..3 (`docs/parhelia-warp-programs-decoded.md:200`).

| attribute | descriptor | offset dw | `[19:16]` | mask | last |
|---|---|---|---|---|---|
| position `x,y` | `0x20c00000` | 0 | `0` → `v0` | xy | 0 |
| colour `r,g,b` | `0x21c50020` | 2 | `5` → `v5` | xyz | 0 |
| texcoord0 `s,t` | **`0xa0c70050`** | 5 | `7` → `v7` | xy | **1** |
| (terminator) | `0x00000000` | | | | |

Universal across all 37 variant captures: untextured emits
`20c00000, a1c50020, 0, 0`; every textured variant emits
`20c00000, 21c50020, a0c70050, 0`. Zero exceptions.

`docs/parhelia-gl-state-encoding.md` §4.8 predicted `0xa0c07050 | B<<17`; the
real value is **`0xa0c70050`** (the offset nibble and the register nibble were
transposed).

### 4.4 The WARP program does not change  **[HW]**

`0x1100`, `0x1104`, `0x10f0`, `0x10f8` and the stage entries
`0x10c0`/`0x10c8`..`0x10d8` are **byte-identical** between the untextured and the
textured run (`base-secs.txt:3132` vs `tex-secs.txt:2413`; all four binds
`0x08557183 / 0x08557383 / 0x08557583 / 0x08557783`, `0x10f8 = 2`).

The vendor's per-vertex program writes `o3..o6 ← v7..v10` unconditionally, as
the secondary halves of its four `dp4` bundles
(`docs/parhelia-warp-programs-decoded.md:139,166`) **[BIN]** — in the untextured
case `v7` simply comes from the default-attribute vector `0x1120..0x112c`
instead of the buffer. Output map: `o0` position, `o1`/`o2` colour 0/1,
`o3..o10` texcoord 0..7, `o11` clip-space position.

For our own driver, `tools/parhelia-warpasm.py --gen passthrough --tex 1`
already emits the right thing (5 instructions):

```
00083e00 00440000 b89a2249   add o0, v0, v0.0000
08087ebf 00440215 b89a2249   add o1, v5, r1.0000 | mov o11, v0
0078023f 00440200 b01a21a2   mov o3, v7
00000200 00040000 14c00000   end
00080200 00440000 301a21a2   wra
```

`--gen transform --tex 1` produces the transforming equivalent. Bind it exactly
as `docs/parhelia-minimal-draw-recipe.md` §3.3 does; the program buffer address
must be a multiple of 12.

### 4.5 `0x1110` is the interpolant-enable mask  **[HW][INF]**

The one register that *does* change on the vertex/setup side:

| variant | `0x1110` |
|---|---|
| untextured | `0x00000003` |
| textured, `GL_MODULATE` | `0x00000103` |
| textured, `GL_REPLACE` | `0x00000101` |
| `fog`, `lines` | `0x00000023` |
| X server's own 2D/3D path | `0x00000f27` |

Reading **[INF]**: a bitmask of live setup-unit interpolants — bit 0 position,
bit 1 diffuse, bit 2 specular, bit 5 fog, **bits `[11:8]` = texture coordinate
sets 0..3**. `GL_REPLACE` clears bit 1 because the combiner no longer reads
`v0`, even though the vertex still carries `r,g,b` and the descriptor still
fetches it.

**`0x1114` and `0x1118` do not change.** `0x1114 = 0x00287210` and
`0x1118 = 0x00006543` are identical in every variant, textured or not.
`docs/parhelia-gl-state-encoding.md` §4.8's prediction "`0x1118` nibble 0
`3 -> 5`" is **falsified** by six independent textured captures. `0x1118`'s
nibbles `3,4,5,6` for units 0..3 already point each unit at outputs `o3..o6`
(`KRN mtx3654` @0x8c9e8: nibble = `(v & 7) + 3`) **[BIN]**, which is why
enabling a texture leaves it alone.

### 4.6 `0x1040` is not the vertex stride  **[HW]**

`0x1040 = 0x00000004` in the textured run, identical to the Gouraud run whose
vertex is 5 dwords. The per-vertex size travels **only** in draw-header
`[22:17]`. `0x10b0 = 0x0006305c` (the primitive word for a triangle list) is
likewise unchanged, as are `0x1000 = 3` and `0x1064 = 0`.

---

## 5. The ordered recipe

Thirty steps, numbered 0–29: three to get the texels into VRAM, fifteen of
context, five of texture state, five of combiner, two per draw. This is
`docs/parhelia-minimal-draw-recipe.md` §3's 24-packet
stream with the texture parts substituted in; steps marked **=** are unchanged
from it and are not re-justified here. Register writes are `(index, value)`
pairs inside opcode-0 packets (`docs/parhelia-fragment-isa.md` §10).

Target of the worked example: 64×64 ARGB8888 texture, `GL_MODULATE`,
`GL_NEAREST`, `GL_REPEAT`, one triangle into a 1920×1200 ARGB target at VRAM
byte offset `0x02000000`.

### 5.0 Before any command: upload the texture  **[BIN]**

```
 0. Allocate TEXBASE in VRAM, 0x200-aligned, size = align(width,32)*bpt*height.
    64x64 ARGB8888 -> pitchBytes = 64*4 = 256, size = 16384.        [BIN 0xa40f0]
 1. Write the texels with plain CPU stores through the frame-buffer BAR,
    addr = TEXBASE + y*pitchBytes + x*4, ARGB8888.                  [INF, tiling 0]
    There is no upload blit and no DMA descriptor for pixel data — the vendor
    kernel memcpys too.                                             [BIN mtx2222 @0x50fb0]
 2. Finish the stores before ringing the doorbell.                  [BIN]
```

### 5.1 Context, once

```
 3. = 0x1328=0, 0x132c=<screen>, 0x12fc=0x0bb80024, 0x12f8=0x007fe000   scissor/raster
 4. = 0x12e8=0x00800202, 0x12e4=0x0000000e, 0x12f0=0, 0x12ec=0          blend/alpha off
 5. = 0x12c8=0x001000e4, 0x12cc=0xff, 0x12d0=0xff, 0x12c0, 0x12c4       depth/stencil off
 6. = 0x12d4=0, 0x12d8=0, 0x12dc=0x00ffffff                             fog off
 7. = 0x1120=0x45ffc5ff, 0x1124=0xc5ff45ff, 0x1128=0, 0x112c=0x3f800000 default attrs
 8.   0x1110=0x00000103, 0x1114=0x00287210, 0x1118=0x00006543          [HW] §4.5
        0x1110 = 0x00000101 for GL_REPLACE
 9. = 0x1374=0, 0x10f0=<mode> x2, 0x1100=<entry>, 0x10f8=2              WARP bind
10. = 0x1374=0, 0x10f0 x3, 0x1130, 0x111c, 0x1144                       WARP arm
11. = 0x1064=0, 0x1060=<capacity, non-zero>, 0x10b4=0                   fetch reset
12.   0x10b8 = 0x20c00000    position x,y      -> v0                   [HW][BIN] §4.3
      0x10b8 = 0x21c50020    colour r,g,b @dw2 -> v5   (last bit clear)
      0x10b8 = 0xa0c70050    texcoord s,t @dw5 -> v7   (last)
      0x10b8 = 0x00000000
13. = 0x1220=0x02000000, 0x1224=0x32020780, 0x1230=0, 0x1234=0x32020000 render target
14. = 0x1324=0xffffffff, 0x1314=0, 0x1318=0                             masks
15. = 0x12b0=0x3fff8842                                                 verbatim
16. = 0x1300=0x00a60ec4        AFTER the WARP bind, ordering rule       [HW]
        unchanged from the untextured recipe: bit 21 (<= 2 stages) set,
        bit 19 (> 5 instructions) clear — both correct here            §2.5
17.   0x12f4=0x42000000        guard band 32.0, <= 2 stages             [BIN][HW]
```

### 5.2 Texture state

```
18.   0x121c = 0x00000000                       ordering rule, before the units [BIN][HW]
19.   0x1200 = 0x00040103                       unit 0: 2D, TA opcode 0, pipes 0+1
      0x11dc = TEXBASE                          [1:0] = 0 -> local frame buffer
      0x11e0 = 64 | (0 << 16) | (0x32 << 24)    width 64, tiling 0 linear, ARGB8888
                = 0x32000040
      0x11e4 = 64 | (1 << 16) = 0x00010040      height 64, depth 1
20.   0x11c0 = 0x01c01000                       MAG/MIN/MIP = nearest        §2.3
      0x11c4 = 0xf0000000                       U/V wrap = repeat, LODmax 15 §2.3
      0x11d4 = 0, 0x11d8 = 0
21.   0x11cc = 0xffffffff, 0x11d0 = 0, 0x11c8 = 0        border colour
22.   0x1204 = 0x0001000c, 0x1208 = 0, 0x120c = 0        units 1-3 off, pipe union 0xf
```

Vendor variants for step 19, all **[HW]**: `GL_RGB` → format `0x33`;
`GL_LUMINANCE` → format `0x11`, 8 bpt; the vendor's own choice of tiling is
`2` for 32-bpt and `4` for 8-bpt, with the tile layout of §1.3.

### 5.3 Fragment combiner

```
23.   slot 0, texture-address stage — plain 2D fetch on unit 0        §3.2
        0x1264 = 0x00004000   0x1260 = 0x00004000
        0x126c = 0x00000c00   0x1268 = 0x00000c00
        0x1278 = 0x127c = 0x1280 = 0x1284 = 0
24.   slot 2, the ALU instruction                                     §3.3
        GL_MODULATE:  0x1264 = 0x08000002   0x1260 = 0x08000000
                      0x126c = 0x01400000   0x1268 = 0x01400000
        GL_REPLACE:   0x1264 = 0x00000002   0x1260 = 0x00000000
                      0x126c = 0x01400400   0x1268 = 0x01400400
        0x1278 = 0x127c = 0x1280 = 0x1284 = 0
      (with an alpha-less base format — GL_RGB / GL_LUMINANCE — GL_REPLACE
       instead needs a separate alpha instruction, §3.3)
25.   pad slot 4:   0x1264 = 0x00000084   0x126c = 0x00400400
26.   pad slot 8:   0x1264 = 0x00000088   0x126c = 0x00400400
27.   pad slot 10:  0x1264 = 0x0000008a   0x126c = 0x00400400
```

### 5.4 Per draw

```
28. = 0x10b0 = 0x0006305c ; 0x1000 = 3 ; 0x1040 = 4
29.   DRAW 0x970c0001 <vtxStart> <vtxEnd>       7 dwords/vertex, end exclusive
```

Vertex buffer, three vertices of 28 bytes, outside the executed command range:

```
struct vertex { float x, y, r, g, b, s, t; };
```

Then ring the doorbell as usual (`0x404 = start`, `0x408 = end | 3`).

### 5.5 Ordering constraints that are real

1. `0x121c = 0` before the `0x1200..0x120c` unit block **[BIN][HW]**.
2. `0x1200 + unit*4` first within the sampler window **[BIN]**, in the same
   packet or before `0x11c0..0x11e4`.
3. `0x1300 = 0x00a60ec4` **after** the WARP bind — a draw opcode armed before
   the program is bound stalls the engine **[HW]**.
4. `0x10b4 = 0` before the `0x10b8` descriptor burst **[BIN]**.
5. The texture pixels must be written before the doorbell.

### 5.6 Delta from the Gouraud recipe, in one block

Everything else in `docs/parhelia-minimal-draw-recipe.md` §3 is unchanged.

```
0x1110       0x00000003    -> 0x00000103           (0x00000101 for GL_REPLACE)
0x10b8 [2]   0xa1c50020    -> 0x21c50020           last flag cleared
0x10b8 [3]   0x00000000    -> 0xa0c70050           texcoord0 -> v7, dword 5, last
0x1200       0x0001000f    -> 0x00040103
0x1204       0x00000000    -> 0x0001000c
0x11dc/e0/e4               -> TEXBASE / width|fmt / height|1     (were unwritten)
0x11c0/c4/c8/cc/d0/d4/d8   -> 0x01c01000 / 0xf0000000 / 0 / 0xffffffff / 0 / 0 / 0
combiner                   -> two live slots (0 = TA, 2 = ALU), pads move to 4/8/10
draw header  0x97080001    -> 0x970c0001
vertex       20 bytes      -> 28 bytes
WARP program               -> add `mov o3, v7`  (--gen passthrough --tex 1)
```

Tool support to add: `tools/gen-minimal-draw.py` hard-codes
`0x1200 = 0x0001000f` and the untextured combiner, and needs a `--texture
BASE,W,H,FMT` option plus `--dw-per-vertex 7`; `tools/diff-gl-variants.py`
matches only `0x97080001` in its `span()` predicate and reports "NO VALID SPAN"
for every textured variant — adding `0x970c0001`/`0x970e0001` fixes it.

---

## 6. Corrections to existing documents

| document | claim | correction | evidence |
|---|---|---|---|
| `parhelia-fragment-isa.md` §11.3/§11.4 | `0x11c0` = wrap, `0x11c4` = filter | **swapped**: `0x11c0` is MAG/MIN/MIP, `0x11c4` is U/V/W wrap + LOD clamps | [HW] `texlin` moves only `0x11c0`, `texclamp` only `0x11c4` |
| `parhelia-fragment-isa.md` §9.5.7 | "no engine register carries a TA descriptor" | it is emitted as a combiner **slot** through `0x1264`/`0x126c`, taking instruction index 0 | [HW] two live slots per textured draw; [BIN] `KRN mtx2132` @0x6e1d0 |
| `parhelia-fragment-isa.md` §11.5 | `0x11e0[13:0]` is "pitch/width aligned to 32" | it is the **raw** width; the hardware rounds | [HW] 2 and 8 for 2×2 and 8×8 |
| `parhelia-texture-layout.md` §1 | the vendor Linux GL uses tiling 0, linear | it uses tiling **2** (32 bpt) and **4** (8 bpt); the linear measurement was of the staging heap | [HW] every texture in the matrix |
| `parhelia-texture-layout.md` §3 | the width field is pre-rounded to 32 | raw width | [HW] |
| `parhelia-texture-layout.md` §4.2 | GL always uses format `0x32`, 32 bpt | `GL_LUMINANCE` → `0x11`, 8 bpt; `GL_RGB` → `0x33` | [HW] |
| `parhelia-vertex-fetch-and-memory-classes.md` §3 | descriptor fields `A = [16:12]`, `B = [19:17]` (`B` open) | one 4-bit field `[19:16]` = WARP input register (`v0`, `v5`, `v7`) | [BIN] `KRN mtx2714` @0xb7f6; [HW] |
| `parhelia-gl-state-encoding.md` §4.8 | texcoord descriptor `0xa0c07050 \| B<<17` | `0xa0c70050` | [HW] |
| `parhelia-gl-state-encoding.md` §4.8 | `0x1118` nibble 0 `3 -> 5`; `0x1100`/`0x10f8` change | none of the three changes | [HW] six variants |
| `parhelia-gl-state-encoding.md` §4.8 | MODULATE combiner in slot 0 | values right, **slot 2** | [HW] |
| `parhelia-surface-address-translation.md` §5.3 | tiling selector "presumably cfg `[31:30]`" `[OPEN]` | it is cfg `[18:16]`; the measured RT carries code 2 | [HW] `0x1224 = 0x32020780` |
| `parhelia-fragment-isa.md` §1.1a | with no separate alpha instruction "the alpha pair is not emitted at all" | it *is* emitted, as `0x1264 & ~0x124` / `0x126c & ~0x124` | [BIN] `KRN mtx3243` @0x694cf; [HW] |
| `parhelia-fragment-isa.md` §1.1 | "more than 5 instructions sets pipeline bit `0x80000`" (register unnamed) | the register is `0x1300`, bit 19; bit 21 = "≤ 2 texture stages" | [BIN] `KRN mtx1389` @0x2575e, @0x257bb |
| `parhelia-fragment-isa.md` §16 | "no `0x1264`/`0x126c` word was observed on the wire" | `captures/live-ring-0528/decoded.txt:39-43,185` are exactly that | [HW] |
| `parhelia-gl-feature-map.md` | "reg `0x1050` = select fragment program" | refuted; `0x1050` is never written, the source was an ICD record slot | [BIN][HW] |
| `parhelia-record-stream.md` | "combiner slot counter += 2 per block ⇒ 10 blocks = 20 stages" | `[rsp+0x18]` at `KRN 0x6e3b5` is the constant-pool pair index (2i = c0, 2i+1 = c1); ten blocks are ten instructions | [BIN] |
| — (not documented anywhere) | | `0x1260[4:0]` carries **no** slot address; the slot lives only in `0x1264[4:0]` | [BIN] `KRN mtx530` @0x69313; [HW] |

`0x1130` is **not** affected by texturing (`0x0200200c` / `0x0700200c` in both
the `flat` and every `tex*` variant). Its bit 0 does move between variants, but
not with the one `parhelia-minimal-draw-recipe.md` §2.2 names — the `cull`
variant leaves it at the baseline `…0d` while `flat` and `pervert` sit at
`…0c`. Not pursued here.

---

## 7. Still open

* The bitfield layout of the TA slot words `0x1264 = 0x00004000` /
  `0x126c = 0x00000c00`. One sample only. A two-texture-unit draw would
  separate the fields.
* Wrap codes other than `0` (repeat) and `6` (clamp).
* Which of `0x11c0[2:0]` and `[5:3]` is MAG and which is MIN — the kernel's two
  hint tables are identical in all four rows.
* `0x11c0[24:16] = 0x1c0`, constant everywhere; LOD bias is the candidate.
* `0x11c8`, `0x11d4`, `0x11d8` — zero in every textured context decoded here
  (a `0x11c8 = 1` does appear elsewhere in the matrix, in another context's
  traffic), role unknown.
* Whether tiling code 0 (linear) is accepted by the texture unit on this
  silicon. MTXPARD's internal blit says yes **[BIN]**; we have not run it.
* Whether the tiling-2 tile is 4096 or 8192 bytes at 32 bpt (§1.3).
* `0x11e0[23:19]`/`[31:30]` and `0x11e4[15:14]`/`[31:28]`.
* The full internal-format-id → hardware-code join table; three entries are now
  known (§1.2).
* A **register-level mmiotrace of a textured draw**. Everything above comes from
  the DMA ring; `captures/mtxtrace-textured-*.txt` carry only ioctls, mmaps and
  heap diffs. One such capture would settle the TA slot fields, the remaining
  wrap codes and the min/mag order in a single run.
