# Parhelia GigaColor and 10-bit textures

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note separates what the vendor binaries prove about **GigaColor
surfaces** from what they prove about **10-bit texture sampling**.  Those are
not currently the same feature.

Evidence labels: **[BIN]** is static analysis of the Windows XPDM driver
`MtxPar/MTXPARD.dll` unless stated otherwise; **[VENDOR]** is the supplied
Matrox Linux documentation; **[OPEN]** needs a hardware experiment.

## Result

Parhelia has a 32-bit storage format for a 30-bit GigaColor surface:

```
surface cfg = width | type << 16 | 0x34 << 24
```

`MTXPARD`'s surface-format selector at `0x1dc67..0x1dcf3` selects code
`0x34` for its 30-bpp cases, and its complete descriptor bpp helper classifies
`0x34` as 32 bpp.  The vendor Linux documentation calls depth 30 “GigaColor”
and enables it with an X screen `DefaultDepth 30` [VENDOR
`matroxdriver-x86_64-cvs-20090511/docs/readme-advanced.txt`, §7].

The binary-proven component layout for code `0x34` is **X2R10G10B10**:

```
31       30 29                  20 19                  10 9                   0
+-----------+----------------------+----------------------+---------------------+
|    X2     |         R10          |         G10          |        B10          |
+-----------+----------------------+----------------------+---------------------+
```

There are two independent Windows-binary paths for this result.  The format
packer for the 10-bit family at `MTXPARD 0x842f8..0x84344` preserves the two
high bits and the middle ten-bit field, and conditionally swaps only
`[29:20]` and `[9:0]`.  More directly, the D3D state constructor at
`MTXPARD 0x152b8a..0x152bcd` masks a live surface cfg with `0x3f000000` and
compares it with `0x34000000`.  Its normal RGB masks are
`0x00ff0000`, `0x0000ff00`, and `0x000000ff`; in the `0x34` arm they become
`0x3ff00000`, `0x000ffc00`, and `0x000003ff`.  Those are exactly R[29:20],
G[19:10], and B[9:0].  The two paths make the channel positions a binary fact,
rather than an inference from the usual Windows 30-bpp convention.

The optional packer swap supplies the BGR sibling but does not alter the two
high bits.  Little-endian memory contains the ordinary little-endian
representation of this 32-bit word.  Neither path assigns meaning to the
high two bits for sampling or blending.

Do **not** assign code `0x36` to an alpha GigaColor format in a new driver.
The existing verified 3-D path uses `0x36` for its S8Z24 depth surface.  Some
older notes infer an A2R10G10B10 meaning from the `0x3x` numbering, but that
inference conflicts with the working depth path and is not sufficient to ship
an alpha colour format.

## Windows API exposure

The D3D format decoder at `MTXPARD 0x83af0` has these relevant outcomes:

| D3DFMT | Windows internal id | result |
|---|---:|---|
| `A2B10G10R10` (31) | `0x0d` | decoded |
| `A2R10G10B10` (35) | 0 | rejected |

This is only the D3D-format-to-internal-id decoder.  It does **not** establish
a usable sampler format.  The common texture border-colour packer at
`0x84060` rejects id `0x0d` along with `0x0e` and all depth formats.  No vendor
capture programs `0x11e0[29:24] = 0x34` for a texture, and the recovered
OpenGL capture matrix contains no `GL_*10_*2` texture use.  Thus the vendor
binaries provide no confirmed Windows API route that creates and samples a
10-bit texture.

The Linux kernel's blend-state builder nevertheless treats internal ids
`0x0f` and `0x10` as the two 10-bit GigaColor render-target variants: it
disables destination dithering for them
(`docs/parhelia-blend-alpha-encoding.md`, §1.3).  This confirms a 10-bit
**render-target** class, but does not join either internal id to a texture
descriptor format code.

## Descriptor and storage constraints

