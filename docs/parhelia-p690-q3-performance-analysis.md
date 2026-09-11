# P690 Quake 3 performance analysis and benchmark log

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

All numbers were measured on the P690 target (192.168.1.111) with the
open kernel module staged under `/tmp` and the `-O2` development Mesa build.
Fill-rate and packet costs come from `parhelia-drm-test` probes; frame times
come from the ioquake3 opengl1 renderer.

## Benchmark setup

`/root/q3bench.sh` runs the timedemo `bench` (q3dm7, bots, 791 frames,
recorded as `demos/bench.dm_71`) at 1280x1024, windowed, with:

```
+set cl_renderer opengl1 +set r_mode -1 +set r_customwidth 1280
+set r_customheight 1024 +set r_swapInterval 0 +set com_maxfps 0
+set r_picmip 0 +set r_texturebits 32 +set r_textureMode GL_LINEAR_MIPMAP_LINEAR
+set r_vertexLight 0 +set r_ext_multitexture 1 +set r_ext_compressed_textures 0
+set r_lodbias 0 +set timedemo 1 +demo bench
```

Environment: `LD_LIBRARY_PATH=/root/parhelia-mip/lib:/root/parhelia-mesa/lib64`,
`MESA_LOADER_DRIVER_OVERRIDE=parhelia`, `vblank_mode=0`.

## Frame rate history (1280x1024, same demo)

| state | fps | ms/frame |
|---|---:|---:|
| start of this work | 18.2 | 55 |
| progress-based hang detection, fence poll, colour in bank A | 20.9 | 48 |
| + kernel `pack_adjacent_packets=1` | 21.6 | 46.4 |
| + `PARHELIA_BATCH_STATE_STREAM=1` | 22.3 | 44.9 |
| mip levels actually defined (level-aware copy), trilinear | 23.5 | 42.5 |
| same, `r_textureMode GL_LINEAR_MIPMAP_NEAREST` | 26.4 | 37.9 |
| same + `PARHELIA_BATCH_STATE_STREAM=1` | 28.0 | 35.7 |
| textured WARP program writing T2 (locks up vertex-alpha draws, reverted) | 28.9 | 34.6 |
| T2 priming draw, resident BOs, measured mip geometry, deferred uploads, trilinear | 23.5 | 42.5 |
| same, `GL_LINEAR_MIPMAP_NEAREST` | 26.4 | 37.9 |

The last two rows are the committed state (kernel `0c28239` with
`pack_adjacent_packets=1`, Mesa `4a8f9bc`): 791 frames in 30.0 s and 33.6 s,
35-38 s wall time including the level load, no engine hangs.  The level load
took 92-142 s before texture uploads were deferred to the first draw (each
level copy re-uploaded the whole object).

640x480 reached 46.6 fps at the start of this work.

## Where a frame goes

`PARHELIA_STATS=1` (new) prints per-frame averages over 100 frames:

```
43.36 ms, 87 batches (83 draw), 2268 triangles, 87 submits,
66394 vertex dwords, 4.0 clears (1.35 Mpixel), 83.2 waits 13.42 ms
```

* The CPU is not the limit: the ioquake3 process used about 30% of one core
  (perf over 10 s: 87% of all samples idle).  Main-thread stack samples:
  29/80 in the vertex-ring slot fence wait, 17/80 in `poll` (present path),
  the rest spread over tgsi_exec, draw and command building.
* The kernel's `gpu_busy_ns` accumulated about 100% over the same window:
  the GPU is saturated.
* Q3 issues only ~90 state batches and ~2,300-3,000 triangles per frame.

`PARHELIA_STATS=sync` (new) additionally waits every submit and charges its
submit-to-fence time to a batch kind; pixel counts are screen-space triangle
areas before the depth test.  Typical frame (trilinear, mips defined):

| kind | submits | GPU ms | Mpixel | Mpixel/s |
|---|---:|---:|---:|---:|
| clear | 3.0 | 3.83 | 1.34 | 348 |
| tex-tiled (no mips) | 7.6 | 4.24 | 0.79 | 186 |
| tex-mip | 17.1 | 12.35 | 1.41 | 114 |
| multitex (diffuse x lightmap) | 19.0 | 12.71 | 1.34 | 105 |
| alpha-two-pass | 22.4 | 4.51 | 0.31 | 69 |

Before the mip fix nearly all textured batches were `tex-tiled` at 110-140
Mpixel/s; with `GL_LINEAR_MIPMAP_NEAREST` tex-mip rises only to ~120-130 and
multitex to ~115-160 Mpixel/s.  The sum of GPU time is the frame time: the
frame is fill (memory bandwidth) bound.

## Command-stream costs

`parhelia-drm-test --fill-rate-probe /dev/dri/card0 COUNT` (extended):

