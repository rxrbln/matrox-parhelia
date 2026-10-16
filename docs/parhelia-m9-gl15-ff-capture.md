<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 vendor GL 1.5 fixed-function capture: state encodings and results

This note condenses `captures/m9x-linux-gl15-ff-20260915` for the clean-room
Mesa m9x driver.  All facts are **CAPTURED** from the saved M9 Linux vendor
ICD on the M9148 (`102b:0540`, host `192.168.1.56`, September 15, 2026)
unless marked otherwise; nothing here was reproduced through the clean-room
kernel yet (no **MEASURED** claims).  A field decode is stated only where every
case of its matrix agrees; single samples are quoted as raw words.

The per-case evidence is in the capture README and in the generated tables
`summary/{texenv,fragops,texture,raster,tnl,pixel,object}.txt` (raster
words, `23` declaration, `c3` entries, fragment slots, `8d..8e` constants and
native vertex programs of every case, as differences to a baseline).  Rebuild
them with `tools/m9-gl15-ff-summary.py`.

## 1. Capture method (read before reusing the tools)

* One GL state per process, traceable geometry/texels, readback checked by a
  built-in GL reference in each client: `tools/m9x-texenv-capture.c`,
  `m9x-fragops-capture.c`, `m9x-texture-capture.c`, `m9x-raster-capture.c`,
  `m9x-tnl-capture.c`, `m9x-pixel-capture.c`, `m9x-object-capture.c`, runner
  `m9x-texenv-run.sh`.
* Tracer `m9x-exec-trace.c` (commit 2df66cf) with `M9XTRACE_DRAWS=1
  M9XTRACE_BM_CURRENT=1 M9XTRACE_OUT=0` selects draw submissions.  Its limit
  counts submissions: the fragops destination setup alone takes four, so
  cases with setup draws need `M9ENV_DRAWS=16` (the first group 4 run missed
  its case draws and was replaced).
* **The process' own VRAM mapping is not linear.**  The 256 MiB mapping at
  mmap offset `0xd0000000` read at `gpu - 0x10000000` returned `ffff...` for a
  texture and allocator words for `c2`.  `M9XTRACE_RES`/`VRAM_ADDR` output for
  VRAM addresses is therefore not resource content.  `c2` and `78` are read
  through host-aperture slot 0 (`tools/m9-vendor-vram-dump.py`) while the
  client holds its idle context (`tools/m9x-draw-resources.py` joins the
  addresses); the slot-0 read returns the uploaded texels and program slots.
* `8d..8e` and `6c` live in the 0x21000 system allocation: allocation record
  `+0x208` GPU base, `+0x210` mmap cookie selects the snapshot.
* A client that starts X inside `flock` must close the lock descriptor
  (`exec 3>&-`), otherwise X holds the lock for its lifetime.
* `GL_ZERO` is 0: capture tables must not use 0 as "unset" for enum fields.
  The first fragops parts silently ran KEEP/ONE for five ZERO cases; they were
  re-captured (`fragops/part-4-zero.tgz`).

## 2. Draw structure

A textured fixed-function draw is several preprocessing passes plus one
raster pass on the same `c2` image: e.g. MODULATE binds preprocessing entries
`c3=4, 0x14, 0x19` and raster `c3=0`.  Raster `c3` enters the fragment
program at slot 0; the first preprocessing `c3` equals the fragment slot
count.  The untextured program is `5e62ad9e ee003c18 | 1e622d9d ae003c18 |
ffff801c a01fc00f | ffff801c a01fc00f`; textured MODULATE is `5e62849c
ee003c18 | 1e621a9c ae003c18 | 413fad9e ac200140 | 06bfad9d ac200140`.

## 3. Texture environment (group 1)

Fragment program slot counts (raster entry 0 to first preprocessing entry):

