# M9 fixed-function and sampler state

This note records controlled direct-rendering differentials from the saved M9
Linux vendor ICD on the M9148 target.  Every A/B pair used one GLX context and
changed one GL state between identical textured triangles.  The X server stayed
alive, every fixture returned `GL_NO_ERROR`, and the post-run renderer remained
`Matrox ICD for M-Series` with direct rendering enabled.

Evidence is in `captures/m9x-linux-fixed-state-20260914`; run:

```sh
python3 tools/m9-fixed-state-evidence.py --self-test
```

## Sampler word `7c`

The baseline 2-D sampler is clamp-to-edge S/T/R, linear magnification, and
linear mipmap-linear minification.  Its word is `02409312`.

| GL change | `7c` |
|---|---:|
| S repeat | `02009312` |
| S mirrored repeat | `03009312` |
| S clamp-to-border | `03c09312` |
| T repeat | `00409312` |
| T mirrored repeat | `08409312` |
| R repeat on `GL_TEXTURE_2D` | `02409312` |
| min nearest | `02409210` |
| min linear, no mip selection | `02409212` |
| nearest-mipmap-nearest | `02409690` |
| linear-mipmap-nearest | `02409292` |
| nearest-mipmap-linear | `02409710` |
| mag nearest | `02409302` |

Bits 24:22 are the S wrap code and bits 27:25 are T: repeat `0`, edge `1`,
mirrored repeat `4`, border `7`.  The unchanged R case is expected for a 2-D
resource and does not assign the R field; a 3-D texture is still required.
The complete low sampler values above are safer initial implementation
constants than an inferred subfield split.

Word `7d` is `0ff80000` at baseline.  LOD bias 1.25 produces `0ff80028`, which
is consistent with 5 fractional bits.  Minimum LOD 2 produces `0ff90000`, and
maximum LOD 2 produces `02000000`.  Each nondefault LOD case also changes the
last `70` word from `00040118` to `000c0118`.  These are exact encodings; the
signed saturation boundaries still need a negative/boundary matrix.

The ICD reports no `GL_EXT_texture_filter_anisotropic`, and the two guarded
anisotropy fixtures therefore issue no extension call.  M9 software should not
advertise anisotropy from this evidence.

## Alpha and blending

Enabling blending changes the last setup words from `88=0110dfe3,
f0=c4008400` to `88=0110ff03,f0=c5008200`.  Four requested factor/equation
combinations all produce those same direct words:

* source alpha / one-minus-source-alpha, add;
* one / one, add;
* destination color / zero, add;
* source alpha / one, subtract.

The factor and equation selection is therefore indirect or program-derived;
this capture proves the enable path but does not assign those fields.  A
destination-sensitive correctness fixture plus linked-record/native-state
capture is needed before implementing the full blend matrix.

Alpha reference 0.5 is materialized twice as float `128.0` (`43000000`) at
resource offsets `600/604`; reference 0.25 becomes float `64.0` (`42800000`).
Greater and greater-or-equal do not differ in the direct setup stream.  This
assigns the reference conversion but leaves the comparison opcode in generated
program state.

Color-mask RGB/no-alpha changes the low half of the last `ab` word from `ffff`
to `7777`; red/alpha changes it to `cccc`.  The high half contains a
process-specific address fragment and must be relocated rather than copied.

## Depth and stencil

Baseline setup ends with `a8=0038038a`, `a9=08ffff00`, and `ef=00000000`.

| State | `a8` | `ef` |
|---|---:|---:|
| depth less, write enabled | `0038038b` | `0000f000` |
| depth lequal, write disabled | `0038038d` | `00007000` |
| depth always, write disabled | `0038039d` | `00007000` |

The extended same-context matrix closes every comparison.  Bits 4:2 use the
following direct hardware codes (GL order): NEVER `0`, LESS `2`, EQUAL `1`,
LEQUAL `3`, GREATER `4`, NOTEQUAL `6`, GEQUAL `5`, ALWAYS `7`.  With depth
enabled and writes disabled, the five added words are `00380381`, `00380385`,
`00380391`, `00380399`, and `00380395` for NEVER, EQUAL, GREATER, NOTEQUAL,
and GEQUAL respectively.  Together with LESS/LEQUAL/ALWAYS above, this is a
complete live-validated depth comparison table.

| Stencil case | `a8` | `a9` |
|---|---:|---:|
| always, ref 3, mask ff, keep/keep/replace, write ff | `003903aa` | `08ffff03` |
| equal, ref 5, mask 7f, keep/incr/incr, write 3f | `003ec0aa` | `083f7f05` |
| notequal, ref 12, mask 5a, invert/decr/zero, write a5 | `003beb2a` | `08a55a12` |

`a9` is exactly `0x08WWCCRR` for these cases: `WW` is the write mask, `CC` the
compare mask, and `RR` the reference.  Word `a8` carries enable, compare, and
the three operations.  The added isolated comparison matrix gives bits 9:7 as
NEVER `0`, LESS `2`, EQUAL `1`, LEQUAL `3`, GREATER `4`, NOTEQUAL `6`, GEQUAL
`5`, ALWAYS `7`.  The added sfail matrix gives bits 12:10 as KEEP `0`, ZERO
`3`, REPLACE `1`, INVERT `2`, INCR_WRAP `4`, DECR_WRAP `5`, saturating INCR
`6`, and saturating DECR `7`.  The same three-bit operation encoding applies
to zfail bits 15:13 and zpass bits 18:16, as confirmed by the earlier mixed
samples and the static packer.

