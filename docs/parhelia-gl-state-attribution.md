# GL state → command bits, measured

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Measured on the 0528 by running synthetic GL variants against the vendor stack
(`tools/gltest.c`), capturing the command stream for each (`tools/capture-gl-matrix.sh`)
and diffing the per-draw span against a baseline single smooth triangle
(`tools/diff-gl-variants.py`). Each variant changes exactly one GL state, so a
difference is that state's encoding. Raw output: `captures/gl-matrix/attribution.txt`.

Baseline (one smooth-shaded triangle, 320×240 window):
`0x10b0=0006305c 0x10f0=00000002 0x1130=0200200c 0x1300=00a60ec4 0x132c=00f00140`
`0x1040=00000004 0x1000=00000003 0x1060=000280cc 0x12e8=00800202 0x12c8=001000e4`

## Primitive type — four registers move together [HW]

| GL primitive | `0x10b0` | `0x10f0` | `0x1130` | `0x1300` |
|---|---|---|---|---|
| GL_POINTS | `0x00060020` | `0` | `0x0000200c` | `0x00a60ecd` |
| GL_LINES | `0x0006103d` | `1` | `0x0100200c` | `0x00a60ec5` |
| GL_TRIANGLES | `0x0006305c` | `2` | `0x0200200c` | `0x00a60ec4` |
| GL_QUADS | `0x0006605c` | `2` | `0x0200200c` | `0x00a60ec4` |

So, confirming and extending the `mtx734`/`mtx3178` tables:
- **`0x10b0`** = `mtx734[prim]` in bits [6:0] (`0x20` point, `0x3d` line, `0x5c` triangle)
  with the topology in bits [14:12] (points 0, lines 1, triangles 3, quads 6) — quads
  differ from triangles *only* in the topology field.
- **`0x10f0`** low nibble = the primitive **class** (0 point, 1 line, 2 triangle) —
  this is the WARP MODE nibble `mtx1159[class]`, now confirmed on hardware.
- **`0x1130` bits [25:24]** = the same class (0/1/2).
- **`0x1300`** low nibble tracks the class too: `4` triangle, `5` line, `d` point.

## Raster state [HW]

| GL state | register | change |
|---|---|---|
| `glEnable(GL_DEPTH_TEST)` | `0x12c8` | `001000e4` → `001000e5` — **bit 0 = depth test enable** |
| `glEnable(GL_BLEND)` | `0x12e8` | `00800202` → `0080eced` — blend factors/equation |
| `glEnable(GL_CULL_FACE)` | `0x1130` | `...200c` → `...200d` — **bit 0 = cull enable** |
| `glShadeModel(GL_FLAT)` | `0x1130` | `0200200c` → `0200300c` — **bit 12 = flat shading** |
| window 320×240 → 640×480 | `0x132c` | `00f00140` → `01e00280` — **scissor max = (h<<16)\|w** |
| per-vertex colours | *(none)* | no state change: colour is vertex data, not state |

`0x1130` therefore packs at least three independent things: class in [25:24],
flat shading at bit 12, cull enable at bit 0.

## Notes

- `glPointSize(8)` shows the points encoding plus `0x1130` bits [25:24] = 3, which
  is not a primitive class — likely a wide-point path [INF].
- The `tex` and `lwidth` captures caught a 2D frame in their span (0 and 1
  address-draws respectively; `0x1300` reads `0x5c0`), so those two rows are not
  yet trustworthy and need re-capturing.
- Method trap worth keeping: the first run of this matrix attributed every
  variant to its **predecessor**, because the ring pages lag the write pointer and
  the newest span still held the previous client's commands. Each variant now
  renders several frames before the capture so the ring is dominated by its own
  work. Any future capture must confirm the span really belongs to the variant.
