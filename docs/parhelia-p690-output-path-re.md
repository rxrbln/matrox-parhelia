# The P690 / SUN90 (0x2539) output path, reverse engineered

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Why: the P690 (`102b:2539`, board `P69-MDDE128F`, two DVI-I connectors) is the
part we ship against.  It works today - our DRM driver drives a 1920x1200 DVI
display on the connector DRM names `DVI-I-1`, reads its EDID, and runs
ioquake3 at 76 fps.  This document is therefore a **correctness and
completeness review**, not a bring-up: it reverses the vendor's complete
"mode computed -> picture out" path for `0x2539`, contrasts it step by step
with the 0x0527/0x0528 (HSLPAR) sequence established in
`parhelia-hslpar-output-path-re.md`, and lists what our SUN90 path still does
differently - separating the items that are wrong *today* on working hardware
from the items needed only for DVI-2 or dual head.

Static reverse engineering only.  No hardware was touched while writing this.

## The answers, up front

* **There is one HAL for all five parts.**  `HSLInitDeviceJumpTable`
  (`mtx740 .text+0x999d0`) installs the same vtable for `0x0527`, `0x0528`,
  `0x2537`, `0x2538` and `0x2539`; `HSLPARSetMode` is shared verbatim and
  contains **no** device-id test.  Every P690 difference lives inside the
  callees, and there are exactly two shapes of gate: `devid - 0x2537 <= 2`
  (all three LX/P-series parts) and `devid == 0x2539` (P690 only) (§1).
* **Only four things in the whole HAL are swapped wholesale for `0x2539`**, by
  `HSLPARInitExtraJmpTbl` (`.text+0xbe9b0`): chip init
  (`HSLS90InitializeHardware`), FS allocation (`HSLS90GetFreeFS`), the
  GCLK/system-clock accessors, and - shared with `0x2538` - the hardware reset
  (`HSLSNXHardwareReset`).  Everything else, including the entire display
  controller, the DAC block, the vid reset, the LUT and the priority logic, is
  the same code the 0528 runs (§1.2).
* **`PARHELIA_DC_CTRL_ON = 0x00040201` is wrong for the P690 too, and bit 9 is
  the primary-LUT bypass here as well.**  `HSLPARGetDCParameters`
  (`.text+0xac770`) contains only two device gates, both `0x0527`, and for a
  plain progressive primary mode it builds `0x1800 = 0x00000001`.
  `HSLPARSetPrimaryLUTBypass` (`.text+0xca0d0`) sets exactly bit 9 of
  `0x1800 + (dc << 9)` with no device test, and `HSLPARResetBandwidthState`
  (`.text+0xcf540`) *clears* bit 18 of all four DC_CTRLs.  **Our 256-entry
  gamma ramp is dead silicon on hardware that works right now** (§5.4).
* **The DEMREQCTL constant `0x0b0b` is - unexpectedly - right in content for
  this family**, because `HSLS90InitializeHardware+0x6c4` (`.text+0xdde84`)
  ORs `0x0808` into `0x1c0c` *and* `0x1c10` at init, which the HSLPAR init
  does not.  What is wrong is the level: the vendor's last act of every mode
  set is `HSLPARUpdatePriorityLevels`, and for a single active head that
  computes level **7**, not 3 (§5.8).
* **The board's DDC pin resolution is exactly what our descriptors say**, and
  the `0x1c4c` channel select really is a no-op here - but for a different
  reason than the sibling document gives: it is `PInS[0x188] = 0x00700021`
  (ids 1, 2 and 7 only) and `PInS[0x18c] = 0x00000000` that leave MISC-IO ids
  5 and 8 undeclared, and `HSLPIN60GetMISCIORegMasks` substitutes its built-in
  default **only** for the pins-0..7 table, never for the pins-8..10 one
  (§10).
* **`0x1c50` is written unconditionally on the S90 init path** - no PInS
  version gate - with `PInS[0x19c] = 0x08000707` on this board; our driver
  never writes it.  And `0x1c58` is written at init and again on every
  VGA->HIRES transition, as `0x40` or as **`0`** depending on a
  `PInS[0x90..0x97]` board test that **fails** on this board (`PInS[0x90] =
  0x42`), so the vendor writes `0x1c58 = 0` here (§2, §3).
* **A P690 at PCI revision 0 needs `MMIO 0x03a8 |= BIT(5)`** from
  `HSLS90InitializeHardware+0x8de` (`.text+0xde09e`); our driver never writes
  `0x03a8` (§2).
* Our LUT loader's `dc + 0x1aa` gate byte is **not** a vendor register: at
  `dc = 0x1800` it is byte 2 of `0x19a8` and at `dc = 0x1a00` byte 2 of
  `0x1ba8` - the *second* (overlay) controller's DC_CTRL (§5.9).

## 0. Binaries, method and how claims are marked

| Tag | Binary | Notes |
|---|---|---|
| `mtx740` | `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | vendor Linux X driver, one `HSLPAR*` HAL covering 0x0527/0x0528 **and** LX/P690; full dynamic symbols |
| `romP690` | `tools/p690.rom` | this family's VBIOS, `PCIR` device `2539`, PInS at file offset `0x76c0`, version `0x0700`, length 1024, 8-bit checksum 0 |

All `mtx740` offsets are `.text` virtual addresses in that file.  `objdump`
renames the exported symbols to local aliases (`HSLPARSetMode` prints as
`<mtx2513>`); the names below are the `nm -D` names and the offsets are the
real ones.

MMIO is reached through `ClientReadRegisterDword(dev, reg, &out)` and
`ClientWriteRegisterDword(dev, reg, val)`, so in every quoted fragment the
register offset is the second argument (`esi`).  Device-struct fields used
below:

* `WORD [dev+0x29e]` - the PCI device id; `DWORD [dev+0x2a0]` - the PCI
  revision.
* `dev+0x3b78` - the PInS shadow, so PInS byte `N` is `[dev+0x3b78+N]`.
* `DWORD [dev+0x4a98]` / `[dev+0x4a9c]` - add-on module present flag and type
  enum.  On this board `PInS[0x1a1] = 0x14`, which matches **none** of the
  constants `HSLPARDetectCEM` (`.text+0xc0b90`) tests (`{1,0xa,0x10}` at
  `+0xc0bea` and `{2,3,4,5,8,9,0xc,0x11,0x12}` at `+0xc0c14`), so every
  add-on-gated branch below is dead on this board.  PROVEN.

### The P690 board's PInS, read out of `romP690`

| PInS field | file offset | value | meaning |
|---|---|---|---|
| `+0x004` version | `0x76c4` | `0x0700` | > `0x609`, so the DDC table comes from PInS; > `0x60b`, so pixel-clock detuning is eligible |
| `+0x024`/`+0x028` | `0x76e4` | `350000` / `0x00000000` | FS0 target kHz / pre-baked raw word (none) |
| `+0x038`,`+0x04c`,`+0x060` | | `0` | FS1..FS3 have no PInS target |
| `+0x084` | `0x7744` | `0xffffffff` | system-clock register shadow (unset) |
| `+0x088` | `0x7748` | `0x000488b4` | I2C/port capability word |
| `+0x090`/`+0x094` | `0x7750` | `0x00000242` / `0x00000703` | the `0x1c58` board test bytes; `PInS[0x90] = 0x42` |
| `+0x174` | `0x7834` | `0x07fc9249` | DAC per-channel trim |
| `+0x178` | `0x7838` | `"P69-MDDE128F"` | board name |
| `+0x188`/`+0x18c` | `0x7848` | `0x00700021` / `0x00000000` | MISC-IO pin-id tables (§10) |
| `+0x190`/`+0x194` | `0x7850` | `0xaac00000` / `0x00000010` | DVO board configuration |
| `+0x19c` | `0x785c` | `0x08000707` | the value the vendor writes to `0x1c50` |
| `+0x1a1` | `0x7861` | `0x14` | add-on type enum - matches no add-on |
| `+0x1a5` | `0x7865` | `0x00` | board type byte |
| `+0x1b4` | `0x7874` | `0x03040102` | DDC pin ids (§10) |
| `+0x1c4` | `0x7884` | `0x00000002` | bit 1 set: detuning flag |

PROVEN (read out of `romP690` at those file offsets).  Note `PInS[0x1b4]`
equals the built-in S90 default that `HSLPARCSetDefaultPinsS90`
(`.text+0xdcaa0`, `+0xdcccf`) installs, so this resolution is a silicon-family
property and not peculiar to this ROM.

Marking: **PROVEN** = read directly out of the named binary at the named
offset (or out of `romP690` at the named file offset).  **INFERRED** =
deduced, consistent, but not a single decisive instruction.  Nothing here has
been run on hardware while writing this document; where a statement rests on
an *earlier* hardware test in our own tree it is marked **HW** with the
document that recorded it.

## 1. The vendor's mode set, in order, and what differs from HSLPAR

### 1.1 `HSLPARSetMode` is shared, byte for byte

`HSLPARSetMode` (`mtx740 .text+0xd11e0`).  PROVEN call order, with **no
device-id comparison anywhere in the function** (an exhaustive scan of the
whole body finds none):

```
 d1201  SelectDC                          -> the DC module index (0/1/2/3)
 d1259  HSLPARWaitEngineNotBusy
 d125e  test BYTE [dev+0x3a8],0x2         -> "hardware already initialized"
 d126a    HSLPARSwitchToHIRES
 d12e3  else call [dev+0x4e80]            -> InitializeHardware vtable slot:
                                             HSLS90InitializeHardware on 0x2539,
                                             HSLPARInitializeHardware otherwise
 d1275  HSLPARValidateSFA
 d127d  HSLPARMemoryControllerCheck
 d1288  HSLPARApplyModeInfoModifications
 d1298  HSLPARDetectSetMinorChanges  -> if it reports a change, ONLY the three
                                        structure-update calls at the bottom run
 -- full path --
 d12f1  HSLPARDisableDisplayOutputs
 d12fc  HSLPARDisableIntermediateModule
 d1307  HSLPARDisableDisplayController
 d1312  HSLPARDisableClock
 d1317  test BYTE [modeinfo+0x16],0x10    -> if set, skip the whole enable half
 d1323  HSLPARComputePitch
 d1334  HSLPARInternalSetDisplayStart
 d1342  HSLPARGetClockInfo
 d1350  HSLPAREnableClock                  <- the only clock step that differs
 d135f  HSLPAREnableDisplayController      (calls HSLPARApplyVidReset at +0x103)
 d136e  HSLPAREnableIntermediateModule     (the DVO/TMDS serializer block)
 d1379  HSLPARApplyLUT
 d1384  HSLPAREnableDisplayOutputs         (the DACs)
 d138f  HSLPARSetModeZoom
 d139a  HSLPARSynchronizeDC
 -- always --
 d12ab  HSLPARSetModeUpdateStructure
 d12b9  HSLPARUpdateClockInfoStructure
 d12c1  HSLPARUpdatePriorityLevels         <- writes the FINAL DEM levels
```

So the ordered answer to "what is the SUN90 mode-set sequence" is: **the same
sequence as HSLPAR**.  The differences are entirely inside `HSLPAREnableClock`
(§4), inside `HSLPAREnableIntermediateModule`/`HSLPAREnableDVO` (§6), and in
the one-off chip init (§2).

### 1.2 The complete set of `0x2539`-specific dispatch points

`HSLPARInitExtraJmpTbl` (`.text+0xbe9b0`) is the only place the HAL swaps a
function for the P690.  PROVEN at `+0xbeae1`..`+0xbeca7`:

| vtable slot | `0x0527`/`0x0528`/`0x2537`/`0x2538` | `0x2539` |
|---|---|---|
| `[dev+0x4e80]` InitializeHardware | `HSLPARInitializeHardware` | **`HSLS90InitializeHardware`** (`.text+0xdd7c0`) |
| `[dev+0x4ef8]` GetFreeFS | `HSLPARGetFreeFS` | **`HSLS90GetFreeFS`** (`.text+0xdc060`) |
| `[dev+0x4f00]` GetGclk | `HSLPARGetGclk` | **`HSLS90GetGclk`** (`.text+0xdc910`) |
| `[dev+0x4f08]` GetPInSSystemCLock | `HSLPARGetPInSSystemCLock` | **`HSLS90GetPInSSystemCLock`** |
| `[dev+0x4f10]` SetSystemCLock | `HSLPARSetSystemCLock` | **`HSLS90SetSystemCLock`** |
| `[dev+0x4f18]` GetSystemCLock | `HSLPARGetSystemCLock` | **`HSLS90GetSystemCLock`** |
| `[dev+0x4f20]` HardwareReset | `HSLPARHardwareReset` | **`HSLSNXHardwareReset`** (also `0x2538`) |

Every other display-path function in the binary is literally the same code for
the 0528 and the P690.  The in-function `0x2539` gates that matter to the
output path are, exhaustively (PROVEN by scanning every `0x2539` immediate in
`mtx740`):

| site | what it selects |
|---|---|
| `HSLPAREnableClock +0xcfd1b`, `+0xcfe7e` | `HWS90InitialiseFSController` instead of `HWPARInitialiseFSController` (§4) |
| `HSLPARInitFreeController +0xb2f29`, `+0xb300c` | same swap on the load-sense path (§7) |
| `HSLPARCheckForClkRecycling +0xcf76e` | S90 clock-recycling test (§4) |
| `HSLPARSetGclk +0xb5afa` | `HSLS90ChangeGclkSpeed` (§4) |
| `HSLS90InitializeHardware +0xddee8` | the revision-0 `0x03a8` erratum (§2) |
| `HSLSI164Enable +0xdb734` | external-TMDS variant (§6) |
| `HSLCHREnable +0x9334d`, `HSLTVEGetIDForParameters +0xdf071`, `HSLCVE2*` | TV encoder; dead on this board |

and the `devid - 0x2537 <= 2` gates that put the P690 with the LX rather than
with the 0528:

| site | effect on the P690 |
|---|---|
| `HSLPAR_Register_PriSurfAddr_Set +0xc5847` | scanout base field is `[27:10]`, mask `0x0ffffc00` (§5.5) |
| `HSLPARGetAddressAlignmentMask +0xace43` | one shift *less* than the 0528: 4 KiB at 32 bpp (§5.5) |
| `HSLPAREnableDVO +0xb06b6/+0xb0754/+0xb0820/+0xb09f6`, `HSLPAREnableDVO1/2/s`, `HSLPARSetDVOClock +0xaef4b`, `HSLPARSetSyncPolDVO`, `HSLPARIsDvoLocked +0xaee12`, `HSLPAREnableIntermediateModule +0xb1eef/+0xb1ff4`, `HSLPARSetDisableIntermediateModule` | the whole integrated-serializer stage (§6) |
| `HSLPARSwitchToHIRES +0xbe258` | the `0x1c58` value comes from a board test instead of being forced to `0x40` (§3) |
| `HSLPARInitializeHardware +0xbf913` | (not reached on `0x2539` - the S90 init runs instead) |
| `HSLPARComputeIntermediateVidParm +0xabfe6` | skips an add-on guard-band branch; timing maths otherwise identical (§5.3) |
| `HSLPARGetDisplayStartParameters +0xacfca` | panning granule `0x10` instead of `0x20`, and only for windows wider/taller than 4096 |
| `HSLPARApplyModeInfoModifications +0xd045c` | skips the whole add-on/Chrontel link-flag block |
| `HSLPARPM_DPMS`, `HSLEnableSyncOnOutput`, `HSLPARApplyVidParmPatch`, `HSLPARClearDisplay`, `HLSPARI2CSetClockState` | not on the native mode-set path for a plain board |

**Negative result worth stating plainly: the display controller
(`HSLPARGetDCParameters`, `HSLPARSetDCParameters`, `HSLPARDimController`), the
vid reset (`HSLPARApplyVidReset`), the LUT (`HSLPARApplyLUT`,
`HSLPARSetPrimaryLUT`), the priority logic (`HSLPARSetPriorityLevels`,
`HSLPARSetDefaultPriorityLevels`, `HSLPARUpdatePriorityLevels`,
`HSLPARGetPriorityLevels`), the analog DAC block and the load sense contain
zero `[dev+0x29e]` comparisons.**  Whatever the sibling document proved about
those on the 0528 is therefore true of the P690 unchanged - including the
DC_CTRL value, which is the single largest finding of this review.

## 2. Chip init: `HSLS90InitializeHardware` versus `HSLPARInitializeHardware`

`HSLS90InitializeHardware` (`mtx740 .text+0xdd7c0`).  PROVEN sequence,
display part only, with the HSLPAR equivalent alongside:

| # | `HSLS90InitializeHardware` (0x2539) | `HSLPARInitializeHardware` (0x0527/0x0528) |
|---|---|---|
| 1 | `dd81d` read **`0x0e24`**; dead if `0` or **`0x80000000`** | `bf7e6` read `0x0e00`; dead if `0` or `0x40000000` |
| 2 | `dd84a` `dev[0x3a8] |= 2` | `bf80e` same |
| 3 | `dd853` `HSLPARApplyVidReset(dev, 0xffffffff)` | `bf824` same |
| 4 | `dd884` `0x1c00 |= 0x00809020`; `dd89b` `0x1c04 |= 0x20` | `bf84d` same constants |
| 5 | `dd8ba` wait 10 us | `bf883` same |
| 6 | `dd8d2` `0x1c08 = 0x00000002` plain | `bf8be` same |
| 7 | `dd8e4` `0x1c24 = 0x00000007` | `bf8d0` same |
| 8 | `dd8f3` `0x1c50 = PInS[0x19c]`, **unconditional** (`dd8c7 mov 0x19c(%rbp),%ebx`) | `bf8df` `0x606` if `PInS[0x04] <= 0x607`, else `PInS[0x19c]` |
| 9 | `dd902` `0x08fc = 0` | `bf8ee` same |
| 10 | `dd91e` `0x1c58` = `0x40` if `[dev+0x4a9c] == 4` **or** the `PInS[0x90..0x97]` board test passes, else **`0`** (`ddfdc`..`de059`) | `bf9a6` `0x40` unconditionally |
| 11 | `dd93e` `0x0920 &= ~BIT(31)` | `bf9bb` same |
| 12 | `dd967` `0x10b0 = 0x40` | `bf9de` same |
| 13 | `dda55` `HSLPARDisableDisplayOutputs` over the board's link flags | `bfad1` same |
| 14 | `dda9c`..`ddd96` `HSLPARDisableDisplayController` + `HSLPARSetPrimaryLUT` for mode words `0x100000`, `0x180000`, `0x100001`, `0x180001` (head 0/1 primary, head 0/1 second) | `bfb30` same four |
| 15 | `dde7a` **`HSLS90InitMemClk`** | `bfee8` `HWPARInitializeSystemClock` + `HSLPARGetSystemCLock` + `HWPARInitializeMemoryController` |
| 16 | `dde84` **`0x1c0c |= 0x0808`; `ddeb1` `0x1c10 |= 0x0808`** (read-modify-write, byte 0 and byte 1 each `|= 0x08`) | **absent** - the HSLPAR init never writes the DEMREQCTLs |
| 17 | `ddee8` if `devid == 0x2539` **and** `[dev+0x2a0] == 0` (PCI revision 0): `0x03a8 |= BIT(5)` (`de0af`..`de0d2`) | absent |
| 18 | `ddf2f` `0x12b8 = 0x00070601`; `ddf43` `0x12b0 = 0` | `bff36` `0x12b8 = 1` for `0x0527`, else `0x00070601` |
| 19 | `ddf50` for `i = 1..0x13`: `MMIO (i<<4) &= 0x0fffffff` | same |
| 20 | `ddf83` `0x0000 = 0x10000000`; `0x0004 = 0`; `0x0008 = 0x10000020`; `0x000c = 0x00010001` | `bffa2` same |

Three of these are live gaps in our `parhelia_engine_init()`:

* **`0x1c50` is never written** (vendor: `PInS[0x19c] = 0x08000707` here).
* **`0x1c58` is never written on the P690.**  Our code writes `0x40` only for
  `PARHELIA_512`/`PARHELIA_8X`.  The vendor writes `0` on this board because
  `PInS[0x90] = 0x42` is neither `9` nor `2` (`ddff7  movzbl 0x90(%rbp),%eax;
  ddffe cmp $0x9,%al; de003 cmp $0x2,%al`, failure path `de057 xor %edx,%edx;
  jmp dd92c`).
* **`0x03a8` is never written**, so a revision-0 P690 misses the erratum.
  (Our tree already records this write from the Windows miniport in
  `parhelia-engine-and-context-init-complete.md` §283; it is the same bit.)

And one is already right: our `parhelia_rmw(pdev, 0x1c0c, ~0u, 0x0808u)` /
`0x1c10` in `parhelia_engine.c:88` matches step 16 exactly, register, value
and read-modify-write form.  It is the `0x1c00` constant that is wrong in the
same function - `0x00808020` where both vendor init paths use `0x00809020`.
The missing bit 12 is DVO1's clock gate (`HSLPARDisableDVO1 .text+0xd732f`,
`HSLPARPowerUpDVO1Clock .text+0xd8382`), so our init leaves DVO1's clock
running on a board whose picture comes out of the other serializer.

## 3. The VGA <-> HIRES mux at `0x1c08` on the P690

`0x1c08` bit 0 = legacy VGA path enabled, bit 1 = HIRES (native display
controller) path enabled - identical on all five parts.

`HSLPARSwitchToHIRES` (`.text+0xbd7d0`) runs on every mode set once
`dev[0x3a8] & 2` is set.  Its entry test (`bd841 movzbl 0x2c(%rsp),%eax;
bd846 test $0x2,%al; bd848 jne be403`) returns immediately only when bit 1 is
already set; a POSTed board in VGA text mode takes the full path.  PROVEN.

The head of the function is device-gated at `bd85c`
(`lea -0x527(%rdx),%eax; cmp $0x1,%eax; ja be258`), and the `0x2539` branch at
`be258` differs from the 0528's in exactly one value:

```
 0x0527/0x0528 (bd86b):  0x1c58 = 0x40                      unconditional
 0x2539        (be264):  0x1c58 = 0x40  if [dev+0x4a9c] == 4
                         (be271)       or the PInS[0x90..0x97] board test passes
                         (be2d1)  else 0x00000000
 -- then identical on both --
 bd891  0x1c18 |= BIT(15)                 DAC1 video power down
 bd8b9  0x1c1c |= BIT(16)                 DAC2 video power down
 ...    HSLPARDisableDVI(dev, 0..3, buf) for each declared output-link flag
