# Matrox M9 series ("Phoenix", 0x102b:0540)

Reverse-engineering notes for the M9 family (M9120 / M9138 / M9140 / M9148).  These
are board variants of **one** silicon: `0x0540` is the only PCI device ID, and the
model is distinguished by the subsystem ID and the VBIOS PInS table, never by the
device ID.

Sources, with the same confidence tags used elsewhere in `docs/`:

* `[M9LIN]` — `m9xdriver-x86_64-1.4.3-.../xserver/1_13_0/m9x_drv.so`.  Contains both
  the `Phx*` (M9) and `S90*` (P690) low-level modules, so every claim below could be
  diffed directly against the P690 code we already had verified.
* `[M9WIN]` — the XDDM Windows driver, `MSerX.cab` → `MxMM.sys` (miniport, does the
  real hardware work), `MxMDI.dll`, `MxMD.dll`.
* `[HW]` — measured on an M9148 LP, confirmed by register readback or by what the
  monitor actually displayed.  **Where `[HW]` and the disassembly disagree, `[HW]`
  wins** — several published-looking facts turned out to be wrong (see the
  corrections at the end).

The display block is an evolved SUN90 (P690) controller: same overall shape, but
several registers moved and — dangerously — **their old addresses were reused for
something else**.  The rendering engine is unrelated to the MGA-lineage blitter and
is out of scope; the driver runs the M9 with shadowfb only.

The modern M9148 target's read-only warm snapshot confirms that this similarity
extends to the complete modesetting flow, not just the register layout. Firmware
left controller 0 in a conventional linear 32-bpp 1024x768 mode (pitch 1024,
surface offset zero) with the standard 65 MHz 1024x768 timing and the external
digital link already trained. A production KMS driver can reuse the mature
Parhelia atomic pipe, framebuffer, gamma, vblank, and event flow through a common
implementation. Register maps and encodings, controller count, output routing,
PLL programming, and M9148 external-link handling remain chip operations. See
`captures/m9x-cleanroom-warm-display-20260914/`. `[HW]`

## Address map at a glance

BAR0 = framebuffer aperture (256 MB on the M9148, regardless of installed RAM),
registers = BAR1, **or BAR2 when BAR0 is a 64-bit BAR** (it is, so the MMIO aperture
lands in PCI resource slot 2).  `[M9LIN, HW]`

| what | P690 | M9 |
|---|---|---|
| display controllers | 2 | **4** |
| DC control blocks | 0x1800 / 0x1a00 | 0x1800 / 0x1a00 / **0x1980 / 0x1b80** |
| DC timing blocks | 0x1900 / 0x1b00 | 0x1900 / 0x1b00 / **0x1e00 / 0x1f00** |
| clock select | 0x1c00 / 0x1c04 | **0x1c00 / 0x1c08 / 0x1c04 / 0x1c0c** (ctrl 0/1/2/3) |
| VGA / native flag | 0x1c08 bit0 | **0x1c10 bit0** (bit1 = native active) |
| memory watermarks | 0x1c0c / 0x1c10 | **0x1c14 / 0x1c1c / 0x1c18 / 0x1c20** |
| DACs | 0x1c18 / 0x1c1c | **0x1c80 / 0x1c84** |
| digital links | 0x1c20/24/54/58 | **0x1c9c / 0x1ca0 / 0x1ca4 / 0x1ca8** |
| DDC I²C | 0x1c48 | 0x1c48 (unchanged), plus 0x1c4c |

**The P690 memory-watermark write (`0x1c0c`/`0x1c10` = 0x303 | 0x808) lands on the M9's
controller-3 clock select and its global vid-reset.**  Any code shared with the older
parts must be guarded; see `PARHELIA_IS_M9()` / `PARHELIA_IS_CLASSIC()` in
`parhelia_reg.h`.

~~Controllers 2 and 3 are not independent heads: they are secondary planes
composited into head 0 / head 1, borrowing those timing blocks.~~  `[M9WIN]`
**Refuted on hardware (2026-09-17):** with four displays attached the vendor
driver gives each of the four controllers its own timing block, its own
synthesiser, its own link and its own advancing line counter.  See
`docs/parhelia-m9148-fourhead-outputs.md`.  `[HW]`

## Display controller

`C` = DC control base, `T` = DC timing base.

| offset | contents |
|---|---|
| `C+0x00` | `1` (enable) \| `interlace<<12` \| `fmt<<22` — written **last**, cleared **first** |
| `C+0x04` | 0, or 0x00010000 when interlaced |
| `C+0x08` / `C+0x0c` | Xstart<<16 / (Xend-1)<<16 |
| `C+0x14` / `C+0x18` | Ystart / (Yend-1) |
| `C+0x48` | scanout start, byte offset (alignment 256/512/1024 B by depth) |
| `C+0x4c` | second-field start (interlaced) |
| `C+0x58` | **pitch in PIXELS** \| `3<<16` \| `fmt<<24` |
| `C+0x88` / `C+0x90` | hold pair around timing writes; always targets DC0/DC1 |

