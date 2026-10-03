# Parhelia texture sampling: filter, wrap, mipmaps and texture memory layout

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

What an open GL driver has to program to sample a texture on Parhelia-512
(PCI 102b:0528) and its LX/P690 relatives: the complete `0x11c0` filter word,
the complete `0x11c4` wrap word, the mipmap chain layout, and the texture
memory layout.

Scope note: this document settles the *sampler* state. The combiner that
consumes the sampled texel is `docs/parhelia-fragment-isa.md` §12; the format
code table is §11.7 of the same document and is not repeated here.

**It corrects several earlier documents** — the full list is §10. The headline
one: `docs/parhelia-fragment-isa.md` §11.3/§11.4 and
`docs/parhelia-texture-path.md` both name `0x11c0` the wrap word and `0x11c4`
the filter word. **They are swapped** (§1).

### Summary

| | |
|---|---|
| `0x11c0` | **filter**: MIN `[1:0]`, MAG `[4:3]`, MIP `[7:6]`; bit 12 always set; LOD bias `[24:16]`, signed 4.4 biased -4.0. `0x01c01000` = nearest, no mip, bias 0 |
| `0x11c4` | **wrap**: S `[2:0]`, T `[5:3]`, R `[8:6]`; LOD min `[23:16]`, LOD max `[31:24]`, unsigned 4.4. `0xf0000000` = repeat, LOD 0..15 |
| filter codes | 0 nearest, 2 linear, 3 anisotropic; MIP 0 none, 1 nearest, 2 linear |
| wrap codes | 0 `GL_REPEAT`, 1 `GL_CLAMP_TO_EDGE`, 4 `GL_MIRRORED_REPEAT`, 5 mirror-once, 6 `GL_CLAMP`, 7 `GL_CLAMP_TO_BORDER` |
| mipmaps | one base address; the hardware walks a contiguous chain. Every level padded to a 32-texel pitch and to `Ha` rows and `A` bytes; levels below 16x16 share one hard-coded 32x16 "mip tail" block. `0x11e4[31:28]` = maximum level |
| texture memory | **tiled** when `0x11e0[18:16]` != 0 — 32x16-texel blocks built from 16-byte micro-tiles, a **different** swizzle from the render target's 64x32. Layout code **0 is linear** and is the right first choice |
| limits | 2048 max, POT only through the vendor ICD, 4 units, anisotropy 2 |

---

## 0. Sources and how to read the evidence tags

| tag | meaning |
|---|---|
| **[HW]** | measured in a live command stream captured from a running vendor GL client on a real 0528, and attributed to one GL state change |
| **[BIN]** | read out of a vendor binary (disassembly / data table) |
| **PROVEN** | [HW], or [BIN] agreeing across two independent binaries |
| **INFERRED** | a single binary, or a model that fits every observation but has no direct write |
| **OPEN** | not determined |

Binaries used (no vendor C source was read; `matroxdriver-.../kernel/src/`
is off limits and was never opened):

* `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` — Linux kernel HAL,
  x86-64, unstripped, symbols `mtxNNNN`.
* `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` — Linux ICD.
* `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` — Windows XDDM D3D driver.
* `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll` — Windows GL ICD.

Captures used:

* `captures/gl-matrix/*-secs.txt` — the GL variant matrix. Every variant runs
  `tools/gltest.c` with **one** GL state changed and a **unique window width**,
  so the scissor register `0x132c = height<<16 | width` in a pushbuffer section
  identifies which variant that section belongs to. The ring is circular and
  holds several runs at once, so *attribution by width is mandatory* — reading a
  capture file as if all its sections belonged to its own variant produces
  wrong answers, and did in earlier revisions of the other documents.
* `captures/glsweep-0528-prims-formats.txt` — `tools/parhelia-texsweep.c`,
  which uploads a full 32x32 mipmap chain and every internal format.
* `captures/live-ring-0528/decoded.txt` — the kernel's own idle/default state.

---

## 1. Which register is which — settled  **PROVEN [HW]**

Attribution by scissor width over `captures/gl-matrix/`. Each row is one
pushbuffer section that carries that variant's own `0x132c`:

| variant | GL state vs. baseline | `0x132c` | `0x11c0` | `0x11c4` |
|---|---|---|---|---|
| `tex` | RGBA 2x2, NEAREST, REPEAT | `00f00188` | `01c01000` | `f0000000` |
| `texlin` | **min+mag = GL_LINEAR** | `00f0018c` | **`01c01012`** | `f0000000` |
| `tex8` | 8x8 image | `00f00190` | `01c01000` | `f0000000` |
| `texrgb` | GL_RGB | `00f00194` | `01c01000` | `f0000000` |
| `texlum` | GL_LUMINANCE | `00f00198` | `01c01000` | `f0000000` |
| `texclamp` | **wrap S+T = GL_CLAMP** | `00f0019c` | `01c01000` | **`f0000036`** |
| `texrepl` | GL_REPLACE texenv | `00f001a0` | `01c01000` | `f0000000` |

`0x01c01012` appears in no capture taken before `texlin` and `0xf0000036` in
none before `texclamp`, so neither can be stale ring content.

**`0x11c0` is the FILTER word. `0x11c4` is the WRAP word.** Both carry two
3-bit fields at `[2:0]` and `[5:3]` that move together when the GL call sets
min and mag (resp. S and T) to the same value: `0 -> 2` for `GL_LINEAR`,
`0 -> 6` for `GL_CLAMP`.

The rest of the unit state is identical in all seven variants and is the
"known-good complete unit" a driver should start from:

```
0x1200 = 0x00040103      unit 0: pipe one-hot 3, [11:8]=1, type 4 = 2D
0x11c8 = 0x00000000
0x11cc = 0xffffffff
0x11d0 = 0x00000000
0x11d4 = 0x00000000
0x11d8 = 0x00000000
0x11dc = <VRAM byte offset> | addressSpace[1:0]     (0x09006000 in every run)
0x11e0 = width | (type << 16) | (format << 24)
0x11e4 = height | (depth << 16)
```

### 1.1 The emission grouping  **PROVEN [HW] + [BIN]**

The live per-unit emitter is **`mtx1381` `.text+0x3f680`**, not `mtx792`
`.text+0x69790` — `mtx792` is dead code, referenced only by its own
`.eh_frame` FDE. `mtx1381` works from a **0x38-byte** per-unit shadow record
and emits three dirty-groups (`0x3f7b9`-`0x3f848`):

| group | dirty bit | registers, in order |
|---|---|---|
| A | 1 | `0x1200 + unit*4`, `0x11dc`, `0x11e0`, `0x11e4` |
| B | 2 | `0x11c0`, `0x11c4`, `0x11d4`, `0x11d8` |
| C | 4 | `0x11e8`, `0x11c8`, `0x11cc`, `0x11d0` |

The capture agrees exactly — a `tex` section emits, in this order:

```
1200 = 00040103   11dc = 09006000   11e0 = 32020002   11e4 = 00010002
11c0 = 01c01000   11c4 = f0000000   11d4 = 00000000   11d8 = 00000000
11cc = ffffffff   11d0 = 00000000   11c8 = 00000000
```

(`0x11e8` absent — it is emitted only under the antialiasing gate.) This
**corrects `docs/parhelia-fragment-isa.md` §11.1**, which describes `mtx792`'s
single 12-register window and its struct-offset map; the record layout differs
(`+0x04` = `0x1200+u*4`, `+0x08` = `0x11e8`, `+0x0c` = `0x11c0`,
`+0x14` = `0x11c4`, `+0x24`/`+0x28` = `0x11d4`/`0x11d8`,
`+0x2c`..`+0x34` = `0x11dc`/`0x11e0`/`0x11e4`).

There are **four** texture units (`mtx1209` `0x3f496`, `cmp ecx,3`), and
`0x1200 + unit*4` bits `[3:0]` are a **unit write-enable mask** for the shared
`0x11c0`..`0x11e8` window (`mtx398` `0x32a00`, `mtx1381` `0x3f88e`) — which is
why the union across active units must cover `0xf`.

Independent confirmation of the whole map: the reset path `mtx1209` `0x3f3b0`
and the unit-disable packet at `mtx2663` `0x3d50a` emit exactly the live kernel
defaults seen in `captures/live-ring-0528/decoded.txt` — `0x11c4 = 0xff000000`,
`0x11cc = 0xffffffff`, `0x11d0 = 0`, and the literals `0x32020001` (`0x3d51d`)
and `0x00010001` (`0x3d524`) for `0x11e0`/`0x11e4`, byte for byte.

Surface words actually captured, one row per variant:

| variant | image | `0x11e0` | `0x11e4` | format | type `[18:16]` |
|---|---|---|---|---|---|
| `tex` | RGBA 2x2 | `32020002` | `00010002` | 0x32 | 2 |
| `tex8` | RGBA 8x8 | `32020008` | `00010008` | 0x32 | 2 |
| `texrgb` | RGB 2x2 | `33020002` | `00010002` | 0x33 | 2 |
| `texlum` | LUMINANCE 2x2 | `11040002` | `00010002` | 0x11 | 4 |
| idle unit | 1x1 dummy | `32020001` | `00010001` | 0x32 | 2 |

---

## 2. `0x11e0` / `0x11e4` — the surface descriptor  **[HW] + [BIN]**

From the descriptor builder `mtx3424` `.text+0x1eab0` (kernel HAL), cross-checked
against every capture:

| reg | bits | field |
|---|---|---|
| `0x11dc` | `[31:2]` | byte address |
| | `[1:0]` | memory pool tag — 0 = local VRAM, non-zero = AGP / system (`mtx3208` `.text+0x5b7b0`) |
| `0x11e0` | `[13:0]` | **width** in texels |
| | `[15:14]` | reserved, never written |
| | `[18:16]` | **tiling / layout code** (§5.3) |
| | `[20:19]` | **target**: 0 = 2D, 1 = cube, 2 = volume, 3 = strided |
| | `[21]` | never written by anything (mask `0xffffffa7` at `0x1ec61`) |
| | `[22]` | **texture has a border** |
| | `[23]`, `[30]`, `[31]` | format flags (`surface[+0x20]&1`, `surface[+0x10]&1`, `surface[+0x11]`) |
| | `[29:24]` | **pixel format code** |
| `0x11e4` | `[13:0]` | **height** in texels |
| | `[15:14]` | reserved, never written |
| | `[27:16]` | **depth / slice count**, minimum 1 |
| | `[31:28]` | **maximum level** — the mip chain depth (§6.3) |

