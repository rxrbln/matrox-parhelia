Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# The WARP constant (block) file: complete address map

This closes the last splice in the 3D path.  Until now we emitted the vendor's
`0x41` block loads verbatim because we did not know what was in them; the
consequence was that our triangles lived in the client's coordinate space and
were clipped by its scissor.  The file is now mapped end to end, the capture
payloads are decoded and shown to match the client's `glOrtho`/`glViewport`
exactly, and §6 gives the literal dwords to emit for our own transform and
viewport.

Every claim is tagged **[BIN]** (binary + offset), **[CAP]** (live capture) or
**[INF]** (inference, with the reasoning given).

---

## 0. Headline

1. **The block file is a flat array of 384 `vec4`** living at `ctx + 0xa18`
   (bank A, the object at `ctx + 0x7e8`) and `ctx + 0x2448` (bank B, the object
   at `ctx + 0x2218`).  Slot *s* is at `base + s*16`.  Both bases are exactly
   `constobj + 0x230`, confirming `docs/parhelia-constant-upload.md`. [BIN]
2. **`c[0..3]` is the fused modelview·projection**, stored math-row-major:
   `c[r] = (M[r], M[r+4], M[r+8], M[r+12])` of the GL column-major matrix, so
   `dp4(v, c[r])` yields clip component *r*.  We reproduce the capture's 8-vec4
   payload bit for bit from `gltri.c`'s `glOrtho(0,320,0,240,-1,1)`. [BIN+CAP]
3. **`c[0x160]` is the viewport scale `(W/2, -H/2, (f-n)/2, 1)` and `c[0x161]`
   the viewport offset `(x0+W/2, y0+H/2, (n+f)/2, H)`.**  The `±0.5` factors are
   the rodata literals `.LC6 = 0.5` / `.LC7 = -0.5`; `H` in `.w` is the same
   field the y-scale is derived from, i.e. the *viewport* height. [BIN+CAP]
4. **`count=224 first=0x160` is a decoder artefact, not a 224-vec4 load.**  The
   packet is `0x4106000f`, a *base/index register* load with **no**
   `[count][first][1][0]` sub-header; the decoder printed its first two payload
   dwords (`0xe0`, `0x160`) as if they were count and first.  We do need it —
   see §7. [CAP+BIN]
5. **`0x0E0` is not an arbitrary base.**  The matrix palette is 32 entries ×
   7 vec4 = 224 = `0x0E0` slots, so `b0 = 0x0E0` is simply the first slot after
   the palette, the start of the lighting block.  That is why `c[b0+84]` lands
   on the material and `c[b0+105]` on the projection. [BIN]
6. **Correction to `docs/parhelia-warp-programs-decoded.md` §3.1:** it records
   the viewport offset as arriving only through the snap constant.  It arrives
   through instruction 101, `mov T1, c[b1+1]` = `c[0x161]`; instructions 104/106
   add and subtract a grid-snap constant around the `mad`.  Net effect
   `o0.xyz = c[0x160].xyz · ndc + c[0x161].xyz`, `o0.w = c[0x161].w`. [CAP+INF]
7. **The snap constant must be written by us either way.**  `c[0x168]` is
   *never written anywhere in the kernel object* [BIN, exhaustive scan] and the
   context is `LocMemSet` to zero at reset (`mtx2209 + 0x2f`), so if the
   program really reads `0x168` it reads `0.0` and the ±K pair is inert.  There
   is strong evidence it actually reads `0x178`, whose `.w` is the hard-coded
   `0x49400000 = 786432.0 = 1.5·2^19`, the canonical 4-subpixel-bit snap magic
   — see §5.  Emit **both** and the geometry is correct under either reading.

---

## 1. How the file is addressed — the proof of the flat layout [BIN]

`mtx2702(constobj, first, last)` marks a dirty slot range.  Every writer calls
it with literal slot numbers, and every writer reaches the data through a
cached pointer.  Resolving those pointers against `mtx2209` (the context reset,
`.text+0x8e400`) fixes the layout:

