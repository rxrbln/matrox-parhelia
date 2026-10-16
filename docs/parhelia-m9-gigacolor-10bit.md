# M9 native 10-bit GigaColor: render, sample and scanout

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

What a driver author needs to render into, sample from and display a
10-10-10-2 surface on the Matrox M-Series, decoded from the saved 32-bit XPDM
package.  It extends `parhelia-m9-gigacolor-faa-static-closure.md`, which owns
the resource identity, the D3DCOLOR clear packing and the multisample side;
this note adds the descriptor field, the storage class, the render class, the
write-mask behaviour and the complete display-controller answer.

Labels: **MEASURED** = read back from this silicon, **CAPTURED** = read
directly from a saved binary, **DERIVED** = a deterministic join of CAPTURED
facts, **INF** = inferred and not established.  Section 9 holds what was
measured on an M9128; where it contradicts the static reading, it wins.

`MxMD.dll`, `MxMM.sys` and `MxMDI.dll` addresses are virtual addresses.
`MxMD.dll` and `MxMM.sys` have image base `0x10000` with section RVA equal to
file offset, so VA minus `0x10000` is the file offset; `MxMDI.dll` and
`MxMGLX.dll` have base `0x10000000`.

## 1. Result

**10-bit render and 10-bit sample are real, fully constructed and safe to
build from static evidence.  10-bit scanout is present in the silicon and in
the miniport, but this driver has no path that ever selects it.**  The two are
independent: the render/sample side is reached through the resource format
code, the display side through a separate mode mask that no caller ever sets
to the 10-bit value.

| role | 10-bit reachable in this driver? | evidence |
|---|---|---|
| texture / sampler source | yes, **MEASURED** | format table, descriptor builder, D3D operation matrix (section 3); section 9.1 |
| colour render target | yes, **MEASURED** | same code, selector 8, render class 0, native clear packer (sections 3, 4); section 9.2 |
| OpenGL texture | yes, behind a device cap bit | `MxMGLX` `GL_RGB10`/`GL_RGB10_A2` (section 6) |
| OpenGL framebuffer | no | 72 static pixel formats, none above 8 bits per channel (section 6) |
| display scanout | **not selectable by the vendor driver; the silicon path is complete** | bpp-to-mode-mask table has no 30-bit input (section 5), but the scanout surface code, its descriptor packing and a ten-bit LUT ramp all exist - see below |

## 2. One 7-bit format-code space, shared by all three consumers (CAPTURED)

This is the fact that makes the rest simple.  The same 7-bit hardware format
code is packed into bits `[31:25]` of a descriptor dword in three unrelated
places:

| consumer | site | instruction |
|---|---|---|
| sampler / render-target descriptor (index `79`, `b1`) | `MxMD 0x46a35..0x46a4a` | `shl ecx,0x15` after `or ecx, code<<4` |
| render-class extractor for index `89` | `MxMD 0x18212a` | `shr ecx, 0x19` |
| display-controller surface descriptor | `MxMM 0x3691f..0x36928` | `shl edx,0x19 ; and ebx,0x01ffffff ; or ebx,edx` |

A captured descriptor byte is therefore `code << 1 | bit24`, which is why the
captured Linux sampler bytes are `0xc8` for code `0x64` and `0x68` for code
`0x34`.  It also means the measured M9 colour-surface descriptor `0xcc210140`
decodes as `0xcc210140 >> 25 = 0x66` = `A8R8G8B8` (DERIVED).

The GigaColor codes:

| Direct3D format | internal ID | hardware code | descriptor byte | R | G | B | A |
|---|---:|---:|---:|---:|---:|---:|---:|
| `A2B10G10R10` | `0x10` | `0x68` | `0xd0` | `0x000003ff` | `0x000ffc00` | `0x3ff00000` | `0xc0000000` |
| `A2R10G10B10` | `0x11` | `0x6a` | `0xd4` | `0x3ff00000` | `0x000ffc00` | `0x000003ff` | `0xc0000000` |

