# P690 render-target row order, scanout address and CPU access

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Findings of 2026-09-11 on the P690 target (`chip=0x2539`, tile layout 1 =
64x16 pixels / 4 KiB), with the Mesa and kernel commits that apply them.

## 1. Render-target tile order: y bits 0 and 1

`p3d_tile_offset_layout()` placed y bit 0 in dword-index bit 6 and y bit 1 in
bit 3.  On P690 render targets the hardware uses the opposite assignment:

```
i = x0 | x1<<1 | x2<<2 | y0<<3 | x3<<4 | x4<<5 | y1<<6 | (x5^trow&1)<<7 | y2<<8 | y3<<9
```

Evidence, all on hardware:

* `parhelia-drm-test --raster-fill-agreement-probe`: 64 two-column
  rectangles (every start row modulo 16, heights 1..4) drawn by the 3-D
  engine into one target and by raw 2D fills into another produce identical
  raw bytes (320 lit dwords, 0 differ).  Both engines agree; only the CPU
  model differed.
* `tools/parhelia-gl-prim-test.c`: GL points of even size read back one row
  off exactly where the old model predicts, and exact with the new one.
* `--fill-geometry-probe`: with the corrected model every raw fill lands on
  exactly the requested rows at bases 0, 16, 64, 144 and 224.

The earlier "coordinate-ramp" verification accepted `|blue - y| <= 2`, which
cannot see a one-row exchange.  The same 64x32 formula was checked on the
older chip with a 500x300 rectangle that mapped back with "99.3% of the
pixels present"; exchanging only edge rows loses about 0.7% of such a
rectangle, so the 64x32 formula is suspect too, but it is unverified.

**Textures differ.**  The P690 texture unit reads 32-bit 64x16 textures in the
*old* order: with the corrected render order used for uploads, nearest-sampled
quads lose texel row 1 (exactly half of every quad).  Texture uploads use a
separate `p3d_texture_tile_offset_64x16()`; render-to-texture followed by
sampling is exact (`tools/parhelia-gl-texture-test.c`).

Mesa `cebafb6` (model split, workaround removal, probes); kernel `cdc88be`
(the diagnostic root-trace copies).

## 2. DC_START takes the bank-encoded address

The native colour class places targets in `[64,96)` MiB, i.e. in bank B.
`parhelia_pipe_enable/update` programmed DC_START with the flat BAR offset
(`0x4000000` for the X front), and the monitor kept showing X's startup root
weave while glamor rendered correctly into the buffer.  Programming the
engine's bank-encoded address (`0x8000000`, bank B starting at 128 MiB) shows
the live front buffer.  In bank A both encodings are identical, which is why
only the 32 MiB anchor had worked.  Kernel `c4da8e8`; the cursor base
(`parhelia_cursor.c`) still uses the flat address and needs the same check if
a cursor BO can land in bank B.

## 3. CPU mappings of render BOs

Only dumb buffers receive a host-aperture slot (a hardware-detiled linear CPU
view).  `CREATE_RENDER` without a class flag still returns an `mmap_offset`,
but on P690 that raw BAR0 view does not reach the BO's VRAM: GPU writes are
invisible to the CPU and CPU writes never reach the display.  All probes that
map legacy render BOs (`--render`, `--cull-probe`, `--point-probe`) fail the
same way with older binaries.  Class targets correctly return no mapping and
go through `UPLOAD_RENDER`/`DOWNLOAD_RENDER`; those ioctls reject legacy BOs.

Open work: stop returning a raw `mmap_offset` for tiled render BOs and let the
transfer ioctls accept them, or migrate the remaining legacy users.

## 4. Render-shadow transfers

Every map of a class target downloaded, and every unmap uploaded, the whole
BO through 8 KiB aperture chunks; one full-screen `XGetImage` of the 9 MB
front took 97 full downloads.  Image rows `[y0,y1)` of a render target are one
contiguous range of whole tile rows (`stride * tile_height * 4` bytes each),
so maps, unmaps and CPU copies now transfer only those rows (Mesa `269a114`).
The same change fixed a partial CPU copy uploading the destination's stale
shadow over the rest of the target.

## 5. X/glamor state after these fixes

With kernel `c4da8e8` and Mesa `7b8f33a`, glamor's own view (`XGetImage`) and
the scanned-out buffer (`parhelia-drm-test --scanout-dump`) agree, and GL
clients present correctly.  Still missing: X core/Xft text and tiled fills
such as the root weave.  Glamor implements both with GLSL programs that the
driver's fixed-function fragment matcher does not lower (8-bit glyph atlases
are not a supported texture format; tile repeat is computed in the fragment
shader).  This is the shader-lowering item of the fixed-function gap audit,
not a display or transfer problem.
