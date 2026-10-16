# M9 open GL feature bits, closed by vendor capture

The M9148 was booted into the Matrox vendor image on 2026-09-17, so the
proprietary ICD (`Matrox ICD for M-Series`, GL 2.0, GLSL 1.10, X driver
`m9x_drv.so` 1.4.3) could be driven directly.  This note records what that
stack advertises, what it actually renders, and the register bits its command
stream carries, for the five features that were still open.

Every case renders traceable data — each texel, attribute and render target
carries a value identifying itself — and compares the readback against a
CPU-computed reference, so a wrong result is visibly wrong rather than blank.

* Probe: `tools/m9x-openbits-probe.c` (one case per process)
* Runner: `tools/m9x-openbits-run.sh`, capture payload `tools/m9x-openbits-capture.sh`
* Evidence: `captures/m9x-vendor-openbits-20260917`
* Decode: `python3 tools/m9-command-decode.py --execute CASE/*-000*-in.bin CASE/*bm*-in.bin`

`MEASURED` = rendered and checked against a reference on this hardware.
`CAPTURED` = read out of the vendor command stream.  `DERIVED` = decoded from
captured values.  `UNTESTED` = not exercised here.

## What the vendor stack advertises (MEASURED)

`GL_ARB_depth_texture`, `GL_ARB_shadow`, `GL_EXT_shadow_funcs`,
`GL_ARB_fragment_program_shadow`, `GL_ARB_draw_buffers`,
`GL_EXT_framebuffer_object`, `GL_ARB_vertex_shader`, `GL_ARB_fragment_shader`,
`GL_ARB_occlusion_query`, `GL_ATI_separate_stencil`.

Neither `GL_EXT_texture_sRGB`, `GL_ARB_texture_sRGB` nor
`GL_EXT_framebuffer_sRGB` appears.  Limits:

```text
MAX_TEXTURE_UNITS                 4     MAX_DRAW_BUFFERS                 4
MAX_TEXTURE_IMAGE_UNITS          16     MAX_COLOR_ATTACHMENTS_EXT        4
MAX_TEXTURE_COORDS                8     MAX_VERTEX_ATTRIBS              16
MAX_VERTEX_TEXTURE_IMAGE_UNITS    4     MAX_VARYING_FLOATS              60
MAX_COMBINED_TEXTURE_IMAGE_UNITS 20     MAX_*_UNIFORM_COMPONENTS       512
MAX_TEXTURE_SIZE              16320     AUX_BUFFERS                      0
```

`MAX_TEXTURE_SIZE = 16320` is the advertised value; it was not exercised.

## 1. Shadow compare and depth textures — closed

`GL_DEPTH_COMPONENT16/24/32` textures upload, sample and compare, and a depth
texture rendered by the GPU samples back complemented (see below).  The probe
builds an 8x8 depth texture whose texel `(i,j)` holds `(8i+j+0.5)/64` and draws
an 8x8 cell grid whose comparison reference sweeps a permutation of the same
range, so every one of the eight functions produces a different fully
determined pattern.  All eight are correct (MEASURED), with one boundary
qualification below.

### Format code: index `79`, bits `31:25` (CAPTURED)

| GL internal format | index `79` | bits `31:25` |
|---|---|---:|
| `GL_RGBA8` (control) | `c8010008` | `64` |
| `GL_DEPTH_COMPONENT16` | `b8010008` | `5c` |
| `GL_DEPTH_COMPONENT32` | `f8010008` | `7c` |
| `GL_DEPTH_COMPONENT24` | `fa010008` | `7d` |

`64` is `A8B8G8R8` in the statically recovered descriptor table, which is what
`GL_RGBA8` must map to, so the field position is confirmed by the control as
well as by the three depth codes.  These three are exactly the depth codes that
`parhelia-m9-texture-state.md` recovered from the Windows descriptor
initializer (`D16=5c`, `D32=7c`, D24 family `7d`) but could not prove were
sampleable.  They are now proven sampleable, with comparison, through GL.

