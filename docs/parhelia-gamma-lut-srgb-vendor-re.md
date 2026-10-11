# Parhelia display LUT and sRGB vendor-driver audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a static audit of the saved Matrox Linux and Windows drivers.  It
separates the display-controller gamma path from texture/render-target colour
space conversion.  Addresses are virtual addresses printed by `objdump`.

## Inputs

| artifact | SHA-256 |
|---|---|
| `matroxdriver-x86_64-cvs-20090511/xserver/7_0_0/mtx_drv.so` | `4be520bcd5c90dd040bf5b0c64650190f58cb3eb4e5348bc9e64b59488343c63` |
| `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` | `84df9a523ecb9d643d78d7e32ea291e30ab85c05c9db5d9b2c94735241b728be` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll` | `41b2317309801a917506c29c2b5998713ddb9bdfa3d26c623ab56617d0c1d848` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARM.sys` | `128f8ec1d52346e0e33d4c9d8deb73b741fb09c6f831d2df5d55d4a0795154e5` |

Evidence terms: **[BIN]** is saved binary code or data, **[VBIOS]** is the
independent ROM trace in `parhelia-vbios.md`, and **[OPEN]** means the static
corpus does not establish the feature.

## 1. The native DC LUT takes the high ten bits of each 16-bit component

The symbolized Linux display driver removes an ambiguity left by the VBIOS.
`HSLPARSetPrimaryLUT` at `0xcc690` selects the native register window at
`0xcd000..0xcd029`:

```
index = DC + 0x38
red   = DC + 0x3c
green = DC + 0x40
blue  = DC + 0x44
component_mask = 0x0000ffc0
```

At `0xcd085..0xcd0e7` it writes zero to the index once and then, for all 256
entries, writes `red[i] & 0xffc0`, `green[i] & 0xffc0`, and
`(blue[i] & 0xffc0) | 0x80000000`.  The blue write latches the entry and
auto-increments the LUT index.  Thus a DRM/X 16-bit component maps as:

```
native_lut_word = component16 & 0xffc0
```

All ten high input bits are significant.  Shifting the high byte left by ten
is incorrect: it discards two bits and places the remaining value four times
too high.  The VBIOS result remains consistent because its source is an 8-bit
VGA component which it first shifts left by ten; that produces the same bit
positions as a 16-bit X ramp shifted right by zero and masked with `0xffc0`.

The alternative `SelectDC()==2/3` path uses registers `DC+0x1b8..0x1c4` and
mask `0xff00` (`0xcc712..0xcc763`).  That is an output-specific eight-bit path,
not the native DC0 gamma LUT used by the P690 KMS path.

`MTXChangeGammaRampCrtc` at `0x4e6a0` supplies another cross-check.  It accepts
256 ten-bit ramp elements, shifts each left by six into a 16-bit intermediate,
and calls `MTXSetColorLUT`; `HSLPARSetPrimaryLUT` later applies `0xffc0`.

The Windows miniport independently contains the same implementation.  In
`MTXPARM.sys`, the routine at `0x48520` selects `0x1838/0x183c/0x1840/0x1844`
and mask `0xffc0` at `0x48592..0x485af`; its loop at `0x485f9..0x48658`
writes 256 masked RGB triplets and ORs bit 31 into blue.  This is direct
cross-platform confirmation of both the register addresses and component
placement.

## 2. Standard scanout and GigaColor use different LUT programming modes

The ordinary 8-bit-per-channel scanout path uses 256 physical RGB entries,
each with a ten-bit output value.  It simply preserves the high ten bits as
described above; it does not add slope/interpolation metadata.

The vendor has a separate path for a 10-bit-per-channel scanout input.
`Program10BitPrimaryLUT` at `0xcbbf0` still starts from a 256-entry RGB ramp,
but normalizes, smooths and remaps it before programming the hardware:

* `0xcbc82..0xcbc8d` writes zero once to `DC+0x38`.
* `0xcbcb6..0xcbd23` emits 255 interval entries to `DC+0x3c/0x40/0x44`.
* `0xcbd34..0xcbda4` emits the final RGB entry.
* Every blue value is ORed with bit 31, which latches and advances the entry.
* `0xcbde5..0xcbe64` writes six compensation words at
  `DC+0x0c, +0x10, +0x14, +0x18, +0x1c, +0x20`.

`Compute10BitLutEntry_us256` at `0xcb120` proves the packed interval format.
Bits `[15:6]` contain the ten-bit base output and bits `[18:16]` contain a
three-bit slope/compensation selector derived from the difference to the next
control point.  `Compute10BitLastLutEntry_us256` at `0xcb1d0` constructs the
terminal entry.  `DC+0x00` bit 10 is then selected from the recovered gamma
level (`0xcbdad..0xcbde0`).

This is how a 256-entry physical LUT approximates a mapping for a 10-bit input.
Those interpolation fields are specific to the 10-bit scanout mode.  They must
not be generated for the normal 8-bit framebuffer's DRM gamma property.