(from the format-record initializer arms `MxMD 0x5005d` and `0x5009a`; the
record layout and the clear packer are in
`parhelia-m9-gigacolor-faa-static-closure.md`).

Both are genuine A2 formats: the alpha mask `0xc0000000` is nonzero, unlike
the Parhelia-era `0x34` surface, which is X2R10G10B10 with no alpha.  Do not
carry the Parhelia conclusion across; `0x34` on the M9 is
`A32B32G32R32F`, 128 bits per texel.

## 3. Texture side

### 3.1 The code is the same in both roles (CAPTURED)

Descriptor builder `MxMD 0x467e0` reads the code once, from layout `+0x140`,
and writes it into `[edi+4]` bits `31:25` for **every** selector; the selector
only changes which descriptor triple at `layout + 12*selector + 8` is written
and which width/height source is used.  Selector 2 is the sampler and
selector 8 the render target, so a 10-bit resource presents the identical
format code in both roles, exactly as the depth formats do (CAPTURED +
DERIVED).

### 3.2 Storage is the ordinary 32-bit texture layout (CAPTURED)

Layout constructor `MxMD 0x45eb0` classifies the hardware code through the
switch at `0x45f6c` (byte table `0x46514`, jump table `0x464f0`).  Codes `0x68`
and `0x6a` are **not** in any of the special arms; they fall into the default
arm at `0x46004`, which sets 32 bits per texel and storage class 2 - the same
class as `A8R8G8B8` and every other packed 32-bit colour format.

The practical consequence: a 10-bit texture or render target is allocated,
tiled, padded, mip-chained and window-uploaded **exactly** like a
`B8G8R8A8` one.  Width granularity 16 texels (aperture rows) or 32
(local rows), height granularity 4, per-level alignment `0x400`, the same
class-3 address calculators `m9x474`/`m9x659`/`m9x587`/`m9x602`, the same
32x16 packed mip tail.  No new layout code is needed; only the format code and
the CPU-side pack/unpack change (DERIVED).

### 3.3 What the vendor declares (CAPTURED)

`MxMD`'s per-format Direct3D operation table gives both `A2B10G10R10` and
`A2R10G10B10` the mask `0x00d8e00f`
(`parhelia-m9-format-operation-matrix.md`): 2-D texture, volume texture, cube
texture, offscreen render target, convert-to-ARGB, offscreen plain, sRGB
sampler read, sRGB render-target write, ARGB conversion group, automatic
mipmap generation and vertex texture.  That is the same group as
`A8R8G8B8`, minus nothing.  Only the display-mode and 3-D-acceleration bits
(`0x400`/`0x800`) are absent, which is consistent with section 5.

## 4. Render target

### 4.1 Render class at index `89` bit `3*s` (CAPTURED)

The per-target three-bit class is built at `MxMD 0x182120..0x182166`:

```c
uint32_t d1  = surface_descriptor[target];   /* the 'b1'-style dword */
uint32_t idx = (d1 >> 25) - 0x30;            /* hardware format code */
uint32_t arg = (d1 >> 19) & 3;               /* dimension bits 20:19 */
uint32_t cls = (idx > 0x4a) ? 7 : class_helper(idx, arg);   /* 0x181830 */
shadow_89 = (shadow_89 & ~(7u << (3*target))) | (cls << (3*target));
```

`0x181830` is a byte table at `0x1818a8` indexed by `code - 0x30` feeding a
nine-entry jump table at `0x181884`.  The complete decode:

| class result | codes |
|---|---|
| **0** | `0x30` L8, `0x46` A4R4G4B4, `0x4a` A1R5G5B5, `0x4e` R5G6B5, `0x51` A8L8, `0x64` A8B8G8R8, `0x66` A8R8G8B8, **`0x68` A2B10G10R10**, **`0x6a` A2R10G10B10** |
| 1 | `0x3b`, `0x50`, `0x54`, `0x74`, `0x75`, `0x78` |
| 2 | `0x34`, `0x38`, `0x3c` (the 64/128-bit float family) |
| `arg ? 7 : 0` | `0x32`, `0x53` |
| `arg ? 7 : 1` | `0x52`, `0x56` |
| `arg==0 ? 1 : arg==1 ? 4 : 7` | `0x76`, `0x7a` |
| `arg<2 ? 1 : 4` | `0x77` |
| 7 | every other code, and every code outside `0x30..0x7a` |

