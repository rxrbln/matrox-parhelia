# M9 FAA textured path toward Quake3 GL1 (m9faa4, 2026-10-08)

Copyright 2026 René Rebe <rene@exactco.de>

All hardware results are from the M9128 102b:0540 rev01 with the analytic pixel oracle
`tests/m9-faa-dual-later-native-20261008/check-later-pixels.py`. The hardware evidence is
in that directory, under `hardware-*-20261008`.

## Measured on hardware

| Feature | Evidence |
| --- | --- |
| No-clear later frames | `hardware-noclear-fix-20261008` |
| Later batches of 2 or 3 DRAWs | `hardware-batch-20261008` |
| Dual FIRST at 1024x768 | `hardware-1024-20261008` |
| Later frames at 1024x768 | `hardware-1024-later-20261008` |
| Depth-only later clear | `hardware-depth-pairs-20261008` |
| Count4 CLEAR3, any texture pair | `hardware-depth-pairs-20261008` |
| CW/CCW face culling | `hardware-cull-20261008` |
| 128x128 textures (kind 0x21) | `hardware-cull-20261008` |
| FAA gears at 1024 regression (guard field) | `hardware-gears1024-20261008` |
| Mipmapped textures (GL level encoding, six isotropic rates) | `hardware-mip-centre-20261008` |
| Culled 3-DRAW later batches | `hardware-mip-centre-20261008` |
| FAA pixel centre: integer; GL needs -0.5 px | `hardware-mip-centre-20261008` |

Notes on the rows:

- No-clear later frames: the source no-clear span began with the public Unlock's own
  F021->F022 copy, which native code had posted twice.
- Later batches: the FIRST append seam is reused with the later epoch.
- Dual FIRST at 1024x768: the guard band setup field comes from original 13b900.
- Depth-only later clear is what Quake3 issues every frame. It is the count4 CLEAR3
  prefix up to the depth constructor, followed by the no-clear DRAW/Lock form, with the
  Lock at Unlock+0x120.
- Face culling confirms the GL mapping BACK + CCW front -> CW.

## Source facts established (emulated original driver, no GPU completion)

- Culled FIRST changes four 6A selectors (0x28ff0000) and the cull field (CW 3, CCW 2).
  A culled later stage keeps two 12-byte 6A groups, 24 bytes in all
  (`tests/m9-faa-dual-cull-source-20261008`). There is no source for culled appends yet,
  so culled DRAWs go one per transaction.
- Later program switches (`tests/m9-faa-program-switch-source-20261008`). An earlier
  capture showed stage-1 COLOROP changes leaving the later image unchanged. That was an
  emulator artifact: the PDEV CRC table at 0x20cda0 was never built, so every
  program-cache key was 0. With `--pdev-crc-table`, a changed TSS, blend or alpha-test
  state builds and uploads a new program inside the later DRAW. That stage is the
  program's FIRST stage plus the usual sampler reload. A cached program is selected
  without a rebuild.
- Texture shapes (`tests/m9-faa-dual-texture-shape-source-20261008`): 128x128 uses kind
  0x21; request +0x2c is the level count. The D3D level/mip-filter words differ from the
  ordinary GL sampler encoding, so a level-coded hardware probe decides between them.
- A single-texture stage (`--single`) is a distinct 2720-byte stage. It has three target
  lanes, one sampler and a 48-byte first program entry, about 40 structural differences
  from the dual stage. It needs its own constructor.

## Quake3 GL1 inventory (ioq3 renderergl1, DERIVED)

- Lightmapped world: COLOR*T0*T1 with unit 0 MODULATE and the lightmap MODULATE. That is
  the dual MODULATE program.
- Each view starts with a depth-only clear; the colour is cleared only with r_fastsky.
- Depth uses LEQUAL by default and EQUAL for multipass. Culling is mostly on. Texture
  sizes vary, with mipmaps.
- Single-texture MODULATE, blended and alpha-tested stages need FAA fragment programs
  beyond the pinned FS72.

## m9faa5 additions (2026-10-08)

| Item | Status | Evidence |
| --- | --- | --- |
| Mipmaps, GL level encoding | MEASURED | `hardware-mip-centre-20261008` |
| Culled 3-DRAW batches | MEASURED | `hardware-mip-centre-20261008` |
| FAA raster samples at integer pixel centres; Mesa moves geometry -0.5 px | MEASURED | `hardware-mip-centre-20261008` |
| Later program switches (single, blend, alpha test) | PROVEN source | `tests/m9-faa-program-switch-source-20261008` |
| Later batches of 8 DRAWs, per-DRAW textures | PROVEN source | `tests/m9-faa-dual-later-batch-source-20261008` (`crc-*`) |
| GL depth range = ZRANGE k[2]/k[6] | PROVEN source, Mesa OFFLINE | `--zrange` of the dual xyz control |
| Single FIRST stage, later switch dual->single | CPU byte-equal, UNTESTED | DRM `m9x/tests/faa-single-first.md` |
| Per-frame fallback, admission census | OFFLINE | Mesa `7ac676d`, `d38a4a9` |

## Remaining, in order

1. Done (m9faa5): mipmaps, culled batches, GL pixel centre on the FAA raster.
2. Single-texture and blended FAA stages (vendor constructors needed).
3. Culled appends; 1024 CLEAR3 later on hardware.
4. First Quake3 GL1 FAA run at 1024 with fallback, then the FAA<->ordinary handoff.

