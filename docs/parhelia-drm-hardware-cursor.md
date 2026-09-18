<!-- SPDX-License-Identifier: GPL-2.0 -->

# Parhelia/P690 DRM hardware cursor

## Recovered hardware interface

The classic Parhelia through P690 display controllers each contain one cursor
block at `DC(head)+0xc0` (`0x18c0` for head 0 and `0x1ac0` for head 1):

| offset | function |
|--------|----------|
| `+0x00` | RGB888 palette data |
| `+0x04` | `(palette_index << 8) | mode`; mode 0 off, 3 mono, 4 palette/alpha |
| `+0x08` | position `((x + 64) & 0x7fff) << 14 | ((y + 64) & 0x7fff)` |
| `+0x0c` | raw VRAM byte address |
| `+0x10` | format, `0x05000000` mono or `0x04000000` palette/alpha |

The controller-level composite gate is `DC(head)+0x88`; the vendor sets bits
`0x16` when showing the cursor and clears them when hiding it. It also writes
mode zero to the other head's `+0x04`, ensuring only the scanning head owns the
cursor. Before moving/showing, the driver polls MMIO `0x450` bit 22 for the
cursor register latch window.

Linux vendor callback `mtx68` at `mtx_drv.so` address `0x45520` proves the
position rule. It adds 64 directly to the signed X and Y values before masking
and packing; it does not clamp negative coordinates to zero. Thus coordinates
from -64 through -1 retain the partially offscreen cursor. DRM supplies the
cursor image top-left through `crtc_x/crtc_y`; this driver does not enable the
virtualized cursor hotspot feature.

The cursor surface is fixed at 64x64. The format captured on 0x0528 hardware
and used by the vendor ARGB callback `mtx67` at `0x45d00` is a 16-color RGB888
palette with five per-pixel alpha bits. Each row has 128-byte stride:

| row bytes | plane |
|-----------|-------|
| `0x00..0x07` | palette index bit 0 |
| `0x08..0x0f` | palette index bit 1 |
| `0x10..0x17` | palette index bit 2 |
| `0x18..0x1f` | palette index bit 3 |
| `0x20..0x27` | source alpha bit 3 |
| `0x28..0x2f` | source alpha bit 4 |
| `0x30..0x37` | source alpha bit 5 |
| `0x38..0x3f` | source alpha bit 6 |
| `0x40..0x47` | source alpha bit 7 |
| `0x48..0x7f` | zero padding |

The vendor's packing loop shifts each accumulator right and inserts the next
pixel at bit 31. After 32 pixels, X=0 is bit 0 and X=31 is bit 31 in the stored
little-endian dword. Total image storage is 8192 bytes. The mono format instead
uses source and mask planes at row offsets 0 and 8 with a 32-byte row stride,
but the DRM implementation uses the alpha format for both ordinary and ARGB
cursors.

## Kernel implementation

`drivers/gpu/drm/parhelia/parhelia_cursor.c` exposes an atomic
`DRM_PLANE_TYPE_CURSOR` supporting linear `DRM_FORMAT_ARGB8888`, up to 64x64,
without scaling or cropping. It allocates a private page-aligned 8 KiB VRAM
surface during modeset initialization, pins and maps it for the device lifetime,
and programs the raw local offset. Allocating it before any userspace or scanout
BOs also avoids the P690 high-VRAM cursor-fetch problem observed by the DDX.

On an image change, the plane maps the source GEM VRAM object, collects up to 16
distinct nontransparent colors, maps further colors to the nearest RGB palette
entry, retains alpha bits 3..7, and writes the nine hardware planes. Position-only
commits only update the position register and composite gate.

The cursor bank follows the active KMS controller (`preferred_head` in the
active connector descriptor), independently of whether that controller reaches
the connector through DVO1, DVO2, DAC1, or DAC2. This matters on P690 DVI-1,
where controller A/head 0 is routed through DVO2.

The implementation uses `drmm_universal_plane_alloc()` so plane cleanup occurs
before its managed allocation is released. The private image has a later DRM
managed release action, which unmaps, unpins, and drops the GEM object before
the cursor-plane container is freed.


## Correction (2026-09-13)

`DC+0x88 = 0x16` is **not** the general cursor composite gate.  The vendor's
single-screen cursor path (mtx_drv.so `MTXHWCursorInit` default pair mtx68 /
mtx69) never writes DC+0x88 on any chip; `0x16` is reached only through the
TripleHead/QuadHead callbacks or the `THI`/`QHI` options.  The cursor is
switched on by its own control register (`DC+0xc4`, 4 for ARGB / 16-colour).
On the original Parhelia (0528) bit 4 of DC+0x88 delays the whole scanout by
8 pixels (measured: picture shifted left, columns 0-7 wrapped to the right
edge, removed by clearing only bit 4), and `captures/cursor-*-0528.txt` read
`0x1a88 = 0` with the vendor hardware cursor visible.  The KMS driver now
writes nothing there on the original family and keeps `0x16` on the P690,
where it was tested.  The cursor image must be uploaded through a host
aperture descriptor on the 0528; a raw BAR0 vmap write does not reach VRAM.