**A 10-bit render target takes class 0**, the same class as every ordinary
packed 8-bit and 16-bit colour target.  The class is not a bit-depth
indicator; it selects the raster's output conversion family, and 10-bit
belongs to the ordinary one.  A driver that already emits class 0 for
`B8G8R8A8` needs no change to index `89` (CAPTURED + DERIVED).

### 4.2 Write masks in `ab`/`ac` (CAPTURED)

The per-target 16-bit colour write mask that lives in `ab[15:0]`, `ab[31:16]`,
`ac[15:0]`, `ac[31:16]` is built by `MxMD 0x1389a0`.  The direct mask path,
which turns `D3DCOLORWRITEENABLE` bits into the repeated nibbles `0x4444`
(red), `0x2222` (green), `0x1111` (blue), `0x8888` (alpha), is taken **only**
when the surface's format callback returns internal type `0x0c` or `0x0d`
(`A8R8G8B8` / `X8R8G8B8`) and the two AA bits at validator `+0x174` are clear
(`0x138a42..0x138a7a`).

Every other format - including both GigaColor IDs `0x10`/`0x11`, and also
`A8B8G8R8`/`X8B8G8R8` - takes the compiler path:

```text
0x138a91   mov word [edi + ebx*2 + 0x358], 0xffff   ; mask = all ones
0x138aa5   call 0x173ee0                            ; channel-routing compiler
```

So for a 10-bit target the 16-bit mask is written as **`0xffff`** and the
channel masking is expressed as native fragment instructions in the per-target
output program instead.  This is exactly what a 10-bit target requires: the
repeated-nibble mask is a byte-lane mask that cannot describe fields which
straddle byte boundaries (CAPTURED + DERIVED).

Implementation rule: for a 10-bit colour attachment, program `ab`/`ac` with
`0xffff` for that target and implement `PIPE_MASK_RGBA` subsets by dropping
the corresponding component stores in the fragment output sequence.  The one
case that still uses the mask is the zero-mask suppression at
`0x138bd9..0x138bf0`, which clears the halfword outright.

### 4.3 Blend (CAPTURED, with an open edge)

Blend lowerer `0x170ff0` routes IDs `0x10`/`0x11` through the generic compiler
`0x173ee0`, preserving the 32-bit native Giga render format; no 16-bit
shortcut and no format-specific blend-factor path exists.  Static code proves
reachability but not the rounding of every blend factor at ten-bit precision;
that remains open (section 8).

A format-conditional colour constant does exist.  At
`0x168e5`, `0x16eb9`, `0x357d7` and `0x36007` the driver masks a 32-bit colour
before emitting it at stream index `0x93`:

```c
uint32_t mask = (code == 0x64 || code == 0x66) ? 0x00ffffff
              : (code == 0x68 || code == 0x6a) ? 0x3fffffff
              :                                  0xffffffff;
```

`0x3fffffff` is precisely the three 10-bit colour fields with the two alpha
bits removed, against `0x00ffffff` for the 8-bit orders.  A driver emitting
that index for a 10-bit target must mask to 30 bits (CAPTURED).  The exact
role of index `0x93` is not yet named in this tree; the four sites reach it
through the packet header `0x45454593`.

### 4.4 Clear (CAPTURED, already recorded)

`parhelia-m9-gigacolor-faa-static-closure.md` has the native clear packer at
`0xb3910` (`0xb3ef7` for ID `0x10`, `0xb3ffa` for ID `0x11`):

```c
u10(c) = (unsigned)(c * 1023.0 / 255.0 + 0.5);
u2(c)  = (unsigned)(c *    3.0 / 255.0 + 0.5);
A2B10G10R10 = u10(R) | u10(G)<<10 | u10(B)<<20 | u2(A)<<30;
A2R10G10B10 = u10(B) | u10(G)<<10 | u10(R)<<20 | u2(A)<<30;
```

