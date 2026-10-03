# The HSLPAR (0x0527/0x0528) output path, reverse engineered

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Why: a retail PCI-X Parhelia 8X (`102b:0528` rev 06, board `PH-P256`, two
DVI-I connectors, no VGA socket) is POSTed, our DRM driver brings the chip up,
X starts, a 1024x768 mode is programmed, the pixel synthesiser reports lock -
and an analog monitor on the card shows NO SIGNAL.  This document reverses the
vendor's complete "mode computed -> picture out" path for this chip generation
and lists, ordered by likelihood, what our kernel driver is missing.

Static reverse engineering only.  No hardware was touched while writing this.

## The answers, up front

* The **VGA -> HIRES switch is not a P690 peculiarity**: `0x1c08` bit 0 is the
  legacy VGA path and bit 1 the native one, and `HSLPARSwitchToHIRES` runs on
  every mode set for every family, with a *longer* 0x0527/0x0528 branch than
  the LX/P690 one (§3).  Our kernel already reaches HIRES.
* The **VGA MISC clock-select mux is part of the native path after all - but
  only for controller A.**  `0x1fc2 = 0x68` is a plain byte write inside
  `HSLPARSelectDisplayControllerClock`, on the controller-A branch, on every
  real mode change.  Controller B gets nothing.  That reconciles the X
  driver's comment (it drives head 1) with the speculative kernel write (which
  drives head 0) - both were right for their own head (§10.4).
* The **VGA sequencer unblank and the attribute-controller palette-address
  source are not needed** by the native display controller; the only live
  SR01 writer skips itself when `0x1c08` bit 0 is clear (§11.11).
* **`0x1c20` is the DVO crossbar only.**  The analog path writes `0x1c18` /
  `0x1c1c` and nothing else; `0x1c20` is reached solely through
  `HSLPAREnableIntermediateModule` -> `HSLPAREnableDVO` (§6).
* **The per-head video-output enable at `DC+0x88` bits 5:4 is not a family
  difference** - those bits belong to the *second* (overlay) display
  controller and are selected by a mode flag, not by the chip.  What the
  primary path does on every family is a dim assert/release,
  `DC+0x88 |= 0x82` then `&= ~0x82` (§11.6).
* **A vid reset is required, twice, on every family**, from
  `HSLPAREnableDisplayController` and again from `HSLPARSynchronizeDC` (§11.7).
* The **synthesiser bank for 0x0527/0x0528 is `0x0e10 + 4*fs`, the same one the
  LX uses; only the P690 (0x2539) uses `0x0e30 + 4*fs`** - and the index is
  *allocated* against `0x0e00`, not fixed per head (§10.1).
* The board's first output is **DAC1 (`0x1c18`) / DVO1**; our kernel's
  descriptor table has it as DAC2/DVO2/head 0, which is the other connector
  (§5.3, §8, checklist item 1).
* DDC on this board needs a **channel select on `0x1c4c` (bit 18 output
  enable, bit 8 data)** that our driver never drives, and the board's own
  VBIOS drives it before every transaction.  That is the most likely reason
  `0x1c48` bit 3 - connector 1's SCL - reads permanently low (§12).

## 0. Binaries, method and how claims are marked

| Tag | Binary | Notes |
|---|---|---|
| `mtx740` | `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | vendor Linux X driver, one `HSLPAR*` HAL covering 0x0527/0x0528 **and** LX/P690; full dynamic symbols |
| `mtxparm` | `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARM.sys` | vendor Windows XPDM miniport, `pei-i386`, no symbols; used only as an independent cross-check |
| `rom0528` | `captures/parhelia-0528-cold/parhelia-0528-vbios.rom` = `vbios-matrox-parhelia` | this board's own VBIOS, `PCIR` device `0528`, PInS at file offset `0x7800`, version `0x60a` |

All `mtx740` offsets are `.text` virtual addresses in that file.  `objdump`
renames the exported symbols to local aliases (`HSLPARSetMode` prints as
`<mtx2513>`); the names below are the `nm -D` names and the offsets are the
real ones.

MMIO is reached through `ClientReadRegisterDword(dev, reg, &out)` and
`ClientWriteRegisterDword(dev, reg, val)`, so in every quoted fragment the
register offset is the second argument (`esi`).  Device-struct fields used
below:

* `WORD [dev+0x29e]` - the PCI device id (`0x0527`, `0x0528`, `0x2537`..`0x2539`).
* `dev+0x3b78` - the PInS (VBIOS board struct) shadow, so PInS byte `N` is
  `[dev+0x3b78+N]`.
* `DWORD [dev+0x4a98]` / `DWORD [dev+0x4a9c]` - **add-on module present flag**
  and **add-on module type enum** (CEM / quad / Chrontel daughterboards).
  These are *not* a chip-family selector.  Every branch gated on them is
  irrelevant to a plain retail board and is called out as such below, because
  an earlier note in `parhelia-dualhead-complete.md` mistook the `0xa`/`0x10`/`0x3`
  values of `[dev+0x4a9c]` for `PInS[0x1a1]`.  **CORRECTION (PROVEN,
  `HSLPARGetDACIndex` `mtx740 .text+0xe2a50`, `HSLPARGetDVOIndex` `+0xe2b80`):
  those enums are the add-on type; `PInS[0x1a1]` is read in only three places in
  the whole binary (`HSLPARDetectCEM+0x5a` `.text+0xc0bea`, `+0xc0c14`, and
  `HSLPIN60InitCEMStructure+0x4c` `.text+0xdaaec`), all add-on-module code.**

Marking: **PROVEN** = read directly out of the named binary at the named
offset (or out of `rom0528` at the named file offset).  **INFERRED** =
deduced, consistent, but not a single decisive instruction.  Nothing here has
been run on hardware.

## 1. The vendor's mode set, in order

`HSLPARSetMode` (`mtx740 .text+0xd11e0`) is the whole thing.  PROVEN call
order:

```
 d1201  SelectDC                          -> returns the DC module index (0/1/3)
 d1259  HSLPARWaitEngineNotBusy
 d125e  test BYTE [dev+0x3a8],0x2         -> set by HSLPARInitializeHardware+0x8e
 d126a    HSLPARSwitchToHIRES             (else: call [dev+0x4e80], a chip vtable slot)
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
 d1350  HSLPAREnableClock
 d135f  HSLPAREnableDisplayController      (calls HSLPARApplyVidReset at +0x103)
 d136e  HSLPAREnableIntermediateModule     (this is the DVO/TMDS serializer block)
 d1379  HSLPARApplyLUT
 d1384  HSLPAREnableDisplayOutputs         (this is where the DACs are programmed)
 d138f  HSLPARSetModeZoom
 d139a  HSLPARSynchronizeDC                (calls HSLPARApplyVidReset at +0x88)
 -- always --
 d12ab  HSLPARSetModeUpdateStructure
 d12b9  HSLPARUpdateClockInfoStructure
 d12c1  HSLPARUpdatePriorityLevels
```

Three points that contradict assumptions in our tree:

* **`HSLPARSwitchToHIRES` is not P690-specific.**  It runs on every mode set
  for every family, and its 0x0527/0x0528 path is *longer* than the LX/P690
  one, not absent (PROVEN, see §3).
* **`HSLPAREnableIntermediateModule` is the DVO/serializer stage.**
  `HSLPAREnableDVO` has exactly two callers in the binary, both inside
  `HSLPAREnableIntermediateModule` (`+0xfe` = `.text+0xb1ede`, `+0x203` =
  `+0xb1fe3`).  For an analog monitor this stage is a no-op.
* **A vid reset is applied twice on every mode set, on every family** - once
  from `HSLPAREnableDisplayController+0x103` and once from
  `HSLPARSynchronizeDC+0x88` (PROVEN from the call sites).  It is not a P690
  peculiarity.

## 2. Chip init: what must already be true before a mode set

`HSLPARInitializeHardware` (`mtx740 .text+0xbf780`).  PROVEN sequence, display
part only:

```
 bf7e6  read 0x0e00; if it is 0 or 0x40000000 -> call [dev+0x4f20] (chip is dead)
 bf80e  dev[0x3a8] |= 2                       (marks "use HSLPARSwitchToHIRES")
 bf824  HSLPARApplyVidReset(dev, 0xffffffff)  (all heads)
 bf82e  read 0x1c00, read 0x1c04
 bf84d  0x1c00 |= 0x00809020                  (bit5|bit12|bit15|bit23)
 bf864  0x1c04 |= 0x00000020                  (bit5)
 bf86d  write 0x1c00, write 0x1c04
 bf883  wait 10 us
 bf8be  write 0x1c08 = 0x00000002             PLAIN WRITE: VGA off, HIRES on
 bf8d0  write 0x1c24 = 0x00000007             both DVOs + common powered down
 bf8df  write 0x1c50 = <PInS>                 0x606 if PInS[0x04] <= 0x607,
                                              else DWORD PInS[0x19c]   (bf81f/bfffa)
 bf8ee  write 0x08fc = 0
 bf9a6  write 0x1c58 = 0x00000040             for 0x0527/0x0528 unconditionally
                                              (bf904: `lea eax,[rdx-0x527];
                                               cmp eax,1; jbe bf9a6`)
 bf9bb  0x0920 &= ~BIT(31)
 bf9de  write 0x10b0 = 0x40
 bfad1  HSLPARDisableDisplayOutputs
 bfb30  HSLPARDisableDisplayController + HSLPARSetPrimaryLUT, x4 controllers
 bfee8  HWPARInitializeSystemClock / HSLPARGetSystemCLock /
        HWPARInitializeMemoryController
 bff36  0x12b8 = f(PCI config 0xf4); 0x12b0 = 0
 bffa2  0x0000 = 0x10000000; 0x0004 = 0; 0x0008 = 0x10000020; 0x000c = 0x00010001
```

Cross-checked independently in `mtxparm`, same order, same constants:
`.text+0x32b95  or DWORD PTR [ebp+0x6c],0x809020`, `+0x32b9f  or ...,0x20`,
`+0x32c02  push 0x2 / push 0x1c08`, `+0x32c0f  push 0x7 / push 0x1c24`,
`+0x32c1d  push 0x1c50`, `+0x32bbf  cmp WORD PTR [ebx+0x3b7c],0x608` (the same
PInS-version gate, PInS shadow at `dev+0x3b78`), `+0x32c46  push 0x1c58` with
the value computed as `0x40` or `0` from a predicate.

For **this board** (`rom0528`, PInS at `0x7800`, version `0x60a` > `0x607`):

| PInS field | file offset | value | meaning |
|---|---|---|---|
| `+0x004` version | `0x7804` | `0x060a` | selects the `PInS[0x19c]` and `PInS[0x174]` paths |
| `+0x174` | `0x7974` | `0x01fc6c36` | DAC per-channel trim, three 3-bit fields per DAC (§5.2) |
| `+0x178` | `0x7978` | `"PH-P256"` | board name |
| `+0x190` / `+0x194` | `0x7990` | `0x0000001f` / `0x00000000` | DVO board config (**not** the P690's `0xaac00000`/`0x10`) |
| `+0x19c` | `0x799c` | `0x08000707` | the value the vendor writes to `0x1c50` |
| `+0x1a1` wiring | `0x79a1` | `0x00` | board wiring enum |
| `+0x1a5` | `0x79a5` | `0x00` | board type byte (the `0x25` special case does not apply) |
| `+0x1b4` DDC table | `0x79b4` | `08 06 07 05` | DDC pin ids (§12) |

PROVEN (read out of `rom0528` at those offsets).

## 3. The VGA <-> HIRES mux at 0x1c08 - required on 0x0527/0x0528

`0x1c08` bit 0 = legacy VGA path enabled, bit 1 = HIRES (native display
controller) path enabled.  PROVEN from three independent sites:

* `HSLPARSetVGAEnable` (`mtx740 .text+0xd75d0`): `0x1c08 &= ~2`, then
  `0x1c08 |= 1`, then `0x1fde = 0`.
* `HSLPARInitializeHardware+0x13e`: `0x1c08 = 2` (plain write).
* `HSLPARSwitchToHIRES` (`.text+0xbd7d0`), the tail at `+0x9a1`..`+0xa72`:

```
 be101  HSLPARVGAWaitForVerticalRetrace
 be12b  HSLPARSetPriorityLevels(dev, 5, &0, &0)
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

Entry condition, PROVEN at `bd841`..`bd848`/`be403`: it reads `0x1c08` and
returns immediately only when bit 1 is set **and** bit 0 is clear, i.e. when
the part is already in HIRES.  A POSTed board left in VGA text mode has bit 0
set and bit 1 clear, so the full switch runs.

`mtxparm` confirms both directions: `.text+0x32993  eax = [ebp-8] & ~2 | 1;
push 0x1c08` (to VGA) and `.text+0x32c02  push 0x2 / push 0x1c08` (init to
HIRES).

The 0x0527/0x0528-specific *head* of `HSLPARSwitchToHIRES` (PROVEN,
`bd85c  lea eax,[rdx-0x527]; cmp eax,1; ja be258` selects it):

```
 bd875  write 0x1c58 = 0x00000040          (0x0527/0x0528: unconditional)
 bd891  0x1c18 |= BIT(15)                  DAC1 video power down
 bd8b9  0x1c1c |= BIT(16)                  DAC2 video power down
 ...    HSLPARDisableDVI(dev, 0..3, buf) for each output-link flag present
        (masks tried in order: 0x20000000, 0x40000000, 0x80000000, 0x100)
```

Same 0x0527/0x0528 gate for the `0x1c58 = 0x40` write appears in
`HSLPARInitializeHardware` (`bf904`).  LX/P690 reach the same write only
through a PInS board test (`be258`..`be2d1`, bytes `PInS[0x90..0x97]`), and
`0x2537` additionally needs `DWORD [dev+0x2a0] == 1`.

