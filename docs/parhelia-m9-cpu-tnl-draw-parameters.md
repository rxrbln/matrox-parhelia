# M9148 CPU T&L draw parameters: vertex layout, depth, render target size

This note parametrizes the verified two-pass first-triangle stream for a CPU
transform-and-lighting Mesa driver.  It covers the preprocessing source
declaration (per-vertex z, w and RGBA), hardware depth test and write, and
arbitrary render target sizes.  Labels: **MEASURED** is an M9148 (102b:0540)
hardware result, **CAPTURED** is a saved vendor capture, and **STATIC** is a
binary-derived document.

Everything below was run through the userspace SUBMIT path with the unchanged
`m9x_drm.ko` (no kernel changes were needed):

```sh
gcc -O2 -Wall -I/root/parhelia-cleanroom/include -o m9-user-draw-params \
    tools/m9-user-draw-params.c -lm
./m9-user-draw-params [-s WxH] [-V WxH] [-t tri|quad|depth|zview] [-p 2|3|4]
    [-q 3|4] [-a a8] [-c cmp] [-e ef] [-z zA] [-B bb] [-Z depth-clear]
    [-H hz-init] [-g]
python3 tools/m9-draw-params-readback.py <color-gpu-address> W H --quad
```

The tool patches the replay stream from `m9-user-triangle-data.h` by walking
packets like the kernel parser (range breaks separate the preprocessing and
raster passes), so every relocation site stays identical.  Color, depth and HZ
BOs are followed by a page of sentinel words; overruns are reported.

## 1. Source declaration: index 10 and index 23

The preprocessing pass binds the CPU vertex array at `00` (SYSTEM relocation),
the canonical record span at `10`, and eight index-`23` values (two
four-value packets).  The raster pass reads the generated views and keeps its
captured index-`23` list.

### Word encoding

| bits | field | evidence |
|---:|---|---|
| `31:24` | source offset of component 0, in dwords | CAPTURED (color `02`/`03` after float2/float3 position, texcoord `05`), MEASURED (`04` after float4 position) |
| `23:20` | attribute: 1 position, 2 color, 3 texcoord (preprocessing) | CAPTURED |
| `19:16` | input stream `00..03` (raster list uses 0..3) | CAPTURED, stream 0 MEASURED |
| `15:12` | element format: `0` float, `c` ubyte4 (normalized) | CAPTURED, `0` MEASURED |
| `11:0` | four 3-bit selectors for record slots 0..3 | CAPTURED + MEASURED |

A selector value `0..3` takes that source component, `4` supplies 0.0, `5`
supplies 1.0, and `6` leaves the slot to the other element of the pair:

```c
word = dword << 24 | attr << 20 | fmt << 12 | s0 | s1 << 3 | s2 << 6 | s3 << 9;
```

Each attribute uses two words.  For position, slots `0,1` are z,w and slots
`2,3` are x,y.  For color, slots `0..3` are r,g,b,a.  The list ends with
invariant `00004b08 0 0 0`.  Index `10` is `4 * (position comps + color
comps)`.

| layout | `10` | index `23` values | status |
|---|---:|---|---|
| float2 pos, float3 color | `14` | `00100dac 00100236 02200d88 02200ab6` | CAPTURED, MEASURED 28000 green |
| float3 pos, float3 color | `18` | `00100daa 00100236 03200d88 03200ab6` | CAPTURED, MEASURED |
| float4 pos, float3 color | `1c` | `00100d9a 00100236 04200d88 04200ab6` | MEASURED (derived word) |
| float4 pos, float4 color | `20` | `00100d9a 00100236 04200d88 042006b6` | MEASURED (derived words) |
| float3 pos, float4 color | `1c` | `00100daa 00100236 03200d88 032006b6` | MEASURED |
| float2 pos, ubyte4 color | `0c` | `00100dac 00100236 0220cd88 0220c6b6` | CAPTURED only |
| + float2 texcoord at dword 5 | `1c` | adds `05300dac 05300236` | CAPTURED only (needs the five-stream textured raster template) |

MEASURED view contents (`-t zview`, transient BO views at `k * 0x16800`; view1
is window position, view3 is clip position, view0 is RGBA), for object
vertices in `glOrtho(0,320,0,240,-1,1)`:

```text
-p 3:     view1 {20,220,0.125,1} {300,220,0.5,1} {160,20,0.875,1}
          view3 {-0.875,-0.8333,-0.75,1} ... {0,0.8333,0.75,1}
-p 4 -q 4 (vertex 1 w=2, xyz premultiplied):
          view0 {0.25,0.5,0.75,0.375} {1,0,0,0.625} {0,1,0,0.875}
          view1 {20,220,0.125,1} {300,220,0.5,0.5} {160,20,0.875,1}
          view3 ... {1.75,-1.6667,0,2} ...
```

Thus per-vertex z reaches the window-space view (`z_win = 0.5 - 0.5 *
z_obj / w` with the ortho constants), view1 w holds `1/w_clip`, and x/y are
divided by w.  With float4 color the flat-shaded pixels carry alpha
`df/e0` (0.875, the last vertex): the provoking vertex is the last one
(MEASURED; `-p 3` with three different vertex colors yields only the third
vertex color, 28000 pixels).

