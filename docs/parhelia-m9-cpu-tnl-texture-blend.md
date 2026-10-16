# M9 CPU T&L texturing and blending: raster encodings and hardware status

This note collects what is known for texturing and blending on the M9148
(`102b:0540`) userspace two-pass draw used by the m9x CPU T&L driver.  Labels:
**MEASURED** = reproduced through `DRM_IOCTL_PARHELIA_M9_SUBMIT` on the M9148,
**CAPTURED** = vendor ICD command capture, **STATIC** = saved vendor binaries.
Read `parhelia-m9-cpu-tnl-raster-state.md` and
`parhelia-m9-cpu-tnl-draw-parameters.md` first.

Probe tool: `tools/m9-user-texture.c` (build next to `m9-user-triangle-data.h`,
`gcc -O2 -Wall -I<uapi>`).  All state is set with `key=value` arguments
(texture unit block, raster `22/88/8c/f0/10/11`, raster declaration `rdecl=`,
a CPU-written transient view, raster stream 0 redirection, c2 raster-entry
slots).  The texture BO carries a per-dword storage-index code.

## 1. Texture sampling is a c2 fragment program (CAPTURED/MEASURED)

`parhelia-m9-native-fragment-program-re.md` shows that raster `c3` enters a
fragment program at the start of the `c2` image.  Slots 0..3 of the flat,
Gouraud and glxgears images are the vendor color fragment program.  The vendor
GLSL `tex2d` raster draw (`captures/m9x-linux-gl2-special-20260914/tex2d`)
uses exactly the REPLACE raster state below with `c3=0` and a fragment
program of the same four-slot size (first preprocessing `c3=4`, as in the
untextured baseline).  Its `c2` bytes were not dumped.  Section 4 measures
that the raster state alone does not sample: the color fragment program then
outputs the texture-coordinate varying.  The texel fetch therefore lives in
the (uncaptured) fragment slots.  Offline execution of the vendor TEXLD
lowerer emits `ffff8000 0000000f` followed by two copy slots and carries no
coordinate or sampler selector, so the unit routing stays in `70/74` and the
raster declaration (STATIC/RUN).

## 2. System constants and linked record are texture independent (CAPTURED)

Textured draw `texture-resources/phase3/case-20` (256x256 target, 4x4 RGBA8,
GL_REPLACE) uses raster `8d/8e = [0,0x10)`, `6c = +0x400`.  Its `0x21000`
snapshot has the same constant layout as the untextured prefix; only the
viewport-size words differ (`256 -> 0x43000000`), and the linked record is
`{W/2,-H/2,.5,0,W/2,H/2,.5,0}`.  No texture-dependent value lives there.

## 3. Captured textured raster templates

| word | untextured gltri | REPLACE, case-20 | MODULATE gltri 64x64 | MODULATE + blend, q3 |
|---|---|---|---|---|
| `22` | `00006403` flat | `00002306` smooth quads | `00006503` flat | `00002506` smooth quads |
| `88` | `0100dff0` | `0010ffe3` | `0110dfe3` | `0110ff03` |
| `8c` | `00500200` | `00500200` | `00500200` | `00500300` |
| `f0` | `c4008400` | `c4008400` | `c4008400` | `c5008200` |
| streams | 4 (`00..03`) | 4 (`00..03`) | 5 (`00..04`) | 5 |
| `10/11/12` | `00100010/00100006/-` | `00100010/00100006/-` | `00100010/00060010/00000010` | same as gltri |
| raster `23` | `80100d88 801006b6 00010688 00c231b6 00d30688` | `00500688 00010688 00c231b6 00d30688` | `80100d88 801006b6 00510688 00020688 00c331b6 00d40688` | as gltri, bit 31 clear |

Unit block (all textured cases): `74..77=100`, `70=00040118`,
`78=<texture>, 79=c8010000|width, 7a=00010000|height`, `7c=00009200`
(nearest; `00009292` = linear min/mag + nearest mip), `7d=0ff80000`,
`87=0`, `82..85=0`, `80=ffffffff, 81=7e=7f=0`, `71..73=0`.

Decoded consequences (CAPTURED, one sample each; not yet MEASURED):

* Raster declaration attribute `5` is texture coordinate 0; attribute `1`
  (the bit-31 link pair) is the color varying.  REPLACE drops the color pair
  entirely and places the texcoord on stream 0.
