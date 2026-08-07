# Matrox Parhelia — acceleration engine (2D, 3D, DPS, cursor, Xv)

Reverse-engineered from the proprietary Linux driver
(`matroxdriver-x86_64-cvs-20090511`, v1.4.7). Evidence tags: **[DRV]** =
disassembly of `xserver/7_4_99/mtx_drv.so` (not stripped, demangled C++ names),
**[GL]** = `opengl6/lib64/libGL.so.1.3.0`, **[SRC]** = kernel C source,
**[HW?]** = inferred. Nothing here is tested on hardware. This documents the
hardware; it is not a commitment to implement 3D.

## How commands reach the engine

The engine register file is an **indexed set of MMIO registers**. The absolute
MMIO offset of engine register *index i* is:

```
offset = 0x1000 + (i << 2)         (equivalently  index = (offset - 0x1000) >> 2)
```

so the engine registers occupy roughly **0x1000–0x13FC** (DWGCTL = index 0xC0 =
offset **0x1300**). They are reached two ways [DRV]:

1. **DMA busmaster pushbuffer** (normal path). Userspace builds a buffer of
   *tag+data* packets: a tag dword encodes up to four register indices, followed
   by up to four data dwords written to those registers in turn. The buffer is
   handed to the kernel via **ioctl `0xc018461b`** (Parhelia core ioctl #7,
   `OsPostBusmasteringBuffer`, descriptor `{postType, start, end, …}`), and the
   closed kernel core programs the busmaster engine to fetch it. Display/CRTC
   registers (0x1800+) fall outside the index range and are always written by
   **direct MMIO** instead — which is why the two subsystems use different
   access paths.
2. **Direct MMIO / PIO** (fallback). `HwDevice::ExecuteEmulDma` replays the same
   packet buffer with `memWriteDword` to `0x1000 + index*4`. This is what proved
   the format.

The command-buffer object holds a pair count at struct `+0x808` and `{u32 reg,
u32 val}` entries from `+0x8`. (This is the same structure an earlier pass
mis-read as a "memory-controller init list".)

### DMA control / status / fence registers (direct MMIO) [DRV]

| Offset | Role |
|---|---|
| `0x3050` | **doorbell** — write 1 to kick the primary DMA FIFO |
| `0x450` | **drawing-progress fence** — a mark counter; poll until it reaches the embedded DwgMark |
| `0x8F0` | **busmaster status** — bit 17 (0x20000) = idle/complete |
| `0x41C` | DMA GET pointer — polled by `ParhlXAASync` (masked `& ~7`) |

Two sync mechanisms: `ParhlXAASync` emits a soft-trap packet (DWGCTL idle value
`0x707`, sync tag in reg index 0xAF) then polls `0x41C`; `HSLPARWaitEngineNotBusy`
reads a status dword and tests **bits [31:30] == 01** for busy. AGP is standard
Linux agpgart (up to 8×, write-combined); pushbuffers are 256 KB; buffer memory
can be VRAM, AGP, PCI-locked, or vmalloc'd [SRC].

## 2D drawing engine

### Registers (by index and absolute offset) [DRV]

| Index | Offset | Name | Purpose |
|---|---|---|---|
| 0xC0 | 0x1300 | **DWGCTL** | drawing control: opcode / ROP / pattern / direction / transparency / blockmode |
| 0xC1 | 0x1304 | **LINE_START** / src rect | line start `(y<<16)|x` (also src geometry in DPS) |
| 0xC2 | 0x1308 | **DSTXY** | destination origin `(y<<16)|x` |
| 0xE2 | 0x1388 | **LINE_END** | line end `(y<<16)|x` |
| 0xE3 | 0x138C | **LEN** | extent `(h<<16)|w` |
| 0xAF | 0x12BC | **DWGSYNC** | soft-trap sync tag |
| 0xC5 | 0x1314 | **FCOL** | foreground colour (replicated per bpp) |
| 0xC6 | 0x1318 | **BCOL** | background colour |
| 0x88–0x8B | 0x1220–0x122C | **SURF descriptors** | src/dst surface descriptor dwords (format+addr+pitch) |
| 0xB0 | 0x12C0 | **PLNWT** | plane write mask (planemask; 0xFFFFFFFF = all) |

FCOL/BCOL were pinned down after the first accel pass: the solid-fill and copy
`Subsequent*` functions leave colour to the context flush, which emits packet
header `0x2121c6c5` writing FCOL (index 0xC5) ← colour and BCOL (index 0xC6) ←
mask/0xFFFFFFFF (disasm @0x575dd).