Two things a driver must not miss:

* **With `0x11e0[22]` set, width, height and depth are written MINUS 2**
  (`0x1eb92`, `0x1eballow`, `0x1ebc4`): the register carries the *interior* size and
  the hardware adds the one-texel border ring. Leave bit 22 clear and pass the
  full size — GL borders are a feature ioquake3 does not use.
* `0x11e0[13:0]` is the **raw texel width**, not a rounded pitch
  (**PROVEN [HW]**: a 2x2 texture programs `2`, an 8x8 programs `8`). The
  **stride is `align(width, 32) * bytesPerTexel`**, computed by the hardware —
  `mtx3441` `.text+0x5ba40` and the mip-chain sizer `mtx1444` `.text+0x5b900`
  both round the width to 32 at every level, and `mtx3496` `.text+0x5b7e0` is
  literally `mov eax,0x20 ; ret`. `docs/parhelia-texture-layout.md` §3 says the
  field holds the *rounded* width; that is what MTXPARD writes, and since
  `align(align(w,32),32) == align(w,32)` both stacks produce the same layout.

### 2.1 Formats seen live

| GL internal format | `0x11e0[29:24]` | bytes/texel |
|---|---|---|
| `GL_RGBA` | `0x32` | 4 |
| `GL_RGB` | `0x33` | 4 (expanded, alpha forced 0xff) |
| `GL_LUMINANCE` | `0x11` | 1 |

The full D3DFORMAT / bits-per-texel tables are in fragment-isa §11.7. The
per-texel byte counts are corroborated by the staging footprints in §6.1.

---

## 3. `0x11c0` — the FILTER word

```
 31    25 24            16 15 13 12 11 10 9 8 7 6 5 4 3 2 1 0
+--------+----------------+-----+--+--+--+-+---+-+---+-+---+
|        |    LOD bias    |     | 1|  | Q|  |MIP| |MAG| |MIN|
+--------+----------------+-----+--+--+--+-+---+-+---+-+---+
```

| bits | field |
|---|---|
| `[1:0]` | **MIN** filter code |
| `[4:3]` | **MAG** filter code |
| `[7:6]` | **MIP** filter code |
| `[2]`, `[5]`, `[8]` | never set; preserved by every writer in both binaries — write mask `0xffffff24`, whose `0x24` is exactly bits 2 and 5. **OPEN** |
| `[10]` | quirk flag `Q`: set iff MAG = linear and MIN = nearest and MIP != 0 |
| `[12]` | set unconditionally at stage init (kernel `0x718d5`, MTXPARD `0x1052f8`) |
| `[24:16]` | **LOD bias**, signed 9-bit 4.4 fixed point, biased -4.0 (`0x1c0` = bias 0) |

The fields are **2 bits wide, not 3**. (`docs/parhelia-fragment-isa.md` §11.3
attributes the `0xffffff24` mask to the wrap register; it belongs here, and it
is what leaves bits 2 and 5 alone.)

### 3.1 Filter codes  **PROVEN [BIN] + [HW]**

| code | MIN / MAG | MIP |
|---|---|---|
| 0 | nearest (point) | **no mipmapping** — base level only |
| 1 | (nearest; `D3DTEXF_POINT` passes through) | mip **nearest** — pick one level |
| 2 | **linear** | mip **linear** — blend two levels (trilinear when MIN is also 2) |
| 3 | **anisotropic** | — |

Evidence:

* **[BIN]** MTXPARD's translation tables, indexed by DX8 `D3DTEXTUREFILTERTYPE`:
  `.data 0x1d4558` (MIN) and `0x1d4570` (MAG) are both `{0,0,2,3,2,2}`;
  `0x1d4588` (MIP) is `{0,1,2,2,2,2}`. That the index really is `D3DTEXF_*` is
  pinned by the legacy-DDI remap tables `0x1d1f8c` (mag `{0,1,2,4,5,3}`),
  `0x1d1fa4` (min, identity) and `0x1d1fb4` (mip `{0,0,1,2}`), which convert the
  DX7 `D3DTFG_/TFN_/TFP_` enums into DX8 numbering.
* **[BIN]** the kernel's three 4-entry hint tables — `mtx1864` `.data 0x19370` =
  `{0,2,2,3}`, `mtx2559` `.data 0x19360` = `{0,2,2,3}`, `mtx1674`
  `.data 0x19350` = `{1,1,2,2}`, written to the three shadow slots at `mtx1652`
  `.text+0x70cde`..`0x70d0d` — give the quality ladder
  `nearest (0,0,1)`, `bilinear (2,2,1)`, `trilinear (2,2,2)`,
  `anisotropic (3,3,2)` as `(MIN, MAG, MIP)`. (`parhelia-fragment-isa.md`
  §9.5.6 read these into `0x11c4`; with §1 they belong to `0x11c0`.)
* **[HW]** `GL_NEAREST` min+mag -> low byte `0x00`; `GL_LINEAR` min+mag -> low
  byte `0x12`. Confirms nearest = 0, linear = 2 and **MIP = 0 = no mipmapping**
  (the GL captures use no mipmaps).
* **[HW]** the Linux ICD's own per-parameter records confirm the same codes and
  name the parameters. Each `glTexParameteri` emits a two-dword record:

  | record header | parameter | capture / GL call | value |
  |---|---|---|---|
  | `0x00100000` | MAG filter | `tex_linear` (`GL_LINEAR` mag only) | **2** |
  | `0x00110000` | MIN filter | `m_linear_mip_linear` | **2** |
  | `0x00120000` | MIP filter | `m_linear_mip_linear` | **2** |
  | `0x00130000` | LOD bias (float) | `m_lodbias` | `0x40000000` = 2.0f |

**MIN vs MAG bit position is PROVEN.** Both binaries load the three values from
the software stage block in the order MAG, MIN, MIP and then **explicitly cross
MIN and MAG over** when packing the register:

```
kernel mtx1271: 0x70503 (word>>17)&7 = MINFILTER -> slot +0x130
                0x704e0 (word>>14)&7 = MAGFILTER -> slot +0x12c
                0x704c4 (word>>20)&7 = MIPFILTER -> slot +0x134
kernel mtx1723: 0x71d04  MIN  -> shadow[+0x0c] [1:0]   (shift 0)
                0x71d22  MAG  -> shadow[+0x0c] [4:3]   (shift 3)
                0x71d3b  MIP  -> shadow[+0x0c] [7:6]   (shift 6)
MTXPARD 0x17a30e-0x17a330:
                eax = ((MIP&3) << 3 | (MAG&3)) << 3 | (MIN&3)
```

(`shadow[+0x0c]` is `0x11c0`, anchored by the LOD-bias field at `+0x0e`, §3.3.)
So the **software** block order really is MAG, MIN, MIP — which is why the ICD
numbers its records that way — and the **hardware** order is MIN, MAG, MIP. The
two differ, and the crossover is explicit in two independent binaries.

Kernel translation tables (`nm mtx_parhelia.o`, `.data`):
`mtx3592` @0x19420 = MIN = `{0,0,2,3,2,2}`, `mtx296` @0x19400 = MAG =
`{0,0,2,3,2,2}`, `mtx1887` @0x193e0 = MIP = `{0,1,2,2,2,2}`. MTXPARD
counterparts `0x1d4558` / `0x1d4570` / `0x1d4588`.

### 3.2 The six GL minification filters and two magnification filters

This is the table ioquake3 needs.
`0x11c0 = 0x01c01000 | MIN | (MAG << 3) | (MIP << 6)`

| `GL_TEXTURE_MAG_FILTER` | MAG `[4:3]` | contribution |
|---|---|---|
| `GL_NEAREST` | 0 | `0x00` |
| `GL_LINEAR` | 2 | `0x10` |

| `GL_TEXTURE_MIN_FILTER` | MIN `[1:0]` | MIP `[7:6]` | contribution |
|---|---|---|---|
| `GL_NEAREST` | 0 | 0 | `0x00` |
| `GL_LINEAR` | 2 | 0 | `0x02` |
| `GL_NEAREST_MIPMAP_NEAREST` | 0 | 1 | `0x40` |
| `GL_LINEAR_MIPMAP_NEAREST` | 2 | 1 | `0x42` |
| `GL_NEAREST_MIPMAP_LINEAR` | 0 | 2 | `0x80` |
| `GL_LINEAR_MIPMAP_LINEAR` | 2 | 2 | `0x82` |

ioquake3's default `r_textureMode GL_LINEAR_MIPMAP_NEAREST` (bilinear +
mip-nearest, mag linear) is therefore
`0x01c01000 | 2 | (2 << 3) | (1 << 6)` = **`0x01c01052`**.

`GL_TEXTURE_MAX_ANISOTROPY_EXT` selects MIN = MAG = 3. The hardware maximum is
**2**, and **no consumer was found that turns a max-anisotropy *degree* into a
sampler field** — anisotropy is expressed only as filter code 3. ioquake3 does
not need it.

### 3.3 `[24:16]` — LOD bias  **PROVEN [BIN], and it explains the captured constant**

```
0x11c0[24:16] = (int)((clamp(lodBias, -4.0, +4.0) + profileOffset - 4.0) * 16.0)
                as a SIGNED 9-bit value
```

Kernel `mtx1723` `.text+0x71e8b`-`0x71edd`:

```
0x71e8f  movss   xmm1,[rcx+0x18]     ; D3DTSS_MIPMAPLODBIAS
0x71e94  ucomiss xmm1,0.0 ; jb ...   ; negative -> clamp against -4.0
0x71ea8  cmpnltss/andps/andnps/orps  ; xmm0 = min(bias, 4.0)
0x71eb9  addss   xmm0,[rcx+0x7e]     ; + per-stage profile offset
0x71ec6  subss   xmm0,4.0
0x71ece  mulss   xmm0,16.0
0x71ed2  cvttss2si edx,xmm0
0x71ed6  and dx,0x1ff ; or into WORD [rcx+0x0e]   ; = 0x11c0[31:16], low 9 bits
```

