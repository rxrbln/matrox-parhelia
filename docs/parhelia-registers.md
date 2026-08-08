# Matrox Parhelia — register reference

This is the consolidated, evidence-tagged register reference for the driver. It
merges two independent reverse-engineering efforts:

- **[VBIOS]** — static analysis of the Parhelia VGA/VBE BIOS
  ([`parhelia-vbios.md`](parhelia-vbios.md), reproducible with `tools/vbios.py`).
- **[DRV]** — disassembly of the proprietary Linux driver
  (`matroxdriver-x86_64-cvs-20090511`, v1.4.7, May 2009).
- **[SRC]** — the proprietary driver's own C source (`kernel/src/`), the
  strongest evidence where it exists.
- **[HW?]** — inferred, **not** verified on real hardware.

Where the two efforts disagreed, the driver disassembly wins (it is complete and
concrete; the VBIOS analysis was working blind). Corrections to the earlier
VBIOS-only conclusions are called out below.

## Chip and apertures

| | | Source |
|---|---|---|
| PCI vendor | `0x102b` | [SRC] |
| PCI device | `0x0528` = "Parhelia8x" (0x0527 = original Parhelia-512, 0x0529 = Phoenix) | [SRC] |
| BAR0 | linear framebuffer aperture (write-combined) | [SRC] `mtx_dev.c` |
| BAR1 | MMIO register aperture | [SRC] |
| BAR2 | MMIO registers *if BAR0 is a 64-bit BAR* (then BAR0 eats slot 1) | [SRC] |

> **Correction.** The VBIOS reaches MMIO through an indirect window in PCI config
> space (`cfg 0x44` index, `cfg 0x48` data) because 16-bit real mode cannot
> address the 32-bit aperture. The driver source names these **MGAINDEX (0x44)**
> and **MGADATA (0x48)**, and confirms they are only a convenience path — the
> driver maps BAR1 and addresses registers directly, which is what we do.

## PCI config space [SRC: `mtx_drv.h`]

| Offset | Name | Purpose |
|---|---|---|
| 0x10 | MGABASE2 / BAR0 | framebuffer base |
| 0x14 | MGABASE1 / BAR1 | MMIO base |
| 0x18 | MGABASE3 / BAR2 | alt MMIO base (64-bit BAR0 case) |
| 0x40 | **OPTION** | memory/clock control (see fields) |
| 0x44 | MGAINDEX | indexed-register index |
| 0x48 | MGADATA | indexed-register data |
| 0x50 / 0x54 | OPTION2 / OPTION3 | secondary options |

**OPTION (0x40) fields:** `SYSCLKDIS` b2, `SYSPLLPDN` b5, `PLLSEL` b6,
`VGAIOEN` b8, `MEMCONFIG` b10-12 (3-bit memory config), `RFHCNT` b15-20
(refresh count), `ENHMEMACC` b22. These are the coarse memory-controller knobs.

## Mode setting — VGA CRTC + CRTCEXT

**This is how the driver sets timing, and it resolves the blocker we could not
crack from the VBIOS alone.** The legacy VGA register file is mirrored into MMIO
at base **0x1C00** (VGA I/O port *P* → MMIO `0x1C00 + P`), so:

| MMIO | VGA equiv | Role | Source |
|---|---|---|---|
| 0x1FD4 / 0x1FD5 | 0x3D4 / 0x3D5 | CRTC index / data | [DRV] |
| 0x1FDE / 0x1FDF | 0x3DE / 0x3DF | **CRTCEXT index / data** | [DRV] |
| 0x1FC2 / 0x1FCC | 0x3C2 / 0x3CC | MISC write / read (clock select bits 3:2) | [DRV] |

