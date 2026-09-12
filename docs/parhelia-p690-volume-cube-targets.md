# P690 sampler targets: 1-D, volume and cube maps

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

The Gallium driver accepted `PIPE_TEXTURE_2D` and `PIPE_TEXTURE_RECT` only.
This document records what the hardware does for the remaining sampler
targets, which of them the driver now advertises, and why the rest stay off.
Every claim is tagged **PROVEN** (read out of a vendor binary or measured on
the P690) or **INFERRED**.

Measurement tools: `parhelia-drm-test --texture-volume-probe DEV [W H D]` for
the volume and cube descriptors and their texel order,
`--mip-address-probe DEV W 1` for the 1-D mip chain, and
`xf86-video-parhelia/tools/parhelia-gl-texture-target-test.c` at the GL level.

## 1. The descriptor fields

| field | meaning | evidence |
|---|---|---|
| `0x11e0[20:19]` | sampler target: 0 2-D, 1 cube, 2 volume | **PROVEN**, see below |
| `0x11e4[27:16]` | 12-bit slice count, minimum 1 | **PROVEN** |
| `0x11e4[31:28]` | maximum mip level | PROVEN (already in-tree) |
| `0x1200 + 4u [18:16]` | coordinate mode: 4 two-coordinate, 6 three-coordinate | **PROVEN** |
| `0x11c4[8:6]` | R/W axis wrap, three bits like S and T | **PROVEN** |

**The target field — PROVEN.**  The Linux HAL's descriptor writer `mtx3424`
(`mtx_parhelia.o` `.text+0x1eab0`) computes `(TYPE & 3) << 3` at
`.text+0x1eb28`-`0x1eb35` and ORs it at `0x1ecd2` into the descriptor byte that
is `0x11e0[23:16]`; the mask at `0x1ec61` (`and $0xffffffa7`) clears bits 3, 4
and 6 of that byte first, leaving the storage-order field `[18:16]` untouched.
The surface-class factory `mtx3789` (`.text+0x29d30`) has exactly three texture
classes and each passes its own TYPE:

| request flag | builder | TYPE | `0x11e0[20:19]` |
|---|---|---:|---:|
| `0x08` | `mtx1747` `.text+0x28e80` (2-D) | 0 | 0 |
| `0x10` | `mtx3887` `.text+0x27610` (cube) | 1 | 1 |
| `0x20` | `mtx325` `.text+0x28190` (volume) | 2 | 2 |

`mtx325` additionally sets bit 4 of the level template's `+0x06` byte directly
(`0x2837b` `and $0xe7`, `0x2837e` `or $0x10`), an independent second write of
`0x11e0` bit 20 for a volume.  TYPE 3 exists in `mtx3424` (`0x1ece0`) but no
call site emits it.

**The slice count — PROVEN.**  `mtx3424` `0x1eafb` masks the argument to 12
bits and `0x1eca9`-`0x1ecc6` merges it into `0x11e4` bits `[27:16]`, preserving
the top nibble (the maximum level, written separately at `0x1eb54`).  It is a
raw count, not `depth-1` and not a logarithm: the 2-D call sites pass the
literal 1 (`mtx1747` `0x290f5`, `mtx3100` `0x270e2`), which matches the
captured `0x11e4 = 0x00010002` of a 2x2 2-D texture, and the allocator
`mtx1444` (`.text+0x5b900`) extracts `(0x11e4 >> 16) & 0xfff` at `0x5b992` and
**multiplies** the level size by it.

**The coordinate mode.**  `0x1200 + 4u [18:16]` selects how many coordinates
reach the texture-address unit: 4 for the two-coordinate targets, 6 for the
three-coordinate ones (`docs/parhelia-texture-path.md`).  Measured here: with
mode 4 a volume descriptor ignores the third coordinate entirely, and with
mode 6 it does not (section 3).