MTXPARD equivalent `0x17a4bf`-`0x17a533`, constants in `.rdata`:
`0x19cf74` = 16.0, `0x19d450` = 4.0, `0x19ee24` = -4.0, `0x19cd5c` = 0.0.

`profileOffset` is an **application-profile LOD-bias offset in 1/16 steps**,
decoded from a packed per-stage control-panel byte (MTXPARD `0x17afb9b`, scaling
a 7-bit magnitude by `+0.0625` / `-0.0625` from `.rdata 0x19ccc0`/`0x19ccc4`).
**It is 0 by default.**

So with `lodBias = 0` and no profile offset the field is `(0 + 0 - 4.0) * 16 =
-64`, which as a 9-bit two's-complement value is `0x1c0` — **exactly the
captured `0x01c01000`.** The constant is a computed default, not a magic number,
and because the formula has no level term it is the same for a mipmapped
texture. A driver that never applies a LOD bias can hard-code `0x01c00000`.

`GL_EXT_texture_lod_bias` is advertised by libGL with
`GL_MAX_TEXTURE_LOD_BIAS_EXT = 2` (`docs/parhelia-gl-limits.md` §13, emit at
`0x10ff14`), and reaches the kernel as record `0x13` carrying the float
verbatim (**[HW]**, §3.1). ioquake3 does not use it.

---

## 4. `0x11c4` — the WRAP word

```
 31        24 23        16 15 14 13    11 10 9 8   6 5   3 2   0
+------------+------------+-----+--------+--+-+-----+-----+-----+
| LOD max    | LOD min    |     | cyl WRP|  |K|  W  |  V  |  U  |
+------------+------------+-----+--------+--+-+-----+-----+-----+
```

| bits | field |
|---|---|
| `[2:0]` / `[5:3]` / `[8:6]` | **U (S) / V (T) / W (R) address mode**, 3 bits each |
| `[10:9]` | the **colour-key control pair**. One is the enable and one is cleared; the two independent reads of the same code disagree about which (`[9]` cleared at `0x71b08` and `[10]` set at `0x71b13`-`0x71b40` on one reading; `[9]` = key-*disable* on the other). The kernel's `0x249` default has bit 9 set and bit 10 clear; the GL path leaves both clear. **Keying is off either way when `0x11d0` = 0** — leave both clear and copy the vendor |
| `[13:11]` | per-axis **cylindrical wrap** (`D3DRS_WRAP` U/V/W); kernel `0x721b2`/`0x721cf`/`0x721ec` |
| `[23:16]` | **LOD minimum clamp**, unsigned 4.4 |
| `[31:24]` | **LOD maximum clamp**, unsigned 4.4 — `clamp(maxLOD,0,15)*16`, so `0xf0` = 15.0 |

### 4.1 The address-mode codes  **PROVEN [BIN] + [HW]**

One shared table, byte-identical in both stacks: MTXPARD `.data 0x1d45d0` and
kernel symbol `mtx3018` `.data 0x19330`, both `{0, 0, 4, 1, 7, 5, 6}`, indexed
by a unified internal enum:

| GL mode | D3D | internal enum | **hardware code** |
|---|---|---|---|
| `GL_REPEAT` | `D3DTADDRESS_WRAP` | 1 | **0** |
| `GL_MIRRORED_REPEAT` | `MIRROR` | 2 | **4** |
| `GL_CLAMP_TO_EDGE` | `CLAMP` | 3 | **1** |
| `GL_CLAMP_TO_BORDER` | `BORDER` | 4 | **7** |
| — | `MIRRORONCE` | 5 | **5** |
| `GL_CLAMP` | (Matrox extension of the enum) | 6 | **6** |

Codes >= 6 pull in the border colour (`0x11c8`, §8). Code 7 is downgraded to 6
under an antialiasing-related condition (MTXPARD `0x17a643`, `0x17a66f`).

Cross-checks:

* **[HW]** the register itself: `GL_REPEAT` -> `0xf0000000`, `GL_CLAMP` ->
  `0xf0000036` = (6, 6, 0). Matches the table exactly.
* **[HW]** the Linux ICD's own wrap records — header `0x000d0000` = wrap S,
  `0x000e0000` = wrap T — carry the *internal enum*, and the measured values
  are `GL_REPEAT` -> **1**, `GL_CLAMP_TO_EDGE` -> **3**, `GL_CLAMP` -> **6**,
  which is that enum on the nose. Captured in
  `captures/glsweep-0528-prims-formats.txt` ops `w_repeat`, `w_edgeclamp`,
  `w_clamp`.
* **[BIN]** MTXOGL/libGL keep the same enum in `texObj+0x08` (WRAP_S `[8:6]`,
  WRAP_T `[11:9]`, WRAP_R `[14:12]`); jump tables `0x103b8cdc` / `0x103b8cf4` /
  `0x103b8d0c`, and libGL `0xbe654` (`and ah,0x8f ; or ah,0x60`) for `GL_CLAMP`.
* **[BIN]** MTXPARD's textured-blit constant `0x11c4 = 0x49` = (1,1,1) is
  clamp-to-edge on all three axes — which is what a blit wants.

This resolves the ambiguity `docs/parhelia-fragment-isa.md` §11.3 carried
("which of 1 and 2 is clamp and which is mirror is not determined"): **1 is
clamp-to-edge and 4 is mirror**, and the field is a 3-bit enum, not a bitfield.

### 4.2 GL wrap table

`0x11c4 = 0xf0000000 | U | (V << 3) | (W << 6)`

| GL wrap mode | code |
|---|---|
| `GL_REPEAT` | 0 |
| `GL_CLAMP_TO_EDGE` | 1 |
| `GL_MIRRORED_REPEAT` | 4 |
| `GL_CLAMP` | 6 |
| `GL_CLAMP_TO_BORDER` | 7 (downgraded to 6 on some paths) |

ioquake3 uses `GL_REPEAT` and `GL_CLAMP_TO_EDGE` only — codes 0 and 1.

### 4.3 `[31:16]` — the LOD clamps

`[23:16]` is the LOD *minimum* and `[31:24]` the LOD *maximum*, both unsigned
4.4 (MTXPARD `0x17a4bf`ff, kernel `mtx1723`). The captured `0xf0000000` is
`min = 0.0`, `max = 15.0`; the kernel's own default `0xff000000` is
`max = 15.9375`, the field's ceiling. So `docs/parhelia-texture-layout.md`
§8.2's "LOD bias and LOD clamp **CONFIRMED absent**" is wrong — both exist,
and `docs/parhelia-texture-path.md` had them right, just attached to the
opposite registers.

`GL_TEXTURE_MIN_LOD` / `GL_TEXTURE_MAX_LOD` map straight onto these.
`0xf0000000` is the correct value for any texture that does not clamp LOD.

## 5. Texture memory layout — the swizzle, recovered

**Textures are tiled when the layout code is non-zero, and the tile is not the
render target's.** The exact swizzle is recoverable because the **CPU** applies
it: the kernel HAL uploads a texture by writing texels into swizzled positions.
Layout code 0 is a genuine linear mode and is what our driver should use first
(§5.5).

### 5.1 The swizzle  **PROVEN [BIN]**

`mtx_parhelia.o`, dispatcher `mtx2515` `.text+0x23140` (`0x2317a`-`0x23197`):
an uncompressed upload goes to **`mtx2146` `.text+0x21120`** when
`texObj+0x80 == 2` (the swizzled pool) and to `mtx3550`/`mtx1828` — plain
row-by-row `LocMemCpy` — otherwise. DXT goes to `mtx2799` `.text+0x20720`,
the same swizzle applied to 4x4 blocks.

Read out of `mtx2146` `0x213df`-`0x2158d`:

```
/* B = bytes per texel; block = 32 texels wide x 16 rows; blockBytes = 512*B */
paddedWidth = align(width, 32)
blockIndex  = (((y >> 4) * paddedWidth) + x) >> 5
base        = (blockIndex * depth + z) * blockBytes      /* slices interleave per block */

inBlockByteOffset =
  B == 1 : ((y & 0xc) << 5) | ((y & 3) << 2) |  (x & 3)       | ((x & 0x1c) << 2)
  B == 2 : ((y & 0xc) << 6) | ((y & 3) << 2) | ((x & 1) << 1) | ((x & 0x1e) << 3)
  B == 4 : ((y & 0xc) << 7) | ((y & 3) << 2)                  | ((x & 0x1f) << 4)

addr = surfaceBase + base + inBlockByteOffset
```

The primitive is a **16-byte micro-tile = 4 consecutive bytes x 4 consecutive
rows**. Micro-tiles run across 32 texels of x, then in 4-row bands down y,
filling a 32x16-texel block; blocks are row-major across `paddedWidth`.

Spot-verified independently against the object file for this document:

```
213f6: and eax,0xc          21407: and edx,0x3
2140a: shl ecx,0x7          21413: lea r11d,[rdx*4+0x0]     ; (y&3) << 2
2140d: shl esi,0x6                                          ; (y&0xc) << 7/6/5
21410: shl eax,0x5
2144d: cmp edi,0x4 ; je 21578    -> 2157c: and [rsp+0xc],0x1f ; 21581: shl ...,0x4
21456: cmp edi,0x2 ; je 21592    -> 2159a: and [rsp+0xc],0x1e ; 2159f: and eax,0x1
       fallthrough (B==1)        -> 21467: and [rsp+0xc],0x1c ; 2146c: and eax,0x3
                                    21478: shl eax,0x2
```

As texel-index bit maps inside a block:

| B | bit 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| 4 (32 bpp) | y0 | y1 | x0 | x1 | x2 | x3 | x4 | y2 | y3 |
| 2 (16 bpp) | x0 | y0 | y1 | x1 | x2 | x3 | x4 | y2 | y3 |
| 1 (8 bpp) | x0 | x1 | y0 | y1 | x2 | x3 | x4 | y2 | y3 |

### 5.2 It is a different swizzle from the render target

| | 3D colour render target | texture |
|---|---|---|
| tile | 64x32 px / 8192 B (32 bpp) | **32x16 px / 2048 B** (32 bpp) |
| low index bits | `x0,x1,x2` | **`y0,y1`** |
| parity term | `bit7 = x5 XOR (tileRow & 1)` | **none** |
| pitch used | surface pitch | `align(width, 32)` |

