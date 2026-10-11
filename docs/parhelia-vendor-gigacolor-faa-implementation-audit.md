# Vendor GigaColor / FAA implementation audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a local, static audit of the saved Matrox drivers.  It separates
hardware facts from names in registry tables and from experiments which still
need a separately recoverable target.  It does not enable either feature.

Evidence terms: **[BIN]** is a disassembled saved binary, **[SRC]** is supplied
vendor source, **[DOC]** is supplied vendor documentation, and **[OPEN]** means
that the saved material does not provide a safe programming value.

Input artifacts, identified so that this result is reproducible:

| artifact | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |
| `matroxdriver-x86_64-cvs-20090511/kernel/src/MtxParhlParms.c` | `0ee330c2b27cfc8845c149c0a9caaee91e77c5d3959971db4978cb0ce3d6d122` |

The relevant Windows addresses below are loaded virtual addresses as
printed by `objdump -d -Mintel`.  `MTXPARD.dll` has image base `0x00010000`.

## 1. GigaColor: proven surface representation

The supported vendor display interface is depth 30: the Linux advanced readme
says to set both `DefaultDepth 30` and `Depth 30` [DOC
`docs/readme-advanced.txt:441-457`].  That establishes a display/surface
feature.  It does not establish a sampler format.

The surface configuration shape is already recovered:

```
cfg = width_pixels | (type << 16) | (format << 24)
```

Windows code selects format code `0x34` for the 30-bpp surface case [BIN
`MTXPARD 0x1dc67..0x1dcf3`].  The complete bpp helper classifies code `0x34`
as 32 bpp [BIN].  A second state-construction path tests `(cfg & 0x3f000000) ==
0x34000000` and replaces the ordinary RGB masks by `0x3ff00000`, `0x000ffc00`,
and `0x000003ff` [BIN `MTXPARD 0x152b8a..0x152bcd`].  The stored word is
therefore exactly:

```
31       30 29                  20 19                  10 9                   0
+-----------+----------------------+----------------------+---------------------+
|    X2     |         R10          |         G10          |        B10          |
+-----------+----------------------+----------------------+---------------------+
```

For a raw clear/readback test, the canonical little-endian dwords are red
`0x3ff00000`, green `0x000ffc00`, blue `0x000003ff`, and black `0x00000000`.
The top two bits are **X**, not recovered alpha.  The closest DRM format name is
`DRM_FORMAT_XRGB2101010`; a Mesa/Gallium format must preserve that component
order rather than infer an A2 layout.

Format `0x36` is not an alpha GigaColor fallback.  The open driver binds it as
its working S8Z24 depth target, while the saved GL capture binds the `0x36`
descriptor in the depth slot.  It must remain `PIPE_FORMAT_Z24_UNORM_S8_UINT` /
`S8_UINT_Z24_UNORM`, never a colour or sampler format.

## 2. 10-bit texture result: no supported sampler has been recovered

The Windows D3D format decoder maps `D3DFMT_A2B10G10R10` (31) to internal id
`0x0d`, but maps `D3DFMT_A2R10G10B10` (35) to zero/reject [BIN `MTXPARD
0x83af0`].  Its border-colour packer rejects internal id `0x0d`; no supplied
stream programs a texture descriptor with `0x11e0[29:24] == 0x34`; and the
captured GL workloads have no ten-bit texture.  This static corpus provides no
vendor-confirmed sampler format, filtering rule, layout, mip behavior, or
R/B-order result for a ten-bit texture.

The fact that `0x34` is 32 bpp means that an allocation *could* use the ordinary
32-bit pitch and tile address calculation.  It is not evidence that the texture
unit accepts it.  Current Mesa emits only sampler descriptor codes `0x32` and
`0x33` in `parhelia_context.c`, which is correct while this remains untested.

Keep all `PIPE_FORMAT_*10*10*10*` texture queries false.  In particular, do not
advertise an `A2R10G10B10` texture merely because the backing dword has two
unused bits: that would promise texture alpha, blending, and alpha-test behavior
which neither vendor result proves.