| pointer field | value set by `mtx2209` | slot marked at the same site | slot from `base + s*16` |
|---|---|---|---|
| `ctx+0x3c50` | `ctx+0x2018` | `0x160` | `(0x2018-0xa18)/16 = 0x160` ✓ |
| `ctx+0x3c58` | `ctx+0x2028` | `0x161` | `0x161` ✓ |
| `ctx+0x3c68` | `ctx+0x2138` | `0x172` | `0x172` ✓ |
| `ctx+0x3c70` | `ctx+0x2168` | `0x175` | `0x175` ✓ |
| `ctx+0x3d30` | `ctx+0x2198` | `0x178` | `0x178` ✓ |
| `ctx+0x3c80` | `ctx+0x1ee8` | `0x14d` | `0x14d` ✓ |
| `ctx+0x3cf0` | `ctx+0x1d58` | `0x134` (via `mtx1322`) | `0x134` ✓ |
| `ctx+0x3ce8` | `ctx+0x1d98` | `0x138` | `0x138` ✓ |
| `ctx+0x3cf8` | `ctx+0x1f78` | `0x156` | `0x156` ✓ |

and the three pointer *arrays* built by the same function:

```
8e850:  ctx+0x238[i] = ctx+0xa18 + i*0x70      i = 0..31   -> slot 7*i      (matrix palette)
8e86b:  ctx+0x360[i] = ctx+0xa58 + i*0x70      i = 0..31   -> slot 7*i + 4  (normal matrix)
8e712:  ctx+0x3c88[i] = ctx+0x1818 + i*0x70    i = 0..11   -> slot 0xe0+7*i (lights)
8e740:  ctx+0x3d00[i] = ctx+0x20d8 + i*0x10    i = 0..5    -> slot 0x16c+i  (clip planes)
8e830:  ctx+0x338[i]  = ctx+0x1da8 + i*0x40    i = 0..3    -> slot 0x139+4*i(texture matrices)
```

`0x70 = 7 vec4` and `0x40 = 4 vec4` fall out of the same base — the arithmetic
only closes for `slot = (addr - 0xa18)/16`.  Bank B is the identical structure
at `ctx+0x2448`: `mtx2926` writes the viewport twice, once through
`*(ctx+0x3c50)` (= `ctx+0x2018`, bank A) and once to `ctx+0x3a48`
(= `0x2448 + 0x160*16`, bank B), and marks `ctx+0x7e8` and `ctx+0x2218`. [BIN]

---

## 2. The constant map

### 2.1 Matrices — slots `0x000..0x0DF`

**32 matrix-palette entries × 7 vec4.**  `mtx1394` (`.text+0x85220`) walks the
palette; per entry it copies the 4×4 through `mtx3265`/`mtx3731` and calls
`mtx2702(obj, r13, r13+7)` with `r13 += 7` (`.text+0x85403`, `+0x8556f`). [BIN]

| slot | contents |
|---|---|
| `7i + 0..3` | MVP (modelview·projection) of palette entry *i*, math-row-major |
| `7i + 4..6` | normal matrix (inverse-transpose modelview), 3 rows |

Entry 0 is the one the fixed-function program reads (`a0.x = 0`,
`dp4 r2.{w,z,y,x} ← T0 · c[a0.x+3..0]`).  **Row convention:** with the GL
column-major array `M[0..15]`,

```
c[4k + r] = ( M[r], M[r+4], M[r+8], M[r+12] )        (k = palette entry, r = 0..3)
```

so `clip.r = dot(c[r], (x,y,z,w))`.  Proven in §4. [BIN+CAP]

### 2.2 Lighting and material — slots `0x0E0..0x159`

`b0 = 0x0E0` is the first slot past the palette. [BIN]

