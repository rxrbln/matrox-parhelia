# Matrox M9 ("Phoenix") 2D acceleration

> **Prototype history, not a working specification.**  Later hardware work
> refuted direct-MMIO execution and left `0xb6/0xb7` versus `0xb8/0xb9` surface
> roles contradictory within this file.  Use the packet payload findings as RE
> leads only.  See `parhelia-m9-3d-submission.md` and
> `parhelia-m9-parhelia-3d-comparison.md` before implementing an engine backend.
>
> **Settled 2026-09-18 against the captures** (see
> `parhelia-m9-texture-upload-and-copy-re.md`).  A sweep of all 606 decodable
> vendor submissions in `captures/` establishes three things about this file:
>
> * The surface roles are `b0/b1`..`b6/b7` = colour attachments 0..3,
>   `b8/b9` = depth, `ba/bb` = hierarchical Z.  Both the "RT0 = `0xb8/0xb9`"
>   table and the later "CORRECTION: the 2D destination is `0x12d8/0x12dc`"
>   section below are **wrong**; `0xb6/0xb7` is colour attachment 3, not a
>   copy source.
> * The **copy** encoding below is never exercised by any capture: index
>   `0x2a` and the literal templates `0x4545FCF3`/`0x45FCF2F3` do not occur in
>   any of the 781 saved command images, and `0xf2` is zero in all 478 of its
>   writes.  Treat it as an unverified STATIC lead.
> * The `f0/f3/f4/fc` launch **is** captured, as a vertexless **rectangle
>   fill/clear** with `fc` repeating the `f4` extent.  That part is real.
>
> The captured surface-to-surface copy is an ordinary textured quad:
> destination at `b0/b1`, source at `78/79/7a`, launched by a draw record.

Reverse-engineered from the vendor Linux driver
`m9xdriver-x86_64-1.4.3-.../xserver/1_13_0/m9x_drv.so` (source file `PLOS_exa.cpp`).
Addresses below are that binary's (file offset == virtual address).

**This engine has nothing in common with the Parhelia/P690 blitter.**  There is no
DWGCTL/FXBNDRY/YDSTLEN register model, so `parhelia_exa.c` cannot be parameterised
for the M9 — it needs a separate implementation.

## Programming model

Work is submitted through a **memory-resident command FIFO** ("BmBuffer") carrying an
indexed register stream:

```
dword0 = idx0 | idx1<<8 | idx2<<16 | idx3<<24     /* 0x45 = pad / stop */
dword1 = data for idx0
dword2 = data for idx1 ...
```

Scanning LSB→MSB, the engine stops at the first `0x45`, so packets carry one to four
data words.  Buffers are chained rather than rung: appending indices `0x2c`/`0x2d`
with a start and end address makes the engine call a sub-buffer — the same idiom as
MGA's SECADDRESS/SECEND (`PostDOSBuffer` @0x303150).

Command-buffer object fields: `+0x08` write pointer, `+0x10` start, `+0x18` limit,
`+0x20` record-only mode, `+0x28` back-pointer.  Vtable `0x150` = flush/grow,
`0x130` = commit, `0xb0` = emit relocated surface address.

Blits are *launched* by an auto-index template plus inline per-rectangle data:

| index | meaning |
|---|---|
| `0x2a` | auto-index template applied to the inline dwords that follow |
| `0x2b` | constant `0x00FFFFFF` |
| `0xe6` | GO: `0x20000000 | (count-1)` |
| `0xfc` | size `w | h<<16` — written last, launches the rectangle |
| `0xf0` | ROP / control word |
| `0xf2` | source `x | y<<16` |
| `0xf3` | destination `x | y<<16` |
| `0xf4` | extent `w | h<<16` |
| `0x90` / `0x92` | fill colour / planemask |

## EXA layer

`PHOENIXEXAInit` @0x284280 fills a 0x130-byte `ExaDriverRec` (EXA 2.6):

* `pixmapOffsetAlign` = `pixmapPitchAlign` = **4096 bytes**
* `maxX` = `maxY` = **16382**
* flags = `EXA_OFFSCREEN_PIXMAPS` only; the CreatePixmap hooks are all NULL, i.e.
  classic linear offscreen management