`GL_ATI_separate_stencil` is advertised and both entry points resolve.  In
case 85, `glStencilFuncSeparateATI(GL_LESS=0201, GL_GREATER=0204, 0x31, 0xa7)`
returns `GL_INVALID_ENUM`; case 86 passes `GL_GREATER=0204, GL_LESS=0201` and
gets the same error.  Case 85 applies FRONT `(KEEP=1e00, INCR=1e02,
REPLACE=1e01)` and BACK `(INVERT=150a, DECR=1e03, ZERO=0)` operations; case 86
swaps those two triples.  All four `glStencilOpSeparateATI` calls return no
error and emit distinct two-face words `a8=1c3bebea` and `a8=3eb9c3ea`.
Treat separate ops as live-supported and the vendor separate-function API as
advertised but broken; do not copy its error into a new implementation.

## Cull and shading

Cull back with CCW front faces produces `ef=03000000`; cull front/CCW and cull
back/CW both produce `02000000`, as expected for the same effective triangle
rejection after reversing the front-face convention.  Baseline culling off is
zero.

Flat shading changes the last `22` word from `00002503` to `00006503` and sets
bit 31 in the two `23` words `00100d88` and `001006b6`.  This is sufficient to
implement the captured smooth/flat switch.  Line and point polygon modes were
only present in the independent-process set and had no selected draw-state
differential; they remain open until a same-context pair is captured.

The follow-up matrix closes the direct line and point paths.  Polygon line
sets `22=00042503` and `f0=c8008400`; width 3 adds `f0=cc008400,f4=3`.
Line smoothing adds `ef=60000000`.  Polygon point sets `22=00042603`,
`c1=00001fff`, `f0=c2008400`, and `f4=1`; size 5 changes `f4` to `5`.
Point smoothing uses `c1=00007fff,ef=00000600`.  A real `GL_POINTS` point
sprite with coordinate replacement ran without error and emitted
`22=00006500,f0=c2008400,f4=20` for the requested 32-pixel point.

## Bounds, fragment modes, and generated state

Scissor `(17,23 101x79)` emits `c4=f5=009a0011` and
`c5=f6=00e90076`.  Viewport `(13,19 171x149)` emits
`c4=f5=0058000d` and `c5=f6=00ed00b8`.  The Y values are the expected
bottom-to-top conversion in the 256-pixel drawable.  Depth range `.25,.75`
materializes float `.25` at resource offset `688` and `.5` at `698` in this
orthographic setup.  Polygon offset fill emits `ef=04000000`; its numeric
factor/unit lowering remains generated state and needs a value matrix.

Reverse subtract, MIN, MAX, and constant-color blending retain the common
direct enable words `88=0110ff03,f0=c5008200`; the constant-color resource
starts with floats `.2,.4,.6,.8` at `600`.  Separate RGB/alpha blending sets
bit 7, `88=0110ff83`.  Factors and equations remain program-derived.  Logic
XOR emits `88=01180000,ef=000000aa`, an exact single-op implementation seed.

Linear, exponential, and exponential-squared fog all use
`22=00002703,88=0910df03` and differ in generated program/constants.  The
explicit `GL_EXT_fog_coord` fixture selects `GL_FOG_COORDINATE_EXT`, supplies
per-vertex values `.1,.5,.9`, and completes with `GL_NO_ERROR`.  Clip plane 0
also completes without error and changes generated constants/program state.
Two-sided lighting emits `22=00002703,88=03109fe3` plus generated state.

The captured legacy visual behaved as unsampled.  Enabling multisampling and
calling sample coverage `.5` with invert both complete without error but leave
the selected direct state and resource byte-identical.  These cases prove API
acceptance only; a sampled FBConfig is required to assign `ad/af` sample bits.

## Driver rules

Emit sampler and fragment/raster state in the B setup position before the draw,
then retain it as context-owned hardware state.  Do not treat `1b`, the high
half of `ab`, or the resource-tail host pointers as state: they vary between
otherwise identical processes.  The command validator must allow the proven
indices while relocating all address-bearing words.

Subsequent `parhelia-m9-texture-resource-capture.md` evidence closes 3-D R
repeat and the signed/clamped LOD boundaries: bias -2, both saturation ends,
minimum LOD 3, and maximum LOD 2 all produce the expected visible level.  The
special/core GL2 captures also prove the corresponding generated-state
resource is selected through tagged index `6c`; index `69` binds the external
fixed-fragment database.  The old address-directed BAR snapshot is an opaque
alias with unresolved translation and must not be treated as drawable backing
or a directly mapped program heap.

Remaining fixed-function capture work is a sampled FBConfig, the fifteen logic
operations beyond XOR, polygon-offset factor/unit lowering, the exact border
color packing, and native-record joins for blend, alpha, fog, clip, and
lighting.  The static packer already names most generated equivalence classes;
targeted visible-result cases should validate those tables rather than repeat
direct state that this matrix has closed.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