This is a native 10-bit clear word, not a replicated 8-bit dword.

## 5. The display controller

### 5.1 The hardware knows a 10-bit scanout format (CAPTURED)

**This driver is not bound by the vendor's mode table.**  What section 5 shows
dead is the Windows driver's *mode enumeration* - it never offers a 30-bit
mode, so nothing downstream is ever asked for one.  The hardware path behind
it is whole: a scanout surface code exists for the 10-bit formats, `0x368fe`
packs it into bits `[31:25]` of the display-controller surface descriptor, and
`0x3dfd0` builds a ten-bit-per-channel ramp for a LUT that is physically ten
bits wide.  This driver writes that descriptor itself and enumerates its own
modes, so 10-bit scanout is **testable here** by writing the code and looking
at the panel.  DisplayPort carries 30-bit colour, and this driver already
trains the link, so the remaining unknowns are whether the transmitter needs
its bit depth stating in the MSA and whether the panel accepts it - both
questions a measurement answers.  UNTESTED.

`MxMM.sys 0x37b31` maps a display format id (1..12) to a scanout surface code:

| id | code | format | | id | code | format |
|---:|---:|---|---|---:|---:|---|
| 1 | `0x30` | 8 bpp palettized | | 7 | `0x58` | - |
| 2 | `0x4a` | A1R5G5B5 | | 8 | `0x7b` | - |
| 3 | `0x4e` | R5G6B5 | | 9 | `0x50` | L16 grey |
| 4 | `0x38` | 64-bit | | 10 | `0x64` | A8B8G8R8 |
| 5 | `0x66` | A8R8G8B8 | | 11 | `0x3c` | A16B16G16R16F |
| **6** | **`0x6a`** | **A2R10G10B10** | | **12** | **`0x68`** | **A2B10G10R10** |

`0x368fe..0x36933` then packs that code into bits `[31:25]` of the surface
descriptor dword the display controller consumes - the same field as the 3-D
descriptor (section 2).

The LUT layer is format-aware.  `MxMM.sys 0x3dfd0` turns the format id into a
surface code and selects the per-channel ramp width:

| code | ramp bits per channel | site |
|---:|---:|---|
| `0x66`, `0x64` | 8 | `0x3e084` |
| `0x4e` | 5 / 6 / 5 | `0x3e011` |
| **`0x6a`**, `0x3c` | **10** | `0x3e07c` |
| other | default path | `0x3e099` |

and the physical LUT is ten bits wide: the load routine
`MxMDI.dll 0x1007f710` masks each 16-bit entry with `0xffc0` (bits 15:6) before
writing R at display-controller `+0x3c`, G at `+0x40` and B plus the latch bit
at `+0x44`, indexed through `+0x38`.

`MxMD.dll 0x2ff50` even contains a complete 32-bpp / 10-10-10 mode descriptor
at `0x302d6`: surface code `0x6a`, masks `0x3ff00000` / `0x000ffc00` /
`0x000003ff`, ten bits per channel, family `6`, total depth 32 (`0x300e5`).

### 5.2 No caller can ever select it (CAPTURED)

The mode record's format selector is a bitmask: `0x001` 8 bpp, `0x008`
A1R5G5B5, `0x010` R5G6B5, `0x020` 24 bpp, `0x040` A8R8G8B8, **`0x080`
A2R10G10B10**, `0x200` L16.  Only two sites produce that mask, and both derive
it from a Windows bit depth through the identical table:

* `MxMD 0x30352..0x303b8`, byte table `0x30424`, jump table `0x3040c`
* `MxMD 0x105f0..0x10684`, byte table `0x10704`, jump table `0x106ec`

Both tables are indexed by `bpp - 8` over the range 8..32 and contain

```text
00 05 05 05 05 05 05 01 | 02 05 05 05 05 05 05 05
03 05 05 05 05 05 05 05 | 04
```