| case | cost |
|---|---:|
| small triangle (8x8) inside one state batch | 0.40 us |
| one complete state batch per triangle (15-16 packets, 124-128 dwords) | 31-34 us |
| 10 state batches concatenated per submit | 18 us per batch |
| one batch per triangle, kernel `pack_adjacent_packets=1` | 16.8-17.6 us |
| 10 per submit, `pack_adjacent_packets=1` | 6.0-7.3 us per batch |

Pixel coverage (5600 lit pixels for 200 triangles) was verified for every
packed/aggregated variant.  Each packet of a state batch otherwise costs one
secondary CALL fetch; packing plus aggregation cuts the per-state cost by
about 4x.  For Q3 this is worth only ~1-2 ms/frame because the frame is fill
bound.

### Multi-triangle 0x97 draw packets do not work on the P690

With `PROBE_GROUP=N`, N triangles share one 0x97 range:

* N=2 draws only the first triangle of every packet (2800 of 5600 pixels),
  with 5-dword and with 6-dword (Z) vertices.
* `PRIMWORD` 0x0002305c instead of 0x0006305c (bit 18) changes nothing.
* N=4 hangs the engine; a 6-vertex triangle strip (0x0006405c) hangs.
* The older mode-3 "depth interlock is per packet" result used identical
  near/far triangles; its far-first failure is equally explained by the
  second triangle of the range simply not being drawn.  The P690 currently
  draws exactly one primitive per 0x97/0x8f packet; the 0528 vendor captures
  (tri8/flat: 24 vertices per packet) show the old chip consumed whole
  ranges.  What state makes the P690 consume more than one primitive is open.

## Fill rates

`--fill-rate-probe`, 1280x1024 native targets, colour at [32,64) MiB (bank A),
depth at [96,128) MiB (bank B):

| mode | Mpixel/s |
|---|---:|
| 2D solid fill | 468 |
| 3D colour, no Z | 655 |
| Z LEQUAL + write | 298 |
| Z LEQUAL no write | 655 |
| Z LESS reject | 655 |
| blend, no Z | 298 |
| blend + Z no write | 298 |

With colour and depth both in bank B the depth-tested rates were 202-185
Mpixel/s, which is why colour moved to bank A.  Any read-modify-write stream
(depth write, blending) caps at ~298 Mpixel/s; Z rejection is not early.

### Textured fill (`--texture-fill-probe`, new)

256x256 32-bit texture with its full mip chain, full-screen quads:

| sampling | tex bank A (default) | tex in depth band (B) | tex in colour band |
|---|---:|---:|---:|
| 1:1 bilinear, no Z | 262 | 546 | 222 |
| 1:1 bilinear, Z write | 262 | 149 | - |

| sampling (bank A, Z write) | Mpixel/s |
|---|---:|
| 1:1 nearest | 212 |
| 1:1 bilinear | 262 |
| 2:1 minified, no mips | 119 |
| 4:1 minified, no mips | 38 |
| 1:4 magnified | 298 (546 without Z) |
| 4:1 bilinear mip-nearest | 262 |
| 16:1 bilinear mip-nearest | 656 |
| 1.5:1 trilinear | 116 |
| 4:1 trilinear (integer LOD) | 262 |

* Without mips minification collapses (38 Mpixel/s at 4:1); with mips it
  stays at the 1:1 rate.
* Trilinear between two levels halves the rate.
* The two 64 MiB halves behave as independent memory channels: textures in
  bank A with colour and Z in bank B is the balanced placement once depth
  writes are involved (262 vs 149).
* `MemLevels` (0x12b0) 0x3fff8842 vs 0x3fff9084 and `TEDLFCounters`
  (0x0bc0, new `tedlf_counters=` module parameter) 0x4087 vs 0x1e4087 made no
  measurable difference.

## Mip chain geometry (measured with `--mip-address-probe`)

* Levels with a pitch of 64 or more reserve 16-row multiples, pitch-32 levels
  32-row multiples; every level starts 4 KiB aligned.
* The tail starts at the first level with w <= 32 and h <= 16.  A 32-wide
  tail level sits at (0,0) with compact (pitch-32) addressing; the smaller
  levels use the tail table x {0,16,24,16,24}, y {0,0,0,8,8} shifted by
  x+32.
* Bases up to 32 texels wide use the compact pitch-32 order, wider bases the
  native pitch >= 64 order.
* LOD bias: 0x11c0[24:16], signed 4.4 with a -4 offset (0x1c0 = bias 0).

`tools/parhelia-gl-mipmap-test.c` checks these against synthetic data
(square, non-square and narrow chains, tails, trilinear, `glGenerateMipmap`,
per-level `glGetTexImage`, `GL_TEXTURE_BASE_LEVEL` 2/3/5, scaled and flipped
blits): 0 failing checks with Mesa `4a8f9bc`.