**The W wrap axis — PROVEN.**  Kernel `mtx1723` `0x72744`-`0x72776` extracts
`(state[0x11] >> 3) & 7`, clamps it to 6, looks it up in the same 6-entry code
table S and T use, and writes it to `0x11c4[8:6]`, unconditionally for every
texture regardless of target.  D3D `VolumeTextureAddressCaps = 0x3F` (all six
address modes) is a literal at `mtxpserd.dll` `0x881fa`.

Note the register naming used by the driver is the measured one:
**`0x11c0` is the filter word** (MIN `[1:0]`, MAG `[4:3]`, MIP `[7:6]`, LOD
bias `[24:16]`) and **`0x11c4` is the wrap word** (S `[2:0]`, T `[5:3]`, R
`[8:6]`, LOD clamps `[23:16]`/`[31:24]`).  `docs/parhelia-texture-path.md` has
these two registers swapped.

## 2. The third texture coordinate costs the vertex colour

The address unit needs three interpolated coordinates, so the vertex must
carry `(s,t,r)`.  The attribute port declares kinds and dword offsets freely,
but the stream width is fixed by the batch builder's contract: a textured draw
is 7 dwords per vertex, 8 when the position carries z (a bound depth buffer or
a hardware-transformed draw).  Position (2 or 3) plus colour (3) plus three
coordinates (3) is 8 or 9 — one dword too many in both cases.

The driver therefore emits, for a volume or cube-map draw,

```
x y [z] s t r  (+ two unused dwords)
```

with descriptors `0x20c00000`/`0xa1c70020` (no z) or
`0x21c00000`/`0xa1c70030` (with z) — position kind 0 and a **three-component**
coordinate set 0 at kind 7, mask 7.  `0x1110` then reads `0x101`, exactly the
vendor's own `texrepl` value.  The primary colour comes from the WARP replace
program's constant white, so:

* `GL_REPLACE` and any fragment program that does not read the vertex colour
  are exact;
* `GL_MODULATE` by a white primitive is exact, because modulating by white is
  the texture itself — this is the GL default state and what fixed-function
  volume and cube-map scenes use;
* a **Gouraud-coloured** volume or cube-map primitive cannot be expressed and
  the draw is dropped with a one-time warning.

Lifting the last restriction needs one more dword per vertex.  Two routes
exist and neither was taken here: widen the builder's `dw_per_vertex`
contract in `parhelia_batch.c` (one term, `+ !!input->texcoord3`), or find the
attribute descriptor's component-type field and fetch the colour as one packed
`D3DCOLOR` dword — bits `[28:26]` and `[21:20]` of the descriptor are still
undecoded.

## 3. The volume texel order — MEASURED

`--texture-volume-probe` fills a texture with self-indexing dwords, samples it
one slice per quad with `r = (z + 0.5) / D`, and reports which dword the unit
fetched for every texel.  The address is XOR-linear in the coordinate bits, so
the probe solves for one dword pattern per coordinate bit and then checks the
solution against every texel.

Control run, 64x32x2, `0x11e0[20:19] = 0`, coordinate mode 4, slice count 1:

```
type 0 coord 4 slice 0: (0,0) dword 0, 0/2048 differ from slice 0's order
type 0 coord 4 slice 1: (0,0) dword 0, delta 0
  x 1,2,4,8,16,32 -> 0x1 0x2 0x4 0x10 0x20 0x80
  y 1,2,4,8,16    -> 0x8 0x40 0x100 0x200 0x480
```

That is the plain P690 64x16 texture order, and the third coordinate changes
nothing: a two-coordinate descriptor ignores `r`.  It also proves the
three-component vertex descriptor works, since the image is sampled exactly.

Volume run, same image, `0x11e0[20:19] = 2`, coordinate mode 6, slice count 2:

```
type 2 coord 6 slice 0: (0,0) dword 0
type 2 coord 6 slice 1: (0,0) dword 0x80, delta 0x80 (0x200 bytes)
  x 1,2,4,8,16,32 -> 0x1 0x2 0x4 0x10 0x20 0x480
  y 1,2,4,8,16    -> 0x8 0x40 0x100 0x200 0xc80
  z 1             -> 0x80
```