They are not the same layout, and a driver must not reuse the render-target
formula for textures. (Every non-zeroing `xor reg,reg` in the object was
enumerated; none appears in texture address arithmetic.)

**A dissenting reading, and why it is rejected.** A second pass over the same
binary did not locate the opcode-`0x0a` box handler and argued from
`docs/parhelia-texture-layout.md` §1.1 — "the uploaded 64x64 texture reads back
with period 256 through the aperture, so there is no CPU swizzle; 'textiled'
must be a memory-controller remap transparent to CPU access". That argument
fails on its premise: §5.4 shows that the buffer that document measures is the
**CPU staging heap**, not VRAM seen through an aperture. With the premise gone, the explicit
code above stands. If the remap theory were nonetheless true, `mtx2146` would be
swizzling on top of a remap and the vendor's own textures would be scrambled.

The RT rule, restated in the 32x32 unit it decomposes into, for reference only:

```
idx  = x0 | x1<<1 | x2<<2 | y1<<3 | x3<<4 | x4<<5 | y0<<6 | y2<<7 | y3<<8 | y4<<9
N    = (y>>5) * (pitch>>5) + (x>>5)
addr = base + (N ^ ((y>>5) & 1)) * 4096 + idx*4
```

### 5.3 The tiling codes `0x11e0[18:16]`  **PROVEN [BIN]**

Assigned per variant index in `mtx2029` `.text+0x1e9a0`, driven by the surface
class at `texObj+0xc0`:

| class | source | codes produced |
|---|---|---|
| 0 | `mtx3100` `0x27058` | `mtx1711` `0x1ea6d` forces **0** on every entry |
| 1 | `0x27058` | 2 (most), 3 (indices 0/6/8), 5 (DXT) |
| 2 "textiled" (`params+0x10 == 0xa`) | `0x272d5` | **4** (most), 3 (index 5), 5 |
| 4 compressed | `0x272c6` | 5 |

`mtx3574` `0x1e8fb`-`0x1e915` additionally remaps 2/3/4 -> 4 when the base
address carries pool tag 2. `mtx263` `.text+0x519a0` sets `params+0x10 = 0xa`
only when the `UseTextiled` switch is on, bpp != 32, and the format is not
`0x29`/`0x2a` — so `UseTextiled` is about class **2**, not about tiling in
general.

| code | layout |
|---|---|
| **0** | **linear**, pitch `align(w,32) * bpp/8`, row granularity 1, explicit byte size |
| 1 | linear, 2-row granularity |
| 2, 3 | tiled (§5.1). No software function distinguishes 3 from 2 — **OPEN** |
| 4 | "textiled" (the `UseTextiled` mode) |
| 5 | block-compressed (DXT), the only 4-bpp case |
| 6, 7 | rejected (`mtx3441` returns 0, `mtx1444` returns -1) |

Pitch, `mtx3441` `.text+0x5ba40`: tiled codes use
`bpp * next_pow2(max(w,32)) / 8`; linear uses `bpp * align(w,32) / 8`;
`w > 0x1fff` clamps to 8192 texels. **Minimum pitch is 32 texels = 128 bytes at
32 bpp** either way.

The layout code also drives the two allocation constants used by the mip-chain
sizer (§6.2). `mtx3208` `.text+0x5b7b0` picks the table from `0x11dc[1:0]`:
address space 0 (local VRAM) uses `mtx1219` below, anything else uses
`mtx2981` `.text+0x5b520`.

**`A` — per-level start alignment, bytes** (`mtx1219` `.text+0x5b610`; the
`flag` argument is 0 for per-level rounding and 1 for the whole allocation):

| layout | bpp | A (per level) | A (allocation) |
|---|---|---|---|
| 0 | any | **0x40** | 0x200 |
| 1 | 1, 8 | 0x40 | 0x40 |
| 1 | 16 | 0x40 | 0x80 |
| 1 | 30, 32 | 0x100 | 0x100 |
| 2, 3 | 8 | 0x400 | 0x2000 |
| 2, 3 | 16 | 0x800 | 0x1000 |
| 2, 3 | 30, 32 | **0x1000** | 0x2000 |
| 4 | 8 | 0x400 | 0x1000 |
| 4 | 16, 30, 32 | 0x800 | 0x2000 |
| 5 | 4 | 0x200 | 0x800 |
| 5 | 8 | 0x400 | 0x1000 |
| anything else | | invalid (`0xffffffff`) | |

`if (record[+0x10] & 0x7fc00) A >>= 1` at `0x5b6b0`-`0x5b6bb`, after the switch,
so it applies to every layout and both flag values.

**`Ha` — row-count granularity, rows** (`mtx625` `.text+0x5b7f0`):

| condition | Ha |
|---|---|
| `0x11dc[1:0] != 0` (non-VRAM) — tested first, overrides everything | 16 |
| layout 0 | **1** |
| layout 1 | 2 |
| layout 2, 3 | 32 (16 with the variant bit) |
| layout 4, 5, variant bit set | 32 |
| layout 4, 5, 30/32 bpp (arm `0x5b8a6`) | 16 if height <= 16, else 32 |
| layout 4, 5, 4/8/16 bpp (arm `0x5b8c0`) | 32 if height <= 32, else 64 |
| anything else | invalid |

`height` is that level's own `0x11e4[13:0]`, so for layouts 4/5 `Ha` changes
level by level.

### 5.4 What the client stages is still linear  **PROVEN [HW]**

The 4 MB region the client writes is a **staging heap**, not the surface: it is
a byte-granular bump allocator (successive textures at `0x0000, 0x1000, 0x2000,
0x2400, 0x2800, ... 0x8554`), the sampled texture's base register reads
`0x11dc = 0x09006000`, and the surface is created by a mailbox ioctl (cmd
`0x0c`) that returns an id, not an mmap cookie.

Its contents were re-checked against both models over three captures:

```
linear, dense, row-major :  4096/4096, 512/512, 4096/4096 texels match
32x16 texture swizzle    :  no
render-target swizzle    :  512/4096 (only the coincident positions)
```

`captures/tex_64x64_0.txt` is `GL_RGBA` and `tex_64x64_1.txt` is `GL_RGB`; the
latter simultaneously pins the in-memory texel format as **A8R8G8B8 dwords**
(byte order B, G, R, A) with `GL_RGB` expanded to alpha `0xff`. 8704 texels
checked, zero mismatches. So the swizzle is applied by the **kernel**, exactly
as §5.1 describes, and `docs/parhelia-texture-layout.md`'s "texture memory is
LINEAR, CONFIRMED on hardware" is measuring the wrong buffer.

### 5.5 What our driver should do

**Start with tiling code 0.** It is a genuine hardware layout — the vendor's own
`mtx1711` `0x1ea6d` forces it for surface class 0 — it needs no swizzle, its row
granularity `Ha` is 1 and its per-level alignment `A` is `0x40`, and the upload
path for a non-swizzled pool is a plain row copy (`mtx3550` / `mtx1828`):

```
pitchBytes  = align(width, 32) * bytesPerTexel
addr(x, y)  = base + y * pitchBytes + x * bytesPerTexel
0x11e0[18:16] = 0
```

Two caveats, stated because they are real:

* **No capture shows the sampler with code 0.** The vendor GL always programs 2
  (32 bpp) or 4 (8 bpp), the kernel registers `UseTextiled` defaulting to **1**,
  and the code that chooses 0 vs 2 vs 4 was not located in this object
  (`mtx325` never writes the field — it preserves bits 16-18 with
  `and ecx,0x7f` / `and ecx,0xe7` at `0x28368`/`0x2837b`, so one layout code is
  inherited by every level of a chain). Confirm code 0 with §9.1 check 1 before
  building on it.
* Code 0 costs memory *bandwidth*, not memory: it is a linear surface, so it is
  actually **cheaper** in bytes than code 2, which rounds every level's height
  to 32 rows and every level's size to `0x1000` at 32 bpp.

If code 0 turns out not to work in the texture unit, fall back to code 2 with
the §5.1 swizzle and the §6.2 padding — that is exactly what the vendor does.

---

## 6. Mipmaps

### 6.1 What the ICD hands the kernel  **PROVEN [HW]**

`captures/glsweep-0528-prims-formats.txt`, op `m_upload_chain`, uploads a
32x32 `GL_RGBA` chain down to 1x1 and then draws with
`GL_NEAREST_MIPMAP_NEAREST`. Region 6 — the 4 MB CPU-written staging heap —
changes by **exactly 5460 bytes, span `0x7000`..`0x8553`**:

```
4096 + 1024 + 256 + 64 + 16 + 4 = 5460 = 0x1554
```

The staging bytes corroborate the offsets: the sweep uploads the *same* source
buffer for every level, and the first dword of each level reads back identical
at exactly those offsets —

```
007000 6f00254a   008000 6f00254a   008400 6f00254a
008500 6f00254a   008540 6f00254a   008550 6f00254a
```

The six upload records (opcode `0x0a`, element size `0x48`) decode exactly:

| record | staging offset (+0x08) | width (+0x24) | height (+0x28) | depth (+0x2c) | **level (+0x30)** |
|---|---|---|---|---|---|
| 0 | `0x7000` | 32 | 32 | 1 | 0 |
| 1 | `0x8000` | 16 | 16 | 1 | 1 |
| 2 | `0x8400` | 8 | 8 | 1 | 2 |
| 3 | `0x8500` | 4 | 4 | 1 | 3 |
| 4 | `0x8540` | 2 | 2 | 1 | 4 |
| 5 | `0x8550` | 1 | 1 | 1 | 5 |

All six records carry the **same destination surface id** (`6`) and the same
staging-heap pointer. So the client stages the chain **dense** — that is the
*source* layout only — and hands the kernel one record per level with an
explicit level number and a surface id. **The kernel computes the destination
offsets.**

The level index in the record is `level - GL_TEXTURE_BASE_LEVEL`
(libGL `0x1cd100`), i.e. **hardware level 0 is the GL base level**.

### 6.2 The chain layout in VRAM  **PROVEN [BIN]**