| slot | meaning | written by |
|---|---|---|
| `0x0E0 + 7i .. 0x0E6 + 7i`, `i = 0..11` | **12 light blocks, 7 vec4 each** | `mtx309`, `mtx2702(obj, r13+0xe0, r13+0xe6)` @ `0x89f0a`; pointer array `ctx+0x3c88` |
| `0x134` | front material **diffuse** | `mtx1322` @`0x153de`, `mtx2441` @`0x4709d` (`mtx2702(_,0x134,0x137)`) |
| `0x135` | front material **ambient** | same (read back by `mtx2588` as `[ptr+0x10]`) |
| `0x136` | front material **specular** (`.w` = shininess) | same |
| `0x137` | front material **emission** | same |
| `0x138` | front **scene-ambient × material-ambient (+ emission)** | `mtx2588` @`0x870d8`, `mtx2702(_,0x138,0)` @`0x8710c` |
| `0x139`,`0x13D`,`0x141`,`0x145` | **texture matrices 0..3**, 4 rows each | pointer array `ctx+0x338` |
| `0x149..0x14C` | **projection matrix**, 4 rows, same convention as §2.1 | `mtx1394` @`0x85463` (`mtx2702(_,0x149,0x14d)`), `mtx3654` @`0x8cca4` |
| `0x14D` | `(0, render-target height, 0, 32.0)` | `.y` ← `(float)[renderTarget+0xc]` in `mtx3654` @`0x8bf68`; `.w` ← literal `0x42000000` in `mtx2209` @`0x8e4f5` |
| `0x14E` | `(., ., 1/maxDiffuse, 1/maxSpecular)` light over-bright normalisation | `mtx309` @`0x8a220`/`0x8a231` (`divss` of 1.0), `mtx2702(_,0x14e,0)` |
| `0x154`, `0x155` | lighting scratch | `mtx3654` @`0x8cd3a`, `0x8d243` |
| `0x156..0x159` | **back material** (diffuse, ambient, specular, emission) | `mtx2441` @`0x470bf` (`mtx2702(_,0x156,0x159)`) |

### 2.3 Raster / setup — slots `0x160..0x17F`

| slot | meaning | written by |
|---|---|---|
| `0x160` | **viewport scale** `(W/2·sx, −H/2·sy, (f−n)/2, 1)` | `mtx2926` @`0x87540`,`0x875a1`,`0x873c6` |
| `0x161` | **viewport offset** `((x0+W/2)·sx, (y0+H/2)·sy, (n+f)/2, H)` | `mtx2926` @`0x87342`,`0x87363`,`0x8737d` |
| `0x162..0x16B` | **never written by the driver** (exhaustive scan) | — |
| `0x16C..0x171` | **6 user clip planes** | `mtx2522` @`0x8797d` (copies from the `ctx+0x3d00` array into bank B `ctx+0x3b08+16i`) |
| `0x172` | `(point attenuation a, b, c, pointSize/2)` | `.xyz` `mtx3654` @`0x8bb2d`; `.w` `mtx2497` @`0x86970`, clamped to `[c[0x173].x, c[0x173].y]` |
| `0x173` | `(pointSizeMin, pointSizeMax, +0.5, −0.5)` | `mtx2209` @`0x8e455`, `mtx2497` |
| `0x174` | **guard band** `(8160, 8160, −8160, −8160)` in window pixels | `mtx2209` @`0x8e7d5` literals `0x45ff0000` / `0xc5ff0000` |
| `0x175` | `(128.0, …)` colour/AA scale | `mtx2209` @`0x8e510` literal `0x43000000`, `mtx3654` @`0x8bff6` |
| `0x178` | `(polyOffsetFactor, polyOffsetUnits·depthScale, 0.5, 786432.0)` | `.xy` `mtx740` @`0x8664c`/`0x8666d`; `.z` `mtx3654` @`0x8d25b`; **`.w` is a fixed literal** `0x49400000` in `mtx2209` @`0x8e56e` |
| `0x179..0x17C` | **fog block**, 4 vec4 | `mtx2209` @`0x8e8ee..0x8ea51`, one `mtx2702(_,0x179,0x17c)` |
| `0x17D` | `(2^23, 0.05, 31.0, 1/31)` fixed helper constants | `mtx2209` @`0x8e5e5` literals `0x4b000000 0x3d4ccccd 0x41f80000 0x3d042108` |
| `0x17E` | `(., 1/16, ., .)` subpixel unit | `mtx2209` @`0x8e641` literal `0x3d800000`; `mtx3654` `.xzw` |

