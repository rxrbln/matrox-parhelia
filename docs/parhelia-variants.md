# Matrox Parhelia-family silicon variants

What silicon the proprietary driver (v1.4.7, 2009) supports and how the variants
differ — for judging how much per-chip code a new driver needs. Evidence tags:
[SRC]=driver C source/headers, [DRV]=disassembly, [INF]=inferred from Matrox
product knowledge (flagged).

## Family map — DONE

Vendor is always `0x102b`. Codenames from `MtxPci0.h`; family letters from
`mtx_drv.h` (`PCI_DEVICE_ID_PSERIES_FAMILY_A..F`); X-driver chipset strings are
literally "P-Series Family A".."E".

| Family | PCI dev | Codename | Marketing name | Driver | Conf |
|---|---|---|---|---|---|
| **A** | 0x0527 | PARHELIA | **Parhelia-512** (AGP, 256-bit bus) | active | [SRC] high |
| **B** | 0x0528 | PARHELIA8X | Parhelia AGP-8X / PCI / HR256 / APVe | active | [SRC] high |
| **C** | 0x2537 | SUNDOG | **Parhelia-LX** — Millennium P650 / P750 (128-bit) | active | [SRC] id; [INF] product |
| **D** | 0x2538 | SUNEX | low-end LX derivative (P650 PCIe / QID-class) | active | [SRC] id; [INF] product |
| **E** | 0x2539 | *(none)* | Millennium **P690** generation | active | [SRC] id; [INF] product |
| **F** | 0x0529 | PHOENIX | Phoenix 512 MB PCIe x16 | **stubbed** | [SRC/DRV] high |

Notes:
- **No family beyond F.** The "`_FAMILY_P`" token is the *"P-Series Family"*
  naming scheme, not a chip. There is **no P790** in this driver.
- **Phoenix (F, 0x0529) is declared but not driven** — it has header defines but
  is absent from the kernel `id_table`, the kernel bind switch, and the X
  driver's chipset table (1 incidental code reference). It will not bind.
- Marketing/board names (P650, P750, P690, QID, Epica, …) come from a
  **subsystem-ID** lookup (`powerdesk/mtxcards`), not the primary device ID.
  **QID is a board grade, not a silicon family** (a `qid` flag on top of
  Sundog/Sunex).
- Maps to the common names: **Parhelia** = A/B, **Parhelia-LX** = C/D,
  **P690** = E.

## Detection & branching [SRC+DRV]

Kernel probe (`mtx_drv.c` / `mtx_dev.c`) registers exactly the 5 active IDs
(A–E) and binds **all of them to one core** (`mtx_parhl_driver`); the X driver
has one HAL path (`MTXParhl*`) shared by all, with **device-ID-conditional
tweaks**, not separate drivers. Observed behavioural buckets in the disassembly:

- **{A,B}** — original Parhelia (grouped by `0x527..0x528`).
- **{C,D,E}** — the newer LX / P6xx generation (grouped for TV/CRTC setup).
- **E (0x2539)** singled out in ~30 mode/CRTC/preinit sites.
- **C (Sundog, 0x2537)** the most special-cased silicon (65 sites), with its own
  block of 3D/rasteriser workaround options (`*Sundog` switches).

Per-ID branch-site counts (X driver): 0x2537=65, 0x2539=30, 0x527=22, 0x2538=16,
0x528=11, 0x529=1 (stub).

## Feature / capability differences

Legend: **P** proven from this tree, **I** inferred from Matrox product
knowledge (the driver delegates these to the HAL board-caps, so they are not
decodable from `mtx_drv.so` alone).