```

On this board the test fails, so **the vendor writes `0x1c58 = 0` at both the
init and the VGA->HIRES transition.**  PROVEN.

The tail (`be101`..`be242`) has no device gate at all:

```
 be101  HSLPARVGAWaitForVerticalRetrace
 be12b  HSLPARSetPriorityLevels(dev, 5, {0,0}, {0,0})   head A parked at level 0
 be133  HSLPARVGAWaitForVerticalRetrace
 be14a  0x1c08 &= ~1                      VGA off
 be160  wait 10 us
 be17f  0x1c08 |= 2                       HIRES on
 be1a7  0x1c00 |= 0x20                    DCM A hold
 be1f3  0x1c00 |= 0x00809020 (bits 5,12,15,23)
 be209  0x1c04 |= 0x20
 be20e  0x1c24 |= 0x07
 be213  write 0x1c00, be218 write 0x1c04, be229 write 0x1c24
 be23a  wait 10 us
```

Our `parhelia_program_clock()` reaches HIRES on the SUN90 path through a
read-modify-write at the bottom of the function (`if (!(ctl &
PARHELIA_DCM_MODE_HIRES)) { clear bit 0; udelay(10); set bit 1; }`), which is
the vendor's two-step transition and is **correct**.  What it does not do is
the two vertical-retrace waits, the `HSLPARSetPriorityLevels(5, 0, 0)` park,
the `0x1c00 |= 0x00809020` / `0x1c04 |= 0x20` / `0x1c24 |= 7` refresh, or the
`0x1c58` write.

## 4. The clock path

### 4.1 `HSLPAREnableClock` and which clock source a display mode uses

`HSLPAREnableClock` (`.text+0xcfc00`), args `(dev, vidparm, clkinfo)`.  It sets
an internal "a real reprogram is needed" flag only when the transition is
neither smooth nor vid-parm free (`cfc93`, `cfca3`, `cfcd0`..`cfcdb`), then
switches on `clkinfo[0]` through the jump table at `.rodata 0x1119ac`
(10 entries).  PROVEN:

| source | what it programs |
|---|---|
| **0** | **the pixel clock.**  `HSLPARTranslateDcmClkSelIntoFS(clkinfo[4])`, the detuning check (§4.5), then `cfe7e cmpw $0x2539` -> **`HWS90InitialiseFSController`** (`cfe94`), else `HWPARInitialiseFSController` (`cfda7`) |
| 1 | `HSLPARSetGclk` - GCLK, see §4.4 |
| 2, 3 | **nothing at all** (immediate return, `cfce7`) |
| 4 | `HWPARInitialiseFSController` with no `0x2539` gate |
| 5..8 | Chrontel TV clocks |
| 9 | `cfd1b cmpw $0x2539` -> `HWS90InitialiseFSController` + `HSLS90EnableTstClk`, else the HSLPAR pair |

Which source a monitor mode gets is decided by `HSLPARGetClockInfo`
(`.text+0xcf9f0`) -> `HSLPARGetMonitorClockSource` (`.text+0xcf810`) when the
link flags carry no TV bit (`vidparm[0x14] & 0x0400f070 == 0`).  PROVEN:

```
 cf86a  HSLPARCheckForClkRecycling
 cf877  if (recyclable) { source = 3; return; }      <- case 3 programs NOTHING
 cf8a3  source = (vidparm[0x24] >= 0x7a120 /*500000 kHz*/) ? 1 : 0
```

So **a normal P690 display mode is source 0** and the only synthesiser writer
reached is `HWS90InitialiseFSController`.  Source 1 needs a pixel clock at or
above 500 MHz; sources 5..9 need TV or add-on hardware.

Source 3 is worth carrying: `HSLPARCheckForClkRecycling` (`.text+0xcf6e0`)
calls `HSLPARIsDvoLocked` first, and if a DVO is already up and pointed at the
target controller the vendor **programs no FS at all** and reuses the existing
DCM selection, recovering it as
`clksel = ((0x1c00 >> 27) & 1) << 2 | ((0x1c00 >> 13) & 3)` (`cf7cc`).  PROVEN.

### 4.2 The synthesiser bank and the divider word

**PROVEN: `0x0e30 + 4*fs`, one register per FS, and there are six of them.**
`sun90pllGetRegOffset` (`.text+0xecaa0`) dispatches through the table at
`.rodata 0x112b78`: fs 0..3 -> `0x0e30`, `0x0e34`, `0x0e38`, `0x0e3c`;
fs 4 and 5 -> `0xffffffff` (invalid, `ecac1`); fs 6 -> `0x0e40`, fs 7 ->
`0x0e44`.  `HWS90InitialiseFSController+0x1d` computes it independently
(`e5bcd lea 0xe30(,%rbp,4),%eax`).  The source file name survives in
`.rodata 0x112bc8`: `"hwsharing/sun90pll.c"`.

`0x0e30`..`0x0e3c` are **shadowed in software**: the write callback
(`.text+0xe5f30`) calls `HWWriteRegisterDword` and then, for exactly those four
registers (`e5f48`..`e5f74`), `HWSun90WriteRegShadowDword`; `sun90pllHwInit`
read-modify-writes from `HWSun90ReadRegShadowDword` (`.text+0xe5f10`), not from
MMIO, while the lock poll uses the real MMIO read (`.text+0xe5ef0`).  INFERRED:
the divider fields do not read back, the status bit does.

Divider word, PROVEN from `sun90pllGetDivider` (`.text+0xecaf0`) cross-checked
against `sun90pllHwInit`'s inserts and the debug string at `.rodata 0x1129d8`
(`"\nBest Err %d KHz, M1=%d,P1=%d,M2=%d,P2=%d\n"`):

| field | bits | stored as | evidence |
|---|---|---|---|
| M1 | `[5:0]` | value - 1 | `ecbf2` / `eb791` |
| M2 | `[10:6]` | value - 1 | `ecc2a` / `eb7ba` |
| P1 | `[15:11]` | value - 1 | `ecc5b` / `eb7a6` |
| P2 | `[19:16]` | value - 1, **4 bits, max 16** | `ecc88` / `eb7cb` |
| - | 20 | always 0 | `eb7d4` |
| **strobe** | **21** | 1 then 0 | `eb7da or $0x200000`, `eb97a and $0x7fdfffff` |
| - | 22 | `P[0x20] & 1` = 0 | `eb7e3` |
| - | 23, 25, 28, 29, 30 | always 0 | `eb7ee and $0x8d7fffff` |
| **lock** | **24** | read-only status | `eb8d9 test $0x1000000` |
| mode | 26, 27 | both 0 for the pixel clock | `eb908 and $0xf3ffffff` |
| **strobe** | **31** | 1 then 0 | `eb7f4 or $0x80000000`, `eb97a` |

Reference is **27000 kHz** (`P[0x08] = 0x6978` in the parameter template at
`.data 0x34b790`; `HWS90InitialiseFSController` divides the PInS windows by
27000 at `e5c68` and multiplies back by `0x6978` at `e5c8d`).  Frequency model:
`VCO1 = 27000 * M1`, `tap = VCO1 / P1`, `VCO2 = tap * M2`,
`Fout = VCO2 / (2 * P2)` - **exactly our kernel's model**, and the same as the
hardware-measured 27 MHz reference recorded in
`docs/parhelia-kms-p690-dvi1-fix.md` (HW).

Limits, PROVEN `sun90pllIsInvalidDividers` (`.text+0xeb380`), called once per
VCO with its own parameter block (`P+0x24` for VCO1, `P+0x78` for VCO2):
`1 <= M1 <= 64`, `1 <= P1 <= 32`, `1 <= M2 <= 32`, `1 <= P2 <= 16`, VCO1 and
VCO2 in `[800000, 1600000]` kHz, tap in `[54000, 108000]` kHz.  The windows come
from PInS as six 16-bit words times 1000 (`e5bf2`..`e5c51` for FS0; FS1 at
`+0x40`, FS2 at `+0x54`, FS3 at `+0x68`), and on this board all four FSes carry
`[800, 1600, 800, 1600, 54, 108]` - identical to the values our kernel
hardcodes.

One real difference: `parhelia_sun90_clock_solve()` clamps M2 (`b1_hi`) to 16.
The vendor's M2 range is `[1,32]` and the field is five bits wide, so we are
searching a smaller space than the hardware allows.  The vendor also searches
M2 descending 32 -> 1 and P2 ascending 1 -> 16 (`P+0x84/0x88/0x8c =
32/1/-1`, `P+0x90/0x94/0x98 = 1/16/1`), preferring large M2 and small P2; we
prefer small dividers.  Same solution space apart from the clamp, different
tie-break.

### 4.3 The programming gesture - our two-write strobe is the vendor's, and `BIT(21)` is a real strobe here

**PROVEN, `sun90pllHwInit` (`.text+0xeb730`)**, called from
`sun90pllTestDividers` with `(dev, mode, M1, P1, M2, P2, P)`:

```
 eb77f  v = shadow_read(reg)                    reg = 0x0e30 + 4*fs
 eb791  v = (v & 0xffff07c0) | (M1-1) | ((P1-1) << 11)
 eb7b1  v = (v & 0xfffff83f) | ((M2-1) << 6)
 eb7c2  v = (v & 0xfff0ffff) | ((P2-1) << 16)
 eb7d4  v &= ~BIT(20) ; v |= BIT(21)
 eb7e3  v &= ~BIT(22) ; v |= (P[0x20] & 1) << 22         P[0x20] = 0
 eb7ee  v &= 0x8d7fffff ; v |= BIT(31)
 eb908  v &= 0xf3ffffff                                  mode 0: bits 26,27 = 0
 eb966  write(reg, v)                           <- WRITE 1, BIT(31)|BIT(21) set
 eb97a  v &= 0x7fdfffff                                  clear BIT(31) and BIT(21)
 eb989  write(reg, v)                           <- WRITE 2
 eba49  wait(P[0x70] = 10 us)
 eb8cf  poll up to P[0x68] = 100 times: locked if (read(reg) & BIT(24))
 eb8f4  sun90pllUnlockStatus(dev, reg, 100, P)
 eb88a  success iff the returned unlocked percentage <= P[0x6c] = 0
```

The pixel clock always uses mode 0: `HWS90InitialiseFSController+0x1a8` passes
`xor %esi,%esi / xor %edx,%edx` to `sun90pllInit` (`e5d56`/`e5d58`), which
forwards that `edx` to `sun90pllTestDividers` (`ec469`) and on as `mode`.
Entry guard for mode 0 (`eb773`): `(P2 - 1) > 0xf` raises error `0x20`.

`sun90pllUnlockStatus` (`.text+0xeb640`) reads the register `count` times and
counts the samples where bit 24 is **clear** (`eb69f test $0x1000000; jne skip;
inc %r14d`), returning `unlocked * 100 / count`; the caller requires `<= 0`, so
**all 100 samples must read bit 24 set** - stricter than HSLPAR's 90 of 100.

**Verdict on our driver: `parhelia_program_clock()`'s
`write(ctl, word | BIT(31) | BIT(21)); write(ctl, word)` is exactly the
vendor's gesture, PROVEN.**  `BIT(21)` is *not* a divider bit on SUN90 - the
dividers stop at bit 19 - it is a genuine second strobe released together with
`BIT(31)`.  The sibling document's concern ("the P690 writer additionally sets
`BIT(21)`, which on this encoding is a divider bit") applies to the *HSLPAR*
encoding only and is **refuted for SUN90**.

Two deviations remain: the vendor settles **10 us** before polling where we
wait 1 ms (harmless), and it requires **100 of 100** samples locked where we
accept the first hit.  The software shadow is immaterial - every functional bit
is explicitly set or cleared and the only preserved bit is the read-only
status, so our plain write of `word` is equivalent.

### 4.4 The oscillator, the system clocks, and whether our fixed FS choice is safe

**One step our driver omits entirely.**  `sun90pllHwOscOn` (`.text+0xeb260`) is
called unconditionally at the top of `sun90pllInit` (`ec0e9`), i.e. on every
P690 pixel-clock program, on register **`0x0e24`**:

```
 v = read(0x0e24)
 if (!(v & BIT(31)) && (v & BIT(30))) return;       already on
 v &= ~BIT(31) ; write ; wait 1000 us
 v |=  BIT(30) ; write ; wait 1000 us