Surface format codes are the drawing engine's: **8 bpp 0x60, 16 bpp 0x9C, 32 bpp 0xCC**
(the P690 used 0x28/0x32).  `[M9LIN]`

Timing fields are start/end pairs packed `start | end<<16`, **15-bit horizontal,
14-bit vertical**, each stored as *(value − 1)* — except where noted:

| offset | contents |
|---|---|
| `T+0x00` | bit0 interlace, bits[4:3]/[6:5] H/V zoom, **bits[17:16] video reset** |
| `T+0x08` | (hdisp-1) \| (htotal-1)<<16 — horizontal blank |
| `T+0x10` | (htotal-1) \| (hdisp-1)<<16 — **horizontal**, not vertical |
| `T+0x14` | (hsync_start-1) \| (hsync_end-1)<<16 |
| `T+0x18` | left at 0 by a working VBIOS mode `[HW]` |
| `T+0x20` | (vdisp-1) \| (vtotal-1)<<16 — **vertical**, not horizontal |
| `T+0x2c` | (vtotal-1) \| (vdisp-1)<<16 |
| `T+0x30` | (vsync_start-1) \| (vsync_end-1)<<16 |
| `T+0x5c` | vtotal − 2 `[HW]` |

The `+0x10` / `+0x20` assignment is the one that matters most: swapping them leaves
every register reading back correctly while the raster stays on a default 800×525.

## Clocks

Nine frequency synthesisers, **not** at a uniform stride: fs 0..3 at
0x0e30/34/38/3c, fs 6 at **0x0e40**, fs 7 at **0x0e44**; fs 4 and 5 have no register.
`[M9WIN]`  Oscillator control at `0x0e24` (bit31 power-down, bit30 enable) — same as
the P690.

Divider word and programming sequence are **identical** to the P690, so
`ParheliaFSPLLSolve()` is shared, parameterised only by the reference clock:

```
word = (b0-1) | ((b1-1)<<6) | ((b2-1)<<11) | ((b3-1)<<16)
write word | bit31 | bit21 ; write word ; wait ; poll bit24 (lock)
Fout = ref * b0 * b1 / b2 / (2*b3)
```

Reference is **27 MHz** on the M9 `[M9LIN, M9WIN]` (the P690 measures 24 MHz on
hardware despite its vendor code also saying 27 — see `docs/parhelia-variants.md`).

Clock-select register: **bit0 = hold**, **bits[4:1] = source select**.  A working
VBIOS mode runs controller 0 at select 4 with only 0x0e30/34/38 programmed, so the
select is `fs + 2` `[HW]`.  Note this contradicts a "clksel = fs − 2" reading of the
Linux driver, and `[M9WIN]` could not resolve the mapping at all.

## VGA → native handoff

**POST always leaves the card here.**  `[HW, CAPTURED]`  The post-POST snapshot in
`captures/m9x-vendor-fourhead-20260917/post-baseline2.csv` holds `0x1c10 = 1` and
VGA MISC `0x63`, and the vendor driver running four heads holds `0x1c10 = 2` and
VGA MISC `0x68`.  A Linux boot with `vga=` merely hid it, because the BIOS mode
set performs the same handoff; without `vga=` the driver must do it itself or the
head keeps sending the VGA raster.

```
already native  <=>  (0x1c10 & 2) && !(0x1c10 & 1) && (0x1fdc & 0x10000)

0x1c10 &= ~1              ; 0x1fdc |= 0x10000 ; wait 10 us
0x1c10 |= 2               ; VGA MISC (0x1fc2) = 0x68      (0x67 to go back)
0x1cac |= 0xf0
0x1c00 |= 0x02000001      ; 0x1c08/0x1c04/0x1c0c |= 1
0x1c24 |= 0x101           ; wait 10 us
0x1800 &= ~0x2000
```

**Which of those steps actually matter, MEASURED 2026-09-18 on the M9148 with a
1280x720 mode programmed on head 0:**