* `memorySize` = `(VRAM / heads) - 0x1001` rounded down to a page
* **`MarkSync` is NULL**, so every `WaitMarker` is a full engine drain

Hooks (DDX → PLOS back end): PrepareSolid 0x284960→0x290480, Solid 0x284930→0x2927e0,
DoneSolid 0x284900→0x292790, PrepareCopy 0x284870→0x2904d0, Copy 0x284840→0x292550,
DoneCopy 0x284810→0x2924e0, WaitMarker 0x284580→0x2926f0.

`Prepare*` only caches state; nothing reaches hardware until the rectangle accumulator
fills (**5000 rects**) or `Done*` runs.  Solid records are 16 bytes `{x1,y1,x2,y2}`,
copy records 24 bytes `{srcX,srcY,dstX,dstY,w,h}`.

## Raster operations

The X11 alu is translated through a table at 0xe8c1e0 which is exactly a **4-bit
reversal** of the alu; GXcopy → `0xC`.  The hardware value is a truth table indexed by
`(src<<1)|dst`.

Control word (`m9x23629` @0x35bef0):

```
ctrl  = hwrop                              /* low 4 bits */
ctrl |= planemask ? 0x07040000 | (bpp<=8 ? 0x10000000 : 0x08000000)
                  : 0x06000000 | needsDst[hwrop]
ctrl |= 0x20000000  when (dst->0x11e & 7) == 0
```

`needsDst[]` (0xed8120) is `0x01000000` for every ROP except 0x0/0x3/0xC/0xF — so
**bit 0x01000000 = "reads destination"** and **0x00040000 = "planemask enable"**.
Copies always use `hwrop | 0x00250000`.

## Solid

```
REG[0x90] = colour replicated to 32 bits by depth (8bpp *0x01010101, 16bpp *0x00010001)
REG[0x92] = planemask (0xFFFFFFFF when disabled)
REG[0xf0] = ctrl
REG[0xf3] = origin, REG[0xf4] = extent
template 0x4545FCF3, GO = 0x20000000|(n-1), then per rect:
    dst x|y<<16   then   w|h<<16   (launch)
```

EXA passes exclusive ends; the driver converts to origin+extent.  Coordinates are
16-bit unsigned.

## Copy

```
REG[0x90] = 0
REG[0x92] = planemask
REG[0xf0] = hwrop | 0x00250000
template 0x45FCF2F3, GO = 0x20000000|(n-1), then per rect:
    dst x|y<<16,  src x|y<<16,  w|h<<16   (launch)
```

The source is bound as **texture unit 0** — a copy is a textured rectangle through the
unified raster back end, not a separate 2D block.

**Overlap:** the DDX `PrepareCopy` discards `dx`/`dy` entirely and there is no
direction/SGN register anywhere in the path, so the hardware appears to resolve
overlapping source and destination itself.  Worth validating on hardware before
relying on it.

## Synchronisation

`WaitMarker` flushes the command buffer, submits, and waits on a **fence / sequence
number** (`board->0x10` vtable+0x100, infinite timeout) — not a status-register poll.
A flush failure calls `exit(1)` in the vendor driver, which a port should obviously
not copy.

## Surfaces

`MmSurfDescEx::Init` @0x5612a0.  Relevant fields: `+0x19c` bits per pixel, `+0x1a4`
pitch alignment in pixels, `+0x1b0` base alignment (4096), `+0x1b8` total size
(rounded to 0x80), `+0x1c4` pitch in bytes.  Format codes are the familiar
**0x60 / 0x9C / 0xCC** for 8/16/32 bpp, shared with the display controller and cursor.

Limits: surface base 4096-byte aligned, size granularity 0x80, blit coordinates and
extents 16-bit, 96 bpp rejected outright.

## RENDER / Composite

Accelerated, and the constraints are explicit: `op <= PictOpAdd`; a repeating source
must be RepeatNormal; a mask must be PICT_a8 or PICT_a8r8g8b8 with no component alpha;
source and destination formats limited to x8r8g8b8, a8r8g8b8, x8b8g8r8, r5g6b5, a8.

## Not determined