### 2.4 Windows twin cross-check [BIN]

`MTXPARD.dll` uses the **identical slot namespace**: `push 0x160` / `push 0x161`
at `0x1193ab`/`0x119422` in the viewport writer, `push 0x14d; push 0x149` at
`0x11b2c4` (the same projection range `mtx1394` marks), `push 0x16c` for the
clip planes, `push 0x172`, `push 0x175`, `push 0x178`, and the same literal
pair `0x49400000` / `0x3d042108` in its context reset at `0x1219aa`/`0x1219f7`.
Its viewport arithmetic reads the same structure fields `+0x30/+0x34/+0x38/+0x3c`
that the Linux `mtx2926` reads at `+0x31/+0x35/+0x39/+0x3d` (the Linux struct
carries one extra leading byte) and marks the same two constant objects,
`[esi+0x6d0]` and `[esi+0x20b4]`.

---

## 3. The viewport formula, exactly [BIN]

`mtx2926` (`.text+0x87220`), main path, with `V` = the viewport struct at
`[state+0x2b8]`:

```
W  = (float)V[0x29]      H  = (float)V[0x2d]
Ax = (float)V[0x21]      Ay = (float)V[0x25]
Cx = (float)V[0x31]      Cy = (float)V[0x35]
sx = V[0x39] (float)     sy = V[0x3d] (float)          ; supersample/AA scale, 1.0 normally

scale.x  =  W * ( 0.5) * sx                   ; .LC6 = 0x3f000000 =  0.5
scale.y  =  H * (-0.5) * sy                   ; .LC7 = 0xbf000000 = -0.5
offset.x = (Cx + Ax) * sx + scale.x
offset.y = (Cy + Ay) * sy - scale.y

c[0x160] = (scale.x,  scale.y,  depthScale, 1.0)       ; .z <- [state+0x28]
c[0x161] = (offset.x, offset.y, depthBias,  H  )       ; .z <- [state+0x2c], .w <- H
```

`(Cx+Ax)` / `(Cy+Ay)` are the viewport origin in the render target, **y measured
downwards** — the Y flip lives entirely in the negative `scale.y`.  For a GL
`glViewport(x, y, w, h)` into a target of height `Ht`:

```
x0 = x            y0 = Ht - y - h            (0 for the usual full-window case)
```

With GL `glDepthRange(n, f)`: `depthScale = (f-n)/2`, `depthBias = (n+f)/2`.
Defaults `n=0, f=1` give `0.5, 0.5` — matching the capture. [INF, confirmed by
[CAP]]

`c[0x160].w = 1.0` is never touched by `mtx2926`; it is context-reset state.

---

## 4. Decoding the capture — the proof

Source: `captures/live-ring-0528/secs.txt`, section `19dc0daa`, offsets 67…229;
the same eight packets repeat once per draw, eight draws in `decoded.txt`.
Framing `hdr = 0x41000000 | ((n<<17)&0x7e0000) | ((n*4+3)&0xffff)` then
`[n][first][1][0]` then `n*4` payload dwords — verified against all five header
values present (`0x41100023`, `0x410a0017`, `0x41080013`, `0x4104000b`,
`0x41020007`).

### 4.1 `count=8 first=0x0` — the MVP and the normal matrix [CAP]

```
41100023 | 00000008 00000000 00000001 00000000
c[0x000] 3bcccccd 00000000 00000000 bf800000    (  0.00625,        0,  0, -1 )
c[0x001] 00000000 3c088889 00000000 bf800000    (        0, 0.008333,  0, -1 )
c[0x002] 00000000 00000000 bf800000 00000000    (        0,        0, -1,  0 )
c[0x003] 00000000 00000000 00000000 3f800000    (        0,        0,  0,  1 )
c[0x004] 3f800000 00000000 00000000 00000000    (        1,        0,  0,  0 )
c[0x005] 00000000 3f800000 00000000 00000000    normal matrix = identity
c[0x006] 00000000 00000000 3f800000 00000000
c[0x007] 00000000 00000000 00000000 00000000    first slot of palette entry 1
```

