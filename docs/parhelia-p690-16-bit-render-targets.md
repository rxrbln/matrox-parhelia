# 16-bit render targets on the P690: format codes, storage order, fill rate

Measured 2026-09-12 on the P690 target with `parhelia-drm-test
--narrow-target-probe`, `--fill-rate-probe` and `--texture-fill-probe`.
Everything below marked MEASURED was read back from the hardware; the format
codes themselves come from the vendor binaries and are cited.

## 1. The format codes

The surface descriptor at `0x1220 + 8*slot + 4` is
`(pitch_in_pixels & 0x3fff) | (type << 16) | (format << 24)`.

| surface | code | source |
|---|---|---|
| A8R8G8B8 colour | `0x32` | already in use |
| S8Z24 depth | `0x36` | already in use |
| **R5G6B5 colour** | **`0x28`** | record (internal format `0x0a`, usage 1 = render target); Linux `mtx_parhelia.o` `mtx1646` @`0x7ca0a` writes `ecx=0x28, edx=1, esi=0x0a` |
| **D16 depth** | **`0x2a`** | record (internal format `0x11`, usage 4 = Z buffer); `mtxpserd.dll` `.data 0x1e3148`, Linux `mtx1646` @`0x7fd26` writes `ecx=0x2a, edx=4, esi=0x11` |

The records are 0x34-byte rows `{flags, internal format, usage, hw code, …}`
written by the static initialiser `MTXPARD.dll 0x14f10` / `mtxpserd.dll
0x14f20` at `.data 0x1de5d0` / `0x1e05d0` (247 rows, count stored at
`0x1e3710`); the Linux driver builds the same table in `mtx1646` @`0x7c050`.
The anchor is proven the same way: S8Z24 is internal format `0x13`, usage 4,
code **`0x36`** (`mtxpserd .data 0x1e27f0`, Linux `mtx1646` @`0x7f0be`).
D3DFORMAT maps to the internal format at `MTXPARD 0x83af0` (byte table
`0x83cac`, jump table `0x83c48`): R5G6B5 (23) → `0x0a`, D16 (80) → `0x11`,
D24S8 (75) → `0x13`.  D15S1 (`0x12`) → `0x2b`; the "2-D view" (usage 0) and
"colour view" (usage 1/7) columns for D16 are `0x20` and `0x28`, which is what
makes `0x28` the generic 16 bpp code.  The Linux bpp classifier `mtx969`
@`0x5b2e0` puts `0x2a000000` in the group returning 16 (`0x5b376`) and
`0x36000000` in the one returning 32 (`0x5b3e2`).

The packed depth clear value comes from `mtxpserd.dll` `0x9c220` (`jmp
[eax*4+0x9c5e8]`, `eax = fmt - 0x11`; twin `MTXPARD 0x9ac89`, table
`0x9b038`): D16 is `z16 | z16<<16` with `z16 = min(d * 65536, 0xffff)` (float
constant `0x19c9e0` = 65536.0), against `min(d * 16777216, 0xffffff) |
stencil<<24` for S8Z24.  `0x9c010`/table `0x9c068` (`MTXPARD 0x9aa60`, table
`0x9aab8`) gives the stencil bit count written to `0x12c8[20:17]`: `0x11`
(D16) → **0**, `0x12` → 1, `0x13` (S8Z24) → 8.  The D16 clear arm also sets
the plane mask to `0xffffffff` (`0x9ad2c`) where S8Z24 uses `0x00ffffff` /
`stencil_mask<<24` (`0x9aee5`).

The same table gives the other 16 bpp colour codes, none of which this driver
drives yet: A4R4G4B4/X4R4G4B4 internal `0x06`/`0x07` → render target `0x24`
(sampler `0x24`/`0x25`), A1R5G5B5/X1R5G5B5 internal `0x08`/`0x09` → render
target `0x26` (sampler `0x26`/`0x27`).  Linux witnesses `mtx1646` @`0x7d0e1`,
`0x7d684`, `0x7cc87`, `0x7ce14`.