## 3. FAA: recovered capability and command boundary

FAA is a fragment/coverage buffer, not normal N-colour-sample MSAA.  The vendor
Linux source calls it “sort-independent antialiasing with the fragment buffer”
[SRC `kernel/src/MtxParhlParms.c:69-76`].  The saved Windows D3D driver contains
its allocation, state and command path; the saved Linux GL ICD exposes only
multisample state and does not allocate or submit FAA.

`AAMode[3:0]` is a real selector [BIN `MTXPARD 0x58410`, `0x86f90`, `0x9fbb0`]:

| low nibble | reported D3D capability | fragment buffer |
|---:|---|---|
| 0 or 1 | 4 sample bit | no |
| 2 | 16 sample bit | no ordinary allocation |
| 3 | 16 sample bit | FAA-16x allocation path |
| 4 and above | none | no |

The Windows shipping value is `0x54`: AA disabled while retaining upper flags.
A controlled FAA capture uses `0x53` (16x/fragment selector plus the existing
`0x50` flags).  The supplied Linux module parameter defaults to `0x52`, but the
Linux vendor stack has no consumer that enables FAA.  Neither default is an
open-driver enable recipe.

The Windows driver allocates a flat VRAM fragment blob [BIN `MTXPARD
0x9fbb0..0x9fccb`, `0x9e690..0x9e809`]:

```
bytes = FragSize * max(width*height/(1024*768),
                       sqrt(width*width + height*height)/1280) * 1048576
```

`FragSize=4.0` gives 4 MiB at 1024x768, 6.67 MiB at 1280x1024 and 9.77 MiB at
1600x1200.  The allocation is capped at 8, 16 or 32 MiB by device class; on
failure it shrinks in 2 MiB steps and fails below 6 MiB.  It has no recovered
pitch, which is consistent with a private fragment-record blob rather than a
normal colour image.

The state shadow contains two adjacent FAA words [BIN `MTXPARD 0x139725`]:

| shadow offset | proven contents | hardware register |
|---|---|---|
| render block `+0xa0` | bits `[1:0]` mode, `[6:5]` compressed-mask choice, `[31:14]` fragment bytes/256 | **[OPEN]** |
| render block `+0xa4` | raw `ThresholdAA` (default `0x007fffff`) | **[OPEN]** |

