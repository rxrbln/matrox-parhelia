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

| allocation clobber fixed, display buffer in bank B, ganged pipes, mip-nearest | 30.6 | 32.6 |
| same, trilinear | 28.1 | 35.5 |

The 26.4/23.5 rows are kernel `0c28239` with `pack_adjacent_packets=1` and
Mesa `4a8f9bc`; the 30.6/28.1 rows add kernel `43c0490` and Mesa `af4d810`
with the patched X server (791 frames in 25.8 s and 28.1 s, 30-38 s wall
time including the level load, no engine hangs).  The level load
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

## Display refresh competes with rendering: give it its own bank

Every fill-rate number above 230 Mpixel/s was measured with the CRTC off
(stopping X disables it).  With a 1920x1200 refresh active, where the
scanned-out image lives matters (textured 256x256 quads, depth test and
write, Mpixel/s):

| scanout location | 1:1 bilinear | 2:1 minified | 1.5:1 trilinear |
|---|---:|---:|---:|
| none (CRTC off) | 262 | 120 | 116.5 |
| dumb buffer, bank A | 182 | 83.5 | 77.6 |
| colour band, bank A (X before this change) | 179.5 | 91 | 85.7 |
| depth band, bank B | 198.6 | 120.3 | 116.5 |

Refresh out of bank B leaves the texture-bound cases untouched.  The kernel
therefore has a display class in the third 32 MiB band (bank B, below the
depth band), which Mesa requests for a resource that is both a scanout and a
front-rendering target; the X modesetting driver passes
`GBM_BO_USE_FRONT_RENDERING` for its front and rotation buffers, which is
what separates the display image from a client's shared back buffer.  Q3
gained 16% (26.4 to 30.6 fps, 23.5 to 28.1 trilinear).

## Ganged texture pipes work with the right combiner merge

The vendor places every single-stage draw of this chip family on pipe mask 5
(pipes 0+2).  The first hardware test sampled correctly only in rows 0-3 of
every 8-row band; the missing half came from combiner slot 0, which is not a
texture-address instruction but an `ADD t0, t0, t1` that merges the partial
samples of the two ganged pipes.  The P690 gang needs `ADD t0, t0, t2`
(`0x1264 = 0x1260 = 0x00008000`); each pipe returns zero for the fragments it
does not own.  With that merge every chain of `--mip-address-probe` samples
exactly (`PROBE_LX_PIPES=1`), and:

| case (1280x1024, depth write, scanout active) | single pipe | pipes 0+2 |
|---|---:|---:|
| 1:1 bilinear | 229.9 | 229.9 |
| 2:1 minified, no mips | 120.2 | 122.5 |
| 1.5:1 trilinear | 116.5 | 145.6 |

Q3 is unchanged (30.7 vs 30.6 fps): its surfaces are two-stage draws, which
already occupy pipes 0 and 2.  Evidence and vendor addresses:
`parhelia-p690-ganged-pipe-ta-re.md`.

## Object creation destroyed other objects' storage

TTM clears a newly placed object with a CPU `memset_io` over BAR0.  On the
P690 BAR0 is a translated host aperture, so those zeros landed wherever the
aperture happened to translate.  `--create-clobber-probe` showed a new
512x512 colour target wiping 18944 of 24576 dwords of a resident texture, and
the GL mipmap test failed on every second X server generation (the placement
alternates with the front buffer).  The kernel now places new objects without
CPU access and zeroes them through its own aperture window; dumb buffers are
cleared through their own mapping.  This predates the resident-object change:
before that, the same clear ran at first submit.

## Mip chain geometry (measured with `--mip-address-probe`)

* Every level reserves 16-row multiples, at a 32-texel pitch too, and the
  pitch is `align(width, 32)` texels rather than the next power of two.
* Levels are **not** 4 KiB aligned: the chain advances in 32x16-texel half
  tiles, a level occupying `(align(height,16)/16) * (pitch/32)` of them and
  the next starting where it ends.  The two halves of a 64x16 tile interleave
  every 512 bytes, so a level can begin mid-tile (0x4200 for the second level
  of 96x40).  Full derivation, measured bases and the vendor addresses:
  `parhelia-p690-mip-chain-half-tiles.md`.
* The tail starts at the first level with w <= 32 and h <= 16.  A 32-wide
  tail level sits at (0,0) with compact (pitch-32) addressing; the smaller
  levels use the tail table x {0,16,24,16,24}, y {0,0,0,8,8} shifted by
  x+32.
* Bases up to 32 texels wide use the compact pitch-32 order, wider bases the
  native pitch >= 64 order.
* The tail slot is a plain count of tail levels, exactly as the vendor chain
  builder computes it (`mtx325` `.text+0x28a6d`, `level - firstTailLevel`,
  with the first level of `w <= 16, h <= 16, d <= 31` recorded at
  `.text+0x28c43` and the three `.rodata` tables `mtx3067`/`mtx516`/`mtx1834`
  indexed by it).  It is **not** a function of the level's own size.
