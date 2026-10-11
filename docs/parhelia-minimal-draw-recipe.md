# The minimal Parhelia-512 draw: every dword justified

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

What is genuinely **required** to put one triangle on the screen, as opposed to
what merely appears in the vendor's stream. The vendor re-emits its whole
context every frame; this file separates that into three classes and gives the
ordered stream that survives the cut, with a line of evidence for every dword.

Tags: **[CAP]** read out of a capture in `captures/`, **[BIN]** read from a
vendor binary, **[HW]** measured on the board, **[INF]** inferred.
Generator: `tools/gen-minimal-draw.py`. Companion reading:
`parhelia-3d-integrated.md` (framing and submission),
`parhelia-draw-sequence-annotated.md` (the full vendor sequence),
`parhelia-findings-crosscheck.md` (the hardware attribution matrix).

---

## 1. The measurement that settles required-vs-redundant

`captures/gl-matrix/base-secs.txt` is a synthetic GL client drawing many
primitives per frame. Framed under the rules of `3d-integrated` §2 it contains
**2569 sections, 6370 packets and 957 draw packets**, and the gap between
consecutive draws is:

| events between draws | occurrences |
|---|---|
| **4** | **931** |
| 121 | 19 |
| 5 | 4 |
| 77, 38 | 1 each |

**931 of 956 consecutive draws are separated by exactly three register
packets.** [CAP] Those three packets are byte-identical across all 931, and
there are only **two** distinct forms in the whole capture — one for triangles
and one for lines, differing in a single register:

```
=== SEC …c32..…c43 (4 dw) ===          ; the call before the draw
00000002 21211000 00000003 00000004    ; 0x1000 = 3, 0x1040 = 4
=== SEC …c7e..…cd7 (22 dw) ===         ; the call containing the draw
97080001 19561c40 19561c7c             ; DRAW, 0x3c bytes = 3 verts x 20
00000001 2121212c 0006305c             ; 0x10b0 = primitive word
0000000e 03020100 00000002 00000000 00000000 00000000
         07060504 00000000 00000000 00000000 00000000
         13121110 00000004 00000000 00000000 00000000
```

Read in execution order that is:

```
0x10b0 = 0006305c            (triangles)   |   0006103d  (lines)
0x1000..0x101c = 2,0,0,0,0,0,0,0
0x1040..0x104c = 4,0,0,0
0x1000 = 3 ; 0x1040 = 4
DRAW 0x97080001 <start> <end>
```

**Fifteen register writes, and the only one that differs between a triangle
draw and a line draw is `0x10b0`.** [CAP] That is the empirical upper bound on
the per-draw REQUIRED set. Everything in the 121-event frame span — **529
register writes across 104 distinct registers** [CAP], ten `0x41` block loads,
two priming pseudo-draws, the legacy DDA plane file and the X server's 2D
blits — is either context that is set once, or state re-emitted because the ICD
marks it dirty every frame. (The live-ring capture of the vendor GL client
gives the same picture at 712 writes in 113 registers over its 185-packet
period, `parhelia-draw-sequence-annotated.md` §5.)

It reduces further. `0x1000` is written 2 and then immediately 3, so the 2 is
dead; `0x1004..0x101c` and `0x1044..0x104c` address streams 1–7, and both
`0x10b8` descriptors declare stream 0 (`[2:0] = 0`) [BIN], so those streams are
never fetched. **The irreducible per-draw core is four items:**

```
0x10b0 = primitive word
0x1000 = 3          (memory class of the vertex buffer)
0x1040 = 4          (copy-path placeholder stride)
0x97080001 start end
```

### The one-register attribution

`captures/gl-matrix/attribution.txt` isolates each GL state change to a single
frame span. Between `base` (smooth triangle) and `flat` (`glShadeModel(GL_FLAT)`)
**exactly one 3D register moves**: `0x1130: 0200200c -> 0200300c`. [HW] Between
`base` and `cull`, exactly one: `0x1130: 0200200c -> 0200200d`. So a
flat-shaded triangle is the baseline stream with bit 12 of `0x1130` set — the
Gouraud and flat register sets are otherwise identical, because the hardware
always interpolates and `GL_FLAT` is implemented by replicating the provoking
vertex colour. [HW][INF]