A CPU T&L driver can therefore submit window-space data directly by setting
the model-view-projection terms to identity-equivalent values, or keep the
ortho constants below and submit object coordinates `x, y, z_obj = 1 - 2 *
z_win` (the tool does the latter).

## 2. Depth test and write

The raster pass needs both `a8` and `ef`.  `a8` alone does not enable
anything (MEASURED: `0038038a`, `0038038b`, `00380389` with `ef=0` give
identical colors and an untouched depth BO).

| state | `a8` | `ef` | evidence |
|---|---:|---:|---|
| depth off (first triangle) | `0038038a` | `00000000` | CAPTURED, MEASURED |
| LESS, write | `0038038b` | `0000f000` | CAPTURED (fixed-state and glxgears `ef=0300f000` with back-face culling), MEASURED |
| LESS, no write | `00380389` | `00007000` | CAPTURED pattern (LEQUAL/ALWAYS no-write), MEASURED |

`a8` bit 0 is depth test, bit 1 depth write, bits `4:2` the compare code in GL
order: NEVER 0, EQUAL 1, LESS 2, LEQUAL 3, GREATER 4, GEQUAL 5, NOTEQUAL 6,
ALWAYS 7 (CAPTURED complete table in `parhelia-m9-fixed-state-capture.md`;
the D3D-order table in `parhelia-m9-3d-static-re.md` is the Windows remap
input, not the hardware code).  `a9=08ffff00`, `aa=00ffff00` stay at the
captured stencil defaults.

Depth scene (`-t depth`, 320x240): red quad A `[40,200)x[40,160)` at constant
window z `zA`, drawn first; green quad B `[120,280)x[80,200)` with z ramping
0→1 over x.  Each quad is 160x120 = 19200 pixels; the overlap is 80x80.

| run | expected | MEASURED |
|---|---|---|
| `ef=0` (any `a8`) | green 19200 red 12800, depth untouched | exact |
| LESS write, zA .25 | crossing at x=160: green 16000 red 16000 | exact; depth `3fffffff*16000` + 120-row ramp columns |
| LESS no write | green 19200 red 12800, depth untouched | exact |
| NEVER / EQUAL | nothing | 0 color, 0 depth |
| LEQUAL | as LESS | exact |
| ALWAYS | green 19200 red 12800, depth written | exact, `3fffffff*12800` |
| LESS, zA .75 | green 19200 red 12800 | exact, `bfffffff*12800` |
| GREATER, clear `000000ff`, zA .75 | red 19200 green 12800 | exact, `bfffffff*19200` |
| LESS write at 300x300 and 1024x768 | as 320x240 | exact |

The D24S8 element (`b9` format `fa>>1 = 0x7d`) stores unsigned depth in bits
`31:8`: 0.25 → `3fffff`, 0.5 → `7fffff`, 0.75 → `bfffff`; the ramp column
at x=120 stores `00cccd` (= 0.5/160).  The low byte keeps its previous content
(the `ff` fill), consistent with stencil (MEASURED).  Clear the depth BO by
filling `depth << 8 | stencil`.

### Hierarchical-Z metadata (`ba/bb`)

With the aux BO initialized to `0x007ff000`, no scene lost a visible fragment
(all counts above are exact, including GREATER against clear 0).  The same
GREATER result was measured with the aux BO initialized to 0.  The hardware
writes exactly one word per 8x4 pixel cell touched:

| surface | depth-tested full quad writes | expected `ceil(W/8)*ceil(H/4)` | highest written element | allocation words |
|---|---:|---:|---:|---:|
| 320x240 | 2400 | 2400 | 2555 | 2560 |
| 300x300 | 2850 (75 partial right-edge cells `fffff7ff`) | 2850 | 3177 | 3200 |
| 1024x768 | 24576 | 24576 | 24575 | 24576 |

The depth scene writes 1000 cells, the exact union of both quads' cells.  The
sentinel after `(align(W,32)/8) * (align(H,32)/4)` words was never touched,
so that is a sufficient HZ extent (MEASURED; the metadata uses the aligned,
tiled grid, not a linear `ceil` grid).  Observed cell values after a z=0.5 quad
are `ffbff7ff`; their bit meaning remains open.

## 3. Render target size

Let `W,H` be the surface size, `P = align(W, 32)`.