The fragment allocation updates the size field at `+0xa0` with `size << 6`
[BIN `0x9e7e3..0x9e809`].  This does **not** identify an MMIO register or the
fragment-buffer base-address register.  The ordinary render stream's `0x12f8 =
0x007fe000` cannot be used as an FAA enable; it is only a matching threshold
value, and no source ties it to the FAA state-block pair.

One command packet is completely identified [BIN `MTXPARD 0x9dcc0..0x9ddec`]:

```
count=3, indices C0 AF AF:  0x1300 = 0x207; 0x12bc = tag|1; 0x12bc = tag
count=2, indices C0 C0:     0x1300 = 0x20a; 0x1300 = normal_DWGCTL  // begin
count=2, indices C0 C0:     0x1300 = 0x00a; 0x1300 = normal_DWGCTL  // end/resolve
```

Index-to-MMIO is `0x1000 + index*4`, so C0 is `0x1300` (DWGCTL) and AF is
`0x12bc` (fence tag).  The sequence is conditional on hardware class bits
`pDev+0xab50 & 0x7fc00`.  It proves P690-family eligibility in the vendor
software gate, not FAA operation on the target.  Issuing `0x20a` without the
unknown control and base-address programming is unsafe and must not be used as
a probe.

FAA changes other state: it needs a full plane-write mask; destination-alpha
selects a compressed coverage-mask route; render-target changes resolve the old
fragment state; and its active guard band drops from 32/16 to 31/15.  The
registry comments for blend/stencil resolve boundaries document constraints,
but the examined Windows build does not read those two registry values.  Treat
blend, stencil and target change as mandatory resolve boundaries until captures
prove otherwise.

## 4. Current open-driver boundary

The current P690 driver has exactly the conservative configuration required by
the evidence:

* KMS accepts only `DRM_FORMAT_XRGB8888` and `DRM_FORMAT_RGB565`.
* Mesa accepts sample count/storage sample count no greater than one and emits
  32-bit `0x32`/`0x33` sampler formats.
* The batch validator permits colour `0x32` and depth `0x36`; it rejects any
  other clear-surface format.

Do not weaken any of these checks to expose 10-bit texture formats, alpha on
`0x34`, FAA sample counts, GLX multisample FBConfigs, or sample buffers.
`GL_SAMPLE_BUFFERS=0` remains the only truthful answer.

## 5. Conservative implementation plan

### 5.1 GigaColor scanout, then render target

1. Add an explicit KMS depth-30 mode under an opt-in module parameter, with
   `DRM_FORMAT_XRGB2101010` mapping to surface format `0x34`.  It must not
   replace the normal `0x32` scanout path.
2. Extend the surface validator only for an isolated, named `0x34` **colour**
   test allocation.  Keep `0x36` depth-only.
3. Add direct tests before Mesa exposure: 2D clear and CPU/BAR readback of the
   four exact words above; then a WARP triangle; then an opaque blend-disabled
   draw.  Restore `0x32` scanout after every result.
4. Once target readback confirms those operations, allow
   `PIPE_FORMAT_XRGB2101010` for render targets and scanout only.  Alpha and
   destination-alpha blend remain unavailable.

### 5.2 Ten-bit texture experiment

Use a separate `0x32` render target and a 4x4 one-level candidate `0x34`
texture in the normal 32-bit layout-2 arrangement.  Render nearest-filtered
red, green, blue and grey texels and read back all four pixels.  Repeat with
linear filtering, then repeat only if necessary with layout 0.  Record exact
texture descriptor words, sampler state, and source/output pixels.

Only after every nearest and linear result agrees with an unambiguous component
mapping may Mesa expose the measured `PIPE_FORMAT_*10*10*10*` **for sampling**.
A failed result leaves all texture-format support disabled; it is not a reason
to fall back to a guessed `0x33` descriptor.

### 5.3 FAA capture and validation sequence

FAA needs a clean Windows capture from a separate known-good matching driver
stack, never the test system's normal open-driver boot path.  Capture a minimal
single-render-target scene with `AAMode=0x53`, then a matched `0x54` baseline.
Diff the streams to identify all of the following before writing an open-driver
packet:

1. MMIO index/value pair for state-block `+0xa0` and `+0xa4`.
2. Fragment blob base-address register, memory class and required alignment.
3. Exact begin, resolve, flush and teardown order around DWGCTL `0x20a`/`0x00a`.
4. Render-target switch, blend, stencil, depth, destination-alpha, and memory
   allocation failure behavior.

Add a kernel private FAA buffer object only after that capture.  It must be
VRAM-only, sized by the recovered formula and caps, zeroed before use, excluded
from scanout, and destroyed/resolved on context close.  Mesa must first expose
an environment-gated test path that creates no GLX/MSAA visual.  Validate a
single edge pixel, full resolve, target switch, blend/stencil boundaries,
teardown, and reboot.  Advertise MSAA only after each test has exact readback
coverage and a stable command stream.

## 6. Reproduction commands

These local commands reproduce the decisive packet evidence without loading a
vendor driver:

```sh
objdump -d -Mintel --start-address=0x9dcc0 --stop-address=0x9de10 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
objdump -d -Mintel --start-address=0x9e690 --stop-address=0x9e830 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
objdump -d -Mintel --start-address=0x139700 --stop-address=0x139790 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
sed -n '62,76p;205,216p;281,285p' kernel/src/MtxParhlParms.c
```

Further detailed provenance and prior capture context are retained in
`parhelia-gigacolor-textures.md`, `parhelia-antialiasing.md`, and
`parhelia-gigacolor-faa-hardware-status.md`.
