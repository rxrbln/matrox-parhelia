# What video decode and encode the Matrox Windows drivers actually carry

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Asked because a video engine, if one existed, would be worth exposing through
VA-API or Gallium's video interface.  The answer is short: **there is no video
encode anywhere in any saved Matrox driver, and the M-series - our M9128 - has
no video decode either.**  Only the older Parhelia family carries decode, and
only MPEG-2 motion compensation.

Labels: **STATIC** is read out of a vendor binary at a named offset;
**DERIVED** is a join of static facts, with the join given.  Nothing here was
run on hardware, and nothing here needs to be.

Re-derivable with `tools/m9-video-decode-evidence.py`.

## 1. No encode, in any driver, in any generation

No encode GUID, no encode entry point and no encode string occurs in
`MxMD.dll`, `MxMDI.dll`, `MxMM.sys`, `MxMGLX.dll`, `MTXPARD.dll`,
`MTXPARM.sys`, `mtxpserd.dll`, `g5dhd.dll`, `g400dhd.dll` or `g200disp.dll`
[STATIC].  This is unsurprising for the generation and is recorded so it is
not asked again.

## 2. The M-series has no DXVA decode at all

The DXVA 1 mode GUIDs all share the tail `{...-a0c7-11d3-b984-00c04f2e73c5}`.
Scanning every file of the saved XDDM package for that tail finds it in
exactly two binaries, and neither is an M-series one [STATIC]:

| binary | family | DXVA GUIDs found |
|---|---|---|
| `MtxPar/MTXPARD.dll` | Parhelia | `MPEG2_A`, `MPEG2_B`, `MPEG2_C`, `MPEG2_D`, `NoEncrypt` |
| `MtxPser/mtxpserd.dll` | P-series | the same five |
| `MSerX/MxMD.dll` | **M-series** | **none** |
| `MSerX/MxMDI.dll`, `MxMM.sys`, `MxMGLX.dll` | M-series | none |
| `g5dhd.dll`, `g400dhd.dll`, `g200disp.dll` | G550, G400, G200 | none |

`GUID_MotionCompCallbacks` (`b1290df0-fe2e-11d1-9066-00c04fd9189d`), the
DirectDraw HAL capability a driver must answer to expose motion compensation,
occurs in **no** binary of the package, neither whole nor as its `Data1`
immediate `0xb1290df0` [STATIC].  `GUID_VideoPortCallbacks`' `Data1`
(`0xefd60cc0`) occurs once in each of `MxMD.dll`, `MTXPARD.dll` and
`mtxpserd.dll`, so what these drivers do answer for is the DirectDraw
**VideoPort** - video *input* and overlay - not decode [STATIC].

## 3. What Parhelia's decode actually is

`MTXPARD.dll` holds a contiguous five-entry GUID table, sixteen bytes each,
back to back at file offset `0x18bb5c` [STATIC]:

```
0x18bb5c  1b81bed0  DXVA_NoEncrypt
0x18bb6c  1b81be0d  DXVA_ModeMPEG2_D
0x18bb7c  1b81be0c  DXVA_ModeMPEG2_C
0x18bb8c  1b81be0b  DXVA_ModeMPEG2_B
0x18bb9c  1b81be0a  DXVA_ModeMPEG2_A
```

That shape is a DXVA mode list the driver returns.  The four MPEG-2 modes are
**motion compensation profiles only**.  `DXVA_ModeMPEG2_IDCT`
(`bf22ad00-03ea-4690-8077-473346209b7e`) and `DXVA_ModeMPEG2_VLD`
(`ee27417f-5e28-4e65-beea-1d26b508adc9`) occur nowhere [STATIC], so the host
CPU does the bitstream parse and the inverse DCT and the card does motion
compensation and the colour conversion.  `NoEncrypt` is the null
content-protection GUID, not a codec.

## 4. What the M-series does have

`MxMD.dll` carries the YUV surface FOURCCs `YUY2`, `UYVY`, `NV12` and `YV12`
[STATIC], and the overlay configuration names `Overlay`, `OverlayEnable`,
`OverlayAutoFlipEnable`.  That is an **overlay and colour-conversion** path -
a scanout format and a scaler - not a decoder.  The G-series and G550 drivers
carry `YUY2`, `UYVY` and `YV12` and no `NV12`.

The three `DdVPE*` names are not a decoder either.  `DdVPEControl`,
`DdVPEStripWidth` and `DdVPEGaussSigma` are each referenced exactly once in
`MxMD.dll` [STATIC]; a strip width and a Gaussian sigma are the parameters of
an **edge-blending** filter for abutting projector outputs, which is a
product feature of this board family, not video processing [DERIVED].

`DxVaSupport` is read once, at file offset `0xaa908`, which pushes the string
at VA `0x1e76f0` into the registry-read helper at `0xab310` and stores the
result at `[edi+0x154]` [STATIC].  `DxVaSupportIndicator` is referenced **zero**
times - a dead string.  Since the binary contains no DXVA GUID of any kind,
the surviving `DxVaSupport` read gates nothing that could be advertised
[DERIVED].  This is the same pattern as the `GigaColorRender` and
`GigaColorFullScreen` keys, which `docs/parhelia-m9-gigacolor-10bit.md`
records as deleted from this driver while the silicon path remained.

## 5. Consequence for this driver

Nothing to implement.  There is no video engine on the M9 to expose through
VA-API, VDPAU or Gallium video, and no encode anywhere in the family.  The
one genuinely useful thing in this area is unrelated to decode: YUV **sampler**
formats for a compositor's video surfaces, which
`docs/parhelia-m9-hidden-feature-config-audit.md` already tracks and which is
blocked on measuring plane order, chroma siting and pitch - and, per
`docs/parhelia-m9-implementation-backlog.md`, on the fact that **sampling a
linear 2-D surface does not work on this hardware**, which is what puts NV12
plane sampling out of reach independently of format codes.