Foreground/background colour (FCOL/BCOL), plane mask, and SRCORG/DSTORG/PITCH are
held in a **DrawContext shadow struct** and emitted on demand. Crucially, source
and destination go through a **surface-descriptor slot table** (`SurfDesc2dList`):
the slot number is encoded into DWGCTL's low nibbles, and origin/pitch live in
per-slot arrays — a driver must replicate this rather than writing flat base/pitch
per blit.

### DWGCTL bit fields [DRV]

```
DWGCTL = dstSlot | (srcSlot >> 2) | (BOP << 4) | opcode | 0x800000(blockmode)
```

| Bits | Field |
|---|---|
| [3:0] | destination surface-descriptor slot (`1 << slot`) |
| **[7:4]** | **BOP** — raster/logic op (see ROP table) |
| bit 0 (0x1) | line / colour-expand enable |
| bit 1 (0x2) | BITBLT/copy opcode |
| [9:8] (0x300) | colour-expand source type (→ base `0x303`) |
| bit 12 (0x1000) | blit X-direction (R→L) / colour-key or pattern transparent |
| bit 13 (0x2000) | blit Y-direction (B→T) / pattern-fill mode |
| bit 22 (0x400000) | transparency (colour-key) enable |
| bit 23 (0x800000) | blockmode |
| [22:20] (line) | line style / octant (`0x700001` / `0x300001`) |

Pattern origin is OR'd into DWGCTL at draw time: `patx`→bits[16:14],
`paty`→bits[19:17].

### ROP encoding [DRV: `g_adwParhlXaaBopTable` @0x333e00]

BOP = 4-bit **bit-reversal** of the X GXrop index:
`{0,8,4,C,2,A,6,E,1,9,5,D,3,B,7,F}`. E.g. GXcopy(0x3)→0xC, GXxor(0x6)→0x6.

### Operation sequences [DRV]

Each is: populate the shadow in `SetupFor*`, then stream packets and ring the
doorbell in `Subsequent*`. DWGCTL is always preceded by an idle load of `0x707`.

- **Solid fill:** `DWGCTL = slot|(bop<<4)|0x800000`, FCOL=colour, PLNWT=mask →
  emit `DWGCTL`, then `DSTXY=(y<<16)|x`, `LEN=(h<<16)|w`, doorbell.
- **Screen-to-screen copy / BitBlt:** `DWGCTL |= 0x2` plus direction bits 12/13
  from sign of dx/dy, plus `0x400000` if colour-keyed. Overlap handled by a
  temp-surface path.
- **Mono 8×8 pattern fill:** `DWGCTL |= 0x2000` (opaque) or `0x3000`
  (transparent); FCOL/BCOL/pattern XY; pattern origin OR'd at draw.
- **CPU-to-screen colour expand:** `DWGCTL |= 0x800303` (opaque) / `|0x1000`
  (transparent), then stream the host bitmap dwords.
- **Solid line:** hor/vert drawn as a 1-pixel fill rect; two-point uses the LINE
  opcode (`0x700001`/`0x300001`) with LINE_START/LINE_END.
- **Clipping:** min/max XY pair `(y<<16)|x` into the clip registers; blockmode
  bit gates it.

### What the X driver accelerates (XAA, not EXA) [DRV]

`MTXParhlSetupXAAInfoRec` advertises: SolidFill / FillSolidRects / Spans, mono
8×8 pattern fill, solid lines (HorVert + TwoPoint), ScreenToScreenCopy/BitBlt,
CPUToScreenColorExpandFill, Read/WritePixmap. No EXA path exists in this driver.

## DPS — the "2D via 3D pixel shader" engine

Parhelia does colour-conversion and scaled blits (video, and what would be
EXA-composite) through a pixel-shader path, **DPS**. It is a register command
list, not microcode [DRV].

- **Blit** (`CDPSSurface::Blit`): emit src/dst surface descriptors
  (`0x1220–0x122C`), op code to DWGCTL (`0x1300 = 0x4C2` for a textured rect),
  and geometry (`0x1304`/`0x1308`/`0x138C`), then post.
- **Texture engine** (`CDPSTextureEngine::UpdateHardware`): per-pipe control at
  **0x1200/0x1204/0x1208/0x120C** (four pipes), filter/combiner block
  **0x11C0–0x11E8** (`0x11CC` = 0xFFFFFFFF mask).
- **Texture setup** (`CDPSTextureSetup`): S/T/W coordinate origins and
  increments at **0x1160, 0x116C, 0x1174–0x1180, 0x1198**.
- **Pixel shader** (`CDPSPixelShader::UpdateHardware`): shader-stage registers
  **0x1260–0x126C** and colour-matrix coefficients **0x1278/0x127C/0x1280/0x1284**.
  Coefficients are `coeff × 1024` as **signed 11-bit**, two per register.