giving 8 bpp -> `0x01`, 15 -> `0x08`, 16 -> `0x10`, 24 -> `0x20`, 32 ->
`0x40`, and slot `5` - the invalid arm that returns zero - for everything
else, 30 included.  **No input to either table produces `0x080` or `0x200`.**
The 10-bit mode descriptor at `0x302d6` is unreachable.

The `DrvGetModes` DEVMODE filler at `MxMD 0x2fe30` closes the loop from the
other side: at `0x2fe80..0x2feaa` both mask `0x40` and mask `0x80` write
`dmBitsPerPel = 32`, so even a miniport mode list carrying `0x80` would be
advertised to Windows as ordinary 32 bpp and would come back through the
tables above as `0x40`.  `dmBitsPerPel = 30` is never written anywhere in the
image.

The two registry switches that reached GigaColor on the previous generation
are gone.  `MTXPARD.dll 0x10b3e0..0x10b4bd` reads `System\VSyncInt`,
`3D\GigaColorFullScreen`, `3D\GigaColorRender` and `DirectX\DxVaSupport`
consecutively; the corresponding `MxMD.dll` routine at `0xba830..0xba8c0`
reads only `VSyncInt` and `DxVaSupport`, with the two GigaColor reads removed
and nothing in their place.  Neither name occurs anywhere in any `MSerX`
binary, in any `.inf` in the package, or in the PowerDesk configuration for a
`0540` device.

### 5.3 The LUT bypass is not tied to 10-bit (CAPTURED)

`MxMDI.dll 0x1007ef50` sets bit 14 (`0x4000`) of the display-controller
control register at base `+0x00` from a plain boolean argument, with no device
or format test; the LUT load routine at `0x1007f710` clears the same bit
before writing entries (`0x1007f782`).  The driver's existing deliberate LUT
bypass (`m9x-scanout-gamma-bypass`) is therefore orthogonal to colour depth,
and the fact that it works at 8 bits says nothing about 10-bit scanout in
either direction.

Note the interaction that *would* matter if 10-bit scanout were enabled: the
LUT is physically ten bits wide, and `MxMM.sys 0x3dfd0` builds a ten-bit ramp
only when the surface code is `0x6a`.  Bypassing the LUT sidesteps that
choice, so a driver that both scanned out 10-bit and bypassed the LUT would
not depend on the ramp width at all - but it would also lose whatever the
ramp is compensating for (INF; the reason the vendor ramp exists is not
recovered).

### 5.4 Render and scanout are independent (DERIVED)

The render/sample format comes from the resource's own format code through
`0x467e0`; the scanout format comes from the mode-record mask through a
completely separate chain ending in `MxMM.sys 0x368fe`.  Nothing joins them.
A driver can therefore render 10-bit and scan out 8-bit - which is exactly
what this vendor driver does when Direct3D creates an `A2R10G10B10` render
target - and, if the scanout path were driven directly, could scan out 10-bit
from an 8-bit-rendered composite.  The only recovered coupling anywhere is the
OpenGL ICD's refusal to page-flip, section 6.

## 6. What the vendor calls it, and the OpenGL side (CAPTURED)

"GigaColor" appears **once** in the whole `MSerX` package, as a diagnostic
string in `MxMGLX.dll` at file offset `0x7f15a0`:

```text
WGLICDLink: disabling page flipping, Page flipping is not supported when using gigacolor
```

No code reference to that string was found, and the module's source path
(`.\OS\XP\WGLPixelFormatMgr.cpp`, `0x7f2960`) places it in pixel-format
selection.  The internal vocabulary is not "GigaColor" but the ordinary
Direct3D format names: `A2B10G10R10` / `A2R10G10B10` in `MxMD.dll`'s name
table (`0x1d6d60`, `0x1d6da0`), and `FORMAT_A2B10G10R10`,
`FORMAT_A2R10G10B10`, `FORMAT_A2R10G10B10_GRAY` in `MxMDI.dll`'s VidPn
source-mode dump (`0xdd4a4`, `0xdd4f8`, `0xdd4e0`), decoded by the switch at
`0x10032fc0..0x100330c3`.  `GigaColorRender` and `GigaColorFullScreen` exist
only in the sibling Parhelia and P-Series drivers in the same package.  Search
for the format names, not for the marketing name.