`tools/gltri.c` lines 118–123 do `glViewport(0,0,320,240)`,
`glOrtho(0, 320, 0, 240, -1, 1)`, identity modelview.  GL's ortho gives
`M[0] = 2/320 = 0.00625`, `M[5] = 2/240 = 0.0083333`, `M[10] = -1`,
`M[12] = M[13] = -1`, `M[14] = 0`, `M[15] = 1`.  Applying
`c[r] = (M[r], M[r+4], M[r+8], M[r+12])` regenerates the four rows **bit for
bit**, including the two `-1.0` in `.w`.  `0.00625` and `0.00833334` are the
only two numbers in the packet that carry the window size, and they carry it as
`2/W` and `2/H` — this is the check that fixes the row convention.

`c[4..6]` = identity = the inverse-transpose of the identity modelview. ✓

### 4.2 `count=2 first=0x160` — the viewport [CAP]

```
4104000b | 00000002 00000160 00000001 00000000
c[0x160] 43200000 c2f00000 3f000000 3f800000    ( 160, -120, 0.5,   1 )
c[0x161] 43200000 42f00000 3f000000 43700000    ( 160,  120, 0.5, 240 )
```

`W/2 = 160`, `−H/2 = −120`, `x0 + W/2 = 160`, `y0 + H/2 = 120`, `H = 240` for
the 320×240 drawable, `sx = sy = 1`, depth range `[0,1]` → `0.5, 0.5`.  Exactly
§3 with `W=320, H=240, Ax=Ay=Cx=Cy=0`.

### 4.3 End-to-end: the client's three vertices [CAP+INF]

`gltri.c` draws `(20,20)`, `(300,20)`, `(160,220)` in window pixels.  The
program computes `r2 = MVP·v`, `r5 = r2/r2.w`, then
`o0.xyz = c[0x160].xyz · r5.xyz + c[0x161].xyz`, `o0.w = c[0x161].w`:

| input | ndc = MVP·v | `o0.x` | `o0.y` | `o0.z` |
|---|---|---|---|---|
| (20, 20) | (−0.875, −0.83333, 0) | 160·(−0.875)+160 = **20** | −120·(−0.83333)+120 = **220** | 0.5 |
| (300, 20) | (+0.875, −0.83333, 0) | **300** | **220** | 0.5 |
| (160, 220) | (0, +0.83333, 0) | **160** | **20** | 0.5 |

The x values come back as the client's own pixel x, and the y values as
`240 − y` — the top-left-origin raster row.  `o0.w = 240`.  The whole chain
closes on the client's numbers, which is what makes the layout certain rather
than plausible.

### 4.4 The other five constant packets [CAP]

```
410a0017 | 5 @ 0x134   c[0x134] (0.8,0.8,0.8,1)   GL default material diffuse
                       c[0x135] (0.2,0.2,0.2,1)   default ambient
                       c[0x136] (0,0,0,0)         default specular / shininess 0
                       c[0x137] (0,0,0,1)         default emission
                       c[0x138] (0.04,0.04,0.04,1)  = 0.2 * 0.2, scene ambient x material ambient
410a0017 | 5 @ 0x149   c[0x149..0x14c] = the same four ortho rows (projection copy)
                       c[0x14d] (0, 1200, 0, 32)  1200 = the 1920x1200 render target height
410a0017 | 5 @ 0x155   c[0x155] (0,0,0,1)
                       c[0x156] (0.8,0.8,0.8,1)   back material, same order as front
                       c[0x157] (0.2,0.2,0.2,1)
                       c[0x158] (0,0,0,0)
                       c[0x159] (0,0,0,1)
41080013 | 4 @ 0x172   c[0x172] (1, 0, 0, 0.5)    GL_POINT_DISTANCE_ATTENUATION default (1,0,0); pointSize/2
                       c[0x173] (0, 2048, 0.5, -0.5)   point size range, half-pixel offsets
                       c[0x174] (8160, 8160, -8160, -8160)  guard band
                       c[0x175] (128, 0, 0, 0)
41020007 | 1 @ 0x178   c[0x178] (0, 0, 0.5, 786432)    polygon offset (0,0); 786432 = 1.5*2^19
4104000b | 2 @ 0x17d   c[0x17d] (8388608, 0.05, 31, 0.0322581)   2^23; 31 and 1/31
                       c[0x17e] (0, 0.0625, 0, 0)      1/16 = the subpixel unit
```