### Procamp (brightness / contrast / saturation / hue) [DRV]

A 3×3 colour matrix + bias. The base is a hardcoded **BT.601 YCbCr→RGB** matrix
(≈`[0.257 −0.148 0.439; 0.504 −0.291 −0.368; 0.098 0.439 −0.071]`). Final matrix =
`CSC · contrast · …`; bias = brightness + (0.0625, 0.5, 0.5) (the 16/128/128
YCbCr offsets). Packed into the shader coefficient registers above.

## Hardware cursor [DRV]

64×64, both 2-colour and full ARGB, direct MMIO. Per head (CRTC1 base 0x18C0,
CRTC2 base 0x1AC0 — inside the display-head blocks, stride 0x200):

| CRTC1 / CRTC2 | Field |
|---|---|
| 0x18C0 / 0x1AC0 | image base address |
| 0x18C4 / 0x1AC4 | control (0 = disable) |
| 0x18C8 / 0x1AC8 | colours / mask |
| 0x18CC / 0x1ACC | X position |
| 0x18D0 / 0x1AD0 | Y position |

Disable writes 0 to the control register and waits for vblank.

## Xv / video overlay [DRV]

Implemented through the DPS textured-rectangle scaler (per-line
`DrawTexturedRectangle` with polyphase filter coefficients), not a classic
backend scaler. Attributes XV_BRIGHTNESS/CONTRAST/HUE/SATURATION feed the procamp
matrix. Image formats: **YUY2, UYVY** (packed 4:2:2), **YV12, I420** (planar
4:2:0). `HSLPARDeviceIoControl_CanUseOverlay` chooses between a CRTC2 hardware
overlay and the DPS scaler.

## 3D pipeline [GL]

Documented for completeness; far beyond any near-term driver goal. This section
consolidates three deep passes over `libGL.so.1.3.0` (render state, texture/
fragment shader, and the Trinity vertex engine), cross-checked against
`mtx_drv.so`. It is a hardware map, not a plan to implement 3D.

```
libGL → vertex submission → Trinity (HW T&L / vertex shader) → clip/guardband
     → Sundog setup/rasterizer → 4 texture pipes → fragment shader (combiner)
     → depth/stencil + FAA-16x fragment buffer → blend/fog → framebuffer (GigaColor 10-bit)
```

**Register model confirmed independently.** The render-state pass re-derived
`offset = 0x1000 + (index << 2)` from `CDPSHwManager::WriteBuffer` and verified it
against known offsets (index 0x98→0x1260 pixel-shader, 0x58→0x1160 texture-
setup). 3D state is emitted through the same indexed command buffer as 2D, via a
table-driven validator at libGL `0x113b80` — which is why these offsets are not
literal immediates and had to be recovered from the emit path. A second register
bank exists (`WriteBuffer` key `0x5`, base undetermined) carrying the blend
factors.

### Render state (bank base 0x1000) [GL, proven indices]

| Group | Register | Index | Offset |
|---|---|---|---|
| Depth | test enable | 0x07 | 0x101C |
| | compare func | 0x17 | 0x105C |
| | write mask | 0x0E | 0x1038 |
| Stencil | enable | 0x34 | 0x10D0 |
| | op sfail/dpfail/dppass | 0x35/0x36/0x37 | 0x10D4/0x10D8/0x10DC |
| | func / ref / valuemask / writemask | 0x38–0x3B | 0x10E0–0x10EC |
| Alpha test | enable | 0x0F | 0x103C |
| | compare func | 0x19 | 0x1064 |
| | reference | 0x18 | 0x1060 |
| Blend | equation | 0xAB | 0x12AC |
| | per-RT enable mask | 0xA8 | 0x12A0 |
| | src/dst factors | 0x09–0x0C | **bank key 0x5, base unknown** |
| Fog | color/enable | 0x1C / 0x22 | 0x1070 / 0x1088 |
| Shade model | flat/smooth | 0x09 | 0x1024 |
| Depth range | (fed to shader consts) | 0x9A/0x9B/0xA6 | 0x1268/0x126C/0x1298 |

Field encodings (from the emit jump tables): **compare funcs** GL_NEVER..ALWAYS →
1..8; **blend equation** ADD/SUB/REVSUB/MIN/MAX → 1..5. Fog is largely
shader-based — the linear factor `(end−z)/(end−start)` is computed on the host
and written to fragment-shader constants; there is no classic FOGCOL block.
Depth-range and polygon offset likewise ride the pixel-shader constant range
(0x1260+), not a dedicated ZORG register.

### Texture engine (4 pipes) [DRV, proven]