## Status after m9faa6 (2026-10-09)

The commits below are in main DRM cf418aa and main Mesa c4fcc07.
Per-run evidence: `tests/m9-faa-q3-probe-20261009` (analytic probe) and
`/home/ai/tmp/agents/m9faa6/runs` (Quake3 logs).

### MEASURED

| Item | Commits | Evidence |
| --- | --- | --- |
| Quake3 frames reach FAA admission: a swapped DRI3 back buffer needs no FAA reset; a refusal after a presented part is a split, not a fallback; `M9X_FAA_BACKOFF=0` | Mesa 7fc9b89 | Quake3 census, slot a |
| RGBA8/RGBX8 textures (codes c8/c9); an RGB view of RGBA8 storage samples as c9; whole-target scissor admitted | DRM 70b1f53, Mesa 308f391/3dbe8fa/66b6326 | slot b census |
| Each transaction of a multi-transaction frame is presented through the CPU Lock view (the kernel Unlocks only after the view); an unposted frame is released when its ioctl fails | Mesa 4af2b12, DRM 60b6bb5 | slot c (diagnostic ESTALE kernel) |
| Quake3 frames presented through FAA: 269 frames, with a split at the first refused DRAW | | slot d, `q3/` in the probe dir |
| Textured blend families ONE/ONE, SRC_ALPHA/INV_SRC_ALPHA and DST_COLOR/ZERO; textured alpha test GEQUAL 0.5; first card run | Mesa d9e564c (textured alpha decode) | probe `families` |
| FAA depth top step: LEQUAL fails from window z 1-2^-17 up to 1; the range is scaled by 1-2^-16 (Quake3 sky at glDepthRange(1,1)) | Mesa 0997158 | probe `sky*`, `skyz*` |
| Textured GPU present, followed by a dual later frame counted from the PRESENT point | DRM d403927/44c04a6/8ccff4e; Mesa `M9X_FAA_GPU_PRESENT_TEXTURED=1` | probe `present` |

Correction to the slot d milestone: Quake3 draws the sky first, as two DRAWs at
glDepthRange(1,1): an opaque cloud layer, then a ONE/ONE layer. With families
off, every FAA part held the depth-only clear and the first sky DRAW only. The
blended layer split the frame, so world, weapon and HUD were ordinary. The black
FAA sky is the depth top-step defect above.

### OFFLINE (host tests only, not yet run in Quake3)

- Later transactions keep depth, cull and program changes. The kernel takes the
  per-DRAW A8 word and keys the 6A groups on the previous DRAW's raster state.
  DRM 56b1251/d42fa3e, Mesa a7d745c; vendor `later/`.
- FIRST transactions take per-DRAW texture pairs (programs version). This
  removes the FIRST -> present -> later chain. Mesa be5bab9.
- Single-level render-surface textures are sampled in place. RGBX ones are
  admitted only where alpha is never observed. Mesa 8a1ef9d.
- Per-DRAW clip window for scissors and sub-viewports: DRM 13e49de, Mesa 25aad0e.
- Per-DRAW colour write mask (fragment bits 27:24): DRM 2a1fa26.
- `M9X_FAA_DEPTH_QUAD=1`: the HUD views' scissored depth clear drawn as the
  vendor's colour-masked ALWAYS quad (m9emu `zclear/`). Mesa 2a6d1ac.
- Whole frame image written before the first END: DRM 8398f45.
  `faa_frame_copy_per_end` keeps the old order.
- Diagnostics:
  - `M9X_FAA_TRACE=N`: per-DRAW decision log for frame N.
  - `M9X_FAA_STOP_AFTER_LOCK=N`.
  - `M9X_FAA_DIAG_FAR`.
  - Stale-check names under `M9X_DEBUG`.

### Open

1. Interlude stall: an FAA part after ordinary jobs stops the engine (Quake3
   slots d/e/f). Things that did not cure it:
   - whole-image copy before the first END
   - page-aligning the FAA part
   With `faa_interlude_restore=1`, slot g did not stall, but every 2-DRAW FIRST
   returned -ESTALE. In every case the failing submission was "FAA frame of 2
   DRAWs": a FIRST with two texture pairs chained through a mid-frame present.
   Mesa be5bab9 removes that chain; it has not run in Quake3.
   m9unify6's `faa_context_restore` is untested with Quake3.
2. The approved but not run Quake3 group, on candidate 3
   (nano:/root/tmp/m9-integ-3):
   - settings: ts-faa, `M9X_FAA_FAMILIES=1`, `M9X_FAA_GPU_PRESENT_TEXTURED=1`,
     `M9X_FAA_TRACE=200`, BACKOFF on
   - one run with `faa_context_restore=1`, one with `faa_interlude_restore=1`
   - script: `/home/ai/tmp/agents/m9faa6/slot-c3.sh`
   Report the FAA frames, the FAA DRAWs per frame (from the trace) and
   screenshots against base and llvmpipe.
3. After a Quake3 check, make families, textured GPU present and the depth quad
   the default. Then performance: one GPU present per frame instead of a CPU
   read and write.
4. Not admitted yet:
   - FS forms beyond COLOR0*TEX0*TEX1 (census)
   - non-zero GL_CLAMP border