> **Correction.** The VBIOS analysis guessed `0x1FDE` was a standalone 8-bit
> bitfield register. It is actually the **CRTCEXT index port** — the classic MGA
> index/data mechanism. Two independent driver disassemblies
> (`HSLCRTCSet` @ `0xeda60`, cross-checked in a second binary) show index-then-
> data programming at `0x1FDE`/`0x1FDF`. (The VBIOS drives legacy VGA through the
> real I/O ports and only pokes the `0x1FDE` index over MMIO, which is why our
> VBIOS trace never showed the paired `0x1FDF` data access and misled the
> earlier reading.)

The `HSLCRTCSet` converter fills VGA CRTC 0x00–0x18 the textbook way (htotal/8−5,
etc.), then writes the CRTCEXT sub-registers that hold the overflow bits:

| CRTCEXT idx | Field | Bits |
|---|---|---|
| **0x00** | display start + pitch high | `[3:0]`=start[19:16], `[5:4]`=pitch[9:8], `[6]`=start[20] |
| **0x01** | horizontal high | `[0]`=HTotal[8], `[1]`=HBlankStart[8], `[2]`=HSyncStart[8] |
| **0x02** | vertical high | VTotal[11:10], VDispEnd[10], VBlank/VSync high bits |
| 0x03 | clock select / MGA mode | — |
| 0x05 | horizontal counter / interlace | — |

CRTCEXT[0x01] is exactly the missing piece: 1600×1200 has htotal 2160, so
`2160/8−5 = 265 = 0x109` — CRTC[0x00] holds `0x09` and CRTCEXT[0x01] bit0 holds
the 9th bit. **Display start** = CRTC 0x0D/0x0C + CRTCEXT[0x00]; **pitch** =
CRTC 0x13 (128-byte units) + CRTCEXT[0x00]. [DRV: `HSLCRTCSetDisplayStart`
@0xee280, `HSLCRTCSetPitch` @0xee430]

### VGA parameter and timing tables [VBIOS]

The VBIOS carries the standard 64-byte IBM VGA parameter table (`0x420`) and
24-byte CRTC timing records (the `0x5827` table holds 800×600 / 1024×768 /
1280×1024 / 1600×1200, cross-validated against the pixel-clock constants). See
`parhelia-vbios.md`; `tools/vbios.py --vga --timings` prints them.

## Native display controllers (triple head)

Parhelia is triple-head. Beyond the legacy VGA CRTC, there are native Display
Controller (DC) blocks with **full 32-bit timing registers** (no bit-splitting):

- Bases **0x1800 / 0x1A00 / 0x1B00**, stride **0x200**. [DRV: `HSLPARSetDCParameters`,
  `HSLPARIsDCInUse` probes 0x1800 & 0x1A00; VBIOS head-select confirmed the first two]
- Per-DC 32-bit registers written by the driver (DC0; add head×0x200): 0x1800,
  0x1834, 0x1888, 0x1900, 0x1908, 0x190C, 0x1910, 0x1914, 0x1918, 0x1920, 0x1928,
  0x192C, 0x1930, 0x1934, 0x19A8, 0x19D8, 0x19E4. Field semantics only partly decoded.
- **Palette LUT** (per head): index at +0x38, then R/G/B at +0x3C/0x40/0x44,
  10-bit left-aligned, blue write latches (bit 31). [VBIOS, proven]

The classic VGA-CRTC path (primary head) is enough for all standard modes; the
DC blocks are the route to the additional heads and are the larger follow-on.

## PLL — pixel clock [VBIOS model, confirmed & corrected by DRV]

Four PLLs, **reg = 0x0E10 + index×4**. Reference **27 MHz**.

The pixel clock is a **four-field divider** (hardware-confirmed on 0x0528, all
standard modes locking cold):

```
Fout = 27 MHz * N * P / X / B3        (two truncating divides: /X then /B3)
  VCO1 = 27 MHz * N        in [600, 1200] MHz   (PLL0 to 1600)
  tap  = 27 MHz * N / X    in [50, 400] MHz
  VCO2 = 27 MHz * N * P / X in [600, 1200] MHz
  B3 = output post-divider, even
```