---

## 2. Classification

**REQUIRED** — the draw cannot work without it, and it must be re-emitted per
draw. **CONTEXT** — must be set once before the first draw; sticky afterwards.
**REDUNDANT** — present in the vendor stream, never load-bearing for a single
triangle into an idle engine.

### 2.1 REQUIRED (per draw)

| reg | value | justification |
|---|---|---|
| `0x10b0` | `mtx734[prim] \| topology<<12 \| 0x60000` | re-armed before all 931 steady-state draws; the only register that differs between primitive types [HW][CAP][BIN] |
| `0x1000` | `3` | vertex stream 0 base OR'd with the memory class; 3 = PCI bus-physical, the class of a system-memory vertex buffer. `mtx1017+0x4bf` re-emits it immediately before every draw packet [BIN][CAP] |
| `0x1040` | `4` | stream 0 stride. On the copy path this is a *placeholder*; the real per-vertex size travels in draw header bits [22:17] [BIN][CAP] |
| `0x1300` | `0x00a60ec4` | the 3D draw opcode word: opcode 4 \| BOP 0xC (COPY) \| the constant `0x00860e00` \| bit 21. Setting it *before* the WARP bind stalls the engine, so it is required but its position is a context ordering rule, not a per-draw write [BIN][CAP] |
| draw packet | `0x97080001 start end` | `0x17` \| bit 31 (GL path, from `ctx+0x42c`); `[22:17]` = dwords per vertex − 1 = 4; low 16 = totalDwords − 2 = 1. Operands are plain byte addresses, `end` exclusive. The packet **is** the launch — there is no 3D go register [BIN][HW] |

### 2.2 CONTEXT (once)

Grouped as the generator emits them. "verbatim" means the value is replayed
from the capture because no bit of it is decoded.

