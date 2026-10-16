<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9148 vendor GL2 extensions, 2D/EXA, XVideo and display-mode capture

This note records the September 15, 2026 capture campaign covering the M9148
vendor stack paths **outside** the two existing campaigns (the GL 1.5
fixed-function/texture sets and the GLSL/native-program corpus). Target:
`root@192.168.1.56`, Linux 2.6.30, Xorg 1.5.2, vendor `m9x` module and
`m9x_drv.so` (X 7.4 build, SHA-256 `17b8d8c0…`), 1920x1200 on `mon0`.

Captures live in
`captures/m9x-vendor-gl2ext-2d-video-modes-20260915/` (README manifest there).
Evidence labels:

* **CAPTURED** read from a traced ExecuteDma/command-buffer/VRAM image;
* **MEASURED** read back from live hardware (MMIO snapshot or GL readback);
* **STATIC** from an earlier hash-pinned binary note.

## Method and what was chosen per group

Every GPU/MMIO step ran inside `flock -w 1800 /tmp/m9cap2.lock` with
`exec 3>&-`, one operation per locked section, on the shared box.

* **Group A (GL2 extensions)** used the sanctioned direct-GL corpus harness
  (`tools/m9x-gl2ext-corpus.{c,sh}`, patterned on the GLSL corpus): a separate
  direct client under `m9x-exec-trace.so`, C2 read through host-aperture slot 0.
  This never touches the X server. Decoded with the existing
  `tools/m9-glsl-program-extract.py` / `m9-native-disasm.py`.
* **Group D (display)** used a new read-only full-MMIO snapshot tool
  (`tools/m9x-mmio-snapshot.c`, `tools/m9x-mmio-diff.py`) taken before/after
  each operation, plus a small Xlib driver (`tools/m9x-x-display-probe.c`) for
  cursor/gamma/DPMS/warp, and a host-aperture dump of the cursor VRAM image.
  This is the lowest-risk observation method and it is decisive for the
  persistent CRTC/cursor register state.
* **Group C (XVideo)** used a new Xv client (`tools/m9x-xv-probe.c`) for
  enumeration (xvinfo is absent) plus before/after MMIO snapshots around
  `XvPutImage`.
* **Group B (EXA)** was driven with a new XRender/Xlib client
  (`tools/m9x-exa-driver.c`) and observed by MMIO snapshot. The full DDX
  ExecuteDma **command stream** was **not** captured: it requires either an X
  restart with the `m9x-exec-trace.so` preload (unsafe while sharing the box)
  or a live-X gdb ExecuteDma dump (attempted, blocked by the safety
  classifier). The static EXA encoding in `parhelia-m9-accel.md` stands.

## Group D — display mode programming (new, MEASURED)

The M9 display registers are in PCI **resource2** (`febfe000`, 8 KiB). A full
2048-dword snapshot diffed around each `xrandr --output mon0 --mode …` isolates
the exact per-mode register set. Restoring 1920x1200 left only engine pointers,
the cursor position, and `dc0_misc` bit 0 changed — the mode registers are
fully restored, confirming the set below is complete. **MEASURED.**

Per-head base for head 0 is `0x1800` (DC, scanout) and `0x1900` (DCT, timing).
Heads 1/2/3 are at `0x1a00`/`0x1980`/`0x1b80` (DC) and
`0x1b00`/`0x1e00`/`0x1f00` (DCT).

### Scanout (DC0, base 0x1800)

| offset | name | encoding (MEASURED across 6 modes) |
|---:|---|---|
| `0x180c` | `dc0_xend` | `(hactive-1) << 16` |
| `0x1818` | `dc0_yend` | `vactive-1` |
| `0x1848` | `dc0_start` | scanout base GPU address |
| `0x1858` | `dc0_pitch` | `format8<<24 | 0x00210000 | (width_pixels & 0xffff)`; format `0xcc` = 32 bpp |

`dc0_pitch` low 16 bits are the width in pixels (`0x780`=1920 … `0x280`=640);
this matches the render-surface descriptor format field (`0xcc` for
BGRA8888) documented in `parhelia-m9-render-surfaces-clear.md`.

### CRTC timing (DCT0, base 0x1900)

All timing registers pack two 16-bit fields as `high<<16 | low`. Verified on
1920x1200, 1600x1200, 1680x1050, 1400x1050, 1280x1024, 1024x768, 640x480
against the xrandr modelines. **MEASURED.**

| offset | name | high 16 | low 16 |
|---:|---|---|---|
| `0x1908` | `dct0_hblank` | `htotal-1` | `hactive-1` (blank start) |
| `0x1910` | `dct0_htotal` | `hactive-1` | `htotal-1` |
| `0x1914` | `dct0_hsync` | `hsync_end-1` | `hsync_start-1` |
| `0x1920` | `dct0_vblank` | `vtotal-1` | `vactive-1` |
| `0x192c` | `dct0_vtotal` | `vactive-1` | `vtotal-1` |
| `0x1930` | `dct0_vsync` | `vsync_end-1` | `vsync_start-1` |
| `0x195c` | `dct0_vsync_end` | 0 | `vsync_end-1` |