MEASURED: with `0x12c8[20:17] = 0` and a `0x2a` depth surface the depth test,
its rejection and its write are exact, and the stored halfword after a quad at
window depth 0.25 is `0x4000` on every one of 131072 pixels — exactly
`0.25 * 65536`, the vendor's packing.

## 2. Storage order (MEASURED)

A 16 bpp render target keeps the P690's 4096-byte, 16-row tile and doubles its
width: **128 x 16 pixels per 4 KiB tile**, `tiles per row = pitch / 128`.

The probe writes one 1x1 2-D fill per pixel over 128x64 pixels, each carrying
its own coordinates, then reads the surface back and inverts the mapping.  All
8192 pixels are placed, with no duplicates and no stray halfwords.  The
resulting in-tile **byte** index is

```
bit 1  = x bit 0        bit 7  = y bit 1
bit 2  = x bit 1        bit 8  = y bit 2
bit 3  = x bit 2        bit 9  = x bit 5 XOR tile-row parity
bit 4  = x bit 3        bit 10 = y bit 3
bit 5  = y bit 0        bit 11 = x bit 6
bit 6  = x bit 4
```

`offset = (trow * tiles_per_row + tcol) * 4096 + index`, `trow = y / 16`,
`tcol = x / 128`.  The tile-row parity on x bit 5 is the same term every other
Parhelia surface order carries.  This is **not** the 32 bpp order shifted down
by one: at 32 bpp the byte bits are
`2=x0 3=x1 4=x2 5=y0 6=x3 7=x4 8=y1 9=x5^p 10=y2 11=y3`, so x and y bits are
interleaved differently.

Implemented as `p3d_tile_offset_16bpp()` / `p3d_tile_position_16bpp()` and
selected by `p3d_tile_offset_layout_cpp(..., cpp)`.  The Mesa host selftest
replays the measured offsets.

The vendor's tile-index arithmetic agrees on the coarse figures without
giving the intra-tile swizzle: the layout 2/3 handler `MTXPARD 0x304a0`
selects a granule shift from bits per pixel (`0x30653` 32 bpp → 4096 B,
`0x30667` 16 bpp → 2048 B, `0x3067b` 8 bpp → 1024 B, each one less when
`desc+0x10 & 0x7fc00`, which also halves the tile height from 32 to 16 rows —
the P690 case), and applies the same tile-row parity XOR on the granule
column.  16 bpp with that flag is a 1024-byte granule of 16 rows, so the
parity-paired tile is the 4096 bytes measured above.  The bit assignment
inside it was measured, not derived.

## 3. What else changes at 16 bpp (MEASURED)

* **The 2-D fill takes a 32-bit FCOL.**  `FCOL = 0x0000abcd` leaves `abcd` in
  half the halfwords and zero in the other half; `FCOL = 0xabcdabcd` fills all
  131072 halfwords with `abcd`.  A 16-bit clear value must be replicated into
  both halves.  That is the vendor's own rule: the solid-rect fill
  `MTXPARD 0x110420` switches on bits per pixel and the 16 bpp arm at
  `0x110537` builds `c | c<<16` before storing FCOL `0x1314` (Linux twin
  `mtx1637` @`0x23c20`, arm `0x2420c`).  The fill *rectangle* is still
  honoured at 16 bpp pixel granularity: the 8192 single-pixel fills of the
  order sweep each wrote exactly one halfword.
* **PLNWT `0x1324` masks that same 32-bit word**, so a 5:6:5 channel mask is
  `(R 0xf800 | G 0x07e0 | B 0x001f)` replicated into both halves.  The vendor
  has a dedicated helper for exactly this, `MTXPARD 0x9b320` (Linux `mtx3347`
  @`0xffc0`): two bytes per pixel → `m &= 0xffff; m |= m<<16`, one byte →
  `m * 0x01010101`, four bytes → verbatim.  MEASURED: a white quad through the
  red plane alone leaves `f800`, and a second white quad through the green
  plane alone leaves `ffe0`, on all 40960 pixels.