| group | registers | justification |
|---|---|---|
| scissor + raster | `0x1328`, `0x132c`, `0x12fc`, `0x12f8` | DWGCTL bit 23 enables the scissor, so `0x1328`/`0x132c` must hold a valid rectangle. `0x12fc` `[1:0]` = cull select; the rest of `0x0bb80024` and all of `0x12f8` (`reg3DThresholdAA`) are **verbatim** [BIN][CAP] |
| blend / alpha | `0x12e8 = 0x00800202`, `0x12e4 = 0x0000000e`, `0x12ec`, `0x12f0` | bit 0 of each is the enable; both clear. Blend factors ONE/ZERO, alpha func ALWAYS [BIN][HW] |
| depth / stencil | `0x12c8 = 0x001000e4`, `0x12cc`, `0x12d0 = 0xff`, `0x12c0`, `0x12c4` | `0x12c8` bit 0 = depth test (clear), bit 4 = stencil (clear); `0x12d0` bit 8 = depth write (clear). `0x12c0/0x12c4` are the z-range clip limits [BIN][HW] |
| fog | `0x12d4 = 0`, `0x12d8 = 0`, `0x12dc = 0x00ffffff` | `0x12d4` bit 0 = enable; `0x00ffffff` is the documented fog-off end value [BIN] |
| vertex format | `0x1120..0x112c`, `0x1110`, `0x1114`, `0x1118`, `0x111c`, `0x1144` | **verbatim**. `0x1118`'s per-unit nibble rule is known (`components+3`, `0x6543` = no texture) [BIN]; nothing else in this group is decoded. `0x1120/0x1124` are the packed halves of ±8160.0f, i.e. the guard band, *not* a float default-attribute vector [CAP] |
| WARP bind | `0x1374`, `0x10f0` ×2, `0x1100`, `0x10f8` | the vendor's canonical bind packet (`mtx693`): strobe, mode twice, entry. `0x10f0` low nibble on the per-vertex bind is the primitive class [BIN][HW] |
| WARP arm | `0x1374`, `0x10f0` ×3, `0x1130`, `0x111c`, `0x1144` | the third `0x10f0` write drops bit 29; `0x1130` carries the three decoded fields (cull bit 0, flat bit 12, class [25:24]) over the captured undecoded remainder [BIN][HW] |
| vertex fetch | `0x1064 = 0`, `0x1060 = capacity`, `0x10b4 = 0`, `0x10b8` ×2 | `0x1064` first usable index, `0x1060` the fetch limit — **must not be zero** [BIN]. `0x10b4` is the descriptor write index, `0x10b8` the data port |
| render target | `0x1220`, `0x1224`, `0x1230`, `0x1234` | `cfg = pitch_px \| type<<16 \| fmt<<24`; slot 0 is the 3D colour target on three independent witnesses [BIN]. Slot 2 is nulled with `0x32020000` (pitch 0) |
| masks | `0x1324 = 0xffffffff`, `0x1314 = 0`, `0x1318 = 0` | PLNWT is the confirmed colour write mask; FCOL/BCOL are the 2D colours, zeroed on the 3D path [BIN] |
| draw arm | `0x12b0`, `0x1300`, `0x12f4` | `0x12b0` **verbatim**; `0x12f4` is the guard band, 32.0f for ≤ 2 texture stages [BIN][CAP] |
| texture units off | `0x121c = 0`, `0x1200 = 0x0001000f`, `0x1204..0x120c = 0` | `0x1200[18:16] = 1` means *disabled*; a disabled unit gets **no** `0x11c0..0x11e4` sampler block at all. `0x121c = 0` before the unit block is an ordering rule from the kernel [BIN] |
| combiner | `0x1264/0x1260/0x126c/0x1268`, `0x1278..0x1284`, 4 pad slots | slot 0 = `MOV_sat r0, v0` in both halves — the interpolated vertex colour straight to the fragment output. Pads are `MOV r0, r0` in slots 2/4/8/10 [BIN] |

### 2.3 REDUNDANT

| what the vendor emits | why it is not needed |
|---|---|
| `0x1300 = 0x001000f4` / `0x005000f5` priming pseudo-draws, each followed by four texture-unit blocks (`0x1200+4u`, `0x11c0..0x11e4`) and the legacy DDA attribute planes `0x1164..0x1198` — **~200 register writes per frame** | the `0x1160..0x119c` plane file is the *legacy DDA* path, explicitly not the WARP vertex path; these are the kernel's per-frame one-pixel replay [BIN][CAP] |
| `0x1160..0x1198` (70 writes) | same |
| `0x11c0..0x11e4` sampler blocks | a unit with `0x1200[18:16] == 1` never reads them [BIN] |
| 2D traffic: `0x138c` LEN, `0x1228/0x122c` rebinds to `0x30030020`, `0x1314 = 0xff000000`, `0x1300 = 0x5c0`/`0x20c0`, `0x1304`, `0x1308` | the X server's fills sharing the same ring; nothing to do with the GL draw [CAP] |
| `0x1300 = 0x307`/`0x407`/`0x707` + `0x12bc` tag pairs | DWGSYNC soft traps: ordering only, "the flush before a `0x12b0` surface rebind, not needed to draw" [BIN]. Required again as soon as a second draw reads a surface a previous draw wrote |
| `0x1090` / `0x1094` fence write-back | completion signalling. Our replay polls bus-master status instead. Not required to *draw* [CAP] |
| `0x1000..0x101c = 2,0…` and `0x1044..0x104c = 0` in the per-draw packet | `0x1000` is overwritten with 3 two dwords later; streams 1–7 are never fetched because both `0x10b8` descriptors carry `[2:0] = 0` [BIN] |
| the trailing `0x10b8 = 0, 0x10b8 = 0` | descriptor 1 already carries the last-attribute flag (bit 31) [BIN] |
| `0x10b0 = 0x0002305c` + `0x10b8 = 0x83c00000` before the WARP binds | the kernel's own one-attribute `xyzw` self-test declaration, overwritten by the real primitive word and descriptors before the draw [BIN][CAP] |
| `0x1378` | payload is visibly garbage in the captures (`0x83828120`, `0x3f800000`, `0x0006305c`, `0x19613d14` …), i.e. a strobe like `0x1374`; never carries information [CAP] |
| `0x1238`/`0x123c` (surface slot 3) | written once at engine init only, never in a draw span [CAP] |
| block loads for materials, lights, texture matrices, projection, fog, point size, clip planes (`c[0x0E0]`, `c[0x134]`, `c[0x149]`, `c[0x155]`, `c[0x172]`, `c[0x178]`, `c[0x17d]`) | never sampled with lighting, fog and texturing off. The attribution matrix confirms: `c[0x0E0..0x0E6]` appear *only* in the `light` variant, `c[0x178]` moves *only* in `polyoff`, `c[0x172]` *only* in `psize` [HW] |