```

`0x0e24` is also SUN90's "is the chip alive" probe (`HSLS90InitializeHardware
+0x6a`, `dd82a`..`dd844`: dead if it reads `0` or `0x80000000`), where HSLPAR
probes `0x0e00`.  INFERRED-benign today: a POSTed board has bit 30 set and bit
31 clear, so the vendor's own call returns immediately.  It would matter after a
cold or soft reset that parks the oscillator.

`clksel = fs + 1` holds on SUN90 too: `HSLPARTranslateDcmClkSelIntoFS`
(`.text+0xb5800`) has no device test (1 -> fs0 ... 4 -> fs3, anything else
invalid, `b5844`..`b5886`).  So our clksel 3 <-> `0x0e38` and clksel 4 <->
`0x0e3c` pairing is internally consistent.  `HSLPARInitFreeController`
confirms the same mapping independently (`b2f4f inc %eax; and $0xf; shl $0x6`)
and falls back to clksel **7** (`0x1c0`) when no FS is free (`b2fb6`).

**But the vendor allocates the index, and SUN90 has three system-clock
registers, not one.**  `HSLPARGetClockSel` (`.text+0xcf8c0`) calls
`SelectDCModule` and then dispatches through `dev+0x4ef8`
(`cf918 call *0x4ef8(%rbx)`), which on `0x2539` is `HSLS90GetFreeFS`
(`.text+0xdc060`):

```
 dc0d8  read 0x0e00 ; dc0ea read 0x0e04 ; dc0fc read 0x0e08
 dc101  for fs = 0..3: if (!IsS90FSInUse(v00, v04, v08, fs)) free[n++] = fs
 dc129  if (module >= n) SetError(0xb2701240); else *out = free[module]
```

so DC module 0 takes the first free FS and module 1 the second, and
`clksel = fs + 1`.  `IsS90FSInUse` (`.text+0xdbf20`), with
`sel(x) = (x >> 1) & 0xf`:

```
fs0 busy if sel(0x0e04) in {1,8,9}   || sel(0x0e00) in {1,8,9}  || sel(0x0e08) in {1,8,9}
fs1 busy if sel(0x0e04) in {2,10,11}                            || sel(0x0e08) in {2,10,11}
fs2 busy if sel(0x0e04) in {3,12,13}                            || sel(0x0e08) in {3,12,13}
fs3 busy if sel(0x0e04) in {4,14,15}                            || sel(0x0e08) in {4,14,15}
```

Note `0x0e00`'s own selector is tested only in the fs0 arm (`dbf6e`, absent
from the others) - INFERRED a vendor oversight.  `IsScUseFS_SUN90`
(`.text+0xe5f90`) is the byte-identical predicate used at init.

Register layout, PROVEN `HSLS90GetSystemCLock` (`.text+0xdba60`): each of
`0x0e00`, `0x0e04`, `0x0e08` is **one system-clock domain** - bit 0 = park on
crystal, bits `[4:1]` = FS selector (1..4 -> fs0..fs3, plus the paired
`8+2fs`/`9+2fs` encodings), bits `[7:5]` = divider code through the jump tables
at `.rodata 0x111a34`/`0x111a54`/`0x111a74`.  That is a **different layout from
HSLPAR**, where all three selectors were packed into `0x0e00` alone.

On **this board**, `HWS90InitializeSystemClock` (`.text+0xe60d0`) writes the
three domains from `PInS[0x200]`, `PInS[0x204]`, `PInS[0x208]` (`e6369`,
`e637f`, `e6395`):

| PInS | file offset | value | decode |
|---|---|---|---|
| `+0x200` -> `0x0e00` | `0x78c0` | `0x00020902` | park 0, selector 1 = **fs0** |
| `+0x204` -> `0x0e04` | `0x78c4` | `0x00000002` | park 0, selector 1 = **fs0** |
| `+0x208` -> `0x0e08` | `0x78c8` | `0x00000002` | park 0, selector 1 = **fs0** |
| `+0x024` | `0x76e4` | `350000` | FS0 target, 350 MHz |
| `+0x028`/`0x3c`/`0x50`/`0x64` | | all `0` | no pre-baked raw words |

**All three system-clock domains run off FS0.**  So `IsS90FSInUse` reports fs0
busy and fs1, fs2, fs3 free, and the vendor's allocation on this board would be
head 0 -> **fs1** (`0x0e34`, clksel 2) and head 1 -> **fs2** (`0x0e38`,
clksel 3).

**Verdict on our driver.**  "FS0 is the system clock and must never be touched"
is PROVEN correct for this board.  Our fixed `head 0 -> FS2` / `head 1 -> FS3`
is *not* the vendor's allocation but is **safe here** because fs1..fs3 are all
free.  The residual risk is that we never look: on a board whose PInS pointed a
domain at fs2 or fs3 we would silently reprogram a live memory or core clock.
The fix is cheap - read the three registers and run the `IsS90FSInUse`
predicate instead of hardcoding.

GCLK and MCLK are not touched by a P690 mode set.  `HSLPARSetGclk`
(`.text+0xb5ac0`, `0x2539` gate at `b5afa` -> `HSLS90ChangeGclkSpeed`) is
reachable only from clock source 1, i.e. a pixel clock at or above 500 MHz.
`HSLS90InitMemClk` (`.text+0xdcef0`) is init-time only - its two callers are
`HSLS90InitializeHardware+0x6ba` and `HSLS90SetSystemCLock+0x2ff` - and it
brackets `HWS90ResetMemoryController` / `HWS90InitializeSystemClock` /
`HWS90InitializeMemoryController` with both DCMs held (`0x1c00`/`0x1c04` bit 5)
and both display controllers disabled.  `HSLS90SetSystemCLock`,
`HSLS90GetPInSSystemCLock`, `HSLS90GetGclk`, `HSLS90DisableTstClk` and
`HSLS90InitializeHardware` have **no direct callers** (vtable-only or dead).

### 4.5 `HSLPARSelectDisplayControllerClock` is shared - and only controller A gets the VGA MISC byte

**PROVEN: `HSLPARSelectDisplayControllerClock` (`.text+0xab100`) contains no
`0x2539`, `0x2537`, `0x527` or `0x528` comparison at all**, confirmed both by
reading the body and by mapping every family immediate in the binary to its
containing function (this one does not appear).  Controller A, unchanged for the
P690:

```
 ab14e  read 0x1c00 -> v
 ab158  if (!(v & 0x20) && ((v >> 6) & 0xf) == clksel) return   nothing to do
 ab170  v |= 0x20 ; ab189 write 0x1c00                          assert hold
 ab196  ClientWait(10)
 ab1a7  v = (v & ~3) | 2 ; ab1b1 v &= ~BIT(10) ; ab1ba write 0x1c00
 ab1d0  vNew = (v & 0xfffffc3f) | ((clksel & 0xf) << 6)
 ab1db  ClientWriteRegisterByte(dev, 0x1fc2, 0x68)              <-- plain literal
 ab1ec  write 0x1c00 = vNew
 ab1f1  vNew &= ~0x20 ; ab202 write 0x1c00                      release hold
```

Controller B (`ab215`..) is the same shape on `0x1c04`, reads both `0x1c00` and
`0x1c04` (`ab26a`/`ab284`), and has **no `0x1fc2` write anywhere**; its only
extra branch needs the TV mode flag together with an add-on module.

**Verdict on our driver.**  The rule is identical to HSLPAR: controller A gets
`0x1fc2 = 0x68` as a plain byte write of the literal; controller B gets
nothing.  Our `PARHELIA_CLOCK_SUN90` branch instead read-modify-writes MISC
bits `[3:2]` **unconditionally for both heads**, and does it *after* the DCM
routing and the DEMREQCTL writes rather than between the `0x1c00` divider write
and the clock-select write with the hold still asserted.  Since our P690 pipe
drives head 1 / controller B, we are writing MISC on the one head the vendor
never writes it on.  It is harmless today (HW: the hardware-tested DDX did the
same and produced a picture, `docs/parhelia-kms-p690-dvi1-fix.md`), so this is
a fidelity item, not a bug - but the `!head` guard the HSLPAR branch already
has belongs here too.

The other write in our SUN90 branch that the vendor does not make is
`parhelia_route_dcm(pdev, 0, 4)` for head 1: it points **controller A's** clock
mux at FS3 before programming controller B.  Controller A is idle today so
nothing notices, but it is a stray write into the other head's clock mux and it
would corrupt a second pipe.

### 4.6 Pixel-clock detuning is armed on this board

`HSLPARPixelClockDetuning` (`.text+0xcebb0`) writes no register; it shifts the
requested clock by 100 kHz:

```
 cebcc  if (vidparm[0x16] & 0x08)  vidparm[0x24] += 100;
 cebe3  else                       vidparm[0x24] -= 100;
```

Gate, PROVEN inside `HSLPAREnableClock` case 0 (`cfe29`..`cfe74`, `r14 =` the
PInS shadow):

```
 cfe29  if (PInS[0x04] <= 0x60b)               skip
 cfe32  if (!(PInS[0x1c4] & 0x02))             skip
 cfe41  if (vidparm[0x24] <= 64999 kHz)        skip
 cfe4e  if ((links & 0x20000000) && (dev[0xa3] & 0x40))   detune
 cfe5c  else if ((links & 0x40000000) && (dev[0x23] & 0x20))  detune
```

This board is PInS `0x0700 > 0x60b` and `PInS[0x1c4] = 0x00000002` (bit 1 set),
so **detuning is armed here**, unlike the 0528 board (PInS `0x60a`).  A
1920x1200 mode at 154 MHz clears the 65 MHz threshold.  Whether it fires
depends on a per-output byte in the output descriptor block
(`dev + 0x20 + idx*0x80`), which INFERRED is an EDID-derived "this monitor
needs detuning" flag.  We do not implement it; the effect would be a 100 kHz
shift, i.e. 0.065% at 154 MHz, so this is a completeness note rather than a
defect.

## 5. The display controller (head)

### 5.1 Four controllers, not two - unchanged on the P690

PROVEN, `HSLPARIsDCInUse` (`mtx740 .text+0xab6f0`), which probes bit 0 of the
four enable registers with no device test:

| DC index | enable register | what it is |
|---|---|---|
| 0 | `0x1800` | head A primary |
| 1 | `0x1a00` | head B primary |
| 2 | `0x19a8` | head A second ("video"/overlay) controller |
| 3 | `0x1ba8` | head B second controller |

`SelectDCModule` (`.text+0xba780`) returns the *head* (0 or 1) from mode flag
bit 19; `SelectDC` (`.text+0xba750`) returns `head + 2 * (flags & 1)`.
Register stride is `head * 0x200`, and `HSLS90InitializeHardware` disables all
four at init (`dda9c`..`ddd96`, mode words `0x100000`/`0x180000`/`0x100001`/
`0x180001`), so the P690 really does carry four display controllers.

### 5.2 `HSLPAREnableDisplayController` (`0xb1130`), in order

PROVEN at `+0xb1156`..`+0xb1238`, no device test:

```
 b1156  module = SelectDCModule(vidparm)
 b11ae  HSLPARDetectSmoothTransition        -> smooth?
 b1211  HSLPARDetectVidParmFreeTransition   -> nothing to reprogram?
 b1226  HSLPARSelectDisplayControllerClock  (only on a real change)
 b11c9  HSLPARGetDCParameters               (computes; reads 0x1918, 0x1888)
 b11d7  HSLPARSetDCParameters               (all the head writes)
 b11e7  HSLPARApplyVidParmPatch             (0x1b14 only; never fires here)
 b1233  HSLPARApplyVidReset(dev, module)
 b11f7  HSLPARSetDefaultPriorityLevels      (all four level fields = 3)
```

**Ordering note that matters to us:** the vendor's vid reset happens *inside*
`HSLPAREnableDisplayController`, i.e. **before** the DVO serializer and the
DACs are programmed, and `DC_CTRL` is written exactly once, inside
`HSLPARSetDCParameters`.  Our `parhelia_pipe_enable()` programs the output
first, then writes `DC_CTRL`, then resets, then writes `DC_CTRL` again with
the comment "the P690 reset transient can clear DC_CTRL's enable bit" - which
is precisely the consequence of resetting after enabling.  Moving the reset
ahead of `parhelia_program_output()` removes the need for the second write.

### 5.3 `HSLPARSetDCParameters` (`0xab850`) - the exact write order

`p[0]` = DC index, `p[4]` = head; `B = 0x1800 + head*0x200`, timing block
`B + 0x100`.  PROVEN order (no device test in the function):

1. `ab886` if `p[0]` is 2 or 3 the enable register is `0x19a8 + head*0x200`,
   else `0x1800 + head*0x200` (`ab9b0`).
2. `ab8a8` `HSLPARDetectVidParmFreeTransition`.
3. `ab8ba` `HSLPARDimController(dev, head, on = 1, level = 0)` ->
   `0x1888 + head*0x200 |= 0x82`, byte `[31:24]` = 0.
4. `ab8ce` enable register `&= ~1`.
5. If the transition is not "vid-parm free", the timing block, **all plain
   writes**, in this order (add `head*0x200`):

| # | reg | content |
|---|---|---|
| 1 | `0x1900` | bit 0 = interlace, everything else 0 |
| 2 | `0x1910` | `[11:0] = HTOTAL-2`, `[27:16] = HDISPEND-2` |
| 3 | `0x1908` | `[11:0] = HBLKSTR-2`, `[27:16] = HBLKEND-2` |
| 4 | `0x1914` | `[11:0] = HSYNCSTR-2`, `[27:16] = HSYNCEND-2` |
| 5 | `0x192c` | `[11:0] = VTOTAL-1`, `[27:16] = VDISPEND-1` |
| 6 | `0x1920` | `[11:0] = VBLKSTR-1`, `[27:16] = VBLKEND-1` |
| 7 | `0x1930` | `[11:0] = VSYNCSTR-1`, `[27:16] = VSYNCEND-1` |
| 8 | `0x1934` | interlace half-line, else 0 |
| 9 | `0x1918` | **RMW**: `[11:0] = VTOTAL-1`, `[15:12]` and `[31:16]` preserved |
| 10 | `0x1928` | **RMW**: `&= 0x7fffffff`, bit 31 = interlace (`abad8`..`abb04`) |
| 11 | `0x191c` | **absolute, not strided** - head A preload count |
| 12 | `0x1b1c` | **absolute** - head B preload count, written on every mode set |

6. `ab8ed` if the pitch word `p[0x3c] & 0x3fff` is non-zero: for DC 0/1
   (`ab9c1`) `0x1834 + head*0x200 = p[0x3c]`, then
   **`0x1800 + head*0x200 = p[0x38]`** (the DC_CTRL enable); for DC 2/3
   (`ab902`..`ab987`) instead `0x19d8`, `0x19e4`, `0x1928`, `0x190c`, `0x19a8`
   and **`0x1888 + head*0x200 = p[0x48]`**.
7. `ab98c` `HSLPARDimController(dev, head, on = 0, level = 0xff)` ->
   `0x1888 &= ~0x82`.  `HSLPARDimController` (`.text+0xab790`) writes the level
   byte only on the assert (`ab80d`), so the release is a pure `&= ~0x82` and
   the `0xff` the caller passes is discarded (PROVEN `ab845 andb $0x7d,0x4(%rsp)`
   with no level store).

Timing derivation, `HSLPARComputeIntermediateVidParm` (`.text+0xabe30`), is
identical to the 0528's - the only `0x2537`-family gate in it (`abfe6`) merely
skips an add-on guard-band branch.  Horizontal values are forced even and
biased `-2`, vertical biased `-1`, both masked to 12 bits.  **Our timing word
encodings agree with the vendor field for field**, exactly as on the 0528.
There is no `VDISPEND` errata on this family (the `+9` fixup at `acd6c` is
`devid == 0x0527 && rev <= 1`).

### 5.4 The DC_CTRL value - `0x00040201` is wrong, and bit 9 is the LUT bypass

**PROVEN, and this is a live defect on hardware that works today.**

`HSLPARGetDCParameters` (`.text+0xac770`) zeroes the whole parameter block
(`+0xac7ba`) and builds `p[0x38]`, the DC_CTRL word, as:

```
 acbbc  movl $0x0,0x38(%rbp)          the dword starts at zero
 acbd6  or  $0x1,%eax                 bit 0  = DC / primary-surface enable
 acbd9  and $0xffffffbf,%eax          bit 6  = 0
 acbdf  andw $0xfe7f,0x38(%rbp)       bits 7 and 8 = 0
 acbeb  and $0xfffffff7,%eax          bit 11 = 0
 acbee  test BYTE [rsp+0x87],0x4      mode flags bit 26
 acbfc    or $0x2,%eax                bit 9  = primary-LUT bypass
 acbff  test BYTE [rsp+0xb8],0x1      interlace
 acc0d    or $0x8,%eax                bit 11 = interlace
 acd6c  bit 9 |= 1 when devid == 0x527 && rev <= 1 && bpp field == 0xC0000
```

The function's **only** two device-id comparisons are the `0x527` tests at
`aca57` and `acd6c`.  So **for a plain progressive 8/15/16/32-bpp primary mode
on a P690 the vendor writes `0x1800 = 0x00000001` and nothing else.**

Bit 9 is independently proven to be the primary-LUT bypass, on this family
too, by `HSLPARSetPrimaryLUTBypass` (`.text+0xca0d0`):

```
 ca0f1  SelectDC / ca0f9 SelectDCModule
 ca14f  ebx = 0x1800 + (dcIndex << 9)
 ca158  read ebx
 ca15d  orb $0x2,0x5(%rsp)             <- byte 1 bit 1 = dword bit 9
 ca16b  write ebx
 ca196  (for DC 2/3 additionally: 0x19a8 + ... byte 1 |= 0x08 = bit 11)