- **Pipe control** 0x1200/0x1204/0x1208/0x120C: value `pipeCtrlBase | (1<<pipe)`;
  active pipe = `0x00040101` (bit 0-3 one-hot pipe id, bit 8 active-fetch, bit 18
  enable), idle pipes = `0x00010000` (bit 16 bypass).
- **Combiner / filter** 0x11C0–0x11E8: op word at 0x11C0, channel routing
  `0x249` at 0x11C4, default colour `0xFFFFFFFF` at 0x11CC, **texture base
  address** low/high at 0x11DC/0x11E0, **surface control word** (format+pitch+
  dims) at 0x11E4, **filter options** (min[7:0]/mag[15:8]/mip[23:16]) at 0x11E8.
- **Texture setup / coordinate DDA** 0x1160–0x1198: IEEE-754 float S/T coordinate
  values and per-scanline increments.

### Pixel / fragment shader [DRV, proven]

`GL_MTX_fragment_shader`, a two-phase combiner (texture-address then colour/alpha
arithmetic), temp regs 0/1, output reg, DP3/DP4, modifiers Saturate/Scale/
Replicate/Convert.

- Stage registers 0x1260–0x126C; **colour-matrix coefficients** 0x1278/0x127C/
  0x1280/0x1284.
- **Coefficient packing (confirmed exactly):** `round(coeff × 1024)` as **signed
  11-bit**, two per register — bits[10:0] and bits[26:16].
- Stage control word: `0x1264 = ((input<<5) & 0x3E0) | 0x0703C000 | slot[4:0]`;
  `0x126C = ((output<<20) & 0xF00000) | opcode-base`.

### Trinity vertex engine [GL, proven format; upload registers not resolvable]

- **Microcode instruction word:** `2 + N_sources` consecutive 32-bit words —
  word0 = opcode/control, word1 = destination, word2+ = sources. Operand fields:
  swizzle [23:16], write-mask [19:16], bit31 = valid, bit24 = destination. Shape
  (scalar/vector/matrix) in two 2-bit fields [27:26] and [23:22].
- **Program object** `GLVertexShaderFunctionDX` (728 bytes): microcode vector at
  +0x298/+0x2A0, instruction count at +0x1CC.
- **Hardware limits:** 192 instructions preferred / 512 max; register-class
  limits 16/256/256/12.
- **Fixed-function T&L is compiled to microcode**, not fixed registers — matrices
  and lights become shader uniforms (MVP = A×B, transposed to column-major,
  uploaded as 16 floats). `reg3DLightingType` selects the number format in the
  generated code.
- **Not statically resolvable in these binaries:** the DX→native opcode table
  (word0 values; only `0x60` observed), and the engine register indices for
  microcode/uniform upload — both sit behind a per-context dispatch table in the
  stripped ICD. Recorded so this is not chased again from these files.

### Formats and features [GL]

4 texture units; formats 8888/4444/1555/565/332, 16/32-bit and float per channel,
**2:10:10:10 "GigaColor"**, S3TC/DXT compression, cube maps, 3D textures,
anisotropic + LOD bias. Depth/stencil S8Z24 with Z-compression; **FAA-16x**
fragment antialiasing. The 3D-triangle DWGCTL opcode lives in the `glEnd`/submit
path (not the state validator) and was not located — still open.

## Confidence and gaps

- **High [DRV]:** the DMA submission model; DWGCTL bits and the six 2D op
  sequences; ROP table; engine register indices/offsets incl. FCOL/BCOL; DPS blit
  + texture/shader register offsets; procamp math; cursor register map; Xv
  formats; XAA caps. 3D render-state register offsets (depth/stencil/alpha/blend/
  fog); texture pipe + combiner encodings; pixel-shader coefficient packing;
  Trinity microcode word format.
- **Medium:** exact bit-packing of the DMA tag dword; individual texture/shader
  control-word semantics (`0x1300 = 0x4C2`, filter-word layouts); fog-colour vs
  tex-env-colour group.
- **Open (and why):** the absolute DMA primary-address registers and the Trinity
  microcode/uniform-upload register indices both live behind closed/dispatch-
  indirected code (kernel core, and the stripped ICD's per-context dispatch
  table) — **not statically resolvable from the supplied binaries**; the blend
  src/dst factor offsets (second register bank, base undetermined); the DX→native
  vertex opcode table; the 3D-triangle DWGCTL opcode (in the draw-submit path).

## Bearing on the open driver

For a first accelerated driver, the **2D engine** section is directly usable:
DWGCTL at 0x1300, the surface-descriptor model, the ROP table, and the six XAA
operation sequences, submitted either by direct MMIO (simplest) or the DMA
pushbuffer (faster). The **cursor** register map is immediately usable. **Xv**
can reuse the DPS blit + procamp. Full **3D** is a large separate effort and is
documented here only as a map.