Worked example, 1920x1200 (h 1920/1968/2000/2080, v 1200/1203/1209/1235):
`dct0_htotal=077f081f` (`0x77f`=1919, `0x81f`=2079), `dct0_hsync=07cf07af`
(`0x7af`=1967=1968-1, `0x7cf`=1999=2000-1), `dct0_vtotal=04af04d2`
(`0x4af`=1199, `0x4d2`=1234), `dct0_vsync=04b804b2` (`0x4b2`=1202,
`0x4b8`=1208). Every mode reproduces these formulas exactly.

### Pixel clock (important MEASURED finding)

The frequency-synthesizer registers (`0x0e24` oscillator, `0x0e30/0e34/0e38`
`fs0..2`) **did not change** for any of the seven modes, including 640x480
(25 MHz) versus 1920x1200 (154 MHz). The only per-mode clock-related register
that changed is **`dvo0` (`0x1c9c`)**, byte 2 nibble: `0x40`(1920), `0x00`(1600/
1400/1280/1024), `0x10`(1680), `0x50`(640). So on this DVI path the visible
clock control is a `dvo0` divider/select field, not a distinct PLL word.
Whether the VCO is reprogrammed through the indirect `e30` port sequence seen
in the cold-init trace (whose final latched value happens to match) is **not**
resolved by a read-only snapshot; capturing that needs the register-accessor
write trace. Do not assume the PLL is left unprogrammed.

### Hardware cursor (base 0x18c0, MEASURED — resolves the static open items)

Setting a 32x32 ARGB cursor and warping the pointer produced:

| offset | name | observed | decode |
|---:|---|---|---|
| `0x18c4` | `cur0_control` | `3 -> 5` | 5 = ARGB/colour cursor (STATIC mode 2) |
| `0x18cc` | `cur0_address` | GPU addr | cursor image VRAM base |
| `0x18d0` | `cur0_config` | `06.. -> cc000000` | matches STATIC colour-upload config |
| `0x18c8` | `cur0_position` | see below | `(X+64-hot)<<16 | (Y+64-hot)` |

Position packing confirmed on two warps: X in bits 30:16, Y in bits 14:0, with
the `+64` origin (STATIC `parhelia-m9-cursor-gamma-re.md`) and the software
hotspot subtracted by the DDX. **MEASURED.**

The cursor VRAM image (`D-display/cursor-vram-image.bin`, dumped through the
host aperture at `cur0_address`) **resolves the previously open byte/channel
order**: it is a **linear 64-row image, 256-byte (64-pixel) stride,
ARGB8888 in native little-endian order (B,G,R,A)** with only **bit 24 cleared**
per dword (`0xff000000 -> 0xfe000000`), exactly the STATIC mask `0xfeffffff`.
Row 0 texel `(x,y)=(1,0)` source `0xff080001` stored `0xfe080001`; row 1 at
byte 256. There is **no R/B swap** in the stored image; the earlier suspected
swap is not present in this path. Config `0xcc000000`, control `5`.

### Gamma LUT and DPMS (MEASURED boundaries)

* **Gamma:** `XRRSetCrtcGamma` produced **zero** changes in the read-only
  8 KiB snapshot. The LUT is written through the write-only auto-incrementing
  index/data ports (`0x1838` index, `0x183c/40/44` data — STATIC), which a
  read cannot observe, and bit 14 (bypass) in `dc0_ctrl` was already clear and
  stayed clear. The STATIC write-order contract is therefore unchanged; a live
  write-order capture needs the register-accessor trace.
* **DPMS:** `DPMSForceLevel` to Off and Standby produced **no stable** register
  delta in the snapshot (only the transient `engine_status` bit). The blanking
  is either a driver no-op on this panel or done through write-only/transient
  sync-disable writes; write-order capture is required to decide. Not claimed.

## Group C — XVideo (new, MEASURED enumeration)

`m9x-xv-probe info` (`C-xvideo/xv-info.txt`), **MEASURED**:

* One adaptor **"Matrox M9X Blitter"**, type `0x11` (Input|Image), **32 ports**,
  base id 112, on the eight depth-24 truecolor visuals.
* Image formats: **YUY2** (packed 16 bpp, YUYV), **UYVY** (packed 16 bpp),
  **YV12** (planar 12 bpp, YVU), **I420** (planar 12 bpp, YUV).
* Port attributes: `XV_BRIGHTNESS`, `XV_CONTRAST`, `XV_HUE`, `XV_SATURATION`,
  each range −128..127, settable+gettable. **No colorkey/autopaint** attribute.
* One encoding `XV_IMAGE`, `2048x2048`, rate 1/1.

