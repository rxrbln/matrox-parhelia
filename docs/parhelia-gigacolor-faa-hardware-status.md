# Parhelia GigaColor and FAA: hardware-status boundary

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is the capability boundary for the open P690 driver.  It consolidates
static evidence from the saved Matrox Linux and Windows drivers and distinguishes
what an open driver can safely expose from hardware mechanisms which still need a
controlled capture.  It deliberately does not turn either feature on.

Evidence tags: **[C]** means an instruction, packet, capture, or vendor source
has been read; **[I]** is a constrained interpretation; **[OPEN]** needs a
target-hardware experiment or a Windows FAA capture.

Canonical detailed evidence is in `parhelia-gigacolor-textures.md` and
`parhelia-antialiasing.md`.  This note supersedes their older sibling documents
where those documents infer an alpha meaning for descriptor `0x36`.

## 1. GigaColor is a scanout/render-surface format, not a proven texture format

The vendor's 30-bit surface selection is:

```
cfg = width | type << 16 | 0x34 << 24
```

`MTXPARD.dll` chooses `0x34` for 30 bpp and its complete bpp decoder treats it
as 32 bits per pixel [C].  Its RGB-mask constructor changes the normal 8-bit
masks to `0x3ff00000`, `0x000ffc00`, and `0x000003ff` when the descriptor is
`0x34` [C].  The layout is therefore:

```
31       30 29                  20 19                  10 9                   0
+-----------+----------------------+----------------------+---------------------+
|    X2     |         R10          |         G10          |        B10          |
+-----------+----------------------+----------------------+---------------------+
```

This agrees with the supplied Linux driver's `DefaultDepth 30` GigaColor
documentation [C].  The high two bits have no recovered alpha behaviour.
Depth-30 scanout must consequently be exposed as **X2R10G10B10**, with alpha
writes, alpha test, and destination-alpha blending disabled until a separate
test proves an alpha-capable colour descriptor.

`0x36` is **not** that descriptor.  The working open stack binds `0x36` as its
S8Z24 depth/stencil target, and captured vendor GL draws bind it in slot 1 as
their depth target [C].  A format-number pattern is not evidence that it can be
used as A2R10G10B10.  Do not expose `0x36` through a colour or texture format.

The Windows D3D format decoder accepts `D3DFMT_A2B10G10R10` (internal id
`0x0d`) but rejects `D3DFMT_A2R10G10B10`; its border-colour path then rejects
`0x0d` [C].  No supplied capture programs a texture descriptor with format
`0x34`, and no recovered GL workload uses a `GL_*10_*10_*10*` texture.  Thus
there is no vendor-proven API path for ten-bit texture sampling.

The only defensible first experiment is independent from a ten-bit colour
target: upload a four-colour 4x4 texture in normal 32-bpp tiled layout 2,
set its descriptor format to `0x34`, and render nearest-filtered samples into
the known-good `0x32` target.  Compare all four pixels, then repeat for linear
filtering and finally layout 0.  Mesa may advertise a `PIPE_FORMAT_*10*10*10*`
texture only after that experiment passes, and it may advertise it as a render
target only after exact packed-word clear and WARP readback tests pass.

## 2. FAA is a fragment buffer with an explicit command, not ordinary MSAA

> **Correction, 2026-09-19.**  Everything in this section is the **older
> Parhelia-512** driver and **none of it carries to the M9**.  See
> `parhelia-m9-faa-re.md` section 5 for the counted evidence:
>
> * the `FragSize * max(w*h/786432, sqrt(w*w+h*h)/1280) * 1 MiB` formula, the
>   2 MiB retry, the 6 MiB floor and the 8/16/32 MiB clamps exist only in
>   `MtxPar/MTXPARD.dll`; the M-series miniport `MxMM.sys` contains no FAA
>   string, allocator or command at all;
> * `0x1300 = 0x20a / 0x00a` is the Parhelia **Z unit's** tile-tracking mode,
>   not fragment accumulation (`parhelia-antialiasing.md` section 9a.0), and
>   index `c0` is written 558 times in the saved M9 streams, always zero;
> * **`0x12f8` is not a threshold register on the M9.**  On Parhelia-512
>   index `be` carries exactly `02800000` and `007fe000`; on the M9 the same
>   index carries a VRAM backing address, 0 of 2633 writes carry either
>   word, and no M9 register anywhere is written with `0x007fe000`.
>
> The M9's multisample state is two dwords at indices `ad` and `af` plus a
> three-bit mode field in index `ef` bits 31:29, and its storage is a
> separate buffer group of about 16.55 bytes a pixel.