`0x1c58` bit 6 on the original family is a DVO-side gate, not an analog one:
`HSLPARDisableDVO2+0xa8` (`.text+0xd7428`) clears it, and
`HSLPAREnableDVO+0x66f` (`.text+0xb0b5f`) clears it before the vid reset.
Setting it in the VGA->HIRES transition and leaving it set for an analog-only
mode is therefore correct (INFERRED from those three sites; the bit's own
function is not named anywhere).

Our kernel's status: it **does** reach HIRES - `parhelia_program_clock()`
writes `0x1c08 = PARHELIA_DCM_MODE_HIRES` (a plain `0x00000002`) in the
`PARHELIA_CLOCK_HSLPAR` case, which matches the vendor's init write.  What it
does not do is the vertical-retrace waits, the `HSLPARSetPriorityLevels(5)`
call, the clean two-step bit0-then-bit1 transition, the
`0x1c00 |= 0x00809020` (it ORs `0x00808020`, missing bit 12 - the DVO1 clock
gate, PROVEN from `HSLPARDisableDVO1+0x4f` `.text+0xd732f  or BYTE PTR
[rsp+0xd],0x10` and `HSLPARPowerUpDVO1Clock+0x62` `.text+0xd8382  and BYTE PTR
[rsp+0x5],0xef`), the `0x1c50` write, or the `0x1c58 = 0x40` write.

## 4. Does the native path need any VGA-side register?

`HSLPARSetVGAEnable` is called from exactly one place, `HSLPARSwitchToVGAMode`
(`mtx740 .text+0xd8b4d`), and `HSLPARSetSyncPolarity` from exactly one place,
`HSLPARReturnToVga` (`.text+0xd9d70`).  Neither is reachable from
`HSLPARSetMode`.  PROVEN by exhaustive call-site enumeration of the `@plt`
stubs.

**But the VGA MISC register is a different matter, and here my own
speculation turned out to be right for the wrong reason:** the native path
*does* write MMIO `0x1fc2`, once, and only when the mode set is changing
**controller A's** clock.  See §10.4 for the exhaustive
`0x1fc0`-`0x1fcc` accounting and §11.11 for the sequencer/attribute-controller
side.  Summary:

* `0x1fc2 = 0x68` (a plain byte write, bits `[3:2] = 0b10`) is issued from
  `HSLPARSelectDisplayControllerClock+0xdb` on the controller-A branch only,
  and that function is on the `HSLPARSetMode` path.  Controller B gets no
  MISC write at all.
* The VGA sequencer and the attribute controller are touched only by the
  VGA/legacy and save/restore paths.

## 5. The analog DAC block

Two RAMDACs, one control dword each: **DAC1 = `0x1c18`, DAC2 = `0x1c1c`**.

### 5.1 Field layout (PROVEN unless noted)

| Bits | DAC1 (`0x1c18`) | DAC2 (`0x1c1c`) | Evidence |
|---|---|---|---|
| `[8:0]` | three 3-bit per-channel trim fields (`[8:6]`,`[5:3]`,`[2:0]`) from `PInS[0x174]` | same | `HSLPAREnableDisplayOutputs+0x6dd`..`+0x707` (`.text+0xb761d`) and `+0x7d2`..`+0x7fe` (`+0xb7712`) |
| `9` | TV / composite-sync mode; **cleared unconditionally** for a monitor | set only when the output-link flags carry `0x04000000` (TV on DAC2) | `+0xb75d0  and eax,0xffffffd5`; `+0xb76ef  test ebp,0x4000000` / `+0xb76fb  or eax,0x2` / `+0xb79cc  and edx,0xffffffe9` |
| `10`, `12` | master DAC power, **active low** (set = off) | same | `HSLPARDisableDAC1 .text+0xd6983  or BYTE PTR [rsp+0xd],0x94`; `HSLPAREnableDAC1 +0xd6aa3  and ...,0x6b` |
| `11` | HSYNC negative | same | `+0xb75d8  or eax,0x8` taken when `modeinfo[0x48] & 0x04` is clear |
| `13` | VSYNC negative | same | `+0xb75e0  or eax,0x20` taken when `modeinfo[0x48] & 0x08` is clear |
| `14` | **1-bit** DC-module select: 0 = DCM A / head 0, 1 = DCM B / head 1 | low bit of a **2-bit** field | `HSLPARSetDAC1OnDCMA .text+0xd6653  and BYTE PTR [rsp+0xd],0xbf`; `HSLPARSetDAC2OnDCMA +0xd66cf  and dh,0x3f` |
| `15` | RGB video power, active low (with bit 16) | high bit of the DC-module select | DAC1: `HSLPAREnableDacSyncOnly .text+0xae1fa  or BYTE PTR [rsp+0xd],0x80`; DAC2: `+0xb7d66/+0xb7d76/+0xb7d94` write `[15:14]` = 0 / 1 / 3 by module index |
| `16` | RGB video power, active low (with bit 15) | RGB video power, active low (with bit 17) | `HSLPAREnableDacSyncOnly +0xae1f5  or BYTE PTR [rsp+0xe],0x1` (DAC1) and `+0xae24d  or BYTE PTR [rsp+0xa],0x3` (DAC2) |
| `17` | - | RGB video power, active low | as above |

So the power masks our tree already carries are right, and now proven:
`PARHELIA_DAC1_PWR = 0x00019400` (bits 10,12,15,16) and
`PARHELIA_DAC2_PWR = 0x00031400` (bits 10,12,16,17) are exactly what
`HSLPAREnableDAC1`/`HSLPAREnableDAC2` clear and
`HSLPARDisableDAC1`/`HSLPARDisableDAC2` set.

Two fields our driver does **not** write and must:

* **DAC2's module select is two bits, `[15:14]`, not one.**  Encoding
  (PROVEN, `HSLPAREnableDisplayOutputs+0xdf0`..`+0xe54`): module 0 -> `00`,
  module 1 -> `01` (`or dh,0x40`), module 3 -> `11` (`or dh,0xc0`); module 2
  raises error `0xb1e03131`.  A *separate* encoding, bit 15 set alone
  (`and dh,0x3f; or dh,0x80` at `+0xb775a`), selects the TV/CVE clock source
  and is taken only when the output-link flags carry `0x04000000`.  Our
  `PARHELIA_DAC_ROUTE_DCMB` only touches bit 14 and `PARHELIA_DAC2_PWR` does
  not include bit 15, so **nothing in our driver ever clears `0x1c1c` bit 15**.
  If POST leaves it set, DAC2 is fed from the TV clock or from module 3 and
  emits no sync.  (The un-POSTed reset value is benign - `captures/
  parhelia-0528-cold/parhelia-0528-regs-unposted.txt:1800` shows
  `1c1c = 00030000`, bit 15 clear - but the state after this board's VBIOS has
  lit its own console is not known.)
* **Bit 9 is cleared unconditionally by the vendor for a monitor output on
  both DACs**; our driver never touches it.

Sync polarity is *also* written to both DACs by `HSLPARSetSyncPolDAC`
(`.text+0xd7920`), which reads `0x1c1c` and `0x1c18`, masks byte 1 with `0xd7`
(clearing bits 11 and 13) and then sets `0x28` (bits 11 and 13) in **both**
unless the caller passes `0xffff`/`0xfffe`.  On the native mode-set path that
function is unreachable (§4), so the inline code in
`HSLPAREnableDisplayOutputs` is the only writer - our polarity convention
(`NHSYNC -> bit 11`) matches it.

### 5.2 The vendor writes each DAC twice

PROVEN, `HSLPAREnableDisplayOutputs` (`.text+0xb6f40`), DAC1 branch at
`+0x670` (`+0xb75b0`):

1. read `0x1c18`; clear bits 9, 11, 13; set 11/13 from the mode's sync flags;
   clear bits 10, 12; **write** `0x1c18` (`+0xb75f6`).
2. load the three trim fields (from `dev[0x425c/0x4260/0x4264]` when
   `dev[0x3b7] & 0x08`, else from `PInS[0x174]` bits `[8:6]`,`[5:3]`,`[2:0]`
   when `PInS[0x04] > 0x603`); insert into `[8:0]`; clear bit 16; clear bit 15;
   write bit 14 from `SelectDC`'s module index; **write** `0x1c18`
   (`+0xb7683`).

DAC2 branch at `+0x773` (`+0xb76b3`) is symmetric, with `[17:15]`,`[14:12]`,
`[11:9]` of `PInS[0x174]` for the non-TV case and `[26:24]`,`[23:21]`,
`[20:18]` for the TV case, bits 16/17 cleared, and `[15:14]` written as a pair.

For `rom0528` (`PInS[0x174] = 0x01fc6c36`) that gives `0x1c18[8:0] = 0x036`
and `0x1c1c[8:0] = 0x161`.  These are almost certainly per-channel output
current/gain trims, and POST will already have written them, so this is a
correctness item rather than a likely cause of NO SIGNAL (INFERRED).

### 5.3 Which DAC belongs to which connector

PROVEN, `HSLPARSetOutputAnalogModeType` (`mtx740 .text+0xd4210`), called from
`HSLPARBuildOutputModeType+0x70`:

```
 analog output index 0 -> output-link flags |= 0x01000000
 analog output index 1 -> output-link flags |= 0x08000000
 analog output index 2 -> |= 0x00000002
 analog output index 3 -> |= 0x00000004
```

and in `HSLPAREnableDisplayOutputs`, on a board with **no** add-on module
(PROVEN, `mtx322 .text+0xaaf26  and r13d,0x1000000` and
`mtx323 .text+0xab096  test r13d,0x8000000` / `+0xab09f  and r13d,0x4000000`):

* `0x01000000` present -> program **DAC1 (`0x1c18`)**;
* `0x08000000` or `0x04000000` present -> program **DAC2 (`0x1c1c`)**.

`HSLPARGetDACIndex` (`.text+0xe2a50`) closes the loop: with no add-on module
it assigns DAC index 0 to board output 0 (when that output's type field is 2 =
analog) and DAC index 1 to board output 1.  `HSLPARGetDVOIndex`
(`.text+0xe2b80`) likewise assigns DVO index 0 to output 0 and 1 to output 1
(type field 1 = digital).

**So the board's first output is DAC1 / DVO1, and the second is DAC2 / DVO2.**
The full output-link flag encoding, recovered from
`HSLPARBuildOutputModeType` (`.text+0xd4280`) against the per-output
capability word at `[dev+0x20 + idx*0x80]`:

| Flag | Output |
|---|---|
| `0x01000000` | analog, DAC1 |
| `0x08000000` | analog, DAC2 |
| `0x04000000` | TV on DAC2 |
| `0x00000002` / `0x00000004` | analog outputs 3 and 4 (quad boards) |
| `0x20000000` | digital output 0 |
| `0x40000000` | digital output 1 |
| `0x80000000` | digital output 2 |
| `0x00000100` | digital output 3 |

Our own hardware-tested X driver agrees with that on this board: `src/
parhelia.h:110` records "1 = DVI-1 (`0x1a00`/`0x1b00`, clock B), 0 = DVI-2
(`0x1800`/`0x1900`, clock A)", `parhelia_ddc.c:281` defaults `dcHead = 1`, and
its no-EDID fallback (`parhelia_ddc.c:326`) senses **DAC1** first and on a hit
sets `dcHead = 1`, so the configuration that produced a picture on this board
was **head 1 + DAC1 (`0x1c18`) with bit 14 set**.

Our kernel driver's table is the mirror image of that.
`parhelia_original_outputs[0]` (`parhelia_kms.c:1143`) declares
`preferred_head = 0`, `analog_route = PARHELIA_ROUTE_DAC2`,
`digital_route = PARHELIA_ROUTE_DVO2`, and
`parhelia_original_outputs[1]` gets head 1 / DAC1 / DVO1.  Since the single
display pipe can only drive `outputs[0]` today (`parhelia-0528-posted-driver-status.md`
§6), the driver always lights **head 0 + DAC2**, which is the *other*
connector, while DRM names that connector `DVI-I-1`.

### 5.4 Sync-only and blanking, for completeness

`HSLPAREnableDacSyncOnly` (`.text+0xae100`) powers the RGB converters down and
leaves sync running: DAC1 gets bits 15+16 set, DAC2 bits 16+17, both keep bits
10 and 12 clear.  A monitor in that state sees sync and reports a black
picture, **not** NO SIGNAL.  That is the useful diagnostic split: NO SIGNAL
means bits 10/12 are set, the module select points somewhere without a clock,
or it is the wrong DAC entirely.

`HSLPARBlankDisplayOutput` (`.text+0xb2210`) calls `HSLPARDimController` and
sets `0x1c18` bit 15; nothing on the mode-set path calls it.

## 6. The DVO crossbar at 0x1c20 - and why analog does not need it

`0x1c20` is the original family's DVO (integrated TMDS serializer) control and
crossbar register, with one control byte per serializer:

* byte 1, bits `[15:8]` = DVO1;
* byte 3, bits `[31:24]` = DVO2.

Within each byte (PROVEN):