| Feature | Parhelia-512 (A) | Parhelia8x (B) | Sundog-LX (C) | Sunex (D) | P690-gen (E) |
|---|---|---|---|---|---|
| Display heads (CRTC) | 2 **P** | 2 **P** | 2 **P** | 2 **I** | 2 **I** |
| TripleHead | yes (not "Pro") **P** | yes (not "Pro") **P** | reduced **P** | no **P** | no **P** |
| QuadHead | via QID boards **P** | via QID **P** | via QID **P** | I | I |
| Texture units | 4 **P** | 4 **P** | 2 **I** | 2 **I** | ? |
| Memory bus | 256-bit **I** | 256-bit **I** | 128-bit **I** | 128-bit **I** | 128-bit **I** |
| Bus type | AGP-8x **I** | AGP-8x **I** | AGP/PCIe **I** | PCIe **I** | PCIe **P** |
| GigaColor (10:10:10:2, depth30) | yes **P** | yes **P** | HAL-gated **P** | HAL-gated | HAL-gated |
| FAA-16x AA | yes **P** | yes **P** | mode-gated **I** | I | I |
| HW T&L (Trinity) | yes **P** | yes **P** | HAL-gated **P** | HAL-gated | HAL-gated |
| HW vertex shader 1.1 | yes **P** | yes **P** | HAL-gated | HAL-gated | HAL-gated |
| Dual-link DVI | board-caps 0x4+0x20 **P** | same | same | same | same |
| TV-out | feature-id 0x8 (HAL) **P** | P | P | P | P |

Proven detail: 4 texture pipes on Parhelia (`reg3DTEPipeSelect=4`, pipe 0123);
FAA via `reg3DThresholdAA`/`reg3DFragSize`/`reg3DAAMode=0x52`; GigaColor = depth
30 (`docs/readme-advanced.txt` §7; `Compute10BitLutEntry`); TripleHead disabled
on "Pro" boards (`MTXPreInit` prints "Parhelia Pro does not support TripleHead").
Feature gating is via **HAL caps bits** (Trinity needs bit 0x4; dual-link needs
0x4+0x20), not device-id immediates.

**RevA/RevB steppings exist within Parhelia-512:** the earliest silicon had a
vertex/lighting limitation — `reg3DLightingType` (RevA = FLOAT-only, RevB =
FLADS+FLOAT) and `reg3DVertexShaderOptions` (RevB adds PATCH_FIXUVW).

### What "Sundog" is

**Both a chip and a mode — the same thing.** Sundog = die C (0x2537), the
Parhelia-**LX**; and because the LX has a different memory/rasteriser
architecture, the driver runs a "Sundog" code path on it (`reg3DClippingType`:
1=Parhelia, 2=Sundog, 0=per-board default). The `*Sundog` switches change:
memory strategy (`UseSundogMemoryStrategies`), Z-compression handling
(`EnableZCompSundog`, …), depth format (`ForceDepthToS8Z24Sundog`), texture
memory-bank placement (`Force3DTextureTo{AB,CD}Sundog`), and rasteriser clear
behaviour. The Sundog board table (`.data`@0x330040) is all device 0x2537 —
P650/P750, RAD*mp, Spectrum, and medical boards.

### Phoenix (F, 0x0529) — reserved, never enabled