| PLL | Reg | VCO | Out |
|---|---|---|---|
| 0 | 0x0E10 | 600–**1600** MHz | 50–400 MHz |
| 1–3 | 0x0E14–0x1C | 600–1200 MHz | 50–400 MHz |

The DC head is driven by **PLL2 (0x0E18)** on the original Parhelia (not PLL0).

**Register encoding** [DRV encoder + VBIOS `SetPixelClock`@ROM 0x7018, hardware
diff] — four fields stored negated & masked:

```
N  -> reg bits  0-5   ((N-1) & 0x20) | ((-N) & 0x1f)
P  -> reg bits  6-10  ((-P)  & 0x1f) << 6
X  -> reg bits 11-15  ((-X)  & 0x1f) << 11
B3 -> reg bits 16-21  ((-B3) & 0x3f) << 16     <-- the field the 2-field model missed
bit 31 = programming strobe (LOAD)
bits 25-24 read back 0x03 = read-only PLL-active status (never written)
```

Golden words (reproduced by the solver): 108 MHz = N24/P3/X3/B3=6 = `0x033aef48`;
65 MHz = N23/P5/X3/B3=16 = `0x0330eec9`; 154 MHz single-link = N34/P2/X3/B3=4.
The DC-head path fixes **X=3** and searches N/P/B3. Implemented in
`parhelia_program_dc_clock` (`src/parhelia_mode.c`); the legacy VGA-CRTC encoder
`ParheliaPLLEncode` (`src/parhelia_pll.c`) still omits B3.

**Programming sequence** [DRV]: read-modify to set bit7 / clear bits 2,3,6 and
write back, delay ~10; write the encoded value with **bit 31 set**, delay ~100;
write again with **bit 31 clear**, delay ~200; read back to commit.

**On the 65 MHz rounding:** the four-field solver lands 0.18% high on 65 MHz
(N23/P5/X3/B3=16 → 64687), matching the proprietary driver's own residual —
hardware quantization, not a model gap. 108 and 162 MHz are exact; 154 MHz lands
0.65% low (153 MHz, ~59.6 Hz), accepted by the panel.

## Native DC-head mode set — hardware-confirmed (original Parhelia 0x0528)