| item | value | evidence |
|---|---|---|
| color `b1` | `0xcc210000 | P` | CAPTURED 320 and 300 (`0x140`), KMS 1024 (`0x400`); MEASURED 300, 1024 |
| depth `b9` | `0xfa210000 | P` | CAPTURED 300/320; MEASURED 300, 1024 |
| HZ `bb` | `0xf0010000 | P` | CAPTURED 300/320; MEASURED 1024 with `0x400` (the `0x140` negative control produced identical depth-scene counts, so the low half is not isolated) |
| raster `f5,c4` | `0` | CAPTURED |
| raster `f6,c5` | `H << 16 | W` (exclusive) | CAPTURED gears `012c012c`; MEASURED |
| preprocessing `f5/f6/c4/c5` | `0 / 7fff7fff` unchanged | CAPTURED (gltri and gears) |
| color and depth BO bytes | vendor constructor: `aw=P`, `ah=align(H,32)` (small rules for <=64), `gap=(ah-H+1)*aw-W`, `extra=max(8-gap,0)`, `align((aw*ah+extra)*4, 0x1000)` | STATIC; MEASURED no sentinel write at `0x50000`, `0x64000`, `0x301000` |
| HZ BO bytes | at least `(P/8) * (align(H,32)/4) * 4` | MEASURED above |
| preprocessing draw | `23000001 0 align(N,4)`, source padded by repeating the last record | CAPTURED rule, MEASURED with N=3, 6, 12 |
| raster draw | `23000001 0 N` | MEASURED |

System constants (48-word prefix, raster reads `[0,0x10)`, preprocessing
`[0x10,0xc0)`) for `glOrtho(0,W,0,H,-1,1)`, full viewport and depth range
`(0,1)`; all other words are invariant:

| word (offset) | value | 320x240 | gears 300x300 capture |
|---|---|---|---|
| 9 (`0x24`) | `2/H` | `3c088889` | (lit program, different layout) |
| 14 (`0x38`) | `2/W` | `3bcccccd` | |
| 22,23 (`0x58`) | `W/2, -H/2` | `160,-120` | `150,-150` at `+0x168` |
| 24,25 | `0.5, 1` | | same |
| 26,27 (`0x68`) | `W/2, H/2` | `160,120` | `150,150` |
| 28,29 | `0.5, H` | `0.5,240` | `0.5,300` |
| 41 (`0xa4`) | `15359/(W/2) - 1` | `94.99399` | `101.39333` |
| 43 (`0xac`) | `-(15359/(H/2) - 1)` | `-126.99167` | `-101.39333` |

The guard-band constant 15359 reproduces all four captured words bit-exact
(CAPTURED/derived); its rendering effect was not isolated.  Words 22..29 follow
the viewport constructor in `parhelia-m9-first-triangle-system-constants.md`
(`x + w/2`, `D - y - h/2`, depth `(f-n)/2`, `(f+n)/2`).

Selector-`6c` linked record: `{W/2, -H/2, 0.5, 0, W/2, H/2, 0.5, 0x31}`
(CAPTURED 320x240 `0x31`; gears `{150,-150,.5,0,150,150,.5,0}`, the last word
is not size dependent).  The tool verifies that these formulas reproduce the
verified 320x240 prefix and linked record byte for byte before submitting.

MEASURED size results (color counts from the tiled BO mmap; positions through
the linear host-aperture view):

| run | expected | result |
|---|---:|---|
| 300x300 `tri` (18.75,25)-(281.25,25)-(150,275) | area 32812.5 | 32810 |
| 300x300 `quad` | 90000 | 90000; linear readback pitch 320: 0 pixels off the position code, max error 1.03/255 |
| 1024x768 `tri` (64,64)-(960,64)-(512,704) | 286720 | 286720 |
| 1024x768 `quad` | 786432 | 786432; linear readback: 0 pixels off, max error 1.12/255 |
| 1024x768 `quad` with 320x240 constants (`-V`) | negative control | 245760 (= 320x768), rejected |

The position-coded quad draws red = x/W and green = y/H with Gouraud shading;
the readback checks every pixel against its own coordinates (window row 0 is
GL y = H-1).  Readback used host-aperture slot 0 with `d1` = the relocated
color address, `d2 = 0xcc210000 | P`, `d3 = 0x10000 | H`, rows of pitch `P`,
and restored the identity window afterwards.

## Kernel interface notes

No kernel change was required: the validator already accepts arbitrary values
at `10`, `23`, `a8..aa`, `ef`, `b1/b9/bb`, `f5/f6/c4/c5`, and variable BO sizes.
Two limitations affect tooling and a later driver:

* VRAM BO addresses are not visible to userspace.  The readback obtained the
  color address from `drm.debug=0x2` relocation messages (`kind 1 -> addr`).
  A render-node debug path (debugfs or ioctl) that performs the slot-0 linear
  copy under the kernel's own KMS slot lock would remove both the address leak
  and the race with the dirty-fb scanout copy.
* The kernel `FIRST_TRIANGLE` template remains fixed at 320x240; only the
  userspace SUBMIT path is size-parametrized.

## Open items

* Texture coordinates: the declaration pair is captured, but the raster pass
  needs the five-stream textured template (`parhelia-m9-vertex-final-contract.md`).
* `bb` low 16 bits at 1024 wide are not isolated (`0x140` and `0x400` gave
  identical depth-scene results).
* Stencil (`a8` bits 5..30, `a9/aa`) and perspective-correct interpolation with
  w != 1 in the raster pass were not exercised on hardware.
* Selector field names `[19:16]` and `[15:12]` rest on captured raster and
  ubyte4 words; only float, stream 0 was measured.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