The OpenGL ICD implements 10-bit **textures** and not 10-bit framebuffers:

* `GL_RGB10` (`0x8052`) and `GL_RGB10_A2` (`0x8059`) canonicalise to
  themselves only when device capability byte `globals+0x1b2` is set
  (`0x100f0a6e`, `0x100f0aa9`); otherwise they silently degrade to `GL_RGB8`
  and `GL_RGBA8`.  The cap is set at `0x1002d4f0` and AND-reduced across every
  attached board at `0x1002d6a9`, so a mixed multi-board system loses it.
* The ICD's internal ids `0x28`/`0x29`/`0x2a`/`0x2b` carry the exact
  `0xc0000000` / `0x3ff00000` / `0x000ffc00` / `0x000003ff` masks at 32 bits
  per texel (`0x100dc34b`, `0x100dc383`).  The ICD never forms the hardware
  descriptor word, so the codes `0x68`/`0x6a` do not appear there in that
  role - and beware that the ICD's *own* ids `0x68` and `0x6a` mean
  `GL_RGBA32F` and `GL_RGB32F`.
* The static `PIXELFORMATDESCRIPTOR` array at `0x108a2850`, 72 entries, has no
  `cRedBits` above 8 and no `cColorBits` of 30, and no runtime synthesis of a
  10-bit descriptor exists.

`GL_UNSIGNED_INT_10_10_10_2` and `GL_UNSIGNED_INT_2_10_10_10_REV` are accepted
only as pixel-transfer types, handled on the host.

## 7. Implementation summary

```c
/* sampler view and colour attachment, both roles, same code */
#define M9_FMT_A2B10G10R10  0x68   /* R at bit 0  */
#define M9_FMT_A2R10G10B10  0x6a   /* B at bit 0  */

d1 = ((uint32_t)code << 25) | (kind << 16) | width;   /* index 79 / b1 */
```

1. Advertise `PIPE_FORMAT_R10G10B10A2_UNORM` (code `0x68`) and
   `PIPE_FORMAT_B10G10R10A2_UNORM` (code `0x6a`) for `PIPE_BIND_SAMPLER_VIEW`
   and `PIPE_BIND_RENDER_TARGET`, in the same commit that implements them.
   Do not advertise them for scanout or display.
2. Allocate and tile them exactly as `B8G8R8A8`: 32 bits per texel, storage
   class 2, no new layout code (section 3.2).
3. Emit render class 0 at index `89` bits `3*s` - the same value an 8-bit
   target already gets (section 4.1).
4. Emit `0xffff` in the target's 16-bit half of `ab`/`ac` and express colour
   write masks by dropping component stores in the fragment output program
   (section 4.2).
5. Pack clears with the 1023/3 rounding of section 4.4, not by replicating an
   8-bit dword.
6. Leave the KMS side alone.  Do not add a depth-30 visual: nothing in these
   binaries selects a 10-bit scanout, and the descriptor field that would do
   it (`[31:25] = 0x6a`) has never been exercised on this hardware.

## 8. Open questions, and the capture that closes each

| question | why static analysis cannot close it | capture or experiment |
|---|---|---|
| Does the raster actually accept code `0x68`/`0x6a` as a colour target on this silicon? | The driver constructs the descriptor; no capture binds one | Bind a `0x6a` target, clear to four sentinel colours, read the raw BO through the host-aperture slot and compare the exact packed dwords |
| Does the sampler filter a 10-bit texture, and with what precision? | The format-operation table declares it; no capture samples one | Sample a 4x4 `0x68` texture with NEAREST then LINEAR into a known-good `0x66` target and compare all texels |
| What are the blend-factor rounding results at ten bits? | `0x173ee0` is a general compiler; no arithmetic is format-specific | Blend a known pair of 10-bit values with each factor and read the target back |
| Is `79[24]` meaningful for a 10-bit descriptor? | The builder never writes it for selector 2 or 8 | Log the full `79`/`b1` for a freshly created 10-bit resource |
| What is stream index `0x93`, whose value is masked to 30 bits for 10-bit targets? | Four emit sites, no consumer joined | Log index `0x93` together with the bound target format across a clear and a draw |
| Would the display controller accept `[31:25] = 0x6a`, and does the 10-bit ramp then matter? | The driver never programs it; the LUT bypass is unconditional | Direct DRM experiment: program the scanout descriptor with code `0x6a`, write a known 10-bit ramp, and photograph or capture the output with the LUT both bypassed and loaded |
| Does any miniport mode list ever carry mask bit `0x80`? | The list comes from the miniport at run time through IOCTL `0x232024` | Log the mode records returned by that IOCTL; note this is moot for the verdict, because `DrvGetModes` collapses `0x80` to 32 bpp and the reverse table then requests `0x40` |