The ICD only ships `{flags, class, formatCode, w, h, d, levelCount}` to the
kernel (built at libGL `0x1cce40`-`0x1ccef9`, sent by `0x357d20` as request
opcode 12). Three kernel chain builders lay it out: **`mtx325` `.text+0x28190`**
(2D and volume), `mtx1747` `.text+0x28e80` (2D), `mtx3887` `.text+0x27610`
(cube).

```
w_i = max(W >> i, 1)   h_i = max(H >> i, 1)   d_i = max(D >> i, 1)
                                       /* cube forces h_i = w_i  (0x282cf-0x28304) */
offset[0] = 0
offset[i] = offset[i-1] +     size[i-1]        /* 2D, volume: 0x2847b, 0x2963d */
offset[i] = offset[i-1] + 6 * size[i-1]        /* cube:       0x27ed4          */
total     = align(offset[n-1] + (cube?6:1)*size[n-1], allocGranule)
faceAddr(level, face) = offset[level] + face * size[level]   /* 0x27cb3 */
```

**Cube maps do multiply by 6 per level — confirmed.**

Per-level size, **`mtx1444` `.text+0x5b900`**:

```
tiling 0        : size = align( explicitByteSize , A )     /* = align(w,32)*bpt*h */
tiling 1..5     : size = align( align(w_i,32) * align(h_i,Ha) * bpp/8 * d_i , A )
tiling 6,7      : invalid
```

* **`align(width, 32)` is applied at EVERY level.** `mtx3496` `.text+0x5b7e0` is
  literally `mov eax,0x20 ; ret`. A 2x2 level still costs a 32-texel pitch.
* **`Ha`** (row-count granularity) and **`A`** (per-level start alignment) are
  the full tables in §5.3. For the two cases that matter: layout 0 gives
  `Ha = 1`, `A = 0x40`; layout 2 at 32 bpp gives `Ha = 32`, `A = 0x1000`.

  Note `0x400 / 0x800 / 0x1000` is exactly `32 * 32 * bpp/8` — the allocation
  quantum for the tiled codes is a 32x32-texel area, i.e. two of the 32x16
  addressing blocks of §5.1. A consequence worth seeing before choosing a
  layout: at layout 2, **every** level of a 32x32 RGBA chain computes
  `align(w,32) * align(h,32) * 4 = 4096` and is already `A`-aligned, so nothing
  shrinks — the chain would cost 6 * 4096 = 24576 bytes were it not for the mip
  tail below. At layout 0 the same chain costs 6144.

This **corrects `docs/parhelia-texture-layout.md` §5**, which states "no
per-level padding beyond the 32-texel pitch rounding": that came from MTXPARD;
the Linux HAL pads every level to `A`.

#### The mip tail — what happens to a 2x2 or 1x1 level  **PROVEN [BIN]**

Every level with `w <= 16 && h <= 16` (and `d <= 31`) is packed into **one
shared 32x16-texel block** and adds **zero** bytes: the first such level gets a
normal size and offset, and every later one reuses that same size and offset
(`mtx325` `0x2854c`, `mtx3887` `0x277fe`). Their register dimensions are forced
to 32x16. The positions are three constant `.rodata` tables — `mtx3067`
`0xc3730` (X), `mtx516` `0xc3710` (Y), `mtx1834` `0xc36f0` (texel index):

| tail slot | level size | X | Y | texel index |
|---|---|---|---|---|
| 0 | 16x16 | 0 | 0 | 0 |
| 1 | 8x8 | 16 | 0 | 16 |
| 2 | 4x4 | 24 | 0 | 24 |
| 3 | 2x2 | 16 | 8 | 272 = 8*32 + 16 |
| 4 | 1x1 | 24 | 8 | 280 = 8*32 + 24 |

`mtx1834[t] == Y*32 + X` exactly, which is what pins the block as 32 texels
wide. The five sub-images tile the 32x16 block without overlap. The stored
per-level byte offset is `bpp * mtx1834[t] >> 3`, i.e. `texelIndex *
bytesPerTexel` (`0x28a9c`).

**A driver that mipmaps must reproduce this.** Hard-coded texel coordinates
sitting in `.rodata` only make sense if the silicon looks in those positions.

*One detail has two readings.* The shortcut at `mtx325` `0x28400` clearly makes
a small level stop computing its own geometry and reuse an earlier level's
record. Whether it also reuses that level's **offset** — so the tail levels
share one block and add zero bytes — or only its **size** — so each tail level
still costs a full block — was read both ways. The shared-block reading is the
one the `mtx3067`/`mtx516`/`mtx1834` X/Y/index tables support: five sub-images
that tile a 32x16 block without overlap have no purpose unless they occupy the
same block. §9.1 check 2 distinguishes them in one run; until then, if you must
guess, allocate as if each tail level were separate (the harmless direction) but
place them at the tabulated coordinates.

#### Worked example: 32x32 `GL_RGBA`, tiling 0

| level | w x h | `align(w,32) * h * 4` | placement |
|---|---|---|---|
| 0 | 32x32 | 4096 | offset 0 |
| 1..5 | 16x16 down to 1x1 | **2048 total** | one 32x16 tail block at offset 4096 |
| | | **total 6144** | |

Not the 5460-byte dense chain the client stages, and not the 8064 a naive
per-level allocation would give.

### 6.3 How many levels, completeness, and the level count register

* Level count (libGL `0x1ccfbe`): `base = max(0, GL_TEXTURE_BASE_LEVEL)`,
  `last = min(base + log2(max(w,h,d)), GL_TEXTURE_MAX_LEVEL)` — **all the way
  down to 1x1** unless capped. If the minification filter is *not* a mipmap
  filter, `last = base`: one level. **[BIN]**
* The level index in the upload record is `level - GL_TEXTURE_BASE_LEVEL`
  (libGL `0x1cd100`), so **hardware level 0 is the GL base level**. **[BIN]**
* **`0x11e4[31:28]` is the maximum level** — `mtx3424` ORs `(arg & 0xf) << 4`
  into descriptor byte `+0x0f` (`0x1eb54`, `0x1ecdc`), and all nine call sites
  agree: literal `0` for a non-mipped surface (`mtx3100` `0x270f4`),
  `numLevels - 1` for a chain parent (`0x2868d`, `0x29206`), and
  `numLevels - 1 - level` for a per-level view (`0x2895a`, `0x29101`, `0x29572`,
  `0x29621`, `0x27c80`). **PROVEN.** (`0x11e0[23:21]` carries no level count —
  bit 21 is never written, mask `0xffffffa7` at `0x1ec61`.)
  The cube parent `mtx3887` `0x279cb` passes `numLevels` with no decrement and
  no compensating adjustment was found — either cube semantics differ or it is a
  vendor off-by-one. **OPEN**, and irrelevant to ioquake3.
* **No automatic mip generation.** `GL_GENERATE_MIPMAP_SGIS` (`0x8191`) appears
  zero times in the 41 MB libGL disassembly, `GL_SGIS_generate_mipmap` is not in
  the extension string, and there is no downsample loop. The application
  supplies every level — ioquake3 does, in `R_MipMap`. **[BIN]**
* **An incomplete chain disables the unit, it does not degrade.** The
  completeness test at libGL `0x1541d0` requires, for every `l > base`,
  `w[l] == max(w[l-1] >> 1, 1)`, `h[l] == max(h[l-1] >> 1, 1)`, the same
  internal format and the same border; cube maps must also be square and equal
  across faces. On failure `0x1ccbf0` returns 0, the emitter sends handle **0**
  and sets bit `unit+4` of `hwctx[+0x778]` (`0x10ffd0`): the unit is left with
  **no texture bound**. Same path for a missing base level and for
  `GL_OUT_OF_MEMORY` (`0x14a451`). **[BIN]**
* Level images live at `texobj + 0x10 + level*8`, 12 slots; the cube index is
  `face*12 + level` (libGL `0x14a65f`).

### 6.4 The hardware walks the chain from one base address  **INFERRED (strong)**

**No second base address, no mip-stride register and no per-level `0x11dc` ever
reaches hardware.** The three shadow dwords `texObj+0x1a0/0x1a4/0x1a8` — which
`mtx1271` copies into `0x11dc`/`0x11e0`/`0x11e4` — are written in only four
places in the whole object: the variant copy in `mtx1271`
(`0x702fc`/`0x70305`/`0x7030e`), a clone in `mtx2325`, and three constructors
that zero them.

So the model is: the hardware walks an **implicit contiguous chain** from
`0x11dc`, using width / height / depth / format / tiling, bounded below and
above by `0x11e4[31:28]` (levels available) and the `0x11c4` LOD clamps, and
biased by `0x11c0[24:16]`.

Three things support it and nothing contradicts it:

1. The sampler has a MIP field whose code 2 is "blend two levels", plus a LOD
   bias and LOD min/max clamps. None of that means anything if the CPU picks
   the level, and `GL_LINEAR_MIPMAP_LINEAR` is advertised and works.
2. The mip tail's texel coordinates are **constants in `.rodata`** (§6.2). A
   layout that exists only for the CPU would not need a fixed one.
3. `0x11e4[31:28]` exists and is set to the chain depth.

The per-level texture objects the driver creates, and `mtx1271`'s per-level
descriptor copy, exist so the **CPU** can address one level for upload — each
re-runs `mtx3574` with `base + offset[level]`. That is also how
`GL_TEXTURE_BASE_LEVEL` is implemented, which is why the upload record carries
`level - baseLevel`.

**Correction to a prior finding:** the 9-entry array at `texObj+0xd8`
(9 entries x 0x14) is a **per-format-variant** array, not a mip-level array —
`mtx3424` `0x1ec67`-`0x1ec77` gives each slot its own pixel-format code while
`mtx3574` `0x1e8f4` writes the *same* base address into all nine, and
`mtx2577` `.text+0x70eb0` picks a slot by capability mask
(`(reqMask & capMask) == reqMask`). `texObj+0x1ac[16:12]` is the variant index,
not a level. This supersedes `docs/parhelia-texture-path.md`'s "9-entry
descriptor array ... one level at a time".

Run §9.1 check 2 before trusting mipmapping.

### 6.5 Partial uploads