```

with no `[dev+0x29e]` test anywhere in the function.  And bit 18 is proven to
be a sticky bandwidth/underflow flag by `HSLPARResetBandwidthState`
(`.text+0xcf540`), also ungated: it waits `0x4e20` us (20 ms) and then
**clears** byte 2 bit 2 (dword bit 18) of `0x1800`, `0x1a00`, `0x19a8` and
`0x1ba8` (`cf5c0`..`cf612`).  `HSLPARUpdatePriorityLevels` calls it at the end
of every mode set (`cf6c1`).

Our `PARHELIA_DC_CTRL_ON = 0x00040201` (`parhelia_drv.h:196`) therefore sets:

| bit | our value | vendor | consequence |
|---|---|---|---|
| 0 | 1 | 1 | correct - the enable |
| 9 | 1 | 0 | **the primary LUT is bypassed**, so `parhelia_pipe_load_gamma()`'s 256 entries never reach the output |
| 18 | 1 | 0 | we *assert* the sticky bandwidth-fault flag the vendor clears |

The header comment above the constant says it "stays as it is because that is
what has been driving a P690 display" - which is true and also why the bug is
invisible: bit 0 is what lights the screen, bit 9 only silently disables gamma,
and bit 18 is a status bit.  `0x00000001` is the value to use.

The surface word is unchanged from the 0528, PROVEN `+0xacc2b`..`+0xacc7b`:
`0x1834 = 0x00030000 | (fmt << 24) | (pitch & 0x3fff)` with `fmt` from
`GetSurfFormat` (`.text+0xac140`): 8 bpp `0x10`, 15 bpp `0x26`, 16 bpp `0x28`,
32 bpp `0x32`, `0x40000 -> 0x2e`, `0xB0000 -> 0x3b`, `0xC0000 -> 0x32`.  That
matches our `parhelia_dc_surface_code()` and our `| 0x00030000 | pitch`.
Pitch is in pixels with 32-pixel granularity (`HSLPARComputePitch
.text+0xbecc0`).

### 5.5 The scanout base register and its alignment

`HSLPAR_Register_PriSurfAddr_Set` (`mtx740 .text+0xc5810`):

```
 c5832  bit2 = (field != 0)
 c5847  sub   eax,0x2537
 c584c  cmp   eax,0x2
 c584f  jbe   c5862
 c5851  and   esi,0x7fff800        <- 0x0527 / 0x0528
 c5862  and   esi,0xffffc00        <- 0x2537 / 0x2538 / 0x2539
```

So on the P690 `0x1824` carries address bits **[27:10]** - mask `0x0ffffc00`,
256 MiB reach.  Our `parhelia_dc_start_mask()` uses exactly that for this
family.  **Correct.**

The byte address handed to it is pre-masked by
`HSLPARGetAddressAlignmentMask` (`.text+0xacdf0`), whose shift is 13 for 32 bpp
and the `0xC0000` 10-bit format, 12 for 16 bpp and 11 otherwise, **minus one
on LX/P690** (`ace43 sub eax,0x2537; ace48 cmp $0x3,%eax; ace4b sbb $0x0,%edx`):

| bpp field | 0x0527/0x0528 | **0x2539** |
|---|---|---|
| 32 (or `0xC0000`) | `~0x1fff` (8 KiB) | **`~0xfff` (4 KiB)** |
| 16 | `~0xfff` (4 KiB) | **`~0x7ff` (2 KiB)** |
| other | `~0x7ff` (2 KiB) | **`~0x3ff` (1 KiB)** |

Our `parhelia_scanout_address()` checks `IS_ALIGNED(*address, 1024)` for every
family, which is one to three shifts too weak here.  In practice our scanout
buffers are placed at MiB granularity, so this is latent, not live - but the
check should follow the format.

The window registers are unchanged: `HSLPARSetDisplayStartParameters`
(`.text+0xad170`) writes `0x1804[27:16]` = window X start,
`0x1808[27:16]` = X start + W - 1, `0x1848[11:0]` = Y start,
`0x184c[11:0]` = Y start + H - 1.  Our `DC_XPAN`/`DC_HDISP`/`DC_YPAN`/
`DC_VDISP` writes match for a full-screen unpanned window.

### 5.6 `DC+0x88`: the vendor's gesture is a dim assert/release, nothing else

`HSLPARDimController` (`.text+0xab790`), PROVEN, no device test:

```
 ab7cf  head must be 0 or 1
 ab7f8  reg = 0x1888 + head*0x200
 ab808  or  BYTE PTR [rsp+0x4],0x82    bits 1 and 7 = dim/fade enable
 ab80d  mov BYTE PTR [rsp+0x7],level   bits [31:24]
 ab845  and BYTE PTR [rsp+0x4],0x7d    release: clear bits 1 and 7 only
```

and bits 5:4 of `DC+0x88` are written in exactly two places, both restricted
to DC index >= 2 - the *second* (overlay) controller:
`HSLPARGetDCParameters+0x549` (`.text+0xacd03  or BYTE PTR [rbp+0x48],0x20`),
inside a block entered only after `test BYTE PTR [rsp+0x84],0x1` (mode flag
bit 0, `+0xacc7e`), whose word reaches `0x1888` only through the `p[0] >= 2`
branch of `HSLPARSetDCParameters` (`ab975`..`ab987`); and
`HSLPARSetDisableDisplayController+0x1e0` (`.text+0xabd2b  and BYTE PTR
[rsp+0x8],0xcf`), also DC >= 2 only.  There is no `0x2539` gate on
`0x1888`/`0x1a88` anywhere in the binary.

Our SUN90 path writes `params = (params & ~PARHELIA_DC_DIM_EN) |
PARHELIA_DC_VIDEO_EN`, i.e. `(v & ~0x0082) | 0x0030`.  Clearing `0x0082` is
the vendor's release and is right.  **Setting bits 5:4 is the second
controller's composite enable and the vendor never sets it from the primary
path.**  It has not hurt on hardware, but it is a write into fields that
belong to `0x19a8`/`0x1ba8`'s controller, and the honest form of the write is
`params & ~0x0082`.  The vendor also *asserts* `|= 0x82` before the timing
writes, which our driver never does - harmless for a POST that left the bits
clear, and the reason we already release them.

### 5.7 The vid reset

`HSLPARApplyVidReset` (`.text+0xafdc0`) contains no device-id test; its only
gates are add-on-module ones.  It is applied twice per mode set in principle -
from `HSLPAREnableDisplayController+0x103` (`b1233`) with this head, and from
`HSLPARSynchronizeDC+0x88` (`b21d8`) with `-1` - but the second call is
reached only when the other head's width, height and pixel clock match this
one, or when the genlock flag `dev[0x3ab] & 0x20` is set
(`b21ac`..`b21ce`).  **On a single-head driver `HSLPARSynchronizeDC` does
nothing**, and one reset per mode set is the vendor's behaviour.  PROVEN.

Sequence, head A shown (head B uses `0x1c04`/`0x1b00`):

```
park    b0410  read 0x1c00; only if bit 5 is currently set:
        b0439    edx = (edx & 0xfffffdfe) | 0x1c2   clear bits 0 and 9,
                                                    set bits 1, 6, 7, 8
        b0449    write 0x1c00
        b044e    clear bit 5; write 0x1c00; remember "bit 5 was set"
arm     b0380  0x1900 |= 0x00030000        head A timing-MISC reset bits 16,17
        b0350  0x1b00 |= 0x00030000        head B
        b0320  0x1c00 |= BIT(4)            route the reset to head A
        b02f0  0x1c04 |= BIT(4)            route it to head B
strobe  affb9  0x1c00 &= ~BIT(24)
        affcf  ClientWait(10)
        affd7  0x1c00 |=  BIT(24)
        afff2  0x1c00 &= ~BIT(24)
disarm  b0260  0x1c00 &= ~BIT(4) ; b0230  0x1c04 &= ~BIT(4)
        b0200  0x1900 &= ~0x00030000 ; b01d0  0x1b00 &= ~0x00030000
        b01a3  0x1c00 |= BIT(5) (only if it had been set) ; b0176  0x1c04 likewise
```

Our `parhelia_apply_vid_reset()` has the shape right (route bit 4, MISC bits
16/17, a `0 -> 10 us -> 1 -> 0` strobe on `0x1c00` bit 24) but it *clears* the
hold on both `0x1c00` and `0x1c04` unconditionally instead of parking
`0x1c00` with bits 1, 6, 7, 8 set and bits 0, 9 clear, and it never restores
the hold afterwards.  Identical deviation to the 0528's.

### 5.8 DEM request priorities: `0x0b0b` is the right *shape* and the wrong *level*

`0x1c0c` is head A's DEMREQCTL and `0x1c10` head B's; each carries two 3-bit
levels at `[2:0]` (the head's primary controller) and `[10:8]` (its second
controller).  `HSLPARSetPriorityLevels(dev, mask, pri[2], sec[2])`
(`.text+0xce500`) is a read-modify-write touching only those fields - PROVEN
field mapping:

| mask bit | register | field | value from |
|---|---|---|---|
| 0 | `0x1c0c` | `[2:0]` | `pri[0]` (head 0 primary) |
| 2 | `0x1c0c` | `[10:8]` | `sec[0]` (head 0 second) |
| 1 | `0x1c10` | `[2:0]` | `pri[1]` (head 1 primary) |
| 3 | `0x1c10` | `[10:8]` | `sec[1]` (head 1 second) |

(`ce5cf`/`ce5e8` for `0x1c0c` bits 0 and 2, `ce578`/`ce594` for `0x1c10` bits
1 and 3; the field masks are `and $0xfffffff8` and `and $0xf8,%dh`.)

Three writers reach it on a P690 mode set, in this order:

1. `HSLPARSwitchToHIRES+0x35b` (`.text+0xbe12b`) - `mask = 5`, both level
   arrays zeroed, i.e. **head A's two fields parked at 0** on the way in.
2. `HSLPARSetDefaultPriorityLevels` (`.text+0xad470`) - `mask = 0xf` and
   `{3,3},{3,3}`, i.e. **all four fields = 3**, at the end of
   `HSLPAREnableDisplayController`.
3. `HSLPARUpdatePriorityLevels` (`.text+0xcf630`), the **last call of
   `HSLPARSetMode`** - `HSLPARGetCurrentBitRates` (`.text+0xce690`) reads
   `0x1834 + head*0x200` bits `[29:24]` for each head's format and
   `[dev + head*0x80 + 0x30]` for its pixel clock, `HSLPARGetPriorityLevels`
   (`.text+0xce9e0`) turns those into levels, `HSLPARSetPriorityLevels(mask =
   0xf)` writes them, and `HSLPARResetBandwidthState` then clears DC_CTRL bit
   18 on all four controllers.

`HSLPARGetPriorityLevels` is worth decoding because it settles what the level
should be.  PROVEN (`ce9e0`..`ceba3`):

```
all four levels default to 1                                   cea38/cea40
maxRate = max over both heads of both bit-rate arrays          cea4b..cea68
for each head with rate != 0:
    q         = (maxRate * 100) / rate                         cea8c..cea91
    levelRaw  = 70000 / q                                      cea9e..ceaa1
    level     = levelRaw / 100 ; rem = levelRaw % 100
    if level == 0 -> level = 1                                 ceaca
    else if rem > 50 -> level++ , clamped to 7 at >= 8          ceb88..ceb9e
```

which reduces to `level = round(7 * rate / maxRate)`.  **For a single active
head, `rate == maxRate`, so `levelRaw = 700` and the level is 7**; the idle
head's fields stay at 1.

So the final vendor state for our configuration (head 1 primary active, no
overlay) is `0x1c10[2:0] = 7`, `0x1c10[10:8] = 1`, `0x1c0c` both fields 1,
plus the `0x0808` bits the S90 init ORed in - i.e. `0x1c10 = 0x0000090f`.

Our driver writes, in `parhelia_program_clock()`'s SUN90 branch,
`0x1c0c = 0x1c10 = 0x0b0b` as **plain** writes.  Decomposed, `0x0b0b` is
`[2:0] = 3`, bit 3, `[10:8] = 3`, bit 11 - which is precisely the union of the
S90 init's `0x0808` and `HSLPARSetDefaultPriorityLevels`' 3s.  So the sibling
document's finding that `0x0b0b` "sets bits 3 and 11, outside the two fields
the vendor ever writes" is true of the 0528 and **not** true here: on this
family the vendor sets bits 3 and 11 itself, at init.  Three things remain
wrong:

* the level is **3 where the vendor ends at 7** for the only active head;
* the write is **plain**, so any other live bit in those registers is lost;
* it writes **both** heads' registers, where the vendor leaves the idle head
  at level 1.

This is a bandwidth-headroom item rather than a first-light one - the vendor
itself runs the whole VGA->HIRES transition with head A at level 0 - but at
1920x1200x32 (154 MHz, ~617 MB/s of scanout) a low request level under 3-D
load is exactly the condition DC_CTRL bit 18 latches, and we are setting that
bit rather than clearing it (§5.4).

### 5.9 LUT, zoom and synchronise

`HSLPARApplyLUT` (`.text+0xcd070`) performs no MMIO - it manipulates the
per-DC shadow at `dev + 0x3e0 + dc*0x808` and calls `HSLPARSetPrimaryLUT`
unless mode flag bit 20 is set.

`HSLPARSetPrimaryLUT` (`.text+0xcc2a0`), PROVEN and ungated:

* DC 0/1 (`ccc19`..`ccc2c`): the quad `0x1838` (index), `0x183c` (R),
  `0x1840` (G), `0x1844` (B), `+ head*0x200`, components masked **`0xffc0`**;
* DC 2/3 (`cc33b`..`cc34e`): `0x19b8`..`0x19c4`, masked `0xff00`;
* the write loop (`ccc95`..`cccf7`) writes the **index register once**, as 0,
  then 256 iterations of R, G, `B | BIT(31)` - the index auto-increments.

Our `parhelia_pipe_load_gamma()` uses the right registers and the right
`0xffc0` mask and rewrites the index every iteration (harmless).  What is not
vendor behaviour is the pair of byte writes bracketing the loop:
`parhelia_write8(pdev, dc + PARHELIA_DC_LUT_GATE, 0x08)` and `... 0x00`, with
`PARHELIA_DC_LUT_GATE = 0x1aa`.  At `dc = 0x1800` that address is `0x19aa`,
byte 2 of `0x19a8`; at `dc = 0x1a00` it is `0x1baa`, byte 2 of `0x1ba8`.  Both
are the **second controller's DC_CTRL**, and `0x08` in byte 2 is bit 19 of it.
No vendor path writes a gate byte around the LUT.  The write is attributed in
our comment to "the VBIOS sequence", so it may be a legacy VGA-palette gesture
mis-strided into the native block; either way it should not be aimed at
`0x19a8`/`0x1ba8`.

`HSLPARSetModeZoom` (`.text+0xce0b0`) touches `0x1900 + (SelectDC() << 9)`
bits `[12:11]` (H zoom) and `[14:13]` (V zoom), writing only when the code
changes.  Our driver never zooms and writes `0x1900 = 0` (`DCT_MISC`), which
clears both fields - consistent.

`HSLPARGetCrtcPreloadCount` (`.text+0xac5c0`) returns zero for both heads
unless the genlock flag is clear, the other head is in use, the two heads'
pixel formats differ *and* the other head's pixel clock exceeds `0x5572f` kHz
(350 MHz).  On a single-head driver the counts are always 0, but
`HSLPARSetDCParameters` writes `0x191c` **and** `0x1b1c` unconditionally from
the zeroed block, so the vendor explicitly writes 0 to both on every mode set
rather than inheriting POST values.  Our driver writes neither.

## 6. The integrated serializer (DVO/TMDS) stage

### 6.1 `0x1c20` is the crossbar on the P690 too - the sibling's family table is wrong on that row

**PROVEN by writers.**  `0x1c20` has six write sites in the binary: `b09d9`,
`b0a97` and `b0ec6` inside `HSLPAREnableDVO`, plus `d8746`
(`HSLPAREnableDVO1`) and `d849e`/`d8585` (`HSLPAREnableDVO2`).  None of them is
device-gated away on `0x2539`.  `0x1c54` is an **additional** register the
LX/P690 path writes, and it mirrors DVO2's control byte `[31:24]` - it is not a
replacement for `0x1c20`.  The sibling document's row "Integrated DVO
format/crossbar: `0x1c20` (original) versus `0x1c54`/`0x1c58` (LX/P690)" should
read: `0x1c20` on all five parts, plus a `0x1c54` byte-3 mirror on LX/P690;
`0x1c58` carries only bit 6 on the mode path.

Register map for `0x2539`, PROVEN:

| | DVO1 | DVO2 |
|---|---|---|
| control byte in `0x1c20` | `[15:8]` | `[31:24]`, **mirrored in `0x1c54[31:24]`** |
| data/format enable pair | bits 10, 12 | bits 26, 28 |
| HSYNC-neg / VSYNC-neg | bits 11 / 13 | bits 27 / 29 |
| head / DC-module select | 3-bit `{bit15, bit14, bit8}` | `0x1c20` bit 30 **and** `0x1c54` bit 30 |
| dual link | - | **`0x1c20` bit 25** |
| clock select | `0x1c00[14:13]` + bit 27 | `0x1c04[17:15]` |
| clock gate (1 = gated) | `0x1c00` bit 12 | `0x1c04` bit 14 |
| power (active low) | `0x1c24` bit 0 | `0x1c24` bit 1 |
| common block power | `0x1c24` bit 2 | `0x1c24` bit 2 |
| output gate (1 = gated) | - | `0x1c58` bit 6 |

The DVO1 module select being **three** bits on LX/P690 is a real family
difference: `HSLPAREnableDVO` encodes module 0/1/2/3 as `{bit15,bit14,bit8}` =
`000`/`001`/`011`/`100` (`b101b`, `b104e`, `b1074`, `b110b`), where the original
family uses the two bits `[15:14]` with module 2 raising `0xb1e03131`
(`b100d`/`b1036`/`b1066`).  `HSLPAREnableDVO1+0x98` (`d8738 and
$0xfffffffe`) clears dword bit 8 on the LX/P690 path, which the 0528 never
touches.

### 6.2 The board-class flag, and why it is 1 on this board

`HSLPAREnableDVO` (`.text+0xb04f0`), signature
`(dev, dvoIdx /*0 = DVO1, 1 = DVO2*/, vidparm, clkArg)`.  Its first act is to
compute a board-class flag, PROVEN `+0xb05b5`..`+0xb08cb`:

```
 b05b5  flag = 1
 b05bd  devid - 0x527 <= 1                                   -> flag = 0
 b0820  devid == 0x2537 && dev[0x2a0] == 1                    -> flag = 0
 b082c  dev[0x4a9c] == 4 (add-on type)                        -> flag = 0
 b0839  PInS[0x90] in {9,2} && (PInS[0x94] & 0x10)
        && !(PInS[0x95..0x97] & 0xda) && !(PInS[0x91..0x93] & 0x0a)  -> flag = 0
        else                                                  -> flag = 1
