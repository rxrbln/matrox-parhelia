# Matrox Parhelia — complete dual-head / dual-output reference

Everything the open-source driver needs to run **two independent displays**
(clone *and* extended desktop) on the two DVI-I / HD15 connectors. This
consolidates the reverse-engineering of the second head, the head→output
crossbar, the per-head clocking, and the independent DC reset, filling the gaps
left open in `parhelia-registers.md` ("Native DC block field semantics … needed
for the 2nd/3rd heads").

Evidence tags: **[CONFIRMED]** = proven on hardware or read straight from the
proprietary driver's own hardware-tested register writes / our HW-tested source;
**[PROBABLE]** = read from the HAL disassembly, self-consistent, but the
*simultaneous two-head* case is not yet HW-verified. Anchors are HAL functions in
`matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` (symbols survive) and
the driver source in `src/parhelia_mode.c` / `src/parhelia_randr.c`.

Family note: "original" = Parhelia-512/8x (0x0527/0x0528); "LX/P690" =
0x2537–0x2539; M9 (0x0540) has its own 4-controller block (`PARHELIA_M9_DC`) that
mirrors this model with different offsets — see `parhelia-m9.md`.

---

## 0. The two heads at a glance

Parhelia has **exactly two usable display heads** (the "triple-head" third block
0x1b00 is head 1's *timing* twin, not a third scanout — see
`parhelia-multihead-sync`). Each head is a full Display Controller (DC) with
32-bit timing, its own scanout base, its own palette LUT, and its own clock mux.

| | Head 0 | Head 1 | Source |
|---|---|---|---|
| DC control/size block | **0x1800** | **0x1A00** | [CONFIRMED] HSLPARIsDCInUse probes 0x1800 & 0x1a00 |
| DC timing block | **0x1900** | **0x1B00** | [CONFIRMED] |
| Clock-mux controller | **0x1C00** (ctrl A) | **0x1C04** (ctrl B) | [CONFIRMED] |
| Live vertical scanline counter | 0x1924 | **0x1B24** | [CONFIRMED] |
| Analog RAMDAC (default board wiring) | DAC2 0x1C1C | DAC1 0x1C18 | [CONFIRMED] |
| Integrated DVO / TMDS (default wiring) | DVO2 | DVO1 | [CONFIRMED] |
| Default connector on tested 0x0528 | DVI-2 | DVI-1 | [CONFIRMED] |

**Head-to-head register delta = 0x200.** Every DC register named below exists at
both heads; `head1_addr = head0_addr + 0x200`. Programmatically:
`PARHELIA_DC(head) = 0x1800 + head*0x200`, `PARHELIA_DCT(head) = 0x1900 +
head*0x200`.

---

## 1. Per-head Display-Controller register map

Two blocks per head. Offsets are **relative to the block base**; add `head*0x200`
for head 1. All 32-bit, no bit-splitting (unlike the legacy VGA CRTC).
[CONFIRMED — cross-validated against VBE golden captures; implemented in
`parhelia_program_dc_head`, HW-tested on DVI-1 and DVI-2.]

### 1a. Control / size / scanout block — base 0x1800 (h0) / 0x1A00 (h1)

| Rel off | h0 abs | h1 abs | Name | Meaning |
|---|---|---|---|---|
| 0x000 | 0x1800 | 0x1A00 | DC_CTRL | bit0 = head enable; enabled value `0x00040201` (progressive RGB) |
| 0x004 | 0x1804 | 0x1A04 | DC_XPAN | horizontal pan, `pan_x << 16` (0 = unpanned) |
| 0x008 | 0x1808 | 0x1A08 | DC_HDISP | `(hdisp-1) << 16` — fetch width |
| 0x024 | 0x1824 | 0x1A24 | **DC_START** | framebuffer byte offset, mask `0x0FFFFC00` (1 KB aligned) |
| 0x034 | 0x1834 | 0x1A34 | DC_FORMAT | `(fmt<<24) | 0x00030000 | (stride & 0x3FFF)` — pixel format + **fetch stride** |
| 0x038 | 0x1838 | 0x1A38 | DC_LUT_INDEX | palette index (per head) |
| 0x03C/40/44 | … | … | DC_LUT_R/G/B | 10-bit left-aligned; blue write latches |
| 0x048 | 0x1848 | 0x1A48 | DC_YPAN | vertical pan, `pan_y` (0 = unpanned) |
| 0x04C | 0x184C | 0x1A4C | DC_VDISP | `vdisp-1` |
| 0x088 | 0x1888 | 0x1A88 | DC_PARAMS | bits5:4 video-output enable, bit7|bit1 dim, cursor composite; **bit4 = cursor→head bind** |

`fmt` byte: 0x32 = 32bpp, 0x28 = 16bpp. Full 32bpp word = `0x32030400 | stride`.

### 1b. Timing block — base 0x1900 (h0) / 0x1B00 (h1)

H fields carry a −2 bias, V fields −1. Both word-swapped "twins" are required.
[CONFIRMED]

| Rel off | h0 abs | h1 abs | Name | Encoding |
|---|---|---|---|---|
| 0x000 | 0x1900 | 0x1B00 | DCT_MISC | interlace flags (0 = progressive); also holds the **reset pulse** bits17:16 |
| 0x008 | 0x1908 | 0x1B08 | DCT_HTIMING | `(htotal-2)<<16 | (hdisp-2)` |
| 0x010 | 0x1910 | 0x1B10 | DCT_HTIMING2 | `(hdisp-2)<<16 | (htotal-2)` (swap twin) |
| 0x014 | 0x1914 | 0x1B14 | DCT_HSYNC | `(hsend-2)<<16 | (hsstart-2)` |
| 0x018 | 0x1918 | 0x1B18 | DCT_VTOTAL | `(vtotal-1) & 0xFFF` |
| 0x020 | 0x1920 | 0x1B20 | DCT_VTIMING | `(vtotal-1)<<16 | (vdisp-1)` |
| 0x024 | 0x1924 | 0x1B24 | (read-only) | live vertical scanline counter |
| 0x028 | 0x1928 | 0x1B28 | DCT_VBLANK2 | 0 for progressive |
| 0x02C | 0x192C | 0x1B2C | DCT_VTIMING2 | `(vdisp-1)<<16 | (vtotal-1)` (swap twin) |
| 0x030 | 0x1930 | 0x1B30 | DCT_VSYNC | `(vsend-1)<<16 | (vsstart-1)` |
| 0x034 | 0x1934 | 0x1B34 | DCT_FIELD | 0 for progressive |

Reset pulse = `DCT_MISC | 0x00030000`, then clear.

---

## 2. Per-head scanout start / panning (extended desktop)

For an extended desktop both heads scan the **same physical framebuffer** but
from different offsets, so each head shows a different region.
[CONFIRMED registers; simultaneous two-region use is PROBABLE — only one head at
a time exercised on HW so far.]

- **Base offset**: `DC_START` (0x1824 / 0x1A24), byte offset into VRAM, 1 KB
  aligned (mask `0x0FFFFC00`). Head 0 → region A's byte offset; head 1 → region
  B's byte offset. Independent per head.
- **Stride**: low 14 bits of `DC_FORMAT` (0x1834 / 0x1A34). **Must equal the
  virtual scanline pitch** (`pScrn->displayWidth`, aligned to 16), *not* the mode
  width and *not* a power-of-two pad — mismatched stride shears/adds bars
  (`randr-native-displaywidth-pow2-stride` finding). For a side-by-side extended
  layout both heads share the same wide stride and differ only in `DC_START`
  (head 1 START = `head0_width_bytes`).
- **Fine pan**: `DC_XPAN` (0x1804/0x1A04, `pan_x<<16`) and `DC_YPAN`
  (0x1848/0x1A48). Sub-region panning without touching `DC_START`.

So the extended-desktop recipe is: one large `displayWidth`-strided framebuffer,
head 0 `DC_START = 0`, head 1 `DC_START = left_width*bpp/8` (1 KB aligned),
identical stride, independent timing/clock per §3.

---

## 3. Clocking — the single-PLL question, answered

**There is no hard single-PLL constraint. Two heads can run independent pixel
clocks, so extended desktop with two *different* resolutions is possible in
hardware.** [PROBABLE — proven from the routing logic and each head working
independently; the two-different-clocks-at-once case is not yet HW-tested.]

### Why

Each display controller has its **own** clock-select field, `bits[9:6]` (a 4-bit
`clksel`) in its clock-mux register: controller A = 0x1C00 (head 0), controller B
= 0x1C04 (head 1). `HSLPARSelectDisplayControllerClock` (mtx1890 @0xAB100)
programs the clksel of exactly the one controller belonging to the selected head
(`SelectDCModule` @0xBA780 picks 0x1C00 vs 0x1C04), holding bit5 across the
change and comparing the current `bits[9:6]` before rewriting. The two fields are
fully independent. [CONFIRMED from disasm]

### clksel → PLL mapping

`clksel = PLL_index + 1` ("DCM clksel = FS+1", HAL comment + our HW test).

| Silicon | PLL bank | Pixel-usable PLLs | Example routing |
|---|---|---|---|
| Original 0x0527/0x0528 | 0x0E10 + i*4 (i=0..3) | PLL1/PLL2/PLL3 (PLL0 = VGA CRTC pixel, only one reaching 1600 MHz) | head0 clksel 3 → PLL2 (0x0E18); head1 clksel 4 → PLL3 (0x0E1C) |
| LX/P690 0x2539 | FS bank 0x0E30 + i*4 | FS1/FS2/FS3 (FS0/0x0E30 = system clock — writing it reboots the host) | head0 clksel 3 → FS2 (0x0E38); head1 clksel 4 → FS3 (0x0E3C) |

The mux mode must also be "PLL" (`bits[1:0] = 2`) and a programming hold (bit5)
asserted across the reprogram, then released. [CONFIRMED — `parhelia_route_dcm`,
HW-tested]

### Current driver limitation (the gap to close for extended)

- **Original family**: `parhelia_program_dc_clock` always programs **PLL2 for
  both heads** and routes each head's controller to clksel 3. This gives
  *identical* clocks only → clone / single-head. For independent extended,
  assign a *second* PLL to head 0 (e.g. head 0 → PLL1/clksel 2, head 1 →
  PLL2/clksel 3), programming each PLL and routing only that head's own
  controller.
- **P690**: `parhelia_program_pll` already assigns FS2(head0)/FS3(head1), **but**
  when `dcHead==1` it routes *both* controller A and B to clksel 4 (a
  single-head convenience). For simultaneous extended this must route **only
  controller B to clksel 4** and leave controller A on clksel 3 (FS2) for head 0.
  (`parhelia_mode.c:179-184`.)

Bottom line for a full implementation: keep a per-head PLL assignment table
(head 0 ↔ PLL_a, head 1 ↔ PLL_b with a≠b), program both PLLs, and route each
controller to its own clksel. Clone can share one PLL; extended needs two.

---

## 4. Head → output crossbar — complete matrix

Every output carries a **head-select** control, so any head can drive any output
(subject only to the board's DVO-instance ↔ connector wiring, VBIOS
`PInS[0x1a1]`). The output's *serializer/DAC clock* must additionally be routed
in the controller reg of the head it is bound to. [CONFIRMED — all four routes
implemented and individually HW-tested; the arbitrary cross-routing is PROBABLE.]

| Output | Register | Head-select bit | Encoding | Aux clock |
|---|---|---|---|---|
| **DAC1** (analog, primary HD15) | 0x1C18 | **bit14** | 0 = head0 (ctrl A), 1 = head1 (ctrl B) | shares head's DC clock |
| **DAC2** (analog, secondary HD15) | 0x1C1C | **bit14** | same encoding | shares head's DC clock |
| **DVO1** original (0x0527/8) | 0x1C20 **byte1** (bits15:8) | byte1 bit6 = **bit14** | `0x14 | (head<<6)`; e.g. head1→byte1 `0x54` | 0x1C00 {bit27, bits14:13} |
| **DVO2** original | 0x1C20 **byte3** (bits31:24) | byte3 bit6 = **bit30** | `0x14 | (head<<6)` | 0x1C04 bits[17:15] |
| **DVO1** LX/P690 | 0x1C54 | **bit30** | bit30 = ctrl B (head1); +bit27/29 H/V sync inv, bit25 dual-link >165 MHz | 0x1C04 bits[17:15] = clksel |
| **DVO2** LX/P690 | 0x1C58 | (twin, bit30) | as 0x1C54 | 0x1C00 bits[14:13] = clksel |

Shared/global bits: DVO power 0x1C24 (active-low: bit0 = DVO1, bit1 = DVO2, bit2
= common); DVO data payload 0x1C70 (all variants); global DVO-control enable
0x1C20 byte0 bit5 (0x20). Sync polarity for the DACs: 0x1C18/0x1C1C bit11 =
HSYNC−, bit13 = VSYNC−.

**Default board map (tested 0x0528):** DVI-1 = head1 = DVO1 = DAC1; DVI-2 = head0
= DVO2 = DAC2. This DVO-instance↔connector binding is **not universal** — the
portable source is `PInS[0x1a1]` via `HSLPARGetDVOIndex`/`HSLPARGetDACIndex`; enum
values 0xa/0x10 swap the DVO index and 3 inverts it. Read the wiring byte for a
general driver.

---

## 5. Clone vs extended — the programming difference

The clone/extended distinction is a **software policy** over the same registers;
there is no single "clone bit". [PROBABLE — the vendor Linux stack drives this
from the X layer; no clone-specific HAL entry point exists in the symbols.]

### Extended desktop (each output = its own head)

1. Both heads enabled: `DC_CTRL` bit0 = 1 at 0x1800 **and** 0x1A00.
2. Each head programmed with its own timing (§1b), its own `DC_START` region and
   shared wide stride (§2).
3. **Two distinct PLLs**, one per head, each controller routed to its own clksel
   (§3).
4. Crossbar: output for head 0 head-select = 0, output for head 1 head-select = 1
   (§4). Each output's aux clock routed to its own controller.
5. Independent vid-reset per head (§7).

### Clone (both outputs = same image)

Two ways, both valid:

- **(a) Crossbar clone (preferred, one PLL, one scanout).** Enable head 0 only
  (`DC_CTRL` 0x1A00 bit0 = 0, head 1 disabled). Point **both** outputs'
  head-select at head 0: DAC/ DVO head-select bit = 0 for the primary output and
  0 for the secondary output too. One PLL, one `DC_START`, identical picture on
  both connectors. Register diff vs extended: second head disabled + second
  output's head-select forced to 0 + single clksel.
- **(b) Twin-head clone (two heads, identical state).** Both heads enabled with
  *identical* timing, identical `DC_START` (both = 0), same pixel clock (share one
  PLL via same clksel, or two PLLs at the same frequency). Each output on its own
  head. Use only when the two panels need independent sync generators but the
  same content.

Net: **extended** = distinct `DC_START` + distinct PLL + distinct head-select per
output; **clone(a)** = one head, one PLL, both head-selects → head 0, other head
disabled.

---

## 6. Second RAMDAC / second TMDS — full setup

Both are symmetric with the primary; the "second" output is just the other
head-select value plus its own power-bit set. [CONFIRMED — DAC2 and DVO2 paths
implemented and HW-tested via head 0.]

**Second analog DAC (DAC2, 0x1C1C):**
- Route: bit14 (0 = head0/ctrl A, 1 = head1/ctrl B).
- Power (active-low): `PARHELIA_DAC2_PWR = 0x00031400` (bits 10,12,16,17). (DAC1
  power set differs: `0x00019400`, bits 10,12,15,16.)
- Sync: bit11 HSYNC−, bit13 VSYNC−.
- **Load-sense** on DAC2: `ParheliaDACLoadSense(pScrn, sel=1)`; test/mode
  registers are per-head at `sel*0x200`, colour-line mask `bits[5:3]` for DAC2
  (DAC1 uses `bits[2:0]`). Non-destructive (saves/restores). [HSLPARSenseLoadOn
  InternalDAC @0xC0E00]

**Second TMDS / DVO2:**
- Original family: crossbar 0x1C20 **byte3** (`0x14|(head<<6)`), power 0x1C24
  **bit1**, serializer clock in **0x1C04 bits[17:15]** (= clksel), hold bit14.
- LX/P690: twin format register **0x1C58**, power 0x1C24 bit1, PInS config from
  `PInS[0x190]/[0x194]`, data 0x1C70. (Head-1 output on P690 is vendor "DVO2"
  driven through 0x1C54 in our tested board — the instance↔connector map is
  board-specific.)
- Sequence (both): power-down → program gated serializer clock → PInS board
  config → crossbar/format/sync → ungate clock → power-up. The caller's
  vid-reset then latches the serializer. [HSLPAREnableDVO @0xB04F0, HSLPAREnable
  DVO2 @0xD83B0]

---

## 7. DC sync / ApplyVidReset — bringing up head 1 without disturbing head 0

The vid-reset that latches new timing is a **shared global strobe with a
per-head route gate**, so one head can be reset while the other keeps scanning.
[CONFIRMED — `parhelia_apply_vid_reset`, HSLPARApplyVidReset @0xAFDC0; HW-tested
single-head.]

Mechanism:
- **Strobe** = 0x1C00 **bit24** (`RSTSTROBE`) — exists only on controller A's reg;
  a single global pulse (low → settle → high → low).
- **Route** = **bit4** (`RSTROUTE`) on the *target head's* clock reg (0x1C00 for
  head 0, 0x1C04 for head 1). Only a head whose RSTROUTE is set actually sees the
  strobe.