The remaining fields of index `79` were identical across all four formats for a
given texture size (`0x...010008` for 8x8, `0x...010001` for 256x256), so only
bits `31:25` carry the format.

### Compare enable and function: index `0x70 + unit` (CAPTURED)

Sampling a depth texture with `GL_TEXTURE_COMPARE_MODE = GL_NONE` emits exactly
the same index-`70` word as an RGBA8 texture, so the depth format alone changes
nothing here.  Turning comparison on sets two fields:

```text
plain 2D RGBA8 or depth-without-compare   00040118
shadow, GL_NEVER                          04040218
shadow, GL_LESS                           0c040218
shadow, GL_LEQUAL                         14040218
shadow, GL_EQUAL                          1c040218
shadow, GL_NOTEQUAL                       24040218
shadow, GL_GEQUAL                         2c040218
shadow, GL_GREATER                        34040218
shadow, GL_ALWAYS                         3c040218
```

* **bit 26 (`0x04000000`) enables shadow comparison** — set in all eight
  comparing cases, clear in both non-comparing cases.
* **bits `29:27` select the function**, with hardware codes
  `NEVER 0, LESS 1, LEQUAL 2, EQUAL 3, NOTEQUAL 4, GEQUAL 5, GREATER 6,
  ALWAYS 7` (DERIVED from the table above).
* **bits `11:8` change from `1` to `2`** for a comparing fetch.  `1` is the
  plain 2D texture-operation code; `2` is what the three-component
  (`shadow2D`) lookup selects.

This confirms, on Linux and through GL, the enable bit and the function
encoding that `parhelia-m9-texture-state.md` had derived only from the Windows
`0x18bf38` lowerer (its D3D-ordered table `NEVER LESS EQUAL LEQUAL GREATER
NOTEQUAL GEQUAL ALWAYS -> 0 1 3 2 6 4 5 7` is the same hardware mapping).  The
sampler-state question the audit left open — which frontend state sets sampler
`+0x46 bit 8` and `+0x08 bit 13` — does not need answering for a new driver:
the two hardware fields are now known directly.

Index `7c` carried no shadow-specific bit.  Its only difference from the RGBA8
control (`00009200` vs `02409200`) is the wrap mode the probe selected, which
independently confirms the `ADDRESSU` field at bits `24:22` and `ADDRESSV` at
`27:25` with `CLAMP = 1`.  One unexplained detail: `GL_DEPTH_COMPONENT16`
*without* comparison also sets bit 31 (`82409200`), which the 24-bit and
32-bit formats and all comparing cases leave clear.  UNTESTED what it does.

Index `7a` (`00010008`) and `7d` (`0ff80000`) were identical everywhere.

### Depth texture mode (MEASURED)

`GL_LUMINANCE` and `GL_INTENSITY` render identically and emit byte-identical
command streams — expected, since the comparison result lands in `.r` for both.
`GL_ALPHA` is broken in the vendor ICD: `shadow2D(...).r` returns a constant
`1.0` for every cell, and the selected submission binds no texture block at
all (index `70` stays zero).  A driver should treat `GL_ALPHA` depth mode as a
software concern, not a sampler bit.

### Comparison at exact equality (MEASURED)

In all eight cases the only deviations from the reference were the four cells
where the probe makes the reference exactly equal to the stored depth *and* the
value is in the upper half of the range (cells `(4,4)` through `(7,7)`, depths
`0.57` to `0.99`; the equal cells `(0,0)` through `(3,3)` are correct).  The
sign of the error is consistent across every function: the interpolated
reference compares strictly greater than the stored texel.  So the comparison
logic is exact and the interpolated `r` coordinate loses precision against the
stored depth as the value grows.  Shadow mapping should not rely on exact
equality, which is normal practice anyway.

### A depth buffer rendered by the GPU stores `1 - z` (MEASURED)

The cases above upload the depth texture from the CPU.  The real shadow-map
path renders it, and there it behaves differently.  `shadowmap_*` attaches a
`GL_DEPTH_COMPONENT16`/`24` texture to a framebuffer, renders an 8x8 grid of
quads at a scrambled per-cell window depth, then binds that texture and samples
it.  Sampling it *without* comparison and reading the value back gives, over
all 64 cells and both formats:

