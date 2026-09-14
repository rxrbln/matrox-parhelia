# 16-bit render targets on the original Parhelia (0528)

Measured 2026-09-14 on the 0528 (256 MiB, tile layout 64x32) with
`parhelia-drm-test --narrow-target-probe` and `--fill-rate-probe`, plain render
objects (the native render classes exist only on the P690).  Companion to
`parhelia-p690-16-bit-render-targets.md`.

## Storage order (MEASURED)

A 16 bpp colour target keeps the 64x32 pixel tile and halves it to 4096
bytes.  One 1x1 2-D fill per pixel over 256x128 pixels (four tile columns,
four tile rows) placed all 32768 pixels, no duplicates, no stray halfwords.
In-tile byte index:

```
bit 1 = x bit 0     bit 5 = y bit 1     bit 9  = x bit 5 XOR tile-row parity
bit 2 = x bit 1     bit 6 = x bit 4     bit 10 = y bit 3
bit 3 = x bit 2     bit 7 = y bit 2     bit 11 = y bit 4
bit 4 = x bit 3     bit 8 = y bit 0
```

`offset = (y/32 * pitch/64 + x/64) * 4096 + index`.  Implemented in
`reference/p3d_tile.c` (`p3d_tile_offset_layout_cpp()` for the 64x32 layout);
the host selftest replays the measured offsets.

The 2-D fill takes a replicated 32-bit FCOL (`0x0000abcd` fills half the
halfwords), as on the P690.  3-D quads write exact `f800`/`07e0`/`001f`/`ffff`
on all 131072 pixels.

## Depth (MEASURED)

A D16 descriptor (`0x2a`) does not give two-byte depth.  The Z unit stores
32-bit words: after a quad at depth 0.25 the depth object holds `0x4000,
0x00ff` alternating on every halfword pair, uniformly over every tile, so half
the pixels test against the wrong word and a cpp 2 depth allocation is
overrun.  16 bpp colour with a 32-bit Z24 buffer of a different pitch (colour
384 px, depth 320 px) is exact for pass, reject and accept.

## Colour masks (MEASURED, open)

A white quad through the red plane only, then the green plane only, leaves
`f800` / `ffe0` on 36864 of 40960 pixels of a 320x128 target whose pitch is
384.  The missing 4096 are a 32-pixel-wide strip, consistent with the last
partial tile column; not investigated yet.

## Fill rate (MEASURED, 400 full-screen quads)

| mode | 32 bpp + Z24 | 16 bpp + Z24 | 16 bpp + D16 descriptor |
|---|---|---|---|
| 2-D fill | 620 | 1285 | 1286 |
| 3D colour, no Z | 954 | 953 | 953 |
| Z LEQUAL + write | 187 | 204 | 348 (overrunning, not usable) |
| Z LEQUAL no write | 399 | 597 | 778 |
| blend + Z no write | 353 | 544 | 739 |

Fewer quads (20-100) give much lower and unstable depth numbers: the
one-time move of the depth object into the window above its colour target
lands inside the timed case.

## State of the driver

The kernel creates 16 bpp render objects on both layouts (kernel `7333bde`).
Mesa keeps 16 bpp render targets on the P690 only: the colour-mask strip is
open, and an X server at depth 16 on the 0528 stops in glamor at
`glEGLImageTargetTexture2D(format not supported)` while importing the RGB565
front buffer as a texture, which Mesa refuses because the sampler and render
storage orders differ.
