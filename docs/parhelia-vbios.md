# Matrox Parhelia — mode setting, reverse engineered from the VBIOS

> **Read [`parhelia-registers.md`](parhelia-registers.md) first.** It is the
> authoritative, consolidated reference, cross-checked against the proprietary
> Linux driver. This document is the original VBIOS-only analysis, kept for its
> derivations and for `tools/vbios.py`. Three of its inferences were later
> **corrected** by the driver disassembly — each is flagged inline below:
> (1) `0x1FDE` is the CRTCEXT *index* port, not a standalone bitfield register;
> (2) the PLL divider word's feedback value is the top byte and the post-divider
> is the middle byte (the frequency relation was right); (3) mode timing goes
> through the VGA CRTC + CRTCEXT, which we suspected and is now confirmed.

Everything here was recovered statically from `vbios-matrox-parhelia` (43008
bytes, "MATROX/PARHELIA VGA/VBE BIOS (V1.6) b34", 05/14/04). Nothing has been
tested against hardware yet — claims are marked **proven** (follows directly
from the disassembly) or **inferred** (consistent with the access pattern, not
yet demonstrated).

Reproduce any of this with `tools/vbios.py` (`--regs`, `--pll`, `--vga`,
`--timings`).

## The chip

| | |
|---|---|
| PCI ID | `0x102b:0x0528` (Parhelia-512), class `0x030000` |
| BAR0 | linear framebuffer aperture |
| BAR1 | MMIO register aperture |
| ROM init entry | `0x80`; checks PCI ID `0x0528102b` before doing anything |

BAR roles are **proven**: the VBE GetModeInfo handler (`0x5d3c`) reads BAR0 via
PCI config 0x10 and stores it into the ModeInfoBlock as `PhysBasePtr`.

## How the ROM reaches registers — and why the driver doesn't care

The ROM never enters unreal mode (there is no `lgdt` and no write to `cr0`
anywhere in the image), so 16-bit code cannot address the 32-bit MMIO aperture.
Matrox works around this with an **indirect register window in PCI config
space**:

```
PCI cfg 0x44           <- MMIO register offset (16-bit)
PCI cfg 0x48+(off&3)  <-> register data     (low bits pick the byte lane)
```

Six helpers wrap it, all taking the MMIO offset in `SI` and data in `CL`/`CX`/`ECX`:

| Addr | Op | | Addr | Op |
|---|---|---|---|---|
| `0x6c9b` | write 8 | | `0x6d0d` | read 8 |
| `0x6cc1` | write 16 | | `0x6d31` | read 16 |
| `0x6ce7` | write 32 | | `0x6d55` | read 32 |

Underneath, `0x6bdb` / `0x6c1c` are plain PCI config read/write via 0xCF8/0xCFC.

**Two consequences.** First, the window is only a real-mode workaround — the
driver maps BAR1 and uses the same offsets directly. Second, because the offset
always arrives as a `mov si, imm16`, *every register access in the ROM is
statically recoverable*; `tools/vbios.py --regs` extracts 236 of them.

The window is also useful in its own right: it works from userspace with **no
driver loaded**, so registers can be dumped after the VBIOS sets a mode. That is
how we intend to validate our own mode sets (see "Bring-up" below).

## Register map

Accesses recovered from the ROM. Offsets are BAR1-relative.

| Block | What | Confidence |
|---|---|---|
| `0x0000–0x000c` | chip ID / status | inferred |
| `0x0e10`,`0x0e14`,`0x0e18`,`0x0e1c` | **the four PLLs** | proven |
| `0x0f00–0x0f5c` | power-on init block, written by `0x837f` (called from the init chain at `0xba`) — not touched per-mode | proven (as init), contents inferred |
| `0x1220–0x1324` | memory controller (`0x12c0←0xffffffff`, `0x12c4←0x7fffffff`, `0x12f4←0x41f80000`) | inferred |
| `0x1800–0x19ff` | **display head A** | proven |
| `0x1a00–0x1bff` | **display head B** (same block, stride `0x200`) | proven |
| `0x1c00–0x1c70` | drawing engine (MGA-style) | inferred |
| `0x1fde` | an 8-bit *bitfield* control register — **not** a CRTCEXT index | proven (that it is a bitfield) |

The PLL registers do not appear in the `--regs` trace: they are reached with a
computed `SI` loaded from the table at ROM `0x6d7c`, not an immediate.

> **A correction worth recording.** On the MGA line, MMIO `0x1fde` is the
> CRTCEXT index (the VGA block mirrored at `0x1fc0`), and it is tempting to
> assume Parhelia inherits that. It does not appear to. At `0x60ac` the VBIOS
> *ORs bit flags together* (`|= 1`, `|= 4`, `|= 8`) and writes the result to
> `0x1fde` as one byte. An index register is not built that way — this is a
> bitfield. Do not carry the MGA assumption into the driver.

### Two display heads