The original Parhelia drives DVI through the **native DC head, not the VGA CRTC**
(the 8-bit CRTC can't express >1024-line timing). Cold-boot native modeset works
on DVI-1 across 800×600, 1024×768, 1280×1024, 1600×1200 and 1920×1200 (154 MHz
single-link). The full chain (`ParheliaSetModeNative`, `src/parhelia_mode.c`):

1. **Pixel PLL** — solve+program the four-field divider into **PLL2 (0x0E18)**
   with the LOAD strobe (see PLL section). B3 is mandatory or PLL2 never locks.
2. **Clock routing** — controller B (**0x1C04**): mux=PLL, clksel=3; then
   **0x1C08 = 2** selects controller B as the DC-head clock source (required —
   the head clock does not engage otherwise).
3. **DC-head timing** — program the timing twin blocks (base 0x1A00 ctrl /
   0x1B00 timing; head×0x200). The active display width sets the memory-fetch
   **stride**, so the framebuffer pitch must equal **HDisplay** (aligned to 16) —
   *not* padded to a power of two, which leaves the padding columns visible as
   vertical bars. (`0x1A34`'s low field, set to displayWidth, is *not* the real
   fetch stride; it only appeared to work when displayWidth == HDisplay.)
4. **Integrated DVO/TMDS serializer** (`parhelia_program_dvo`) — the VBIOS only
   sets this up during POST, so a native mode must bring it up:
   - **0x1C20** format+crossbar: byte1 = DVO1, byte3 = DVO2; value
     `0x14 | (head<<6)` routes a head into the serializer (DVI-1 = head1→DVO1 →
     byte1 = 0x54). Byte0 bit5 (0x20) is a POST-set global enable (read-only from
     software).
   - **0x1C24** serializer power, active-low: bit0=DVO1, bit1=DVO2, bit2=common;
     DVI-1 up = 0x02.
   - Serializer clock-source code in the controller reg: DVO1 =
     0x1C00 {bit27, bits14:13}, DVO2 = 0x1C04 bits[17:15] = the clock index (3),
     with a hold bit (0x1C00 bit12 / 0x1C04 bit14) asserted across the reprogram.

Mapping (0x0528): **DVI-1 = head1 = DVO1** (0x1C00 / 0x1C20 byte1 / 0x1C24 bit0);
DVI-2 = head0 = DVO2 (0x1C04 / byte3 / bit1). `0x1A00` bit18 (0x40000) is a
read-only "CRTC active" status; the live vertical scanline counter is `0x1B24`.
[RE: `HSLPAREnableDVO`@0xb04f0, `HSLPARSetDVOClock`@0xaef00, all cross-checked
against a working VBE register dump; validated on hardware.]

## Memory controller — position corrected

> **Correction.** An earlier revision of this document listed a "memory
> controller at 0x12BC–0x1324" based on a `{offset,value}` init list in the
> proprietary driver (`mtx1641`) whose writes matched the VBIOS. The
> acceleration pass showed that list is the **2D-engine command buffer** (its
> `count` field at struct `+0x808` is the DPS command-buffer index), and those
> offsets are **drawing-engine registers addressed by index**, not memory
> registers: `0x1300` = DWGCTL, `0x12C0 ← 0xFFFFFFFF` = the plane-write mask
> (PLNWT), `0x12BC` = DWGSYNC, `0x1220`/`0x1224` = surface descriptors. See
> [`parhelia-accel.md`](parhelia-accel.md).

Actual memory setup is via **PCI OPTION (0x40)** — the `MEMCONFIG` (b10-12) and
`RFHCNT` (b15-20) fields — and the VBIOS power-on block at MMIO **0x0F00–0x0F5C**
(written by the init chain, contents not decoded). **0x1E44** is a memory
arbitration / priority register [DRV: `HSLCRTCSetPriorityRequest`]. None of this
is needed if the VBIOS POST is allowed to run.

## 2D / 3D acceleration engine

The drawing engine, texture units, pixel shader, and DPS video scaler all live
in one **indexed register file** reached both by direct MMIO and by a DMA
pushbuffer (register index = `(offset − 0x1000) >> 2`). DWGCTL is at **0x1300**.
This is a large subsystem in its own right — see
[`parhelia-accel.md`](parhelia-accel.md) for the full 2D/3D/DPS/cursor/Xv
reference. (It is *not* at 0x2000, and 0x1C00 is the VGA mirror, not the engine —
both earlier guesses were wrong.)

## Driver structure notes (for further digging)

The proprietary driver's register-level logic lives in the X-server binary
`xserver/7_4_99/mtx_drv.so`, whose HAL/HSL function names survive as debug
strings. The kernel object `kernel/mtx_parhelia.o` is name-obfuscated
(`mtxNNNN`) OS glue with no register map. Highest-value functions for future
work: `HSLPARSetMode`→`HSLCRTCSet` (timing), `HSLDACComputePLLParam`/
`HSLDACPIXPLLSET` (PLL), `HSLS90InitMemClk`/`HSLPARInitializeHardware` (memory
bring-up). TV/DVI/LVDS output encoders (Maven, Chrontel, CVE2, SI164) are a
whole further subsystem, irrelevant to basic modeset.

## What is still open

1. **CRTCEXT[0x02] vertical bit layout** — we have the horizontal bits (index 1)
   precisely; the vertical high bits (index 2) are known in outline but not to
   the individual bit. Easy to finish from `HSLCRTCSet`.
2. **Native DC block field semantics** (0x1800/0x1A00/0x1B00) — needed for the
   2nd/3rd heads, not for the primary.
3. On-hardware verification of everything: dump BAR1 after a VBIOS-set 640×480
   and after 1600×1200 and diff, then compare our register writes against it.