1. **The render-target base/pitch/format encoding (regs 0xb8/0xb9).**  These are
   emitted by the command buffer's relocated-address helper rather than as literals,
   so the layout never appears as a constant.  **This is the one blocker for a
   from-scratch EXA** — everything else in Solid/Copy is pinned down.
2. The index → MMIO offset mapping.  This build never touches the 2D registers through
   MMIO; by analogy with MGA it is probably a 1 KB window at `base + K + index*4`, but
   `K` is unverified.  Irrelevant if the port reuses the DMA stream.
3. The final doorbell / ring head-tail registers (in the kernel module).
4. The `WaitForFence` implementation — shadow-memory spin or ioctl.
5. Meaning of the constant `REG[0x2b] = 0x00FFFFFF` and of the unused raster-block
   registers 0xef/0xd7/0xee/0xec/0x22/0xa7/0xc6.

Resolving (1) needs either a trace of the relocated-address emitter or a live command
buffer dumped on hardware, correlating regs 0xb8/0xb9 against a known framebuffer
address and pitch.

## RESOLVED: the register file is directly MMIO-addressable

The command-stream "index" space is a plain MMIO window:

```
MMIO_offset = 0x1000 + index * 4        /* index 0x00..0xff -> 0x1000..0x13fc */
```

Proved four independent ways in the Windows display driver (`MxMD.dll`), each a case
where a direct-MMIO path and a FIFO path compute the same value: index 0x1a↔0x1068,
0x1b↔0x106c, 0x1c↔0x1070, and 0xcb/0xcc/0xcd↔0x132c/0x1330/0x1334.

**So an EXA port can skip the command FIFO entirely and poke registers**, writing the
launch register last:

| purpose | index | MMIO |
|---|---|---|
| RT0 base | 0xb8 | 0x12e0 |
| RT0 mode (pitch+format) | 0xb9 | 0x12e4 |
| copy source base / mode | 0xb6 / 0xb7 | 0x12d8 / 0x12dc |
| control / ROP | 0xf0 | 0x13c0 |
| fill colour | 0x90 | 0x1240 |
| planemask | 0x92 | 0x1248 |
| src x\|y<<16 | 0xf2 | 0x13c8 |
| dst x\|y<<16 | 0xf3 | 0x13cc |
| extent w\|h<<16 | 0xf4 | 0x13d0 |
| size w\|h<<16 — **launches** | 0xfc | 0x13f0 |
| clip min / max | 0xf5 / 0xf6 | 0x13d4 / 0x13d8 |
| line vertex | 0xfb | 0x13ec |

### Render-target encoding (the former blocker)

```c
/* MMIO 0x12e0 */  addr = (vram_byte_offset & ~3) | (fmt7 & 3);
/* MMIO 0x12e4 */  mode = (fmt8 << 24) | 0x00200000 | (pitch_pixels & 0xffff);
/* fmt8 = 0x60 (8bpp) / 0x9C (16bpp) / 0xCC (32bpp);  fmt7 = fmt8 >> 1 */
```

### Synchronisation

* **Engine busy: poll `MMIO 0x8f0 & 0x00030000`** until clear — the only hardware idle
  check in the Windows driver.  That is all an EXA `WaitMarker` needs.
* The FIFO path instead uses a memory write-back fence (ring start at `MMIO 0x400`,
  doorbell at `MMIO 0x404` written with the byte address of the write pointer, tag
  addresses at `0x1068/0x106c/0x1070`, values ORed with 1 so a zeroed word never reads
  complete).  Neither the vendor's fence wait nor its idle poll has a timeout — a port
  must add one.
* An in-stream wait exists as index 0x43 (MMIO 0x110c).

Note the miniport maps only 0x2000 bytes of MMIO while the display driver normalises
against 0x4000; every offset used here is below 0x2000.

Also confirmed: `MxMD.dll` touches no CRTC/PLL/DDC/cursor registers at all — modesetting
lives entirely in the miniport, the same split this driver already has.

### Still open

The low two bits of the base dword (a format tag), mode-dword bit 23, the low field of
the GO register 0xe6, and the context-base registers 0x1094/0x1098/0x109c/0x10ec.

## Acceleration APIs in the vendor drivers