**Proven** at code `0x93cd`/`0x93d2`, which select between `0x1800` and `0x1a00`
for otherwise identical code. Offsets below are relative to a head base.

| Off | Register | Confidence |
|---|---|---|
| `+0x000`, `+0x001` | enable / control bits (ORed with 1, 2) | inferred |
| `+0x038` | **palette LUT index** (write 8) | proven |
| `+0x03c` / `+0x040` / `+0x044` | **palette LUT data R / G / B** | proven |
| `+0x1aa` | LUT access gate (`0x08` before, `0x00` after) | proven |

The LUT is **proven** by the loader at `0x970a`: it reads VGA port `0x3c8`/`0x3c9`
256 times and, for each entry, writes each 8-bit component **shifted left by 10**
into `+0x3c`, `+0x40`, `+0x44`, setting **bit 31 on the third write** as a latch.
So the LUT is **10 bits per component**, left-aligned in a 32-bit field, and the
blue write commits the entry. A second loader at `0x9400` takes the shift from a
register (supporting 6-bit VGA palette values, shifted by 12) and swaps the
component order on a flag — so R/G/B ordering is selectable.

## The PLL — proven

`0x7018` is `SetPixelClock(EAX = target kHz, SI = PLL register offset)`.

- **Reference clock 27 MHz** (`0x6978`, loaded six times by the divider search).
- Four PLLs; their MMIO offsets come from a table at ROM `0x6d7c`:
  `0x0e10`, `0x0e14`, `0x0e18`, `0x0e1c`.
- Each PLL has a **20-byte record** at `0x7800 + {0x28,0x3c,0x50,0x64}` (index
  bytes at ROM `0x6d84`): a dword default divider word, then six words of limits
  in MHz.

| PLL | Reg | VCO | Output |
|---|---|---|---|
| 0 | `0x0e10` | 600–1200 MHz | 50–400 MHz |
| 1 | `0x0e14` | 600–**1600** MHz | 50–400 MHz |
| 2 | `0x0e18` | 600–1200 MHz | 50–400 MHz |
| 3 | `0x0e1c` | 600–1200 MHz | 50–400 MHz |

The default divider words are zero in the ROM image; the init routine at
`0x6d8c` skips a PLL whose default is 0, so they are patched at runtime from
straps.

**Divider word format.** The search packs its result one byte per field:

```
value = (P << 16) | (X << 8) | N
Fvco  = N × 27 MHz      must land in the VCO range
Fout  = Fvco / P        must land in the 50–400 MHz output range
```

Both relations are **proven** — they are exactly the range checks the search
loop performs on its own candidate before accepting it (`0x71b4`–`0x71f0`).
Minimum divider is 3 (the code clamps).

**Byte 1 (`X`) is not yet pinned down.** Reimplementing the search with just N
and P (`tools/vbios.py --pll`) reproduces the ROM's own clock constants:

| Target | Result |
|---|---|
| 108000 kHz (1280×1024@60) | N=24 P=6 → **exact** |
| 162000 kHz (1600×1200@60) | N=24 P=4 → **exact** |
| 65000 kHz (1024×768@60) | N=41 P=17 → 65117 kHz (+0.18%) |

Two of three land exactly, which is good evidence the N/P semantics are right.
The 65 MHz miss is very likely what `X` exists to fix (a pre-divider would give
the extra granularity). Worth resolving before trusting the PLL on hardware,
though +0.18% is within monitor tolerance.

**Programming sequence** (`0x6e63`–`0x6e8c`): write the divider word with **bit
31 set**, delay, then rewrite with **bit 31 clear**. A strobe, not a plain store.

## Mode setting goes through the standard VGA CRTC

This is the headline finding, and it makes the driver far simpler than expected:
**Parhelia sets mode timing through the standard VGA CRTC**, driven over the
legacy I/O ports (`0x3c4` sequencer, `0x3ce` graphics, `0x3d4` CRTC, `0x3c8`/
`0x3c9` palette). The per-head `0x1800`/`0x1a00` blocks are the *output* side
(LUT, enables), not timing.

**Clock selection is classic VGA too:** the routine at `0x94c4` reads MISC
(`0x3cc`), masks with `0xf3`, ORs in `sel << 2` and writes MISC (`0x3c2`) — i.e.
the four PLLs hang off the VGA MISC clock-select field (bits 3:2). It is called
with `sel` = 1 and 2.

### The timing → CRTC converter (`0x883a`) — proven

Takes `SI` = a timing record, `ES:DI` = a 64-byte VGA parameter block, and fills
in the CRTC bytes. It is textbook VGA:

```
CRTC[0]  = htotal/8 - 5          CRTC[1]  = hdisp/8  - 1
CRTC[2]  = hblank_start/8 - 1    CRTC[3]  = (hblank_end/8 - 1) & 0x1f | 0x80
CRTC[5]  = hsync_end/8 & 0x1f, bit7 = hblank_end bit5
CRTC[6]  = vtotal - 2            CRTC[18] = vdisp - 1
CRTC[21] = vblank_start - 1      CRTC[16] = vsync_start - 1
```