- **Reset bits** = `0x00030000` (bits17:16) in the *target head's* timing MISC
  (0x1900 head 0 / **0x1B00 head 1**).

Independent-head sequence (reset head 1, head 0 untouched):
1. Clear the programming-hold (bit5) on both 0x1C00 and 0x1C04 if set.
2. Set reset bits `0x00030000` in **0x1B00** (head 1 timing MISC) only.
3. Set RSTROUTE (bit4) in **0x1C04** only.
4. Pulse the shared strobe on 0x1C00 bit24 (low, ~10 µs, high, low).
5. Clear RSTROUTE in 0x1C04; clear reset bits in 0x1B00.

Head 0's generator (0x1900, RSTROUTE on 0x1C00 left clear) never sees the pulse,
so its scanout is undisturbed. For simultaneous bring-up set RSTROUTE on both
controllers and reset bits in both timing blocks before the single strobe.

---

## 8. What the driver still needs (residual gaps)

1. **Independent per-head PLL assignment** — original-family `dc_clock` shares
   PLL2; P690 `program_pll` over-routes controller A when `dcHead==1`. Both must
   assign two distinct PLLs and route only each head's own controller for a
   real extended desktop (§3). *Software only; registers understood.*
2. **RandR wiring** — `ParheliaCrtcInit` (P690 path) creates 2 CRTCs + 2 outputs
   but with `possible_clones = 0` and a single fixed `possible_crtcs` bit per
   output. To expose clone in RandR, set each output's `possible_clones` to the
   other's mask; the crossbar (§4) already supports both outputs from one head.
3. **HW verification of the simultaneous two-head case** — every register here is
   individually HW-proven, but two heads *live at once* with two different
   clocks/regions has not been run end-to-end. This is the one true test gap.
4. **Board-portable output wiring** — head↔DVO/DAC binding is hardcoded to the
   tested 0x0528/P690 boards; the general path reads `PInS[0x1a1]` (§4).
