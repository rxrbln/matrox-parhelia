# Parhelia texture updates and small UI images

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note narrows the safe texture-update path for the open P690 driver.  It
cross-checks the saved Linux GL HAL and Windows GL ICD, without assuming that a
small image is a special sampler format.

## Findings

### Texture redefinition and sub-image uploads

The Windows GL ICD validates two-dimensional image dimensions independently in
`MTXOGL.dll` at `0x103cc360`: after removing the optional border it accepts
each dimension up to `0x800`; non-power-of-two dimensions are accepted when
the runtime NPOT capability byte at `context+0x1a9` is set.  The saved Linux
stack is more conservative at its GL API boundary, but the Linux kernel HAL
does not require a power-of-two base image.

The vendor Linux `glTexSubImage2D` capture is more useful for implementation:
a 32x32 image followed by a 16x16 update at (8,8) sends a single upload of the
complete 32x32 level.  The record has `x0 = y0 = 0` and a 32x32 extent.  This
is an upload-policy choice, not a hardware restriction: the record format has
explicit x/y/width/height fields.  An open driver may update only the mapped
box when it preserves all other texels, which Mesa's transfer staging does.

### 4x4, 8x8 and 16x16 images

These dimensions are normal level-zero images, not a glyph-only path:

| image | captured descriptor `0x11e0` | descriptor width |
|---|---:|---:|
| 2x2 RGBA | `0x32020002` | 2 |
| 8x8 RGBA | `0x32020008` | 8 |
| 16x16 | same field rule | 16 |

The sampler derives its row pitch.  For layout 0, a 32-bit texture is stored
at `base + y * align(width, 32) * 4 + x * 4`; the descriptor still carries the
unrounded width.  Therefore a 4x4/8x8/16x16 image needs a 128-byte row pitch
but must program its actual width.  The Linux HAL gives sub-32 and NPOT images
a linear shadow descriptor, which supports keeping small UI textures on the
linear layout-0 path.

### Alpha and DECAL UI composition

The vendor sampler's RGBA code is `0x32` (A8R8G8B8) and its RGB code is
`0x33` (X8R8G8B8).  GL RGB uploads are expanded with an opaque alpha.  The
combiner has dedicated DECAL state: RGB is `LRP(TEX, PREV, TEX.a)` and alpha
passes through PREV.  Alpha testing is an independent `0x12e4` state with an
8-bit reference.  A 32-bit RGBA upload must therefore retain byte 3, while a
RGB/XRGB source must set it to `0xff`; treating both as code 0x32 changes
DECAL and alpha-test results.

## Mesa comparison and safe changes

The current Gallium transfer code already does the required box update for
base-level 32-bit textures:

* it uses the requested box width for client staging;
* it copies exactly that box into the linear shadow at `linear_stride`;
* it retains the descriptor's true `width0` rather than substituting the
  padded pitch; and
* it maintains the R/B conversion at the transfer boundary.

Do not route small UI textures through the experimental tiled sampler solely
because they have a tiled render allocation.  The safe descriptor is layout
0 backed by `linear_handle`; layout 2 has a different 32x16 texture swizzle
and must only be used with matching storage.

There is one required coherency operation when a resource changes role.  A
resource written by the native tiled renderer has `tiled_dirty` set, whereas a
layout-0 sampler reads `linear_handle`.  Before emitting a layout-0 descriptor
for sampler unit 0 or 1, call `ph_resource_resolve_linear(resource)`.  For an
explicit experimental layout-2 descriptor, call `ph_resource_upload_tiled()`
first if its linear shadow is dirty.  This fixes render-to-texture and avoids
stale small UI images after a framebuffer copy or redraw.

Mip levels remain a separate implementation item.  The vendor places a chain
in one allocation, while this Mesa driver deliberately samples level zero and
ignores maps to higher levels.  Do not claim mipmap support or expose a
non-zero sampler-view base level until that allocation and descriptor path is
implemented.