`0.8 / 0.2 / 0 / (0,0,0,1)` are the GL defaults for `glMaterial` and
`0.04 = 0.2 · 0.2` is the light-model ambient times the material ambient — the
product `mtx2588` computes at `.text+0x870a6` by multiplying by
`[ctx->3cf0 + 0x10]`, i.e. slot `0x135`.  That is what identifies `0x134` as
*diffuse* and `0x135` as *ambient*, against the usual GL ordering.

---

## 5. `c[b1+8]`: which slot the snap really is

`docs/parhelia-warp-programs-decoded.md` reads instructions 104/106 as
`c[b1+8] = c[0x168]` and notes that nothing loads `0x168`.  Re-deriving from
`tools/parhelia-warpdis2.py`'s field layout: instruction 104 is
`w0 = 0x40004e00, w1 = 0xea540107`, giving `src1 = 8`, `file1 = 3` (constant
file), `f1i = 5` (src1 relative), `relcomp = 1` → base `b1`, `extsel = 2` so the
`exthi` extension is claimed by src2 and cannot widen src1.  On the plain
reading the slot is `0x160 + 8 = 0x168`.

Two facts sit against that:

* **`0x168` is written by nothing.**  A scan of every store into
  `ctx+0xa18 .. +0x2218` and `ctx+0x2448 .. +0x3c48` across the whole object
  finds no writer for slots `0x162..0x16B`, and `mtx2209` `LocMemSet`s the
  context, so the value is `0.0`.  An `add K` / `sub K` pair compiled into every
  vertex program to add and subtract zero is not a plausible thing for the
  vendor's code generator to emit. [BIN]
* **`0x178.w` is exactly the constant that shape wants.**  `mtx2209 + 0x16e`
  stores the literal `0x49400000 = 786432.0 = 1.5·2^19` there and nothing ever
  changes it; `1.5·2^(23−f)` is the standard single-precision magic number that
  rounds to `f` fractional bits, `f = 4` → 1/16 pixel, and `c[0x17e].y = 1/16`
  sits right beside it.  `0x178` is `b1 + 0x18`.  The only field that separates
  `+8` from `+0x18` is `w1[21:20]`, which the ISA doc records as "never written
  by the assembler, seen = 1", and which is **1 on exactly instructions 104 and
  106 and on no other instruction in the program**. [BIN+CAP]

So `w1[21:20]` is most likely the high bits of a relative source index when the
`exthi` extension slot is taken by another operand, making the read
`c[0x178].ww00`. [INF, strong]

**This does not block us.** Adding and subtracting the same value is a no-op up
to rounding under both readings, so a generator that writes `c[0x168] = 0` *and*
`c[0x178] = (0, 0, 0.5, 786432)` is correct either way.  What is *not* safe is
leaving `0x168` alone: if the program does read it and a previous context left
garbage there, every position is quantised to a garbage grid.  Write it.

---

## 6. Packets to emit

### 6.1 Framing

```c
uint32_t hdr = 0x41000000 | ((n << 17) & 0x007e0000) | ((n*4 + 3) & 0xffff);
/* hdr, n, first_slot, 1, 0, then n*4 float dwords */
```

Each block load must be preceded by the destination-select preamble from
`docs/parhelia-constant-upload.md`; in the capture the immediately preceding
register list is

```
0x1000 = 0x00000000   0x1040 = 0x00000004   0x10b0 = 0x0002305c
0x10b4 = 0x00000000   0x10b8 = 0x83c00000
0x1374 = 0x00000000   0x10f0 = 0x20000028   0x10f0 = 0x20000028
0x1100 = <entry of the kind-0 constant-loader microprogram>
```