```text
max |sampled - (1 - written)| = 0.0029      (8-bit readback quantization)
max |sampled - written|       = 0.9844
max |sampled - rotated(i,j)|  = 0.7813
```

**The stored depth is the complement of the window depth.**  The spatial layout
is untouched — a 180-degree rotation, which for a linear ramp is
indistinguishable from complementing, is excluded by the scrambled pattern.
The same complement explains the comparison results: with comparison enabled on
a rendered depth texture, 30 of the 31 cells where plain and complemented
storage disagree came out flipped, the one remainder being the equality
boundary cell described above.

This is invisible to ordinary depth testing, because the incoming fragment
depth is complemented too and the comparison sense is preserved, but it is
exactly visible to a shadow sampler.  A Mesa shadow-map implementation must
account for it — by complementing the comparison reference, by rendering the
shadow map with a reversed depth range, or by mirroring the comparison function
— otherwise the shadow comes out inverted while looking plausible.
UNTESTED which of the three the vendor stack itself uses, and whether the
complement is in the depth unit or in the ICD's viewport z mapping.

### What a driver needs

Allocate the depth texture, put `5c`/`7c`/`7d` into index `79` bits `31:25`,
set index `0x70 + unit` bit 26, bits `29:27` to the function code and bits
`11:8` to `2`, and emit an ordinary texture fetch.  When the depth texture was
rendered rather than uploaded, complement the comparison reference as well.

## 2. More than four fragment samplers — closed

Both the dependent and the independent case were measured, and they behave
differently.  All of them render exactly right (MEASURED): `samplers_1` through
`samplers_16` (a 16-deep dependent pointer chase through 16 distinct textures)
and `indep_2` through `indep_8`, plus `taps_4/9/16/25/49` (one sampler, up to
49 taps of a box kernel).

The command stream shows how (CAPTURED; "draws" counts raw draw records in the
selected submission, one of which is the clear):

| case | scene passes | slot `70` | `71` | `72` | `73` |
|---|---:|---:|---:|---:|---:|
| `indep_2` | 1 | 1 | 1 | 0 | 0 |
| `indep_3` | 1 | 1 | 1 | 1 | 0 |
| `indep_4` | 1 | 1 | 1 | 1 | 1 |
| `indep_5` | 2 | 2 | 2 | 1 | 0 |
| `indep_6` | 2 | 2 | 2 | 2 | 0 |
| `indep_8` | 3 | 3 | 3 | 2 | 0 |
| `samplers_4` | 4 | 4 | 0 | 0 | 0 |
| `samplers_5` | 5 | 5 | 0 | 0 | 0 |
| `samplers_9` | 9 | 9 | 0 | 0 | 0 |

So:

* **Each extra block is a framebuffer pass, not a texture phase.**  Every pass
  renders to the scratch surface `0x10ec1000` (index `b0`) and the last pass
  writes the real target; the next pass binds the scratch surface back through
  index `78`.  This is the multipass lowering that
  `parhelia-m9-five-sampler-capture.md` first saw with five samplers, now
  measured up to sixteen.
* **The maximum safe single-pass schedule is four independent fetches**, in the
  four physical slots `70..73`.  `indep_4` issues all four in one pass.
* **A continuation pass carries at most three application textures**, because
  one slot holds the previous pass's scratch result.  The pass count for `N`
  independent fetches is `1` for `N <= 4` and `ceil(N/3)` above that — measured
  `5 -> 2`, `6 -> 2`, `8 -> 3`.
* **A fully dependent chain costs one pass per sampler**, because each fetch
  needs the previous result as its coordinate.  That is a data-dependency
  limit, not a slot limit, and the earlier reading of the five-sampler capture
  as "all application textures time-slice full slot `70`" is the dependent
  case only.

This unblocks the glmark2 scenes that motivated the question: `terrain` (six
mostly independent samplers) needs two passes.