```

That is the **same** board test as the `0x1c58` value in
`HSLS90InitializeHardware+0x81c` and `HSLPARSwitchToHIRES+0xa88`, and the same
test appears again in `HSLPARSetDVOClock+0x5c` (`aef5c`).  **On this board
`PInS[0x90] = 0x42` so the test fails and flag = 1**, which means: `0x1c58`
bit 6 is *cleared* after a DVO2 enable, `0x1c54` is written, and DVO2's own
clock select `0x1c04[17:15]` is programmed.  A flag = 0 board would instead get
`0x1c58 = 0x40` and would route DVO2 through DVO1's clock.

### 6.3 The ordered LX/P690 `HSLPAREnableDVO` body for DVO2

PROVEN, for `dvoIdx = 1`, flag = 1, not already locked, not a smooth
transition:

| # | offset | action |
|---|---|---|
| 1 | `b05a9` | `HSLPARIsDvoLocked(dev, vidparm, &locked)` - reads `0x1c00`, `0x1c24`, `0x1c20` |
| 2 | `b05df` | `HSLPARDetectSmoothTransition` |
| 3 | `b0601` | `0x1c04 \|= BIT(14)` - gate DVO2's clock |
| 4 | `b0ba0` | `0x1c24 \|= BIT(1)` - power DVO2 down |
| 5 | `b0bd7` | `0x1c04 \|= BIT(14)` again |
| 6 | `b0f2c` | `HSLPARSetDVOClock(dev, 1, clkArg, 0)` |
| 7 | `b0f44` / `b10bc` | **the driving controller's DCM clock select is forced to 6**: on `0x1c00` when `SelectDC() == 0`, on `0x1c04` otherwise - `\|= BIT(5)` (hold), write, then `(v & 0xfc3f) \| 0x180` i.e. `[9:6] = 6`, clear `BIT(5)`, write |
| 8 | `b06c4`..`b0730` | **`0x1c24 = (PInS[0x190] & 0xfffffff8) \| (live 0x1c24 & 7)`**, then **`0x1c70 = PInS[0x194]`** |
| 9 | `b0735` | read `0x1c20` |
| 10 | `b07c8` | `HSLPARGetFpgaMode(...)` - LX/P690 always take this branch (`b0754 sub $0x2537; cmp $0x2; jbe b07b0`) |
| 11 | `b0c71`..`b0c9e` | `0x1c20[31:24]`: clear bits 27 and 29, set them from `vidparm[0x48]` bits 2 and 3, **clear bits 26 and 28** (`and $0xffffffeb`) unless the `PInS[0x95] & 0x08` + `dev[0x3a7] & 0x02` gate at `b0fa7` passes |
| 12 | `b0cab`..`b0cf8` | **`0x1c54[31:24]` gets the identical treatment and is written** (flag != 0 only) |
| 13 | `b0d58`/`b0d75` | dual link: if `dev[0x3b4] & 0x20` and `dev[0x3d0] == 2` and `vidparm[0x24] > 0x28488` (165 000 kHz) and the digital link flag is present, **`0x1c20 \|= BIT(25)`**; else `b0989` clears it |
| 14 | `b09d5` | **write `0x1c20`** |
| 15 | `b0e8d`..`b0ec6` | re-read `0x1c20`; `SelectDC() == 0` -> clear `BIT(30)`, `== 1` -> set `BIT(30)`, else `SetError(0xb1e02131)`; **write `0x1c20`** |
| 16 | `b0edf`..`b0f17` | re-read `0x1c54`; the same `BIT(30)` select; **write `0x1c54`** |
| 17 | `b0ac1` | `0x1c04 &= ~BIT(14)` - ungate DVO2's clock |
| 18 | `b0aec` | `0x1c24 &= ~(BIT(1) \| BIT(2))` - power DVO2 and the common block up |
| 19 | `b0b2f`/`b0b5f` | **`0x1c58 &= ~BIT(6)`** (flag != 0 and not locked) |
| 20 | `b0b8d` | `HSLPARApplyVidReset(dev, SelectDCModule())` |

`dvoIdx = 0` differs only in using `0x1c00 |= BIT(12)` / `0x1c24 |= BIT(0)` for
the gate and power, operating on `0x1c20[15:8]`, omitting steps 12 and 16, and
replacing step 15 with the three-bit `{bit15,bit14,bit8}` module encode at
`b0a04`..`b0a93`.

Differences from the 0x0527/0x0528 branch, PROVEN:

| | 0x0527/0x0528 | 0x2539 |
|---|---|---|
| PInS board-config word goes to | **`0x1c54 = PInS[0x190]`** (`b0c00`), or `0x1c54 = 0x1f` when PInS <= `0x605` | **`0x1c24 = (PInS[0x190] & ~7) \| (live & 7)`** (`b06c4`) |
| `0x1c70` | `PInS[0x194]` (0 when PInS <= `0x605`) | `PInS[0x194]` |
| sync-polarity source | `vidparm[0x48]` directly, into `0x1c20` only | via `HSLPARGetFpgaMode`, into `0x1c20` **and** `0x1c54` |
| data/format enables | **set** `or $0x14` when `dev[0x3a7]` bit 0/1 | **cleared** unless the `PInS[0x95] & 0x08` gate passes - and it does not on this board (`PInS[0x95] = 0x07`) |
| DVO1 module select | 2 bits `[15:14]` | 3 bits `{15,14,8}` |
| DVO2 module select | `0x1c20` bit 30 | `0x1c20` bit 30 **and** `0x1c54` bit 30 |
| `0x1c58` bit 6 after enable | set | **cleared** |
| DVO2's own clock select | never programmed | `0x1c04[17:15]` |

### 6.4 `HSLPARSetDVOClock`, and the `0x00030000` our driver inherited from a mis-transcription

`HSLPARSetDVOClock` (`.text+0xaef00`) remaps `clkArg` 0..9 through
`.rodata 0x111754` and then, PROVEN `+0xaf010`..`+0xaf1e2`:

* DVO1 (or DVO2 on a flag = 0 board): if `0x1c00` bit 12 is clear, set it and
  write first; `0x1c00 = (v & 0xf7ff9fff) | ((c & 3) << 13) |
  (((c >> 2) & 1) << 27)`; ungate by clearing bit 12.
* DVO2 on a flag = 1 board: if `0x1c04` bit 14 is clear, set it and write;
  `0x1c04 = (v & 0xfffc7fff) | ((c & 7) << 15)`; ungate by clearing bit 14.

The standalone helper `HSLPAREnableDVO2` (`.text+0xd83b0`) is off the mode-set
path (§6.5) but it is the routine our own DDX translation was taken from, so its
exact clock block matters.  PROVEN `d85c3`..`d8664`, with `r13d` = the clock
argument taken as **signed**:

```
 d85d0  read 0x1c04
 d85d5  if (clk >= 0):  0x1c04 = (v & 0xfffffc3f) | ((clk & 0xf) << 6)    [9:6] = clk
 d8686  if (clk <  0):  0x1c04 = (v & 0xfffffc3f)                        [9:6] = 0
        write 0x1c04
 d8611  read 0x1c04
 d866f  if (clk >= 0):  0x1c04 = (v & 0xfffc7fff) | ((clk & 7) << 15)   [17:15] = clk
 d861b  if (clk <  0):  0x1c04 = (v & 0xfffc7fff) | 0x30000             [17:15] = 6
        write 0x1c04
 d864e  0x1c04 &= ~BIT(14)                                              ungate
        write 0x1c04
```

**The `| 0x00030000` and the `[17:15] = clk` insertion are mutually exclusive
branches of the same field.**  `0x30000 >> 15 = 6`, so the OR *is* the
"clk < 0" way of writing the value 6 into `[17:15]`.

`docs/parhelia-kms-p690-dvi1-fix.md` transcribed this as four sequential steps -
`0x1c04[9:6] = clk & 0xf`, `0x1c04[17:15] = clk & 7`, `0x1c04 |= 0x00030000`,
`0x1c04 &= ~BIT(14)` - and our kernel implements that transcription:

```c
v = (v & ~(0xfu << 6))  | ((head ? 4u : 3u) << 6);
v = (v & ~(7u << 15))   | ((head ? 4u : 3u) << 15);
v |= 0x00030000u;
```

which lands on `[17:15] = 4 | 6 = 6` for head 1 and `3 | 6 = 7` for head 0.
**Code 7 is never written into that field by any vendor path**, and code 6 is
the vendor's "no explicit clock" fallback rather than FS3.  Our head-1 value
happens to be a legal code, which is presumably why 1920x1200 works (HW), but
the field should carry `clk & 7` - 3 for FS2, 4 for FS3 - and the `0x00030000`
OR should not be there.

### 6.5 The standalone DVO helpers are not on the mode-set path

PROVEN by exhaustive call-site enumeration: `HSLPAREnableDVOs` (`.text+0xd87d0`)
has exactly one caller, `HSLPARReturnToVga+0x3e4` (`d9ef4`), and
`HSLPAREnableDVO1`/`HSLPAREnableDVO2` have exactly one caller each -
`HSLPAREnableDVOs`.  **The only mode-set DVO writer is `HSLPAREnableDVO`**,
reached from `HSLPAREnableIntermediateModule+0xfe` (`b1ede`, DVO2) and `+0x203`
(`b1fe3`, DVO1).  `HSLPAREnableIntermediateModule` (`.text+0xb1de0`) performs no
MMIO of its own; it only dispatches on the link flags.

`HSLPARDisableDVO2` (`.text+0xd7380`) LX/P690 branch (`+0xa8`):
`0x1c58 &= ~BIT(6)`, `0x1c04 |= BIT(14)`, `0x1c24 |= 0x6`.  The original family
instead sets `0x1c00 |= BIT(12)`.

`HSLPARIsDvoLocked` (`.text+0xaec70`) treats a DVO as up when its `0x1c24`
power bit is clear **and** `0x1c00` bit 12 is clear - it tests bit 12 for both
serializers (`aed0d`/`aed20`, INFERRED a vendor inconsistency, since DVO2's gate
is `0x1c04` bit 14) - and then compares `0x1c20`'s module select against the
target DC block.

### 6.6 `0x08f4` is not an output gate; it is `ClientWait`'s dummy read

**PROVEN, and this retires a register from our driver.**  The string `0x8f4`
appears **exactly once** in the whole disassembly, at `ClientWait+0x1b`:

```
0000000000004a9a0 <ClientWait>:
 4a9a2  eax = 3 * n
 4a9b6  loop: ClientReadRegisterDword(dev, 0x8f4, &scratch)
 4a9cc  until the counter reaches 3*n
```

`0x08f4` is never written, on any family, by either the mode-set path or the
init path.  What the init paths *do* write is `0x08fc = 0`
(`HSLS90InitializeHardware+0x142`, `dd902`), a different register that our
`parhelia_engine_init()` already writes.  Our
`PARHELIA_REG_OUTPUT_GATE = 0x08f4` / `PARHELIA_OUTPUT_GATE_TMDS = BIT(5)`
clear in `parhelia_program_output()` therefore corresponds to nothing in the
vendor driver.  It was attributed in our comment to "the working P690 VBE
state"; if it is load-bearing, its provenance is the VBIOS and not the vendor
HAL, and it should say so.

### 6.7 Dual link above 165 MHz

Two independent mechanisms, both keyed on `0x28488` = 165 000 kHz:

* **integrated**: `0x1c20` **bit 25** (`b0d58`/`b0d75`), gated on
  `dev[0x3b4] & 0x20` and `dev[0x3d0] == 2` and the digital link flag;
* **external**: `HSLPAREnableDVI+0x1b5` (`b5f25`) pairs two SiI transmitters on
  the same I2C port at different slave addresses and calls `HSLSI164Enable` plus
  `HSLSI164EnableDualLinkMode` on both.

`HSLPARApplyModeInfoModifications` adds the *second* digital link flag to the
mode when the clock exceeds the same 165 000 kHz and the board declares it
(`d068e cmpl $0x28488,0x24(%r12)`, `d06a9 or $0x40000000,%edi`).

Our driver puts its dual-link bit in **`0x1c54` bit 25**, not `0x1c20` bit 25.
Irrelevant at 1920x1200 (154 MHz), wrong above 165 MHz.

### 6.8 External SiI164 versus the integrated serializer

**PROVEN: the external transmitter is selected by a runtime I2C probe, not by
the chip and not by a PInS "external" flag.**

* `HSLPARDetectSiliconImage` (`.text+0xc3a90`) probes slave addresses `0x70`,
  `0x76`, `0x72` (`c3a94`..`c3aa1`) on I2C port list `{1, 0x10}` (`c3ab5`,
  `c3aba`, `c3abf`), reading transmitter registers 0, 2, 6, 7 to build an id.
  Each hit is recorded at `dev + 0x41d0 + n*0x10` and sets bits in
  `dev[0x3a4]`.
* **`HSLPAREnableDVI` (`.text+0xb5d70`) does nothing at all unless
  `dev[0x3a4] & 0x6000`** - the decisive gate is `b5e50 and $0x60,%dh; jne
  b5ec6`.  It is called once per output from `HSLPAREnableDisplayOutputs`
  (`b715e`, `b732b`, `b744f`, `b752e`).
* `HSLPARI2COpenPort` gates ports 1 and `0x10` on `PInS[0x88]` bits 2 and 3.
  This board's `PInS[0x88] = 0x000488b4`: bit 2 set, bit 3 clear, so **port 1 is
  enabled and port `0x10` is not**, and the probe does run on the `0x1c4c`
  MISC-IO I2C bus.
* `HSLSI164Enable`'s **only** `0x2539` behaviour (`db734`): if
  `BYTE [dev+0x3d0b] != 0` it **sets** bit 1 in the control byte of the
  `Si164Param`/`Si1162Param`/`Si1178Param` blocks (`db7b5`..`db7d1`), where every
  other device clears it (`db746`..`db762`).  Those blocks are then written over
  I2C to transmitter registers `0x08`/`0x0a`/`0x0c`.

So on a board that carries an external transmitter the vendor drives it *in
addition to* whatever the integrated stage does; the two are not exclusive.  We
have no evidence of an external transmitter on the fitted card and our driver
does not probe for one.

## 7. The analog DACs, load sense and the DAC-versus-TMDS decision

### 7.1 The DAC block is bit-for-bit identical on the P690

**PROVEN and stronger than the sibling stated.**  `HSLPAREnableDisplayOutputs`
(`.text+0xb6f40`, 914 instructions) contains **zero** `movzwl 0x29e` device-id
reads, and neither do `HSLPAREnableDAC1` (`.text+0xd6a60`), `HSLPAREnableDAC2`
(`+0xd6ad0`), `HSLPARDisableDAC1` (`+0xd6940`), `HSLPARDisableDAC2`
(`+0xd69b0`), `HSLPARDisableDACs` (`+0xd6a20`), `HSLPAREnableDACs` (`+0xd6b40`),
`HSLPARSetDAC1OnDCMA` (`+0xd6610`), `HSLPARSetDAC2OnDCMA` (`+0xd6680`) or
`HSLPAREnableDacSyncOnly` (`+0xae120`).

So the sibling's field table carries over unchanged: **DAC1 = `0x1c18`,
DAC2 = `0x1c1c`**, `[8:0]` three 3-bit per-channel trims, bit 9 TV/composite
sync (cleared unconditionally for a monitor), bits 10 and 12 master power
(active low), bit 11 HSYNC negative, bit 13 VSYNC negative, DAC1 bit 14 the
1-bit module select, DAC2 `[15:14]` the 2-bit module select (00 / 01 / 11 for
module 0 / 1 / 3, module 2 raising `0xb1e03131`), DAC1 bits 15+16 and DAC2 bits
16+17 the RGB video power.  Reconfirmed at `b75c7`..`b768b` (DAC1) and
`b76c5`..`b7788` (DAC2).  `PARHELIA_DAC1_PWR = 0x00019400` and
`PARHELIA_DAC2_PWR = 0x00031400` remain exactly what the enable/disable pairs
clear and set.

Two fields our driver still does not write on either family: **bit 9**, which
the vendor clears unconditionally for a monitor output on both DACs, and the
`[8:0]` trims.  For this board `PInS[0x174] = 0x07fc9249`, and the extraction
(`b7a4f`..`b7a66` for DAC1, `b79ff`..`b7a1c` for DAC2's monitor case) gives
`[8:6] = [5:3] = [2:0] = 001` for both, i.e.

```
0x1c18[8:0] = 0x049      0x1c1c[8:0] = 0x049
```

(a uniform 1 in every monitor trim field; the TV fields are all `111`).

`HSLPARAdjustDAC1SyncForChrontel` (`.text+0xd8ea0`) *does* have an LX/P690
branch (`d8ef8`: `0x1c18 &= ~(BIT(10)|BIT(12))` instead of calling
`HSLPAREnableDAC1`), but its sole caller chain is
`HSLPARAdjustDACSyncForChrontel+0xb3 <- HSLPARReturnToVga+0x556`, so it never
fires on a mode set.

### 7.2 `HSLPAREnableDisplayOutputs`, ordered

No device-id test anywhere in the function.  `SelectDC()` at `b6f60` supplies
the module index.

**Analog output** (DAC1 shown; the DAC2 branch at `b76b3` is symmetric with
`[17:15]`, `[14:12]`, `[11:9]` of `PInS[0x174]` and bits 16/17 for power):

1. `b75b5` read `0x1c18`;
2. `b75c7`..`b75e3` clear bits 9, 11, 13; set bit 11 when
   `!(vidparm[0x48] & 4)` and bit 13 when `!(vidparm[0x48] & 8)`; clear bits
   10 and 12;
3. `b75f6` **write `0x1c18`**;
4. `b75fb` trim source: `dev[0x425c/0x4260/0x4264]` when `dev[0x3b7] & 0x08`,
   else `PInS[0x174]` when `PInS[0x04] > 0x603` (`b7a34`);
5. `b761d`..`b7647` insert the three 3-bit fields into `[8:0]`; `b764f` clear
   bit 16;
6. `b7653`..`b7a25` module select: 0 -> `[15:14] = 00`, 1 -> `01`, else
   `SetError(0xb1e03131)`;
7. `b768b` **write `0x1c18`** a second time.

**Digital output**: for each output 0..3 the function checks the digital link
flag and the SiI-present words `dev[0x41d8]/[0x41e8]/[0x41f8]/[0x4208]` and
calls `HSLPAREnableDVI(dev, dviIdx, vidparm)` (`b715e`/`b732b`/`b744f`/`b752e`)
- which, as §6.8 shows, is a no-op without an external transmitter.  The
integrated serializer is **not** touched here; it was already programmed by
`HSLPAREnableIntermediateModule` earlier in `HSLPARSetMode` (`d136e` before
`d1384`).

### 7.3 Load sense - unchanged, and our implementation matches

`HSLPARSenseLoadOnInternalDAC` (`.text+0xc0e00`) has no device gate.  Second
argument is the analog output index; `sel = (index != 0)` selects both the link
flag (`0x01000000` for DAC1, `0x08000000` for DAC2) and the `sel * 0x200` stride
into the test registers:

```
 c10f8  0x1c18 |= 0x1400                 bits 10, 12: master DAC power off
 c111f  save 0x188c + sel*0x200
 c1130  write 0x188c + sel*0x200 = 0x16459164
 c1138  save 0x1910 + sel*0x200
 c1171  write 0x1910 + sel*0x200 = saved & 0xf000ffff
 c1180  ClientWait(10000)                10 ms
 c1192  read 0x1c14                      comparator result
 c11c1  restore 0x1c18, 0x1c1c, 0x188c+, 0x1910+
