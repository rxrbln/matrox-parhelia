# P690 texture address modes and the border colour

Copyright 2026 René Rebe <rene@exactco.de>

Hardware validation of the Parhelia texture-address unit on the P690
(PCI 102b:2539), and the Gallium driver support built on it.

The reverse engineering that produced the candidate encoding is in
`parhelia-texture-sampling.md` §4, §4.1, §4.2 and §8 (the vendor's shared
address-mode table, the register roles and the identification of `0x11c8`).
This document adds what was missing: **every code measured on silicon**, the
border colour proved end to end including its format-directed packing, the
colour-key polarity, and the driver mapping and caps that follow.

Probe: `parhelia-drm-test --wrap-probe /dev/dri/renderD128`.
GL conformance: `tools/parhelia-gl-wrap-test.c`.

## 1. The registers

| register | role |
|---|---|
| `0x11c0` | filter word: MIN `[2:0]`, MAG `[5:3]`, MIP `[8:6]`, bit 12 set, LOD bias `[24:16]` signed 4.4 biased -4.0 |
| `0x11c4` | **address word**: S `[2:0]`, T `[5:3]`, R `[8:6]`, colour-key control `[10:9]`, cylindrical wrap `[13:11]`, LOD min `[23:16]`, LOD max `[31:24]` |
| `0x11c8` | **border colour** |
| `0x11cc` / `0x11d0` | colour-key value and mask (**not** the border colour) |
| `0x11d4` / `0x11d8` | bump-environment matrix |

`docs/parhelia-fragment-isa.md` §11.3/§11.4 has `0x11c0` and `0x11c4` the wrong
way round and calls `0x11cc` the border colour.  Both corrections were already
recorded in `parhelia-textured-draw.md` §6 and `parhelia-texture-sampling.md`
§8; the sweep below confirms them a third way, from the live P690: only
`0x11c4` moves the sampled image, and only `0x11c8` moves the border colour.

MTXPARD settles the remaining filter-field ambiguity from the D3D side:
`0x17a2e4`-`0x17a34e` builds `0x11c0` as `[2:0] = tbl0x1d4558[MINFILTER]`,
`[5:3] = tbl0x1d4570[MAGFILTER]`, `[8:6] = tbl0x1d4588[MIPFILTER]`, so
**`0x11c0` is MIN, MAG, MIP in that order** — `parhelia-fragment-isa.md`
§11.4's open question. **PROVEN [BIN]**

## 2. Measured address-mode codes  **PROVEN [HW]**

A 64x64 texture whose texels carry their own `(x, y)` is sampled by a 320x320
1:1 quad running from -2 to +3, so output pixel `p` addresses texel `p - 128`
before the wrap.  Each of the 320 samples on the swept axis is compared with
the arithmetic of every wrap mode a GL driver can ask for; a code is only
reported when **all 320 match exactly**, with `GL_NEAREST` filtering and a
`0x11c8` border of `0xc0ffee` so a border mode cannot be mistaken for a clamp.

| code | S axis | T axis | mode |
|---|---|---|---|
| 0 | exact | exact | **repeat** |
| 1 | exact | exact | **clamp to edge** |
| 2 | exact | exact | repeat (alias) |
| 3 | exact | exact | repeat (alias) |
| 4 | exact | exact | **mirrored repeat** |
| 5 | exact | exact | **mirror once, then clamp to edge** |
| 6 | exact | exact | **clamp to edge under point sampling** (`GL_CLAMP`) |
| 7 | exact | exact | **clamp to border** |

This confirms the vendor table `{0,0,4,1,7,5,6}` (MTXPARD `.data 0x1d45d0`,
kernel `mtx3018` `.data+0x4750`) on silicon, and it settles what the binaries
could not: **code 6 point-samples the edge texel, not the border**, which is
exactly `GL_CLAMP` with a zero-width texture border, while **code 7 returns the
border colour outside `[0,1]`**.  Both load `0x11c8` — the vendor gates the
register write on `field >= 6` on all three axes (MTXPARD `0x17a691`, kernel
`0x71f88`) — so code 6 differs from code 1 only in the outermost half texel,
where a linear footprint reaches the border.

