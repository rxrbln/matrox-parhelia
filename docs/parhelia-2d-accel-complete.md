# Matrox Parhelia — complete 2D acceleration register map

Provenance: reverse-engineered from the proprietary driver
`matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` (not stripped,
demangled C++/`ParhlXAA*` symbols); all `@0x…` addresses are virtual addresses
inside that object (`objdump -d -C`, disassembly cached in
`scratchpad/mtx_drv.asm`).  Tags: **[CONF]** = read directly from the
disassembled emit path, **[HW]** = proven on real hardware by our own driver,
**[INF]** = inferred from surrounding code, not proven.

Live-hardware ground truth this document is written against: commands reach the
engine through a bus-master DMA ring (`0x404` = buffer bus address, `0x408` =
end|3, `0x314` = 1 kicks; poll `0x8F0` bit17 idle and the `0x450` fence); the
buffer is a stream of tag+data packets; a 2D solid fill has been driven that way
on a real board.  The vendor's own ring uses a different doorbell (`0x3050`),
but the *packet* format and every register below are identical between the two.

Revision 2 (2026-09) closed the gaps left by revision 1 and **corrects three
substantive errors** — see "Corrections to revision 1" at the end.

---

## 1. Command format

Register file: MMIO offset = `0x1000 + (index << 2)`; index = `(offset −
0x1000) >> 2`.  Engine registers occupy 0x1000–0x13FC.

A packet is one **tag dword** followed by up to four **data dwords**.  The tag's
four bytes are register indices in little-endian order (byte0 first); index
`0x21` is the pad/no-op and consumes **no** data dword.  So `0x2121e3c2` writes
two dwords: first to index 0xC2 (0x1308), then to index 0xE3 (0x138C).  An index
may repeat: `0x2121c0c0` writes index 0xC0 twice (used for the `0x707` idle
prewrite followed by the real DWGCTL).  [CONF, every `Subsequent*` and
`HwThread2d::SetupHardware @0x72920`]

Direct MMIO replay of the same (index, value) pairs to `0x1000 + idx*4` is
equivalent; that is what the open driver does.

## 2. Register index map (2D)

| idx | offset | Name | Role |
|---|---|---|---|
| 0x88 / 0x89 | 0x1220 / 0x1224 | **SURF0_BASE / SURF0_CFG** | surface descriptor slot 0 |
| 0x8A / 0x8B | 0x1228 / 0x122C | **SURF1_BASE / SURF1_CFG** | slot 1 |
| 0x8C / 0x8D | 0x1230 / 0x1234 | **SURF2_BASE / SURF2_CFG** | slot 2 |
| 0x8E / 0x8F | 0x1238 / 0x123C | **SURF3_BASE / SURF3_CFG** | slot 3 |
| 0xAF | 0x12BC | **DWGSYNC** | soft-trap sync tag |
| 0xBD | 0x12F4 | filter/LOD constant | init `0x41F80000` (31.0f) |
| 0xC0 | 0x1300 | **DWGCTL** | opcode / BOP / slots / modifiers |
| 0xC1 | 0x1304 | **SRC** | 2D source `(y<<16)\|x`, line start, **or** a linear source offset |
| 0xC2 | 0x1308 | **DSTXY** | destination `(y<<16)\|x` |
| 0xC3 | 0x130C | alternate extent | used only by the non-XAA (DDI) geometry path; semantics unresolved |
| 0xC5 | 0x1314 | **FCOL** | foreground / colour-key colour, bpp-replicated |
| 0xC6 | 0x1318 | **BCOL** | background colour, bpp-replicated |
| 0xC7 | 0x131C | **PAT0** | 8×8 mono pattern rows 0–3 |
| 0xC8 | 0x1320 | **PAT1** | 8×8 mono pattern rows 4–7 |
| 0xC9 | 0x1324 | **PLNWT** | plane write mask (`0xFFFFFFFF` = all) |
| 0xCA | 0x1328 | **SCISSOR_MIN** | `(top<<16)\|left` |
| 0xCB | 0x132C | **SCISSOR_MAX** | `(bottom<<16)\|right` |
| 0xDD | 0x1374 | soft-trap companion | always written 0 next to `DWGCTL=0x707` |
| 0xE2 | 0x1388 | **LINE_END** | `(y<<16)\|x` |
| 0xE3 | 0x138C | **LEN** | extent `(h<<16)\|w`; **the write that fires the op** |

All [CONF] from `HwThread2d::SetupHardware @0x72920`, the `Subsequent*`
functions, and `SetupHardwareForDisplayDriver @0x59c80`.

> **CORRECTION 1 — PLNWT is 0x1324 (index 0xC9), not 0x12C0.**
> `SetupHardware` streams the shadowed plane mask through tag `0x212121c9`
> (@0x72a90), and the engine init list writes `0x1324 = 0xFFFFFFFF` through tag
> `0xbfc5c6c9` (@0x59ea0).  `0x12C0`/`0x12C4` are 3D/DPS registers (init
> `0xFFFFFFFF` / `0x7FFFFFFF` via tag `0xbab1b0b9`), which is what the earlier
> pass mistook for the plane mask.

## 3. Surface descriptors — the source/destination pitch, offset and format

There are **four** descriptor slots in hardware; the vendor 2D list instantiates
only slots 0 and 1 (`SurfDesc2dList::SurfDesc2dList @0x7bdf0` loops twice), and
slot 3 is used ad hoc as the scratch/linear surface by the host-transfer paths.
`SurfDesc2d::Init @0x7bc80` gives, per slot, an *id* and the tag that programs
it [CONF]:

| slot | id (`+0x10`) | tag (`+0x14`) | registers |
|---|---|---|---|
| 0 | `0x000` | `0x21218988` | 0x1220 / 0x1224 |
| 1 | `0x400` | `0x21218b8a` | 0x1228 / 0x122C |
| 2 | `0x800` | `0x21218d8c` | 0x1230 / 0x1234 |
| 3 | `0xC00` | `0x21218f8e` | 0x1238 / 0x123C |

`id == slot << 10`.  DWGCTL takes the destination as `id` (bits [11:10]) and the
source as `id >> 2` (bits [9:8]).

**Base register** (`0x1220 + slot*8`) = the surface's VRAM byte offset.  Bits
[1:0] are a memory-space selector: `Surface2d::Enable @0x7bf00` ORs `0x3` into
it when the surface lives in non-local (AGP/system) memory; for VRAM they are 0.
`XferVidToTmpToVid @0x56330` also branches on **bit 27** of this word to pick a
temp-surface format code — [INF] a "wide/GigaColor" address attribute.

**Config register** (`0x1224 + slot*8`) — layout recovered from
`DeviceBitmap::Create @0x7c430`, `Surface2d::ModifySurfMode @0x7c230` and
`Surface2d::SwapSurfMode @0x7c2d0` [CONF]:

```
bits [13:0]  pitch, in PIXELS, rounded up to a multiple of 32
             (Create: (width + 0x1f) & 0x3fe0)
bits [15:14] reserved (preserved by ModifySurfMode)
bits [18:16] surface type: 3 as created; the "bind as source" path
             (AssignCurrentSurfDescTo, 2nd arg != 0) rewrites it to 2
             (`& 0xC0FAFFFF | 0x20000`).  Bit 17 is always set. [CONF value,
             INF meaning: bit16 ~ "writable/render target"]
bits [20:19] reserved (ModifySurfMode 4th arg)
bit  23      "swap" (Surface2d::SwapSurfMode) — byte/word swap [INF]
bits [29:24] pixel format code
bits [31:30] reserved
```

**Pixel format codes** [CONF `DeviceBitmap::Create @0x7c610`]:

| bytes/px | bpp (`+0x128`) | format (`+0x130`) |
|---|---|---|
| 1 | 8 | **0x11** |
| 2 | 16 | **0x28** |
| 4 | 32 | **0x32** |

**There is no 24bpp packed surface format** — the vendor maps depth 24 onto the
32bpp descriptor.  A second dword at object `+0xC8` carries width/height
(`ModifySurfDim @0x7c290`: `[13:0]` = height, `[27:16]` = 1) but is **not**
emitted to any 2D register; it feeds the memory-size calculation only.

So the open driver's existing `(pitch_px & 0xffff) | 0x20000 | (fmt << 24)` is
correct in effect ([HW] — a fill has been drawn with it); the exact pitch mask
is `0x3FFF` and the pitch must be a multiple of 32 pixels.

## 4. DWGCTL — the complete field map

```
 31           23    20 19  17 16 14 13 12 11 10 9  8 7    4 3    0
 +-------------+-----+------+-----+---+--+-----+----+------+------+
 |             |clip |patY  |patX |P  |T | dst | src| BOP  |opcode|
 +-------------+-----+------+-----+---+--+-----+----+------+------+
```

| Bits | Field | Values |
|---|---|---|
| [3:0] | **opcode** | `0` solid/rect fill · `1` line · `2` BITBLT/copy · `3` colour-expand · `0xC` textured rect (DPS) |
| [7:4] | **BOP** | raster op, `g_adwParhlXaaBopTable[alu]` |
| [9:8] | **source** descriptor slot (0–3) | `srcId >> 2` |
| [11:10] | **destination** descriptor slot (0–3) | `dstId` |
| 12 (0x1000) | pattern-transparent (opcode 0 + bit13) / colour-expand-transparent (opcode 3) | |
| 13 (0x2000) | 8×8 pattern enable (opcode 0) | |
| [16:14] | pattern X origin (0–7) | `patx << 14` |
| [19:17] | pattern Y origin (0–7) | `paty << 17` |
| [22:20] | line mode (opcode 1): `0b011` omit-last, `0b111` draw-last | |
| 22 (0x400000) | colour-key transparency (opcode 2 / textured blit) | |
| 23 (0x800000) | **clip/scissor enable** | |

> **CORRECTION 2 — bits [3:0] are the opcode, not a one-hot destination slot.**
> Revision 1 read `DWGCTL = dstSlot | (srcSlot>>2) | …` as putting a `1<<slot`
> mask in [3:0].  That `(1<<slot)&0xF` value is the driver-side *dirty mask* for
> the shadow flush (`or %eax,0x8(%rbx)`), not part of DWGCTL.  The slot fields
> are [11:10] (dst) and [9:8] (src).  Four independent witnesses:
> `XferVidToTmpToVid` pass 1 `= (src<<8) | 0xCC2` writing into the temp bound at
> slot 3 (`0xC00`) and pass 2 `= (dst<<10) | (bop<<4) | 0x302` reading the temp
> at slot 3 (`0x300`); `CDPSSurface::Fill` `0x5C0` (src 1, dst 1 — the DPS
> colour target is slot 1); `CDPSSurface::Blit` `0x4C2` (src 0, dst 1);
> colour-expand `0x800303` (src slot 3 = the mono scratch).  Under the old
> reading none of those four decode.

**BOP table** `g_adwParhlXaaBopTable @0x333E00`, dumped verbatim [CONF]:
`00 08 04 0c 02 0a 06 0e 01 09 05 0d 03 0b 07 0f` — the 4-bit **bit-reversal**
of the X GX rop index (GXcopy 0x3 → 0xC, GXxor 0x6 → 0x6, GXset 0xF → 0xF).
X defines only 16 GC alu ops and there is no ROP3 table anywhere in the X path,
so these 16 entries are the complete raster-op support.