| step | effect |
|---|---|
| `0x1c10 &= ~1` | **decisive for the raster.**  With the bit set the transmitter reported `640x480 htotal 800 vtotal 525`; clearing it gave `1280x720 htotal 1650 vtotal 750`.  Setting it back afterwards stopped the transmitter sending anything at all. |
| **VGA MISC = `0x68`** | **decisive for the clock.**  Bits [3:2] are the CRTC clock select and POST leaves them `00`, the fixed **25.175 MHz VGA oscillator**.  With the geometry already right the head rastered at **15.0 kHz / 20 Hz**, and writing a different divider word to its synthesiser (locked, verified) did not change it by a hertz; sweeping `0x1c00`'s select over 0..7 gave the same 20 Hz for every pixel select.  `0x68` took it to **45.0 kHz / 60.00 Hz**, the programmed 74250 kHz.  A monitor reports the 20 Hz raster as *out of range*, which is the whole visible symptom. |
| `0x1fdc |= 0x10000` | no measured effect on what is sent; written anyway because a native card carries it. |
| `0x1cac |= 0xf0` | **must not be written blind** — see the pad-gate note below. |
| `0x1c00/04/08/0c` holds, `0x1c24 |= 0x101` | kept as measured, not proven to be required. |
| `0x1800 &= ~0x2000` | no measured effect; the POSTed control word has bit 13 *set*. |

The register is byte wide: VGA MISC is written at `0x1fc2` and reads back at
`0x1fcc`, and three unrelated VGA ports share the dword at `0x1fc0`, so it may
only be accessed 8 bits at a time.

Landed in `drivers/gpu/drm/m9x` as `m9x_mode_take_native()`.

Video reset is a global strobe rather than the P690's per-module one:

```
0x1c24 &= ~1
0x1900 |= 0xf0000 ; 0x1b00/0x1e00/0x1f00 |= 0x30000
0x1c10 bit31: 0 -> 1 -> 0
0x1c24 |= 1
all four T+0x00 &= ~0x30000
```

`0x1c28` is the **genlock / frame-sync crossbar**, not the video path: set it once at
init to `0x0F021084` and leave it.  Writing it per-modeset takes the output dark.
`[M9WIN 0x348a9, HW]`

Memory watermarks (`0x1c14`/`0x1c1c`/`0x1c18`/`0x1c20`, per controller):
`(old & 0xfffef000) | 0x20700 | ((ratio-1)<<4) | 7`, where the bandwidth ratio is
`round(bpp * pixclk_kHz / 125)`.  `[M9WIN 0x37729]`

## Outputs

DACs (`0x1c80` / `0x1c84`): pad trims from PInS `+0xA8` / `+0xAC` packed
`(b1&7)<<6 | (b0>>1)&0x38 | b0&7`; bit11/bit13 sync inversion; **bits[19:16] = source
controller, encoded 0/1/4/5**; bit24 = data-path disable, bits 10/12 = sync blank.

Digital links (`0x1c9c + 4*link`): **bits[4:0] = source controller, encoded
0x00 / 0x08 / 0x01 / 0x09** for controllers 0..3 (not 0/1/2/3); bit18 = data-path
disable; bit20 / bit22 sync inversion (set when the corresponding mode flag is
*clear*); bit25 = dual link, above 165 MHz.  Companion registers `0x1c38` (links 0,1)
and `0x1c3c` (links 2,3) hold one 16-bit half each, plus `0x1c70`, `0x1cac`, `0x1cb0`.

**Those companion registers carry PInS register-patch entries keyed by pixel clock.**
The values POST leaves are the entries for its own (low) clock and are wrong at any
other frequency.  They cannot be computed — PInS must be read.  `[M9WIN 0x36057]`

### The mini-DisplayPort outputs need an external transmitter

This is why a correct modeset still shows nothing on an M9148: the mini-DP connectors
are driven by an **external DisplayPort 1.1 transmitter on a bit-banged I²C bus**.
The `0x1c9c..0x1ca8` links are only the parallel pixel feed into that chip.  The
VBIOS trains it at POST; changing the DC timing without re-initialising and
re-training it drops the link, and **nothing writable in the GPU register file can
restore it**.  `[M9WIN, HW]`

* I²C slave addresses 0x72 / 0x76 (8-bit write form) in the saved binaries; **the
  M9148-E1024LAF answers at 0x10 / 0x12 / 0x14 instead** `[HW]`.  Each connector has
  its own bit-banged bus on `0x1c48`: clock bit `2n`, data bit `2n+1`, output enable
  `+16`.  `0x1c4c` is MISCIO — the four transmitter reset lines plus the board
  identification EEPROM, not a DDC bus.  `docs/parhelia-m9148-fourhead-outputs.md`.
* **An HMC microcontroller on the board arbitrates the bus.**  Take the mutex first
  by **clearing** (active low) `0x318` bit29 for the DDC register `0x1c48` or bit28
  for MISCIO `0x1c4c`, and poll `0x3b0` bit1 / bit0 respectively for the grant.
  (The earlier "bit28 DDC / bit29 MISCIO" reading here was swapped; corrected from
  the vendor trace, `docs/parhelia-m9148-fourhead-outputs.md`.)  `[HW]`
  Touching `0x1c4c` without it can hang the machine `[HW]` — it did, twice.