```

Our `parhelia_output_load_sense()` matches this exactly - the `sel * 0x200`
offset, the `0x16459164` value, the `0xf000ffff` mask and the 10 ms settle.
Same caveat as on the 0528: at `+0xc0ef8` the vendor branches on whether
`0x1c08` bit 0 (VGA) is still set, so a sense taken before the VGA->HIRES
switch is not the same measurement as one taken after.

The useful diagnostic split also carries over: `HSLPAREnableDacSyncOnly`
(`.text+0xae120`) powers only the RGB converters down (DAC1 bits 15+16, DAC2
bits 16+17) and leaves H/V sync running, so a monitor in that state reports a
**black picture**, not NO SIGNAL.

### 7.4 How the vendor decides DAC versus TMDS - it is EDID, with a board AND

The per-output **type** field lives at `dev + 0x3a14 + idx*0x4c`, offset `+4`,
and is produced by `HSLPARGetDetectedOutputType` (`.text+0xe2870`), the first of
nine resolvers run per output by `HSLPARGetDefaultAllHwOutputInfo`
(`.text+0xe38a0`, four `0x4c`-byte entries at `dev+0x3a14`).  PROVEN:

```
 eax = DWORD [dev + 0x2464 + idx*0x548]         the per-output monitor-info word
 if (eax & 0x08)                   type = 1     DIGITAL
 else if ([dev+0x39f8+idx*8] & 8)  type = 4
 else if (eax & 0x10)              type = 3
 else if (eax & 0x01)              type = 2     ANALOG
 else                              type = 0
```

The decisive bit 3 is set by `HALPGetMonitorProperties` from **the EDID's own
digital-input flag** - `93b69 cmpb $0x0,0x14(%rbx); js 93d5a`, i.e. **EDID byte
`0x14` bit 7** (the EDID 2.0 variant is the same gate at `93c6c`..`93c97`) - and
then only if the board declares a digital output at all:
`93d61 test $0x200004,%eax` / `93d68 test $0x1,%ah` / `93d6d test $0x2,%ah` on
`dev[0x3b4]`, i.e. `dev[0x3b4] & 0x00200304`.  Monitor-info bit 1 is "EDID
valid" and bit 0 "analog load sensed"; bit 0's only writer,
`HSLDACSenceMonitorLoad+0x180` (`.text+0xa0ff0`), **has no caller** in this
binary (it belongs to the dead G-series `HSLDAC*` layer).

`HSLPARGetDACIndex` (`.text+0xe2a50`) then assigns a DAC only for `type == 2`
(or `3` on output 1) and `HSLPARGetDVOIndex` (`.text+0xe2b80`) a DVO only for
`type == 1`; each defaults its field to `-1` (`e2b03`, `e2bb8`).

**So the answer to "confirm or refute our detect() rule" is: the EDID half is
confirmed, the fallback half is refuted.**

* Our `output->edid_digital` from the EDID's digital-input flag is exactly the
  vendor's rule.  **Confirmed.**
* Our no-EDID fallback `edid_digital = !analog_sensed && connector_type !=
  VGA`, i.e. "digital unless the connector is VGA", is the **opposite** of the
  vendor's.  `HSLPARGetNotRespondingMonitorInfo` (`.text+0xd58a0`) builds the
  mode type through `HSLPARBuildOutputModeType` (`d599a`) and
  `HSLEnableSyncOnOutput` (`d5a4c`), and `HSLPARSenseLoadOnInternalDAC` is the
  only sense primitive; **nothing promotes a non-responding output to digital.**
  A DVI-I connector with no EDID is analog as far as the vendor is concerned.
* The vendor additionally **ANDs in a board capability** we do not consult: an
  output the PInS does not declare digital can never become digital however the
  EDID reads.

Link-flag encoding, reconfirmed for completeness
(`HSLPARSetOutputAnalogModeType .text+0xd4210`,
`HSLPARBuildOutputModeType .text+0xd4280`, tests at `d4331`, `d4373`, `d43b2`,
`d43f4`):

| flag | output |
|---|---|
| `0x01000000` | analog, DAC1 |
| `0x08000000` | analog, DAC2 |
| `0x04000000` | TV on DAC2 |
| `0x00000002` / `0x00000004` | analog outputs 3 and 4 |
| `0x20000000` / `0x40000000` / `0x80000000` / `0x00000100` | digital outputs 0..3 |

## 8. Per-output mapping on this board

### 8.1 What the vendor's resolvers say

Three resolvers, all with no device-id test and no PInS involvement on a plain
board (`dev[0x4a98] == 0`, which holds here because `PInS[0x1a1] = 0x14`
matches none of `HSLPARDetectCEM`'s constants).  PROVEN:

| resolver | output 0 | output 1 |
|---|---|---|
| `HSLPARGetControllerIndex` (`.text+0xe3620`), plain board with <= 2 usable outputs (`e37a3 cmp $0x2,%r9d; ja e37c2`) | controller **0** (`e37b4`) | controller **1** (`e379a`) |
| `HSLPARGetDACIndex` (`.text+0xe2a50`) | **DAC1** `0x1c18` (`e2b32`) | **DAC2** `0x1c1c` (`e2b6c`) |
| `HSLPARGetDVOIndex` (`.text+0xe2b80`) | **DVO1** (`e2dc4 setne %al` on `idx != 0`) | **DVO2** |

and the output index is tied to a physical connector through DDC:
`HSLPARGetMonitorEDID` (`.text+0xd36f0`) maps output 0 -> port 0 -> DDC line 1
and output 1 -> port 5 -> DDC line 2, which on this board's pin table resolve to
GPIO SCL bit 1 / SDA bit 0 and SCL bit 3 / SDA bit 2 respectively (§10.1).

Our kernel's `parhelia_bios_ddc_pins()` candidate lists
(`parhelia_bios.c:128`) are **literally the vendor's** `DDCLINE1` and
`DDCLINE2` id pair lists, so our `ddc_line 0` *is* vendor output 0 and our
`ddc_line 1` *is* vendor output 1.  That fixes the correspondence with no
ambiguity.

| | vendor resolver | `parhelia_p690_outputs[]` |
|---|---|---|
| index 0 - DDC line 1, GPIO SCL 1 / SDA 0 | controller 0, **DVO1**, DAC1 | head 1, **DVO2**, DAC1 |
| index 1 - DDC line 2, GPIO SCL 3 / SDA 2 | controller 1, **DVO2**, DAC2 | head 0, **DVO1**, DAC2 |

### 8.2 Which of the three is board wiring, and which is not

* **The DAC index is board wiring** and our table agrees with the vendor.
* **The controller index is not wiring at all.**  It is an allocation:
  `HSLPARGetControllerIndex` hands controller 0 to the first output and 1 to the
  second, and the actual routing is the module-select field inside the DAC or
  DVO register, which can point either output at either controller.  Our
  `preferred_head` being the mirror of the vendor's default allocation is
  therefore not a defect - it is a different, equally legal choice, and it is the
  one that has been driving a display (HW,
  `docs/parhelia-kms-p690-dvi1-fix.md`: controller B / head 1, FS3, live at
  1920x1200).
* **The DVO index is board wiring, and here our table and the vendor's resolver
  disagree.**  Hardware says the connector that answers EDID on DDC line 1 is
  lit through the serializer whose control byte is `0x1c20[31:24]` mirrored in
  `0x1c54` - that is vendor **DVO2** - at 1920x1200 (HW, same document, live
  readback `0x1c54 = 0x28000000`, `0x1c24 = 0xaac00001`, `0x1c70 = 0x10`).  The
  vendor's resolver would pair that connector with DVO1.

### 8.3 Why the disagreement cannot be settled statically, and the honest reading

The vendor's DVO index is only ever assigned to an output whose type field is 1
(digital), and the type field requires `dev[0x3b4] & 0x00200304` (§7.4).
`dev[0x3b4]` is built by `HSLPARGetAddOnInfo` (`.text+0xc3f70`) from
`PInS[0x94]`/`PInS[0x95]` **AND-ed with corroborating conditions**.  PROVEN for
the digital bits:

```
 c41ec  PInS[0x95] bit 1 -> dev[0x3b4] |= 0x200000   (digital output 1)
        but only if (dev[0x3a4] & 0x02000000) or (PInS[0x88] & 0x4000)
 c41b8  PInS[0x95] bit 5 -> |= 0x10000002
        but only if (dev[0x3a4] & BIT(31)) or (PInS[0x88] & 0x0400)
 c419c  PInS[0x95] bit 0 or bit 3 -> |= 0x2          (analog output 2)
 c4276  PInS[0x95] bit 2 -> |= 0x8                   (TV)
```

This board has `PInS[0x95] = 0x07` (bits 0, 1, 2) and `PInS[0x88] =
0x000488b4`, whose bits 10 and 14 are both **clear**.  So the digital
capability bit for output 1 is produced only if `dev[0x3a4]` bit 25 is set at
runtime, and `dev[0x3a4]`'s runtime bits come from the add-on and
external-transmitter probes.  **INFERRED: with the PInS in `tools/p690.rom`
alone, the vendor driver would declare no digital output and would drive both
connectors from the DACs.**  That makes its static DVO-index resolver
unreachable on this ROM and therefore not an authority on which serializer is
wired to which connector.

Two further reasons not to "fix" our table on the strength of the resolver
alone: `tools/p690.rom` identifies itself as a **"Matrox P650/P750 VGA/VBE
BIOS"** with board name `P69-MDDE128F`, and it is not established that it is the
ROM of the card actually fitted (our kernel's own comment records subsystem
`102b:0040`); and the hardware result is a live 1920x1200 picture, which is
stronger evidence about physical wiring than a policy resolver that never runs.

**Recommendation: leave `parhelia_p690_outputs[0]` as it is (head 1, DVO2,
DAC1) and record the vendor's contrary default in a comment, with the one-line
experiment that would settle it** - program the *other* serializer (DVO1:
`0x1c20[15:8]`, module select `{bit15,bit14,bit8}`, clock `0x1c00[14:13]`+bit 27
gated by bit 12, power `0x1c24` bit 0) for the same connector and see whether a
second monitor on the other socket lights instead.  Until then, do not reverse
a mapping that produces a picture.

Routing register values the vendor would write, for reference:

| | controller A / head 0 | controller B / head 1 |
|---|---|---|
| DVO1 digital | `0x1c20[15:8]`, module select `{15,14,8} = 000`, sync bits 11/13, enables 10/12 cleared | `{15,14,8} = 001` |
| DVO2 digital | `0x1c20` bit 30 = 0 **and** `0x1c54` bit 30 = 0, sync bits 27/29, enables 26/28 cleared, `0x1c20` bit 25 above 165 MHz | bit 30 = 1 in both |
| DAC1 analog | `0x1c18` bit 14 = 0, bit 9 = 0, bits 10/12/15/16 = 0, `[8:0] = 0x049` | bit 14 = 1 |
| DAC2 analog | `0x1c1c[15:14] = 00`, bit 9 = 0, bits 10/12/16/17 = 0, `[8:0] = 0x049` | `[15:14] = 01` |
| DVO clocks | DVO1 `0x1c00[14:13]` + bit 27, gate bit 12 | DVO2 `0x1c04[17:15]`, gate bit 14 |

## 9. What driving DVI-2 independently actually costs

Our comment says index 1 "remains detached from the single display pipe until
its independent output route is enabled".  The reverse engineering splits that
into two very different jobs.

### 9.1 Driving DVI-2 *instead of* DVI-1 on the existing pipe: a crossbar value, a clock route and two power bits

**No second display controller and no second synthesiser are needed.**  The
serializer's module-select field selects which display controller feeds it
(`0x1c20` `{bit15,bit14,bit8}` for DVO1, `0x1c20`+`0x1c54` bit 30 for DVO2), so
either serializer can be driven from either head.  What the second connector
needs is:

1. `desc->ready = true` on `parhelia_p690_outputs[1]` so the connector gets an
   encoder (`parhelia_output_init()` only calls
   `drm_connector_attach_encoder()` when `ready`).
2. The **DVO1** register set instead of DVO2's, because the second connector's
   serializer is the other one: control byte `0x1c20[15:8]`, sync bits 11/13,
   enables 10/12, the three-bit module select `{bit15,bit14,bit8}`, clock select
   `0x1c00[14:13]` + bit 27, clock gate `0x1c00` bit 12, power `0x1c24` bit 0.
   Our `parhelia_program_output()` has only the DVO2 path today
   (`output->desc->digital_route == PARHELIA_ROUTE_DVO2`) and falls through to
   the DAC for anything else.
3. `HSLPARSetDVOClock`'s DVO1 form for the clock:
   `0x1c00 = (v & 0xf7ff9fff) | ((c & 3) << 13) | (((c >> 2) & 1) << 27)`, then
   clear bit 12.
4. Nothing else.  The head, the FS, the timing block, the LUT and the DEMREQCTL
   are whatever the pipe already uses.

That is the cheap change, and it is what "independent output route" really
means in our comment.

### 9.2 Driving both connectors at once: a second CRTC

This is the expensive job.  Enumerated precisely:

* **A second display controller instance.**  The silicon has four
  (`HSLPARIsDCInUse`, §5.1); head 0's primary block at `0x1800`/`0x1900` is
  free.  No new discovery needed - the register stride is `head * 0x200` and our
  `PARHELIA_DC()`/`PARHELIA_DCT()` macros already take a head.
* **A second synthesiser.**  fs1, fs2 and fs3 are all free on this board
  (§4.4), so two heads can each own one; the allocation should go through the
  `IsS90FSInUse` predicate rather than two hardcoded constants.
* **The second DCM clock route**, `0x1c00[9:6] = fs + 1` for controller A
  (plus the `0x1fc2 = 0x68` plain byte write that controller A and only
  controller A gets, §4.5).
* **The second head's DEMREQCTL level.**  `HSLPARUpdatePriorityLevels`
  computes `round(7 * rate / maxRate)` per head (§5.8), so with two active heads
  the levels stop being 7/1 and become a real division of DEM bandwidth.  Two
  1920x1200x32 heads is 1.23 GB/s of scanout; `HSLPARGetModeBandwidth`
  (`.text+0xcdda0`) rejects a single mode above `0x7a11f` kHz and halves the
  figure for some digital targets, and `HSLPARValidateDualCRTCConfig`
  (`.text+0xcdd90`) is a **stub that always succeeds**, so the arbitration
  headroom is the only limit the vendor enforces - through the priority levels
  and DC_CTRL bit 18.
* **The preload counts.**  `HSLPARGetCrtcPreloadCount` (`.text+0xac5c0`)
  becomes non-zero exactly when two heads are in use with *different* pixel
  formats and the other head's clock exceeds 350 MHz; `0x191c` and `0x1b1c` must
  then carry `a + b + (a >> 1)` with `a`, `b` = `1024 / bytes_per_pixel`.  Below
  that they stay 0, but they must be written.
* **The genlock path.**  `HSLPARSynchronizeDC` (`.text+0xb2150`) applies a vid
  reset to **both** heads when their width, height and pixel clock match, or
  when `dev[0x3ab] & 0x20` is set.  With one pipe it does nothing (§5.7); with
  two identical modes it is required, or the two rasters drift.
* **A working per-head vblank source.**  This is the one item with no vendor
  answer in this document: our `parhelia_vblank_mask()` already records (HW)
  that on the P690 bit 28 - "the vendor multi-head object's second independent
  CRTC source" - produces no interrupt for our routing and makes atomic commits
  time out, so head-0's bit 1 is used for both.  A second CRTC needs two
  independent vblank sources and that has to be settled on hardware.
* **DRM plumbing.**  `drm_simple_display_pipe_init()` gives exactly one CRTC,
  one plane and one encoder.  Two heads means a full atomic CRTC/plane/encoder
  set per head, a second `drm_crtc_vblank_on/off` pair, and dropping the
  `pdev->active_output` single-slot model (`parhelia_kms_head()` and
  `parhelia_commit_output()` both assume one).

**Cost summary:** enabling DVI-2 on the existing pipe is a descriptor flag plus
a DVO1 branch in `parhelia_program_output()`.  True dual head is a DRM
rewrite - two CRTCs, FS allocation, per-head priority levels, preload counts,
genlock and an unresolved second vblank source - and none of it is required for
the second connector to be usable one at a time.

## 10. DDC on the P690

The DDC bit-bang register is `0x1c48` on every Parhelia variant, with
data/readback at `1 << pin` and an open-drain output enable at
`1 << (pin + 16)`, exactly as `parhelia_ddc.c` implements it.

### 10.1 The pin table and the line resolution - our descriptors are right

`HSLPIN60GetDDCIORegMasks` (`mtx740 .text+0xda430`) reads a four-byte table:
byte index `i` (0..3) **is the GPIO bit** and its value is the logical pin id;
on a match it returns data mask `1 << i` and output-enable mask
`1 << (i + 16)`.  PROVEN:

```
 da45a  cmpw $0x609,0x4(%r8)        PInS version
 da467  mov  $0x5070608,%r10d       built-in table if version <= 0x609
 da4ce  mov  0x1b4(%r8),%r10d       else PInS[0x1b4]
 da480  loop i = 0..3: extract byte i, compare with the requested id
 da4b5  data = 1 << i ; da4c2 oe = data << 16