**Correction (2026-09-17).**  This paragraph also claimed that `effect2d` at 9
and 25 taps "needs none at all — many taps of one sampler stay in one pass,
which `taps_49` confirms".  That pass count is not supported by anything saved
here.  What the `taps_*` cases establish is only correctness:
`result-matrix.txt` has `taps_4/9/16/25/49 PASS checked=64 mismatches=0`
(MEASURED).  No `cap-taps_*` submission was captured, the table above has no
`taps_*` row, and the number of raster draws the vendor used for them was never
observed.  `tools/m9x-openbits-probe.c` builds those shaders as
`a += texture2D(s, uv + vec2(dx, dy) / TS)`, a distinct coordinate per tap,
which is an independent fetch by the same rule as `indep_*`.  Until a `taps_*`
submission is captured, budget many taps of one sampler as `ceil(N / 3)` like
any other independent fetch set.  The split contract a scheduler must emit is
in `parhelia-m9-fragment-pass-splitting.md`.

## 3. sRGB sampler decode — closed, negative

The live vendor ICD advertises no sRGB extension of any kind, so there is no GL
path to probe and no sampler bit to find.  This agrees with the static closure
in `parhelia-m9-srgb-format-static-closure.md`, which found the Direct3D
`D3DSAMP_SRGBTEXTURE` transport reaching sampler shadow `+0x46 bit 5` with no
consumer in the final lowerers.  **There is no evidence that this hardware
decodes sRGB in the sampler**, and the vendor's own GL never tries.  Mesa
should keep sRGB decode as a shader-side or format-side concern.

## 4. MRT 1..3 — closed functionally

`GL_ARB_draw_buffers` with one, two, three and four colour attachments all
render and read back exactly (MEASURED).  Each fragment writes
`gl_FragData[k]` with a blue channel of `0x11 * (k + 1)` and red/green encoding
the cell, and every attachment was read back with `glReadBuffer` and compared
per cell: 64, 128, 192 and 256 checks with zero mismatches.  Destroying the
framebuffer and its textures and building them again, then drawing and reading
again, also passes for two and four targets (`mrt_2_rebuild`, `mrt_4_rebuild`).
No hang, no error, and the box stayed up for the rest of the session.

The command stream shows the four output pairs (CAPTURED).  The draw range of
each case writes exactly one address/descriptor pair per bound target:

```text
mrt_1   b0=120c9000 b1=c8210100
mrt_2   b0=12111000 b1=c8210100   b2=120c9000 b3=c8210100
mrt_3   b0=12159000 b1=c8210100   b2=12111000 b3=c8210100
        b4=120c9000 b5=c8210100
mrt_4   b0=121a1000 b1=c8210100   b2=12159000 b3=c8210100
        b4=12111000 b5=c8210100   b6=120c9000 b7=c8210100
```

`c8210100` has the same bits `31:25` (`64`, `A8B8G8R8`) as the RGBA8 sampler
descriptor, consistent with one descriptor encoding for both roles.

The attachment textures were allocated in ascending address order
(`120c9000`, `12111000`, `12159000`, `121a1000` for attachments 0..3, visible
as the `f0210100` allocation writes earlier in the same submission), so the
pairs appear in **reverse attachment order**: pair `p` holds attachment
`N-1-p`.  Since the per-attachment readback is correct, the vendor either
numbers the hardware pairs downwards or permutes the fragment outputs to match;
this capture cannot separate those two (DERIVED, ambiguous).  A new driver must
verify its own output-to-pair mapping with a per-target probe like this one
rather than assume `b0` is attachment 0.

This does not by itself lift the Linux teardown concern recorded in
`parhelia-m9-driver-readiness.md` — that was a no-read mask-15 case in the
resource-close poll, and every case here reads every target back.  What is now
established is that four simultaneous targets render correctly, survive a
destroy/rebuild cycle, and use pairs `b0/b1` through `b6/b7`.  The remaining
risk is specifically a target that is written and never read.

## 5. Vertex attributes 3 and above — closed