A distinct defined chip ID that this build does **not** support: absent from the
kernel `id_table`/bind switch and the X probe/name tables; the HAL memory-init
returns "unsupported" (`SetError 0x80701060`) for 0x529. The *"Phoenix" product*
shipped on **Parhelia-class silicon** (a board subsystem ID under the Parhelia
family), while the 0x0529 *die* was reserved. It sits in the Parhelia ID block
(0527/0528/**0529**), i.e. intended as a Parhelia derivative — but no capability
data for it exists in this tree.

### Feature-flag table locations [DRV, `mtx_drv.so`]

Family identity `{deviceID, pad, name*}` @0x32e700; X probe table @0xf6ce0;
Sundog board table @0x330040; runtime feature list
`MtxFeatureConfig_GenerateFeatureList`@0x4e930; validators
`DsTrinityEnabled::Validate`@0x64860 (HAL caps & 0x4),
`MTXSetDualLinkCaps`@0x8bd80 (caps 0x4 & 0x20). Device-id-keyed gates:
`mtx2819`@0x8aca0 (head caps: A/B/C vs D/E), `mtx1612`@0x97f20 (memory aperture:
A–E share, 0x529 errors).

## Register / init / low-level differences [DRV]

The HAL is a per-chip vtable built by `mtx2780`@0xbe9a0, keyed on the chip-id at
`PDEV+0x29e`. Empirically the low-level init/clock/reset cores dispatch as:

| chip-id | Init + system-clock | Hardware reset |
|---|---|---|
| 0x527, 0x528, 0x2537 (Sundog), 0x529 | **HSLPAR** | HSLPAR |
| 0x2538 (Sunex) | HSLPAR | **HSLSNX** (simpler soft-reset) |
| 0x2539 (P690-gen) | **HSLS90** (SUN90 core) | HSLSNX |

> **Correction:** despite the "Sundog/SUN90" name similarity, it is **0x2539**,
> not Sundog (0x2537), that runs the SUN90 (`HSLS90`/`HWS90`) core. Sundog uses
> the mainstream HSLPAR path — it is the most *mode-level* special-cased chip
> (65 X-driver mode/CRTC sites), but *not* low-level-divergent.

**Genuinely forked (separate code, not flags):**
- **Memory controller — different register block.** `HWPARInitializeMemoryController`
  @0xe4100 programs 0x1800/0x1a00/0x1c00; `HWS90InitializeMemoryController`
  @0xe59c0 programs the **0x0f00 block** (0xf00, 0xf48–0xf5c, 0xff4). Different
  access helpers too (`…MemoryControllerDword` vs `…S90…`). This is the one
  divergence a flag can't express.
- **System clock / frequency-synthesiser** — `HWPARInitialiseFSController` /
  `GenereMPTable` vs `HWS90…`.
- **Hardware reset** — PAR (full PCI-config save/restore) vs HSLSNX (Sunex,
  simpler soft-reset via reg 0x300, different wait timing).
- **PLL VCO limits — thin fork:** `pllGetVcoOutputDriver` tail-calls a `sun90`
  variant when a descriptor field == 1. The heavy PLL math (`pllGetDivider`,
  27 MHz ref) is **shared and data-driven** — only the VCO readback differs.

**Uniform across all families (one path + flags):**
- **CRTC** — a single `HSLCRTC*` module (`HSLCRTCSet`@0xeda60, …); no per-chip
  fork. Mode timing/start/pitch are identical.
- **DAC / pixel PLL** — shared `HSLDAC*`. Max pixel clock is per-**output**
  (default 125000 kHz), not per-chip. (No "Cap66Mhz" register or token exists in
  the shipped binaries — that term is not in this driver.)
- **Display heads** — dual CRTC at 0x1800/0x1a00 common to all; triple-head is a
  *software* mode combiner, and "DualHead2Go" is an external box — not a silicon
  head-count fork.
- Within HSLPAR there are only small inline `if(id==0x527)` / `if(id==0x2537)`
  tweaks, not separate paths.

### EAR547 — the errata workaround

`ApplyEAR547` (`_Z11ApplyEAR547P4PDEV`@0x58e80) is a memory-controller / DMA-engine
**errata read-modify-write** over the 0x1220–0x138c engine window (0x1220/0x1224/
0x1228←0x100000, 0x1300/0x1304/0x1308, 0x1324/0x1328/0x132c, 0x138c←0x10001,
0x12bc, …). It is **capability-gated, not device-id-gated**: it skips entirely if
`PDEV+0x20 & 0xe000` (bits 13-15, derived from chip-id + straps by `mtx2819`) is
set — so it patches the baseline Parhelia engine parts. Siblings `ApplyEar63`
@0xce850 and `DetectEAR66`@0xacd90 confirm a numbered Matrox errata/ECO series.

## Verdict — per-chip code a unified driver needs [DRV]

Not N separate drivers, and not a single flag-only path, but:

**1 main register path (HSLPAR) + a capability-flag table + ~2 small alternate
low-level modules:**
- the **SUN90 memory-controller + system-clock core** for 0x2539 (its MC lives
  in a physically different register block — 0x0f00 vs 0x1xxx);
- a **small Sunex reset variant** (0x2538);
- a **thin PLL VCO-limit fork** (the math stays shared/data-driven).

CRTC, DAC, pixel-PLL, heads, and the engine are shared. So: start with A/B
(Parhelia-512/8X — mainstream path, fewest special cases), add the Sundog/Sunex
`if(id==…)` tweaks + the Sunex reset, and treat the P690 (0x2539) SUN90 memory/
clock core as a second bring-up module. Phoenix (F) is out of scope — the
reference driver never drove it.