| state | slots | other raster deltas vs MODULATE/RGBA |
|---|---:|---|
| MODULATE RGBA/RGB/L/I/LA | 4 | `79` format byte only |
| MODULATE ALPHA | 5 | `88=0110ffc3` |
| REPLACE RGBA | 4 | `22=2306`, `88=0010ffe3`, 4-entry `23` without color |
| DECAL | 8 | `88=0110ff83`, `23` color pair `00100d9a 00100236` |
| BLEND | 8 (I: 6, A: 5) | `8c=00500400`, env color = raster constant pair 0..1 |
| ADD | 6 (I: 4) | `88=0110ff83` |
| COMBINE RGB REPLACE/MODULATE/ADD/ADD_SIGNED/INTERPOLATE/SUBTRACT/DOT3_RGB/DOT3_RGBA | 4/6/6/8/8/6/12/10 | ADD_SIGNED constants `[1 .5 0 1]` |
| COMBINE alpha REPLACE/MODULATE/ADD/ADD_SIGNED/INTERPOLATE/SUBTRACT | 4/5/5/6/6/5 | |
| RGB or alpha scale 2 or 4 | 8 | |
| 2 / 3 / 4 units MODULATE | 6 / 8 / 10 | `71..73=00040118`, `88=0130ff0f/0170e33f/01f083ff`, one `23` texcoord entry per unit |

Every combine source, operand and scale gives its own fragment program hash;
the slots are in `summary/texenv.txt` and `all-cases.json`.

## 4. Sampler and texture descriptor words (groups 1-3)

| word | field | values |
|---|---|---|
| `7c[24:22]`, `[27:25]`, `[30:28]` | wrap S, T, R | REPEAT 0, CLAMP_TO_EDGE 1, MIRRORED_REPEAT 4, CLAMP 6, CLAMP_TO_BORDER 7 (2-D R reads 0; 3-D, cube and rectangle default R 1) |
| `7c[11:0]` | filters | NEAREST/NEAREST `200`, mag LINEAR `+010`, min LINEAR `202`, NMN `280`, LMN `282`, NML `300`, LML `302`; NMN with mag LINEAR `690` |
| `7e`, `7f` | border color | border (0x33,0x99,0xcc,0x66) gives `7e=99003300`, `7f=6600cc00` |
| `79[31:24]` | format | RGBA8 `c8`, RGB8 `c9`, BGRA upload `cc`, RGB5 and R3_G3_B2 `9d`, RGBA4 `8c`, RGB5_A1 `94`, ALPHA8 `66`, LUMINANCE8 `61`, LUMINANCE8_ALPHA8 and INTENSITY8 `a2`, DEPTH16 `b8`, DEPTH24 `fa`, DEPTH32 `f8` |
| `79[23:16]` | resource kind (raw) | 2-D `01`, 1-D `18`, 3-D `11`, cube `09`, FBO-rendered 2-D `21` |
| `79[15:0]`, `7a[15:0]`, `7a[27:16]` | width, height, depth | e.g. 13x7 NPOT `c801000d/..010007`, 3-D 4x4x4 `c8110004/00040004` |
| `7a[31:28]` | highest mip level of the chain from the base | 16x16 chain 4, base level 1 of 16x16 3 (dims 8x8), 13x7 chain 3, 4x4 chain 2, 128 FBO chain 7 |
| `70[11:8]` | target class (raw) | 2-D `1`, 3-D `2`, cube `3`, 2-D depth compare `2` |
| `70[26]`, `70[29:27]` | depth compare enable, function | NEVER 0, LESS 1, LEQUAL 2, EQUAL 3, NOTEQUAL 4, GEQUAL 5, GREATER 6, ALWAYS 7 (order differs from the `a8` depth test codes) |
| `70[19]` | set with non-default LOD bias/min/max LOD or max level | `00040118 -> 000c0118` |
| `7d[9:0]` | LOD bias, signed, 5 fraction bits | +1 `020`, -1.5 `3d0` |
| `7d` raw | LOD bounds | default `0ff80000`, MIN_LOD 2 `0ff90000`, MAX_LOD 1 `01000000`, MAX_LEVEL 2 `02000000`, base 1 max 2 `01000000` |