and the whole run of `0x41` packets then follows without re-binding.  MODE
`0x20000028` = run the bound program once per following packet; the loader
(`mtx1079`, §4.2 of the warp-programs doc) consumes `[count][first][1][0]` as
its `v0` and writes the payload through destination file 3.

### 6.2 Identity MVP — `c[0..3]`

```c
0x41080013, 0x00000004, 0x00000000, 0x00000001, 0x00000000,
0x3f800000, 0x00000000, 0x00000000, 0x00000000,   /* c[0] = (1,0,0,0) */
0x00000000, 0x3f800000, 0x00000000, 0x00000000,   /* c[1] = (0,1,0,0) */
0x00000000, 0x00000000, 0x3f800000, 0x00000000,   /* c[2] = (0,0,1,0) */
0x00000000, 0x00000000, 0x00000000, 0x3f800000,   /* c[3] = (0,0,0,1) */
```

Vendor-shaped 8-vec4 form (adds the identity normal matrix at `c[4..6]` and the
zero `c[7]`), for streams that also enable lighting:

```c
0x41100023, 0x00000008, 0x00000000, 0x00000001, 0x00000000,
0x3f800000, 0x00000000, 0x00000000, 0x00000000,
0x00000000, 0x3f800000, 0x00000000, 0x00000000,
0x00000000, 0x00000000, 0x3f800000, 0x00000000,
0x00000000, 0x00000000, 0x00000000, 0x3f800000,
0x3f800000, 0x00000000, 0x00000000, 0x00000000,
0x00000000, 0x3f800000, 0x00000000, 0x00000000,
0x00000000, 0x00000000, 0x3f800000, 0x00000000,
0x00000000, 0x00000000, 0x00000000, 0x00000000,
```

For a general matrix, transpose the GL column-major array:
`c[r] = (M[r], M[r+4], M[r+8], M[r+12])`.

### 6.3 Viewport — `c[0x160..0x161]`

```c
/* rect (x0, y0, w, h), y0 measured from the TOP of the render target,
   depth range [zn, zf] (GL default 0..1) */
c160 = ( w/2.0f, -h/2.0f, (zf-zn)/2.0f, 1.0f );
c161 = ( x0 + w/2.0f, y0 + h/2.0f, (zn+zf)/2.0f, (float)h );
```

**Full 1920×1200 screen** — `(0, 0, 1920, 1200)`:

```c
0x4104000b, 0x00000002, 0x00000160, 0x00000001, 0x00000000,
0x44700000, 0xc4160000, 0x3f000000, 0x3f800000,   /* ( 960, -600, 0.5,    1 ) */
0x44700000, 0x44160000, 0x3f000000, 0x44960000,   /* ( 960,  600, 0.5, 1200 ) */
```

**640×480 rect at (100, 50)** on the same screen:

```c
0x4104000b, 0x00000002, 0x00000160, 0x00000001, 0x00000000,
0x43a00000, 0xc3700000, 0x3f000000, 0x3f800000,   /* ( 320, -240, 0.5,   1 ) */
0x43d20000, 0x43910000, 0x3f000000, 0x43f00000,   /* ( 420,  290, 0.5, 480 ) */
```

**Reproduction of the capture** — `(0, 0, 320, 240)`; this is byte-identical to
the packet in `secs.txt`, which is the regression test for the generator:

```c
0x4104000b, 0x00000002, 0x00000160, 0x00000001, 0x00000000,
0x43200000, 0xc2f00000, 0x3f000000, 0x3f800000,
0x43200000, 0x42f00000, 0x3f000000, 0x43700000,
```

### 6.4 Snap constants — emit both

```c
/* c[0x168] = 0 : neutral if the tail reads b1+8 */
0x41020007, 0x00000001, 0x00000168, 0x00000001, 0x00000000,
0x00000000, 0x00000000, 0x00000000, 0x00000000,

/* c[0x178] = (polyOffFactor, polyOffUnits, 0.5, 1.5*2^19) : the vendor value */
0x41020007, 0x00000001, 0x00000178, 0x00000001, 0x00000000,
0x00000000, 0x00000000, 0x3f000000, 0x49400000,
```