## Assessment of the 60 fps goal

A textured, depth-writing pixel costs ~16 bytes of memory traffic split over
the two banks; ~262 Mpixel/s is the resulting ceiling with 32-bit textures and
one texture pipe.  Q3 at 1280x1024 draws ~4-5 Mpixel of textured fragments
per frame (overdraw included), so fill alone needs ~15-19 ms at the ceiling,
plus ~3.8 ms of clears, and trilinear blending doubles the texture part.
60 fps at these settings needs either more texture throughput (the vendor's
P690 route gangs pipes 0+2 for one texture, see
`parhelia-p690-texture-pipes-re.md`) or less traffic per pixel.  Z
compression and fast clear exist but ship disabled on the P690 in the vendor
driver (`parhelia-p690-zcompression-clear-banks-re.md`).

## Known issues found while measuring

* `trace_vram_reloc=1` hangs the first submit after module load.
* After the engine hung during a module reload, every following warm reboot
  hung the first 3D draw (fence 0x30 after a completed 2D clear,
  `BM=03000000 908=db8fa047 924=0020100a`), also with the previously working
  module and parameters; after a cold power cycle every draw was rejected.
  **Root cause: WARP temporary T2.**  Draws depend on T2, which holds garbage
  after power-on and a stale value after a warm reboot until a program writes
  it.  The replace, passthrough and multitexture programs now write it
  (`mul T2, v5, r1.0000 | mov o11, v0`, kernel `aa7afa1`); the textured
  program must not (with the write, the first 9-dword RGBA vertex-alpha batch
  of the demo, submit 58224, locked up the engine every run; kernel
  `0c28239`).  Mesa primes T2 with one degenerate passthrough draw per
  context (`ph_prime_warp()`).  Initializing it from the kernel at load time
  through WARP loader mode 8 or draw mode 2 did not work.
* Textures sampled garbage past their first 16 KiB whenever X was running:
  TTM evicted and moved VRAM BOs with a CPU memcpy through BAR0, which on the
  P690 is a translated host aperture, so moved contents were corrupted.
  Render and texture BOs are resident on P690 now (kernel `0a3ecce`).
  Hypotheses measured and rejected first: DCM priorities, 0x1c24, texture
  unit priming (MMIO and command stream), 0x10fc/0x1330, the combiner entry,
  the WARP invalidate bit, aperture slot validity, host aperture readback
  drain, VRAM reservation, scanout activity and a second DRM client.
* ioquake3 can block in `XGrabPointer` during shutdown after SIGTERM.

## Tools added

* `parhelia-drm-test --fill-rate-probe DEV [COUNT]` with `PROBE_GROUP`,
  `PROBE_TRI_Z`, `PROBE_PRIMWORD`, `PROBE_BOUNDED`, `PROBE_AGG`,
  `PROBE_COLOUR_FLAGS`, `PROBE_DEPTH_FLAGS`.
* `parhelia-drm-test --texture-fill-probe DEV [COUNT]` with `PROBE_TEX`,
  `PROBE_TEX_FLAGS`, `PROBE_TEX_Z`, `PROBE_PATCH=OLD:NEW`.
* `parhelia-drm-test --mip-address-probe DEV W H`: samples every level of a
  chain filled with self-indexing dwords and reports the fetched dword per
  texel against the Gallium layout.
* `tools/parhelia-gl-mipmap-test.c`: synthetic GL mipmap/blit conformance.
* `--mip-address-probe` options: `PROBE_MIP_CONTENT`, `PROBE_STALE`,
  `PROBE_PAD_KIB`, `PROBE_COLOUR_PAD_KIB`, `PROBE_PAUSE_S`, `PROBE_KMS`,
  `PROBE_HOLD_DUMB=WxH`, `PROBE_SINGLE`.
* `--transfer-probe`, `--draw-sequence-probe` (`PROBE_PRIME`, `PROBE_SEQ_TEX`,
  `PROBE_SEQ_REPLACE`, `PROBE_SEQ_ENV`, `PROBE_SEQ_DEGENERATE`, `PROBE_DUMP`).
* Mesa `PARHELIA_STATS=1|sync`, `PARHELIA_TRACE_MIPS=1`, `PARHELIA_NO_MIPS=1`,
  `PARHELIA_DUMP_BATCHES`, and the hang recorder (`/tmp/parhelia-hang-PID.txt`
  or `PARHELIA_HANG_DUMP`: the last 16 batches with vertices).
* `/root/parhelia-mmio REG[=VAL]|START-END` (source
  `/home/ai/src/parhelia/tools/parhelia-mmio.c`): BAR2 register access.