* **Three probe levels that looked like tail-placement failures are the
  probe's own LOD rounding.**  `--mip-address-probe` draws each level over a
  1:1 quad of that level's size with `MIP_NEAREST`, so the minification ratio
  is `max(W/lw, H/lh)` and the unit rounds `log2` of it to the nearest level.
  A level whose ratio is `1.5 * 2^k` therefore samples the *next* level and
  reports that level's address: 96x48 level 5 (48/1 = 1.5 * 32) fetches level
  6's `0x7880`, and 192x24 level 4 (24/1 = 1.5 * 16) fetches level 5's
  `0x9080`, while 96x40 - the same level widths, ratio 1.25 * 32 - is exact at
  every level.  The probe now expects the sampled level's addresses and prints
  the substitution; `PROBE_PIN_LEVEL=1` caps the descriptor's maximum level at
  the level under test so the LOD clamp forces the intended one.  Predicted
  substitutions, all at the only level with a 1.5x ratio: level 5 of every
  height-48 chain (64x48, 96x48, 128x48, 160x48, 192x48, 224x48) and level 4
  of every height-24 chain (96x24, 192x24); 96x44 (ratio 1.375) must show
  none, which brackets the rounding threshold.
* LOD bias: 0x11c0[24:16], signed 4.4 with a -4 offset (0x1c0 = bias 0).

`tools/parhelia-gl-mipmap-test.c` checks these against synthetic data
(square, non-square and narrow chains, tails, trilinear, `glGenerateMipmap`,
per-level `glGetTexImage`, `GL_TEXTURE_BASE_LEVEL` 2/3/5, scaled and flipped
blits): 0 failing checks with Mesa `4a8f9bc`.

## What the GPU feature work bought (2026-09-12)

Each of these was implemented and measured on the P690; the frame rate is the
ioquake3 `bench` timedemo at 1280x1024 with `GL_LINEAR_MIPMAP_NEAREST`.

| change | result |
|---|---|
| hardware transform on WARP, two-set route enabled | 30.1 -> **31.9 fps** (+6%); 37.2 hardware-transform draws, 15.4 not expressible and 12.1 software-clipper draws per frame |
| multi-primitive packets, 2 triangles per packet | 27.9 -> **29.6 fps** (+6%), draw packets per frame 2858 -> 1434 |
| multi-primitive packets, 8 triangles per packet | every GL test passes, but Q3 wedges during the level load: default stays at one primitive per packet |
| S3TC compressed textures (`r_ext_compressed_textures 1`) | **30.3 fps, no gain**; DXT3 and DXT5 sample exactly, two checks still fail |
| narrow texture formats (L8, 4:4:4:4, 1:5:5:5, 5:6:5) | sampled exactly; no frame-rate change |
| page flips, synchronised and asynchronous | work (75 flips/s and 5 us each); default off, see `parhelia-p690-page-flipping.md` |

The two CPU-side levers each return about 6%, and the texture-side levers
return nothing measurable.  That is consistent with the fill measurements:
a textured, depth-writing pixel runs at ~230 Mpixel/s while a colour-only
pixel runs at 655, so the frame is bound by the colour and depth traffic,
not by texture reads.  **Cutting bytes per pixel is the only remaining
route**: 16-bit colour and a 16-bit depth buffer halve both streams.

## 16-bit rendering is what moved the frame (2026-09-12)

The frame is bound by colour and depth traffic, so halving both is the only
change that attacked the real constraint.  Fill rates, 1280x1024:

| case | 32-bit | 16-bit |
|---|---:|---:|
| 2-D fill | 479.5 | **771.3** Mpixel/s |
| Z LEQUAL + write | 304.8 | **516.7** |
| blend | 302.5 | **503.4** |
| textured bilinear, depth test and write | 262.1 | **343.9** |

Colour `R5G6B5` is surface code `0x28` and depth `D16` is `0x2a`; a 16 bpp
target keeps the 4 KiB tile and doubles it to 128x16 pixels, with a bit
assignment that is not the 32 bpp one shifted down.

A 16-bit visual at first ran *slower* than 24-bit (29.0 against 31.9 fps)
because every alpha route was gated on a 32-bit colour target and Z24, so
Quake 3's alpha surfaces fell back and the triangle count tripled.  With
each route re-measured at 5:6:5/Z16 against its 32-bit reference and the
gates opened, the same demo frames run through the same routes on half the
traffic: **55.1 fps with glamor, 50.1 without**, against 35.3 and 32.0 at
depth 24.