Reading the bit positions off that table, the dword address is

```
b0..b2 = x0,x1,x2      b3 = y0      b4,b5 = x3,x4      b6 = y1
b7     = z0 ^ g0
b8,b9  = y2,y3
b10..  = g              where g = t ^ (t >> 1),
                              t = (y >> 4) * (pitch >> 5) + (x >> 5)
```

so a volume is laid out as

* a **512-byte granule of 32x4 texels** — the same seven low bits the 2-D
  order uses;
* the slices of one granule consecutively, `z` occupying `log2(D)` bits
  immediately above it;
* the four granules of a 32x16 sub-tile above that;
* the sub-tile index Gray-coded, which is the same `x5 ^ trow` half-swap the
  plain 64x16 texture tile applies, generalised.

A volume is therefore **not** a stack of whole 2-D images, and it is not the
vendor's generic `(blockIndex * depth + z) * blockBytes` either (that formula,
`mtx2146` `.text+0x213df`, interleaves whole 32x16 blocks; the P690 interleaves
the 512-byte granule inside them).  One slice still costs
`pitch * align(height,16) * 4` bytes, so the object is
`pitch * align(height,16) * 4 * D`.

`ph_volume_texel_offset()` in `parhelia_mipmap.h` implements this; the
contribution table above is a host selftest (`--selftest`, "measured P690
volume texel order").

## 4. Cube maps

Vendor evidence, all **PROVEN**:

* one base register and a fixed six-image stride.  `mtx3887` `0x27cb3`-`0x27cd4`
  computes `faceAddr = offset[level] + faceIndex * size[level]` and the level
  recurrence at `0x27ed4`-`0x27eef` is `offset[i] = offset[i-1] + 6*size[i-1]`,
  so the storage is level-major and face-minor.  There is no per-face register
  and no six-face constant in the register programming.
* face order is `target - GL_TEXTURE_CUBE_MAP_POSITIVE_X`, i.e.
  +X, -X, +Y, -Y, +Z, -Z (libGL `0x14a65f`), identical to
  `D3DCUBEMAP_FACE_POSITIVE_X..NEGATIVE_Z`.
* faces are square: `mtx3887` `0x27870`-`0x2789c` writes the same
  `max(W>>level,1)` into the level template's width and height.
* the descriptor's slice field carries the **face edge length**, not six:
  `mtx3887` `0x279bd`-`0x279c7` sets width = height = depth = N for the cube
  parent and `0x27c73` sets depth = `w_i` for each per-level view.

`D3DPTEXTURECAPS` is `0x0001EC4D`, byte-identical in `mtxpserd.dll`
(`0x1d3ec4`) and `MTXPARD.dll` (`0x1d1ec4`): `CUBEMAP` (bit 11), `VOLUMEMAP`
(13), `MIPCUBEMAP` (16) and `MIPVOLUMEMAP` (15) set, `POW2`, `CUBEMAP_POW2`,
`VOLUMEMAP_POW2` and `SQUAREONLY` clear.  `CubeTextureFilterCaps` and
`VolumeTextureFilterCaps` are assigned from the same word as
`TextureFilterCaps` (`0x881bd`/`0x881c3`/`0x881c9`), so cube and volume get the
full point/linear/anisotropic filter set.

Hardware result: see section 7.

## 5. 1-D textures

There is **no hardware 1-D target** — PROVEN.  `mtx3789` has three texture
classes and `0x11e0[20:19]` only ever receives 0, 1 or 2.  The vendor ICD keeps
a separate 1-D *binding point* (per-unit block `+0x08`, libGL `0x110039`) whose
resolution order is CUBE > 3-D > 2-D > 1-D (`0x10f49d`-`0x10f4ac`), but all
four converge on the same sampler emit at `0x10f821`, and the 1-D texture-object
constructors preset height = depth = 1 (`0x1a0370`, `0x1a03d0`).

`GL_TEXTURE_1D` is therefore a height-1 2-D surface.  The driver creates it
exactly like a 2-D texture and lets the address unit derive the chain, which
`--mip-address-probe W 1` measures level by level.

## 6. Array textures are not supported

**PROVEN**, and this is a silicon limit, not a driver one:

* `GL_EXT_texture_array` / `GL_ARB_texture_array` do not appear in the vendor
  libGL's extension strings, and `GL_TEXTURE_1D_ARRAY` (0x8C18) and
  `GL_TEXTURE_2D_ARRAY` (0x8C1A) appear zero times as immediates in libGL or
  MTXOGL;
* the ICD has four target slots per unit and the kernel three surface classes;
* D3D `TextureCaps = 0x1EC4D` has no array bit, and DX8/DX9 had none.

The 12-bit slice field could hold N layers of a volume, but the R axis has no
"do not filter" control anywhere in `0x11c0`/`0x11c4`, so a layered fetch would
blend neighbouring layers — that is not an array texture.  `PIPE_TEXTURE_1D_ARRAY`,
`PIPE_TEXTURE_2D_ARRAY` and `PIPE_TEXTURE_CUBE_ARRAY` stay unadvertised and
`max_texture_array_layers` stays zero.

## 7. Hardware results

`--texture-volume-probe /dev/dri/renderD128 64 32 2`, P690, module `1e8f7fb`:

| run | result |
|---|---|
| `PROBE_TYPE=0 PROBE_COORD=4 PROBE_DEPTH_FIELD=1` | 0/2048 texels differ from the plain 2-D order in either slice, and both slices fetch the identical addresses: a two-coordinate descriptor ignores `r`. Also proves the three-component vertex descriptor, since the image samples exactly. |
| `PROBE_TYPE=2 PROBE_COORD=6` | the slices separate (slice 1's origin is 0x200 bytes above slice 0's) and the thirteen coordinate-bit contributions are those in section 3, which `ph_volume_texel_offset()` reproduces exactly. |

The contribution table is kept as a host selftest so the order cannot drift:
`parhelia-drm-test --selftest`, "measured P690 volume texel order and the
three-coordinate volume descriptor".

## 8. What the driver advertises

| target | state |
|---|---|
| `PIPE_TEXTURE_1D` | on; level-0 height-1 2-D storage in the measured order |
| `PIPE_TEXTURE_2D`, `PIPE_TEXTURE_RECT` | unchanged |
| `PIPE_TEXTURE_3D` | `PARHELIA_VOLUME_TARGETS=1`; sampler-only, level 0, width at least 64 and a power-of-two depth up to 64 |
| `PIPE_TEXTURE_CUBE` | `PARHELIA_CUBE_TARGETS=1` as well; sampler-only, level 0, square faces |
| 1-D/2-D/cube arrays | off; no hardware target (section 6) |

The two opt-ins record exactly how far the hardware evidence reaches.  The
volume descriptor and texel order are measured, but for one image geometry
only and never yet end to end through the GL path; the cube-map face stride is
PROVEN from the vendor HAL and has not been measured on this silicon at all.
Both gates should fall as soon as `--texture-volume-probe` has run over the
remaining geometries and `tools/parhelia-gl-texture-target-test` is green.

## 9. Open

* Mip chains for volumes and cube maps.  The vendor's own geometry is known
  (`d_i = max(D>>i,1)`, `offset[i] = offset[i-1] + size[i-1]` for a volume and
  `+ 6*size[i-1]` for a cube map, both collapsing every level at or below
  16x16 into one shared 32x16 block), but the P690's own chain differs from
  the vendor's for 2-D as well, so it has to be measured before it is used.
  Until then a volume or cube map carries level 0 and samples it.
* `0x11e4[31:28]` for a cube parent: the vendor writes `numLevels` there
  (`mtx3887` `0x279ad`) where every other path writes `numLevels - 1`
  (`mtx325` `0x2867f`).  Irrelevant while only level 0 exists.
* Whether `0x11e0[20:19]` is a 2-bit enum or two independent flags.  Only
  TYPE 3 distinguishes them and nothing emits it.
* A Gouraud-coloured volume or cube-map primitive; see section 2.