* **The pitch field stays in pixels** and aligns to the 128-pixel tile width.
  The vendor derives bytes from it the same way: `MTXPARD 0xa40f0` (Linux
  `mtx3441` @`0x5ba40`) rounds the `cfg & 0x3fff` pitch up to 32 and
  multiplies by bits per pixel, and the alignment helper `0xa3b10` (Linux
  `mtx3496` @`0x5b7e0`) is `mov $0x20,%eax; ret` for every layout and bpp.
  Colour and depth surfaces may have **different** pitches: MEASURED with a
  320-wide 16 bpp colour target (pitch 384) against a 320-wide 32-bit depth
  target (pitch 320), the depth test, rejection and acceptance are exact.
* **3-D colour output is exact 5:6:5**: red `f800`, green `07e0`, blue `001f`,
  white `ffff` over a whole 512x256 target, all 131072 pixels each time.
* Nothing else in the pixel pipeline changes.  `0x12c8/0x12cc/0x12d0` are the
  ordinary depth/stencil control, stencil mask/ref and write mask; only the
  stencil-bit-count field `0x12c8[20:17]` differs.

## 4. Fill rate, 1280x1024, 60 quads or 20 textured quads

`--fill-rate-probe`, colour and depth both native classes:

| mode | 32-bit | 16-bit | gain |
|---|---|---|---|
| 2-D solid fill (a clear) | 479.5 Mpixel/s | **771.3** | +61% |
| 3-D colour, no Z | 666.4 | 688.4 | +3% |
| Z LEQUAL + write, pass | 304.8 | **516.7** | +70% |
| Z LEQUAL no write, pass | 655.4 | 688.5 | +5% |
| Z LESS reject | 666.4 | 688.5 | +3% |
| blend, no Z | 302.5 | **503.4** | +66% |
| blend + Z LEQUAL no write | 302.5 | **503.4** | +66% |

`--texture-fill-probe`, 256x256 mip chain, depth test and depth write on
(`PROBE_TEX_Z=1`):

| mode | 32/32 | 16 colour/32 Z | 32 colour/16 Z | 16/16 |
|---|---|---|---|---|
| 1:1 nearest | 285.0 | 298.0 | 284.6 | **388.3** |
| 1:1 bilinear | 262.1 | 297.9 | 261.6 | **343.9** |
| 2:1 minified bilinear | 120.2 | 136.5 | 120.1 | 136.4 |
| 4:1 minified bilinear | 37.8 | — | — | 39.4 |
| 16:1 bilinear mip-nearest | 297.9 | — | — | **502.1** |
| 4:1 trilinear | 262.1 | — | — | **344.0** |

Reading these: a 16-bit **depth** buffer alone buys nothing (261.6 vs 262.1).
The colour traffic is what the read-modify-write stream is waiting on, and only
halving both gets the full 31% on a textured, depth-tested, depth-writing fill
and 69% on the pure colour+depth stream.  A 4:1 minified fill is texture bound
and does not move.

## 5. Kernel allocation

`drm_parhelia_create_render` gained a `cpp` field in the former `pad` slot
(0 and 4 are the 32-bit formats, 2 is R5G6B5/D16, everything else `-EINVAL`).
The kernel aligns the pitch to `64 * 4 / cpp` pixels, keeps the 16-row tile
alignment, and returns a byte pitch.  `cpp = 2` is refused for the texture
class (the sampler has its own narrow order) and on any chip that is not the
64x16/4 KiB layout, since that is all that has been measured.

## 5a. The scanout path (MEASURED)

`DC_FORMAT` already encoded `0x28` for RGB565; the plane, the framebuffer
object and the dumb allocation did not.  With those three opened up:

* `--narrow-scanout-probe` 3-D draws eight 5:6:5 colour bars into a native
  scanout object, publishes it with `ADDFB2 DRM_FORMAT_RGB565` and modesets
  onto it.  At 1920x1200: **0 of 2304000 pixels differ** from the model, the
  active head's `DC_CTRL` reads `0x00040201`, `DC_START` `0x08000000` (the
  bank-B display band) and **`DC_FORMAT` `0x28030780`** — format `0x28`,
  type 3, pitch `0x780` = 1920 pixels.  `--scanout-dump`, now format aware,
  recovers all eight bars through a GPU copy of what the CRTC scans.
* The 16 bpp host-aperture code is **`0x20`**, the vendor's "2-D view" entry
  for every 16 bpp format.  `--narrow-dumb-probe` writes one distinct
  halfword per pixel through a 16 bpp dumb buffer's CPU mapping and reads the
  object back with the 2-D engine: **all 32768 land exactly where the measured
  16 bpp tile order says.**
* The X modesetting driver's native Present/DRI2 BITBLT takes the same
  two-byte 2-D view (`0x20030000 | pitch_in_pixels`);
  `xserver-modesetting-16bpp-present-blit.patch` carries that change.

With those in place an X server starts at **depth 16** with DRI3 and direct
rendering, glxgears runs, and ioquake3 gets a real
`color(16-bits) Z(16-bit) stencil(0-bits)` visual.

## 5b. The alpha routes at 16 bits (MEASURED)

Every alpha route in the Gallium driver was gated on a `B8G8R8A8/X8` colour
target **and** a `Z24_UNORM_S8_UINT` depth buffer.  Nothing in them looks at
the target's format: they drive the blend unit, the alpha comparator and the
depth compare, all of which sit behind the fragment pipeline.
`--narrow-alpha-probe` runs each live form into a 32-bit target with Z24 and
into a 5:6:5 target with Z16 and compares, taking the 32-bit result as the
reference:

| form | agreement |
|---|---|
| vertex alpha source-over, no depth | 0/36864 pixels off by more than one code |
| vertex alpha source-over, depth | 0/36864 |
| vertex alpha additive, depth | 0/36864 |
| sampled-alpha companion source-over | 0/36864 |
| sampled-alpha companion cutout, depth test + write | 0/36864 |
| sampled-alpha companion cutout, rejected | 0/36864, coverage 0 on both |
| two-pass attenuation `ZERO`/`1-SRC_COLOR`, alpha plane masked | 0/36864 |
| two-pass additive `ONE`/`ONE`, depth | 0/36864 |

Coverage is identical in every case.  The gates therefore became
`ph_alpha_route_colour()` (B8G8R8A8, B8G8R8X8 or B5G6R5) and
`ph_alpha_route_depth()` (Z24S8 or Z16), and the two-pass route's "a target
without destination alpha makes the two passes exact" shortcut became
`ph_target_no_dest_alpha()`, which 5:6:5 satisfies: it has no alpha channel,
and `p3d_colour_mask()` already drops that route's `mask_a` for it.

`ph_use_cpu_alpha_cutout()` stays gated to the 32-bit formats **on purpose**:
it rasterizes on the CPU into the linear companion images and reads and writes
a 32-bit pixel and a 32-bit Z24 word directly (`ph_cpu_linear_pixel()`,
`ph_cpu_depth_pass()`), neither of which a 16 bpp target has.  It is only a
fallback under the native sampled-alpha route, which now covers 5:6:5.

## 5c. ioquake3 at 16 bits (MEASURED)

1280x1024 `bench` timedemo, `GL_LINEAR_MIPMAP_NEAREST`, same build, same
session, ioquake3 reporting `color(16-bits) Z(16-bit) stencil(0-bits)` on the
depth-16 screen and `color(24-bits) Z(24-bit) stencil(8-bits)` on the other:

| X screen | without glamor | with glamor |
|---|---|---|
| depth 24 | 32.0 fps | 35.3 fps |
| depth 16 | **50.1 fps** | **55.1 fps** |
| gain | **+57%** | **+56%** |