```

This board is PInS `0x0700` > `0x609`, so the table is
`PInS[0x1b4] = 0x03040102`, i.e. bytes `02 01 04 03`:

| GPIO bit | pin id |
|---|---|
| 0 | 2 |
| 1 | 1 |
| 2 | 4 |
| 3 | 3 |

The four line resolvers each try a list of (SCL id, SDA id) pairs in order and
record a **mux level** for whichever pair resolves.  PROVEN
(`HSLPARGetDDCIORegMasks_DDCLINE1 .text+0xb82c0`, `...LINE2 +0xb8160`,
`...LINE3 +0xb8030`, `...LINE4 +0xb7f00`; each pair is `esi = SCL id,
edx = SDA id` passed to `HSLPARGetDDCIORegMasks_DDCID`):

| line | pairs tried, in order (SCL, SDA) -> mux level |
|---|---|
| 1 | (1,2)->0, (5,6)->0, (13,14)->0, (9,10)->1 |
| 2 | (3,4)->0, (7,8)->0, (13,14)->1, (11,12)->1 |
| 3 | (5,6)->1, (15,16)->0, (9,10)->0 |
| 4 | (7,8)->1, (15,16)->1, (11,12)->0 |

On this board line 1 resolves on its **first** pair and line 2 on its first
pair:

| DDC line | port id | SCL | SDA | mux level |
|---|---|---|---|---|
| 1 | `0` | id 1 -> **bit 1** | id 2 -> **bit 0** | 0 |
| 2 | `5` | id 3 -> **bit 3** | id 4 -> **bit 2** | 0 |
| 3 | `0x11` | id 5 - absent | - | - |
| 4 | `0x12` | id 7 - absent | - | - |

That is **exactly** what `parhelia_p690_outputs[]` declares - index 0
`ddc_scl = 1, ddc_sda = 0` and index 1 `ddc_scl = 3, ddc_sda = 2` - and it
matches `docs/parhelia-ddc-complete.md`'s recorded P690 resolution.  PROVEN,
nothing to change.  It also explains why the 0528 needed the *second* pair of
each list: its `PInS[0x1b4] = 0x05070608` declares ids 5..8 and not 1..4.

Connector to line, PROVEN from `HSLPARGetMonitorEDID` (`.text+0xd36f0`,
`d3752`..`d3850`): output index 0 -> port `0` -> line 1; 1 -> port `5` ->
line 2; 2 -> port `0x11` -> line 3; 3 -> port `0x12` -> line 4.  Detection
order is 0, 1, 2, 3 (`HSLPARGetAllMonitorInfo .text+0xd5ad0`).
`HSLPARI2COpenPort` (`.text+0xc1c40`) accepts only port ids `0`, `1`, `5`,
`0x10`, `0x11`, `0x12`, `0x85`, `0x92` and raises `0xb1d05120` otherwise
(`c1d56`..`c1d72`); ports `0` and `5` set `[dev+0x4164] = 0x1c48`
(`c2214`, `c21ca`).  The `0x527`-revision gate at `c1da5` belongs to port
`0x10` only, so it is irrelevant to both families' DDC lines.

### 10.2 The `0x1c4c` channel select is a no-op here - and the reason is `PInS[0x188]`/`[0x18c]`, not `PInS[0x1b4]`

`HSLPARI2COpenPort` does drive a channel-select before any transaction, on
every family - there is no device gate.  PROVEN at `c1ec7`..`c1eee`:

```
 c1ec7  if ([dev+0x4148] == 0) skip          the "port is open" flag
 c1eda  r8  = &muxLevel                      from the line resolver
 c1edf  r9d = 0
 c1ee2  ecx = 0                               0 = write the pin
 c1ee4  edx = 5                               MISC-IO logical id 5
 c1ee9  esi = 0                               0 = address by id
 c1eee  call HSLPARAccessMiscIo
```

`HSLPARAccessMiscIo` (`.text+0xc1970`) resolves id 5 and, if id 5 is not
declared, retries with **id 8 and inverted polarity** (`c1ae4`..`c1b60`; the
polarity flag `r12d` is 1 for id 5 and zeroed at `c1b5b` for the id-8
fallback).  It then read-modify-writes the register: `v |= OE`, and the data
bit is **set** when `level != 0` under normal polarity or when `level == 0`
under inverted polarity (`c1ba4`..`c1bea`).

`HSLPIN60GetMISCIORegMasks` (`.text+0xda150`) resolves a MISC-IO id in two
passes.  PROVEN:

```
 da1a0  reg = 0x1c4c                          (pins 0..9)
 da1b0  pass 0: table = PInS[0x188]           pins 0..7, nibble per pin
 da1c1    if (table - 1) > 0xfffffffd -> table = 0x43706521   built-in default
 da1b4  pass 1: table = PInS[0x18c]           pins 8..10, nibbles 0..2
 da2a3    -- jumps straight to the compare; NO default substitution --
 da23b  match at pin <= 9: data = 1 << pin ; oe = data << 10
 da2af  match at pin 10:   reg = 0x0308 ; oe = 1 << (pin-9) ; data = oe << 4