**One documented recommendation is contradicted here.**
`parhelia-vertex-fetch-and-memory-classes.md` §"what the replay must do" item 5
says to keep both `0x10b0` writes (`0x0002305c` then `0x0006305c`). The
steady-state evidence is stronger: 931 draws re-emit only the `0x0006` form,
and the `0x0002` form is a dead store. Item 3 of the same list — keep
`0x1000 = 3`, `0x1040 = 4`, `0x1064 = 0`, and do not zero `0x1060` — is
confirmed and kept.

---

## 3. The minimal stream

24 packets, **128 section dwords, 74 register writes in 60 registers**, against
the vendor's 529 writes in 104 registers for one frame. Target parameters are
`--rt-base`, `--pitch`, `--fmt`, `--screen`, `--viewport`.

Generated for a 1920×1200 ARGB8888 target at VRAM byte offset `0x02000000`,
flat-shaded triangles, WARP program at bus `0x1a001000`:

```
 0 [ 6dw] 1328=00000000 132c=04b00780 12fc=0bb80024 12f8=007fe000
 1 [ 6dw] 12e8=00800202 12e4=0000000e 12f0=00000000 12ec=00000000
 2 [ 8dw] 12c8=001000e4 12cc=000000ff 12d0=000000ff 12c0=80000000 12c4=7fffffff
 3 [ 5dw] 12d4=00000000 12d8=00000000 12dc=00ffffff
 4 [ 6dw] 1120=45ffc5ff 1124=c5ff45ff 1128=00000000 112c=3f800000
 5 [ 5dw] 1110=00000003 1114=00287210 1118=00006543
 6 [ 8dw] 1374=00000000 10f0=20000002 10f0=20000002 1100=08aab003 10f8=00000002
 7 [10dw] 1374=00000000 10f0=20000002 10f0=20000002 10f0=00000002
          1130=0200300c 111c=00000fdb 1144=00000000
 8 [ 5dw] 1064=00000000 1060=000280cc 10b4=00000000
 9 [ 4dw] 10b8=20c00000 10b8=a1c50020
10 [ 6dw] 1220=02000000 1224=32020780 1230=00000000 1234=32020000
11 [ 5dw] 1318=00000000 1314=00000000 1324=ffffffff
12 [ 3dw] 12b0=3fff8842
13 [ 3dw] 1300=00a60ec4
14 [ 3dw] 12f4=42000000
15 [ 8dw] 121c=00000000 1200=0001000f 1204=00000000 1208=00000000 120c=00000000
16 [11dw] 1264=00000200 1260=00000200 126c=01400400 1268=01400400
          1278=00000000 127c=00000000 1280=00000000 1284=00000000
17 [ 4dw] 1264=00000082 126c=00400400
18 [ 4dw] 1264=00000084 126c=00400400
19 [ 4dw] 1264=00000088 126c=00400400
20 [ 4dw] 1264=0000008a 126c=00400400
21 [ 3dw] 10b0=0006305c
22 [ 4dw] 1000=00000003 1040=00000004
23 [ 3dw] DRAW 97080001 <vtxstart> <vtxend>
```