The upload record carries `x0,y0,z0` at `+0x18/+0x1c/+0x20` and
`width,height,depth` at `+0x24/+0x28/+0x2c`, so a sub-rectangle upload is
expressible. The Linux ICD does not use it: `s_subimage`
(`glTexImage2D` 32x32 followed by `glTexSubImage2D(8,8,16,16)`) emits a
**single** record with `x0=y0=0` and `32x32`, and stages 4096 bytes — it
re-uploads the whole level. **[HW]**

---

## 7. Limits and non-power-of-two

| limit | value | evidence |
|---|---|---|
| `GL_MAX_TEXTURE_SIZE` | **2048** | libGL `0x13df0b` [BIN]; MTXPARD `.data` `MaxTextureWidth/Height` 2048 |
| `GL_MAX_3D_TEXTURE_SIZE` | 2048 | libGL `0x13bced` [BIN] |
| `GL_MAX_CUBE_MAP_TEXTURE_SIZE` | 2048 | libGL `0x13c546` [BIN] |
| texture units | **4** | libGL `0x146aa7` hard check [BIN] |
| max anisotropy | 2 | both stacks agree [BIN] |
| `0x11e0[13:0]` field width | 14 bits, pitch computation clamps at 8192 | MTXPARD `0xa40f0` [BIN] |

Corroboration from the kernel HAL: `mtx1723` `0x719a4`/`0x719ab`/`0x719b2` and
`0x72a83` publish the caps `0x800/0x800/0x800`, and `mtx3634` `.text+0x26530`
routes any texture wider or taller than `0x800` to the **software** rasteriser
`mtx918` `.text+0x24bd0` (`0x26588`, `0x2659d`, `0x26773`). Hardware field
widths are 14 / 14 / 12 bits.

**Non-power-of-two: the Linux GL stack rejects it, so ioquake3's own
power-of-two rescaling is all that is needed.** The *kernel* would accept it —
see below — but the ICD never gets that far.

* **[HW]** `captures/glsweep-0528-prims-formats.txt` op `f_npot_48x24`
  (`glTexImage2D` 48x24 `GL_RGBA`) changes **nothing** in the staging heap —
  no pixels are staged, no upload record is emitted. The immediately following
  `f_rect_64x16` stages 4096 bytes. So NPOT is refused and **rectangular
  power-of-two is fine** (64x16 works, no vertical padding requirement visible).
* **[BIN]** the refusal is `libGL` `0x14a28d` / `0x14b078`: `glTexImage2D` /
  `glTexImage3D` validate `dim <= 2048 && (dim-1) & dim == 0` per dimension and
  return `GL_INVALID_VALUE` otherwise.
* **[BIN]** the Windows GL ICD is *not* unconditional about it. `MTXOGL.dll`'s
  size check (`0x103cc318`, applied per dimension by `0x103cc360`) rejects
  `size - 2*border > 0x800`, and rejects a non-power-of-two size
  (`eax & (eax-1)`) **only when the runtime cap byte `ctx[0x1a9]` is clear**;
  a second device cap pair (`dev[0x1a0] && dev[0x184]`) can allow it anyway.
  `GL_ARB_texture_non_power_of_two` is appended to the extension string exactly
  when `ctx[0x1a9]` is set (`0x1001c70b`-`0x1001c71e`), and that byte is
  `and`-ed from a device cap at `dev[0x160]` (`0x100315a3`).
  `GL_NV_texture_rectangle` is present too.
* **[BIN]** MTXPARD's `.data TextureCaps = 0x1EC4D` has
  `D3DPTEXTURECAPS_POW2`, `CUBEMAP_POW2`, `VOLUMEMAP_POW2` and `SQUAREONLY`
  all clear, and it writes `0x800` into the D3D caps blocks at `0x86bc4`,
  `0x86e6f`, `0x873bf`.
* **[BIN]** the kernel HAL does not reject NPOT — it *de-tiles* it. The
  popcount loop at `mtx3100` `0x271a0`-`0x271ba` takes the normal tiled path
  only when `popcount(width) == 1 && width > 31`; anything else enables a
  **linear shadow descriptor** at `texObj+0x240` with pitch
  `align(width, 4) * bpp/8` and height rounded to 4 (`0x271ce`-`0x271f6`), which
  the CPU blit paths then use. So an NPOT or sub-32 texture is stored linear.
  Worth noticing: every texture in our register captures is 1, 2 or 8 texels
  wide — **below 32, so all of them fail that test** — while still carrying
  `0x11e0[18:16] = 2`. Either the shadow is only a CPU-side staging descriptor
  and the surface stays tiled, or a sub-32 texture is genuinely stored linear
  under a tiled layout code. **Unresolved**, and it means the captures do not
  demonstrate the >= 32-wide tiled case at all.
* So the silicon most likely does NPOT, but it is gated behind a per-device cap
  in the vendor's own stack and the registry switch `np2Conditional`
  (default 0) withholds `NONPOW2CONDITIONAL`. **Do not expose
  `ARB_texture_non_power_of_two` without a hardware test.** The Linux ICD
  refuses NPOT outright, so there is no vendor reference behaviour to copy.

For ioquake3 this is a non-issue: it rescales every image to a power of two
before `glTexImage2D`, and its largest texture is well under 2048.

---

## 8. The remaining sampler registers  **[BIN]**

`0x11c8`, `0x11cc`, `0x11d0`, `0x11d4` and `0x11d8` are **not** "always 0" and
they are **not** what the earlier documents say. Both vendor binaries write
them from the per-unit shadow struct:

| reg | struct off | identification | evidence |
|---|---|---|---|
| `0x11c8` | +0x10 | **Border colour**, written iff any wrap field >= 6, else 0 | MTXPARD `0x17a691`-`0x17a6db`, kernel `0x72611`/`0x72653`, converting `block[+0x14]` = `D3DTSS_BORDERCOLOR`. **PROVEN** |
| `0x11cc` | +0x14 | **Colour-key value**, packed in the texture's own format; default `0xffffffff` | MTXPARD `0x105381`, packer `0x84060`; kernel `mtx1723` `0x71af1`-`0x71aff` copies `texObj+0x330`, filled by `mtx2611` `.text+0xa960` via the format converter `mtx3283` `.text+0x79710`. **PROVEN dataflow, INFERRED role** |
| `0x11d0` | +0x18 | **Colour-key mask**; default 0. The compare is `(texel & MASK) == KEY` — the disabled state `KEY = 0xffffffff, MASK = 0` never matches under that rule and always matches under the alternative | kernel `0x71b02`-`0x71b10` (`texObj+0x334`). **PROVEN dataflow, polarity INFERRED** |
| `0x11d4` | +0x1c | **Bump-environment matrix** m00 (low 16 bits) / m01 (high) | MTXPARD `0x17a755`-`0x17a814`; kernel `mtx1723` `0x7251e`-`0x725e5`. **Encoding PROVEN, role INFERRED** |
| `0x11d8` | +0x20 | bump matrix m10 / m11 | same |

Four signed 16-bit values, **S4.11**, clamped to +/-16.0 = +/-`0x7ff0`
(`.LC15 = 2047.0`, `.LC13`/`.LC14` = +/-16.0). A second path (kernel `0x72956`)
applies an extra x1.0638 gain and a tighter +/-`0x77e6` clamp. They are sourced
from four consecutive floats at `[stage+0x28]+0x60/0x64/0x68/0x6c` — the shape
of D3D's `BUMPENVMAT00/01/10/11` — and gated on the texture-op enum. Not a
palette, not a chroma key.

This corrects three earlier statements at once:

* `docs/parhelia-fragment-isa.md` §11.1 and
  `docs/parhelia-texture-layout.md` §8.3 call `0x11cc` the **border colour** and
  `0x11d0` its mask. They are the **colour key** and its mask; the border colour
  is `0x11c8`. The tell is `0x11c4[9]`, which the same code clears exactly when
  `0x11cc`/`0x11d0` are loaded — a *colour-key enable*, not a border enable.
  (The elaborate per-format packing that §8.3 of that document recovered is
  still correct; it is the packing of the **key**, and the border colour in
  `0x11c8` goes through a different converter.)
* `docs/parhelia-texture-path.md` and fragment-isa §11.1 call `0x11d4`/`0x11d8`
  "always 0". They are the bump matrix, zero only because no capture uses
  `GL_ARB_texture_env_dot3`-style bump mapping.
* `docs/parhelia-texture-path.md`'s "the bump matrix lives in `0x11e0`/`0x11e4`"
  is wrong.

**For a plain ioquake3 texture all five are 0** except the colour-key default:

```
0x11c8 = 0                       /* border colour; GL default (0,0,0,0)   */
0x11cc = 0xffffffff              /* colour-key value, vendor default      */
0x11d0 = 0                       /* colour-key mask = 0 -> keying off     */
0x11d4 = 0 ; 0x11d8 = 0          /* bump matrix unused                    */
```

which is exactly what every capture carries — including `texclamp`, whose
`GL_CLAMP` (code 6) does pull in the border colour and whose `0x11c8` is 0
because GL's default border colour is transparent black. **[HW]**

Two more, written outside the per-unit block:

| reg | value | role |
|---|---|---|
| `0x11e8` | `0x00500000` for one active stage | MTXPARD: texture-cache partition, table indexed by active-stage count. Kernel HAL: emitted only when the antialiasing gate `ctx+0x2440 & 0x7fc00` is set, reset defaults `0x00100000` (`mtx398` `0x32a98`) or 0 (`mtx1209`), never written by the GL translation path — **role INFERRED, the two stacks read differently**. Not written in any of our captures |
| `0x12f4` | `0x42000000` | guard band 32.0 (`0x41f80000` = 31.0 for more than two stages) |

### 8.1 Inventory of every value actually seen

Across all 74 `captures/gl-matrix` files (12 147 decoded `0x11c0` writes,
12 006 `0x11c4`):

```
0x11c0 : 00000000, 01c01000, 01c01012
0x11c4 : 00000000, f0000000, f0000036, ff000000
0x11c8 : 00000000
0x11cc : ffffffff        0x11d0/0x11d4/0x11d8 : 00000000
0x11e4 : 00010001, 00010002, 00010008
```

A handful of other values (`0x11c4 = 03800015`, `00048e70`;
`0x11c0 = c0afafc0`; `0x11c8 = 1`, `6`; `0x11e4 = 085579bf`, `2121c0af`) occur
only inside 4-, 11-, 36- and 38-dword sections, where the packet decoder
mis-frames a non-register packet. They are **decode artefacts.**

