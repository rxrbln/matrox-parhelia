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
| 0x88–0x8F | 0x1220–0x123C | **SURF descriptors** | four {base, config} slot pairs |
| 0xC7 / 0xC8 | 0x131C / 0x1320 | **PAT0 / PAT1** | 8×8 mono pattern data |
| 0xC9 | 0x1324 | **PLNWT** | plane write mask (planemask; 0xFFFFFFFF = all) |
| 0xCA / 0xCB | 0x1328 / 0x132C | **SCISSOR MIN / MAX** | 2D clip corners |

> **Corrected 2026-09** (see [`parhelia-2d-accel-complete.md`](parhelia-2d-accel-complete.md)
> for the evidence): PLNWT is index **0xC9 = 0x1324**, not 0xB0/0x12C0 —
> 0x12C0/0x12C4 are 3D/DPS colour masks.

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
DWGCTL = opcode | (BOP << 4) | (srcSlot << 8) | (dstSlot << 10) | modifiers
```

| Bits | Field |
|---|---|
| **[3:0]** | **opcode**: 0 fill · 1 line · 2 BITBLT · 3 colour-expand · 0xC textured rect |
| **[7:4]** | **BOP** — raster/logic op (see ROP table) |
| **[9:8]** | **source** surface-descriptor slot (0–3) |
| **[11:10]** | **destination** surface-descriptor slot (0–3) |
| bit 12 (0x1000) | pattern-transparent (opcode 0) / expand-transparent (opcode 3) |
| bit 13 (0x2000) | 8×8 pattern enable (opcode 0) |
| [16:14] / [19:17] | pattern X / Y origin |
| bit 22 (0x400000) | colour-key transparency (opcode 2) / draw-last-pixel (opcode 1) |
| bit 23 (0x800000) | **clip/scissor enable** |
| [22:20] (line) | line mode (`0x700001` draw-last / `0x300001` omit-last) |

> **Corrected 2026-09:** bits [3:0] are the *opcode*; the slot fields are
> [11:10] (dst) and [9:8] (src).  The `1 << slot` value in the vendor's
> `SetupFor*` is the driver's shadow dirty mask, not a DWGCTL field.  Bit 23 is
> clip-enable, not "blockmode".  The blit direction bits 0x1000/0x2000 are
> **unproven** — the vendor's own copy path never uses them (it routes every
> screen-to-screen blit through a temporary surface).  Full derivation in
> [`parhelia-2d-accel-complete.md`](parhelia-2d-accel-complete.md).

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

### Precise XAA op specs [DRV: `ParhlXAA*` in `xserver/7_4_0/mtx_drv.so`]

The `Setup*` functions build a **base DWGCTL** and cache FCOL/BCOL/PLNWT/surface
into a DrawContext shadow; the flush (`HwThread2d::SetupHardware`) streams them as
tag+data packets. A packet is one **tag dword** (4 register-index bytes, LE order;
`0x21` = filler) followed by up to 4 data dwords. DWGCTL always gets a `0x707`
idle prewrite. Register indices used: DWGCTL 0xC0, LINE_START 0xC1, DSTXY 0xC2,
LINE_END 0xE2, LEN 0xE3, FCOL 0xC5, BCOL 0xC6, PLNWT 0xB0. A direct-MMIO driver
reimplements each by writing `0x1000+idx*4` in the same order; the final geometry
write (LEN, or LINE_END for lines) triggers the op — **no DMA doorbell needed**
(the vendor's `[MMIO+0x3050]=1` only kicks its pushbuffer ring).

**base DWGCTL** (all ops): `dstSlot | (srcSlot>>2) | ((BopTable[rop]<<4)&0xF0) |
(clip ? 0x800000 : 0)`. BOP = `g_adwParhlXaaBopTable[rop]` (4-bit-reversed GXrop).
The `0x800000` clip-enable is added iff a clip rect is armed (`[priv+0x10]!=0`).

| Op | DWGCTL adds | Geometry writes | Source |
|---|---|---|---|
| **SolidFill** | — | DSTXY=(y<<16)\|x, LEN=(h<<16)\|w; FCOL=colour, PLNWT=mask | @0x536e0 / @0x539c0 |
| **ScreenToScreenCopy** | `0x2` blit \| src-slot \| `0x1000`(xdir<0) \| `0x2000`(ydir<0) \| `0x400000`(trans≠−1) | reverse walk starts at far edge (x+=w−1 / y+=h−1); LINE_START(0xC1)=src (y<<16)\|x, DSTXY=dst, LEN | @0x551e0 / @0x55530 |
| **SolidLine HorVert** | — (1-px fill) | DSTXY=(y<<16)\|x, LEN=`(1<<16)\|len`(horiz) / `(len<<16)\|1`(vert) | @0x55c00 / @0x55ed0 |
| **SolidLine TwoPoint** | `0x700001`, or `0x300001` when bias flag bit0 set | LINE_START=(y1<<16)\|x1, LINE_END(0xE2)=(y2<<16)\|x2 | @0x560f0 |
| **Mono8x8Pattern** | `0x2000`(opaque) / `0x3000`(bg==−1, transparent); per-rect `\|(patx<<14 &0x1C000)\|(paty<<17 &0xE0000)` | DSTXY, LEN; FCOL=fg, BCOL=bg, PLNWT=mask | @0x53be0 / @0x53f10 |
| **CPUToScreenColorExpand** | `0x800303`(opaque) \| `0x1000`(transparent, bg==−1) | host mono bits CPU-copied into a mapped scratch surface, then colour-expand blitted | @0x54220 / flush @0x54570 |
| **Set/DisableClipping** | sets/clears `0x800000` in shadow + `[priv+0x10]` flag | SCISSOR_MIN=(y1<<16)\|x1, SCISSOR_MAX=(y2<<16)\|x2; Disable → (0,0)/(0xfff,0xfff) | @0x535e0 / @0x53660 |

**Clip semantics:** `SetClippingRectangle` writes the scissor MIN/MAX (0x1328/
0x132c) and arms the flag; every armed `Setup*` ORs `0x800000` so the scissor
bounds it. `DisableClipping` resets the scissor to full and clears the flag —
which is exactly why an unconditional `0x800000` without a programmed scissor
clips against garbage (the earlier crash).

**Host-data ops are MMIO-feasible, no busmaster DMA** [DRV]: `memWriteDword`
(@0x589a7) is a plain store, `MmHostAperture::Lock` (@0x72e20) returns a CPU
pointer to a mapped BAR window. ColorExpand streams the mono bitmap by CPU stores
into a mapped scratch/aperture surface then blits (`XferNativeToVid` @0x574a0 for
WritePixmap; `XferVidToNative` @0x56a50 reads back for ReadPixmap).

> **Corrected 2026-09:** the mono 8×8 pattern is **not** an offscreen VRAM
> surface. `Mono8x8PatternFillFlags` sets `HARDWARE_PATTERN_PROGRAMMED_BITS`, so
> patx/paty are the pattern's two *data* dwords and go to registers **0x131C /
> 0x1320**; only the 3-bit pattern origin rides in DWGCTL[19:14]. Stipple fill
> therefore needs no scratch-VRAM allocator.

### Implementation status in `src/parhelia_xaa.c`

Implemented by direct MMIO (recovered specs above): **solid fill, screen-to-
screen copy (all directions), hor/vert + two-point lines, hardware clipping.**
Deferred to software pending a scratch-VRAM allocator + on-hardware iteration:
**colour expand, mono 8×8 pattern fill, image up/download** — all RE-complete
above, each needs an offscreen surface and the reverse-direction/pattern-origin
details validated on the card first.

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

## Hardware cursor [DRV — register map re-RE'd 2026-08-09; still not visible on HW]

64×64, both 2-colour and full ARGB, direct MMIO. Per head (CRTC1 base 0x18C0,
CRTC2 base 0x1AC0 — inside the display-head blocks, stride 0x200). The earlier
table in this doc (image@0x1AC0, X/Y pos@0x1ACC/0x1AD0) was **wrong**; the
corrected map below was traced twice from `MTXHWCursorInit`@0x43a70 and the
cursor callbacks `mtx58/59/60/63/64/67/68/69` and matches the user's own notes:

| CRTC1 / CRTC2 | Field | Value |
|---|---|---|
| 0x18C0 / 0x1AC0 | colour / LUT data (RGB888) | written after selecting index+mode in +0x04 |
| 0x18C4 / 0x1AC4 | control | `(lut_index<<8) \| mode`; mode 0=off, 3=2-colour, 4=16-colour/ARGB |
| 0x18C8 / 0x1AC8 | position (one packed dword) | `((x+64)&0x7fff)<<14 \| ((y+64)&0x7fff)` |
| 0x18CC / 0x1ACC | image base | **surface address in 8-byte units = `byte_offset >> 3`** (`HSLCRTCSetDisplayStart` >>3 default; the CPU still memcpy's to the byte offset) |
| 0x18D0 / 0x1AD0 | format | `0x05000000`=2-colour, `0x04000000`=16-colour/ARGB |

Plus a **DC-level composite enable** at `DC(head)+0x88` = **0x1888 / 0x1A88**,
RMW `|= 0x16` (asserted in SetCursorPosition and ShowCursor; HideCursor clears
it / writes control 0). `HSLPARSetDCParameters` never touches the cursor.

**2-colour image**: 2 bits/pixel, 16 bytes/row copied verbatim, 32-byte surface
row stride (upper 16 bytes zeroed); the 2 bits index LUT[0]=bg / LUT[1]=fg with
transparency. xf86 realize flags = **swap+and** (0x300 in the vendor's era, i.e.
`HARDWARE_CURSOR_SWAP_SOURCE_AND_MASK | HARDWARE_CURSOR_AND_SOURCE_WITH_MASK`),
not INTERLEAVE_1. **ARGB image**: 9 bit-planes per row (4 palette-index planes
then 5 alpha planes, LSB-first = column 0 in bit 0), 128-byte row stride; alpha =
top 5 bits of source alpha; ≤16-entry quantised palette loaded into the LUT.

**UNRESOLVED — cursor is invisible on the 0x0528 hardware.** With every register
above set correctly (verified live: mode 3/4, format 0x05/0x04000000, composite
0x1A88=0x16, image data confirmed present in VRAM, position on-screen centre, on
the active head1), the sprite never appears. Confirmed NOT the cause: head swap
(head1 is the sole active display head, head0 dead), the image-base encoding
(tried raw `0xffc000`, shifted `0x1ff800`, and the note's raw `0x03f00000` at
63 MB — all invisible), and the modeset (invisible under BOTH native and VBE
int10 modeset, including the VBE state where the head is fully active
0x1A00=0x00040201 and the DVO is fully enabled 0x1C20 byte0=0x30). Software
cursor is the working fallback. The missing factor is not yet identified.

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