Phase 5 measures that difference directly: with `GL_LINEAR` and the
coordinates shifted half a texel, so pixel centres land on texel boundaries,
the columns either side of `s = 0` read (texel `(0,32)` is `002000` and the
border is `c0ffee`):

| S code | boundary -2/-1 | boundary -1/0 | boundary 0/1 |
|---|---|---|---|
| 1 clamp-to-edge | `002000` | `002000` | `002001` |
| 6 `GL_CLAMP` | `608f77` | `608f77` | `002001` |
| 7 clamp-to-border | `c0ffee` | `619078` | `002001` |

`608f77` is the half-and-half mix of the border and texel `(0,32)`.  Code 1
never touches the border; **code 6 clamps the coordinate, so the border stays
confined to that outermost half texel however far outside the coordinate
goes**; **code 7 lets the footprint leave and returns pure border**.  That is
`GL_CLAMP` and `GL_CLAMP_TO_BORDER` respectively, now measured rather than
inferred. **PROVEN [HW]**

Codes 2 and 3 sample as plain repeat.  Neither vendor stack can emit them —
the shared table never produces them and the D3D path clamps an out-of-range
enum to index 6 (MTXPARD `0x17a58b`) — so they are treated as reserved.

The three fields are independent: sweeping one axis leaves the other's image
bit-exact, which also proves the fields are **three bits wide**, not two (code
6 on S would otherwise spill bit 2 into T).

`0x11c0` does not address.  Sweeping its image-filter fields over 1, 2 and 3
leaves the addressing at repeat in every case (code 1 is not a defined filter
and returns a filtered image with no wrap change; codes 2 and 3 return repeat
exactly).

### 2.1 The R axis

For a 2-D sampler the R field is not evaluated, and a 2-D sweep can only show
non-interference, which it does for codes 0 and 1.  Codes above that were not
swept on a 2-D descriptor: an early run locked the machine hard during the R
sweep, so `--wrap-probe` keeps the R axis behind `PROBE_WRAP_R=1`.  The R
field's **position** (`[8:6]`) and its shared code table are proven from two
independent vendor binaries — kernel `mtx1723` `0x72744`-`0x72776` masks
`0xfe3f` and shifts the table result left 6, and the Windows ICD encodes
`GL_TEXTURE_WRAP_R` into `texObj+0x08[14:12]` with the same five GL enums as S
and T (MTXOGL `0x103b6e90`, `0x103b7b9c`; readback table `0x103b8d0c`).  Its
**behaviour** needs a volume or cube sampler; `parhelia-gl-wrap-test`'s
`GL_TEXTURE_WRAP_R` check drives it, and as of this measurement it still
skips because a 3-D texture does not yet sample its own slices (§6.1).  **The
R axis is therefore INFERRED, not measured.**

## 3. The border colour  **PROVEN [HW]**

**The border colour is `0x11c8`.**  With S = 7 and a 64x64 A8R8G8B8 texture,
writing each of eight values to `0x11c8` reproduces it exactly in every
out-of-range texel, while the same values in `0x11cc` or `0x11d0` change
nothing outside:

```
0x11c8=00c0ffee -> outside c0ffee     0x11cc=<any>    -> outside 000000
0x11c8=ff112233 -> outside 112233     0x11d0=<any>    -> outside 000000
0x11c8=ffffffff -> outside ffffff
0x11c8=00000000 -> outside 000000
```

`0x11c4[10:9]` has no effect on the border: all four values leave the border
and the in-range texels unchanged.  It is the colour-key control, as §8 of
`parhelia-texture-sampling.md` says.

### 3.1 Channel order is format directed

The vendor converter is a jump table on the texture's internal format
(MTXPARD `0x84560`, kernel `mtx678` `.text+0x7bc50`) with two classes that
matter for the formats this driver samples.  Both were measured, offering the
same colour `0xff336699` in each packing to each sampled format:

| sampler format | `0x11c8` as A8R8G8B8 | `0x11c8` as A2R10G10B10 |
|---|---|---|
| `0x32` A8R8G8B8 | **336699** | c66665 |
| `0x33` X8R8G8B8 | **336699** | c66665 |
| `0x24` A4R4G4B4 | **336699** | c66665 |
| `0x11` L8 | **336699** | c66665 |
| `0x28` R5G6B5 | fc36a6 | **336699** |
| `0x26` A1R5G5B5 | fc36a6 | **336699** |