`DWGCTL = 0x707` (+ `0x1374 = 0`) is the **idle / soft-trap** value.  It is
written immediately before every real DWGCTL **except** when the cached BOP is
GXcopy: `if ((dwgctl & 0xF0) == 0xC0) skip` [CONF @0x53a63, @0x53fac, @0x5618c,
@0x545d0].  [INF] the prewrite drains the destination-read pipeline that a
read-modify-write BOP needs.

## 5. The DrawContext shadow and its flush

Every `ParhlXAASetupFor*` writes a shadow at `pdev->[0x10] + 0xA68`;
`HwThread2d::SetupHardware @0x72920` streams the dirty entries as packets.
Layout, fully recovered [CONF]:

| Shadow | Contents |
|---|---|
| +0x00 | effective dirty word = `+0x08 & +0x10` (recomputed by the flush) |
| +0x04 (byte) | geometry variant select (0 ⇒ index 0xC3, else index 0xE3) |
| +0x08 (dword) | dirty bits, see below |
| +0x10 (dword) | enable mask (each Setup sets it `= +0x08`) |
| +0x18 | DWGCTL |
| +0x1C / +0x20 | SCISSOR_MIN / SCISSOR_MAX |
| +0x24 | PLNWT |
| +0x28 / +0x2C | BCOL / FCOL |
| +0x30 / +0x34 | PAT0 / PAT1 |
| +0x38 / +0x3C / +0x40 | geometry (DDI path only) |
| +0x44 + 4·slot | surface base, slots 0–3 |
| +0x54 + 4·slot | surface config, slots 0–3 |

Dirty bits (bit → what the flush emits, in flush order):

| bit | emits |
|---|---|
| 0 / 2 / 1 / 3 | SURF slot 0 / 2 / 1 / 3 (base+config) |
| 7 | BCOL 0x1318 |
| 8 | FCOL 0x1314 |
| 6 | PAT0 0x131C + PAT1 0x1320 |
| 5 | PLNWT 0x1324 |
| 10 | SCISSOR_MIN 0x1328 + SCISSOR_MAX 0x132C |
| 4 | DWGCTL 0x1300 |
| 9 | geometry (DDI path) |