(50.0 / 50.2 / 50.1 over three depth-16 runs.)  Before the gates were opened
the same 16-bit visual ran 29.0 fps, *slower* than 24-bit.

`PARHELIA_STATS=1` shows the fallbacks are gone.  On frame-for-frame identical
demo frames (the hardware-transform counters line up exactly):

| | depth 24 | depth 16, gates closed | depth 16, gates open |
|---|---|---|---|
| triangles per frame | 2046 / 2593 | 7509 – 9473 | **2046 / 2593** |
| submits per frame | 8 / 10 | 22 – 30 | **8 / 10** |
| frame time | 28.45 / 32.90 ms | 33 – 38 ms | **18.43 / 21.38 ms** |
| fence wait | 3.87 / 6.62 ms | 7.3 – 9.6 ms | **2.11 / 3.80 ms** |

The geometry and submission counts at depth 16 are now exactly those at depth
24: the same draws, through the same routes, on half the memory traffic.  A
scanout dump of a live 16-bit frame (GPU copy of what the CRTC scans) shows a
correct picture with the alpha-blended HUD, crosshair and weapon intact, both
with and without glamor.

### glamor

glamor had been failing to start, which is why the first measurements ran
without it.  It is **not** related to the pixel depth: it fails identically at
depth 24 and depth 16 with "Failed to get GLSL version", because the driver
reports GL 1.5 and no GLSL version at all, while glamor needs a 2.1 context.
`MESA_GL_VERSION_OVERRIDE=2.1 MESA_GLSL_VERSION_OVERRIDE=120` brings it up at
both depths (`restart-xg.sh`, `restart-x16g.sh` on the target); the driver's
own caps still advertise only what it can lower.  The 16-bit gain is the same
either way.

## 6. What is implemented and what is not

Implemented and validated end to end: `PIPE_FORMAT_B5G6R5_UNORM` render
targets and `PIPE_FORMAT_Z16_UNORM` depth buffers — allocation, clear (whole,
scissored and channel-masked), 3-D draws, the depth test at the format's own
precision, CPU transfer/readback and the `ph_blit` conversion path.
`parhelia-gl-16bit-test` exercises all of that through GL renderbuffers in an
FBO: 0 failing checks.

Also implemented and validated: the 16 bpp scanout path of §5a — the KMS plane,
the RGB565 framebuffer, the 16 bpp dumb buffer and its host aperture, the
X server's native Present BITBLT, and B5G6R5 for `SCANOUT`,
`DISPLAY_TARGET` and `SHARED` in `ph_format_supported`.

Also implemented and validated: the hardware alpha routes on a 5:6:5 target
with a Z16 buffer (§5b), which is what turns the fill-rate win into 50.1 fps
in Quake 3 (§5c).

Not implemented, and not currently needed: a 16 bpp **linear companion**.  A
resource that is shared, scanned out or a display target takes the
`native_share` path, which allocates no companion at any pixel size, so a
depth-16 GLX window's colour buffer is a plain native B5G6R5 surface and its
depth buffer a native Z16 one (traced: `bind=0x18000a`, `create_flags=0x8`,
`tiled_pitch_px=256`).  `ph_copy_present_quad()` needs a companion on the
source and therefore never runs on this configuration — traced over a live
glxgears at both depths, zero hits — so its four-byte guard is not what keeps
it off, and widening it would be untested code with no measured effect.

`ph_use_cpu_alpha_cutout()` remains 32-bit only, by design (§5b).

One resource cannot be both a 16 bpp sampler image and a 16 bpp render target:
the two storage orders differ.  `ph_format_supported` refuses the combination,
and `ph_resource_create` routes a B5G6R5 resource bound as a render target to
render-target storage (Mesa puts `PIPE_BIND_SAMPLER_VIEW` on every
renderbuffer).

Copyright 2026 René Rebe <rene@exactco.de>