* `22` bits `9:8`: `1` in both MODULATE captures, `3` in REPLACE.
* `88`: bit 24 is set whenever the color attribute is declared; the low
  byte is `f0` untextured and `e3` textured without blend, `03` with blend;
  bit 13 is set in REPLACE and in the blended draw.
* `10/11/12` are consistent with one 16-bit record stride per raster stream
  (low half stream `2k`, high half stream `2k+1`): the stream carrying the
  one-component attribute `c` (vendor `b8` view) is `6`, all float4 views are
  `0x10`, in all three layouts.

## 4. Hardware results (MEASURED, September 15, 2026)

Baseline (flat white quad x[40,296) y[40,232) on the untextured template):
49152 pixels `ffffffff`, exact.

| change on the untextured template | result |
|---|---|
| unit block with `7e..85/87`, old kernel | `EPERM` (now allowed) |
| unit block `70/78/79/7a/7c/7d` + `22|=0x100` + `88=0110dfe3` | retired, no pixel written |
| `22|=0x100` only | retired, no pixel written |
| `88=0110dfe3` without a bound texture | engine stop (requester interval never retires; module reload cannot recover; kernel now rejects it with `EINVAL`) |
| REPLACE form: `22=00002303`, `88=0010ffe3`, 4-value decl `00500688 00010688 00c231b6 00d30688`, stream 0 = CPU view of float4 `(s,t,0,1)` records at 16-byte stride, unit block incl. `7e..87`, flat c2 | 49152 pixels written, full quad; every pixel is the interpolated coordinate: `R = s`, `G = t` (sample grid `10..ef` in both axes), `B = ff`, `A = ff`; no texel code (`5a` marker 0 of 48260 interior pixels) |

Consequences:

* Raster declaration attr `5` on stream 0 with a 16-byte float4 record is
  accepted and interpolated; the REPLACE raster state is correct and safe.
* The vendor color fragment program reads that first varying: textures need
  the TEXLD fragment slots (section 1).  Candidate programs were built
  (`tools/m9-user-texture.c frag=`; TEXLD sample slot + output copy in the
  class-4 `2d` form) but could not run: at 256 s after boot another
  submitter's job (target `0x2500`, after this probe's last retired job) timed
  out and left the requester at `0x4a90dbe/0x4a90dc8`; the reload inside the
  lock failed bus-master enable again.
* Texture storage (tiled or linear) remains unmeasured.

## 5. Next measurements (after a reboot)

Common arguments `R="tex=1 texext=1 r22=0x2300 r88=0x0010ffe3 linkbit=0
rs0view=4 tcstream=4 rdecl=00500688,00010688,00c231b6,00d30688,0,0,0,0"`:

1. Output-form check: `frag=ffe22d9d,a2803f1f,ffe2ad9e,a2803f1f` (MOV of
   varying pair `44/45`) must reproduce the section-4 coordinate output.
2. TEXLD candidates, each verified against the storage-index code:
   `frag=ffff8000,0000000f,ffc02d9d,a2803f0f,ffc0ad9e,a2803f0f` and
   `preslot=8 prec3=8 frag=ffff8000,0000000f,ffc0001c,a2803f0f,ffc0011c,a2803f0f,ffc02d9d,a2803f1f,ffc0ad9e,a2803f1f`.
   The verifier reports exact texel matches under the linear-row layout; a
   tiled layout shows as systematic misses naming the fetched dword.
3. Highest value without hardware risk: dump the vendor `tex2d` `c2` slots
   0..3 with `tools/m9-vendor-vram-dump.py` (and a MODULATE fixed-function
   texture draw) to obtain the exact fragment programs.
4. Then MODULATE on four streams, linear filter/wrap, blend.

## 6. Kernel requests

* Allow indices `7e..87` (border `7e..81`, bump `82..85`, reserved `87`)
  with plain values; the vendor emits them in every textured draw.
* Five-stream raster (MODULATE vendor form): allow index `04` with the
  `STREAM` relocation kind (as `00..03`) and index `12`.
* Done: `7e..87`, `04`, `12` allowed; textured `88` without `78` rejected.
* Still open: a recovery path for a stopped engine (non-retired requester
  interval) short of a reboot.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