Colour replication before shadowing (branch on the bitmap's bpp field `+0x128`):
8bpp `c|c<<8|c<<16|c<<24`, 16bpp `c|c<<16`, 32bpp verbatim.  Applies to FCOL,
BCOL **and** PLNWT.

## 6. Engine initialisation / default state

`SetupHardwareForDisplayDriver @0x59C80` is the complete power-on state the
vendor programs before any 2D work [CONF]:

1. Four texture pipes, `i = 0..3`: `0x1200+4i = 0x00040100 | (1<<i)`,
   `0x11C0 = 0`, `0x11C4 = 0x249`, `0x11C8 = 0`, `0x11CC = 0xFFFFFFFF`,
   `0x11D0 = 0x11D4 = 0x11D8 = 0`.
2. `0x1200 = 0x0001000F`, `0x1204 = 0x1208 = 0x120C = 0` — pipes bypassed for 2D.
3. `DWGCTL = 0x2010CC` (opcode 0xC, BOP GXcopy, slots 0).
4. Four pixel-shader stages (`0x1260/0x1264/0x1268/0x126C`).
5. `0x12E4 = 0`, `0x12C0 = 0xFFFFFFFF`, `0x12C4 = 0x7FFFFFFF`, `0x12E8 = 0`.
6. `0x12D4 = 0`, `0x12D0 = 0`, `0x12CC = 0xFFFF`, `0x12C8 = 0x1FF`.
7. **`0x1324 (PLNWT) = 0xFFFFFFFF`, `0x1318 (BCOL) = 0`, `0x1314 (FCOL) = 0`,
   `0x12FC = 0`.**
8. **`0x1328 (SCISSOR_MIN) = 0`, `0x132C (SCISSOR_MAX) = 0x10001000`**
   (= 4096 × 4096), `0x1304 = 0`, `0x1308 = 0`.
9. `0x12F4 = 0x41F80000` (31.0f).
10. Soft trap: `DWGCTL = 0x707`, `0x1374 = 0`, `DWGSYNC` ×2 with the running tag.

---

# The operations, simplest to most advanced

Each list is the exact ordered register sequence.  "SURFn" means write the
base and config pair for that slot.

## Op 1 — Solid rectangle fill  [CONF `SetupForSolidFill @0x536E0`, `SubsequentSolidFillRect @0x539C0`]

```
DWGCTL   = (dstSlot<<10) | (srcSlot<<8) | (BOP<<4) | 0x0
           | 0x800000            if a clip rect is armed
FCOL     = fg  (bpp-replicated)          0x1314
PLNWT    = planemask (bpp-replicated)    0x1324
SURF<dst> base = pixmap VRAM byte offset, config = pitch|0x20000|(fmt<<24)

per rect:
  [ DWGCTL = 0x707 ]                     unless BOP == 0xC
  DWGCTL = value                         0x1300
  DSTXY  = (y<<16) | (x & 0xFFFF)        0x1308
  LEN    = (h<<16) | (w & 0xFFFF)        0x138C   <-- fires
```

For a screen-only fill both slots are the same descriptor, so with slot 0 the
DWGCTL is simply `BOP<<4`.  Width and height are *counts*, not last-pixel
coordinates.

## Op 2 — Horizontal / vertical line  [CONF `SubsequentSolidHorVertLine @0x55ED0`]

Not a line at all: a 1-pixel rectangle fill using the solid-fill DWGCTL.

```
horizontal (dir == 0):  DSTXY = (y<<16)|x ; LEN = (1<<16) | len
vertical   (dir != 0):  DSTXY = (y<<16)|x ; LEN = (len<<16) | 1
```

## Op 3 — Two-point line  [CONF `SubsequentSolidTwoPointLine @0x560F0`]

```
DWGCTL |= 0x700001        normal (last pixel drawn)
DWGCTL |= 0x300001        when XAA passes OMIT_LAST (flags bit0)
packet 0x21e2c1c0:
  DWGCTL     = value                     0x1300
  LINE_START = (y1<<16) | x1             0x1304
  LINE_END   = (y2<<16) | x2             0x1388   <-- fires
```

The modified DWGCTL is written back to the driver's cache, i.e. it is sticky
until the next `SetupFor*`.  There is **no Bresenham entry point and no
DDA/error-term register** — see §"Not supported".

## Op 4 — Screen-to-screen copy (blit)  [CONF `SetupForScreenToScreenCopy @0x551E0`, `SubsequentScreenToScreenCopy @0x55530`, `XferVidToTmpToVid @0x56330`]

Setup builds:

```
DWGCTL = (dstSlot<<10) | (srcSlot<<8) | (BOP<<4) | 0x2
         | 0x1000    if xdir < 0        (see the note below)
         | 0x2000    if ydir < 0        (see the note below)
         | 0x400000  if trans_colour != 0xFFFFFFFF
         | 0x800000  if a clip rect is armed
FCOL   = trans_colour (bpp-replicated), BCOL = 0xFFFFFFFF   (only when keyed)
PLNWT  = planemask
```

The single-pass form is then:

```
  [ DWGCTL = 0x707 ]
  DWGCTL = value                         0x1300
  SRC    = (ysrc<<16) | xsrc             0x1304
  DSTXY  = (ydst<<16) | xdst             0x1308
  LEN    = (h<<16) | w                   0x138C   <-- fires
```

**Overlapping-copy handling — the vendor does NOT use hardware direction bits.**
`SubsequentScreenToScreenCopy` extracts only the BOP (bits [7:4]) and the
transparency bit (22) from the shadow and delegates to `XferVidToTmpToVid`,
which copies **src → temporary surface → dst** in strips that fit the temp
buffer, so overlap can never corrupt.  Concretely, per strip:

```
pass 1 (src -> temp):
  SURF3 base/config = temp surface           0x1238 / 0x123C
  SURF<src> base/config                      (via the shadow flush)
  DWGCTL = 0x707
  packet 0xe3c2c1c0:
    DWGCTL = (srcSlot<<8) | 0xCC2            dst = slot 3, BOP = GXcopy, blit
    SRC    = (ysrc<<16) | xsrc               0x1304
    DSTXY  = 0                               0x1308
    LEN    = (h<<16) | w                     0x138C
pass 2 (temp -> dst):
  SURF<dst> base/config
  DWGCTL = 0x707
  packet 0xe3c2c1c0:
    DWGCTL = (dstSlot<<10) | (BOP<<4) | 0x302   src = slot 3
             | 0x400000 when colour-keyed
    SRC    = 0                               0x1304
    DSTXY  = (ydst<<16) | xdst               0x1308
    LEN    = (h<<16) | w                     0x138C
```

The temp surface is allocated by `Surface2d::AllocTmp` with format code
`0x13/0x14/0x15/0x16` chosen from base-word bit 27.

> **CORRECTION 3 — the blit direction bits 0x1000/0x2000 are unverified.**
> Revision 1 presented them as the hardware reverse-blit controls.  They *are*
> written by `SetupForScreenToScreenCopy` (@0x55505 / @0x554F5) and do reach
> register 0x1300 through the shadow flush, but the draw that follows always
> overwrites DWGCTL with a freshly built value that does not contain them.  So
> the vendor never exercises them and they are [INF] at best; worse, bits 12/13
> are definitely pattern-transparent / pattern-enable for opcode 0 (§Op 6), so
> the two readings collide.  **Recommendation for the open driver:** do not
> rely on them.  Either implement overlapping copies the vendor way (temp
> surface, GXcopy in, real BOP out) or split the copy into non-overlapping
> bands.  If the direction bits are to be tried, they must be validated on
> hardware first.

## Op 5 — Clipping (scissor)  [CONF `SetClippingRectangle @0x535E0`, `DisableClipping @0x53660`]

```
SCISSOR_MIN = (top<<16)    | (left  & 0xFFFF)     0x1328
SCISSOR_MAX = (bottom<<16) | (right & 0xFFFF)     0x132C
DWGCTL     |= 0x800000
```

Disable = `SetClippingRectangle(0, 0, 0xFFF, 0xFFF)` then clear bit 23; the
engine init instead uses `0x10001000` (4096×4096).  Bounds are inclusive
right/bottom in the XAA call, and the colour-expand path programs
`right = x + w`, `bottom = y + h`, so the max corner is **exclusive** there —
program it as `x+w-1 / y+h-1` for an inclusive rect, or `x+w / y+h` to match the
vendor's expand path.  Never set bit 23 without programming both corners.

Hardware clipping is advertised for **solid fill, solid line and
screen-to-screen copy only** (`ClippingFlags = 0x250000`
= `HARDWARE_CLIP_SCREEN_TO_SCREEN_COPY | HARDWARE_CLIP_SOLID_FILL |
HARDWARE_CLIP_SOLID_LINE`, [CONF `SetupInfoRec @0x53110`]).  It is *not*
advertised for the 8×8 pattern fill, even though the register works there.

## Op 6 — 8×8 mono pattern / stipple fill  [CONF `SetupForMono8x8PatternFill @0x53BE0`, `SubsequentMono8x8PatternFillRect @0x53F10`]

> **CORRECTION 4 — the 8×8 pattern is TWO PATTERN REGISTERS, not an offscreen
> VRAM surface.**  Revision 1 said "patx/paty are its VRAM origin".  They are
> not.  `Mono8x8PatternFillFlags = 0x230200` includes
> `HARDWARE_PATTERN_PROGRAMMED_BITS (0x10000)`, so XAA passes the pattern's two
> data dwords in the `patx`/`paty` arguments of `SetupFor…`, and the driver
> stores them straight into shadow +0x30/+0x34, which the flush emits through
> tag `0x2121c8c7` to **0x131C and 0x1320**.  This removes the "needs a scratch
> VRAM allocator" blocker entirely — stipple fill is directly implementable.

```
PAT0  = pattern rows 0-3 (8 bytes as 2 dwords)   0x131C
PAT1  = pattern rows 4-7                          0x1320
FCOL  = fg (bpp-replicated)                       0x1314
BCOL  = bg (bpp-replicated)                       0x1318   (opaque only)
PLNWT = planemask                                 0x1324
DWGCTL = (dstSlot<<10) | (BOP<<4) | 0x0
         | 0x2000                     opaque      (bg != -1)
         | 0x3000                     transparent (bg == -1, BCOL not emitted)
         | 0x800000                   if clipping armed

first rect after Setup (packet 0x21e3c2c0):
  DWGCTL |= (patx << 14) & 0x1C000     pattern X origin, 0-7
  DWGCTL |= (paty << 17) & 0xE0000     pattern Y origin, 0-7
  DWGCTL = value                        0x1300
  DSTXY  = (y<<16) | x                  0x1308
  LEN    = (h<<16) | w                  0x138C
subsequent rects (packet 0x2121e3c2): DSTXY, LEN only
```

Bit order inside each pattern byte is **MSB first**
(`BIT_ORDER_IN_BYTE_MSBFIRST` in the flags word); the origin is a *screen*
origin (`HARDWARE_PATTERN_SCREEN_ORIGIN`), and is programmed rather than
requiring XAA to pre-rotate the pattern (`HARDWARE_PATTERN_PROGRAMMED_ORIGIN`).

**Colour 8×8 pattern is NOT supported** — `SetupForColor8x8PatternFill` is left
NULL.  A colour tile has to be drawn as repeated blits from a cached pixmap.

## Op 7 — Transparency / colour key

The only chroma mechanism the 2D engine offers is:

```
DWGCTL |= 0x400000
FCOL    = key colour, bpp-replicated       0x1314
BCOL    = 0xFFFFFFFF                       0x1318
```

There is no separate chroma-key register and no key *range* — the key lives in
FCOL, so a keyed blit cannot also carry a foreground colour.  Used by keyed
screen-to-screen copies and by `WritePixmap` when a transparency colour is
given.  [CONF @0x553E9, @0x56869, @0x575DE]

## Op 8 — CPU → screen colour expand (mono glyphs / stipple upload)  [CONF `SetupForCPUToScreenColorExpandFill @0x54220`, `Subsequent… @0x54530`, `Sync… @0x54570`]

**There is no host-data FIFO / ILOAD port.**  XAA is handed a plain system-RAM
staging buffer — `ColorExpandBase = ParhlXaaData + 0x44`, `ColorExpandRange =
0x10000` (64 KiB), i.e. an in-object host buffer, **not** an MMIO window
[CONF `SetupInfoRec @0x53259`].  `Subsequent…` only records `x, y, w, h,
skipleft` at `priv+0x10044…0x10054` and sets a pending flag; the transfer is
performed lazily by `ParhlXAASync` → `SyncCPUToScreenColorExpandFill`.

Setup:

```
DWGCTL = (dstSlot<<10) | (BOP<<4) | 0x800303
         | 0x1000    when bg == -1 (transparent)
FCOL   = fg, BCOL = bg (opaque only), PLNWT = planemask
```

`0x800303` = opcode 3 (colour expand) + source slot 3 + clip enable.  Bit 23 is
set unconditionally here because the expand always programs a scissor for the
skipleft/right edge; `ParhlXAASync` restores the caller's clip state afterwards
(@0x5356A / @0x535B3).

Flush, per pass (a pass covers as many scanlines as the scratch buffer holds):

```
  [ DWGCTL = 0x707 ]                        unless BOP == 0xC
  shadow flush (surfaces, colours, planemask, DWGCTL)
  packet 0x2121cbca:
    SCISSOR_MIN = (y<<16)     | x           0x1328
    SCISSOR_MAX = ((y+h)<<16) | (x+w)       0x132C
  packet 0x2121218f:
    SURF3_CFG   = scratch config & 0xC0F8FFFF     0x123C
                  (format field 0 and type field 0 => 1bpp linear)
  copy the mono rows from the host buffer into the scratch surface,
  through MmHostAperture::Lock (a CPU pointer to a mapped BAR window)
  packet 0xe3c2c18e:
    SURF3_BASE  = (scratchOffset & 0xFFE00000) + surfaceBase   0x1238
    SRC         = (scratchOffset & 0x001FFFFF) << 3            0x1304  (BIT offset)
    DSTXY       = (y<<16) | (x - (skipleft & 7))               0x1308
    LEN         = (lines<<16) | widthBits                      0x138C  <-- fires
  packet 0x2121afc5:
    FCOL        = fg                                           0x1314
    DWGSYNC     = tag | 0x4                                    0x12BC
```

Key facts for an implementation: the source is **linear**, its "XY" register
0x1304 carries a **bit offset**, the scratch config's format and type fields are
zeroed to select 1bpp, `skipleft` is split into a whole-byte part folded into
the offset and a 0–7 bit part subtracted from the destination X, and the row
width is padded to 32 bits.  Bit order in the host data is LSB-first
(`CPUToScreenColorExpandFillFlags = 0x8000` = `SYNC_AFTER_COLOR_EXPAND` only —
no `BIT_ORDER_IN_BYTE_MSBFIRST`, no `LEFT_EDGE_CLIPPING`, dword padding).

## Op 9 — CPU → screen colour image (WritePixmap / UploadToScreen)  [CONF `ParhlXAAWritePixmap @0x55780` → `XferNativeToVid @0x574A0`]

Same shape, colour instead of mono:

```
  DWGCTL = 0x707
  if transparency colour given:
    DWGCTL |= 0x400000
    packet 0x2121c6c5: FCOL = trans colour, BCOL = 0xFFFFFFFF
  packet 0x218f8ec0:
    DWGCTL     = value                      0x1300
    SURF3_BASE = scratch base               0x1238
    SURF3_CFG  = scratch config             0x123C
  TempBitBuffer::AllocSurfaceGen + TempBit::OpenSurfaceWrite
  memcpy the host rows into the aperture-mapped scratch
  packet 0x21e3c2c1:
    SRC   = scratchOffset >> log2(bytes/pixel)   0x1304  (PIXEL offset, linear)
    DSTXY = (y<<16) | x                          0x1308
    LEN   = (h<<16) | w                          0x138C  <-- fires
  packet 0x2121afc5: FCOL, DWGSYNC
```

`ParhlXAAWritePixmap` also programs PLNWT directly (tag `0x212121c9`, @0x55937)
— another confirmation that 0x1324 is the plane mask.

For EXA, a plain `memcpy` into the destination pixmap is a legitimate
simplification when `alu == GXcopy` and the plane mask is full; it is *not*
equivalent when a BOP or plane mask is in play, which is exactly why the vendor
routes even uploads through the engine.

## Op 10 — Screen → CPU (ReadPixmap / DownloadFromScreen)  [CONF `ParhlXAAReadPixmap @0x55AC0` → `XferVidToNative @0x56A50`]

The mirror image: blit VRAM → linear scratch (slot 3 as the **destination**,
packets `0x21218fc2` and `0xe3c1c08e`), soft-trap sync (`0x21afddc0`), wait for
the `0x450` fence, then `MmHostAperture::Lock` and CPU-read the rows out.

## Op 11 — Scaled blit / stretch (Xv, and any scaled composite)  [CONF `CDPSSurface::Stretch @0x6EC00`; unchanged from revision 1]

Runs through the DPS textured-rectangle path, not the 2D blitter:

| Reg | Value |
|---|---|
| 0x1300 | `0x1400CC` or `0x1C00CC`, `\| (activePipeMask<<8)` — opcode 0xC, BOP GXcopy; the 0x1C variant when `hwrev+0x180 > 5` |
| 0x12F4 | `0x41700000` (15.0f) when filtering is on (default state is 31.0f) |
| 0x1220 / 0x1224 | source surface descriptor (slot 0) |
| — | `CDPSTextureEngine::UpdateSrcSurfDesc` binds the source as a texture |
| 0x12C0 / 0x12C4 | `0xFFFFFFFF` / `0x7FFFFFFF` (3D colour masks, **not** PLNWT) |
| 0x12E8, 0x12C8, 0x12FC, 0x12E4 | 0 |
| 0x1324 | `0xFFFFFFFF` — plane mask |
| — | pixel-shader, texture-engine, texture-setup `UpdateHardware` |
| 0x1300 | `0x307` — launch |
| 0x12BC | DWGSYNC ×2 |

The scale factors are **not** an integer register: they are the per-scanline
S/T DDA increments `srcW/dstW`, `srcH/dstH` as IEEE-754 floats, computed by
`CDPSTextureSetup::ComputeIncrements` and emitted to the coordinate block
**0x1160–0x1198**.  `SetSrc(LRECT&,LRECT&)` / `SetDst(LRECT&)` define the
rectangles that drive them.

Reference points for the same opcode family: `CDPSSurface::Fill @0x6E9E0` emits
`SURF1_BASE/CFG` then `DWGCTL = 0x5C0` (opcode 0, src slot 1, dst slot 1) and
`FCOL / 0x1304 / 0x1308 / LEN`; `CDPSSurface::Blit @0x6F020` emits
`0x1220, 0x1224, 0x1228, 0x122C, DWGCTL = 0x4C2, 0x1308, 0x1304, 0x138C`
(opcode 2, src slot 0, dst slot 1).

## Op 12 — Synchronisation and the reset path

**Fast idle** [HW]: poll MMIO `0x8F0` bit 17 (0x20000) = bus-master idle.

**Fence / tag sync** [CONF `ParhlXAASync @0x53410`]:

```
tag = (tag + 8); if (tag == 0) tag += 8;      running 32-bit counter, step 8
packet 0x21afddc0:
  DWGCTL  = 0x707                             0x1300
  0x1374  = 0
  DWGSYNC = (pdev->flags & 1) | tag           0x12BC
submit; then poll MMIO 0x41C (DMA GET pointer, masked & ~7) and compare
against the tag window, handling wrap.
```

`MMIO 0x450` is the **drawing-progress fence**: the surface-busy test
(`MmSurface::IsBusyDWG`) reads it and compares against the mark stored with the
surface, with a wrap window of 0x80 (@0x54C25).  `MMIO 0x384` is written 0 to
unstick a stalled queue (@0x54EA9).  The vendor's pushbuffer doorbell is
`MMIO 0x3050 = 1`, written after every `Subsequent*`.

`HSLPARWaitEngineNotBusy @0xBA850` just returns a cached driver status dword —
it is **not** an MMIO poll.  (Revision 1's "reads a status dword and tests bits
[31:30] == 01 for busy" attributed the caller's test to the wrong function.)

**Reset path** [CONF `HSLPARHardwareReset @0xC5580`].  Entered when the cached
status `dev+0x8 & 0xC0000000` is `0b00` or `0b01`.  For device IDs 0x0528 and
0x2537–0x2539:

```
1. HSLPARSaveCriticalData()
2. save PCI cfg 0x04, 0x0C, 0x10, 0x14, 0x30, 0x3C, 0x40
   (plus 0xF4, 0xF8 when (dev+0x3a8 & 0xE0) == 0x80)
3. MMIO 0x308 = 3
4. read MMIO 0x300, rebuild the field word, write it back with bit 8 cleared
5. PCI cfg 0xE0 = 3
6. PCI cfg 0xE0 = 0            <-- the chip reset strobe
7. wait 10000 (ClientWait, ~10 ms)
8. restore the saved PCI config dwords
9. MMIO 0x300 = the full rebuilt word
10. HSLPARRestoreCriticalData()
```

On a plain PCI/PCI-X board the 0x300 rebuild collapses to
`old_low16 | ((cfg04 >> 2) & 0x80000)`.  There is **no engine-only soft reset**
in this driver — recovery is a chip-level reset plus full re-init
(`SetupHardwareForDisplayDriver`).

---

# What the hardware does NOT accelerate

Taken from `ParhlXaaData::SetupInfoRec @0x53110`, which is the authoritative
capability list: every `XAAInfoRec` hook the vendor leaves NULL is a hook the
Parhelia 2D engine cannot serve.  Offsets verified against
`/usr/X11R7/include/xorg/xaa.h` (three independent anchor matches).

**Accelerated** (hook installed): `SetupForScreenToScreenCopy` +
`SubsequentScreenToScreenCopy`, `SetupForSolidFill` +
`SubsequentSolidFillRect`, `SetupForSolidLine` +
`SubsequentSolidTwoPointLine` + `SubsequentSolidHorVertLine`,
`SetClippingRectangle` + `DisableClipping`, `SetupForMono8x8PatternFill` +
`SubsequentMono8x8PatternFillRect`, `SetupForCPUToScreenColorExpandFill` +
`SubsequentCPUToScreenColorExpandFill`, `ScreenToScreenBitBlt`,
`FillSolidRects`, `FillSolidSpans`, `WritePixmap`, `ReadPixmap`.

**Not accelerated — must fall back to software:**

| Missing | Consequence |
|---|---|
| `SetupForDashedLine`, `SubsequentDashedTwoPointLine`, `SubsequentDashedBresenhamLine` | **no styled/dashed lines** — there is no line-pattern register at all |
| `SubsequentSolidBresenhamLine`, `SolidBresenhamLineErrorTermBits` | **no Bresenham/DDA line entry point**; the engine takes two endpoints and runs its own internal DDA, so there are no exposed error-term / major-minor / octant registers |
| `SubsequentSolidFillTrap`, `SubsequentMono8x8PatternFillTrap` | no trapezoid/edge-walked fills |
| `SetupForColor8x8PatternFill` + Subsequents | **no colour 8×8 pattern/tile fill** |
| `SetupForScreenToScreenColorExpandFill` | no VRAM-sourced colour expand |
| Scanline colour expand (`SetupForScanlineCPUToScreen…`, buffers) | no per-scanline expand |
| `SetupForImageWrite`, `ImageWriteBase`, scanline image write | **no host-data image port** — colour uploads go through a scratch surface |
| `SetupForImageRead`, `SubsequentImageReadRect` | no image-read port |
| `WriteBitmap` | mid-level bitmap write unaccelerated |
| `FillMono8x8PatternRects/Spans`, `FillColor8x8Pattern*`, `FillCacheBlt*`, `FillColorExpand*`, `FillCacheExpand*`, `FillImageWriteRects` | all the composite mid-levels |
| `TEGlyphRenderer`, `NonTEGlyphRenderer` | **no hardware glyph/text engine**; text is drawn by the generic colour-expand path.  (`TEGlyphRendererFlags \|= 0x8000` is written at @0x5320A but the renderer pointer is NULL, so that OR is dead code.) |
| `CopyArea`, `CopyPlane`, `PushPixelsSolid`, `PolyFillRect*` | GC-level hooks all NULL |
| 24bpp packed surfaces | `DeviceBitmap::Create` only emits 8/16/32bpp format codes |

Flag words, for completeness [CONF]: `SolidFillFlags = 0`,
`SolidLineFlags = 0`, `ScreenToScreenCopyFlags = 0` (all four blit directions
allowed at the XAA level), `ClippingFlags = 0x250000`,
`Mono8x8PatternFillFlags = 0x230200`,
`CPUToScreenColorExpandFillFlags = 0x8000`, `ColorExpandRange = 0x10000`,
`WritePixmapFlags = ReadPixmapFlags = FillSolidRectsFlags =
FillSolidSpansFlags = ScreenToScreenBitBltFlags = 0`.

## Composite / alpha blend — CONFIRMED absent from the 2D path

Revision 1's claim is **confirmed** and now rests on a stronger negative:

- A byte-level scan of `mtx_drv.so` finds **no packet tag containing register
  index 0xAB** (0x12AC, the blend-equation register) and **no MMIO write to
  0x12AC** anywhere in the object.  The only textual hit for "12ac" in the
  disassembly is a branch offset.
- `CDPSSurface::Fill @0x6E9E0` (`DWGCTL = 0x5C0`) and `CDPSSurface::Blit
  @0x6F020` (`DWGCTL = 0x4C2`) are plain opaque rectangles; neither touches a
  blend factor bank.
- `CDPSSurface::Stretch @0x6EC00` is a filtered textured rectangle; its pixel
  shader (`BuildColorConvertShader`) does YCbCr→RGB colour-space conversion for
  Xv, not Porter-Duff.
- The vendor X driver therefore does RENDER/Composite entirely in software.

Practical guidance unchanged: **do not advertise `PrepareComposite`**.  A
hardware Porter-Duff path would have to go through the 3D/WARP blend unit
(blend equation index 0xAB = 0x12AC, plus the src/dst factor bank behind
`WriteBuffer` key 0x5 whose base is still undetermined) — a separate 3D effort,
not a 2D one.

---

# Implementable summary (direct MMIO)

Colours are bpp-replicated; `SURF<n>` = write `0x1220+8n` (base) and
`0x1224+8n` (config).  Slot 0 for the destination and slot 1 for a distinct
source keeps DWGCTL slot fields at `0` and `0x100`.

| Hook | Sequence | Tag |
|---|---|---|
| **Init once** | full `SetupHardwareForDisplayDriver` list (§6); minimally `PLNWT=0xFFFFFFFF`, `SCISSOR=0 / 0x10001000`, `FCOL=BCOL=0`, `DWGCTL=0x707` | CONF |
| **PrepareSolid**(alu,pm,fg) | `dwgctl = bop[alu]<<4`; `FCOL=fg`; `PLNWT=pm` | CONF |
| **Solid**(x1,y1,x2,y2) | wait 0x8F0.17; SURF0; `[0x1300=0x707]`; `0x1300=dwgctl`; `0x1308=(y1<<16)\|x1`; `0x138C=((y2-y1)<<16)\|(x2-x1)` | CONF+HW |
| **PrepareCopy**(alu,pm) | `dwgctl = bop[alu]<<4 \| 0x2 \| (srcSlot<<8) \| (dstSlot<<10)`; `PLNWT=pm` | CONF |
| **Copy**(sx,sy,dx,dy,w,h) | wait; SURF0 = dst, SURF1 = src; `[0x1300=0x707]`; `0x1300=dwgctl`; `0x1304=(sy<<16)\|sx`; `0x1308=(dy<<16)\|dx`; `0x138C=(h<<16)\|w`. **Overlap: band the copy or route through a scratch surface — the direction bits are unproven** | CONF |
| **Copy, colour-keyed** | as Copy, `dwgctl \|= 0x400000`, `FCOL=key`, `BCOL=0xFFFFFFFF` | CONF |
| **Stipple / Mono8×8** | `0x131C=pat0`; `0x1320=pat1`; `FCOL=fg`; `BCOL=bg`; `PLNWT=pm`; `dwgctl \|= 0x2000` (opaque) / `0x3000` (transparent) `\| (patx<<14 & 0x1C000) \| (paty<<17 & 0xE0000)`; then DSTXY + LEN | CONF |
| **HorVert line** | solid-fill dwgctl; `0x1308=(y<<16)\|x`; `0x138C = (1<<16)\|len` (H) or `(len<<16)\|1` (V) | CONF |
| **Two-point line** | `dwgctl \|= 0x700001` (or `0x300001` to omit the last pixel); `0x1300`; `0x1304=(y1<<16)\|x1`; `0x1388=(y2<<16)\|x2` | CONF |
| **Clip** | `0x1328=(top<<16)\|left`; `0x132C=(bottom<<16)\|right`; `dwgctl \|= 0x800000` | CONF |
| **UploadToScreen** | GXcopy + full planemask: `memcpy` rows into the pixmap (aperture).  Otherwise: scratch surface + blit as in Op 9 | CONF |
| **DownloadFromScreen** | `memcpy` rows out; for a non-linear source use Op 10 | CONF |
| **Composite** | **do not implement** — no hardware path | CONF (neg) |
| **Xv / scaled blit** | Op 11 (DPS texture path) | CONF |

## Corrections to revision 1

1. **PLNWT = 0x1324 (index 0xC9)**, not 0x12C0.  0x12C0/0x12C4 are 3D/DPS
   registers.
2. **DWGCTL [3:0] is the opcode**; the surface slots are [11:10] (destination)
   and [9:8] (source).  The `1<<slot` value revision 1 placed in [3:0] is the
   driver's internal shadow dirty mask.
3. **The 8×8 mono pattern is programmed as data**, in PAT0 (0x131C) / PAT1
   (0x1320) — not as an offscreen VRAM surface.  The "needs a scratch VRAM
   allocator, deferred" note for stipple fill is withdrawn; only the *colour*
   expand and the colour image upload need a scratch surface.
4. **The blit direction bits (0x1000/0x2000) are unproven** and are never used
   by the vendor's own copy path, which routes every screen-to-screen blit
   through a temporary surface.  They also collide with the pattern-enable /
   pattern-transparent meanings of the same bits.
5. `HSLPARWaitEngineNotBusy` does not poll MMIO; the `[31:30]` status test
   belongs to `HSLPARHardwareReset`'s caller-side check.
6. The colour-expand staging buffer is **host RAM inside the driver object**
   (`ColorExpandBase = ParhlXaaData+0x44`, 64 KiB), which is the concrete
   evidence for "no ILOAD port" that revision 1 asserted from the Lock/blit
   sequence alone.
7. The full engine default-state list (§6) and the chip reset path (§Op 12) were
   missing entirely.

## Still open

1. **Config bits [20:19] and [15:14]** of the surface descriptor: written by
   `Surface2d::ModifySurfMode`, but that function has no call site inside
   `mtx_drv.so` (the callers live in the closed Windows-lineage HAL), so the
   argument values were never observed.  Bit 23 ("swap") likewise has only one
   call site and no observed value.
2. **Index 0xC3 (0x130C)**, the alternate extent register used by the non-XAA
   geometry path, and the shadow `+0x04` flag that selects it.
3. **Index 0xDD (0x1374)** — always written 0 alongside `DWGCTL = 0x707`; role
   unknown.
4. **Bits 12/13 of DWGCTL under opcode 2** — pattern bits, direction bits, or
   both depending on opcode.  Needs a hardware experiment.
5. **The temp-surface format codes 0x13–0x16** passed to `Surface2d::AllocTmp`,
   and the meaning of base-word bit 27 that selects between them.