If a direct hardware test is attempted, use the ordinary 32-bpp texture
descriptor shape, substituting only format code `0x34`:

```
0x11dc = base | address_space                 /* local VRAM is 0 */
0x11e0 = raw_width | (layout << 16) | 0x34000000
0x11e4 = raw_height | (1 << 16)               /* one 2-D slice */
```

The descriptor's format field is six bits, so `0x34` is an unambiguous value.
The common pitch helper gives a linear texture `align(width, 32) * 4` bytes per
row; its 32-bpp calculation includes code `0x34` [BIN].  That proves only the
allocation calculation, not sampler acceptance.

For a vendor-aligned sampler experiment, use the ordinary 32-bpp **layout 2**
path and its chip-specific tiled upload mapping.  The recovered Linux GL
driver programs layout 2 for captured 32-bpp textures; no vendor capture
samples layout 0.  A linear-layout test is useful as a separate isolation
experiment, but it must not be described as the safe or vendor-proven path.
Use one level, nearest filtering, repeat or clamp-to-edge, and no border
colour.  **No capture proves that code `0x34` accepts any filtering,
mipmapping, layout, or border mode.**

The 10-bit border-colour packer is useful evidence for channel order, not
proof that border sampling works: the selected case can expand/swap a 10-bit
word, but the D3D texture-format path above cannot reach an accepted 10-bit
sampler record.  Leave border mode off for the first test.

## Render targets, alpha, and blending

`0x34` is proven as a GigaColor surface descriptor and as the vendor's 30-bpp
selection.  It is not yet proven as a 3-D colour slot on the target P690.
The legacy 3-D setup normally rewrites its 32-bpp colour target to `0x32`,
and the known good depth slot is `0x36`.  The Windows and later Linux binaries
contain registry-name strings `GigaColorRender` and `GigaColorFullScreen`, both
defaulting to zero.  Those strings establish configuration vocabulary only:
the static pass did not recover a reader which joins either value to a sampler
or render-target state packet.  They must not be treated as evidence of a
usable texture capability or as an enabling recipe.

No alpha behaviour can be claimed for code `0x34`: its documented form is
X2R10G10B10, so alpha writes, alpha test, and destination-alpha blend factors
must be disabled or treated as opaque until a separate A2 format is proved.
The blend constants themselves are four 10-bit values in `0x12ec/0x12f0`,
but that register precision does not prove the render target or a texture has
two alpha bits.

## Implementation order

1. Add KMS/Xorg depth-30 scanout as an **X2R10G10B10 / code 0x34** surface.
   It is the only externally documented GigaColor interface and avoids
   advertising untested sampling.
2. Add a direct DRM test that binds a `0x34` colour surface, clears red,
   green, blue and grey ramps, reads it back, and compares the exact packed
   words.  Repeat with a WARP triangle and with blending disabled.  This
   determines whether the 3-D colour slot accepts `0x34` on the P690.
3. Test texture sampling independently: create a 4x4 layout-2 `0x34` texture
   with four saturated corner colours, render it into the established `0x32`
   target, and test nearest then linear filtering.  The result settles the
   descriptor code, outer-channel swap, and whether sampler filtering is
   supported.  A layout-0 repetition can then distinguish a sampler-format
   limitation from a layout limitation.
4. Only after that test passes, expose the corresponding Gallium
   `PIPE_FORMAT_*10*10*10*` format.  Select its exact Mesa name and any R/B
   conversion from the measured corner pixels, rather than from a guessed
   `A2R10`/`A2B10` naming convention.  Initially advertise it for sampling
   only if the sampler test passes, and for render targets only if step 2
   passes.
5. Test an alpha-bearing 10:10:10:2 surface separately.  It needs a second
   confirmed descriptor code and destination-alpha/blending readback; do not
   derive it from `0x36`.

This order keeps a real, documented GigaColor display feature separate from
the presently unproven texture feature and makes every Mesa capability claim
depend on a target-hardware result.