Packets 21–23 repeat per additional triangle. Ordering constraints, all of
them real:

1. `0x121c = 0` **before** the `0x1200..0x120c` unit block [BIN].
2. `0x1300 = 0x00a60ec4` **after** the WARP bind — a draw opcode armed before
   the program is bound stalls the engine (paid for once already, see
   `tools/gen-tri-stream.py`) [HW].
3. `0x10b4 = 0` **before** the `0x10b8` descriptor burst [BIN].
4. In `--program transform`, the `0x41` block load must follow the
   constant-loader bind and precede the per-vertex bind: an `0x41` packet is
   interpreted as a constant write only while the loader is the bound program.

### 3.1 Vertex layout and the draw operands

Five floats, 20 bytes, `struct vertex { float x, y, r, g, b; }` — confirmed
against known input in `parhelia-minimal-triangle.md` and against our own
replayed colour on hardware [HW]. The two descriptors declare it:

* `0x20c00000` — stream 0, dword offset 0, component mask `xy`, slot 0
* `0xa1c50020` — stream 0, dword offset 2, component mask `xyz`, last attribute

Draw header `0x97080001` = `0x97<<24 | (5−1)<<17 | (3−2)`. `start`/`end` are
plain byte addresses of the vertex bytes, `end` exclusive, 60 bytes for three
vertices. **The vertex bytes must lie outside the executed command range**: a
float like `20.0f = 0x41a00000` inside it parses as an `0x41` block-load header
and locks the engine [HW]. `tools/parhelia-3dtri.c` parks them at
`SEC_OFF − 256` dwords for exactly this reason.

### 3.2 The WARP program

Default (`--program passthrough`) — four instructions from
`tools/parhelia-warpasm.py`, reading **no constants at all**:

```
add o0, v0, v0.0000                 00083e00 00440000 b89a2249
add o1, v5, r1.0000 | mov o11, v0   08087ebf 00440215 b89a2249
end                                 00000200 00040000 14c00000
wra                                 00080200 00440000 301a21a2
```

`o0` is window-space position, `o1` the colour, `o11` the clip position. With
this program the vertices are supplied in **window coordinates** and there is
nothing else to upload: no constant file, no constant-loader program, no
base-register setup program, no `0x41` packet, no `b0..b3`, no `a0`.

`--program transform` is the task's preferred shape — our own identity MVP and
our own viewport, no vendor payload spliced into the constants:

```
add T0, v0, v0.0000                   dp4 r2.w, T0, c3
dp4 r2.z, T0, c2                      dp4 r2.y, T0, c1
dp4 r2.x, T0, c0
add o1, v5, r1.0000 | mov T1, c5
smul r5, r2, r2.xyz1 | mov o11, r2
add o2, v6, r1.0000
mad T1.xyz, c4.xyz1, r5, T1.xyz0
add o0, T1, r1.0000
end / wra
```

This is the vendor's own fixed-function program with two changes, and only
two: the matrix rows are absolute `c0..c3` instead of `c[a0.x+k]`, and the
viewport pair is absolute `c4`/`c5` instead of `c[b1+0]`/`c[b1+1]`. Every word
differs from the vendor's only in the constant-addressing fields — instruction
8 is `2c105e00 00440191 bb6a2222` against the vendor's
`2c105e00 22440181 bb6a2222`. The assembler reproduces the vendor's own
13-word program bit-for-bit (`--gen transform --matrix a0 --tex 4 --vs
captures/live-ring-0528/warp-programs/slot-16c06200.txt --vs-entry 96` →
IDENTICAL), so the encoder is trustworthy.

Making both operands absolute **removes the base registers entirely**, and with
them the kind-1 setup program and its `0x4106000f` packet. The viewport numbers
are the measured formula, just at slots 4/5 instead of `0x160`/`0x161`: the
slot index carries no hardware meaning beyond what the program asks for. For an
800×600 viewport the generator emits

