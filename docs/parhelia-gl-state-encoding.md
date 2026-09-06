# Predicting the command stream: GL state -> Parhelia register encoding

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a **prediction document**, written from the vendor binaries *before* the
hardware captures exist. `tools/capture-gl-matrix.sh` runs `tools/gltest.c` in a
matrix of variants, each differing from one baseline in exactly one GL state, and
`tools/diff-gl-variants.py` reports the register/opcode delta. Everything below
says what that delta *should* be, so the capture confirms or corrects a stated
claim instead of being read as an oracle.

Tags on every claim:

* **[BIN file offset]** — read out of a vendor binary.
  `KRN` = `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (`.text`
  offsets; `.data` at file 0xdce00, `.rodata` at file 0xc1cc0),
  `ICD` = `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0`
  (x86-64, VMA = file offset for `.text` 0xb9000 and `.rodata` 0x3679c0),
  `PARD` = `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` (VMA).
* **[CAP]** — the existing live capture `captures/live-ring-0528/decoded.txt` and
  the ICD record traces `captures/gltri-{flat,gouraud,textured}-0528-trace.txt`.
* **[INF]** — inference; the reasoning is always given.
* **[OPEN]** — the binaries do not settle it; candidates are listed.

Prior art this builds on and does not repeat: `parhelia-3d-integrated.md`,
`-draw-builders-kernel.md` (DBK), `-pixel-pipeline-complete.md` (PPC),
`-vertex-fetch-and-memory-classes.md`, `-gl-limits.md` (the PIPELINE-KEY decode),
`-raster-clip.md` (RC), `-slot-translation.md` (ST), `-gl-feature-map.md` (FM),
`-shading-texturing-capture.md`.

---

## 0. What the instrument can and cannot see

`capture-gl-matrix.sh` dumps the primary ring page window and then every
secondary section named by a `0x20292820` call. `diff-gl-variants.py` decodes
opcode-0 register lists into a *last-writer* register map. Consequences that must
be kept in mind when reading a diff:

1. **Only registers written through opcode-0 lists are compared.** Opcode `0x41`
   block loads are counted and skipped, so **anything delivered by mechanism B
   (viewport, raster block, polygon offset, clip planes — ST §"Three delivery
   mechanisms") is invisible**. If a variant is predicted to change only a block
   load, the expected diff output is *"no register differences"* plus a change in
   the `blockload` opcode count or in nothing at all. That is a limitation of the
   tool, not evidence that the state does not reach the chip.
2. **Vertex bytes are not dumped.** Address-mode draws (`0x97…`) point at
   pushbuffer bytes outside the executed range (`parhelia-3d-integrated.md` §5),
   so a variant that only changes vertex *values* (flat vs smooth, per-vertex
   colours) produces an empty diff by construction.
3. **Per-run noise.** These differ between any two runs and must be ignored:
   `0x1090` (write-back address), `0x1094` (channel id), `0x1220`/`0x1228`
   (render-target and depth surface bases, per-allocation), and **`0x1060`**
   — it is `(bufferBytesFree)/stride` (`0x000280cc` = 163020 = free/20 in the
   capture, DBK), so it moves with pushbuffer occupancy as well as with stride.
4. The kernel drops writes whose value is unchanged (ST: `cmp [cache], value; je`),
   so a register missing from a variant's map may simply have been already
   correct — compare *values*, not presence, and treat "not written" as weak.

**Recommended tool change before the run:** decode `0x41` payloads (index and
dword count at least) so mechanism-B changes are visible, and print
`end - start` of each draw packet, since for most variants below that difference
*is* the entire observable delta.

---

## 1. The baseline, and one correction to the assumed baseline

`gltest.c` defaults: 320x240 window, `GL_TRIANGLES`, N=1, `GL_SMOOTH`, one
`glColor3f`, `glVertex3f(x, y, 0.0f)`, no depth/blend/cull/texture, point size
and line width 1.

### 1.1 The vertex is `{x, y, r, g, b}`, 20 bytes — *not* 24

`gltest.c` calls `glVertex3f` where the old `gltri.c` called `glVertex2f`, which
looks like it should add a Z to the vertex. It does not, because **the ICD's
installed `glVertex3f` handler compares Z against 0.0f and only upgrades the
vertex format when Z is non-zero**:

```
c8e70  ucomiss xmm2, [rip+0x2a0a9f]      ; .rodata 0x36992c = 0.0f
c8ea8  jne  c8f35        ; c8f35: mov esi,1 ; call c3170  -> position code 1 = x,y,z
c8eae  jp   c8f35
c8eb4  <fast path: append the vertex at the current stride>
```
[BIN ICD 0xc8e70–0xc8f3d; constant [BIN ICD .rodata 0x36992c = 0.0f]]. The
double-precision twin at `0xc8fb0` does the same against the double 0.0 at
`0x369930` [BIN ICD 0xc8fcb]. `0xc3170` is `SetPositionSize(ctx, code)`:
`code 0 -> 0xa530 |= 0x18` (position `x,y`), `code 1 -> |0x19` (`x,y,z`),
`code 2 -> |0x1a` (`x,y,z,w`), `code >= 3` = leave unchanged
[BIN ICD 0xc3170, 0xc38cc, 0xc3420, 0xc3675]. The key builder then reads
`ctx+0xa530 & 3` as the PIPELINE-KEY position field [BIN ICD 0xc3f88–0xc3fa9].

So the baseline is byte-for-byte the layout of the existing flat/Gouraud capture
[CAP]: PIPELINE-KEY `0x00000010`, stride 20, and every register in
PPC §5.1 applies unchanged.

### 1.2 Baseline expectations (what `base` should print)

| item | value | source |
|---|---|---|
| draw header | `0x97080001` (5 dwords/vertex -> `(5-1)<<17`) | [CAP], DBK |
| draw operands | `end - start = 3 * 20 = 60` bytes | [INF from DBK] |
| `0x10b0` | `0x0002305c`, then `0x0006305c` | [CAP], [BIN KRN `mtx3319` 0xb610] |
| `0x10b8` | `0x20c00000`, `0xa1c50020`, `0`, `0` | [CAP] |
| `0x1000/0x1040` | `2`/`4` then `3`/`4` | [CAP], DBK |
| `0x1328/0x132c` | `0` / `0x00f00140` | [CAP] |
| `0x12c8/0x12cc/0x12d0` | `0x001000e4` / `0xff` / `0xff` | [CAP] |
| `0x12e8/0x12e4` | `0x00800202` / `0x0000000e` | [CAP] |
| `0x12fc/0x12f8` | `0x0bb80024` / `0x007fe000` | [CAP] |
| `0x1200..0x120c` | `0x0001000f`, `0`, `0`, `0` | [CAP] |
| combiner slot 0 | `0x1264=0x200`, `0x126c=0x01400400`, alpha identical | [CAP] |
| `0x1300` | `0x00a60ec4` | [CAP] |

---

## 2. Correction to the primitive tables, verified byte for byte

The two tables were re-read out of the object file, and the GL-enum translation
table that feeds them was found. This corrects the value quoted for lines in the
task brief (`0x0002303d`) and pins every remaining primitive.

`mtx734` = `.data+0x40`, `mtx3178` = `.data+0x80`, both 52 bytes = 13 `u32`
[BIN KRN symbol table; dumps at file 0xdce40 / 0xdce80]:

| prim idx p | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `mtx734[p]` | 20 | 20 | 3d | 3d | 3d | 5c | 5c | 5c | 5c | 5c | 5c | 3d | 5c |
| `mtx3178[p]` | 0 | 0 | 1 | 2 | 2 | 3 | 4 | 5 | 5 | 6 | 7 | 1 | 3 |

`mtx3319` composes exactly [BIN KRN 0xb610–0xb672]:
`word = (word & ~0x7f) | mtx734[p]`; `word[14:12] = mtx3178[p] & 7`;
`word |= 0x20000`; `word |= 0x40000` iff `ctx+0x430 == 1`.

**The GL primitive enum is translated by `mtx2468` = `.rodata+0x9c0`** (13 `u32`)
[BIN KRN 0xc2680; used at 0x4b5d0 `mov esi,[rax*4+mtx2468]` immediately before the
`mtx3319` call at 0x4b5de, and at 0x47245 where the result is cached into
`ctx+0x297c` — which is exactly what `mtx3712` passes to `mtx3319`
(`mov esi,[rbp+0x4]`, `rbp = ctx+0x2978`) at 0x19040 / 0x1949f]:

`mtx2468 = { 0, 2, 3, 4, 5, 6, 7, 9, 10, 1, 5, 5, 5 }`

Sibling table `mtx3289 = .rodata+0xa00 = { 0,0,1,1,1,2,2,2,2,3,3,1,2 }` is indexed
by the *prim index* and is the vertices-per-primitive class (0 = 1, 1 = 2,
2 = 3, 3 = 4); it is cached at `ctx+0xc14` [BIN KRN 0x4728c] and only controls how
many vertices the copy builder packs per packet — **no register**.

Resulting `0x10b0` per GL primitive (bit 17 form; the second, bit-18 form is the
same value `| 0x40000`):

| GL primitive | enum | p | `0x10b0` (bit17) | `0x10b0` (bit17+18) |
|---|---|---|---|---|
| `GL_POINTS` | 0 | 0 | `0x00020020` | `0x00060020` |
| `GL_LINES` | 1 | 2 | **`0x0002103d`** | `0x0006103d` |
| `GL_LINE_LOOP` | 2 | 3 | `0x0002203d` | `0x0006203d` |
| `GL_LINE_STRIP` | 3 | 4 | `0x0002203d` | `0x0006203d` |
| `GL_TRIANGLES` | 4 | 5 | `0x0002305c` [CAP] | `0x0006305c` [CAP] |
| `GL_TRIANGLE_STRIP` | 5 | 6 | `0x0002405c` | `0x0006405c` |
| `GL_TRIANGLE_FAN` | 6 | 7 | `0x0002505c` | `0x0006505c` |
| `GL_QUADS` | 7 | 9 | `0x0002605c` | `0x0006605c` |
| `GL_QUAD_STRIP` | 8 | 10 | `0x0002705c` | `0x0006705c` |
| *(topology 9 = sized point, §4.9)* | — | 1 | `0x00020020` | `0x00060020` |
| *(topology 10 = smooth wide line, §4.10)* | — | 5 | `0x0002305c` | `0x0006305c` |

The brief's `lines = 0x0002303d` is wrong: no table row produces `mtx3178 = 3`
together with `mtx734 = 0x3d`. **Lines are `0x0002103d`.**

**`mtx2468` is indexed by the ICD's internal topology enum, not by the GL enum**
— they coincide for 0..8 because the ICD passes `glBegin`'s mode through verbatim
[FM], but the enum has extra codes above that. Entry 9 -> prim 1 (the *second*
point row) and entry 10 -> prim 5 (triangles) are exactly the two codes the
ICD's software point/line paths emit: `Clip_Point_to_Sizedpoint` sets topology
**9** and a smooth wide line sets topology **0xa** (§4.9/§4.10). That reading
disposes of the apparent "`GL_POLYGON` renders as points" absurdity — GL_POLYGON
never reaches the driver as 9; it is decomposed to a fan on the ICD side [INF].
Entries 11 and 12 also map to prim 5 and are unidentified.

Line-strip note: the ICD is documented to toggle `0x1104` per draw as
`(flag ^ 1) << 2` **on line strips** (`parhelia-3d-submission-audit.md` §"0x1104"),
so `linestrip` may additionally show `0x1104` differing from `base` [INF, low
confidence — one draw per capture means the toggle may not be observable].

---

## 3. Vertex layout: the rule, and the `0x10b8` descriptor arithmetic

The PIPELINE-KEY (`gl-limits.md` §1.2) is the ICD's vertex-format contract; the
kernel never programs it into a register, but its fields determine the copied
vertex, hence the `0x10b8` descriptors and the draw header's `[22:17]` field. The
fields that any variant here can move:

| key bits | field | set by |
|---|---|---|
| [1:0] | position 0=`xy` 1=`xyz` 2=`xyzw` | `SetPositionSize` [BIN ICD 0xc3170], driven by a non-zero Z / W in the immediate-mode handler [BIN ICD 0xc8e70, 0xc8fb0] |
| [6:4] | diffuse 0=absent 1=`rgb` 5=`rgba` | the requirement pass forces 5 (`or eax,0x50` after masking `0x8f`) when `ctx+0xc988` is set [BIN ICD 0xc70d5] |
| [18:16] | texcoord set 0, component count | copied from `ctx+0xa560` under shift table `.rodata 0x369830 = {16,19,22,25}` when `ctx+0xc98c[u]` is set [BIN ICD 0xc7080–0xc7137] |

`ctx+0xc988..0xc990` is a *required-attribute* set computed at `0xc7910` from the
compiled program before every draw [BIN ICD 0xc7910–0xc7972]; it is what makes an
enabled feature pull an attribute into the vertex.

### 3.1 `0x10b8` descriptor arithmetic (DBK §"0x10b8", field A resolved)

`d = stream[2:0] | dwordOffset<<4 | A<<12 | B<<17 | C<<22 | D<<27 | last<<31`,
with `D = 4` on every ICD attribute and `C` the component mask (`0b0011` = xy,
`0b0111` = xyz, `0b1111` = xyzw).

**Field A is the WARP input register number.** The decoded fixed-function vertex
program reads `v0` for position, `v5` for the diffuse colour, `v6` for the second
colour and `v7..v10` for the four texcoord sets (`add T0<-v0`;
`add o1<-v5`; `add o2<-v6`; `mov o3..o6 <- v7..v10` —
`parhelia-3d-integrated.md` §6). The captured descriptors carry exactly `A=0` for
position and `A=5` for the colour [CAP], which pins the reading. Therefore
**texcoord set 0 is `A = 7`** [INF, but from two independent directions].

Field `B` is 0 for position and 2 for the colour [CAP]; what it selects is
**[OPEN]** (both attributes are float vectors, so it is not a numeric type).
Candidates for a texcoord: `B = 0` (`0xa0c07050`), `B = 1` (`0xa0c27050`),
`B = 2` (`0xa0c47050`). The capture decides.

---

## 4. Per-variant predictions

### 4.1 `points`, `lines`, `linestrip`, `tristrip`, `trifan`, `quads`

| what changes | value |
|---|---|
| `0x10b0` (both writes) | per the table in §2 |
| draw operands | `end - start` = `nVerts * 20`: points 20, lines/linestrip 40, tristrip/trifan 60, quads 80 |
| everything else | **unchanged** |

The draw header stays `0x97080001`: the opcode is `0x17|bit31`, `[22:17]` is
dwords-per-vertex (layout unchanged) and the low 16 for address mode is the
constant 1 (3-dword packet) [BIN KRN DBK; `mtx1017+0x67c`, `+0x54d..0x650`]. The
primitive never reaches a raster register: `0x12fc`'s cull field is *cleared*
when the primitive has no facing [BIN PARD 0xa0490–0xa0681], which for points and
lines means the same value the baseline already has (cull already off), so
`0x12fc` should not move. `mtx3289` changes `ctx+0xc14` only, no register.

**Prediction that the capture should falsify if the model is wrong:** points and
lines produce *exactly one* register difference, `0x10b0`.

### 4.2 `tri2` — `GLTEST_N=2`, six vertices

Nothing changes but the draw operands: `end - start = 6 * 20 = 120`. No register
difference at all; `0x1060` may move because the pushbuffer write pointer moved
(§0.3), and must not be read as state. If the vertex byte count ever exceeded the
per-packet capacity the builder would split into two `0x97` packets [BIN KRN
`mtx1017`, DBK]; 120 bytes does not come close.

### 4.3 `flat` — `glShadeModel(GL_FLAT)`

**Predicted: no register difference, no opcode difference, no draw-header
difference — an entirely empty diff.**

* The ICD carries the shade model as one state-record id (`key 0x0c id 0x09`,
  `1` = flat, `2` = smooth, and smooth is the default so it is simply not
  re-sent) [CAP, `parhelia-shading-texturing-capture.md`]. Neither
  `mtx_parhelia.o` nor `MTXPARD.dll` derives a register write from it, and the
  vendor implements `GL_FLAT` by writing the provoking colour into all three
  vertices — the hardware always interpolates [PPC §3.8, BIN scan].
* In this matrix the baseline uses `GLTEST_PERVERT=0`, so all three vertices
  already carry the same colour: even the vertex *bytes* are identical.

**But it is not quite "nowhere".** The shade model is bank `0x0c` id `0x09`,
which `mtx2617` stores as the **top two bits of the byte `ctx+0x1be4`**
[BIN KRN, `gl-limits.md` §2.2] — the same byte whose low nibble holds the
primitive (`mtx1011[topology]`, [BIN KRN 0x4724d]) and whose bits [5:4] hold bank
`0x0c` id `0x08`. That byte is expanded into the wide raster/shader descriptor:

```
17ba7  movzx eax, BYTE PTR [rdi+0x1be4]
17bae  shr   al, 6
17bb4  mov   DWORD PTR [rsi+0x24], eax      ; shade model -> descriptor +0x24
```
[BIN KRN 0x17ba7–0x17bb4; the neighbouring `[rdi+0x1be5] & 3 -> [rsi+0x58]` at
0x17bc6 is the cull field]. Raster state reaches hardware through `mtx3654`,
which writes **the block file as well as registers** [ST §"Validation order"]. So
`flat` may change an **opcode-`0x41` block-load payload**, which
`diff-gl-variants.py` currently does not decode (§0.1). Expected observable
result with today's tool: an empty diff. With `0x41` decoding: possibly one
payload dword. It should still not move any `0x1000..0x13ff` register.

**A conflict that is now resolved, and should not be re-raised:** FM attributes
id `0x09` to the blend source-RGB factor, `parhelia-shading-texturing-capture.md`
to the shade model. Both are right — they are **different banks**. Blend factors
are bank `0x05` ids `0x09..0x0c` (`mtx1734`); the shade model is bank `0x0c` id
`0x09` (`mtx2617`) [gl-limits.md §2.2, BIN KRN].

### 4.4 `pervert` — a different colour per vertex

**Predicted: an entirely empty diff**, for the same reason as §4.3 point 2 in
reverse: the colour attribute already exists in the baseline vertex (`A=5`,
`C=0b0111`), only its per-vertex *values* change, and vertex bytes are outside
the dumped sections (§0.2). The Gouraud capture proved this directly: flat and
Gouraud differ in one ICD record and in the vertex ring, and in nothing else
[CAP `gltri-gouraud`].

### 4.5 `depth` — `glEnable(GL_DEPTH_TEST)`

Registers (`0x12c8/0x12cc/0x12d0` group, PPC §3.4; ICD slots 0x07 depth test,
0x17 depth func, 0x0e depth write, ST):

| register | base | predicted | why |
|---|---|---|---|
| `0x12c8` | `0x001000e4` | **`0x001000e5`** | bit 0 = depth-test enable; func field `[3:1]` already 2 = LESS (the GL default) |
| `0x12d0` | `0x000000ff` | **`0x000001ff`** | bit 8 = depth write; GL's default `glDepthMask(TRUE)` only becomes effective once the test is enabled, and the baseline shows the bit clear [CAP] |
| `0x12cc` | `0xff` | unchanged | stencil masks |

Vertex layout: **unchanged**. `gltest.c` passes `z = 0.0f`, so the position stays
`x,y` (§1.1); the missing Z is supplied by the default attribute vector
`0x1120..0x112c = 0,0,0,1.0f` [CAP], and the kernel's transform is four
unconditional `dp4`s on `r0.xyzw` regardless [BIN KRN, §5.3].

FM's sweep did show `depth_on` moving the key `0x10 -> 0x11` and the stride
`20 -> 24` — **and that is fully explained without any depth-driven rule**: its
`depth_on` op calls `tri(0.5f)`, i.e. `glVertex3f(x, y, 0.5f)`, while its
baseline calls `tri(0)` [`tools/parhelia-glsweep.c` lines 117-121, 201-206]. The
non-zero Z is what raised the position code. §6 works the whole of that table
through.

**Discriminator:** if the capture nevertheless shows the draw header becoming
`0x970a0001`, a third `0x10b8` descriptor or a position mask `C = 0b0111`, this
prediction is wrong and there is a depth-driven Z requirement inside ICD
`0xc7910`'s requirement set that I did not find.

`0x1228/0x122c` (the S8Z24 depth surface, format `0x36`) are already bound in the
baseline [CAP], so no surface binding should appear.

### 4.6 `blend` — `glEnable(GL_BLEND)`, `SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA`

Register `0x12e8`, layout PPC §3.2 / ST: `bit0` enable, `[4:1]` srcRGB, `[8:5]`
dstRGB, `[12:9]` srcA, `[16:13]` dstA, `[19:17]`+`[22:20]` equation, `bit23`
dither. Hardware factor codes `SRC_ALPHA = 6`, `INV_SRC_ALPHA = 7`
[BIN PARD 0x9a5f0 table; KRN ST].

Baseline `0x00800202` decodes as disabled / ONE / ZERO / ONE / ZERO / ADD /
dither. Substituting 6,7,6,7:

> **`0x12e8` = `0x0080eced`** — `1 | 6<<1 | 7<<5 | 6<<9 | 7<<13 | 0<<17 | 1<<23`.

`0x12ec`/`0x12f0` (blend constant) stay `0`. `0x1300` must keep BOP = COPY or the
enable bit is force-cleared [BIN PARD 0x9c617]; the baseline `0x00a60ec4` already
has BOP `0xc` so nothing there moves.

**Vertex layout: predicted UNCHANGED** — 20 bytes, header `0x97080001`, the same
two `0x10b8` descriptors. Reasoning, and why this contradicts FM:

* `gltest.c` calls `glColor3f`, so the immediate-mode colour code stays 1 (`rgb`).
* The requirement pass that can force the RGBA code 5 only fires when the diffuse
  field is **entirely absent** (`test al,0x70; je` before `and 0x8f; or 0x50`)
  [BIN ICD 0xc7025–0xc70db] — it fills in a missing attribute, it does not widen
  an existing one.
* The producer of the requirement set, `0xc1e30`, derives it from the **texture
  environment**: it clears `[req+0]`, `[req+1]`, `[req+2]`, `[req+4..7]`,
  `[req+8]` and then walks the four texture units at `ctx+0x4160`
  [BIN ICD 0xc1e30–0xc1f6a]. Nothing in it reads blend state.
* Alpha is not needed in the vertex: the default attribute vector
  `0x1120..0x112c = 0, 0, 0, 1.0f` [CAP] supplies `v0.a = 1.0`, which is exactly
  what GL specifies for `glColor3f`.

FM's sweep did see blending move the key to `0x…51` and the stride to 28 —
because its `blend_srcalpha` op calls `glColor4f(1, 0, 0, 0.5f)` on top of the
sticky `x,y,z` left behind by the preceding depth ops
[`tools/parhelia-glsweep.c` lines 224-229; §6]. Neither the blend enable nor the
blend function contributed a byte. **The discriminator in the capture is the
draw header**: `0x97080001` confirms this reading, `0x970a0001` refutes it and
means blend state does pull alpha into the vertex (in which case the colour
descriptor becomes `0xa3c50020`, `C` = `0b1111`, and the operand span 72 B).

### 4.7 `cull` — `glEnable(GL_CULL_FACE)`

**The published chain for this one is wrong, and the correction is the single
most useful prediction in this document.** ST says cull lands via `mtx2215`
(validation step 2b) and PPC §3.5 says it lands in `0x12fc[1:0]`. That is true
for **MTXPARD/D3D CULLMODE**. On the Linux/GL path it is not: the GL cull enum
takes a different route and ends up in **`0x1130`**.

**(a) ICD.** The cull/polygon-mode validator reads the block at `ctx+0x9e0`
(`+0` enable, `+4` cullFace, `+8` frontFace, `+0xc/+0x10` polygon modes) and
emits [BIN ICD 0x112105–0x1128d3]:

| condition | record |
|---|---|
| front face CCW / CW | key `0x05` slot `0x05` = 3 / 2 [BIN ICD 0x112147, 0x112b42] |
| cull off, both polygon modes equal | key `0x0c` slot `0x16` = **1** [BIN ICD 0x112b8a] |
| cull on, the culled face is the CW-wound one | key `0x0c` slot `0x16` = **2** [BIN ICD 0x112bdb] |
| cull on, the culled face is the CCW-wound one | key `0x0c` slot `0x16` = **3** [BIN ICD 0x1121ce] |

The winding test is `frontFace == GL_CW ? cullFace == GL_FRONT : cullFace ==
GL_BACK` [BIN ICD 0x112b9f / 0x112bed]. `glFrontFace` is therefore folded into
the value *and* sent separately. `gltest` uses the GL defaults (`GL_BACK`,
`GL_CCW`) -> **slot `0x16` = 2**, confirming FM's measurement.

**(b) Kernel.** Slot `0x16` writes `HWSTATE[0x1be5][1:0]` with dirty
`[0x1bed] |= 4` [BIN KRN 0x19d99–0x19dbb], and is consumed by **`mtx3654`**
(validation step 6, raster), not `mtx2215` [BIN KRN 0x8c20c]:
`2 -> field 1` [0x8cee6], `3 -> field 2` [0x8ce76], `1 -> [dev+0x700]` (observed
0) [0x8d368], `0 -> unchanged`; then
`byte[S+0x5f0] = (byte[S+0x5f0] & ~3) | field` with `S = [dev+0x40]+0x1c0`
[BIN KRN 0x8c230–0x8c25b]. `S[0x5f0]` is emitted verbatim as register index
`0x4c` = **`0x1130`** [BIN KRN `mtx3400` 0x43a3b–0x43a81, reglist `0x2121214c`].

> **`0x1130` = `0x0700300c` / `0x0200300c` -> `0x0700300d` / `0x0200300d`**
> (low nibble `c -> d`), i.e. `0x1130[1:0]`: **0 = no cull, 1 = cull the
> CW-wound face, 2 = cull the CCW-wound face**.
>
> **`0x12fc` stays `0x0bb80024`.**

This also decodes two bits of `0x1130`, which
`parhelia-3d-submission-audit.md` and `parhelia-warp-binding.md` both leave open.

**(c) Why `0x12fc[1:0]` cannot move on the GL path.** `mtx2215` does write
`0x12fc`'s low byte from `[ctx+0xd4]` [BIN KRN 0x11f4a–0x11f59], but `[ctx+0xd4]`
comes from `mtx2137` [BIN KRN 0x119a0] out of `HWSTATE[0xc2d]`, which is written
only by key-`0x0c` slots `0x02` and `0xa1` [BIN KRN 0x19a9c, 0x1aac5]. A sweep of
all 28 key-`0x0c` emit sites in the 64-bit ICD finds slots
`0x07,0x08,0x09,0x0a,0x0c,0x0e,0x0f,0x16..0x1d,0x22..0x26,0x29,0x34..0x3b,0x3e,
0x89,0x8c..0x8f,0x98,0x9a..0xa0,0xa6,0xa8,0xab` and **neither `0x02` nor `0xa1`**
[BIN ICD, scan]. So `HWSTATE[0xc2d]` stays 0 for the life of a GL context and
`0x12fc[1:0]` is a second, GL-unreachable cull field.

**Secondary effect worth predicting so it is not misread as noise:** in
`mtx3400`, `if ([S+0x60] != 0 && (S[0x5f0] & 3) != 1) { mtx2316(...); [S+0x68] = result; }`
[BIN KRN 0x43cd0–0x43cfa]. Cull-BACK gives field `== 1` and therefore **skips**
that call, while the baseline (field 0) makes it. Expect the program/block-upload
section of the two streams to differ slightly. That is program selection, not
vertex format.

**No vertex-layout change**, no pipeline-key change: culling consumes no
attribute and the cullface op emits no `(0x07,1)` or `(0x08,1)` record
[BIN ICD; FM's "cullface -> stride 24" row is carry-over from the preceding
`depth_on` op in that lazily-flushed sweep].

Practical note: with `glFrontFace(GL_CCW)` and `glCullFace(GL_BACK)`,
`gltest.c`'s vertices (`(-0.7,-0.6), (0,0.6), (0.7,-0.6)`) are wound clockwise,
so **the triangle is expected to disappear from the screen**. The command stream
is submitted regardless — culling is in hardware — so the diff stays valid.

### 4.8 `tex` — a 2x2 RGBA texture, `GL_MODULATE`, `GL_NEAREST`

The largest delta. Register set = PPC §5.3 with the 64x64 dimensions replaced:

| register | base | predicted | source |
|---|---|---|---|
| `0x1200` | `0x0001000f` | `0x00040101` | enabled 2D, pipe 0 [BIN PARD 0x15a37a] |
| `0x1204/0x1208/0x120c` | `0` | `0x00010002` / `0x00010004` / `0x00010008` | units 1-3 idle [BIN PARD 0x15a2f9] |
| `0x11c0` | (n/a) | `0x00001000` | wrap = repeat (GL default) [BIN PARD 0x105401] |
| `0x11c4` | (n/a) | `0x00000040` | nearest/nearest/no-mip = fields (0,0,1) [BIN KRN hint tables 0x70cde] |
| `0x11cc/0x11d0` | (n/a) | `0xffffffff` / `0` | border defaults [BIN PARD 0x105381] |
| `0x11dc` | (n/a) | texture VRAM byte offset (per-run) | [BIN PARD 0x1051e4] |
| `0x11e0` | (n/a) | **`0x32000002`** | width 2, tiling 0, format 0x32 ARGB8888 [BIN PARD 0xa40f0] |
| `0x11e4` | (n/a) | **`0x00010002`** | height 2, depth 1 [BIN PARD 0xa4091] |
| `0x11e8` | (n/a) | `0x00500000` | cache partition, 1 active stage [BIN PARD .data 0x1c43f0] |
| `0x12f4` | `0x42000000` | unchanged | <= 2 stages -> 32.0f [BIN PARD 0x16fe0d] |
| `0x1264` | `0x00000200` | **`0x08000000`** | `MUL r0, t0, v0`: A = t0 (code 0), C = v0 (code 0x10) << 23 |
| `0x126c` | `0x01400400` | **`0x01400000`** | opcode MUL = 0, dst r0, saturate |
| `0x1260/0x1268` | as the RGB pair | same substitution | co-issued alpha [CAP textured] |

Note the 2x2 image is stored with a **32-texel row pitch**: the texel address is
`base + y*align(w,32)*bpt + x*bpt` [TL, hardware-proven], so the upload is
2 rows of 128 bytes, not 2 rows of 8. That is an upload-layout claim, not a
register claim.

Vertex layout **does** change (this one is solid — it is the one case the
existing textured capture measured [CAP]):

| item | base | predicted |
|---|---|---|
| PIPELINE-KEY | `0x00000010` | `0x00020010` (texcoord set 0 = 2 components) |
| stride | 20 | **28** (`x,y,r,g,b,s,t`) |
| draw header | `0x97080001` | **`0x970c0001`** (7 dwords/vertex) |
| `0x10b8` | `0x20c00000`, `0xa1c50020`, `0`, `0` | `0x20c00000`, **`0x21c50020`** (last flag cleared), **`0xa0c07050 \| B<<17`**, `0` |
| draw operands | 60 B | 84 B |

The texcoord descriptor: offset 5 dwords (`5<<4 = 0x50`), `A = 7` (§3.1),
`C = 0b0011`, `D = 4`, last. `B` is [OPEN] (§3.1).

The WARP program is predicted **not** to change (§5), so `0x1100`/`0x1104` should
keep the baseline entries; the texcoord path is expected to be selected by the
vertex-format group `0x1110/0x1114/0x1118` instead [PPC §5.3 from the record
trace: the ICD's `key 0x05` WARP bind serial is identical textured and
untextured]. Baseline values are `0x1110 = 3`, `0x1114 = 0x287210`,
`0x1118 = 0x6543` [CAP]; **if anything in the ring is going to reveal how a
texcoord is routed to the sampler, it is these three words** — watch them.

### 4.9 `psize` — `GLTEST_PRIM=points GLTEST_PSIZE=8`

**Diff against `points`, not against `base`** — this variant changes two things
relative to the baseline. `diff-gl-variants.py` only diffs against one baseline,
so run it a second time with `points` as the baseline (`diff-gl-variants.py
captures/gl-matrix points`).

`glPointSize` stores at `state+0xad4` (`if (v > 0) [p+4] = v; else
GL_INVALID_VALUE`) and sets dirty bit `0x20` in `state+0x10`
[BIN ICD 0x123c10, setter 0x1cb930]. **Any point size != 1.0f switches the ICD to
its software point path** — the predicate has no smooth/aliased split for points,
unlike lines:

```
ea70f  xmm0 = [state+0xad4]
ea717  ucomiss xmm0, xmm1            ; xmm1 = 1.0f (.rodata 0x367a0c)
ea71a  jne/jp -> Clip_Point_to_Sizedpoint   else Clip_Point_to_Point
```
[BIN ICD `GLPrimitiveSoftwareClipDispatch::ValidateState` 0xea660, arm 0xea70f].

**There is no CPU quad expansion.** `Clip_Point_to_Sizedpoint` (`0xebf10`) and
`Clip_Point_to_Point` (`0xebfd0`) are instruction-for-instruction identical
except for the topology they set and the viewport pre-pass they run
[BIN ICD, vtables `0x666200` / `0x6661a0` located via typeinfo
`.rodata 0x36ab00`]: one point in, **one vertex out**. What differs:

* topology `0` -> **`9`** (`mov esi,0x9` at 0xebf5a vs `xor esi,esi` at 0xec001,
  both into `SetPrimitiveType` 0xf3530, emitted as the `0x0c000000` record);
* pre-pass `0xe6fd0` (plain transform) -> **`0xea450`**, which pre-transforms to
  window space and pixel-snaps: `N = (int)(size + 0.5f)`, then odd and even `N`
  take different centring paths, `x = floorf(x)`, `y = ceilf(y - 0.5f)`, writing
  16-byte `[x][y][z][1/w]` [BIN ICD 0xea450–0xea587].

Predicted deltas (vs `points`):

| item | `points` | `psize` | source |
|---|---|---|---|
| ICD key `0x0c` slot `0x9a` | `0x3f800000` | **`0x41000000`** | [BIN ICD 0x114302] |
| ICD key `0x0c` slots `0x9b`/`0xa6` | defaults | **unchanged** | the clamps at 0x114343 are dead code (below) |
| topology -> `0x10b0` | `0x00020020` (prim 0) | **`0x00020020`** (topology 9 -> prim 1) | §2 |
| PIPELINE-KEY | `0x00000010` | **`0x00000852`** (`xyzw` + `rgba` + software bit 11) | [INF from the wide-line key [HW] and the identical pre-transform path] |
| stride | 20 | **32** | idem |
| draw header | `0x97080001` | **`0x970e0001`** (8 dwords/vertex) | [INF] |
| `0x10b8` | `0x20c00000`, `0xa1c50020` | **`0x23c00000`**, **`0xa3c50040`** | [INF from DBK's formulas: `xyzw` at 0, `rgba` at dword 4, `A=5`, `C=0b1111`, last] |
| draw operands | 20 B | **32 B** (still one vertex) | [BIN ICD 0xebf90] |
| block reg `0x172` dword 3 | `0.5f` | **`0x40800000` = 4.0f** | the point **radius**, `0.5f * (float)(int)(size + 0.5f)` [BIN KRN `mtx2497` 0x869cb–0x86970] |
| `HWREG+0xe2` bit 1 | 0 | 1 (`N` even) | [BIN KRN 0x869f5] |
| `0x1100` / `0x10f8` | baseline entries | **changed** | pre-transformed program (§5) |

`0x172` is a **block load**, invisible to the tool as it stands (§0.1).

**Two corrections to `parhelia-raster-clip.md` §11 fall out of this.**
(i) Slot `0x9a` -> `0x1268`, `0x9b` -> `0x126c`, `0xa6` -> `0x1298` is the
discredited `reg = 0x1000 + slot*4` rule; `0x1268`/`0x126c` are combiner ALPHA
instruction words. The kernel puts these slots in software shadows
`HWSTATE+0x1e30/0x1e34/0x1e38` [BIN KRN 0x1aa07, 0x1aa1d, 0x1aae5] and only
`mtx2497` turns them into block registers `0x172`/`0x173`.
(ii) The `GL_POINT_SMOOTH`-dependent clamps at ICD `0x114343` (`[0.25,10]`) and
`0x114fb8` (`[1,2048]`) are **dead code**: `0x1076a0`/`0x1076b0` are
`std::max`/`std::min` returning a *pointer* (`cmovbe rax, …; ret`, no store), and
both call sites discard `rax` and re-read the unmodified stack slot [BIN ICD].
Only `max = max(max, min)` survives; the real clamping is the kernel's, against
block `0x173` dwords 0/1.

Also worth knowing before diffing: if `pointSize == 1.0f` **and** either polygon
mode is `GL_POINT`, slot `0x9a` is forced to the magic `0x3f8020c5` (1.001f)
[BIN ICD 0x114a80]. `gltest` never sets `glPolygonMode`, so this does not fire.

### 4.10 `lwidth` — `GLTEST_PRIM=lines GLTEST_LWIDTH=8`

**Diff against `lines`.** The largest delta in the matrix.

`glLineWidth` stores at `state+0xaa8`, dirty bit `0x10` in `state+0x10`
[BIN ICD 0x123ba0, setter 0x1c1c50]. The software predicate has two regimes
[BIN ICD 0xea6cd–0xea8fd]:

* `GL_LINE_SMOOTH` on: software when `width != 1.0f` (`.rodata 0x367a0c`);
* aliased (our case): software when **`width >= 1.5f`** (`.rodata 0x36ad04`).

At width 8 the aliased path takes `Clip_Line_to_Wideline` (vtable `0x665c60`,
op `0xeee50`), keeps topology **1** (`0xa` = 10 would be the *smooth* case, which
maps to prim 5 = triangles — see §2), budgets `2 * (int)width = 16` vertices per
line [BIN ICD 0xeef06] and CPU-expands one line into **eight parallel 1-pixel
lines** at offsets `{0, -1, +1, -2, +2, -3, +3, -4}` [BIN ICD 0xe7190]. The
`assert(iLineWidth > 1)` at `0xe77c7` (`.rodata 0x36ad50`) only fires for
`N - 1 <= 0`, so not here.

| item | `lines` | `lwidth` | source |
|---|---|---|---|
| ICD key `0x05` slot `0x1b` | `0x3f800000` | **`0x41000000`** | [BIN ICD 0x1117f3–0x111838, from `state+0xaa8`] |
| `0x10b0` | `0x0002103d` | **`0x0002103d`** (unchanged; topology stays 1) | §2, [BIN ICD 0xeeedc] |
| PIPELINE-KEY | `0x00000010` | **`0x00000852`** | [HW] FM measured exactly this for `glLineWidth(4)` |
| stride | 20 | **32**, pre-transformed window space `[x,y,z,w,r,g,b,a]` | [HW] FM |
| draw header | `0x97080001` | **`0x970e0001`** | [INF] |
| `0x10b8` | `0x20c00000`, `0xa1c50020` | **`0x23c00000`**, **`0xa3c50040`** | [INF] |
| vertices / operands | 2 / 40 B | **16 / 512 B** | [BIN ICD 0xeef06, 0xe7190] |
| block reg `0x178` dword 2 | `0.5f` | **`0x40800000` = 4.0f** | line half-width, `width < 2.0 ? 0.5f : 0.5f * truncf(width)` [BIN KRN `mtx3654` 0x8c715–0x8d263] |
| `0x1144` (idx `0x51`) | baseline | **`0`** | `mtx2497` zeroes `S[0x618]` on the wide-line path [BIN KRN 0x868af] |
| `0x1100` / `0x10f8` | baseline entries | **changed** | pre-transformed program (§5) |

The kernel chain for the width value: ICD key `0x05` slot `0x1b` ->
`HWSTATE[0x1bef] |= 2; HWSTATE+0x1e50 = v` (= `raster+0x278`)
[BIN KRN 0x1ac4b] -> `mtx3654` -> block `0x178` d2. This pins the formula that
`parhelia-state-slot-map.md` §6.1 records as "clamped".

**Unresolved and worth a deliberate sweep:** the hardware line half-width
register *and* the CPU expansion are both active. If the hardware also widened,
8 CPU lines x 8 hardware pixels would give a 64-pixel band. One of the two must
be inert in this configuration — most plausibly `0x178` feeds only a legacy
DDA/AA line unit the WARP path does not use [INF]. A `GLTEST_LWIDTH` sweep
1 -> 2 -> 3 with a pixel count settles it in one run.

### 4.11 `big` — a 640x480 window instead of 320x240

The window is created as a direct child of the root at (0,0) and the harness runs
X with no window manager, so there is no reparenting offset.

| item | base | `big` | source |
|---|---|---|---|
| `0x1328` | `0x00000000` | **`0x00000000`** | scissor min |
| `0x132c` | `0x00f00140` | **`0x01e00280`** | scissor max, **exclusive** `(y+h)<<16 \| (x+w)` |
| `0x1220` / `0x1224` | `0x02000000` / `0x32020780` | **unchanged** | see below |
| `0x1228` / `0x122c` | depth base / `0x36020780` | **unchanged** | idem |
| block `0x160` d0/d1 | `160.0` / `-120.0` | **`320.0` (`0x43a00000`) / `-240.0` (`0xc3700000`)** | viewport scale `(W/2, -H/2)` |
| block `0x161` d0/d1/d3 | `160.0`/`120.0`/`240.0` | **`320.0`/`240.0`/`480.0`** | viewport bias `(X + W/2, Y + H/2)` and `H` |
| everything else | | unchanged | |

**Scissor.** The kernel derives it as an exclusive max corner, which the
`x = y = 0` baseline cannot distinguish from `(w, h)` but the code can
[BIN KRN `mtx558` 0x45f5f–0x45fe6 stores the rect at `HWSTATE+0xcd9/+0xcdd/
+0xce1/+0xce5`; `mtx2480` 0x12e96–0x1301a computes
`xmin = x; ymin = y; xmax = x + w; ymax = y + h`, clamps against the surface
extents `surf+0x1dc/+0x1e0`, **rejects** the whole validation if
`xmin > 0xfff || xmax > 0x1000 || ymin > 0xfff || ymax > 0x1000`, and writes
`0x1328 = (ymin<<16)|xmin`, `0x132c = (ymax<<16)|xmax` via the `0xbebfcbca`
reglist in `mtx1651` 0x41e20]. Note the clamp differs from the Windows driver,
which *clamps* to `0xffe`/`0xfff` instead of rejecting [BIN PARD 0x9d03c].
This also corrects `parhelia-rendertarget.md` §1.4, which gives the shadow
offsets as `+0xce9/+0xced/+0xcf1/+0xcf5` — they are 0x10 lower.

**Why the render target does not move.** The GLX back buffer is created **once
per device, not per drawable**, with the front buffer's geometry, and every GLX
drawable on that device renders into it at a translated origin under the hardware
scissor [BIN ICD 0x358ac0; `parhelia-rendertarget.md` §6.4]. So the pitch is
`align(screen_width, 32)` = `0x780` = 1920 px on this desktop for **any** window
size, and only a modeset or a different visual would move `0x1224`.

**The viewport reaches no register at all** — it is mechanism B, block indices
`0x160` (scale) and `0x161` (bias) [BIN KRN `mtx2926` 0x87220, corroborated by
BIN PARD 0x119371/0x1193d3, and `parhelia-block-state-map.md` §1.a]:

```
0x160.d0 =  0.5*W*fx     0x160.d1 = -0.5*H*fy     0x160.d2 = depth-range scale
0x161.d0 = (X1+X0)*fx + 0.5*W*fx
0x161.d1 = (Y1+Y0)*fy + 0.5*H*fy
0x161.d2 = depth-range bias      0x161.d3 = (float)H
```
with `fx = SURF[0x39]`, `fy = SURF[0x3d]` (1.0 unscaled). The packet is
`0x4104000b, 0x00000002, 0x00000160, 0x00000001, 0x00000000` + 8 payload dwords.
**Under today's `diff-gl-variants.py` the `big` variant should therefore show
exactly two register differences, `0x132c` and nothing else** — if it shows more,
something drawable-derived that I have not found is reaching a register.

Placing the test window at a non-zero screen position in one extra run would
settle whether the drawable origin lands in `0x1328/0x132c`, in `0x161.d0/d1`, or
in both. That is cheap and worth adding to the matrix.

---

## 5. Does the WARP vertex program change?

### 5.1 A structural correction first

`parhelia-shading-texturing-capture.md` concludes that texturing does not change
the vertex program because "the key `0x05` WARP bind is unchanged
(`(0x13,1)(0x14,2)`)". **That record is not a WARP bind.** Key `0x05` slot `0x14`
does `[0x12ef] = v + 1` and calls `mtx424`, and the live sweep pins the same pair
to `reg 0x1050 = 3` — "the register that selects which uploaded **fragment**
program a draw uses" [HW, FM §8; `parhelia-state-slot-map.md`]. The ICD compiles
only the pixel/combiner program (records `0x45`/`0x46`); **the WARP vertex
program is generated in the kernel** and reaches the ring as `0x1100`, `0x10f8`,
`0x10c0`, `0x10c8..0x10d8` written by `mtx3400`/`mtx693`/`mtx1814`/`mtx676`
[BIN KRN; `parhelia-warp-binding.md`]. That sentence in the capture document
should be corrected, and with it the paradox it created disappears.

### 5.2 What actually selects a program

`mtx3558` (`.text+0xa0f20`) is **not** a decision point — it is a pure linker over
three flat vectors (recipe at `engine+0xa0`, relocs at `+0xe8`, patches at
`+0xc0`, fragments copied from `engine[0x9c108 + 24*frag]`)
[BIN KRN 0xa0f20–0xa1080]. The decision lives in the emission tree `mtx2179`
(`0xa2400`) and, decisively, in the **cache comparator `mtx3930` (`0xa6e60`)** —
whatever `mtx3930` compares *is* the program key. Read field by field
[BIN KRN 0xa6e60–0xa7440], it compares: transform/vertex-blend mode `K[0x10]`
(short-circuit at 0xa6eb2 — `0` = pre-transformed takes a wholly different arm at
0xa7170), `K[0x04] & 0x90a`, texture-array and raster/fog flags, `GL_LIGHTING`,
`GL_LIGHT_MODEL_LOCAL_VIEWER`, `GL_FOG`, user clip-plane count `K[0x08]>>5`,
normal-transform need, fog mode/coord, the light counts and colour-material
sources, output-format class, **point-size flag `K[0x04] bit2` then `K[0x48]`**,
last-texture-stage flag, **`nTex` = `G[0x2a0]->u32[8]` (0xa7312)** and a per-unit
loop over the texcoord descriptors (0xa7334–0xa7368).

**It compares no depth-test, blend, cull, shade-model, primitive-type,
primitive-count, point-size-*value*, line-width-*value* or window-size field.**
Those live in a different object entirely (`HWSTATE`) and go out as mechanism-A
registers and mechanism-B block loads.

### 5.3 Verdict per variant

| variant | WARP program | evidence |
|---|---|---|
| points / lines / linestrip / tristrip / trifan / quads (size 1, width 1) | **no** | primitive is not in `mtx3930`; it is a rasteriser word (`0x10b0`) plus the vertex count in the draw packet [BIN KRN] |
| 2 triangles | **no** | vertex count only |
| flat shading | **no** | shade model is `HWSTATE[0x1be4][7:6]`, a different object [BIN KRN 0x17ba7]; and [HW] the flat and Gouraud programs were identical |
| per-vertex colours | **no** | same layout, same fragment set (`0x54` colour pass-through either way) |
| `GL_DEPTH_TEST` | **no** | the transform is four unconditional `dp4`s on `r0.xyzw`; missing components come from the default attribute vector `0x1120..0x112c = 0,0,0,1.0f` [BIN KRN `mtx3400` 0x43c06]. The PIPELINE-KEY block at `HW+0x1e04` is **written and never read** in `mtx_parhelia.o` [BIN KRN, exhaustive scan] — it is a shadow, not a codegen input |
| `GL_BLEND` | **no** | idem; alpha likewise comes from the default vector |
| `GL_CULL_FACE` | **no** program regeneration — but see §4.7's secondary effect: cull field `== 1` **skips** the `mtx2316` call in `mtx3400` [BIN KRN 0x43cd0], so the *upload* section differs |
| **2x2 texture, MODULATE** | **YES** | `nTex` is in the cache key (0 -> 1 can never hit) [BIN KRN 0xa7312]; `mtx2177` (`0xc0380`) emits a component-fill fragment `0xa7` plus one texgen fragment per active coordinate from the 28-entry tables at `engine+0x9f6f0`/`+0x9f760` — for a 2-component, non-texgen, no-matrix `GL_MODULATE` stage that is `0xa7` + `0x91` + `0x96` [BIN KRN; `parhelia-warp-codegen.md` §1.7]. Texture state also moves five key bits via `mtx3483` (`0x85fa0`) |
| `glPointSize(8)` | **YES** (via the software path) — and **possibly even in hardware mode** | `mtx740` (`0x864f0`) sets `K[0x04] \|= 4` and `K[0x48] = 3`, which makes `mtx2179` emit the point fragment `0xc4`/`0xc5` and force the `0x4e`/`0x4f` tail [BIN KRN 0x865b4–0x865e6]. Its gate reads `G[0x09]` bits 3/4/5 against `G[0x0c][5:4]` [BIN KRN 0x86515–0x86550] — that field is **[OPEN]**, candidates being `D3DRS_FILLMODE` (1 POINT / 2 WIREFRAME / 3 SOLID) or a primitive class. Independently, the ICD's sized-point path pre-transforms, which drives `K[0x10] -> 0` and short-circuits `mtx3930` into the pre-transformed arm |
| `glLineWidth(8)` | **YES, drastically** | same pre-transform argument; the program becomes the short `0x01`/`0x02` head chain instead of `0x50`/`0x54`/`0x4d`. [INF] — that GL wide lines specifically drive `K[0x10]` to 0 is reasoned from the pre-transformed vertex layout [HW], not read directly |
| 640x480 window | **no** | the viewport is consumed by the setup unit, never by the WARP |

For the baseline (one smooth triangle, no lighting/fog/texture/clip) the recipe is
the four-fragment, 13-instruction program `0x50` (+4 `dst1` patches) -> `0x54` ->
`0x4d` -> `0xc8`, 156 bytes [`parhelia-warp-codegen.md` §4.2], which is exactly
the program `tools/parhelia-warpasm.py` reproduces bit for bit [HW].

### 5.4 Output set, and a naming correction

Outputs are **port-B registers**, not "o*n*": the setup unit reads position from
port-B 0, B1/B2 are the colours, B3+ the texcoords; opcode `0x60` writes the
port-A temp file, never an output [HW, `parhelia-warp-output-b0`]. The set is a
consequence of which fragments `mtx2179` emits:

| output | fragments | emitted when |
|---|---|---|
| position | `0x50..0x53` (or `0x06..0x09` with a normal); `0x01`+`0x02` on the pre-transformed path | always |
| primary / secondary colour | `0x54` pass-through when `!(G[0x08]&8)`, else `0x55/0x56/0x57` | always |
| texcoords B3..B6 | `mtx2177`: `0xa7` + one of the 56 texgen fragments `0x6f..0xa6` per (unit, S/T/R/Q) | only when `nTex > 0` |
| clip | `0xb0 + (tm-1)` for `tm = K[0x08]>>5`, then `0xb6` | only with user clip planes |
| point size | `0xc4` (`K[0x48]==3`) or `0xc5` | only when `K[0x04] & 4` |
| fog | `0x5a..0x64` | only when fog is enabled |
| terminator | `0xc8` | always |

[BIN KRN `mtx2179` 0xa2400; `parhelia-warp-codegen.md` §1.2]

### 5.5 What a program change looks like in the ring

Watch, in order of usefulness:

1. **`0x1100` (idx `0x40`) and `0x10f8` (idx `0x3e`)** — the two generated
   fixed-function entries, from `S[0x598]`/`S[0x5a0]` [BIN KRN `mtx3400` 0x43d89].
   On the 0528 heap path the entry encodes a bus address into a 682-slot heap of
   `0x600`-byte slots, so **a new program object moves the entry dword by exactly
   `0x200`** — the capture's four binds are `08a57183 / 08a57383 / 08a57583 /
   08a57783` [HW]. That `0x200` step is the reliable signature; the absolute
   value is per-boot noise.
2. **`0x1118` (idx `0x46`)** = `S[0x610]`, default `0x6543`; per texture unit `i`,
   nibble `4*i` = `(perStageComponentCount & 7) + 3`
   [BIN KRN `mtx3654` 0x8c9e8–0x8ca1d]. **Texture on/off moves this word** — with
   one 2-component stage, nibble 0 goes `3 -> 5`. Together with `0x1110`
   (`S[0x61c] | S[0x608]`) and `0x1114` (`S[0x60c]`) this is the vertex-format
   group; the baseline is `0x1110 = 3`, `0x1114 = 0x287210`, `0x1118 = 0x6543`
   [CAP].
3. `0x10c0`, `0x10c8..0x10d8` (idx `0x30`, `0x32..0x36`) — stage entries, all six
   always written, `2` = unbound; they move together with the program.
4. `0x10f0` (idx `0x3c`) — WARP mode latch, `0x20000028`; its low nibble is
   shadowed in `S[0x57c]`, which `mtx740` sets to 6 / bit 7 on the point path
   [BIN KRN 0x86598, 0x8655c]. A nibble change here means a point/line path change.
5. `0x1144` (idx `0x51`) = `S[0x618]`, zeroed on the wide-line path (§4.10).
6. `0x918` / `0x91c` upload burst length, in streamed (non-heap) mode — the most
   direct proof the microcode changed, but the 0528 capture is in heap mode.
7. `0x1120..0x112c` should stay `0,0,0,1.0f` in **every** variant; if the depth
   or blend variant moves them, the driver is compensating for a missing
   attribute in a way this document did not predict.

---

## 6. `parhelia-gl-feature-map.md` §4's stride table, fully explained

That table is the main reason to expect vertex-layout changes from depth and
blend, and it has been the source of two contradictory readings. Reading
`tools/parhelia-glsweep.c` — the harness that produced it — against the ICD
finding of §1.1 explains every row with no free parameters, and **removes** the
apparent depth/blend layout effects:

* the sweep's helper is `tri(z)` = three `glVertex3f(x, y, z)`;
* `tri_flat`, `points`, `lines`, `quad`, `tristrip`, `trifan` call `tri(0)` -> Z
  is 0.0f -> position stays `x,y` -> **stride 20**;
* `depth_on`, `depth_func_gequal`, `depth_mask_off` call **`tri(0.5f)`** -> the
  `ucomiss` against 0.0f fails -> `SetPositionSize(1)` -> position `x,y,z` ->
  **stride 24**. The Z is the harness's, not the depth test's;
* the position code is **sticky**: `SetPositionSize` is only ever called to
  *raise* it from a vertex handler, and the `glVertex3f` fast path with `z == 0`
  simply appends at the current stride [BIN ICD 0xc8e70–0xc8f3d]. So every op
  *after* the depth block — `stencil`, `scissor`, `cullface`, `polymode`, `fog`,
  `viewport` — keeps `x,y,z` and reports stride 24 although none of them needs Z;
* `blend_srcalpha` and `blend_additive` call **`glColor4f(1,0,0,0.5f)`**, whose
  alpha is not 1.0, so the colour is raised to RGBA on top of the sticky `x,y,z`
  -> key `0x51`, **stride 28**. Neither the blend enable nor the blend function
  contributed;
* `alphatest` likewise uses `glColor4f(…, 0.8f)`;
* `lighting` supplies a `glNormal3f` -> key `0x05`, stride 24;
* `linewidth` is the software path -> key `0x852`, stride 32 (§4.10).

**Consequence for this matrix:** `gltest.c` uses `glVertex3f(x, y, 0.0f)` and
`glColor3f` in *every* variant, so unless a hidden requirement exists, the vertex
layout should be constant at 20 bytes across the whole matrix **except** for the
texture variant (a real requirement, §4.8) and the two software paths (§4.9,
§4.10). That makes `gltest` the clean experiment the older sweep was not — and it
makes the draw header `[22:17]` field the single best summary statistic to watch.

---

## 7. Prediction summary

Draw header is `0x97080001` unless stated. "empty" = no `0x1000..0x13ff`
register difference expected.

| variant | registers that must change | vertex layout | WARP | confidence |
|---|---|---|---|---|
| `points` | `0x10b0` -> `0x00020020` / `0x00060020` | — | maybe (§5.3) | high on `0x10b0`, [OPEN] on the program |
| `lines` | `0x10b0` -> **`0x0002103d`** / `0x0006103d` | — | no | high |
| `linestrip` | `0x10b0` -> `0x0002203d` / `0x0006203d`; possibly `0x1104` | — | no | high / low on `0x1104` |
| `tristrip` | `0x10b0` -> `0x0002405c` / `0x0006405c` | — | no | high |
| `trifan` | `0x10b0` -> `0x0002505c` / `0x0006505c` | — | no | high |
| `quads` | `0x10b0` -> `0x0002605c` / `0x0006605c` | — | no | high |
| `tri2` | **empty**; operands 60 -> 120 B | — | no | high |
| `flat` | **empty** (possibly one `0x41` block dword) | — | no | high |
| `pervert` | **empty** | — | no | high |
| `depth` | `0x12c8` `0x001000e4` -> **`0x001000e5`**; `0x12d0` `0xff` -> **`0x1ff`** | unchanged | no | high on `0x12c8`; medium on `0x12d0`; medium-high on "layout unchanged" |
| `blend` | `0x12e8` `0x00800202` -> **`0x0080eced`** | unchanged | no | high on `0x12e8`; medium on "layout unchanged" |
| `cull` | **`0x1130`** `0x…300c` -> **`0x…300d`**; `0x12fc` unchanged | unchanged | no (upload section differs) | medium-high — this is the prediction most worth testing |
| `tex` | `0x1200`, `0x1204..0x120c`, `0x11c0`, `0x11c4`, `0x11cc`, `0x11dc`, `0x11e0` = **`0x32000002`**, `0x11e4` = **`0x00010002`**, `0x11e8`, `0x1264` -> `0x08000000`, `0x126c` -> `0x01400000` (+alpha twins), `0x1118` nibble 0 `3 -> 5`, `0x1100`/`0x10f8` | **`x,y,r,g,b,s,t`**, stride 28, header **`0x970c0001`** | **yes** | high |
| `psize` (vs `points`) | `0x10b0` unchanged; `0x1100`/`0x10f8`; block `0x172` d3 = 4.0f | **pre-transformed**, stride 32, header **`0x970e0001`** | yes | medium — the software-path *fact* is high, the exact key/descriptors are [INF] |
| `lwidth` (vs `lines`) | `0x10b0` unchanged; `0x1144` -> `0`; `0x1100`/`0x10f8`; block `0x178` d2 = 4.0f | **pre-transformed**, stride 32, header **`0x970e0001`**, 16 vertices / 512 B | yes | medium-high ([HW] for the key and stride, [INF] for the rest) |
| `big` | `0x132c` `0x00f00140` -> **`0x01e00280`**, and nothing else | — | no | high |

---

## 8. Open items, with candidates

1. **`0x10b8` field `B`** for a texcoord attribute. `B = 0` for position and 2 for
   the diffuse colour [CAP]; both are float vectors, so `B` is not a numeric
   type. Candidates for the texcoord descriptor: `0xa0c07050` (`B=0`),
   `0xa0c27050` (`B=1`), `0xa0c47050` (`B=2`). Field `A = 7` is well founded
   (§3.1).
2. **Whether blending pulls alpha into the vertex.** §4.6 predicts no; the
   discriminator is the draw header. The unread code is ICD `0xc1e30`'s producer
   of `ctx+0xc988`.
3. **The point-size codegen gate**, ICD-independent: `mtx740`'s test of `G[0x09]`
   bits 3/4/5 against `G[0x0c][5:4]` [BIN KRN 0x86515]. Candidates for
   `G[0x0c][5:4]`: `D3DRS_FILLMODE` (1 POINT / 2 WIREFRAME / 3 SOLID) or a
   primitive class. This decides whether plain `GL_POINTS` at size 1 already gets
   a different WARP program than triangles.
4. **`0x1130`'s remaining bits.** `[1:0]` are now cull (§4.7). The rest of
   `0x0700300c` / `0x0200300c` is built by `mtx3654` at `0x8b923`, `0x8c091`,
   `0x8c4ab`, `0x8c533`.
5. **`0x12fc[1:0]`'s enum**, and what GL state (if any) drives key-`0x0c` slots
   `0x02` / `0xa1`. Not emitted anywhere in the 64-bit ICD; likely a D3D-compat
   or 2D-side path. The scan matched the first slot store after each
   `mov esi,0xc`, so a slot buried as the 2nd..Nth pair of a multi-pair record
   could have been missed — low risk, not zero.
6. **Whether the hardware line half-width (`0x178` d2) is inert while the CPU
   expansion runs** (§4.10). A `GLTEST_LWIDTH` 1/2/3 sweep with a pixel count
   settles it.
7. **Where the drawable origin lands** — `0x1328/0x132c`, `0x161.d0/d1`, or both.
   One run with the window at a non-zero screen position settles it.
8. **`0x1114 = 0x287210` / `0x1118 = 0x6543`.** `0x1118`'s per-unit nibble rule is
   read [BIN KRN 0x8c9e8]; `0x1114`'s layout is not. The `tex` variant is the one
   capture that will move this group.
9. The clip fragments accumulate into port-B 5 in their unrelocated form while
   B3+ are texcoords [HW]. Either the clip accumulator is relocated at link time
   or B5 is shared. Not resolved, and not exercised by this matrix.

## 9. Two changes to the instrument, worth making before the run

1. `diff-gl-variants.py` should decode `0x41` block-load payloads (block index +
   dwords) and print each draw packet's `end - start`. Without the first, cull's
   secondary effect, the viewport, the point radius and the line half-width are
   all invisible; without the second, six of the fifteen variants have no
   observable delta at all.
2. `psize` and `lwidth` must be diffed against `points` and `lines`, not against
   `base` — each differs from `base` in two states. `diff-gl-variants.py` already
   takes the baseline name as `argv[2]`; run it three times.