**There is no XAA anywhere in the M9 Linux driver** — no `XAAInit` / `XAACreateInfoRec`
reference and no XAA string in any build of the package (6.9.0 through 1.13.0).  EXA
only, and the driver bundles its own forked copy (`exa_for_xorg1.13/exa.c`), which is
why `nm -D` shows no `exaDriverInit` import and why its `ExaDriverRec` carries four
Matrox-private fields past the standard ones.  Its only accel knob is `NoAccel`.

The **Windows** XDDM driver hooks a much wider set of GDI operations — `MxMD.dll`
imports the whole punt family, which tells us what it hooks: `EngBitBlt`, `EngCopyBits`,
`EngStretchBlt`, `EngStretchBltROP`, `EngAlphaBlend`, `EngTransparentBlt`,
`EngGradientFill`, `EngTextOut`, `EngLineTo`, `EngStrokePath`, `EngPlgBlt`.  That is not
extra hardware capability, only a wider API surface: EXA implements text, lines and
gradients on top of solid/copy/composite.  Both drivers share a `PuntAll2d` debug knob.

## Implementation status in this driver

`parhelia_m9_exa.c` implements solid and copy over the direct-MMIO path, gated behind
`Option "EXA"` on the M9 (XAA is refused there).  It initialises cleanly on hardware and
X stays up under both VBE and native modesetting, logging
`M9 EXA: solid + copy, offscreen 0x300000..0x10000000`.

**Acceleration is not yet proven to work**, for two reasons that are independent of the
EXA code:

1. **The engine reports permanently busy** — `0x8f0 & 0x00030000` stays set.  Static
   recovery now shows that the vendor `EnableHardware` lifecycle explicitly enables
   `PCI_COMMAND_MASTER`, then builds bridge/display services; it is not a hidden drawing
   reset.  The working path also initializes the privileged requester at `0x400/0x404`,
   its `0x500..0x524` descriptor, address windows, and memory-tag fences.  The direct-MMIO
   prototype omits that submission architecture.
2. ~~Nothing X draws reaches VRAM on the test machine at all.~~ **Retracted 2026-08-12.**
   That conclusion came from scanning VRAM at a guessed offset.  Measured through X
   instead (XFillRectangle + XGetImage readback), software rendering lands correctly:
   accel off gives 0/20 sampled pixels wrong, XAA gives 20/20.  The defect is in the M9
   hardware path only.

**Correction — RT0 is the destination.**  A later session briefly swapped the destination
to `0xb6/0xb7`, claiming `0xb8/0xb9` was a 3D texture stage.  That contradicts the
direct-MMIO table above and is wrong: `0xb8/0xb9` (RT0) is the render target, `0xb6/0xb7`
is the copy source.  With the destination misbound the status register is inert across the
launch (`01060001 -> 01060001`); with RT0 restored it changes (`-> 00060001`), i.e. the
engine consumes the operation.  Pixels still do not appear; the persistent
`0x041c = 0x00080000` and the untraced `EnableHardware` step remain the open leads.

Two build notes worth keeping: adding a source file requires regenerating the target
Makefile (`automake src/Makefile && ./config.status src/Makefile`) or the object is
silently not linked, and `xf86LoadSubModule(pScrn, "exa")` must run before any EXA
symbol is referenced or the server dies with `undefined symbol: exaDriverAlloc`.

## Line drawing

The engine has a real **vector line primitive** — not a Bresenham-parameter engine.  There
is no dx/dy, no error term and no octant code, and horizontal/vertical lines are *not*
special-cased into rectangles.  [MxMD.dll `DrvLineTo` @0x35ae0, `DrvStrokePath` @0x352a0]

| register | index | meaning |
|---|---|---|
| `0x13cc` | 0xf3 | start vertex, `(y<<16) \| (x & 0xffff)`, both signed 16-bit |
| **`0x13ec`** | **0xfb** | **end vertex — draws the segment and becomes the new current point** |
| `0x1224` | 0x89 | pixel mode; bits[2:0] = 1 when bpp >= 16, 0 below.  Only on a bpp change |
| `0x13d4` / `0x13d8` | 0xf5/0xf6 | clip min/max, `(y<<16)\|x`, right/bottom exclusive |

A polyline is one `0x13ec` write per segment with no re-setup.

Lines use their **own control-word base**, not the rectangle path's `0x06000000`:

```
ctrl = (bpp >= 16 ? 0x48470000 : 0x48430000) | hwrop | (clipping ? 0x00008000 : 0)
```

**The endpoint is exclusive — the engine omits the last pixel**, which maps directly onto
XAA's `OMIT_LAST`.  When the caller wants the endpoint drawn, extend by one step along the
major axis.  (No inclusive-endpoint control bit could be located; the vendor never needs
one because GDI also wants exclusive endpoints.  Its `PD_CLOSEFIGURE` handling re-emits
vertex 0 as a final `0xfb`, which would double-hit every joint on an inclusive engine.)

**There is no hardware line styling.**  `DrvStrokePath` punts to `EngStrokePath` for any
geometric/styled/dashed line, any non-NULL style, coordinates outside ±0x7fff, or a complex
clip that overflows its buffer.  Only solid, cosmetic, one-pixel-wide lines are accelerated,
so an XAA port should hook the solid line entries only and leave dashes to software.

Optional: index 0xec (`0x13b0`) carries 28.4 sub-pixel fractions (`y_frac` bits[31:28],
`x_frac` bits[23:20]) — irrelevant for integer XAA lines.

`INDEX_DrvFillPath`, `DrvStrokeAndFillPath` and `DrvPaint` are **not** hooked by the vendor.

## CORRECTION: the 2D destination is 0x12d8/0x12dc, not 0x12e0/0x12e4

The earlier identification of `0xb8`/`0xb9` (`0x12e0`/`0x12e4`) as the render target was
wrong.  In the whole Windows display driver those are written from exactly one place —
inside the **D3D texture-stage** flush — and never from any 2D path.

**The 2D destination binds through `0xb6`/`0xb7` = `0x12d8`/`0x12dc`**, the pair previously
labelled "copy source".  That is consistent with a copy binding its source as texture unit
0: destination on `0xb6/0xb7`, source on `0xb8/0xb9`.

This is the defect that made fills execute cleanly and produce nothing: the engine ran
against whatever stale surface descriptor `0xb6`/`0xb7` happened to hold.

Corrected encoding (`0x467e0`, slot 8 = the 2D surface):

```c
base = (vram_byte_offset & ~3) | (memory_location & 3);   /* 0 = local VRAM   */
mode = (fmt7 << 25) | (flags16_18 << 16) | (pitch_pixels & 0xffff);
/* fmt7<<25 == the familiar 0x60/0x9C/0xCC << 24.  There is NO 0x00200000 bit on
   a 2D slot -- bit 21 belongs to the 3D/texture paths.  Pitch is in PIXELS. */
```

Two further corrections to the control word and init:

* **The top nibble is a pixel-size field** (`[dev+0x238]`, built at `0x48892`): `0x10000000`
  for 32/16 bpp, `0x08000000` for 8 bpp, `0x18000000` for 1 bpp, and it is ORed into *every*
  control word.  A bare `0x06001C0C` tells the engine the wrong pixel size.
* **The aperture windows are triples, not pairs** — `0x1320/0x1324/0x1328` and
  `0x132c/0x1330/0x1334`.  ~~The vendor brackets the block with `0x1398 <- 4`.~~
  **Retracted 2026-08-12:** that came from matching `+0x1398` displacements in
  `m9x_drv.so`, but the same base register is used at `+0x1460`, `+0x1479` and `+0x1570`,
  past the 0x13fc end of the register file — it is a driver context struct, not MMIO.  At
  `0x43b58d` the value stored there is a computed **log2**, i.e. a size/shift field of that
  struct.  No `0x1398` MMIO write is evidenced.  Matching struct displacements against
  register offsets is not valid evidence and was the source of several bad leads.
* The plane mask is an init-time default (`0x00FFFFFF` at 32 bpp), never written per
  operation; and `0x1398` is written *before* an operation (0 then 1) only when the control
  word's bit 24 says the ROP reads the destination.  Nothing is required *after* the launch.
* Clipping is opt-in via **control bit 15 (0x8000)**; if that bit is set while
  `0x13d4`/`0x13d8` are zero, everything is clipped away.
* At 32 bpp the fill colour goes in raw; at 8/16 bpp it must be replicated across the dword.