```
c[0] c[1] c[2] c[3] = identity
c[4] = (400, -300, 0.5, 1)      c[5] = (400, 300, 0.5, 600)
```

which is exactly what the vendor's `big2` variant put in `c[0x160]`/`c[0x161]`
for the same window [HW]. Slots 4–6 are the normal-matrix rows of palette entry
0 and are dead with lighting off.

### 3.3 Binding it

```
0x1374 = 0                     strobe, payload ignored
0x10f0 = 0x20000002            mode, written twice; low nibble = primitive class
0x10f0 = 0x20000002
0x1100 = (progbus/3) & ~3 | 3  the entry
0x10f8 = 2                     shader-object slot 2 = unbound
```

The entry is an instruction address in units of a quarter instruction —
instructions are 12 bytes, `addr/12 × 4 = addr/3` — with the memory class in the
two low bits. **The program buffer address must be a multiple of 12**, which is
why the vendor rounds its heap base up to `0x180`; `gen-minimal-draw.py`
refuses a `--warp-bus` that is not. Verified against the capture:
`0x07957783 & ~3 = 0x07957780`, `×3 = 0x16C06680`, minus slot base `0x16C06200`
= `0x480`, `/12` = instruction 96 [HW].

There is no start register. The bind is latched state; the draw packet in the
ring, with `0x408 = end | 3`, is what makes the program run [BIN][HW].

---

## 4. Using the generator

```
tools/gen-minimal-draw.py OUT --warp-bus 0x1a001000 --flat \
        --rt-base 0x2000000 --pitch 1920 --screen 1920x1200 --selfcheck
```

Writes `OUT.hex`, `OUT-ring.hex`, `OUT-sec.hex`, `OUT-reloc.txt` (the
`ringIdx secOff secLen drawOperandIdx startTag endTag` form
`tools/parhelia-3dtri.c --ringreplay` consumes), `OUT-warp.hex` (the program
words to place at `--warp-bus`) and `OUT-justify.txt` (the per-register
table). Each packet becomes its own secondary call with tags `(2, 3)` — the
cadence the hardware accepts; a single call over the whole image hangs the
engine [HW].

`--selfcheck` re-parses the emitted image the way the engine does and fails on:
a packet whose header length does not land on the next header; any leftover
dwords; a register list that runs off the end of its packet; an `0x41` whose
sub-header is not `[n][first][1][0]` or whose length is not `n*4+5`; a draw
packet that is not 3 dwords; an unknown opcode; a register with no entry in the
justification table; a relocation list that does not tile the section blob
exactly; a relocation marking a draw operand in a non-draw packet. All nine
classes were exercised against deliberately corrupted images.

Sanity anchors the generator reproduces from the hardware matrix, independently:
`--prim lines` yields `0x10b0 = 0x0006103d`, `0x1130 = 0x0100200c`,
`0x1300 = 0x00a60ec5` and a final `0x10f0 = 0x00000001` — the four values the
`lines` variant measured, none of them hard-coded: they fall out of the
`mtx734`/topology table and of the primitive class replicated into
`0x10f0[3:0]`, `0x1130[25:24]` and `0x1300[3:0]`. `--flat` yields
`0x1130 = 0x0200300c`, the `flat` variant's value. [HW]

A second, independent check: `tools/verify-ring-framing.py`, written for other
work and using its own decoder, walks the emitted image end to end with no
DESYNC.

---

## 5. What could not be justified, and is still in the stream

Nothing here is guessed; each is a captured constant kept because removing it
is a hardware experiment nobody has run.