The first three are the exposure gate.  Until they pass, a Gallium driver must
not advertise either format, and the KMS driver must not offer a depth-30
visual.

## 9. MEASURED on an M9128

Everything here was read back from the hardware through
`tools/m9-mesa-glsl` against the clean-room Gallium driver and the clean-room
kernel module, with `GL_RENDERER` asserted to be `Matrox M9` on every case.
The fixture data is traceable: red names the texel's column and the row's low
two bits, green names the row, so every one of the 1024 levels of every
channel is exercised and a texel identifies itself.

Gallium's `PIPE_FORMAT_R10G10B10A2_UNORM` carries red at bit 0, so it is D3D
`A2B10G10R10`, code `0x68`; `B10G10R10A2_UNORM` carries blue at bit 0 and is
`A2R10G10B10`, code `0x6a`.  Both were exercised: the driver's
`M9X_GIGA_ORDER` debug option hides one so the state tracker has to take the
other, and every case below passes either way.  Section 7's mapping is
confirmed.

### 9.1 The sampler delivers ten bits (`giga10tex`)

Two 10-bit textures are uploaded, identical except that one has the low two
bits of every colour channel cleared, and the fragment program renders their
difference amplified by 256.  A sampler that truncated to eight bits would
render the difference as zero.  The image is exact, every pixel: the low two
bits survive the upload, the tiled storage and the fetch.  The two alpha bits
come back as the four distinct values as well.

### 9.2 The render target keeps ten bits (`giga10rt`)

The same 10-bit texture is copied through a `GL_RGB10_A2` renderbuffer and
read back with `GL_UNSIGNED_INT_2_10_10_10_REV`.  Exact, every pixel, over
all 1024 levels of each channel.  Storage class 2 with the 32-bit layout,
render class 0 at index `89` and the descriptor code in `b1[31:25]` are
therefore all correct as section 3 and section 4 give them; no new layout
code was needed, which is what section 3.2 predicted.

### 9.3 The write conversion rounds, and the raster dithers

The first clear measurement looked like a truncation: a clear to `682/1023`
green came back as `681`.  It is not.  `ef[7:0]` is `0x55` with dithering off
and `0x00` with it on (CAPTURED, `parhelia-m9-gl15-ff-capture.md`), and the
driver was leaving it on; with dithering on the surface came back `681` on
exactly half its pixels and `682` on the other half, and the two-bit alpha
likewise spread over two values on 1.6% of the surface.  That is a spatial
dither between the two levels the value falls between.

With `ef[7:0] = 0x55` every pixel holds the nearest level.  A clear to
`343`, `685` and `1021` -- none of which is on the 255-step ladder an 8-bit
write is confined to -- comes back exactly, on every pixel.  So:

* the float-to-10-bit conversion **rounds to nearest**, not truncates;
* the raster **dithers a 10-bit target by default**, which a driver wanting
  the nearest level has to turn off, exactly as the vendor's Parhelia path
  does for its own GigaColor target.

Blending `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` over an off-ladder destination
lands within one level of a software reference computed at ten bits, and the
destination it reads back is the off-ladder value, so the destination read
keeps ten bits too.  This closes the "blend-factor rounding at ten bits" open
question for that pair of factors; the remaining factors are not measured.