Raw DEPTH16 sampling sets `7c[31]` while raw DEPTH24/32 do not (single
samples).  Texture targets and
formats keep the 4-slot REPLACE program except non-RGBA base formats (see
`summary/texture.txt`); depth compare uses a 7-slot program.

## 5. Per-fragment operations (group 4)

| state | direct words | fragment program / constants |
|---|---|---|
| alpha test (7 functions) | none | 6 slots per function; constants `[128 128 .5 255 2 1 ...]` (reference `*256`); NEVER uses reference `-1` |
| blend, factors without destination read (e.g. SRC_ALPHA/ZERO, ZERO/ZERO) | `88=0100dfe0` | 4 slots, a distinct program per source factor |
| blend, destination-reading factors or equations | `88=0100ffc0`, `f0=c5008200` | 4..10 slots, one constant pair added (`8c` +1) |
| blend CONSTANT_* factors | as above | `8c` +3; constants start with the blend color |
| BlendFuncSeparate | `88=0100ffe0` | 6 slots |
| logic op (15 non-COPY ops) | `88=01080000`, `ef[7:0]=aa`, `f0=c5008200` | 39 slots per op, constants `[6.10352e-05 ... 65536 65535 .00390625 4 ... 1024]`; COPY emits nothing |
| depth test | `a8[4:2]` NEVER 0, LESS 2, EQUAL 1, LEQUAL 3, GREATER 4, NOTEQUAL 6, GEQUAL 5, ALWAYS 7; `a8[1]` and `ef[15]` depth write | |
| stencil | `a8[9:7]` function (same codes as depth), `[12:10]` sfail, `[15:13]` zfail, `[18:16]` zpass: KEEP 0, REPLACE 1, INVERT 2, ZERO 3, INCR_WRAP 4, DECR_WRAP 5, INCR 6, DECR 7 (ZERO from the `g4z` re-capture); `a9=08WWMMRR` | |
| core two-sided stencil | raw: front LESS/INCR + back GREATER/DECR `a8=6017026a`, `aa=00ffff5c`; front and back ALWAYS REPLACE/INVERT `a8=103a03ea`, `a9=08ffffa3`, `aa=00ffff5c` (`aa` has the `a9` layout) | |
| color mask | `ab[15:0]`: R `4444`, G `2222`, B `1111`, A `8888` | |
| dither | `ef[7:0]` `55` off, `00` on | |
| polygon offset enable | `ef[26]` | factor and units are not in the direct words; their consumer is not identified |
| cull (raw) | `ef[25:24]`: BACK/CCW 3, FRONT/CCW 2, BACK/CW 2, FRONT/CW 3, FRONT_AND_BACK/CCW 2; all set `6a=24cc0000` | |

## 6. Rasterization, fog, points (groups 5/6)

* Lines: `22=00006401`, `6a=25012505`, `f0=c8008400`, width `f4` with
  `f0=cc008400`; line stipple emits no stipple word (identical to width 1)
  while the visible pattern is exact.  Line smooth adds `ef=6000....`, blend
  `88=0100ffc0`, `f0=c9008200` and a 6-slot fragment program.
* Polygon mode LINE/POINT change `22` (`00016453` front, `00046403` both),
  `6a` and `f0`; POINT binds a 1x1 texture unit (`79=cc010001`).
* Points: `22=00006500`, `6a=250128ab`, `f0=c2008400`, `f4` = size, and a
  1x1 texture unit.  Point smooth sets `70=0004111c`, `71=0004111d`, a 32x32
  LUMINANCE8 descriptor (`79=61010020`, `7a=50010020`), `88=0130b30f` and
  `f0=c3008200`; point sprite binds the sprite texture with the REPLACE
  program and `22=00006300`.