The **hardware depth clear** (`PARHELIA_ZINIT_CLEAR=1`) then removed the
per-frame clear: a 1280x1024 Z16 clear costs 0.01 ms instead of 1.69 ms, and
per-frame clear coverage falls from 1.34 Mpixel to 0.03.  Quake 3 reached
**62.7 fps (15.9 ms)** in that agent's configuration - 1280x1024 display,
CPU governor `performance`.  Small scissored depth clears have to go through
the depth pipe as well, or an armed surface forces a full resolve every
frame and gives most of the gain back.

**Caution on all figures of that day:** independent runs reported 47 to
57 fps for the same tree depending on the display mode, the X depth, the CPU
governor and whether another agent was rebooting the target.  Measure
single-occupant, state the configuration, and prefer an A/B inside one
window over comparing numbers across runs.

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

## CPU transfers of render targets (piglit timeouts, 2026-10-02)

Thirteen core piglit tests timed out at 7 s.  At 120 s most of them were
slow but correct, and every slow one was CPU-bound in the kernel's aperture
copy (`memcpy_fromio` 78-95% of all samples).

`xferbench` (UPLOAD/DOWNLOAD_RENDER on a 256x256 native colour target,
P690 on a PCIe 1.0 x8 link, VIA Nano host), MEASURED:

| size | download | upload |
|---:|---:|---:|
| 4 B | 8.1 us | 7.1 us |
| 256 B | 36.6 us | 9.4 us |
| 4 KiB | 454 us | 46 us |
| 64 KiB | 6.8 ms (9.6 MB/s) | 0.66 ms (99 MB/s) |

* An uncached BAR0 read is one PCIe round trip, ~0.4 us per dword.  A
  read-only probe of the slot-0 window: 4-byte loads 9.8 MB/s, 8-byte loads
  19.1 MB/s, 16-byte and `movntdqa` loads no better, a write-combined
  mapping (`resource0_wc`) identical.
* The host-aperture descriptor addresses VRAM in **32-byte units**: single
  dword transfers at offsets 1..7 of a unit read dword 0 and wrote dword 0.
  The kernel published the unaligned offset itself.

Mesa moved whole tile rows (12-16 KiB) for every one-pixel `glReadPixels`
or `glCopyPixels` on a class target.  It now collects the 32-byte units of
the box, merges runs closer than one call's cost (64 bytes) and moves only
those:

| test | before | after |
|---|---:|---:|
| copypixels-sync | 108 s | 1.9 s |
| copypixels-draw-sync | 75 s | 4.0 s |
| draw-copypixels-sync | 37 s | 2.8 s |
| draw-sync | 37 s | 3.1 s |

Tests that read whole windows per subtest are bound by the read rate
itself.  The kernel now publishes sub-32-byte offsets correctly and reads
the aperture with `readq`: downloads 9.6 -> 18.5 MB/s (64 KiB calls,
MEASURED), clipflat 24 -> 17 s, texsubimage 48 -> 38 s.

`streaming-texture-leak` is bound by bus writes and cannot finish in a
reasonable time on this link.  It creates, fills, draws and deletes 5000
1024x1024 RGBA textures (20 GB of texels).  Each texture costs about
16 MB of uncached bus writes at ~100 MB/s (DERIVED from the profile):
the kernel clears both new 4 MiB objects (tiled and linear companion)
through the aperture (`memset`, ~20% of samples), the 4 MiB render-shadow upload (`memcpy_toio`, ~21%),
and the linear companion written through its mapping.  Byte-wise stores
into the mappings were replaced by texel-wide accesses and gathered row
copies: 92 -> 146 textures per 30 s (MEASURED), i.e. ~1000 s for the
whole test instead of ~1600 s.  Further gains need fewer bytes on the bus:
clearing new objects with the 2-D engine, not writing both representations
of a sampler image from the CPU, or a write-combined mapping.

The register-list compaction allocated a ~100 KiB scratch batch per draw
and scanned the relocations per packet (25% of longprim); it now compacts
in place: longprim 11.0 -> 8.4 s.

## Known issues found while measuring

* `trace_vram_reloc=1` hangs the first submit after module load.
* A valid draw can take longer than the old 50 ms asynchronous hang budget
  (a 4:1 minified full-screen quad without mipmaps needs 53 ms) and the
  device was declared hung in the middle of a measurement.  The budget is
  now 2 s per fence section (`hang_timeout_ms`).
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
  `PROBE_TEX_FLAGS`, `PROBE_TEX_Z`, `PROBE_PATCH=OLD:NEW[,OLD:NEW...]`,
  `PROBE_LX_PIPES`, and `PROBE_KMS`/`PROBE_KMS_FLAGS` to keep a scanout of a
  chosen allocation class active while measuring.
* `parhelia-drm-test --create-clobber-probe DEV`: allocation side effects on
  other objects' storage (`PROBE_PAD_KIB`, `PROBE_CLOBBER_CLOSE`).
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