The absence of a colorkey attribute and the "Blitter" name identify a
**textured (blitter) Xv path, not a hardware colour-key overlay**, consistent
with the vendor strings `GetImage_UYVY_YUY2` / `GetImage_YV12_I420` and
`xf86XVAllocateVideoAdaptorRec`.

`XvPutImage` at 1:1 and scaled (YUY2/UYVY/YV12) changed **only** `cmd_base`
(`0x400`) and `cmd_doorbell` (`0x404`) plus the transient `engine_status`
(`C-xvideo/diff-*.txt`). **MEASURED:** video is submitted through the same
ExecuteDma command FIFO as GL/EXA; the CSC and scale factors live in the
command stream (a shader/blit), **not** in persistent scaler/CSC registers.
Decoding those coefficients needs the DDX command stream (see Group B).

## Group A — GL2 extension native programs and state (CAPTURED)

`tools/m9x-gl2ext-corpus` runs 40 cases the GLSL/fixed-function corpus does not
cover, each a real position/texel-coded draw with the vendor ICD emitting a
native program into C2 (read via slot 0) with its constant span and draw state.
Decode any case with `python3 tools/m9-glsl-program-extract.py <case>` from the
`A-gl2ext` directory. Case families:

* **ARB_vertex_program** native programs: `vp-mov/add/mul/dp3/dp4/swz`
  (`vp-mad` is a negative capture: ARBvp1.0 has no MAD).
* **ARB_fragment_program** native programs: `fp-mov/add/mul/dp3/dp4/tex/txp/
  kil/lrp/cmp`, plus `fp-shadow` (fragment_program_shadow compile).
* **Separate stencil:** `stencil-separate-core` (core `glStencil*Separate`);
  `stencil-separate-ati` records that the vendor libGL does not export the ATI
  entrypoints (looked up via `glXGetProcAddressARB`).
* **Point sprites:** `sprite-ff` (fixed-function `GL_POINT_SPRITE`/
  `GL_COORD_REPLACE`), `sprite-glsl-pointsize` (GLSL `gl_PointSize`).
* **Multisample:** `msaa-coverage`, `msaa-coverage-invert-a2c` (queries all
  sample state and which `GLX_SAMPLES` fbconfigs exist).
* **Draw buffers (RT0 only, MRT-safe):** `drawbuf-fbo-rt0`, `drawbuf-back`.
* **Logic op:** `logic-xor/and/or/copy/invert`, `blend-logic-op-xor`.
* **Texture compression queries:** `compress-rgba`, `compress-rgb`.
* **NV_blend_square:** `blend-square-src`, `blend-square-dst`.
* **Hints/swap:** `cliphint-none/nicest/fastest`, `swap-hint`.
* **Back-buffer copy:** `copytexsubimage-back`.

Worked example `fp-mul` (`MUL result.color, fragment.texcoord[1], u`): three
preprocessing draws (c3 = `04`, `13`, `1a`) and one raster draw (c3 = `00`)
into one C2 image; the fragment constant span at arena base `0x40c3a000`
carries `u = program.local[0]` as entries `(u.x,u.y),(u.z,u.w)`; sampler words
`7c..7f` on the raster draw. This is the same native encoding model as the
GLSL corpus (`parhelia-m9-native-glsl-corpus.md`), now reached through the ARB
assembly frontends. Per-case decoded results are in `A-gl2ext/*/case.json` and
the corpus README.

## Group B — EXA/2D (MEASURED route only; stream deferred)

`m9x-exa-driver` exercised solid fill, overlapping copy, RENDER composite and
glyph composite. MMIO snapshots show EXA moves only `cmd_base`/`cmd_doorbell`/
`engine_status`, i.e. it submits through the ExecuteDma FIFO, as the task
premise states. The register-level solid/copy/composite encodings, ROP table,
surface descriptors and RENDER operator/format constraints are already reverse
engineered in `parhelia-m9-accel.md`. The **live command-stream confirmation**
(reloc records, RT descriptor `0xb6/0xb7`, per-op control words) was not
captured this session — see the method note above. `tools/m9x-exa-driver.c` is
retained so a future X-restart session can capture it under the preload tracer.

## Remaining gaps

1. EXA and XVideo ExecuteDma **command streams** (composite operator matrix,
   YUV CSC/scale coefficients, reloc/descriptor records). Needs the DDX traced
   (X restart + preload, or an approved live-X ExecuteDma dump).
2. Gamma-LUT and DPMS **write order** (write-only ports): needs the
   register-accessor gdb trace used for the cold-init capture.
3. Pixel-clock/PLL programming path for large clock deltas (indirect `e30`
   synthesizer vs `dvo0` divider): needs the accessor write trace.
4. Group A per-family native-program **semantics** beyond the captured bytes
   (e.g. ARBfp KIL/CMP/LRP native fields, point-size route, sample-coverage
   state) — the images are captured; the field decode is future work joined to
   `parhelia-m9-native-glsl-corpus.md`.
