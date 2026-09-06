# Cross-check: agent findings against hardware

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Every load-bearing claim from the eight reverse-engineering agents, checked
against the synthetic-GL hardware matrix (`captures/gl-matrix/attribution.txt`)
and against each other. **[HW]** = confirmed on the board, **[REFUTED]** =
contradicted, **[OPEN]** = unresolved conflict between sources.

## Confirmed on hardware

| claim | source | hardware check | verdict |
|---|---|---|---|
| Primitive word = `mtx734[prim] \| topology<<12 \| 0x60000` | draw-builders (binary tables) | all six captured primitives match the formula exactly: points `00060020`, lines `0006103d`, linestrip `0006203d`, triangles `0006305c`, tristrip `0006405c`, quads `0006605c` | **[HW]** |
| Lines encode `0x…103d`, not `0x…303d` | gl-state-encoding (correcting earlier docs) | measured `0006103d` | **[HW]** |
| Cull reaches `0x1130` bit 0, **not** `0x12fc` | gl-state-encoding (correcting pixel-pipeline) | `glEnable(GL_CULL_FACE)` → `0200200c` → `0200200d`; `0x12fc` unchanged | **[HW]** |
| Viewport = `c[0x160]` scale `(W/2, −H/2, (f−n)/2, 1)`, `c[0x161]` offset `(x0+W/2, y0+H/2, (n+f)/2, H)` | block-file-constants | 320×240 → `(160,−120,0.5,1)`/`(160,120,0.5,240)`; 640×480 → `(320,−240,0.5,1)`/`(320,240,0.5,480)` | **[HW]** |
| Lights are 7-vec4 blocks from `c[0x0E0]` | block-file-constants | `glEnable(GL_LIGHTING)` writes exactly `c[0x0E0..0x0E6]` | **[HW]** |
| Point size lives in block `0x172`, component w = **half** the size | gl-state-encoding (refuting raster-clip's `0x9a→0x1268`) | `glPointSize(8)` → `c[0x172].w` `0.5` → `4.0` | **[HW]** |
| Polygon offset lives in `c[0x178]` | block-file-constants | `GL_POLYGON_OFFSET_FILL` → `c[0x178]` `(0,0,…)` → `(1, 5.96e-08, …)` = factor 1.0, units 2⁻²⁴ | **[HW]** |
| `0x1090` = write-back address, `0x1094` = value (audit right, draw-builders swapped) | annotated-draw | `0x1090` carries address-shaped values that change every run | **[HW]** |

## Refuted

| claim | source | evidence |
|---|---|---|
| Cull is encoded in `0x12fc` | pixel-pipeline, state-slot-map | `0x12fc` does not move when cull is enabled **[HW]** |
| `0x10b8` descriptor field A = 5 for the second attribute | draw-builders | `0xa1c50020`, bits [16:12] = **16**, not 5 |
| `0x1060 = 0x280cc` is 163020 | draw-builders | `0x280cc` = **164044** |
| `0x1130` = `S[0x5f0] \| 0x200` (bit 9 always set) | warp-binding, engine-init | every measured value (`0200200c`, `0100200c`, `0000200c`) has **bit 9 clear**; the set bit is 13 (`0x2000`) |
| `0x41` "preformatted body" packets are program blobs | opcode-41 | they are a **bind table** — body dwords resolve to valid WARP entries into real heap slots |
| `brn` is a WARP opcode | earlier decode | `0x46` is `shli`; pinned by a rotate-left idiom in the self-test |
| `b0..b3`/`a0` are control state needing `ctl33`/`ctl35` | earlier reading | they are address latches `T0`/`T1`, written by the ordinary secondary op `mova` |

## Open conflicts

- **`0x10b8` field A meaning.** gl-state-encoding predicts A = the WARP input
  register number (position `v0`, diffuse `v5`, texcoords `v7..v10`); the
  measured field is 16 for the colour attribute, which fits neither `v5` nor a
  texcoord. Either the field boundaries differ from `[16:12]` or A is not the
  input register. Field B is unassigned by both.
- **Texturing and the WARP program.** gl-state-encoding argues texturing must
  regenerate the program (nTex is in the codegen cache key), contradicting an
  earlier "the bind is unchanged" reading — which rested on key `0x05` slot
  `0x14`, actually the *fragment* program selector. Not yet measured: every
  texture variant's capture window filled with 2D upload traffic and contained
  no 3D draw, so those rows are rejected, not answered.
- **`0x12c8` depth mask.** `glDepthMask(GL_FALSE)` produced the same value as
  plain depth enable (`001000e5`), so the write mask is elsewhere.
- **`glColorMask`** produced no state change at all in the captured span.

## Method notes that changed results

- Attribution must use **one frame's span**, and that span must contain a 3D
  draw: gate on `0x10b0` bit 17. Nine of 33 variants failed this gate.
- The first matrix attributed every variant to its **predecessor** — the dumped
  ring pages lag the write pointer. Each variant now renders warm-up frames first.
- Index `0x21` terminates the current **tag**, not the packet; parsing continues
  with the next dword. My parsers already did this; the annotated-draw agent
  found docs that did not.