So the wide, 4-bit and 8-bit classes take the A8R8G8B8 value verbatim, and the
**5-bit-channel 16 bpp classes take A2R10G10B10**: `A[31:30] | R[29:20] |
G[19:10] | B[9:0]`, each channel scaled `v * 1023 / 255` and alpha `v * 3 /
255` (the vendor's truncating integer form).  `p3d_border_colour()` implements
exactly this; `0xff336699` packs to `0xccc66665`, which the hardware returns as
`336699` with no error.

GL's default border, transparent black, is zero in both packings — which is
why every vendor capture carries `0x11c8 = 0`.

### 3.2 The colour key is always armed — `0x11cc` must not be left at zero

A side result of the sweep settles the polarity §8 left inferred.  With
`0x11cc = 0` and `0x11d0 = 0` **every in-range texel disappeared** and the
clear colour showed through, while every other `0x11cc` value drew normally:

```
border 0x11cc=00000000 -> outside 000000, inside fefefe   (fefefe = the clear)
border 0x11cc=<other>  -> outside 000000, inside 002020
```

So the compare is `(texel & MASK) == KEY` and it is **unconditionally armed**:
there is no enable bit.  The vendor disables keying by programming the
impossible pair `KEY = 0xffffffff, MASK = 0`, which can never match.  Our
`0x11cc = 0xffffffff` default is therefore load bearing, not cosmetic: a
driver that zeroed `0x11cc` would key out its whole texture. **PROVEN [HW]**

## 4. Driver support

`ph_sampler_wrap()` maps Gallium onto the measured codes:

| `PIPE_TEX_WRAP_*` | GL | code |
|---|---|---|
| `REPEAT` | `GL_REPEAT` | 0 |
| `CLAMP_TO_EDGE` | `GL_CLAMP_TO_EDGE` | 1 |
| `MIRROR_REPEAT` | `GL_MIRRORED_REPEAT` | 4 |
| `MIRROR_CLAMP_TO_EDGE` | `GL_MIRROR_CLAMP_TO_EDGE` | 5 |
| `CLAMP` | `GL_CLAMP` | 6 |
| `CLAMP_TO_BORDER` | `GL_CLAMP_TO_BORDER` | 7 |
| `MIRROR_CLAMP`, `MIRROR_CLAMP_TO_BORDER` | `GL_MIRROR_CLAMP_EXT`, `..._TO_BORDER` | **none** |

A mode with no code is **refused**: `ph_sampler_wrap_axes()` returns false, the
draw is dropped with `-EOPNOTSUPP` and one diagnostic line.  Substituting a
different mode would render the wrong image silently, which is what the
previous builder did — it mapped *every* non-repeat mode to code 6, so
`GL_CLAMP_TO_EDGE` was programmed as `GL_CLAMP` and `GL_MIRRORED_REPEAT` and
`GL_CLAMP_TO_BORDER` were clamped.

The R axis is only programmed for targets that sample three coordinates
(`PIPE_TEXTURE_3D`, `PIPE_TEXTURE_CUBE`, `PIPE_TEXTURE_CUBE_ARRAY`), so a
`GL_TEXTURE_WRAP_R` the unit cannot express never refuses a 2-D draw.

`GL_TEXTURE_BORDER_COLOR` is clamped to `[0, 1]`, rounded to eight bits per
channel as `A8R8G8B8` and packed for the sampled format by
`p3d_border_colour()`.  `0x11c8` is written only when an axis asks for a
border (`p3d_tex_border_used()`), matching the vendor gate.  The vendor's own
stacks do **not** clamp — MTXOGL `0x1039df9b` and the Linux ICD's
`libGL 0x10f839` both compute `trunc(f * 255 + 0.5)` and keep the low byte, so
a border colour outside `[0, 1]` wraps modulo 256 there.  GL requires the
clamp, so this driver clamps.

Caps: `texture_mirror_clamp_to_edge` is now advertised
(`GL_ARB_texture_mirror_clamp_to_edge`), `texture_mirror_clamp` is not.
`GL_MIRRORED_REPEAT` and `GL_CLAMP_TO_BORDER` have no cap of their own in
Gallium — they are core GL 1.3/1.4 — and are now genuinely implemented rather
than silently replaced.

## 5. What the hardware cannot do

* **Mirror into a border.**  The unit mirrors once into the *edge* (code 5)
  and has no mirror-and-border mode, so `GL_MIRROR_CLAMP_EXT` and
  `GL_MIRROR_CLAMP_TO_BORDER_EXT` cannot be expressed.  Neither vendor stack
  exposes them either: the Windows ICD's `glTexParameter` accepts exactly
  `{GL_CLAMP, GL_REPEAT, GL_CLAMP_TO_BORDER, GL_CLAMP_TO_EDGE,
  GL_MIRRORED_REPEAT}` and rejects `0x8742`/`0x8743` with `GL_INVALID_ENUM`
  (MTXOGL `0x103b7337`), and `GL_EXT_texture_mirror_clamp` appears in no
  vendor extension string.
* **A one-texel texture border ring** (`glTexImage2D(..., border=1)`) is a
  different feature: `0x11e0[22]` selects it and this driver leaves it clear.
  It is unrelated to the border *colour*.
* Codes 2 and 3 exist in the field but are aliases of repeat on the S and T
  axes and are emitted by no vendor path; the driver never programs them.
  One run locked the machine while sweeping codes above 1 in the **R** field
  of a 2-D descriptor.  That is a single observation on a shared target that
  was being rebooted from elsewhere at the time, so it is recorded as a
  hazard, not a result: `--wrap-probe` keeps the R sweep behind
  `PROBE_WRAP_R=1`, and the driver programs the R field only for targets that
  sample three coordinates.
* The D3D side advertises the full `D3DPTADDRESSCAPS` set `0x3f` — wrap,
  mirror, clamp, border, independent UV and mirror-once (MTXPARD `.data`
  `0x1d1ed0` and `0x1d1f08`, plus a hard-coded `0x3f` for
  `VolumeTextureAddressCaps` at `0x86bba`) — which agrees with the six usable
  codes measured here and with the absence of a mirror-into-border mode.

## 6. The two tests

`parhelia-drm-test --wrap-probe /dev/dri/renderD128` drives the registers
directly.  `PROBE_WRAP_PHASES` is a bit mask over its phases: 1 the address
sweep, 2 the filter word, 4 the border colour, 8 the border colour per sampled
format, 16 the linear-filter separation of codes 1, 6 and 7.  `PROBE_WRAP_R=1`
adds the R-axis sweep, `PROBE_WRAP_FIRST=axis*8+code` resumes after a code
that locked the engine, and `PROBE_WRAP_DUMP=1` prints the sampled line.  It
opens with the plain `[0,1]` 1:1 draw the mip probe makes, so a broken sampler
set-up cannot be mistaken for a wrap result.

`tools/parhelia-gl-wrap-test.c` is the GL-level test, built on the target with

```sh
cc -O2 -o parhelia-gl-wrap-test parhelia-gl-wrap-test.c \
   -I/usr/X11R7/include -L/usr/X11R7/lib64 -lGL -lX11 -lm
```

It draws the same self-indexing 64x64 texture 1:1 over `[-2,3)` and compares
every one of the 102400 pixels against the mode's arithmetic, for
`GL_REPEAT`, `GL_CLAMP`, `GL_CLAMP_TO_EDGE`, `GL_CLAMP_TO_BORDER`,
`GL_MIRRORED_REPEAT`, `GL_MIRROR_CLAMP_TO_EDGE` and the two mirror-into-border
forms, each on both axes and then on one axis with `GL_REPEAT` on the other.
The border colour is `(0.2, 0.6, 0.898, 1.0)` — no channel equal, none at an
end of the range and none coincidable with a texel code.  It also checks the
linear border blend at the `s = 0` boundary and `GL_TEXTURE_WRAP_R` on a 3-D
texture, skipping the latter where the implementation has no 3-D target.
`WRAPTEST_ONLY=N` runs one mode.

### 6.1 GL result on the P690, depth 24, glamor

```
renderer: Matrox P690 (SUN90)
border colour e59933
PASS: GL_REPEAT both axes: 0/102400 pixels differ
PASS: GL_CLAMP_TO_EDGE both axes / on S / on T: 0/102400
INFO: GL_CLAMP behaves as the clamp-to-edge interpretation
PASS: GL_CLAMP both axes / on S / on T: 0/102400
PASS: GL_CLAMP_TO_BORDER both axes / on S / on T: 0/102400
PASS: GL_MIRRORED_REPEAT both axes / on S / on T: 0/102400
PASS: GL_MIRROR_CLAMP_TO_EDGE both axes / on S / on T: 0/102400
SKIP: GL_MIRROR_CLAMP: rejected by the implementation
SKIP: GL_MIRROR_CLAMP_TO_BORDER: rejected by the implementation
PASS: GL_CLAMP_TO_BORDER linear boundary is half border, half texel 0: 0/24
SKIP: GL_TEXTURE_WRAP_R: no usable 3-D texture
PASS: 0 failing checks, 3 skipped
```

Every mode the hardware has is exact over all 102400 pixels on both axes and
independently per axis, with a non-trivial border colour; the two
mirror-into-border forms are refused by the caps rather than substituted.
`GL_CLAMP` matching clamp-to-edge under `GL_NEAREST` is the correct behaviour
for a zero-width texture border and agrees with the register measurement of
code 6.

`GL_TEXTURE_WRAP_R` needs a 3-D texture.  With `PARHELIA_VOLUME_TARGETS=1` one
can now be created, but the test's control draw — each slice sampled at its
own centre with clamp-to-edge on R — does not return the slice's colour, so
the R axis is still skipped rather than reported either way:

```
SKIP: GL_TEXTURE_WRAP_R: a 3-D texture does not sample its own slices yet
      (slice 0 read ff, wanted 20)
```

### 6.2 What the old mapping did, measured

`PARHELIA_LEGACY_WRAP=1` restores the previous builder, which sent every
non-repeat mode to code 6.  The same test then reports **9 failing checks**:

```
FAIL: GL_CLAMP_TO_BORDER both axes: 98304/102400 pixels differ
FAIL: GL_MIRRORED_REPEAT both axes: 97216/102400 pixels differ
FAIL: GL_MIRROR_CLAMP_TO_EDGE both axes: 64764/102400 pixels differ
```

`GL_CLAMP_TO_EDGE` still passes under the old mapping because code 6 point-
samples the edge exactly like code 1; its error only showed in the outermost
half texel under linear filtering, which is why it went unnoticed.

### 6.3 Cost

None.  The Quake 3 `bench` timedemo at 1280x1024 in a depth-16 glamor screen
measures **47.1 fps with the measured mapping and 47.0 with the legacy one**
(791 frames, 16.8 s).  The mapping changes a field value, not the number of
register writes.

## 7. Corrections this work makes to earlier documents

| document | claim | correction |
|---|---|---|
| `parhelia-fragment-isa.md` §11.3 | `0x11c0` is the address/wrap register, 2 bits per axis | `0x11c4` is, 3 bits per axis; `0x11c0` is MIN/MAG/MIP plus the LOD bias |
| `parhelia-fragment-isa.md` §11.4 | `0x11c4` is the filter word, `[10:9]` a border-colour enable | `0x11c4` is the address word and `[10:9]` is the colour-key control, which does not touch the border colour |
| `parhelia-fragment-isa.md` §11.1 | `0x11cc` is the border colour and `0x11d0` its mask | `0x11c8` is the border colour; `0x11cc`/`0x11d0` are the colour key and its mask |
| `parhelia-fragment-isa.md` §11.4 | which of `0x11c0`'s first two fields is min and which is mag is undetermined | `[2:0]` is MIN and `[5:3]` is MAG (MTXPARD `0x17a2e4`) |
| `parhelia-texture-sampling.md` §4.1 | which of codes 6 and 7 is which is not decidable from the binaries | measured: 6 point-samples the edge, 7 returns the border |
| `parhelia-texture-sampling.md` §8 | the colour-key polarity is inferred | measured: the compare is `(texel & MASK) == KEY`, always armed |