| Bit in byte | DVO1 dword bit | DVO2 dword bit | Meaning | Evidence |
|---|---|---|---|---|
| 0 | 8 | 24 | cleared by `HSLPAREnableDVO1` on LX/P690 only | `.text+0xd8738  and ecx,0xfffffffe` |
| 2, 4 | 10, 12 | 26, 28 | data/format enable pair, written as `0x14` | `HSLPAREnableDVO1 .text+0xd87ac  or edx,0x14`; `HSLPAREnableDVO .text+0xb090b  or ecx,0x14` |
| 3 | 11 | 27 | HSYNC negative | `HSLPAREnableDVO .text+0xb080a  or ecx,0x8`, taken when `modeinfo[0x48] & 4` is clear |
| 5 | 13 | 29 | VSYNC negative | `+0xb0811  or ecx,0x20` |
| 6, 7 | 14, 15 | 30, 31 | **2-bit** source/DC-module select, same encoding as DAC2's `[15:14]` | `HSLPAREnableDVO1 .text+0xd874b  and dh,0x3f` clears **both** bits, not just bit 14 |

That refines the entry in `parhelia-dualhead-complete.md` §4, which described
the head select as the single bits 14 and 30: it is a two-bit field and the
compact `HSLPAREnableDVO1`/`HSLPAREnableDVO2` helpers force it to module 0.

Other fields: byte 0 bit 6 (dword bit 6) is cleared before the write
(`HSLPAREnableDVO .text+0xb09d0  and BYTE PTR [rsp+0x24],0xbf`); byte 3 bit 1
(dword bit 25) is cleared at `+0xb0989  and BYTE PTR [rsp+0x27],0xfd`.  The
second `0x1c20` read-modify-write at `+0xb0a08` is gated on
`devid - 0x2537 <= 2` (`+0xb09f6`), i.e. it is LX/P690 only.

**Is `0x1c20` required for an analog monitor?  No.  PROVEN by omission:** the
only writer of `0x1c20` on the mode-set path is `HSLPAREnableDVO`, reached
solely through `HSLPAREnableIntermediateModule`; the analog branch of
`HSLPAREnableDisplayOutputs` writes `0x1c18`/`0x1c1c` and nothing else.  The
sync-polarity mirror into `0x1c20` comes from `HSLPARSetSyncPolDVO`
(`.text+0xd7a60`, which itself tail-calls `HSLPARSetSyncPolDAC` at
`+0xd7c0d`), and that path hangs off `HSLPARSetSyncPolarity`, which only
`HSLPARReturnToVga` calls.

`0x1c24` is the shared DVO power register, active low: bit 0 = DVO1, bit 1 =
DVO2, bit 2 = the common block.  PROVEN from `HSLPARDisableDVO1 .text+0xd7357
or BYTE PTR [rsp+0x8],0x5` (bits 0 and 2), `HSLPARDisableDVO2 .text+0xd7409
or BYTE PTR [rsp],0x6` (bits 1 and 2), `HSLPAREnableDVO1 .text+0xd8776  and
BYTE PTR [rsp+0x8],0xfa`, and the `0x1c24 = 7` writes at init and in
`HSLPARSwitchToHIRES`.  `0x1c00` bit 12 and bits `[14:13]`+bit 27 are DVO1's
clock gate and 3-bit clock select (`HSLPARPowerUpDVO1Clock .text+0xd8348`:
`0x1c00 &= 0xf7ff9fff; |= (clk&3)<<13; |= (clk>>2 &1)<<27;` then clear bit 12);
`0x1c04` bits `[17:15]` and bit 14 are DVO2's.

## 7. Analog load sense, and what "NO SIGNAL" narrows it to

`HSLPARSenseLoadOnInternalDAC` (`mtx740 .text+0xc0e00`).  PROVEN: the second
argument is the **analog output index** (`r13d = esi`, `cmp r13d,1; ja` for
anything above 1), and at `+0xc0ecd` the code derives `sel = (index != 0)`,
passes it to `HSLPARSetOutputAnalogModeType` to obtain the link flag
(`0x01000000` for DAC1, `0x08000000` for DAC2), and uses the same `sel` to
index the DC-block test registers:

```
 c10f8  0x1c18 |= 0x1400                 (bits 10,12: master DAC power off)
 c111f  save 0x188c + sel*0x200          (sel<<9 - the head's DC block +0x8c)
 c1130  write 0x188c + sel*0x200 = 0x16459164
 c1138  save 0x1910 + sel*0x200          (the head's DCT block +0x10)
 c1171  write 0x1910 + sel*0x200 = saved & 0xf000ffff
 c1180  ClientWait(10000)                (10 ms)
 c1192  read 0x1c14                      -> comparator result
 c11c1  restore 0x1c18, 0x1c1c, 0x188c+, 0x1910+
```

Our kernel's `parhelia_output_load_sense()` matches this exactly, including the
`sel * 0x200` offset, the `0x16459164` value, the `0xf000ffff` mask and the
10 ms settle.  One caveat worth carrying: at `+0xc0ef8` the vendor reads
`0x1c08` and takes a **different** path depending on whether bit 0 (VGA) is
still set, so a sense done before the VGA->HIRES switch is not the same
measurement as one done after.

The useful diagnostic split, PROVEN from `HSLPAREnableDacSyncOnly`
(`.text+0xae100`): powering only the RGB converters down (DAC1 bits 15+16,
DAC2 bits 16+17) leaves H/V sync running, so a monitor in that state reports a
**black picture**, not NO SIGNAL.  NO SIGNAL therefore means one of:

1. bits 10/12 of the DAC in use are still set (master power off);
2. the DAC's module-select field points at a controller that is not running -
   and for DAC2 that field is two bits wide, `[15:14]`;
3. the picture is being driven out of the *other* DAC entirely.

## 8. Board wiring on this card

PROVEN from `rom0528` and the two index resolvers:

* The board's **first** output is DAC1 (`0x1c18`) / DVO1; the **second** is
  DAC2 (`0x1c1c`) / DVO2 (`HSLPARGetDACIndex .text+0xe2a50`,
  `HSLPARGetDVOIndex .text+0xe2b80`, `HSLPARSetOutputAnalogModeType
  .text+0xd4210`).
* Which display controller drives an output is *not* board-fixed: it is the
  module index `SelectDC` returns, written into the DAC's route field.
  `SelectDC` (`.text+0xba750`) computes it from the output-link flags:
  bit 19 (`0x80000`) selects modules {1,3} over {0,2} and bit 0 selects
  {2,3} over {0,1}.  DAC1 accepts only modules 0 and 1 and raises error
  `0xb1e03131` otherwise (`.text+0xb7672`).
* `PInS[0x1a1]` on this board is `0x00` (`rom0528` file offset `0x79a1`), so
  there is no wiring permutation to apply.

Our own hardware-tested X driver recorded the head pairing that produced a
picture here: `src/parhelia.h:110` - "1 = DVI-1 (`0x1a00`/`0x1b00`, clock B),
0 = DVI-2 (`0x1800`/`0x1900`, clock A)" - and `src/parhelia_ddc.c:326` senses
**DAC1 first** and on a hit sets `dcHead = 1`.  So the working analog
configuration on this board was **controller B / head 1 driving DAC1
(`0x1c18`) with the route bit 14 set**.

## 9. The DVO/serializer clocks, for completeness

PROVEN, `HSLPARSetDVOClock` (`.text+0xaef00`) and `HSLPARPowerUpDVO1Clock`
(`.text+0xd8320`):

| | DVO1 | DVO2 |
|---|---|---|
| clock select | `0x1c00` bits `[14:13]` + bit 27 (3 bits, low pair then high bit) | `0x1c04` bits `[17:15]` |
| clock gate (1 = gated) | `0x1c00` bit 12 | `0x1c04` bit 14 |
| power (active low) | `0x1c24` bit 0 | `0x1c24` bit 1 |
| common block power | `0x1c24` bit 2 | `0x1c24` bit 2 |

`0x1c00 &= 0xf7ff9fff; |= (clk & 3) << 13; |= ((clk >> 2) & 1) << 27;` then
clear bit 12 (`.text+0xd8374`..`+0xd8382`).  `0x1c04 &= 0xfffc7fff;
|= clk << 15;` then clear bit 14 (`.text+0xaf17f`..`+0xaf19a`).  The disable
path sets `0x1c00` bit 12 / `0x1c04` bit 14 (`.text+0xaf1ad`, `+0xaf1ca`).

That is why the vendor's `0x1c00 |= 0x00809020` at init and in
`HSLPARSwitchToHIRES` includes bit 12: it parks DVO1's clock.  Bits 15 and 23
of `0x1c00` are not named anywhere in either binary; they are only ever ORed
in as part of that one constant, so the right thing for our driver is to use
the same constant rather than a subset (our `parhelia_engine.c:79` uses
`0x00808020`).

## 10. The clock path

### 10.1 Which synthesiser bank, and which index feeds which controller

**PROVEN, and it settles the question in the negative for our current code:
`0x0e10 + 4*fs` is the bank for 0x0527/0x0528 *and* for LX 0x2537/0x2538.  The
`0x0e30 + 4*fs` bank is 0x2539-only.**

`HWPARInitialiseFSLimit` (`mtx740 .text+0xe4b20`) computes the register with no
device test at all: `e4b34  lea eax,[rbp*4+0xe10]`, `e4b3e  mov DWORD PTR
[rbx],eax`, where `rbp` is the FS index.  `HWPARWriteFSParameters`
(`.text+0xe4580`) then takes the register out of that struct (`e45ae  mov esi,
DWORD PTR [rsi]`).  The `0x0e30` bank appears only in
`HWS90InitialiseFSController` (`.text+0xe5bb0`, `e5bcd  lea eax,[rbp*4+0xe30]`),
reached only when `WORD [dev+0x29e] == 0x2539`
(`HSLPAREnableClock .text+0xcfd1b`, `+0xcfe7e`).  Same split in the two
`pllGetRegOffset` tables (`.rodata 0x11263c` -> `0xe10/0xe14/0xe18/0xe1c`,
`.rodata 0x112b78` -> `0xe30/0xe34/0xe38/0xe3c`; dispatch at `.text+0xeb1f4`).
Confirmed identical in the 7_0_0 build at `.text+0xe4f24`.

**The FS index is not fixed per head; it is allocated dynamically.**
`HSLPARGetClockSel` (`.text+0xcf8c0`) calls `SelectDCModule` to get 0 (DC A) or
1 (DC B) and passes that as the *n*-th free FS to `HSLPARGetFreeFS`
(`.text+0xc34f0`), then maps the returned index to `clksel = index + 1`
(`+0xcf91e`..`+0xcf9df`).  `HSLPARGetFreeFS` reads `0x0e00` and walks
`fs = 0..3` through `IsFSInUse` (`.text+0xc3400`), collecting the free ones;
DC A takes the first free, DC B the second, and it raises `0xb1c01240` if there
are not enough.  `IsFSInUse` treats an FS as taken when any of the three
system-clock selectors in `0x0e00` point at it:

```
fs0: ((v>>8)&0xf) in {1,2} || ((v>>1)&7)==1 || ((v>>16)&0xf) in {1,2}
fs1: ((v>>8)&0xf) in {3,4} || ((v>>1)&7)==2 || ((v>>16)&0xf) in {3,4}
fs2: ((v>>8)&0xf) in {5,6} || ((v>>1)&7)==3 || ((v>>16)&0xf) in {5,6}
fs3: ((v>>8)&0xf) in {7,8} || ((v>>1)&7)==4 || ((v>>16)&0xf) in {7,8}
```

`0x0e00` layout (PROVEN from `HSLPARGetSystemCLock .text+0xbf370`,
`HSLPARGetGclk +0xb5a30`, `HSLPAREnableTstClk +0xb5b30`, `IsFSInUse`,
`HWPARInitializeSystemClock +0xe5480`): bit 0 / 7 / 15 are per-domain
crystal-park bits, `[3:1]` = MCLK FS select (1..4 -> fs0..fs3), `[6:5]` = GCLK
divider code (0 = /1, 1 = /3, 2 = /2.5, 3 = /2), `[11:8]` = GCLK FS select
encoded `2*fs+1`, `[14:13]` = third-clock divider code, `[19:16]` =
third-clock FS select (`2*fs+1`), `[24:20]` = TSTCLK source (table
`{1,4,7,10}`), bit 29 = TSTCLK enable.
`HWPARInitializeSystemClock` parks everything on the crystal by writing
`0x0e00 = 0x8081` first and restores `0x0e00 = PInS[0x84]` afterwards
(`+0xe5551`..`+0xe5584`, `+0xe56dd`..`+0xe5716`).

Our kernel hardcodes `0x0e18` (fs 2) and `clksel 3` for the whole HSLPAR
family, for both heads.  `clksel = fs + 1` is right
(`HSLPARTranslateDcmClkSelIntoFS .text+0xb5800`, no device test, 1->fs0,
2->fs1, 3->fs2, 4->fs3; identical in 7_0_0 at `+0xb5bf0`), so `clksel 3` does
mean `0x0e18` - but the bank must be allocated against `0x0e00`, not assumed,
or a mode set can reprogram the FS that is feeding MCLK, GCLK or the third
clock.  Note also the two *different* FS encodings: `fs+1` in the DCM
`clksel` field, `2*fs+1` in `0x0e00`'s GCLK/third-clock fields
(`HSLPARTranslateGclkSelIntoFS .text+0xb5760`, table `.rodata 0x1117a0`).

### 10.2 How a synthesiser is programmed

`HWPARWriteFSParameters` (`mtx740 .text+0xe4580`) is the only primitive; every
write is a **plain** write.  PROVEN:

```
1. v = read(reg)
2. v.byte3 = (v.byte3 | 0x80) & 0xb3      -> v |= BIT(31); v &= ~(BIT(26)|BIT(27)|BIT(30))
                                             bits 24,25,28,29 preserved       e45cb/e45ce
   write(reg, v)                                                             e45d9
3. wait 10 us                                                                e45e6
4. pack the divider fields into v, keeping bits [31:22]:                     e45eb..e465c
     [5:0]   = enc6(M)          , M = (param>>24)&0xff
     [10:6]  = (-P) & 0x1f      , P = (param>>16)&0xff
     [15:11] = (-(int8)N) & 0x1f, N = (param>>8)&0xff
     [21:16] = (param&0xff)==0 ? 1 : ((-(((u8)param)>>1)) & 0x1f) << 1
   v |= BIT(31)                                                             e4660
   write(reg, v)                                                            e4669
5. wait 100 us                                                              e4676
6. v &= ~BIT(31); write(reg, v)                                             e467b/e4687
7. wait 200 us                                                              e4698
8. HWPARFSWaitLock(dev, reg)                                                e46a7
```

**The init strobe is bit 31 of the FS register itself; there is no bit-21
strobe** - bit 21 is the top of the post-divider field.  Our kernel's
`0x0e18` writer sets `BIT(31)` (correct) but omits the vendor's first
strobe-only write and its 10 us settle; the P690 writer additionally sets
`BIT(21)`, which on this encoding is a divider bit, not a strobe (that path is
outside this document's scope but worth re-reading).

**The lock indication is `[25:24] == 0b11` of the same FS register**, not bit
24 alone, and the vendor requires at least 90 of 100 samples to read locked
after a 100 us settle: `HWPARFSWaitLock` (`.text+0xe4510`),
`e4550  eax = v.byte3 & 3`, `e4558  if (eax == 3) count++`,
`e4568  *out = (count > 0x59)`.  Identical in 7_0_0 at `+0xe4940`.

Frequency model, from the inverse decoder `HSLPARGetFSFreq` (`.text+0xbf280`):
`M = dec6(reg[5:0])`, `P = (-reg[10:6]) & 0x1f`, `N = (-reg[15:11]) & 0x1f`,
`D = reg[16] ? 2 : 2*((-(reg[21:16]>>1)) & 0x1f)`, and
`f(kHz) = 27000 * M * P / (N * D)` (`bf32c  imul rax,rax,0x6978`).
`HWPARValidateFSParam` (`.text+0xe46d0`) accepts only +-50 kHz of target
(`e4718`), a VCO between 50 000 and 400 000 kHz (`e4776`), the PInS windows at
the limit struct `+0xc/+0x10` and `+0x14/+0x18`, and rejects `M == 0x21`
together with `D == 2`.

PInS shadow offsets for the four synthesisers (relative to `dev+0x3b78`,
PROVEN from `HWPARInitializeSystemClock` and `HWPARInitialiseFSLimit`):
target kHz / pre-baked raw word at `0x24`/`0x28` (FS0), `0x38`/`0x3c` (FS1),
`0x4c`/`0x50` (FS2), `0x60`/`0x64` (FS3); the per-FS limit windows at
`0x2c`, `0x40`, `0x54`, `0x68`; the `0x0e00` shadow word at `0x84`.  When the
raw word is non-zero the vendor uses a shorter gesture with no lock poll:
`write(reg, raw | BIT(31))`, wait 100 us, `write(reg, raw)`
(`+0xe55af`..`+0xe55cf`).

### 10.3 What selects the synthesiser for a controller, and the hold

**PROVEN: bits `[9:6]` of `0x1c00` (controller A) and `0x1c04` (controller B).
`0x1c08` is not a clock mux.**  Every writer uses the mask `0xfffffc3f`:
`HSLPARSelectDisplayControllerClock .text+0xab1d0` and `+0xab2f6`,
`HSLPARSetClockSrcDCMB +0xd7889`, `HSLPARInitFreeController +0xb3030`; the
read-back side shifts by 6 and masks 0xf (`+0xab161`).

Other `0x1c00`/`0x1c04` bits the clock path uses (PROVEN):

| bit | meaning | evidence |
|---|---|---|
| `[1:0]` | divider field, set to `0b10` for normal operation | `+0xab1a7`; `HSLPARSetClkDiv2CRTCA +0xd8bef` |
| 5 | **DCM disable / programming hold** (1 = held) | `HSLPAREnableDCMA +0xd76b3  and ...,0xdf`; `HSLPARDisableDCMA +0xd8a63  or ...,0x20` |
| `[9:6]` | clock select, `fs + 1` | above |
| 10 | cleared while the clock changes | `+0xab1b1  and BYTE PTR [rsp+0xd],0xfb` |
| `[14:13]`, 27 | DVO1 clock sub-select (see §9) | `HSLPARCheckForClkRecycling +0xcf7d1` |
| 12 | DVO1 clock gate | §9 |
| 15 | set by both init paths and by `HSLPARSetDisableDisplayController`; **never cleared anywhere in the binary**, so it is not an enable gate (INFERRED: a clock or output park bit that the vendor simply leaves asserted) | `HSLPARSetDisableDisplayController +0xabca3  or BYTE PTR [rsp+0xd],0x80`, reached only when mode flag bit 26 is set (`+0xabc3d`); `HSLPARSwitchToHIRES +0xbe1f3`; `HSLPARInitializeHardware +0xbf852`. An exhaustive search for any `and`-with-`~BIT(15)` on `0x1c00` finds none. |
| 24 | soft VIDRST strobe | `HSLPARApplyVidReset +0xaffb9/+0xaffd7/+0xafff2` |

Ordering across the whole mode set (PROVEN): the hold (bit 5) is asserted in
`HSLPARSwitchToHIRES` and stays asserted while the FS is programmed by
`HSLPAREnableClock`; it is released at the end of
`HSLPARSelectDisplayControllerClock`, which runs inside
`HSLPAREnableDisplayController` (`.text+0xb1226`) and only for a real mode
change (`HSLPARDetectSmoothTransition` at `+0xb11ae` and
`HSLPARDetectVidParmFreeTransition` at `+0xb1211` skip it otherwise).  The
controller-A body:

```
 ab147  read 0x1c00 -> v
 ab158  if !(v & 0x20) && ((v>>6)&0xf) == clksel: return      (nothing to do)
 ab170  v |= 0x20 ; ab173 write 0x1c00                        assert hold
 ab196  wait 10 us
 ab1a7  v = (v & ~3) | 2 ; ab1b1 v &= ~BIT(10) ; ab1ba write 0x1c00
 ab1d0  vNew = (v & 0xfffffc3f) | ((clksel & 0xf) << 6)
 ab1db  ClientWriteRegisterByte(dev, 0x1fc2, 0x68)            <-- see 10.4
 ab1ec  write 0x1c00 = vNew
 ab1f1  vNew &= ~0x20 ; ab202 write 0x1c00                    release hold
```

Controller B (`+0xab21a`..`+0xab315`) is the same shape on `0x1c04`, with an
extra `0x1c00` bit-20 / `0x1c04` bit-13 variant taken only when the mode's
flag bit 26 (a TV flag) is set, **and with no `0x1fc2` write anywhere**.

`HSLPARDisableClock` (`.text+0xce340`) touches no register at all - it only
zeroes the per-output clock records at `dev + 0x39c0 + 12*idx`.  And
`HSLPARSetClockSrcDCMA` (`.text+0xd7590`), whose plain
`0x1c00 = (clksel & 0xf) << 6` write zeroes every other bit, is a **VGA-restore
gesture**: its only caller chain is `HSLPARReturnToVga ->
HSLPARSwitchToVGAMode`.  It must not be used as a model for the native path.

### 10.4 The VGA MISC clock mux - the speculation was right, with a twist

**PROVEN: the native mode-set path *does* write VGA MISC, exactly once, only
for controller A, and as a plain byte write of the literal `0x68`.**

`HSLPARSelectDisplayControllerClock+0xdb` (`mtx740 .text+0xab1db`):
`mov edx,0x68 / mov esi,0x1fc2 / call ClientWriteRegisterByte`.  Identical in
the 7_0_0 build at `+0xab5b3`.  `0x68` = bits `[3:2] = 0b10` (the
programmable-synthesiser family), bit 5, bit 6, with bits 0 and 1 clear.  The
call sits between the `0x1c00` divider write and the `0x1c00` clock-select
write, i.e. before the DCM mux is switched and before the hold is released.
There is no device-id and no add-on gate on that branch.

Exhaustive accounting of every `0x1fc0`/`0x1fc2`/`0x1fcc` access in `mtx740`:

| function | what it does to MISC | which path |
|---|---|---|
| `HSLPARSelectDisplayControllerClock` `+0xab1db` | plain byte write `0x68` (bits[3:2] = `0b10`) | **native mode set, controller A only** |
| `HSLPARInitFreeController` `+0xb303d` | plain byte write `0x08` | load-sense only (`HSLEnableSyncOnOutput` <- `HSLPARSenseLoadOnInternalDAC` / `HSLPARGetNotRespondingMonitorInfo`) |
| `HSLPARSwitchToVGA` `+0xbd1eb` | plain byte write `0x67` (bits[3:2] = `0b01`, the 28.322 MHz VGA clock) | VGA / legacy |
| `HSLVGAReadVgaInfo` `+0xdf4f9` / `HSLVGAWriteVgaInfo` `+0xdf648` | read `0x1fcc` into the state block / write it back verbatim | save / restore |
| `HSLDACInit` `+0x9ef31` | RMW of bits `[7:6]` only | external-DAC vtable slot, **no caller in this binary** |
| `HSLDACPIXPLLSET` `+0x9f340` | sets bits `[3:2] = 0b11` (legacy MGA pixel PLL) | vtable slot, **no caller** |
| `HSLCRTCUnLock` `+0xee237` | writes the value back unmodified | vtable slot, **no caller** |
| `HSLCRTCSelectHiresMode` `+0xee65b` | sets bit 0, clears bit 1; bits `[3:2]` untouched | vtable slot, **no caller** |

`0x1fc0`/`0x1fc1` (attribute controller) are touched only by
`HSLVGAReadVgaInfo`/`HSLVGAWriteVgaInfo`, i.e. the VGA state save/restore.

So the two claims in our tree are each half right: the comment in
`src/parhelia_mode.c` ("the DC head runs off PLL2 via its display clock
controller, not the VGA-MISC-selected PLL0") is correct about *which* PLL the
controller uses, and correct for the head it was written for - the X driver
drives head 1 / controller B, where the vendor writes no MISC at all.  The
speculative MISC write added to the kernel is also correct - but only on
controller A, which is exactly the head our kernel drives on this board.

### 10.5 Clock-path branches a retail board must not need

On a plain retail Parhelia `[dev+0x4a98] == 0`, so `mtx337` (`.text+0xaa250`)
returns 0 and all of the following are dead code for this board (PROVEN from
the gates named):

* `HSLPARGetPixelClockSourceCaps` (`+0xbe7f0`) returns immediately
  (`be846`/`be84c`) and is in any case called only from `HSLPARGetBoardInfo`,
  never from the mode set.
* `HSLPARUpdateClockInfoStructure` (`+0xce3e0`) performs no MMIO and
  short-circuits at `+0xce470`.
* `HSLPAREnableClock` cases 5..8 are Chrontel TV clocks; cases 2 and 3 do
  nothing; case 4 and case 9 need TV flags or add-on hardware.
* `HSLPARSelectDisplayControllerClock`'s controller-B special branch
  (`+0xab31a`) needs mode flag bit 26 (a TV flag).
* `HSLPARApplyVidReset`'s CEM/FPGA tail (`+0xb0063` onwards).
* `HSLPARSetClockSrcDCMB`, `HSLPAREnableDCMB`, `HSLPARDisableTstClk`,
  `HSLPARSetSystemCLock` and `HSLPARGetPInSSystemCLock` have **no caller at
  all** in the binary.

Finally, `HSLPARPixelClockDetuning` (`+0xcebb0`) shifts the pixel clock by
+-100 kHz and is applied only when `PInS[0x04] > 0x60b` **and**
`PInS[0x1c4] & 2` and the clock exceeds 65 MHz (`+0xcfe29`..`+0xcfe74`).  This
board's PInS is `0x60a`, so it never applies here.

Independently confirmed in `mtxparm`, which has exactly three `0x1fc2` writers
and they line up one-for-one with the Linux HAL:

```
 3e893  shl edi,0x6 ; xor edi,[ebp-4] ; push 0x68 ; push 0x1fc2 ; and edi,0x3c0
        xor DWORD PTR [ebp-0x4],edi                 <- SelectDisplayControllerClock
 43303  push 0x8   ; push 0x1fc2                    <- InitFreeController
 32986  push 0x67  ; push 0x1fc2                    <- SwitchToVGA
```

The `<< 6` / `& 0x3c0` arithmetic straddling the MISC write is the `clksel`
field of `0x1c00`, which removes any doubt about which function this is.

## 11. The display controller (head)

### 11.1 Four controllers, not two

PROVEN, `HSLPARIsDCInUse` (`mtx740 .text+0xab6f0`) probes bit 0 of:

| DC index | enable register | what it is |
|---|---|---|
| 0 | `0x1800` | head A primary |
| 1 | `0x1a00` | head B primary |
| 2 | `0x19a8` | head A second ("video"/overlay) controller |
| 3 | `0x1ba8` | head B second controller |

`SelectDCModule` (`.text+0xba780`) returns the *head* (0 or 1) from mode flag
bit 19; `SelectDC` (`.text+0xba750`) returns `head + 2 * (flags & 1)`, so mode
flag bit 0 selects the second controller of that head.  Register stride is
`head * 0x200`.  The `0x19a8` block being the overlay controller is INFERRED
from `MTXInitXV` (`.text+0x4d3f0`) zeroing `0x19a8`/`0x1888` and
`0x1ba8`/`0x1a88` at server start and from the XV put-image path programming
the whole `0x19a8`/`0x19ac`-`0x19f4` set; that it is DC index 2/3 is PROVEN.

`WORD [dev+0x29e]` is the device id and **`DWORD [dev+0x2a0]` is the PCI
revision** (PROVEN, `HALPGetBoardConfigSpaceInfo .text+0x98056` reads config
dword `0x08` and `+0x98068` stores its low byte there).

### 11.2 `HSLPAREnableDisplayController` (0xb1130), in order

PROVEN at `+0xb1156`..`+0xb1238`:

```
 b1156  module = SelectDCModule(vidparm)
 b11ae  HSLPARDetectSmoothTransition        -> smooth?
 b1211  HSLPARDetectVidParmFreeTransition   -> nothing to reprogram?
 b1226  HSLPARSelectDisplayControllerClock  (only on a real change; see 10.3/10.4)
 b11c9  HSLPARGetDCParameters               (computes; reads 0x1918 and 0x1888)
 b11d7  HSLPARSetDCParameters               (all the head writes)
 b11e7  HSLPARApplyVidParmPatch             (0x1b14 only; never fires on a plain board)
 b1233  HSLPARApplyVidReset(dev, module)    (skipped only for a smooth transition)
 b11f7  HSLPARSetDefaultPriorityLevels
```

### 11.3 `HSLPARSetDCParameters` (0xab850) - the exact write order

`p[0]` = DC index, `p[4]` = head; `B = 0x1800 + head*0x200`, timing block
`B + 0x100`.  PROVEN order:

1. `HSLPARDimController(dev, head, on)` (`+0xab8ba`) ->
   `0x1888 + head*0x200`: `|= 0x82` (bits 1 and 7) with the level byte
   `[31:24]` written as 0 (`+0xab7f8`..`+0xab80d`).
2. RMW: enable register `&= ~1` (`+0xab8ce`).
3. The timing block, **all plain writes**, in this order (add `head*0x200`):

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
| 9 | `0x1918` | **RMW**: `[11:0] = VTOTAL-1`; `[15:12]` and `[31:16]` preserved from a read at `+0xaca39` |
| 10 | `0x1928` | **RMW**: `&= 0x7fffffff`, bit 31 = interlace |
| 11 | `0x191c` | **absolute, not strided** - head A preload count |
| 12 | `0x1b1c` | **absolute** - head B preload count, written on every mode set regardless of which head is being programmed |

4. If the pitch word is non-zero: `0x1834 + head*0x200 = p[0x3c]`, then
   **`0x1800 + head*0x200 = p[0x38]`** (the DC_CTRL enable).
5. `HSLPARDimController(dev, head, off)` -> `0x1888 &= ~0x82`.  The level byte
   is not rewritten on release, so it stays at whatever the assert wrote (0).

Timing derivation, `HSLPARComputeIntermediateVidParm` (`.text+0xabe30`), called
once for H (`esi=1`, `+0xac8d8`) and once for V (`esi=2`, `+0xac932`), with
`D` active, `G` guard, `FP`/`SW`/`BP` front porch / sync width / back porch:

```
TOTAL   = D + 2G + FP + SW + BP
DISPEND = D
BLKSTR  = D + G
BLKEND  = D + G + FP + SW + BP
SYNCSTR = D + G + FP
SYNCEND = D + G + FP + SW
```

Bias: **horizontal values are forced even and then biased by -2**
(`+0xac906`..`+0xac929` `and ...,0xfffffffe`, `+0xac96a  sub edx,0x2; and
dx,0xfff`); **vertical values are biased by -1** (`+0xaca24  dec edx; and
dx,0xfff`); both masked to 12 bits.  Preload counts carry no bias.  With zero
guard band the vendor's six values collapse to our driver's four registers and
our current encodings agree field for field - `0x1908` low = `hdisplay-2`, high
= `htotal-2`; `0x1910` low = `htotal-2`, high = `hdisplay-2`; `0x1920` low =
`vdisplay-1`, high = `vtotal-1`; `0x192c` low = `vtotal-1`, high =
`vdisplay-1`.  **Our timing words are correct.**

**Revision errata, PROVEN** (`+0xaca57`..`+0xacd3f`): on **device id `0x0527`
with PCI revision <= 1 only**, the `0x192c[27:16]` VDISPEND field is written as
`VDISPEND + 9` instead of `VDISPEND - 1`.  It does not apply to `0x0528`.

### 11.4 The DC_CTRL value, and the surface word

PROVEN, `HSLPARGetDCParameters`: `p` is zeroed (`+0xac7ba`), then
`p[0x38]` is built as

```
 acbd6  bit0 = 1                                  DC / primary-surface enable
 acbd9  bit6 = 0
 acbdf  bit7 = 0, bit8 = 0
 acbe5  bit9 = (mode flags & 0x04000000)           primary-LUT bypass
 acbd2  bit11 = interlace
 acd6c  bit9 |= 1 when devid==0x527 && rev<=1 && bpp field == 0xC0000
```

**So for a plain progressive 8/15/16/32-bpp primary mode on 0x0527/0x0528 the
vendor writes `0x1800 = 0x00000001` and nothing else.**  Bit 9 is independently
proven to be the LUT bypass by `HSLPARSetPrimaryLUTBypass` (`.text+0xca0d0`,
`or BYTE PTR [rsp+0x5],0x2`), and **bit 18 is a sticky bandwidth/underflow
flag**: `HSLPARResetBandwidthState` (`.text+0xcf540`) waits 20 ms and then
*clears* bit 18 in `0x1800`, `0x1a00`, `0x19a8` and `0x1ba8`
(`+0xcf5c0`..`+0xcf612`).  Our `PARHELIA_DC_CTRL_ON = 0x00040201` sets bit 9
(bypass) and bit 18 (that fault flag).  The remaining-work note in
`HANDOVER.md` that guessed `0x00040001` was on the right track; the vendor's
value for this case is plainer still.

The surface word, PROVEN `+0xacc2b`..`+0xacc7b`:
`0x1834 = 0x00030000 | (fmt << 24) | (pitch & 0x3fff)`, with `fmt` from
`GetSurfFormat` (`.text+0xac140`): 8 bpp `0x10`, 15 bpp `0x26`, 16 bpp `0x28`,
32 bpp `0x32`, `0x40000` -> `0x2e`, `0xB0000` -> `0x3b`, `0xC0000` -> `0x32`.
This matches our `parhelia_dc_surface_code()` and our `| 0x00030000 | pitch`.
Pitch is in **pixels with 32-pixel granularity** (`HSLPARComputePitch
.text+0xbecc0`: `round_up_32(max(width, displaywidth))`, `SetError(0xb1d0a141)`
if still misaligned).  `HSLPARComputeDSTORG` (`.text+0xbed20`) is a stub.

### 11.5 The scanout base register is narrower on this family

**PROVEN and this is a live defect in our driver.**
`HSLPAR_Register_PriSurfAddr_Set` (`mtx740 .text+0xc5810`):

```
 c5832  bit2 = (field != 0)
 c5840  movzx eax,WORD PTR [rdi+0x29e]
 c5847  sub   eax,0x2537
 c584c  cmp   eax,0x2
 c584f  jbe   c5862
 c5851  and   esi,0x7fff800        <- 0x0527 / 0x0528
 c5862  and   esi,0xffffc00        <- 0x2537 / 0x2538 / 0x2539
```

So `0x1824` on 0x0527/0x0528 carries address bits **[26:11]** - a 2 KiB
granule and a **128 MiB reach** - against bits [27:10] and 256 MiB on
LX/P690.  Bit 2 is the interlace field flag riding in the same register.

The byte address handed to it is pre-masked by `HSLPARGetAddressAlignmentMask`
(`.text+0xacdf0`), whose shift is 13 for 32 bpp (and for the `0xC0000`
10-bit format), 12 for 16 bpp and 11 otherwise, **minus one on LX/P690**
(`+0xace43  sub eax,0x2537; cmp eax,0x3; sbb edx,0x0`):

| bpp field | 0x0527/0x0528 | LX/P690 |
|---|---|---|
| 32 (or `0xC0000`) | `~0x1fff` (8 KiB) | `~0xfff` (4 KiB) |
| 16 | `~0xfff` (4 KiB) | `~0x7ff` (2 KiB) |
| other | `~0x7ff` (2 KiB) | `~0x3ff` (1 KiB) |

Our kernel uses `address & 0x0ffffc00` and a range check of
`> 0x0ffffc00 || !IS_ALIGNED(address, 1024)` for every family.  On the 0528
that silently drops address bit 27 and permits a 1 KiB-aligned base that the
hardware cannot express.  **The front buffer must live below 128 MiB and be
8 KiB aligned at 32 bpp on this chip.**

The window registers our driver already writes are right:
`HSLPARSetDisplayStartParameters` (`.text+0xad170`) writes
`0x1804[27:16] = window X start`, `0x1808[27:16] = X start + W - 1`,
`0x1848[11:0] = Y start`, `0x184c[11:0] = Y start + H - 1`, which for a
full-screen unpanned window is exactly our `0`, `(hdisplay-1)<<16`, `0`,
`vdisplay-1`.

### 11.6 `DC+0x88` on 0x0527/0x0528 - the precise answer

**There is no device-id gate on `0x1888`/`0x1a88` anywhere.  But bits 5:4 are
written only through the *second* display controller, which is selected by
mode flag bit 0, not by the chip family.  On the primary path of any family,
bits 5:4 are never touched; bits 1, 7 and `[31:24]` are.**  PROVEN:

* Bit 5 is set in exactly one place, `HSLPARGetDCParameters+0x549`
  (`.text+0xacd03  or BYTE PTR [rbp+0x48],0x20`), inside a block entered only
  after `test BYTE PTR [rsp+0x84],0x1` (`+0xacc7e`), i.e. mode flag bit 0; and
  the resulting word reaches `0x1888` only in the `p[0] >= 2` branch of
  `HSLPARSetDCParameters` (`+0xab975`..`+0xab987`).
* Bits 4:5 are cleared in exactly one place,
  `HSLPARSetDisableDisplayController+0x1e0` (`.text+0xabd2b  and BYTE PTR
  [rsp+0x8],0xcf`), also only for DC index >= 2.

What the **primary** path does write to `DC+0x88`, on every mode set and on
every family, is `HSLPARDimController` (`.text+0xab790`):

```
 ab7f0  reg = 0x1888 + head*0x200          (head must be 0 or 1)
 ab808  or  BYTE PTR [rsp+0x4],0x82        bits 1 and 7 = dim/fade enable
 ab80d  mov BYTE PTR [rsp+0x7],r13b        bits [31:24] = dim level
 ab845  and BYTE PTR [rsp+0x4],0x7d        clear bits 1 and 7
```

called with "on" at the top of `HSLPARSetDCParameters` (level byte 0) and with
"off" at the bottom.  So the original family's `DC+0x88` gesture is
**`|= 0x82` before the timing writes, `&= ~0x82` after** - a blank-to-black
while the generator is being reprogrammed.  Our kernel's P690-only
`(params & ~0x0082) | 0x0030` write therefore does two different things at
once: clearing `0x0082` matches the vendor's release, while setting bits 5:4 is
the second-controller composite enable that the vendor reaches only through DC
2/3.  On the 0528 our driver writes `DC+0x88` not at all, so if POST left bit 1
or bit 7 asserted with a zero level the head scans out black.

`HSLPARSetOverscanColor` (`.text+0xc5390`) writes `0x188c + head*0x200` and has
no callers.

### 11.7 The vid reset, decoded

**Required, on every family, from inside `HSLPAREnableDisplayController`
immediately after `HSLPARSetDCParameters` (`+0xb1233`), and again at the end of
the mode set from `HSLPARSynchronizeDC` (`+0xb21d0`) with head `-1`.**  It is
skipped only for a smooth transition.  There is no device-id test in
`HSLPARApplyVidReset` (`.text+0xafdc0`); the only gates are add-on-module ones.

Argument: 0 = head A, 1 = head B, `-1` = both (PROVEN from the tests at
`+0xaff38`..`+0xaff98`).  Sequence, head A shown (head B uses `0x1c04`/`0x1b00`):

```
park    b0410  read 0x1c00; only if bit5 is currently set:
        b0439    edx &= ~(BIT(0)|BIT(9)); edx |= 0x1c2 (bits 1,6,7,8)
        b0449    write 0x1c00
        b044e    clear bit5; b045f write 0x1c00 ; remember "bit5 was set"
arm     b0380  0x1900 |= 0x00030000        (head A timing-MISC reset bits 16,17)
        b0350  0x1b00 |= 0x00030000        (head B)
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

So the strobe is **active high, a single 0 -> 1 -> 0 pulse on `0x1c00` bit 24
for either head, with the 10 us delay before the rising edge and none after**.
Our `parhelia_apply_vid_reset()` has the shape right (route bit 4, MISC bits
16/17, strobe bit 24) but it *clears* the hold instead of parking `0x1c00`
with `bits 1,6,7,8` set and `bits 0,9` clear, and it does not restore the hold
afterwards.  Cross-checks: `HSLPARGenerateSoftVidReset` (`.text+0xd8c80`) is the
bare bit-24 clear/set/clear; `HSLPARSoftVidResetCRTCA` (`+0xd8d40`) is the
VGA-path variant and is reached only from `HSLPARReturnToVga`.

### 11.8 DEM request priorities

`0x1c0c` is head A's DEMREQCTL and `0x1c10` head B's; each carries two 3-bit
levels at `[2:0]` and `[10:8]`.  `HSLPARSetPriorityLevels(dev, mask, pA, pB)`
(`.text+0xce500`) is a read-modify-write with `and dh,0xf8`, so it writes only
those two fields per register; `mask` bit 0 -> `0x1c0c[2:0]`, bit 2 ->
`0x1c0c[10:8]`, bit 1 -> `0x1c10[2:0]`, bit 3 -> `0x1c10[10:8]`.

* `HSLPARSwitchToHIRES` calls it with `mask = 5` and both level arrays zeroed
  (`+0xbe11b`/`+0xbe123`), i.e. **`0x1c0c[2:0] = 0` and `0x1c0c[10:8] = 0`** -
  head A parked at level 0 while the DEM is still off.
* `HSLPARSetDefaultPriorityLevels` (`.text+0xad470`) uses `mask = 0xf` and
  `{3,3},{3,3}`, i.e. **all four fields = 3**, and it runs at the end of
  `HSLPAREnableDisplayController` on every mode set.
* `HSLPARUpdatePriorityLevels` (`.text+0xcf630`) then computes levels clamped
  to **1..7** from the per-head bit rates (`HSLPARGetCurrentBitRates` reads
  `0x1834 + head*0x200` bits `[29:24]` for the format), writes both registers
  with `mask = 0xf`, and finishes with `HSLPARResetBandwidthState` (20 ms wait,
  then clear bit 18 of the four DC_CTRLs).
* `HSLPARSetDEMREQCTLtoDCMA`/`...toDCMB` (`+0xd76e0`/`+0xd7720`) plain-write
  `0x1c0c = 0x101` and `0x1c10 = 0x003` and are called only from
  `HSLPARSwitchToVGAMode`.

INFERRED: the priorities are memory-arbitration headroom, not a first-light
gate - the vendor itself runs with `0x1c0c = 0` through the whole transition.
But a head left at level 0 after the mode set has no guaranteed share of DEM
bandwidth, and `HSLPARInitializeHardware` on this family does **not** write
`0x1c0c`/`0x1c10` at all, so the level fields hold whatever POST left.  Our
`parhelia_engine_init()` ORs `0x0808` into both, which sets bits 3 and 11 -
outside the two level fields the vendor ever writes - and leaves the levels
untouched.

`0x1c08` bit 2 is the VGA H-counter mode (`HSLPARSetDEMVGAHCntMode
.text+0xd66f0`, called only from `HSLPARReturnToVga`).

### 11.9 The "intermediate module", LUT, zoom and sync

`HSLPAREnableIntermediateModule` (`.text+0xb1de0`) performs **no MMIO of its
own**; it dispatches on mode flags to `HSLPAREnableVDP` + `HSLPAREnableTVE`
(flag bit 26), `HSLPAREnableDVO(0)`, `HSLPAREnableDVO(1)`, or
`HSLPAREnableCEM`.  **On a plain retail 0527/0528 with a CRT on a DAC it and
its Disable twin are complete no-ops**; the `0x1c18 |= 0x1400` /
`0x1c1c |= 0x1400` tails inside it are behind
`dev[0x4a98] && dev[0x4a9c] in {0xa,0x10}`.

`HSLPARApplyLUT` (`.text+0xcd070`) also performs no MMIO - it manipulates the
per-DC shadow at `dev + 0x3e0 + dc*0x808` and then calls
`HSLPARSetPrimaryLUT` unless mode flag bit 20 is set.  `HSLPARSetPrimaryLUT`
(`.text+0xcc2a0`) writes the quad `0x1838`, `0x183c`, `0x1840`, `0x1844`
(+`head*0x200`) with values masked `0xffc0`; the DC 2/3 quad is
`0x19b8`..`0x19c4` masked `0xff00`.

`HSLPARSetModeZoom` (`.text+0xce0b0`) touches `0x1900 + (SelectDC() << 9)`
bits `[12:11]` (H zoom code) and `[14:13]` (V zoom code), writing only when the
code changes; `log2(zoom)` encoding.  Note it uses `SelectDC` rather than
`SelectDCModule`, so for DC 2/3 it would address `0x1d00`/`0x1f00` - a latent
vendor bug, harmless because zoom is only used on DC 0/1.

`HSLPARSetCRTC` (`.text+0xbe640`) is a **stub with no callers**: there is no
legacy VGA CRTC programming anywhere on the native path.

`HSLPARSynchronizeDC` (`.text+0xb2150`) returns at once if mode flag bit 20 or
bit 0 is set; otherwise it compares this head's width, height and pixel clock
against the other head's stored copy and, if they match, applies a vid reset to
**both** heads - the dual-head genlock.

### 11.10 The preload counts, and why writing zero is what the vendor does

`HSLPARGetCrtcPreloadCount` (`.text+0xac5c0`) returns zero for both heads
unless *all* of the following hold (PROVEN `+0xac611`..`+0xac6b2`): the genlock
flag `dev[0x3ab] & 0x20` is clear, the other head is in use, the two heads'
pixel-format fields differ, and the other head's pixel clock exceeds
`0x5572f` kHz (350 MHz).  When they do hold, the count is
`a + b + (a >> 1)` where `a` and `b` are `1024 / bytes_per_pixel` of the two
heads.  On a single-head driver the counts are always 0 - but
`HSLPARSetDCParameters` writes `0x191c` and `0x1b1c` unconditionally from the
zeroed parameter block, so the vendor **explicitly writes 0 to both on every
mode set** rather than inheriting POST values.

### 11.11 Legacy VGA registers on the native path - full accounting

Decoder map: `0x1fc0`/`0x1fc1` attribute index/data, `0x1fc2` MISC write,
`0x1fc4`/`0x1fc5` sequencer index/data, `0x1fc6`-`0x1fc9` DAC mask and palette,
`0x1fcc` MISC read, `0x1fce`/`0x1fcf` graphics index/data, `0x1fd4`/`0x1fd5`
CRTC index/data, `0x1fda` input status 1, `0x1fde`/`0x1fdf` Matrox CRTCEXT
index/data.  Reachable from `HSLPARSetMode`:

| register | function | verdict |
|---|---|---|
| `0x1fc2` (MISC write) | `HSLPARSelectDisplayControllerClock+0xdb` | **on the native path, head A only** (§10.4) |
| `0x1fda` (read) | `HSLPARVGAWaitForVerticalRetrace` (`.text+0xbd420`), called twice by `HSLPARSwitchToHIRES` | **on the native path**, and itself gated on `0x1c08` bit 1 being clear (`+0xbd45f`), i.e. it only spins while still in VGA; polls bit 3 with a 1 000 000-iteration cap |
| everything else | see below | not reachable from `HSLPARSetMode` |

* **VGA sequencer SR01 bit 5 (screen off): not on the native path.**  The only
  live user is `HSLPARPM_DPMS`, and it is explicitly gated on VGA still being
  enabled: `+0xbc6d5  read 0x1c08`, `+0xbc78e  test BYTE PTR [rsp+0x14],0x1`,
  `+0xbc793  je` - skipping the SR01 poke entirely in native mode.  Every other
  SR01 writer (`HSLDACScreenState/Off/On`, `HSLCRTCEnable/Disable`) has **no
  caller in the binary**.
* **Attribute-controller palette-address-source (index 0x20 at `0x1fc0`): not
  on the native path.**  `0x1fc0` appears only in `HSLVGAReadVgaInfo` /
  `HSLVGAWriteVgaInfo`, the legacy state save/restore around
  `MTXSaveVgaState` / `MTXRestoreVgaState`.
* The whole `HSLCRTC*` layer (`HSLCRTCSet`, `...Lock`, `...UnLock`,
  `...SetDisplayStart`, `...SetPitch`, `...SelectHiresMode`,
  `...SetPriorityRequest`, `...Disable`, `...Enable`) and the `HSLDAC*` layer
  (`HSLDACInit`, `HSLDACPIXPLLSET`, `HSLDACSetZoom`) are **dead code** in this
  binary - no callers - and are the G-series HAL left in place.

So our `parhelia_vga_unblank()` is not something the vendor does on this
family.  It is harmless (POST leaves SR01 bit 5 clear because the text console
is visible), but it is not a requirement and the attribute-controller write is
not part of any vendor path.

## 12. DDC on this board: the missing channel select

The DDC bit-bang register is `0x1c48` for all four lines on every Parhelia
variant, with data/readback at `1 << pin` and an open-drain output enable at
`1 << (pin + 16)`, exactly as `parhelia_ddc.c` implements it.  Two corrections
and one omission came out of the vendor resolver.

**The pin table.**  `HSLPIN60GetDDCIORegMasks` (`mtx740 .text+0xda430`) reads a
four-byte table: byte index `i` (0..3) **is the GPIO bit** and its value is the
logical pin id.  The table is `PInS[0x1b4]` when the PInS version word at
`PInS[0x04]` is `> 0x609`, else the built-in `0x05070608`
(`+0xda45a`..`+0xda4ce`).  `HSLPARCSetDefaultPinsS90` (`.text+0xdcaa0`)
installs `0x03040102` as the LX/P-series default (`+0xdcccf`), so the P690's
table is a silicon-family default and not a property of that particular ROM.

For **this board** `PInS[0x1b4] = 0x05070608` (`rom0528` file offset `0x79b4`,
bytes `08 06 07 05`), so id 8 -> bit 0, id 6 -> bit 1, id 7 -> bit 2,
id 5 -> bit 3, and the line resolver (`HSLPARGetDDCIORegMasks_DDCLINE1..4`,
`.text+0xb82c0`/`+0xb8160`/`+0xb8030`/`+0xb7f00`) lands on:

| DDC line | port id | SCL | SDA | mux level |
|---|---|---|---|---|
| 1 | `0` | id 5 -> **bit 3** | id 6 -> **bit 1** | 0 |
| 2 | `5` | id 7 -> **bit 2** | id 8 -> **bit 0** | 0 |
| 3 | `0x11` | bit 3 | bit 1 | **1** |
| 4 | `0x12` | bit 2 | bit 0 | **1** |

That confirms `parhelia_ddc.c`'s `cand_orig[]` and our kernel's descriptor pin
pairs, and it means **the stuck-low `0x1c48` bit 3 is precisely connector 1's
SCL**.  Lines 3 and 4 are the same two pad pairs at the other mux level, so
trying them with the mux unchanged is pointless - which the status document
had already guessed for the wrong reason.

**There is a fifth output nobody had noticed, and it is the answer.**
`HSLPARGetDDCIORegMasks` (`.text+0xb8420`) initialises **five** outputs, the
last pre-set to `-1` (`+0xb846f  mov DWORD PTR [r9],0xffffffff`), and each
line helper writes a **mux level** into it.  `HSLPARI2COpenPort`
(`.text+0xc1c40`) then drives that mux through the MISC-IO GPIO before any
transaction (`+0xc1eda`..`+0xc1eee`):

```
 c1eda  r8 = &muxSel
 c1ee4  edx = 5                 MISC-IO logical id 5
 c1ee9  esi = 0                 lookup by id
 c1eee  call HSLPARAccessMiscIo
```

`HSLPARAccessMiscIo` (`.text+0xc1970`) tries id 5, and if that id is not
declared by the board it falls back to **id 8 with inverted polarity**
(`+0xc1b1a`..`+0xc1b5b`, `xor r12d,r12d`).  `HSLPIN60GetMISCIORegMasks`
(`.text+0xda150`) resolves a MISC-IO id to **register `0x1c4c`, data
`1 << pin`, output enable `1 << (pin + 10)`** for pins 0..8
(`+0xda241`/`+0xda252`), with the id table in `PInS[0x188]` (nibbles for pins
0..7, default `0x43706021`) and `PInS[0x18c]` (nibbles for pins 8..10); pins 9
and 10 live in register `0x0308` instead (`+0xda2b8`).

On **this board**: `PInS[0x188] = 0x007a6921` and `PInS[0x18c] = 0x00000008`
(`rom0528` file offsets `0x7988`, `0x798c`).  Id 5 is **absent**; **id 8 is at
MISC-IO pin 8**.  So the required write is

```
0x1c4c |= 0x00040000      /* OE  = 1 << (8 + 10) */
0x1c4c |= 0x00000100      /* data = 1 << 8, mux level 0, inverted polarity */
```

for ports 0 and 5, i.e. for both physical connectors; `& ~0x00000100` selects
mux level 1 (ports `0x11`/`0x12`).  `HSLPARI2CClosePort` (`.text+0xc2300`)
drives it back.

**The board's own VBIOS does exactly this**, which makes it as close to proof
as static analysis gets.  `rom0528` file offsets `0x4c26`-`0x4cf9` are the
VBIOS's OpenPort: `0x4c52  mov ax,0x208` (AL = `0x08`, SCL bit 3; AH = `0x02`,
SDA bit 1) for lines 0 and 2, `0x4c6a  mov ax,0x104` (SCL bit 2, SDA bit 0) for
lines 1 and 3, `or di,0x100` to request mux level 1, then `0x4cca  mov bl,0x5`
-> lookup, `0x4cd4  or di,0x1` when id 5 is missing (the inverted-polarity
flag), `0x4cd7  mov bl,0x8` -> lookup, `0x4ce8  call 0x4d6a`.  The writer at
`0x4d6a` is `mov si,0x1c4c`, `shl edx,0xa`, `or ecx,edx`, then data set or
cleared according to the polarity flag - independently confirming both the
register and the `+10` output-enable offset.  The id lookup at `0x4cfa` reads
`PInS+0x188` and `PInS+0x18c`, the same two tables.

**Why the P690 never needed it:** the S90 default table `0x03040102` declares
neither id 5 nor id 8, so `HSLPARAccessMiscIo` finds no pin and the write is a
no-op on that family.  That is exactly the asymmetry in the bug report - the
same driver code reads EDID on a P690 and not here.

Three further corrections to the DDC documents, all PROVEN:

* **`0x1c48` bits 8..15 are not a second GPIO bank.**  The VBIOS's
  "high-byte" helpers take a pre-shifted mask but still sample and clear pads
  0..7 (`rom0528 0x4eba  and eax,0xff00; shl eax,8` then `0x4ee1  shr dx,0x8;
  test dx,cx`).  The two conventions exist because the VBIOS packs SCL in AL
  and SDA in AH of one word.  Neither binary ever touches `0x1c48[15:8]`.
  The untested-second-bank hypothesis in `parhelia-ddc-complete.md` §1 should
  be deleted.
* **`HSLPARI2COpenPort` accepts only port ids `0`, `1`, `5`, `0x10`, `0x11`,
  `0x12`, `0x85`, `0x92`**; anything else raises `0xb1d05120`
  (`.text+0xc1d56`).  Ports 1 and `0x10` are the `0x1c4c` MISC-IO I2C buses
  (SiI164 and friends, slave addresses `0x70`/`0x76`/`0x72`), gated on
  `PInS[0x88]` bits 2 and 3 - `0x0000e1f4` on this board, so port 1 is enabled
  and port `0x10` is not.  The literal `0x1c4c` masks in
  `parhelia-ddc-complete.md` §8 belong to port `0x10` and are additionally
  gated on **device id `0x0527` with revision <= 1** (`.text+0xc1da5`), which
  a 0528 rev 06 never takes.
* **`PInS[0x1a1]` is the add-on-module type enum, not a routing byte.**  Its
  only readers are `HSLPARDetectCEM` (`.text+0xc0bea`, `+0xc0c14`, gated on
  `PInS[0x04] > 0x608`) and `HSLPIN60InitCEMStructure` (`+0xdaaec`), and
  DetectCEM compares it against exactly the constants that
  `HSLPARGetDVOIndex`/`HSLPARGetDACIndex` later test on `[dev+0x4a9c]`.  On
  this board it is `0x00`, so every add-on-gated path is dead.
  `PInS[0x1a8]` is the TV-encoder GPIO id table (sole reader
  `HSLPIN60GetCHRGPIOData .text+0xda543`).

Connector to DDC line, PROVEN from `HSLPARGetMonitorEDID` (`.text+0xd36f0`):
output index 0 -> port `0` -> line 1; 1 -> port `5` -> line 2; 2 -> port
`0x11`; 3 -> port `0x12`.  `HSLPARGetAnalogMonitorEDID` (`.text+0xc1760`)
additionally swaps output 0 between port 0 / DAC index 0 and port 5 / DAC
index 1 on `dev[0x3a8]` bit 4.  Detection order is 0, 1, 2, 3
(`HSLPARGetAllMonitorInfo .text+0xd5ad0`).

Nothing else is a DDC pre-condition.  `0x1c40` and `0x1c44` are referenced
zero times in either binary and in the VBIOS; the G200/G400 `0x1e00` GENIO
block does not exist on this part; `0x1e18` is only the hot-plug interrupt
clear; `HLSPARI2CSetClockState`/`RestoreClockState` (which do touch `0x1c00`,
`0x1c04` and `0x1c24`) are called only from
`HSLPARAccessCEM_FPGAI2CRead`/`Write`; and `HSLPAREnableCEMDDCSel2` reduces to
a no-op on this board because `HSLParCEMSpSetMiscIO` (`.text+0xe1b00`) bails
when `PInS[0x04] <= 0x60a` (`+0xe1b3b`) - and this board is exactly `0x60a`.

## 13. Per-revision differences

Family membership as the vendor tests it: `WORD [dev+0x29e]` must be in
`{0x0527, 0x0528}` or `{0x2537, 0x2538, 0x2539}` or `HSLInitDeviceJumpTable`
fails (`.text+0x99a93`).  Gates seen in the output path are of exactly three
shapes: `devid - 0x527 <= 1` (the original family), `devid - 0x2537 <= 2`
(all three LX/P-series parts) and `devid == 0x2539` (P690 only).

| Area | 0x0527 / 0x0528 | 0x2537 / 0x2538 (LX) | 0x2539 (P690) |
|---|---|---|---|
| Synthesiser bank | `0x0e10 + 4*fs` | **same** | `0x0e30 + 4*fs` |
| Synthesiser writer | `HWPARWriteFSParameters` | same | `HWS90InitialiseFSController` |
| DCM clock select | `0x1c00`/`0x1c04` bits `[9:6]`, `clksel = fs + 1` | same | same |
| VGA MISC write on the controller-A clock change | **yes, `0x1fc2 = 0x68`** | same (no device gate) | same |
| VGA <-> HIRES mux | `0x1c08` bit 0 / bit 1 | same | same |
| `0x1c58 = 0x40` at init and on the VGA->HIRES transition | **unconditional** | only when the `PInS[0x90..0x97]` board test passes; `0x2537` also needs `dev[0x2a0] == 1` | same as LX |
| `0x1c50` | `PInS[0x19c]` if `PInS[0x04] > 0x607`, else `0x606` | same | same |
| `0x1c00 \|= 0x00809020`, `0x1c04 \|= 0x20`, `0x1c24 \|= 7` at init | yes | yes | yes (via `HSLS90InitializeHardware`) |
| Scanout base field (`0x1824`) | **bits `[26:11]`, mask `0x07fff800`** | bits `[27:10]`, mask `0x0ffffc00` | same as LX |
| Scanout byte alignment at 32 bpp | **8 KiB** | 4 KiB | 4 KiB |
| Timing registers and bias | `0x1908`/`0x1910`/`0x1914`/`0x1918`/`0x1920`/`0x1928`/`0x192c`/`0x1930`/`0x1934`, H `-2` and even, V `-1` | same | same |
| `VDISPEND` errata | **`+9` instead of `-1` on `0x0527` rev <= 1 only** | - | - |
| DC_CTRL for a plain progressive mode | `0x00000001` | same | same |
| Surface word `0x1834` | `0x00030000 \| fmt<<24 \| pitch` | same | same |
| `DC+0x88` on the primary path | `\|= 0x82` then `&= ~0x82` (dim assert/release) | same | same |
| `DC+0x88` bits 5:4 | second-controller composite only, selected by mode flag bit 0 - **not a family difference** | same | same |
| Vid reset | `0x1c00` bit 24 strobe, bit 4 route, `0x1900`/`0x1b00` bits 16/17 | same | same |
| DEM priorities | `0x1c0c`/`0x1c10`, `[2:0]` and `[10:8]`, default 3; init does **not** write them | same | `HSLS90InitializeHardware` does write them |
| Analog DAC block | `0x1c18`/`0x1c1c`, **no device-id gate anywhere** | identical | identical |
| Integrated DVO format/crossbar | **`0x1c20`** byte 1 (DVO1) and byte 3 (DVO2) | `0x1c54` / `0x1c58` | `0x1c54` / `0x1c58` |
| DVO1 clock | `0x1c00` bits `[14:13]` + bit 27, gate bit 12 | same | same |
| DVO2 clock | `0x1c04` bits `[17:15]`, gate bit 14 | same | same |
| DVO power | `0x1c24` bits 0, 1, 2 (active low) | same | same |
| DVO board config | `PInS[0x190]`/`[0x194]`; `0x0000001f`/`0` on this board | `PInS[0x190]`/`[0x194]`; `0xaac00000`/`0x10` on the tested P690 board | same |
| DDC pin table | `PInS[0x1b4]` if `PInS[0x04] > 0x609`, else `0x05070608` | built-in S90 default `0x03040102` | same |
| DDC channel select on `0x1c4c` | **required** (MISC-IO id 5, falling back to id 8 inverted; pin 8 on this board) | not declared in the S90 default table, so a no-op | same |
| Sense-load test registers | `0x188c + sel*0x200` and `0x1910 + sel*0x200`, result in `0x1c14` | same | same |

The important negative result: **the analog DAC block is identical across all
five parts.**  `HSLPAREnableDisplayOutputs`, `HSLPAREnableDAC1/2`,
`HSLPARDisableDAC1/2`, `HSLPARSetDAC1/2OnDCMA`, `HSLPARSetSyncPolDAC` and
`HSLPAREnableDacSyncOnly` contain zero `[dev+0x29e]` comparisons between them.
Whatever is wrong with the analog output on the 0528 is therefore not a
DAC-block difference; it is the head, the clock, the routing or the board.

## 14. Implementation checklist for the kernel driver

Ordered by how likely each item is to be the reason an analog monitor sees no
signal on this board.  Nothing in this list has been tested on hardware.

1. **Drive the connector the monitor is actually on, with the DAC the vendor
   pairs with it.**  `HSLPARSetOutputAnalogModeType` (`.text+0xd4210`) and
   `HSLPARGetDACIndex` (`+0xe2a50`) prove the board's first output is **DAC1
   (`0x1c18`)** and its second is DAC2, and our own hardware-tested X driver
   lit this board from **head 1 / controller B into DAC1 with route bit 14
   set** (`src/parhelia_ddc.c:281`, `:326`, `src/parhelia.h:110`).
   `parhelia_original_outputs[0]` in `parhelia_kms.c:1143` declares the
   opposite - `preferred_head = 0`, `analog_route = PARHELIA_ROUTE_DAC2`,
   `digital_route = PARHELIA_ROUTE_DVO2` - and because the single pipe can only
   drive `outputs[0]`, the driver always lights the *other* connector while DRM
   names it `DVI-I-1`.  Swap the two descriptor entries so index 0 is
   DAC1/DVO1/head 1, and make the pipe take its output from the atomic state
   rather than the array index (already the largest open item in
   `parhelia-0528-posted-driver-status.md` §6).
2. **Write the VGA MISC clock mux when programming controller A's clock.**
   `HSLPARSelectDisplayControllerClock+0xdb` (`.text+0xab1db`) issues a plain
   byte write `0x1fc2 = 0x68` on the controller-A branch of every real mode
   change, and nothing on the controller-B branch.  Our
   `parhelia_program_clock()` writes VGA MISC only in the `PARHELIA_CLOCK_SUN90`
   case, so on the 0528 - where we drive controller A - the mux keeps whatever
   POST left, which for a VGA console is the 28.322 MHz legacy clock
   (`HSLPARSwitchToVGA` writes `0x67`, bits `[3:2] = 0b01`).  A synthesiser can
   report lock while the head is clocked from something else entirely, which is
   exactly the reported symptom.  This also explains why the X driver's comment
   ("the DC head runs off PLL2 ... not the VGA-MISC-selected PLL0") and the
   speculative kernel write are *both* right: the X driver drove head 1.
   Place the write between the `0x1c00` divider write and the `0x1c00`
   clock-select write, with the hold (bit 5) still asserted, as the vendor does.
3. **Program the DEM request priorities on every mode set.**
   `HSLPARSetDefaultPriorityLevels` (`.text+0xad470`) writes `0x1c0c` and
   `0x1c10` fields `[2:0]` and `[10:8]` to 3 at the end of
   `HSLPAREnableDisplayController`, and `HSLPARSwitchToHIRES` deliberately
   parks head A's two fields at **0** on the way in
   (`HSLPARSetPriorityLevels(dev, 5, &0, &0)`, `.text+0xbe12b`).  Our
   `parhelia_engine_init()` ORs `0x0808` into both registers, which sets bits 3
   and 11 - outside the only two fields the vendor ever writes - and leaves the
   level fields at their POST values.  Write the level fields explicitly,
   read-modify-write, `[2:0] = [10:8] = 3`, after the head is programmed.
4. **Release the `DC+0x88` dim bits on the original family.**
   `HSLPARDimController` (`.text+0xab790`) asserts `0x1888 + head*0x200 |=
   0x82` with the level byte `[31:24]` zeroed before the timing writes and
   clears `&= ~0x82` after, on every family.  Our driver writes `DC+0x88` only
   in the `PARHELIA_CLOCK_SUN90` branch, so on the 0528 a POST-asserted dim at
   level 0 is never released.  (This produces a black picture with sync
   present, so it is a secondary suspect for "NO SIGNAL", but it is
   unconditional vendor behaviour.)  Do **not** copy the `| 0x0030` half of our
   P690 write to this family: bits 5:4 are the second/overlay controller's
   composite enable, reachable only through DC index 2/3
   (`HSLPARGetDCParameters+0x549`, gated on mode flag bit 0).
5. **Write the DAC control word as fields, not as a partial mask.**  DAC2's
   source select is the **two** bits `[15:14]` (0 = controller A, 1 =
   controller B, 3 = DC module 3, and bit 15 alone = the TV/CVE clock source),
   and **bit 9 is a TV/composite-sync mode bit that the vendor clears
   unconditionally for a monitor output on both DACs**.  Our
   `PARHELIA_DAC_ROUTE_DCMB` touches only bit 14 and `PARHELIA_DAC2_PWR`
   (`0x00031400`) does not include bit 15, so nothing in our driver ever clears
   `0x1c1c` bit 15 or bit 9 on either DAC.  A POST that used DAC2 for a TV or
   for module 3 leaves the analog output silent.  Also program the three 3-bit
   per-channel trim fields in `[8:0]` from `PInS[0x174]` as the vendor's second
   DAC write does (§5.2); on this board that is `0x1c18[8:0] = 0x036` and
   `0x1c1c[8:0] = 0x161`.
6. **Use the vendor's DC_CTRL value.**  For a plain progressive primary mode on
   0x0527/0x0528 `HSLPARGetDCParameters` builds `0x1800 = 0x00000001`; bit 9 is
   the primary-LUT bypass and bit 18 is the sticky bandwidth-fault flag that
   `HSLPARResetBandwidthState` *clears*.  Our `PARHELIA_DC_CTRL_ON =
   0x00040201` sets both.  This closes `HANDOVER.md` remaining-work item 11.
7. **Use the vendor's `0x1c00` init constant.**  Both vendor binaries OR
   `0x00809020`; `parhelia_engine.c:79` ORs `0x00808020`.  The missing bit 12 is
   DVO1's clock gate (`HSLPARDisableDVO1 .text+0xd732f`,
   `HSLPARPowerUpDVO1Clock .text+0xd8382`), so our init leaves DVO1's clock
   running.  Bits 15 and 23 are unnamed in both binaries and are never cleared
   anywhere, so match the constant rather than a subset.
8. **Write `0x1c50`.**  `HSLPARInitializeHardware+0x15f` writes
   `PInS[0x19c]` when the PInS version exceeds `0x607`, else `0x606`.  This
   board is PInS `0x60a` with `PInS[0x19c] = 0x08000707` (`rom0528` file offset
   `0x799c`).  Our `parhelia_engine_init()` never writes it.
9. **Fix the scanout base encoding for this family.**  `0x1824` carries bits
   `[26:11]` on 0x0527/0x0528 (mask `0x07fff800`, 128 MiB reach) and the byte
   address is pre-aligned to **8 KiB** at 32 bpp, against `[27:10]` and 4 KiB on
   LX/P690 (`HSLPAR_Register_PriSurfAddr_Set .text+0xc5851`/`+0xc5862`,
   `HSLPARGetAddressAlignmentMask .text+0xace43`).  Our
   `parhelia_scanout_address()` uses `0x0ffffc00` and a 1 KiB alignment check
   for every family, so a front buffer above 128 MiB silently loses bit 27.
   Keep the display buffer in the low 128 MiB on this chip.
10. **Allocate the synthesiser instead of hardcoding `0x0e18`.**
    `HSLPARGetFreeFS` (`.text+0xc34f0`) reads `0x0e00`, marks every FS that
    MCLK, GCLK or the third clock already selects (`IsFSInUse .text+0xc3400`),
    and hands controller A the first free index and controller B the second;
    `clksel = index + 1`.  `parhelia_program_clock()` always uses `0x0e18` and
    `clksel 3` for both heads, which risks reprogramming a system clock.  The
    bank is right for LX too - only 0x2539 uses `0x0e30 + 4*fs`.
11. **Match the synthesiser programming gesture and the lock test.**  The
    vendor writes the strobe first on its own (`v |= BIT(31)`, bits 26/27/30
    forced clear, bits 24/25/28/29 preserved), waits 10 us, writes the packed
    divider word with the strobe still set, waits 100 us, clears the strobe,
    waits 200 us, and then requires **`[25:24] == 0b11` in at least 90 of 100
    reads** (`HWPARWriteFSParameters .text+0xe4580`, `HWPARFSWaitLock
    .text+0xe4510`).  Our writer skips the strobe-only write and its 10 us
    settle; check what `parhelia_fs_locked()` actually tests.
12. **Preserve the fields the vendor preserves in the timing block.**  `0x1918`
    is a read-modify-write that keeps `[15:12]` and `[31:16]`, and `0x1928` is a
    read-modify-write of bit 31 alone; our driver plain-writes `vtotal-1` and
    `0` respectively.  And the vendor writes `0x191c` and `0x1b1c` (the CRTC
    preload counts, at absolute addresses, both of them) explicitly as 0 on
    every single-head mode set rather than inheriting POST values
    (`HSLPARGetCrtcPreloadCount .text+0xac5c0` only produces a non-zero count
    for two active heads with different formats and the other head above
    350 MHz).
13. **Drive the DDC channel select before any DDC transaction.**  `0x1c4c` is
    the MISC-IO GPIO (data `1 << pin`, output enable `1 << (pin + 10)`, pins
    0..8), and `HSLPARI2COpenPort` resolves MISC-IO id 5 - falling back to id 8
    with inverted polarity - and drives it to the line's mux level before
    touching `0x1c48`.  This board declares id 8 at pin 8
    (`PInS[0x18c] = 0x00000008`) and no id 5, so both connectors need
    `0x1c4c |= 0x00040000` (OE) and `|= 0x00000100` (data, mux level 0), as a
    read-modify-write that leaves bits 0, 1, 3, 4 and their enables at 10, 11,
    13, 14 alone.  The board's own VBIOS does exactly this
    (`rom0528 0x4cca`..`0x4d6a`).  The S90 default id table declares neither
    id, which is why the same code reads EDID on a P690.  That is very probably
    why `0x1c48` bit 3 - connector 1's SCL - reads permanently low.
14. **Add the vertical-retrace waits around the VGA -> HIRES transition** and
    split it into `0x1c08 &= ~1`, 10 us, `0x1c08 |= 2` rather than a single
    plain write of `2` (`HSLPARSwitchToHIRES .text+0xbe101`..`+0xbe17f`).
15. **Park `0x1c00` properly across the vid-reset strobe.**  When the hold
    (bit 5) is asserted the vendor first writes `(v & ~(BIT(0)|BIT(9))) |
    0x1c2`, then clears bit 5, strobes, and restores bit 5 afterwards
    (`HSLPARApplyVidReset .text+0xb0410`..`+0xb0464`, `+0xb01a3`).  Our
    `parhelia_apply_vid_reset()` only clears the hold and never restores it.
16. **`parhelia_vga_unblank()` can go.**  Neither the VGA sequencer's SR01
    bit 5 nor the attribute controller's palette-address-source is written by
    any vendor path reachable from `HSLPARSetMode`; the only live SR01 writer
    is `HSLPARPM_DPMS`, and it skips the write when `0x1c08` bit 0 is clear
    (`.text+0xbc78e`).  Keeping it is harmless but it is not the missing piece.
17. **Two more things that are already right, so they need no work:** our
    timing word encodings agree with the vendor field for field (§11.3), and
    our `parhelia_output_load_sense()` matches
    `HSLPARSenseLoadOnInternalDAC` exactly - including the `sel * 0x200`
    offset, the `0x16459164` test value, the `0xf000ffff` mask and the 10 ms
    settle.  One caveat: the vendor's sense takes a different branch depending
    on whether `0x1c08` bit 0 (VGA) is still set (`.text+0xc0ef8`), so a sense
    taken before the VGA->HIRES switch is not the same measurement as one
    taken after.

### The one-change experiments worth running first

Items 1 and 2 are independent and each on its own would explain the symptom.
The cheapest discriminator is that they predict different outcomes:

* If the cause is item 1, then forcing the pipe onto the *other* connector -
  head 1 and DAC1 (`0x1c18`), with `0x1c18` bit 14 set - produces a picture
  with no other change, because controller B needs no MISC write.
* If the cause is item 2, then keeping head 0 and adding only
  `0x1fc2 = 0x68` produces a picture on the connector currently being driven.

Run them separately, in that order, and the answer falls out of which one
lights the monitor.


## Corrections from hardware measurement (2026-09-13)

* **DVO2 clock (sections 9 and 13):** on 0x0527/0x0528 both serializers take
  their clock from `0x1c00` `[14:13]` + bit 27 with the gate at bit 12.  The
  `0x1c04` `[17:15]` / bit 14 branch in `HSLPARSetDVOClock` is unreachable for
  `devid - 0x527 <= 1` (mtx926 `aef3c`/`aefd0`/`af14d`), the board's VBIOS clock
  routine (ROM `0x9e37`) writes only `0x1c00` for both, and writes to
  `0x1c04[17:15]` are dropped by the hardware.
* **Connector wiring (sections 8 and 14):** measured on the retail board, DDC
  pins 3/1 carry DVO1/DAC1 and pins 2/0 carry DVO2/DAC2; §14 item 1's
  derivation from the index resolvers does not match the hardware.
* **Crossbar sync bits (section 6):** the 0x0528 enable path forces bits 3 and
  5 of the serializer byte clear (`or $0x14; and $0xffffffd7`); the VBIOS only
  touches them from a separate polarity routine.
* **Enable order:** both the HAL and the VBIOS gate the DVO clock and power the
  serializer down, write the crossbar byte, then ungate and power up, with the
  display controller already enabled.
* **`0x1c58` bit 6 (line ~243):** on this family `HSLPAREnableDVO` sets it
  (`b0b59 -> b0db1`); the clear at `b0b5f` needs `board_flag != 0`, which is
  always 0 for 0x0527/0x0528.

## 15. The DVO ports end in external Chrontel CH7301C transmitters (2026-10-03)

Correction to sections 6, 9 and 13: on this board the "integrated serializer"
registers (`0x1c20` crossbar byte, `0x1c00` DVO clock, `0x1c24` power) drive the
**DVO port**; the TMDS link itself comes from an external **Chrontel CH7301C**
per connector on the MISC-IO (`0x1c4c`) I2C bus.  Its TMDS PLL must be retuned
for every pixel clock, which is why DVI ran at 154 MHz only after a VBE boot.

* **Probe, PROVEN `rom0528 0x8508`:** I2C port 4 (`0x4c26`: gated on
  `PInS[0x88]` bit 2; SDA = MISC-IO id 1, SCL = id 2 through the nibble table
  `PInS[0x188]`/`[0x18c]` = pins 0/1; channel select id 5 driven low, else id 8
  driven high = pin 8 high here), slave `0xec` (7-bit `0x76`) -> runtime flag
  `[0x7fed] |= 0x40`, address `[0x7fec]`; second transmitter at `0xea` (`0x75`)
  on port 4, else `0xec`/`0xea` on port 5 (ids 3/4, `PInS[0x88]` bit 3) ->
  `[0x7fea]/[0x7feb]`; VID register `0x4a` -> `[0x7fe7]` (`0x8634`).  DVO1 pairs
  with `0x76`, DVO2 with `0x75` (same `[0x7fdd]` selection in `0x9d88` and
  `0x7428`).  MEASURED from the running BIOS shadow at `0xc7fe0`: `0x7fe7 = 0x95`,
  `0x7fec = 0xec`, `0x7fea/0x7feb = 04/ea`, `0x7fed = 0x41`; over I2C both
  chips answer VID `0x95`, DID `0x17` (CH7301C).
* **Per-mode programming, PROVEN `rom0528 0x7461` (from the hi-res set
  `0x68e4` -> `0x6a0d`, clock in MHz) and `mtx740 HSLCHREnable .text+0x932b0` /
  `HSLCHREnableDigitalMode +0x8e2f0` (clock `vidparm[0x24]` kHz):** reset
  `0x48 = 08, 18, 10` (`0x7577`, `HSLCHRReset +0x907a0`, which also writes
  `0x49 = 1e` and keeps `0x1e`'s high nibble); `0x1c = 04`, `0x1f = 80`,
  `0x49 = de`; for VID `0x85`/`0x95` (vendor also `0x80`) `0x23 =
  PInS[0x88] & 0x8000 ? 6 : 4`; VID `0x95`/`0x80` also `0x70 = a0`; then
  `0x31..0x37` from the band table; `0x48 = 18`; PLL lock check.

  | VID | band | `0x31..0x37` | ROM | mtx740 |
  |---|---|---|---|---|
  | `0x95`/`0x80` | < 65 MHz | `00 23 08 16 30 60 00` | `0x740c` | `ChrVerCDVIParam[0]` `.data 0x33c310` |
  | `0x95`/`0x80` | >= 65 MHz | `00 2d 06 26 70 a0 00` | `0x741a` | `ChrVerCDVIParam[1]` |
  | `0x85` | <= 32 / < 65 / >= 65 MHz | `..30 10..` / `..30 60..` / `2d 06 26 30 a0` | `0x73e2/0x73f0/0x73fe` | `ChrVerBDVIParam` `0x33c320` |
  | other | any | `00 2d 07 26 30 e0 00` | `0x73d4` | `ChrDVIParam` `0x33c335` |

* **PLL lock check, PROVEN `rom0528 0x773a`:** `0x23 |= 8`, `0x35 |= 0x40`; poll
  `0x4d` bit 2 until 3 consecutive set within 150 reads, then require 20 more in
  a row; on failure toggle `0x48` bit 3 (up to three restarts), then toggle
  `0x49` bit 6 once and try again.  The vendor `HSLCHRPllCheck` (`+0x93120`)
  is the same state machine but ORs `0x23` bit **2**
  (`HSLCHRPllCheck_AccessPllLockBit +0x92f1a`); the chip reads `0x23 = 0x0e`
  after the VBIOS ran, i.e. the VBIOS form.
* **The vendor HAL calls it from `HSLPAREnableDVI`** (`.text+0xb5fc5`) when
  `dev[0x3a4] & 0x3000000` (Chrontel found) - the branch section 6.8 missed.
* **MEASURED failing state** (text boot, our module before ae34aa5, 1920x1200
  154 MHz): `0x76` holds the < 65 MHz table, `0x4d = 00` - **the TMDS PLL is
  not locked**.  A text-mode POST tunes for 28 MHz, a `vga=0x31f` boot for
  162 MHz, and the MMIO dumps were identical because the state lives in the
  I2C device.
* **`0x1fde`, PROVEN `rom0528`:** set to 1 at `0x94ba` in the hi-res set
  (with GCTL06 = graphics, A0000/64K), ORed with granularity flags at `0x60ac`
  inside the VBE window function `0x5fc6` (its far pointer is stored into the
  ModeInfoBlock at `0x5c45`; the same routine writes the bank number to
  `0x19f0`), and cleared at `0x5330`/`0x8376` on the way back to VGA.  It is the
  VGA aperture window/bank control, not a display register; the native vendor
  path writes it only in `HSLPARSetVGAEnable` (`.text+0xd7656`).

Kernel: `parhelia_ch7301.c` (commit ae34aa5 on p0528-dvi0528) implements the
probe and the vendor enable sequence with this board's lock check, called at
the end of the original family's DVO mode set.