Note `0x11e4[15:14]` and `[31:28]`, and `0x11e0[23:21]` and `[31:30]`, are 0 in
every capture — every capture is single-level, so `0x11e4[31:28]` (the maximum
level, §6.3) reads 0 correctly and the mip path is never exercised on hardware
here.

## 9. What an open driver programs, end to end

One 2D texture on unit 0, everything ioquake3 needs:

```c
/* --- 0x11c0: filter + LOD bias -------------------------------------- */
#define PARH_TF_NEAREST   0u
#define PARH_TF_LINEAR    2u
#define PARH_TF_ANISO     3u
#define PARH_MIP_NONE     0u
#define PARH_MIP_NEAREST  1u
#define PARH_MIP_LINEAR   2u

/* 0x11c0[24:16] = (clamp(bias,-4,4) - 4.0) * 16, signed 9 bit.
 * (The vendor adds an application-profile offset here; it is 0 by default.) */
static uint32_t parhelia_lodbias(float bias)
{
	float v = bias < -4.0f ? -4.0f : (bias > 4.0f ? 4.0f : bias);
	int q = (int)((v - 4.0f) * 16.0f);

	return ((uint32_t)q & 0x1ffu) << 16;
}

static uint32_t parhelia_filter(GLenum min, GLenum mag, float bias)
{
	uint32_t f_mag = (mag == GL_LINEAR) ? PARH_TF_LINEAR : PARH_TF_NEAREST;
	uint32_t f_min, f_mip;

	switch (min) {
	case GL_LINEAR:                 f_min = 2; f_mip = 0; break;
	case GL_NEAREST_MIPMAP_NEAREST: f_min = 0; f_mip = 1; break;
	case GL_LINEAR_MIPMAP_NEAREST:  f_min = 2; f_mip = 1; break;
	case GL_NEAREST_MIPMAP_LINEAR:  f_min = 0; f_mip = 2; break;
	case GL_LINEAR_MIPMAP_LINEAR:   f_min = 2; f_mip = 2; break;
	default:                        f_min = 0; f_mip = 0; break; /* GL_NEAREST */
	}
	/* bit 12 is set unconditionally; bit 10 is the vendor's mag-linear +
	 * min-point + mipmapped quirk flag and is left clear here. */
	return 0x1000u | f_min | (f_mag << 3) | (f_mip << 6) |
	       parhelia_lodbias(bias);
}

/* --- 0x11c4: wrap + LOD clamps -------------------------------------- */
static uint32_t parhelia_wrap1(GLenum w)
{
	switch (w) {
	case GL_CLAMP_TO_EDGE:   return 1u;
	case GL_MIRRORED_REPEAT: return 4u;
	case GL_CLAMP:           return 6u;
	case GL_CLAMP_TO_BORDER: return 7u;
	default:                 return 0u;   /* GL_REPEAT */
	}
}

/* lodMin/lodMax are unsigned 4.4; 0.0 and 15.0 disable the clamps. */
static uint32_t parhelia_wrap(GLenum s, GLenum t, GLenum r,
			      float lod_min, float lod_max)
{
	uint32_t lo = (uint32_t)(lod_min * 16.0f), hi = (uint32_t)(lod_max * 16.0f);

	if (lo > 0xff) lo = 0xff;
	if (hi > 0xff) hi = 0xff;
	/* bit 9 is the colour-key disable.  The vendor GL path leaves it CLEAR
	 * and disables keying with a zero mask in 0x11d0 instead; match that. */
	return parhelia_wrap1(s) | (parhelia_wrap1(t) << 3) |
	       (parhelia_wrap1(r) << 6) | (lo << 16) | (hi << 24);
}
```

`parhelia_filter(GL_NEAREST, GL_NEAREST, 0)` = `0x01c01000` and
`parhelia_filter(GL_LINEAR, GL_LINEAR, 0)` = `0x01c01012`;
`parhelia_wrap(GL_REPEAT, GL_REPEAT, GL_REPEAT, 0, 15)` = `0xf0000000` and
with `GL_CLAMP` on S and T, `0xf0000036` — the four measured values, exactly.

Emission (one DMA register-list packet per unit, in this order — the
`0x1200 + unit*4` write is in the *same* packet and appears to latch the
window; both vendor stacks do it this way):

```
group A:  0x1200 + unit*4,  0x11dc,  0x11e0,  0x11e4
group B:  0x11c0,           0x11c4,  0x11d4,  0x11d8
group C:  0x11e8,           0x11c8,  0x11cc,  0x11d0
```

(§1.1 — this is what `mtx1381` emits and what the capture contains, in that
order. `0x11e8` may be omitted. As DMA register-list indices that is
`0x80+unit, 0x77, 0x78, 0x79` / `0x70, 0x71, 0x75, 0x76` / `0x7a, 0x72, 0x73,
0x74`.)

```
0x1200+u*4 = 0x00040000 | (1 << 8) | pipeOneHot   /* type 4 = 2D, enabled */
             (units 0..3 pipe masks must union to 0xf; a *disabled* unit gets
              0x00010000 | pipeMask and the 0x11c0..0x11e4 block is skipped)
0x11c0 = parhelia_filter(min, mag, lodBias)
0x11c4 = parhelia_wrap(s, t, r, lodMin, lodMax)
0x11c8 = border colour   (0 for GL's default transparent black)
0x11cc = 0xffffffff      /* colour-key value, vendor default */
0x11d0 = 0               /* colour-key mask = 0, keying off  */
0x11d4 = 0 ; 0x11d8 = 0  /* bump matrix, unused              */
0x11dc = vramByteOffset  /* [1:0] = 0 for local VRAM         */
0x11e0 = width | (layout << 16) | (formatCode << 24)   /* layout 0 = linear */
0x11e4 = height | (1 << 16) | (maxLevel << 28)
0x11e8 = 0x00500000      /* cache partition, one active stage */
0x12f4 = 0x42000000      /* guard band 32.0                   */
```

Texture memory (§5.5, §6.2) — **not** a dense chain:

```c
/* layout 0 (linear): Ha = 1, A = 0x40.  bpt = bytes per texel. */
static size_t parhelia_level_size(unsigned w, unsigned h, unsigned d,
				  unsigned bpt)
{
	size_t sz = (size_t)((w + 31) & ~31u) * h * d * bpt;

	return (sz + 0x3f) & ~(size_t)0x3f;
}

/* offset[0] = 0 ; offset[l] = offset[l-1] + parhelia_level_size(level l-1),
 * except that every level with w <= 16 && h <= 16 shares ONE 32x16-texel
 * block and sits at the fixed coordinates of the mip tail (section 6.2). */
```

Every level is padded to a **32-texel pitch** and its size rounded up to `A`,
so a 32x32 `GL_RGBA` chain is **6144** bytes (level 0 plus one shared tail
block), not the 5460 the vendor ICD stages densely. Upload each level row-major
at `stride = align(w_l, 32) * bpt`, in **A8R8G8B8 dwords** (byte order B, G, R,
A) for the 32-bpp formats. Align the surface base to 4096. Program
`0x11dc`/`0x11e0`/`0x11e4` for the **base level** only, set
`0x11e4[31:28] = levels - 1`, and let the hardware walk the rest — see §6.4, and
run §9.1 check 2.

If you use layout 2 instead, add the §5.1 swizzle to the upload and the §5.3
`Ha = 32` / `A = 0x1000` padding to the sizer.

### 9.1 Hardware checks to run before trusting a textured frame

These are cheap, and each one is a genuine unknown rather than a formality:

1. **Layout.** Sample a linear, dense 64x64 texture 1:1 onto a screen-aligned
   quad and read the result back. Confirms §5.
2. **Mip chain.** Upload a chain whose levels are flat, distinct colours, draw
   at a minification that must select level 2, and check the colour. Confirms
   whether the hardware walks a dense chain from one base, and whether a level
   register is needed (§6).
3. **MIN vs MAG bit position.** Set `GL_NEAREST` min with `GL_LINEAR` mag and
   capture `0x11c0`: `0x02` means MAG is the low field, `0x10` means MIN is.
   This is the only remaining ambiguity in the filter word and one capture
   settles it.
4. **Wrap codes 1 and 4.** Draw with texture coordinates running 0..2 under
   `GL_CLAMP_TO_EDGE` (expect code 1) and `GL_MIRRORED_REPEAT` (expect 4).

**These are already wired up.** `tools/gltest.c` now takes `GLTEST_TEXMIN`
(0..5 = the six minification filters, uploading the whole chain when a mipmap
mode is selected), `GLTEST_TEXMAG` (0..1) and `GLTEST_TEXWRAP` (1..4 = clamp,
clamp-to-edge, mirrored-repeat, clamp-to-border), and
`tools/capture-gl-matrix.sh` has six new variants appended after `big2` — so
every existing variant keeps its window width and the `0x132c` attribution of
the existing captures stays valid:

| variant | what it settles |
|---|---|
| `texasym` | min nearest + mag linear -> check 3. `0x11c0 & 0x1f == 0x10` means MIN is `[1:0]` |
| `texedge` | `GL_CLAMP_TO_EDGE` -> expect wrap code **1** |
| `texmirror` | `GL_MIRRORED_REPEAT` -> expect **4** |
| `texborder` | `GL_CLAMP_TO_BORDER` -> expect **7** (or 6 if downgraded), and a non-zero `0x11c8` |
| `texmipnear` | 32x32 chain, `GL_LINEAR_MIPMAP_NEAREST` -> `0x11c0` MIP field 1, `0x11e4[31:28]` = 5 |
| `texmiplin` | 32x32 chain, `GL_LINEAR_MIPMAP_LINEAR` -> MIP field 2 |

Run `tools/capture-gl-matrix.sh <host>` and decode with
`tools/decode-gl-secs.py --regs 132c,11c0,11c4,11c8,11dc,11e0,11e4`, attributing
each section by its `0x132c` width (§0). The two mipmap variants also settle
§6.4 and §11 items 2 and 3 by showing what the vendor programs for a real
mipmapped texture — something no existing capture contains.

---

## 10. Corrections to existing documents