```

The asymmetry at `da2a3` is the load-bearing instruction: **the built-in
default only ever replaces the pins-0..7 table.**  On this board

* `PInS[0x188] = 0x00700021` -> pin 0 = id 1, pin 1 = id 2, pin 5 = id 7, the
  rest 0; neither id 5 nor id 8 is present;
* `PInS[0x18c] = 0x00000000` -> pins 8, 9, 10 all declare id 0.

so id 5 is not found, the id-8 retry is not found either, and
`HSLPARAccessMiscIo` returns with a zero data mask and issues **no MMIO at
all** (`c1b3f`/`c1b53` both exit through `c1a86`).  **Confirmed: SUN90 needs no
DDC channel select, and our driver is right not to drive one.**

This corrects the sibling document's reason, which attributed the no-op to the
S90 DDC pin table `0x03040102`.  That table is `PInS[0x1b4]` and has nothing
to do with MISC-IO ids; the 0528 differs because *its* `PInS[0x188] =
0x007a6921` plus `PInS[0x18c] = 0x00000008` declare id 8 at pin 8.  The
practical consequence is the same for both boards, but if a different P690
board ever ships a non-zero `PInS[0x18c]`, the channel select becomes live on
that board - and at `0x0308`, not `0x1c4c`, if the id lands on pin 10.  Note
that our `parhelia_engine_init()` already writes `0x0308` unconditionally
(`0x01` on the P690), which is the same register the pins-10 MISC-IO path
uses.

## 11. Per-family differences, 0x0527/0x0528 versus 0x2539

Family membership as the vendor tests it: `WORD [dev+0x29e]` must be in
`{0x0527, 0x0528}` or `{0x2537, 0x2538, 0x2539}` or `HSLInitDeviceJumpTable`
fails (`.text+0x99aab`, error `0xb0302060`).

| Area | 0x0527 / 0x0528 | 0x2537 / 0x2538 (LX) | **0x2539 (P690)** |
|---|---|---|---|
| InitializeHardware | `HSLPARInitializeHardware` | same | **`HSLS90InitializeHardware`** |
| "chip alive" probe | `0x0e00`, dead at `0`/`0x40000000` | same | **`0x0e24`, dead at `0`/`0x80000000`** |
| Synthesiser bank | `0x0e10 + 4*fs` | same | **`0x0e30 + 4*fs`** (fs 6/7 at `0x0e40`/`0x0e44`) |
| Synthesiser writer | `HWPARWriteFSParameters` | same | **`HWS90InitialiseFSController` -> `sun90pll*`** |
| Divider encoding | `enc6(M)`, negated P/N/D fields; `f = 27000*M*P/(N*D)` | same | **M1-1 `[5:0]`, M2-1 `[10:6]`, P1-1 `[15:11]`, P2-1 `[19:16]`; `f = 27000*M1*M2/(P1*2*P2)`** |
| Synthesiser strobes | `BIT(31)` only, three writes, 10/100/200 us | same | **`BIT(31)` and `BIT(21)` together, two writes, 10 us** |
| Lock indication | `[25:24] == 0b11`, 90 of 100 | same | **`BIT(24)`, 100 of 100** |
| Oscillator enable | - | - | **`0x0e24` bits 30/31, on every program** |
| System-clock registers | `0x0e00` alone, three selectors packed | same | **`0x0e00`, `0x0e04`, `0x0e08`, one domain each** |
| FS allocator | `HSLPARGetFreeFS` / `IsFSInUse` | same | **`HSLS90GetFreeFS` / `IsS90FSInUse`** |
| DCM clock select | `0x1c00`/`0x1c04` bits `[9:6]`, `clksel = fs + 1` | same | **same** |
| VGA MISC on the controller-A clock change | `0x1fc2 = 0x68` plain byte | same | **same, and still controller A only** |
| VGA <-> HIRES mux | `0x1c08` bit 0 / bit 1 | same | same |
| `0x1c50` | `PInS[0x19c]` when `PInS[0x04] > 0x607`, else `0x606` | same | **`PInS[0x19c]` unconditionally** = `0x08000707` here |
| `0x1c58` at init and on VGA->HIRES | `0x40` unconditionally | board test | **board test - fails here, so `0`** |
| DEMREQCTL at init | **not written** | not written | **`0x1c0c \|= 0x0808`, `0x1c10 \|= 0x0808`** |
| Revision erratum | `0x0527` rev <= 1: `VDISPEND + 9`, DC_CTRL bit 9 at 10 bpp | - | **rev 0: `0x03a8 \|= BIT(5)`** |
| `0x12b8` | `1` on `0x0527`, else `0x00070601` | `0x00070601` | `0x00070601` |
| Scanout base field `0x1824` | bits `[26:11]`, mask `0x07fff800` | bits `[27:10]`, mask `0x0ffffc00` | **same as LX** |
| Scanout alignment | 8 KiB at 32 bpp, 4 KiB at 16, 2 KiB else | **4 / 2 / 1 KiB** | **same as LX** |
| Timing registers, bias, derivation | H `-2` and even, V `-1`, 12-bit | same | **same - our encodings are correct** |
| DC_CTRL for a plain progressive mode | `0x00000001` | same | **`0x00000001`** |
| DC_CTRL bit 9 / bit 18 | LUT bypass / sticky bandwidth fault | same | **same** |
| Surface word `0x1834` | `0x00030000 \| fmt<<24 \| pitch` | same | same |
| `DC+0x88` on the primary path | `\|= 0x82` then `&= ~0x82` | same | same |
| `DC+0x88` bits 5:4 | second-controller composite, mode flag bit 0 | same | same |
| Vid reset | `0x1c00` bit 24 strobe, bit 4 route, `0x1900`/`0x1b00` bits 16/17, `0x1c2` park | same | same |
| Primary LUT | `0x1838`..`0x1844 + head*0x200`, mask `0xffc0`, index written once | same | same |
| Priority levels | park 0 in HIRES, default 3, final `round(7*rate/maxRate)` | same | same |
| Analog DAC block | `0x1c18` / `0x1c1c`, **no device gate anywhere** | identical | **identical** |
| DAC trim source | `PInS[0x174]` | same | same; `0x049`/`0x049` here |
| DVO format/crossbar | **`0x1c20`** byte 1 (DVO1) and byte 3 (DVO2) | `0x1c20` **plus a `0x1c54[31:24]` mirror for DVO2** | **same as LX** |
| DVO1 module select | 2 bits `[15:14]` | **3 bits `{15,14,8}`** | **same as LX** |
| DVO2 module select | `0x1c20` bit 30 | `0x1c20` bit 30 **and** `0x1c54` bit 30 | same as LX |
| DVO data/format enables | **set** when `dev[0x3a7]` bit 0/1 | **cleared** unless `PInS[0x95] & 0x08` | cleared here |
| Dual link > 165 MHz | - | `0x1c20` **bit 25** | same |
| PInS board-config word | `0x1c54 = PInS[0x190]` | **`0x1c24 = (PInS[0x190] & ~7) \| (live & 7)`** | same as LX; `0xaac00000` here |
| `0x1c70` | `PInS[0x194]` | same | `0x10` here |
| `0x1c58` bit 6 after a DVO2 enable | set | **cleared** on a flag = 1 board | cleared here |
| DVO2's own clock select | never programmed | **`0x1c04[17:15]`** | same as LX |
| DVO1 clock | `0x1c00[14:13]` + bit 27, gate bit 12 | same | same |
| DVO power | `0x1c24` bits 0, 1, 2, active low | same | same |
| DDC pin table | `PInS[0x1b4]` when `PInS[0x04] > 0x609`, else `0x05070608` | built-in S90 default `0x03040102` | **`PInS[0x1b4] = 0x03040102`** |
| DDC line 1 / line 2 pins | SCL bit 3 / SDA bit 1, SCL bit 2 / SDA bit 0 | - | **SCL bit 1 / SDA bit 0, SCL bit 3 / SDA bit 2** |
| DDC channel select | **required**: `0x1c4c`, MISC-IO id 8 at pin 8, inverted | - | **no-op**: neither id 5 nor id 8 declared |
| MISC-IO id tables | `PInS[0x188] = 0x007a6921`, `[0x18c] = 0x00000008` | - | `0x00700021`, `0x00000000` |
| Pixel-clock detuning | not armed (PInS `0x60a`) | - | **armed** (PInS `0x0700`, `PInS[0x1c4] & 2`) |
| Sense-load test registers | `0x188c + sel*0x200`, `0x1910 + sel*0x200`, result `0x1c14` | same | same |
| `0x08f4` | `ClientWait` dummy read, never written | same | **same - not an output gate** |

## 12. Implementation checklist for the kernel driver

Ordered by impact.  Each item is tagged **[LIVE]** - our SUN90 path is wrong
today on hardware that works - **[LATENT]** - wrong but not currently reachable
- or **[DVI-2]** / **[DUAL]** - needed only for the second connector or for two
heads.  Nothing in this list has been tested on hardware.

1. **[LIVE] Use `PARHELIA_DC_CTRL_ON = 0x00000001` on the P690.**
   `HSLPARGetDCParameters` (`.text+0xac770`) has only two device gates, both
   `0x0527`, and builds `0x1800 = 0x00000001` for a plain progressive primary
   mode.  Bit 9 is the primary-LUT bypass on this family too
   (`HSLPARSetPrimaryLUTBypass .text+0xca0d0`, `ca15d orb $0x2,0x5(%rsp)` on
   `0x1800 + (dc << 9)`, no device test), so **the 256-entry ramp
   `parhelia_pipe_load_gamma()` loads never reaches the output** - CRTC gamma and
   any colour-management path are silently inert.  Bit 18 is the sticky
   bandwidth-fault flag that `HSLPARResetBandwidthState` (`.text+0xcf540`)
   *clears* after a 20 ms wait, and we assert it.  This is the one item that is
   a functional defect rather than a fidelity deviation, and the fix is a
   one-constant change plus deleting the special case in
   `parhelia_dc_ctrl_on()` (§5.4).
2. **[LIVE] Fix the DVO2 clock-select field.**  `0x1c04[17:15]` must carry
   `clk & 7` - 3 for FS2, 4 for FS3 - and the `|= 0x00030000` must go.  In
   `HSLPAREnableDVO2` (`.text+0xd83b0`) the `and $0xfffc7fff; or $0x30000` at
   `d861b` and the `and $0xfffc7fff; or ((clk & 7) << 15)` at `d866f` are
   **mutually exclusive branches of the same field**, selected by the sign of the
   clock argument, and `0x30000 >> 15 = 6` *is* the fallback value 6.  Our code
   ORs both, so we write field 6 for head 1 and field 7 for head 0; code 7 is
   never written by any vendor path.  `docs/parhelia-kms-p690-dvi1-fix.md`'s
   four-step transcription is the source of the error and should be corrected
   there too (§6.4).
3. **[LIVE] Program the DEM request level the vendor computes, by
   read-modify-write, for the active head only.**  `HSLPARUpdatePriorityLevels`
   (`.text+0xcf630`) is the **last** call of `HSLPARSetMode`, and
   `HSLPARGetPriorityLevels` (`.text+0xce9e0`) reduces to
   `level = round(7 * rate / maxRate)`, so a single active head ends at **7**,
   the idle head at 1.  Our plain `0x1c0c = 0x1c10 = 0x0b0b` in
   `parhelia_program_clock()` writes level 3 into both heads.  The `0x0808` half
   of `0x0b0b` *is* correct for this family and belongs in
   `parhelia_engine_init()` where we already have it; the per-mode-set part
   should be an RMW of `[2:0]` (and `[10:8]`) on the active head's register only
   (§5.8).
4. **[LIVE] Correct the DVO sync-polarity sense and the register it lands in.**
   The vendor sets the polarity bit when the mode-info sync flag is **clear**,
   i.e. for a negative-sync mode - `b0fd9 test $0x4,%al; jne skip; or $0x8,%ecx`
   for DVO1 (dword bit 11) and `b108e` for DVO2 (dword bit 27) - which is the
   same sense as the DAC's bits 11/13 that our driver already gets right.  Our
   DVO code sets bits 27/29 for a *positive*-sync mode, so it is inverted; and
   it writes them into `0x1c58`, which on this family carries only bit 6 on the
   mode path.  The polarity bits belong in `0x1c20[31:24]` **and** its
   `0x1c54[31:24]` mirror (§6.3).  Low risk on a DVI link, but it is wrong and
   it is cheap.
5. **[LIVE] Write `0x1c54` as a read-modify-write of byte 3, and put the
   dual-link bit in `0x1c20` bit 25.**  The vendor only ever touches
   `0x1c54[31:24]` on LX/P690 (`b0cab`..`b0cf8`, `b0edf`..`b0f17`); our plain
   write zeroes `[23:0]`, whose POST content no vendor code ever restores.  The
   dual-link bit above 165 MHz is `0x1c20` bit 25 (`b0d58`/`b0d75`), not
   `0x1c54` bit 25.  And **`0x1c20` bit 30 - DVO2's head select - is never
   written by our driver at all**, while we do write `0x1c20` bits 11 and 13,
   which are **DVO1's** sync bits, on a path that drives DVO2 (§6.1, §6.3).
6. **[LIVE] Drop the `0x08f4` "output gate" write, or re-attribute it.**
   `0x08f4` appears exactly once in the whole vendor binary, at `ClientWait+0x1b`
   (`.text+0x4a9bb`), as the dummy register a delay loop reads `3 * n` times.
   No vendor path writes it, on any family.  The register the init paths write is
   `0x08fc = 0`, which we already do.  If the bit-5 clear is genuinely needed it
   came from the VBIOS, not from the HAL, and the comment must say so (§6.6).
7. **[LIVE] Aim the LUT gate byte somewhere real, or remove it.**
   `PARHELIA_DC_LUT_GATE = 0x1aa` added to `dc` lands on `0x19aa` for head 0 and
   `0x1baa` for head 1 - byte 2 of the **second (overlay) controller's**
   `DC_CTRL`, and `0x08` there is its bit 19.  `HSLPARSetPrimaryLUT`
   (`.text+0xcc2a0`) writes no gate at all: it writes the index register once as
   0 and then 256 triples of R, G, `B | BIT(31)` (§5.9).
8. **[LIVE] Complete `parhelia_engine_init()` against `HSLS90InitializeHardware`.**
   Three missing writes and one wrong constant (§2):
   * `0x1c00 |= 0x00809020`, not `0x00808020` - the missing bit 12 is DVO1's
     clock gate, so our init leaves DVO1's clock running;
   * `0x1c50 = PInS[0x19c]` unconditionally on this family (`0x08000707` here);
   * `0x1c58 = 0` on this board - the `PInS[0x90..0x97]` test fails because
     `PInS[0x90] = 0x42`, and the vendor writes the register either way;
   * `0x03a8 |= BIT(5)` when the PCI revision is 0 (`de09e`..`de0d2`).
9. **[LIVE] Stop setting `DC+0x88` bits 5:4, and assert the dim before the
   timing writes.**  `(params & ~0x0082) | 0x0030` does two things: the
   `~0x0082` is `HSLPARDimController`'s release and is right, while `| 0x0030`
   is the second controller's composite enable, which the vendor reaches only
   through DC index 2/3 behind mode flag bit 0
   (`HSLPARGetDCParameters+0x549`, `HSLPARSetDCParameters` `ab975`).  The full
   vendor gesture is `|= 0x82` with `[31:24] = 0` before the timing block and
   `&= ~0x82` after (§5.6).
10. **[LIVE] Move the vid reset ahead of the output programming, and park
    `0x1c00` across the strobe.**  The vendor resets from inside
    `HSLPAREnableDisplayController` (`b1233`), i.e. before
    `HSLPAREnableIntermediateModule` and `HSLPAREnableDisplayOutputs`, and writes
    `DC_CTRL` exactly once.  Doing the reset last is why we need the second
    `DC_CTRL` write and the comment about the reset clearing the enable bit.  And
    when the hold is asserted `HSLPARApplyVidReset` first writes
    `(v & ~(BIT(0)|BIT(9))) | 0x1c2`, then clears bit 5, strobes, and **restores
    bit 5** (`b0410`..`b0464`, `b01a3`); our `parhelia_apply_vid_reset()` only
    clears the hold on both `0x1c00` and `0x1c04` and never restores it (§5.2,
    §5.7).
11. **[LIVE] Guard the VGA MISC write to controller A, and drop the stray
    `route_dcm(0, 4)`.**  `HSLPARSelectDisplayControllerClock` (`.text+0xab100`)
    has no device gate at all: controller A gets a plain byte write of the
    literal `0x68` between the divider write and the clock-select write with the
    hold still asserted, and controller B gets nothing.  Our SUN90 branch
    read-modify-writes MISC for both heads and does it after the DCM routing,
    and for head 1 it additionally points **controller A's** mux at FS3 before
    programming controller B (§4.5).
12. **[LIVE] Make the no-EDID fallback analog, not digital.**  The vendor's
    digital decision is `EDID[0x14] bit 7` AND a PInS-declared digital output
    (`HALPGetMonitorProperties` `93b69`, `93d61`;
    `HSLPARGetDetectedOutputType .text+0xe2870`), and nothing in
    `HSLPARGetNotRespondingMonitorInfo` (`.text+0xd58a0`) promotes an output
    with no EDID to digital.  Our `edid_digital = !analog_sensed &&
    connector_type != VGA` is the opposite rule for a DVI-I connector.  The EDID
    half of our detect() is **confirmed correct**; only the fallback is wrong
    (§7.4).
13. **[LIVE] Write the DAC fields the vendor writes.**  Bit 9 is cleared
    unconditionally for a monitor output on both DACs, and the `[8:0]`
    per-channel trims come from `PInS[0x174]` - `0x049` for both DACs on this
    board.  Our `parhelia_program_output()` touches neither.  Also note the
    vendor writes each DAC **twice**: once with the sync and power bits, then
    again with the trims and the module select (§7.1, §7.2).
14. **[LATENT] Widen the clock solver's M2 range to 32.**
    `sun90pllIsInvalidDividers` (`.text+0xeb380`) allows `1 <= M2 <= 32` and the
    field `[10:6]` is five bits wide; `parhelia_sun90_clock_solve()` clamps
    `b1_hi` to 16, so we search half the space.  The vendor also prefers large
    M2 and small P2, we prefer small dividers - same solutions, different
    tie-break (§4.2).
15. **[LATENT] Allocate the synthesiser instead of hardcoding `0x0e38`/`0x0e3c`.**
    `HSLS90GetFreeFS` (`.text+0xdc060`) reads `0x0e00`, `0x0e04` **and**
    `0x0e08` - SUN90 has one selector per register, not three packed into one -
    and marks an FS busy through `IsS90FSInUse` (`.text+0xdbf20`).  On this board
    all three domains select FS0 (`PInS[0x200/0x204/0x208]` =
    `0x00020902`/`0x00000002`/`0x00000002`), so "FS0 is the system clock, never
    touch it" is **proven correct** and fs1..fs3 are all free - our fixed choice
    is safe *here*, but we never check, and on a board pointing a domain at fs2
    or fs3 we would reprogram a live memory or core clock (§4.4).
16. **[LATENT] Enable the synthesiser oscillator.**  `sun90pllHwOscOn`
    (`.text+0xeb260`) runs at the top of every P690 pixel-clock program: if
    `0x0e24` does not already read bit 31 clear and bit 30 set, it clears bit 31,
    waits 1 ms, sets bit 30, waits 1 ms.  We never touch `0x0e24`.  A POSTed
    board is already in that state, so this matters only after a cold or soft
    reset (§4.4).
17. **[LATENT] Tighten the FS lock test and shorten the settle.**  The vendor
    waits **10 us**, then requires bit 24 set in **100 of 100** reads
    (`sun90pllUnlockStatus .text+0xeb640`, caller threshold `P[0x6c] = 0`).  We
    wait 1 ms and accept the first hit.  Our `BIT(31) | BIT(21)` two-write
    gesture is **exactly the vendor's** and needs no change - `BIT(21)` is a
    genuine second strobe on SUN90, not a divider bit (§4.3).
18. **[LATENT] Fix the scanout alignment check per format.**
    `HSLPARGetAddressAlignmentMask` (`.text+0xacdf0`) pre-aligns the byte address
    to 4 KiB at 32 bpp, 2 KiB at 16 bpp and 1 KiB otherwise on this family; we
    check 1 KiB for everything.  The `0x0ffffc00` mask we use for `0x1824` is
    **correct** for the P690 (§5.5).
19. **[LATENT] Preserve what the vendor preserves in the timing block, and write
    the preload counts.**  `0x1918` is a read-modify-write keeping `[15:12]` and
    `[31:16]`; `0x1928` is a read-modify-write of bit 31 alone; and the vendor
    writes `0x191c` **and** `0x1b1c` explicitly as 0 on every single-head mode
    set rather than inheriting POST values (§5.3, §5.9).
20. **[LATENT] Add the VGA->HIRES niceties.**  `HSLPARSwitchToHIRES`'s tail does
    two `HSLPARVGAWaitForVerticalRetrace` calls, parks head A's DEM levels at 0
    via `HSLPARSetPriorityLevels(dev, 5, {0,0}, {0,0})`, and refreshes
    `0x1c00 |= 0x00809020` / `0x1c04 |= 0x20` / `0x1c24 |= 7` after the
    transition.  Our two-step `0x1c08` transition is already correct (§3).
21. **[LATENT] Implement or explicitly decline pixel-clock detuning.**  It is
    **armed** on this board - PInS `0x0700 > 0x60b` and `PInS[0x1c4] & 2` - and
    shifts the requested clock by +-100 kHz above 65 MHz depending on a
    per-output EDID-derived byte (`.text+0xcebb0`, gate `cfe29`..`cfe74`).  At
    154 MHz that is 0.065%, so declining it is defensible; the comment should
    say we know (§4.6).
22. **[DVI-2] Enable the second connector on the existing pipe.**  It costs a
    `ready = true` on `parhelia_p690_outputs[1]` plus a **DVO1** branch in
    `parhelia_program_output()`: control byte `0x1c20[15:8]`, sync bits 11/13,
    enables 10/12 cleared, the **three-bit** module select `{bit15,bit14,bit8}`
    (LX/P690 widen this field - the 0528 uses two bits), clock
    `0x1c00 = (v & 0xf7ff9fff) | ((c & 3) << 13) | (((c >> 2) & 1) << 27)` then
    clear bit 12, power `0x1c24` bit 0.  **No second display controller and no
    second synthesiser are involved** - the module-select field routes either
    serializer to either head (§9.1).
23. **[DVI-2] Record, but do not act on, the vendor's contrary output mapping.**
    `HSLPARGetDVOIndex` (`.text+0xe2b80`) pairs output 0 - which is DDC line 1,
    GPIO SCL 1 / SDA 0, our descriptor index 0 - with **DVO1**, where our table
    says DVO2; `HSLPARGetControllerIndex` (`.text+0xe3620`) pairs it with
    controller 0, where our table says head 1.  The controller pairing is only an
    allocation and ours is legal.  The DVO pairing is wiring and the two
    disagree - but the resolver is unreachable on this ROM (its digital
    capability bits need `PInS[0x88]` bit 10 or 14, both clear, or a runtime
    external-transmitter probe), the ROM self-identifies as a P650/P750 image,
    and our mapping produces a live 1920x1200 picture.  **Do not reverse it
    without the hardware experiment in §8.3.**
24. **[DUAL] The full cost of two heads**, enumerated in §9.2: a second CRTC and
    plane in place of `drm_simple_display_pipe`, FS allocation through
    `IsS90FSInUse`, controller A's clock route plus its `0x1fc2 = 0x68`,
    per-head DEM levels from `round(7 * rate / maxRate)`, the `0x191c`/`0x1b1c`
    preload counts when the formats differ and the other head is above 350 MHz,
    `HSLPARSynchronizeDC`'s both-heads genlock reset for identical modes, and an
    unresolved second vblank source (bit 28 produces no interrupt for our
    routing, HW).  `HSLPARValidateDualCRTCConfig` (`.text+0xcdd90`) is a stub
    that always succeeds, so bandwidth is policed only by the priority levels
    and DC_CTRL bit 18 - which item 1 stops us from asserting.
25. **Already right, so no work:** the timing word encodings (§5.3), the
    `0x1824` mask (§5.5), the surface word and `parhelia_dc_surface_code()`
    (§5.4), `parhelia_output_load_sense()` (§7.3), the two-step `0x1c08`
    transition (§3), the `0x1c0c`/`0x1c10` `|= 0x0808` at init (§2), the
    `0x1c24 = (PInS[0x190] & ~7) | (live & 7)` and `0x1c70 = 0x10` writes
    (§6.3), the synthesiser divider model and its two-write strobe (§4.2, §4.3),
    the EDID half of `detect()` (§7.4), and both DDC pin pairs, which are
    `PInS[0x1b4] = 0x03040102` resolved through the vendor's own candidate lists
    (§10.1) - our `parhelia_bios_ddc_pins()` tables are literally the vendor's.

### The one-change experiments worth running first

* Item 1 is self-checking without a monitor change: set
  `PARHELIA_DC_CTRL_ON = 0x00000001` and a non-identity gamma ramp must become
  visible.  If it does not, bit 9 is not the bypass on this silicon and the
  finding is wrong.
* Item 2 predicts nothing visible at 1920x1200 (both 6 and 4 deliver the same
  frequency through different paths) but should be verified not to *break* the
  link before it is called a fix.
* Item 23's experiment - program DVO1 for the same connector and watch which
  socket lights - is the only way to settle the wiring question, and it is
  worth doing before any dual-head work.