With position travelling through `gl_Vertex` and the remaining attributes
declared as generic `attribute vec4`s fed from buffer objects, the vertex
shader checks each attribute against its declared value `float(k) + id*0.001`
in `x` and its negation in `y`, and reports how many matched.  Every attribute
count from two to fifteen returns all attributes correct over all 64 cells
(MEASURED):

```text
attribs_2 .. attribs_15   PASS, checked=64 mismatches=0
attribs_16                link fails, GL_ACTIVE_ATTRIBUTES=0, empty info log
```

So **fifteen simultaneous vertex attributes work** — `gl_Vertex` plus fourteen
generic ones at locations 1..14 — and the sixteenth, which
`MAX_VERTEX_ATTRIBS = 16` advertises, fails to link.  Attribute 3 and every
attribute up to 15 is therefore verified per attribute and per value, which
closes the question `parhelia-m9-native-vertex-isa.md` left open ("attribute 3
is CAPTURED only... upper limit UNDECODED").  Our own probe's earlier failure
at attribute 3 was a probe problem, not a hardware limit.

Two mechanical findings from the same runs:

* **`glBindAttribLocation` crashes the vendor ICD.**  Every case that bound its
  own attribute locations before linking died with SIGSEGV inside
  `glLinkProgram`, at any attribute count including two.  Querying locations
  with `glGetAttribLocation` after linking works, and the ICD then assigns
  locations `1, 2, 3, ...` in declaration order.
* **Client-side generic attribute arrays crash it too**; the arrays must live
  in buffer objects.

### Declaration stream: index `23` (CAPTURED)

The vertex preprocessing declaration is a run of index-`23` words terminated by
`00004b08` and padded with zeros to a four-word boundary, followed by a
constant eight-word epilogue block (`00100d88 001006b6 00010688 00c231b6
00d30688`).  For N attributes:

```text
N=3   00100d88 00110236 00220dac 00220236 00004b08
N=4   00100d88 00110b36 00220d88 00210236 00330d88 00004b08
N=6   00100d88 00110b36 00220d88 00210236 00330d88 00340236 00450d88 00004b08
N=8   00100d88 00110b36 00220d88 00210236 00330d88 00340236 00450d88 00460236
      00970d88 00004b08
N=14  00100d88 00110236 00220d88 00230236 00004b08                    (block 1)
      00100d88 00110b36 00220d88 00210236 00330d88 00340236 00450d88 00460236
      00970d88 00980236 00a90d88 00aa0236 00004b08                    (block 2)
```

The block is **not** capped at eight words: it grows with the attribute count
and splits into several terminated blocks when it gets long.  The earlier note
that "the preprocessing pass has room for eight index-`23` words, so three
attributes fit and four do not" describes the one shipping stream that had been
captured, not a hardware limit.

The word format is only partly decoded.  Entries alternate between a `0d88`
form and a `0236`/`0b36` form, and the byte at bits `23:16` walks
`10, 11, 22, 21, 33, 34, 45, 46, 97, 98, a9, aa`.  The low nibble of that byte
runs `0,1,2,1,3,4,5,6,7,8,9,a` and the high nibble `1,1,2,2,3,3,4,4,9,9,a,a`.
**UNDECODED**; the experiment that would settle it is dumping the compiled C2
vertex program alongside these words (add `M9XTRACE_RES=c2:0x...` and
`tools/m9-vendor-vram-dump.py` to the capture payload) for the same attribute
counts, and joining each declaration entry to the attribute register the
program reads.

## Reproduction

On a host with the vendor image booted, from `xf86-video-parhelia`:

```sh
scp tools/m9x-openbits-probe.c tools/m9x-openbits-run.sh root@TARGET:/root/tmp/
ssh root@TARGET 'nohup setsid /root/tmp/m9-x-run.sh openbits \
        /root/tmp/m9x-openbits-run.sh &'          # then poll for .sentinel
```

`tools/m9-x-run.sh` takes `/root/tmp/m9-hw.lock`, starts the vendor X server,
runs the payload and always kills X in the same script.  The capture variant
additionally preloads `tools/m9x-exec-trace.c`, built with `-std=gnu99`.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