The Windows miniport again matches: `MTXPARM.sys 0x47b42..0x47d3d` programs
the interval form through `0x1838/0x183c/0x1840/0x1844`, toggles `DC+0x00`
bit 10, and begins writing the same compensation-register range at `0x180c`.

## 3. The colour-enhancement LUT is also an output stage

The driver's `HSLPARSetCEMLUT` at `0xca5c0` programs 1024 values through
`HSLPARAccessCEM_2FPgasI2CCachedWrite`, after calling
`HSLPARSetPrimaryLUTBypass`.  `HSLPARSetCEMLUTBypass` at `0xca3e0` manipulates
the external CEM through I2C.  `HSLPARApplyLUT` chooses between this block and
the primary LUT according to the attached output and display mode.

Both LUT implementations are downstream of framebuffer storage and 3D
rendering.  A raw VRAM or framebuffer capture cannot contain either one's
transfer function.

## 4. No vendor texture or framebuffer sRGB conversion was found

Neither saved OpenGL implementation advertises `GL_EXT_texture_sRGB`,
`GL_EXT_framebuffer_sRGB`, `GL_ARB_framebuffer_sRGB`, or a corresponding GLX/WGL
extension.  The Linux binary's advertised texture list stops at the ordinary
texture, S3TC, cube-map, environment, and anisotropy extensions.  The Windows
ICD has the same relevant boundary.  The extension header shipped beside a
binary contains sRGB token declarations, but the executable extension string
does not.

The stronger executable-token check agrees:

* The Linux `libGL.so.1.3.0` contains no little-endian dword occurrence of
  `0x8c40`, `0x8c41`, `0x8c42`, `0x8c43`, `0x8c48`, or `0x8c49`.
* `MTXOGL.dll` contains none of `0x8c41`, `0x8c42`, `0x8c43`, `0x8c48`,
  `0x8c49`, `0x8db9`, or `0x8dba`.  Its two byte occurrences of `0x8c40` are
  instruction-address/displacement bytes, not a GL-enum comparison or format
  table entry.
* No captured vendor command stream binds a texture or render-target state that
  has been tied to an sRGB decode/encode control.

The Windows display driver's `AlphaTextGamma` registry name is not contrary
evidence.  At `MTXPARD.dll 0x10b587..0x10b5a5` it is read from the
`DisplayDriver` registry group with default decimal 20 and stored in the
driver's configuration object.  The name alone identifies no texture format,
3D register, transfer function, or sRGB state.

Therefore the saved vendor implementations provide **no evidence** for a
fixed-function texture sRGB decoder, render-target sRGB encoder, or
linear-domain sRGB blending.  This is a vendor-software support result, not
proof that no unpublished hardware mechanism exists.  Mesa must keep sRGB
texture and render-target formats unsupported until a controlled register or
command-stream delta identifies such a mechanism and readback verifies the
transfer curve.

## 5. Implementation consequences

* A 256-entry DRM gamma LUT should program `component16 & 0xffc0` per channel,
  with bit 31 only on the blue write.
* The identity ramp is `i * 257 & 0xffc0`; this reaches `0xffc0` at entry 255.
* The output LUT must not be applied to rendered pixels in Mesa.  Doing so
  changes framebuffer contents and causes double correction on physical output.
* Do not advertise Gallium sRGB sampler or render-target support based on the
  display LUT.  The LUT has no per-texture decode state and is after blending,
  scanout selection, and framebuffer capture.
* GigaColor's `0x34` XRGB2101010 surface representation does not imply sRGB.
  Its special LUT interpolation mode is a scanout-input precision feature.

## 6. Reproduction commands

```sh
nm -D --defined-only \
  matroxdriver-x86_64-cvs-20090511/xserver/7_0_0/mtx_drv.so | \
  grep -E 'PrimaryLUT|10Bit|ApplyLUT|CEMLUT|GammaRamp'

objdump -d -Mintel --start-address=0xcbbf0 --stop-address=0xcbe78 \
  matroxdriver-x86_64-cvs-20090511/xserver/7_0_0/mtx_drv.so

objdump -d -Mintel --start-address=0xcc690 --stop-address=0xcd0ef \
  matroxdriver-x86_64-cvs-20090511/xserver/7_0_0/mtx_drv.so

objdump -d -Mintel --start-address=0x4e6a0 --stop-address=0x4e71c \
  matroxdriver-x86_64-cvs-20090511/xserver/7_0_0/mtx_drv.so

objdump -d -Mintel --start-address=0x47b42 --stop-address=0x47d40 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARM.sys

objdump -d -Mintel --start-address=0x48520 --stop-address=0x48662 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARM.sys

strings -a matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0 | \
  grep -E 'GL_(ARB|EXT)_'

strings -a xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll | \
  grep -E 'GL_(ARB|EXT)_|WGL_(ARB|EXT)_'

objdump -d -Mintel --start-address=0x10b570 --stop-address=0x10b5b0 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
```