### 9.4 Write masks

`ab`/`ac` gate whole byte lanes, so they cannot describe a channel that
straddles a byte boundary; section 4.2 has the vendor expressing the mask in
the fragment output instead.  That lowering is not implemented, so the driver
refuses a partial colour mask on a 10-bit target rather than applying it to
the wrong bits.  The full mask, which is what `0xffff` means, is what every
measurement above ran under.

### 9.4 Scanout

The display controller takes its pixel format from bits `[31:25]` of the
surface descriptor, which on this head is the high half of the pitch word at
DC bank `+0x58`.  Nothing else had to change: a 10-bit pixel is four bytes,
the same 32-bit storage class the scanout allocation already sizes.

MEASURED on the M9128, head 0 on DP-1 at 1920x1200, with the clean-room KMS
driver advertising `XR24 XR30 AR30 XB30 AB30` on the primary plane:

* every modeset completes and the DisplayPort link retrains;
* the pitch word read back **while the head is scanning out** is
  `0xcc210780` for `XR24`, `0xd4210780` for `XR30` and `AR30` and
  `0xd0210780` for `XB30` and `AB30` -- codes `0x66`, `0x6a` and `0x68`;
* nothing in dmesg, and the 3-D engine is undisturbed afterwards.

The whole chain from GL to the CRTC works (`tools/m9-gigacolor-scanout.c`):
GBM reports `XR30`/`AR30`/`XB30`/`AB30` supported for
`SCANOUT|RENDERING`, EGL offers **six 30-bit configs** out of twelve
(`10/10/10/2` with native visual `AR30` and `10/10/10/0` with `XR30`), a GL
context on one of them renders an interpolated ramp into the GBM buffer, and
the buffer maps back with **1024 distinct codes across 1920 pixels and no
non-monotonic step** -- an 8-bit target could resolve at most 256.
`drmModeAddFB2` and `drmModeSetCrtc` then take it.

So a 30-bit EGL config does fall out of advertising the formats on both
sides.  EGL offers only the `AR30`/`XR30` orders: Mesa builds its GBM config
list from a fixed visual table that has no `XB30`/`AB30` entry, which is a
Mesa property and not a hardware one -- the DRM plane takes all four and the
controller is programmed with the right code for each.  A depth-30 **X**
visual needs the X server and its DDX at depth 30 as well and was not tried.

**UNTESTED: what the panel makes of the 30-bit pixels.**  No capture on this
bench sees the M9's DisplayPort output, so this is the one claim that still
needs a look at the monitor.  It is also where the last open question sits --
whether the transmitter has to be told its bit depth.  The link-format
register `0x1c38` is patched from the PInS table keyed on pixel clock alone
and carries no colour-depth field this driver knows of; it read
`0x111d011d` identically for 8-bit and 10-bit modesets.  Reproduce with

```sh
flock -w 600 /root/tmp/m9-hw.lock -c \
  'HOLD=60 FORMATS="XR24 XR30" /root/tmp/giga-scanout.sh'
```

The whole fixture suite, with these cases and the S3TC ones in it, is **221
of 221** on the M9128 with `GL_RENDERER` asserted on every case.

### 9.5 Still open

* `79[24]` for a 10-bit descriptor: the driver writes the code doubled with
  bit 24 clear and everything above works, so the bit is not required; what
  it would mean is still unknown.
* Stream index `0x93` and its 30-bit mask (section 4.3): the driver never
  emits that index, so nothing was measured.
* Sampler *filtering* precision at ten bits: every case above samples
  `GL_NEAREST`.  A `GL_LINEAR` fetch of a 10-bit texture is not measured.
* Blend factors other than `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`.
* What the panel shows (section 9.4), and whether the DisplayPort
  transmitter needs its bit depth stated.
* A converting blit out of a 10-bit surface: `glReadPixels` asks st for a
  blit into an `R10G10B10A2` staging image, the driver declines it as a
  scaled/converting blit, and the read comes back untouched rather than
  falling back.  `tools/m9-gigacolor-scanout.c` maps the buffer instead.