* Polygon stipple binds a 32x32 descriptor (`79=ea010020`, `70=0004111c`)
  and adds a raster draw: the submission's `c3` entries are `c, 0, 4` and the
  final raster draw enters at `c3=4`.
* Fog (all modes): `22=00006606`, `88=0910df10`, a 1x1 texture unit and a
  7-slot fragment program; LINEAR/EXP/EXP2, start/end/density and color differ
  only in preprocessing entries and raster constants (fog color RGB at raster
  constant floats 10..12; LINEAR start/end at floats 0..3).
* Shading: flat `22=00006406`, smooth `00002406` with the declaration link bit
  cleared.

## 7. Transform and lighting (group 7)

Directional lighting, local viewer, color material and normalize/rescale leave
the raster words unchanged and change only the preprocessing vertex program
and its constants.  Positional, spot and multi-light cases additionally bind
a 1x1 texture unit (`70=00040118`, `79=cc010001`, `88=0110dff0`) and add
preprocessing entries (positional light: entries `4` with 108 slots and `70`
with 27 slots; eight lights: nine entries).  Texgen and the texture matrix
bind the sampled texture with the REPLACE program and add entries.  Clip planes set `6a=2501033a`.  Two-sided
lighting uses `88=03109ff0`, a declaration with front and back color pairs
and an 8-slot fragment program; separate specular and secondary color use
`88=0310dff8` and a 10-slot program.

## 8. Vendor behaviour a clean implementation must not copy

| defect | evidence |
|---|---|
| lower texture unit disabled: enabled higher unit samples one constant texel | `texenv mt2_unit0_off`, `mt4_unit3_only`, `mt2_unit0_off_tc0` |
| `BlendEquationSeparate(MIN, ADD)` alpha wrong | `fragops blend_eq_separate_min_add` |
| `GL_STENCIL_TEST_TWO_SIDE_EXT` enable `INVALID_ENUM`; `StencilFuncSeparateATI` `INVALID_ENUM` | `fragops stencil_two_side_ext/_ati` |
| `CopyTexSubImage2D` loses texels outside the copied rectangle | `texture copytexsubimage` |
| `CullFace(FRONT_AND_BACK)` culls only front faces (encoded as FRONT) | `raster cull_front_and_back` |
| point distance attenuation ignored | `raster point_attenuation` |
| two-sided lighting lights front faces with the back shininess | `tnl light_two_side` (max error 1 with that model) |
| `DrawPixels(STENCIL_INDEX)` writes depth | `pixel drawpixels_stencil_depth_side_effect` |
| `ReadPixels(LUMINANCE)` wraps R+G+B modulo 256 | `pixel readpixels_luminance_ubyte` |
| render to texture level 1 or cube face stays empty although complete | `object fbo_render_to_level_1/_cube_face` |
| `GenerateMipmapEXT` on an FBO texture leaves wrong levels | `object fbo_generate_mipmap(_getteximage)` |
| context teardown with a live occlusion query object crashes the client | `object g910 occlusion_query_visible` (exit 139) |

Implementation-defined results worth matching or documenting: source colors
are truncated to 8 bits (visible only in exact logic ops), polygon offset
units scale by about 2^-24 in FILL/POINT but about 100 times more in LINE
mode, a size-1 point at an integer vertex covers pixel (x-1, y), and point
sprite t decreases upward.

## 9. Coverage and remaining work

All ten requested groups ran: texenv 98+5, fragops 134, samplers/targets/
formats 79, fog/raster 50, T&L 31, pixel 35, FBO/buffers/queries 25 cases.
Not covered: PBO (extension not advertised), EXT_packed_depth_stencil (not
advertised although DEPTH24_STENCIL8 storage works), multisample, polygon
smooth coverage and smooth line/point coverage values, point fade threshold
effect (attenuation itself is ignored).  The vertex program slots are saved
per case but not yet decoded into operations; that is the input for the
fixed-function vertex lowering.