* Transmitter reset over GPIO on `0x1c4c` (data bits[7:0], output-enable bits[17:10]),
  low 10 ms then high 10 ms, pin from PInS GPIO ids `0x1c+head` / `0x20+head`.
* Link training is run **by the transmitter**, not in software: there are no TPS1/TPS2
  writes and no DPCD 0x100-0x106 accesses anywhere.  Software writes the rate
  (`0x5D`), lane count (`0x5E`), kicks `0x5F` bit0, then polls `0x76` bit1 and checks
  `0x8D` bits[6:4] == 0x40, verifying against DPCD 0x202/0x203 (`& 0x77 == 0x77`) and
  0x204 bit0.
* Rates are 1.62 and 2.70 Gbps only (DisplayPort 1.1).  Per-lane pixel-clock limits:
  1.62 → {45000, 85000, 125000, 165000} kHz; 2.70 → {85000, 165000, 248000, 340000}.
* **`reg 0x0A` bits[5:4]: 0x10 blanks, 0x30 enables — this is the write that makes the
  picture appear.**

The transmitter part number is not identifiable from the binaries (no ID read, no
probe list, no name string); the I²C addresses and the register fingerprint above are
what there is to match against a datasheet.

## PInS 8.0

The M9 requires PInS version ≥ 0x0800 and reads it from **SPI flash**, not the
ROM BAR.  Layout is the familiar one — near-pointer at flash 0x7ffc, signature
`0x412e` at that offset, length at +2, version at +4 — but **no checksum is computed**
`[M9WIN]`, unlike earlier generations.

| offset | contents |
|---|---|
| `+0x0c` | offset of the output-descriptor table |
| `+0x0e` / `+0x10` | start / end of the register-patch table |
| **`+0x44`** | **memory configuration** — see below |
| `+0x50` / `+0x54` | lane → signal map |
| **`+0x5c .. +0x68`** | **4 dwords, one per connector: the connector → link map** |
| `+0x6c` / `+0x70` | GPIO pin map (transmitter reset) |
| `+0xa8` / `+0xac` | DAC0 / DAC1 pad trims |
| `+0x120 .. +0x140` | per-connector DDC bus |

**Video memory size** is `+0x44`: four independent 4-bit bank codes, summed, with
`{0, 32M, 64M, 128M, 256M, 512M, 1G}`; `0xFFFFFFFF` means 512 MB.  It is *not* in any
MMIO register — in particular `0x0000` is the memory-controller remap window (it
happens to read `0x40000000` on a 1 GB board, which is a coincidence, not a size).
`[M9LIN 0x2f6464]`

Connector dword decode: byte0 → DAC index or link, byte1 → link when the connector
type is 2, byte2 → link when type is 1 or 3; codes `0x11..0x14` mean links 0..3.
Connector technology enum: 5 = external DisplayPort (the M9148's mini-DP), 6 = HDMI,
7 = DVI, 10 = HD15.

Register-patch entries are variable length:

```c
u16 reg; u8 flags; u8 clksel;
u32 data;      /* if flags & 1 */
u32 mask;      /* if flags & 2 */
u32 alt_data;  /* if flags & 4 */
/* stride = 4 + 4 * popcount(flags & 7); terminator = 0 or 0xffffffff   */
/* clksel 1 && pixclk > 100000, or clksel 2 && pixclk > 200000 -> alt_data */
/* apply: new = (old & ~mask) | data                                      */
```

## Corrections to earlier findings

Recorded because each of these looked authoritative and cost time:

* The M9 cursor is **not** the packed 64×64×4 / pitch-256 linear ARGB surface the
  Linux driver describes.  The chip *accepts* `CFG = 0xcc000000` and `CTL = 5` (the
  P690 masks them away), and colours come out **R/B swapped** versus an ARGB write,
  but no stride tried so far reproduces a correct image.  Static analysis now
  closes the saved driver's intended four-bank register, position, control,
  allocation, and upload contract in `parhelia-m9-cursor-gamma-re.md`; it does
  not supersede this hardware contradiction.  Unresolved; use the software
  cursor.  `[HW] [M9LIN]`
* `C+0x00` uses `fmt<<22` and `C+0x58` uses `fmt<<24` — a working VBIOS mode reads
  `0x33046001` and `0xcc210400` on a 32 bpp surface.  `[M9WIN]` claimed `<<23` and
  `<<25`; those even truncate the 8-bit code.  `[HW]`
* MMIO `0x0000` is not the VRAM size (see above).
* `0x1c28` is genlock, not the video crossbar.
* The VBE/int10 path is unusable on the M9: it never produced a picture and locked the
  card within seconds.  Native modesetting is stable.  `[HW]`