The Windows D3D driver has a real sort-independent fragment-AA path [C].  It
uses a separate linear VRAM allocation, not N full colour/depth samples per
pixel.  Its size is exactly:

```
bytes = FragSize * max(width * height / (1024 * 768),
                       sqrt(width * width + height * height) / 1280) * 1 MiB
```

With the normal 4.0 setting this is 4 MiB at 1024x768, 6.67 MiB at 1280x1024,
and 9.77 MiB at 1600x1200 [C].  The Windows allocator retries in 2 MiB steps
and refuses FAA below 6 MiB; hardware-class clamps are 8, 16, or 32 MiB [C].
The fragment allocation is a flat VRAM blob with no pitch field [C].

`AAMode` is recovered [C]: low nibble 3 selects 16-sample fragment-buffer FAA;
the shipping Windows value `0x54` disables AA.  The required live mode is
`0x53` (16x selector plus fragment-buffer bit, retaining the 0x50 upper
flags).  Mode 2 reports 16 samples but has no normal fragment-buffer path;
4x modes do not allocate the buffer.

The direct, safe-to-name DMA command is also recovered [C]:

| MMIO | DMA index | value | meaning |
|---|---:|---:|---|
| `0x1300` | `0xc0` | `0x20a` | begin fragment accumulation |
| `0x1300` | `0xc0` | `0x00a` | end accumulation / resolve current target |
| `0x12bc` | `0xaf` | `tag|1`, then `tag` | required fence pair around command |
| `0x12b4` | `0xad` | command parameter | used by a sibling command only |

The Windows state shadow also proves an AA control word: bits `[1:0]` are the
enable/mode, `[6:5]` select the compressed coverage-mask form, and `[31:14]`
carry fragment-buffer size divided by 256 [C].  The adjacent dword is the raw
`ThresholdAA` setting, normally `0x007fffff` [C].  The command-stream emitter,
the actual MMIO indices for those two dwords, and the fragment-buffer base
register are still **[OPEN]**.  `0x12f8` is programmed as the normal draw's
vendor threshold value (`0x007fe000`) but is not sufficient evidence that it is
the Windows FAA control/threshold register.

FAA has mandatory constraints [C unless noted]:

* it uses the full plane-write mask; the Linux vendor comment says to suppress
  the PLNWT workaround, and PLNWT is `0x1324` [I for the hardware reason];
* destination alpha selects a compressed coverage-mask path;
* render-to-texture/special surfaces bypass fragment allocation;
* a live FAA context reduces the guardband from 32/16 to 31/15;
* changing render target resolves the old fragment buffer; blend and stencil
  changes are unsafe boundaries according to the vendor parameter comments;
* P690 class bits pass the vendor FAA capability gate, but that proves only
  driver eligibility, not success on this card.

The shipped Linux ICD and the Windows OpenGL pixel-format path expose no
multisample framebuffer: `GL_SAMPLE_BUFFERS` is zero and no sample allocation
or hardware record is emitted [C].  The open driver must keep that truthful
state until a Windows capture with `AAMode=0x53` identifies the unresolved
registers and independently validates allocation, rendering, resolve, blending,
stencil, target change, and teardown on the P690.

## 3. Current implementation decision

The kernel and Mesa drivers must retain the working `0x32` colour plus `0x36`
S8Z24 path.  They must not advertise 10-bit texture formats, FAA/MSAA FBConfigs,
or FAA sample counts.  A future capability flag may be introduced only after
the specific isolated hardware tests above pass; it must default off and never
change the normal `glxgears` or `ioquake3` path.