| document | claim | correction |
|---|---|---|
| `parhelia-fragment-isa.md` §11.3/§11.4 | `0x11c0` = wrap, `0x11c4` = filter | **swapped** — §1 |
| `parhelia-fragment-isa.md` §9.5.6 | the filter hint tables drive `0x11c4` | they drive **`0x11c0`**, and they are `(MIN, MAG, MIP)` |
| `parhelia-fragment-isa.md` §11.3 | wrap is 2 bits per axis; "which of 1 and 2 is clamp and which is mirror is not determined"; write mask `0xffffff24` | wrap is **3 bits** per axis with codes 0/1/4/5/6/7 — §4.1. The `0xffffff24` mask belongs to the **filter** register, where it leaves the unused bits 2 and 5 alone |
| `parhelia-fragment-isa.md` §11.1 | the emitter is `mtx792` @0x69790, one 12-register window, struct offsets `+0x00`..`+0x2c` | `mtx792` is **dead code**. The live emitter is `mtx1381` `.text+0x3f680`, a 0x38-byte record emitted in three dirty-groups — §1.1, confirmed by the capture's emission order |
| `parhelia-fragment-isa.md` §11.1, `parhelia-texture-layout.md` §8.3 | `0x11cc` = border colour, `0x11d0` = its mask | those are the **colour key** and its mask; the **border colour is `0x11c8`**, written only when a wrap code is >= 6 — §8 |
| `parhelia-fragment-isa.md` §11.1, `parhelia-texture-path.md` | `0x11d4`/`0x11d8` "always 0" | **bump-environment matrix**, S4.11 halves — §8 |
| `parhelia-fragment-isa.md` §11.5 | `0x11e4[16:14]` extra field, bit16 forced set | `0x11e4[27:16]` = depth/slices (minimum 1) and `[31:28]` = maximum level — §2, §6.3 |
| `parhelia-fragment-isa.md` §11.9 | "mip selection is done on the CPU; the driver reprograms the base per draw" | that is the Windows path. The Linux stack has a LOD bias, LOD min/max clamps, a mip-linear filter code and a level-count field, none meaningful under CPU selection — §6.4 |
| `parhelia-texture-path.md` | "the bump matrix lives in `0x11e0`/`0x11e4`" | it lives in `0x11d4`/`0x11d8` |
| `parhelia-texture-path.md` | `0x11c0` = wrap + LOD bias, `0x11c4` = filter + LOD clamps | register roles swapped. Its *field structures* are right and attached to the right addresses — only the names are exchanged, and its LOD bias / clamp reading is **confirmed** |
| `parhelia-texture-path.md` | the 9-entry array at `texObj+0xd8` is a mip-level array, "only one level is programmed at a time" | it is a **per-format-variant** array — each slot carries its own format code and they all share one base address — §6.4 |
| `parhelia-texture-layout.md` §1 / "Independent verification" | "texture memory is LINEAR, **CONFIRMED on hardware**" | it measures the **CPU staging heap**, not VRAM. The kernel applies a 32x16-block swizzle on the way in — §5.1, §5.4 |
| `parhelia-texture-layout.md` §1 | "a from-scratch driver must use tiling code 0" — because the intra-tile order was not recovered | the intra-tile order **is** recovered now (§5.1). Code 0 is still the right first choice, but for simplicity, not for ignorance |
| `parhelia-texture-layout.md` §3 | `0x11e0[13:0]` is "the width already rounded up to a multiple of 32" | it is the **raw width** in the Linux GL path; the hardware rounds to 32 itself — §2 |
| `parhelia-texture-layout.md` §5 | "no per-level padding beyond the 32-texel pitch rounding" | every level's size is rounded up to `A` (`0x40`..`0x2000`) and its height to `Ha`, and there is a hard-coded shared **mip tail** below 16x16 — §6.2 |
| `parhelia-texture-layout.md` §8.2 | "LOD bias and LOD clamp **CONFIRMED absent**" | both exist: bias in `0x11c0[24:16]`, clamps in `0x11c4[23:16]`/`[31:24]` — §3.3, §4.3 |
| `parhelia-3d-coverage.md` | "Sampler registers SOLVED ... field layouts in `parhelia-texture-path.md`" | point it at this document instead |
| `tools/annotate-stream.py` (idx `0x70`/`0x71` entries) | labels `0x11c0` `TEX_ADDRMODE` and `0x11c4` `TEX_FILTER`, `0x11cc`/`0x11d0` border colour/mask | swapped, and the border/key naming is wrong — see the patch applied alongside this document |
| `tools/gen-tri-stream.py` | emits `0x11c4 = 0x249` as part of a "raster/blend/depth state" block | that value is clamp-to-edge on all three axes plus a colour-key bit; `0` is `GL_REPEAT` |

---

## 11. Open items

1. **MIN vs MAG bit position** is proven from the binaries (§3.1) but has never
   been seen asymmetric in a capture. §9.1 check 3 is one draw and would make it
   [HW] too.
2. **Layout code 0 in the texture unit.** No capture shows it; the vendor GL
   always uses 2 (32 bpp) or 4 (8 bpp), and the code that chooses is not
   located. §9.1 check 1.
3. **The mip tail's offset sharing** — whether the sub-16x16 levels occupy one
   block or one block each (§6.2).
4. **Tiling code 3 versus 2** — no software function distinguishes them; the
   difference is hardware-only.
5. **`texObj+0x80`**, the pool enum that gates the swizzle path, is written
   outside this object, so "pool 2 <-> tiling code 2/3" stays inferred.
6. **Sub-32-wide textures.** `mtx3100` `0x271a0` sends any width that is not a
   power of two *greater than 31* down a linear shadow path, yet the captured
   1/2/8-wide textures still carry layout code 2 (§7). Which of the two the
   sampler actually uses is unresolved, and it means the captures never
   exercise the >= 32-wide tiled case.
7. **`0x11c0` bits 2, 5, 8** — preserved by every writer in both binaries, never
   set. No producer found.
8. **`0x11c0` bit 12** — set unconditionally at kernel stage init
   (`0x718c2` zeroes the dword, `0x718cd` clears bit 11, `0x718d5`
   `or BYTE PTR [rax+0xd],0x10` sets bit 12; verified directly for this
   document). Its *meaning* is unknown. Program it.
9. **`0x11c4[10:9]`** — the colour-key control pair; which bit is the enable is
   read two ways (§8). Keying is off when `0x11d0` is 0 regardless.
10. **Anisotropy degree** — filter code 3 selects anisotropic filtering, but no
    consumer turns `D3DTSS_MAXANISOTROPY` into a hardware field. Maximum is 2.
11. **`0x11e0[15:14]`, `[23]`, `[30]`, `[31]`; `0x11e4[15:14]`** — sources known
    (surface flag bytes), meanings not. `0x11e0[21]` is never written.
12. **The cube-map level-count off-by-one** at `mtx3887` `0x279cb`
    (`numLevels`, not `numLevels - 1`). Irrelevant to ioquake3.
13. **Non-power-of-two** — the caps say the silicon does it, both ICDs gate it,
    nothing is tested. §7.

---

Copyright (C) 2026 René Rebe <rene@exactco.de>

## P690 measured sampling limits (fixfn, 2026-10-03)

Probes: `tools/p690-gl-probes/*.c`, built on the target with
`gcc -o p p.c -I/usr/X11/include -L/usr/X11/lib64 -lglut -lGL -lm` and run
against the Mesa parhelia driver. All results MEASURED on the P690 (0x2539).

* **Raw LOD overflows above 12** (`lodprobe.c [maxlod maxlevel base minlod]`).
  A minification of 2^13 texels per pixel or more selects level 0 at every
  bias, including bias 0. Base sizes 4, 64 and 1024 behave alike, and neither
  `GL_TEXTURE_MAX_LOD` nor `GL_TEXTURE_MAX_LEVEL` changes it. The overflow
  happens before the bias is added. For raw LOD up to 12 every bias in
  -12..0 selects exactly clamp(L + b). The `GL_TEXTURE_MIN_LOD` clamp is
  applied after the overflow: MIN_LOD 2 turns the overflowed L >= 13 into
  level 2. This is a hardware limit, and it is why piglit `lodbias` fails:
  its bias -12 cells for levels 1 and 2 need raw LOD 13 and 14. The bias
  field and the advertised +/-12 are exact.
* **Texture coordinate offsets** (`coordprobe.c`): a 1:1 64x64 quad is exact
  for s,t offsets up to +/-65536. 131072 fails, which is float32 precision of
  the coordinate itself.
* **Bilinear sample position precision** (`bilin.c N`): at 1:1 every
  power-of-two size is exact and every point-sampled size is exact. NPOT sizes
  leak a neighbour in the low bits: 96 -> 1 LSB, 160 -> 2, 320 -> 3, 1000 -> 9.
  The error doubles when s crosses 0.5, so the interpolated coordinate behaves
  like a float with a ~16-bit mantissa (about 2^-16 relative). Pixel-centre
  coordinates of a power-of-two texture are exactly representable; NPOT ones
  are not. No register in the known texture map (0x11c0..0x11e4) carries a
  texel step or scale; the hardware derives it from 0x11e0's width. This is
  why piglit `1-1-linear-texture` (160x160, exact compare) fails.
* **Two-unit sampled alpha** (`alphaprobe.c mode`): the P690 two-unit route
  (0x1200 = 0x40101, 0x1204 = 0x40104) is exact in RGBA only in the vendor
  form, with no pipe merges and the ALU reading t0 and t2. The former merges
  `ADD t0, t0, t1` / `ADD t1, t2, t1` read the parked pipe 1, which returns
  alpha 1. Every two-unit draw then had alpha 1.0 (Mesa 097970c fixes it).
* **Dither on 8-bit targets** (`round.c fbo dither`): with GL_DITHER on (the
  vendor default, 0x12e8 bits [24:23] = 01 for 32-bit targets), a 32-bit
  target converts colours with a position-dependent threshold. At one pixel
  0.3..0.7 LSB all stored as 0 in an FBO and as 1 in the window. With dither
  off, colours round to nearest. piglit `gl-1.1-xor-copypixels -fbo` probes a
  pixel half a pixel off a quad diagonal, whose green channel is 0.8 LSB. The
  dither stores it as 0, so the test reports "glCopyPixels appeared to fail"
  although the copy itself is exact (`copypix.c`).