with the overflow bits scattered into `CRTC[7]` (bits 0,1,2,3,5,6,7) and
`CRTC[9]` bit 5, exactly per the VGA spec.

**The 10-bit escape hatch:** if `vtotal >= 0x3ff`, the converter halves every
vertical value and sets `CRTC[23] |= 4` (count-by-two). That is how 1280×1024
(vtotal 1066) fits VGA's 10-bit vertical fields. The driver must do the same.

### Timing record — 24 bytes, 12 words

```
+0x00 htotal        +0x0c vtotal
+0x02 hdisp         +0x0e vdisp
+0x04 hblank_start  +0x10 vblank_start
+0x06 hblank_end    +0x12 vblank_end
+0x08 hsync_start   +0x14 vsync_start
+0x0a hsync_end     +0x16 vsync_end
```

### Timing tables

`0x87bb` selects a table from bits in `DL` (display/panel type). The important
one is **`0x5827`**, the high-res VESA set — and it cross-validates exactly
against the pixel-clock constants found independently in the code:

| Timing (h: disp/ss/se/total, v: same) | Clock in ROM |
|---|---|
| 800×600 — 800/840/968/1056, 600/601/605/628 | 40 MHz |
| 1024×768 — 1024/1048/1184/1344, 768/771/777/806 | `0xfde8` = **65 MHz** |
| 1280×1024 — 1280/1328/1440/1688, 1024/1025/1028/1066 | `0x1a5e0` = **108 MHz** |
| 1600×1200 — 1600/1664/1856/2160, 1200/1201/1204/1250 | `0x278d0` = **162 MHz** |

Two structures recovered by completely different routes agreeing to the digit is
strong evidence both are read correctly.

Other tables: `0x8b5a` (VGA modes, 640×480 raster), `0x8c68` and `0x8d17` (the
same VGA modes rastered onto 1280×1024 and 1600×1200 — the flat-panel scaling
path), `0x57b7` (640×400/640×480).

### VGA mode parameter table — `0x420`

24 classic 64-byte IBM records:

```
+0x00 cols  +0x01 rows-1  +0x02 char height  +0x03 page size (word)
+0x05 SEQ[1..4]  +0x09 MISC  +0x0a CRTC[0..24]  +0x23 ATTR[0..19]  +0x37 GFX[0..8]
```

Verified field-by-field against the 80- and 40-column records: `CRTC[19]`
(offset) is `0x28` / `0x14` as it must be, the text ATTR palette is the standard
`00 01 … 07 10 11 … 17`, and `GFX[8] == 0xff` in every record.

### VBE

VBE 3.0. Mode list at `0x56dc`, 0xFFFF-terminated, 21 modes:

```
101 110 111 112   102 103 113 114 115   105 116 117 118
107 119 11a 11b   11c 11d 11e 11f
```

640×480, 800×600, 1024×768, 1280×1024 at 8/15/16/24 bpp, plus `11c`–`11f`
(1600×1200). A second short list at `0x56ce`: `101 100 103 105 107 11c`.

## What is still open

1. **The extended CRTC register — the one real blocker.** It must exist and we
   have not found it. Proof that it exists: the ROM's own 1600×1200 timing has
   `htotal = 2160`, so `CRTC[0] = 2160/8 − 5 = 265`, which does not fit in eight
   bits. Something carries the 9th horizontal bit, and likewise the high bits of
   the start address, the pitch, and the pixel format. It never appears in the
   static trace, so the VBIOS must reach it through a computed offset rather
   than a `mov si, imm16` — which is exactly the case our extractor is blind to.

   Best way to find it, now that we have hardware: dump the whole BAR1 aperture
   through the PCI-config window after the VBIOS sets 640×480, dump it again
   after 1600×1200, and diff. The registers that move are the answer. This
   sidesteps the disassembly entirely.

2. **Byte 1 (`X`) of the PLL divider word** — the one loose end in an otherwise
   proven PLL. Likely a pre-divider; would explain the 65 MHz rounding.
3. **What `0x1fde`'s bits mean.**
4. **`0x1c00` drawing engine** — irrelevant until acceleration.
5. Contents of the `0x0f00` init block and the memory controller: needed to
   bring a card up from cold, but *not* needed if we let the VBIOS POST run.

## Bring-up plan

1. **PreInit only** — match `0x102b:0x0528`, map BAR0/BAR1, size VRAM; let
   `int10` set the mode. A working driver with no mode-setting risk, and a
   known-good baseline.
2. **Native mode set** — PLL, then VGA CRTC + CRTCEXT, then start address /
   pitch / depth. Validate by dumping registers through the PCI config window
   after the VBIOS sets a mode, then after ours, and diffing. That turns
   modesetting from guesswork into a checkable equality.
3. **Acceleration** — `shadowfb` first, EXA later.

Keep a serial console or SSH session open: a bad mode set blanks the display.