### 6.5 Guard band — `c[0x174]`

The clipper limit is in the same window-pixel units as `o0`, so a viewport
larger than ±8160 px would need this raised.  For anything on a 1920×1200
screen the vendor default is fine:

```c
0x41020007, 0x00000001, 0x00000174, 0x00000001, 0x00000000,
0x45ff0000, 0x45ff0000, 0xc5ff0000, 0xc5ff0000,   /* (8160, 8160, -8160, -8160) */
```

### 6.6 What a minimal triangle actually needs

The vendor's fixed-function per-vertex program reads exactly four constants:
`c[0..3]` (via `a0.x = 0`), `c[0x161]`, `c[0x160]` and the snap.  With lighting
off, the material, light, texture-matrix, fog, point and clip-plane blocks are
never sampled — skip them.  So the minimum is **§6.2 + §6.3 + §6.4**, plus §7
if the bound program uses relative addressing at all.

---

## 7. `count=224 first=0x160` — what it is and whether we need it

It is a **mis-decode**.  The header is `0x4106000f`: `count` field
`(0x06000f & 0x7e0000) >> 17 = 3` → 4 quads → 16 dwords follow, and unlike a
block load this packet carries **no `[count][first][1][0]` sub-header**
(`docs/parhelia-opcode-41.md` §1 already notes the two shapes).  The decoder
printed payload dwords 0 and 1 as if they were `count` and `first`, giving
`224` and `0x160` — which are in fact `0xe0` and `0x160`, the *base registers*.

Full payload from `secs.txt` @ `19dc0daa+243`:

```
000000e0 00000160 00000080 00000800    b0, b1, b2, b3
00000000 00000001 00000002 00000003    a0.x, a0.y, a0.z, a0.w
02003004 0200300c 00000000 00000000    stage-mode latches (the 0x1130 values)
08a55583 ffffffff ffffffff ffffffff    a resident program entry + three -1 sentinels
```

It is consumed by the kind-1 microprogram `mtx251`, bound immediately before it
with `0x1100 = <kind-1 entry>` and `0x1130 = 0x0700300c`; `mtx251`'s `mova` /
`ctl33` sequence installs `v0` as the four base registers and `v1` as the index
register file. [CAP for the payload, BIN for the program, INF for the
`mova`→base mapping — but note that `b1 = 0x160` and `b0 = 0xe0` are precisely
the two bases §2 derives independently from the constant map, which is a strong
independent check.]

**Do we need it?  Yes, unless our own vertex program uses only absolute
constant indices.**  It is what makes `c[b1+0]`, `c[b1+1]`, `c[b1+8]` and
`c[a0.x+k]` resolve.  If we bind the vendor's fixed-function program — or any
program the vendor's generator produced — we must emit it, and `b1` must be
`0x160` for the viewport constants written in §6.3 to be the ones the tail
reads, and `a0.x` must be `0` for the MVP at `c[0..3]` to be the one the four
`dp4`s read.  The last eight dwords are per-context values (a VRAM program
address and the `0x1130` mode words), not constants; substitute your own.

If instead we emit a hand-written program with absolute `c[N]` operands, drop
the packet entirely and the base registers become irrelevant.

---

## 8. Open items

* `w1[21:20]` as the relative-index high bits (§5) is inference from three
  independent circumstantial facts.  It can be settled on hardware in one shot:
  write a distinguishing value into `c[0x168]` only (say `.w = 1024.0`, a
  1-pixel grid) and see whether the triangle's edges quantise.
* `c[0x14E].x/.y`, `c[0x154]`, `c[0x155]`, `c[0x175].x = 128.0` and the four fog
  vec4 at `0x179..0x17C` are located and attributed to their writers but their
  component semantics are not decoded — none of them are read by the
  no-lighting, no-fog path.
* The 7 vec4 of a light block are not broken down per component.