| dwords | why they are still there |
|---|---|
| `0x12b0 = 0x3fff8842` | traced to MTXPARD and `mtx398`; **no bit has a meaning**. Replayed verbatim |
| `0x12f8 = 0x007fe000` | the vendor parameter `reg3DThresholdAA`; no field decoded |
| `0x12fc = 0x0bb80024` | `[1:0]` is the cull select; the remaining `0x5dc001` above bit 4 is undecoded, in the register that also carries cull |
| `0x1110 = 3`, `0x1114 = 0x00287210` | the vendor kernel's state offsets are known, no bit layout is. `0x1118`'s texture nibbles are the only decoded part of the group |
| `0x111c = 0x00000fdb`, `0x1144 = 0` | named in `warp-binding`'s index table with an empty role column |
| `0x1120 = 0x45ffc5ff`, `0x1124 = 0xc5ff45ff` | packed halves of ±8160.0f, matching guard-band slot `c[0x174]`. Whatever they are on the draw path, they are not the float default-attribute vector the docs call them |
| `0x121c = 0` | [INF] a texture-engine latch. The kernel always writes it before the unit block; MTXPARD has no writer at all |
| `0x1300`'s constant `0x00860e00` | the published assembly formula does **not** reproduce the captured value: its `((surf>>25)&0xf)` term would contribute 9, and `0x00a60ec4`'s low nibble is 4. Used literally |
| `0x1130`'s bits 2, 3, 13 and everything above [25:24] | only cull (0), flat (12) and class ([25:24]) are measured; the rest is carried over from the capture |
| the four combiner pad slots | all three vendor emitters unconditionally pad every program to 5 or 10 instructions and write every pad. No instruction-count register and no terminator was found, so whether the padding is a hardware requirement or a driver convention is **open**. Four extra packets is a cheap insurance premium |
| `0x1230`/`0x1234` null slot 2 | the capture writes them in the same packet as slot 0. Depth is off, so nothing should read slot 2 — but `parhelia-rendertarget.md` (from the kernel) says slot 2 *is* the depth surface while the live GL capture puts depth on slot 1, and that conflict is unresolved. Nulling slot 2 costs two dwords |
| `0x10b8 = 0x20c00000, 0xa1c50020` | used as a *pair*, verbatim. `draw-builders-kernel`'s own field formula puts the destination-slot field at `[16:12]`, which reads 16 out of `0xa1c50020`, while the prose says 5 — an open conflict in two documents. The pair is nonetheless HW-proven together with the `[x,y,r,g,b]` layout and a program that reads `v0`/`v5` |

### Still spliced, and the honest size of it

* **`--program transform` needs `--loader-entry`**: a valid entry for the
  vendor's kind-0 constant-loader program (`mtx1079`, 14 instructions, live
  slot `16c05600`). An `0x41` packet only lands in the constant file while that
  program is bound, and the assembler cannot yet regenerate it — its opcodes
  are in the undecoded `ctl*`/`sop*`/`getc`/`streg` control class. Its words
  are available verbatim from `captures/live-ring-0528/warp-programs/` and from
  `mtx_parhelia.o` `.data`, so it can be relocated into our own buffer, but it
  is not *ours*. **`--program passthrough` has no such dependency and is the
  only variant that splices nothing.**
* **The five stage entries `0x10c8/0x10cc/0x10d0/0x10d4/0x10d8`** are omitted by
  default. Whether a triangle rasterises with them unbound is **open**: the
  only capture with `2,2,2,2,0` in them is the priming packet, which contains
  no 3D draw. `--stages` passes them through if a same-boot capture supplies
  them.
* **Kind 3**, the program that publishes the resident clipper (kind 6) and
  stage-6 entries into hardware latches, is not emitted. Whether the setup unit
  needs it when nothing else is bound is **open** (`sync0d`/`op6e` undecoded).

### The first three hardware questions this stream asks

1. Does the setup unit accept `o0.w = 1.0`? The passthrough program produces
   `o0 = v0 = (x, y, 0, 1)`; the vendor's tail always sets `o0.w` from
   `c[0x161].w`, which is the *viewport height*, not 1. If the answer is no,
   `--program transform` is mandatory and the four-instruction variant dies.
2. Do the five stage entries have to be bound?
3. Are the four combiner pad slots required?

Each is a single-register delta against a stream that otherwise frames exactly
and whose every other dword has a line of evidence behind it.
