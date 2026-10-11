Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# Cold bring-up of an un-POSTed Parhelia-512 / 8X (HSLPAR) — reverse engineered

Static reverse engineering only.  No hardware was touched while writing this
document; the hardware numbers quoted are from captures taken by the maintainer
and committed separately (§1).

**Scope.**  PCI `102b:0527` (Parhelia-512) and `102b:0528` (Parhelia-8X), i.e.
the `PARHELIA_MC_HSLPAR` / `PARHELIA_CLOCK_HSLPAR` /
`PARHELIA_RESET_HSLPAR` families.  `0x2537` (LX / P650) shares the whole path
with a two-bank instead of a four-bank memory controller; `0x2538` shares the
clocks and memory controller but takes the SUNEX reset; `0x2539` (P690) has its
own `HWS90*` memory controller and is *not* covered here — that one is already
decoded in [`parhelia-p690-engine-reset-re.md`](parhelia-p690-engine-reset-re.md)
§5.  Do not carry any register number between the two families: on the P690
`0x0f00` is a memory-controller **data** register, on the Parhelia it is the
**bank-select** register.

**Short answer.**  In vendor order:

1. enable PCI memory decode (BAR1 only is needed for every step below);
2. read `0x0e00`; if bit 30 is set, or the register is `0` or `0x40000000`, the
   chip has no clocks — issue the chip reset (`HSLPARHardwareReset`, §3);
3. video reset, put the engine and both display controllers in hold;
4. **system clock**: park `0x0e00`, then bring up the two frequency
   synthesisers that `PInS[0x84]` selects — this board's PInS carries no ready
   divider words, so each is *searched for* from its target frequency
   (250000 and 550000 kHz) over the two-stage PLL relation
   `Fout = 27000 kHz × A × B / (C × D)`,
   programmed with a bit-31 load strobe and verified against the synthesiser's
   own lock bits, then install `PInS[0x84]` into `0x0e00` (§4);
5. **memory controller**: per bank, 13 PInS dwords into `0x0f04..0x0f58`
   wrapped in a `0x0f5c`/`0x0f08` training sequence, four banks in the order
   1, 0, 2, 3, then `0x12b8`/`0x12b0` and the global phase-error reset (§5);
6. release the holds and run the ordinary engine init (§6).

Nothing in steps 2–5 touches BAR0.

Every claim is tagged **PROVEN** (read directly out of a vendor binary, the
board's own VBIOS, or a committed hardware capture, with the address) or
**INFERRED**.  The sequence is recovered **twice, independently** — from the
vendor Linux X driver and from the board's own VBIOS — and the two agree on
every register, every magic value and the register encoding of the
synthesisers, differing only in their delay lengths.

---

## 1. Evidence

| file | what it is |
|---|---|
| `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | vendor X driver v1.4.7, x86-64, **the only binary with symbols** (2418 dynamic symbols).  Source of every `HSL*`/`HW*` address below. |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARM.sys` | Windows miniport for `0x0527`/`0x0528`, the i386 sibling of the same code.  Listed because it is the correct third binary for *this* family — `MtxPser/mtxpserm.sys`, used by the earlier reset notes, is the P650/P690 one.  **Not disassembled for this pass**: the independent confirmation here comes from the board's own ROM instead, which is a stronger check because it is a different implementation rather than the same code recompiled. |
| `captures/parhelia-0528-cold/parhelia-0528-vbios.rom` | 43008 bytes, the ROM of **this** board (`102b:0528` rev 06), read from the ROM BAR. |
| `captures/parhelia-0528-cold/parhelia-0528-regs-unposted.txt` | the whole 8 KiB BAR1 register aperture as read on the un-POSTed board, `OFFSET = VALUE`, 2048 dwords, 331 nonzero. |
| `vbios-matrox-parhelia` | an older 0528 ROM already in the tree.  Different file (different md5) but a **byte-identical PInS block** (the 512 bytes at ROM `0x7800` have md5 `03a0bdfafb9f21cd5fd1263f08d590e8` in both), so the tables below are not board-specific guesswork.  The 303 differing bytes are all in the VGA/CRTC tables at `0x365..0xa80`, at `0x56e4..0x5709`, at `0x7a11`, at `0x7fdd..0x7ff7` and in the ROM checksum byte. |

**Address convention.**  Three kinds of hex number appear, and they do not
overlap in practice: **MMIO registers** are BAR1-relative and always introduced
as registers or as the target of a `wr32`/`rd8`/`ReadReg`/`WriteReg`
(`0x0000..0x1fff`); **ROM addresses** lie in `0x80..0xa7ff` and are always
inside a ROM listing or prefixed "ROM"; **`mtx_drv.so` addresses** are
`0x36000` and above.

### 1.0 Where PInS is, and how much of it the vendor keeps

`HSLBIOSReadPins` `0xecee0` reads a dword at ROM `0x7ffc` and uses its low word
as the PInS offset (`0xed08c`), checks the `0x412e` signature (`0xed0e0`), takes
the structure length from the **word** at `+0x02` when the version is above
`0x5ff` (`0xed0d5`) and from the single **byte** at `+0x02` otherwise
(`0xed359`), checksums `length` bytes (`0xed3e6`–`0xed407`), and copies exactly
`length` bytes into `dev+0x3b78`.  `HWPARGetPINS` `0x4abd0` tail-calls
`MTXGetPinsPointer` `0x8aca0`, which is `lea 0x3b78(%rdi),%rax; ret` — so every
`PInS[x]` in this document is a read of that 512-byte copy.  **PROVEN.**

For this board: PInS at ROM **`0x7800`**, signature `0x412e`, length
**`0x200`**, version **`0x060a`**, checksum byte `+0x1ff = 0x9a` (the 512 bytes
sum to zero).  Board strings `+0x0c = "BE23797"`, `+0x1c = "117"`,
`+0x178 = "PH-P256"` — Parhelia, 256-bit bus, matching the four-bank memory
controller of §5.  **PROVEN** (extracted from the capture).

Reproduce the disassembly with:

```sh
D=/home/ai/src/parhelia/xf86-video-parhelia
L=$D/matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so
nm -D --defined-only $L | sort > /tmp/mtxdrv.syms
objdump -d --no-show-raw-insn -C $L   > /tmp/mtxdrv.dis
objdump -d --no-show-raw-insn $D/xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARM.sys > /tmp/parm.dis

# the board's own ROM, 16-bit real mode
R=$D/captures/parhelia-0528-cold/parhelia-0528-vbios.rom
ndisasm -b16 -o0x80   -e0x80   $R | head -60     # the init chain
ndisasm -b16 -o0x80fe -e0x80fe $R | head -40     # the chip reset
ndisasm -b16 -o0x6d8c -e0x6d8c $R | head -60     # the clock/PLL init
ndisasm -b16 -o0x7018 -e0x7018 $R | head -180    # SetPixelClock: the divider search
ndisasm -b16 -o0x72f4 -e0x72f4 $R | head -60     # the lock poll and the register encoder
ndisasm -b16 -o0x837f -e0x837f $R | head -40     # the memory controller
ndisasm -b16 -o0x83dd -e0x83dd $R | head -80     # ... per bank
```

The functions that matter, all `T` in `mtx_drv.so`:

| symbol | address | role |
|---|---|---|
| `HSLPARInitializeHardware` | `0xbf780` | the whole cold path, in order |
| `HSLPARHardwareReset` | `0xc5580` | the `0x0527/0x0528/0x2537` chip reset |
| `HSLPARSaveCriticalData` | `0xba960` | snapshots `0x0e20`, `0x0e2c` |
| `HSLPARRestoreCriticalData` | `0xbd4d0` | re-strobes them |
| `HWPARInitializeSystemClock` | `0xe5480` | the clocks |
| `IsScUseFS` | `0xe5390` | which synthesiser a domain uses |
| `HWPARInitialiseFSController` | `0xe4fc0` | divider search when PInS has no divider word |
| `HWPARInitialiseFSLimit` | `0xe4b20` | PInS → search limits |
| `HWPARWriteFSParameters` | `0xe4580` | synthesiser register encoding |
| `HSLPARGetFSFreq` | `0xbf280` | read a synthesiser back |
| `HSLPARGetSystemCLock` | `0xbf370` | decode `0x0e00` into three domain frequencies |
| `HSLPARGetPInSSystemCLock` | `0xc0090` | the same decode from `PInS[0x84]` — the more complete of the two |
| `HSLBIOSReadPins` | `0xecee0` | locate, validate and copy PInS |
| `pllGetRegOffset` | `0xeb1f0` | FS index → MMIO offset |
| `HWPARInitializeMemoryController` | `0xe4110` | the memory controller |
| `HWPARGetMCRegisters` | `0xe3b30` | PInS → 13 values |
| `HWPARSetMCRegisters` | `0xe3dc0` | the per-bank programming sequence |
| `SelectMemoryController` | `0xe3af0` | bank select through `0x0f00` |
| `WriteMemoryControllerDword` | `0xe3bf0` | banked write |
| `ReadMemoryControllerDword` | `0xe3c80` | banked read |
| `WaitForMemoryIdle` | `0xe3d20` | `0x0f08` bit 31 poll |
| `HWResetMemoryPhaseError` | `0xe3ff0` | `0x0f08` bit 2 / `0x0f24` bit 16 |
| `HSLPARMemoryControllerCheck` | `0xc3790` | read-only per-bank health test |
| `HWDelay` / `ClientWait` | `0x4abc0` / `0x4a9a0` | the delay primitive |

### 1.0b The ROM does the same job, and is the better template

A second, independent implementation of the whole cold path is in the board's
own VBIOS, and it is **simpler and more complete than the driver's**.  The ROM
init entry `0x80` checks PCI `0x0528102b`, latches the bus/devfn, and then:

| ROM | step |
|---|---|
| `0x00b4` → `0x80fe` | chip soft reset (§3.2) |
| `0x00b7` → `0x816d` | clock block and the four programmable PLLs (§4) |
| `0x00ba` → `0x837f` | **the memory controller, four banks** (§5.6) |
| `0x00bd`…`0x00cf` | DDC/EDID probe and monitor flags |
| `0x00d8` → `0x8aaa` → `0x6e29` | the two legacy VGA reference clocks `0x0e20`/`0x0e2c` (§4.6) |
| `0x00e8` → `0x8308` | `0x0000`/`0x0004`/`0x0008`/`0x000c`, font copy |
| `0x00eb` → `0x8ac7` | "VRAM size" — a hardcoded `mov ax,0x100; ret` (§5.5) |

**PROVEN.**  Two things make this the better source.  First, the ROM reaches
MMIO through the PCI-config window (cfg `0x44` ← offset, cfg `0x48+(off&3)` ↔
data; helpers at `0x6c9b` wr8, `0x6cc1` wr16, `0x6ce7` wr32, `0x6d0d` rd8,
`0x6d31` rd16, `0x6d55` rd32 — all verified unchanged in this image), so every
register access is a `mov si, imm16` and the whole sequence is statically
recoverable.  Second, **the ROM reads its tables out of PInS**: the addresses it
hardcodes are `0x7828/0x783c/0x7850/0x7864` (PLL divider words),
`0x7014 = {0x2c, 0x40, 0x54, 0x68}` (PLL limit blocks),
`0x78a4/0x78d8/0x790c/0x7940` (memory-controller records), `0x7884`
(the `0x0e00` value), `0x799c` (the `0x1c50` value), `0x7874`/`0x787c`
(the two legacy clock targets) — and PInS is at ROM `0x7800`, so these are
exactly `PInS + 0x28/0x3c/0x50/0x64`, `PInS + 0x2c/0x40/0x54/0x68`,
`PInS + 0xa4/0xd8/0x10c/0x140`, `PInS + 0x84`, `PInS + 0x19c`,
`PInS + 0x74/0x7c`.  **The ROM and the driver read the same PInS fields**, which
independently confirms every offset in §4 and §5 without relying on the
disassembly of either alone.  **PROVEN.**

The ROM's delay primitive `0x5005` is a real timer — it programs PIT channel 2
(`0x43` ← `0xb0`), loads `DX` as the tick count, gates it through port `0x61`
and polls OUT2 with a 5,000,000-iteration escape.  At 1193182 Hz one tick is
0.8381 µs, so `DX = 0xf0` is **201 µs**, `0x2ee0` is **10.06 ms** and `0x5dc0`
is **20.11 ms**.  **PROVEN.**

### 1.1 The driver's delay primitive — a correction

`HWDelay(n)` is a one-instruction tail call to `ClientWait(n)` (`0x4abc0`), and
`ClientWait(n)` (`0x4a9a0`) is **`3 * n` dummy dword reads of MMIO `0x08f4`** —
no timer, no sleep.  Every "delay" in this document is therefore a count of
register reads, which at PCI 66 MHz works out at roughly `n` microseconds.
**PROVEN.**  This replaces the INFERRED "the wait unit is microseconds" note in
`parhelia-p690-engine-reset-re.md` §3.3: the unit is not microseconds by
definition, it is microseconds by accident of bus speed.  A kernel
reimplementation should use `udelay(n)`/`usleep_range()` with the same `n`,
which is equal or conservative on any bus faster than 33 MHz, and must keep a
read of `0x08f4` (or an equivalent posted-write flush) where the vendor's loop
provided one.

---

## 2. Telling a POSTed chip from an un-POSTed one, read-only

This is the urgent item.  Every test below is a plain `readl()` of the BAR1
register aperture; none of them touches BAR0, and none of them writes anything.
Reading the register aperture was demonstrated safe on the un-POSTed board
while a single dword written to BAR0 resets the machine.

The measured values are quoted from
`captures/parhelia-0528-cold/parhelia-0528-regs-unposted.txt`.

### 2.1 The one to use: `0x0e00` (clock-domain source select)

```
un-POSTed (measured):  0x0e00 = 0x40000000
POSTed  (this board):  0x0e00 = PInS[0x84] = 0x00034114
```

This is **the vendor's own liveness test**, not an invention of ours.
`HSLPARInitializeHardware` `0xbf7e1`–`0xbf808`:

```
v = ReadReg(0x0e00);
if (v == 0 || v == 0x40000000)  ->  0xc003f: call dev[0x4f20]()   /* chip reset */
```

and the P690 sibling `HSLS90InitializeHardware` `0xdd81d` does the same on
`0x0e24` against `{0, 0x80000000}`.  **PROVEN** (the code, two binaries — see
`parhelia-p690-engine-reset-re.md` §3.2 for the miniport cross-check; the
measured `0x40000000` is from the capture).

**And the ROM explains why `0x40000000` specifically.**  The very first MMIO
operation of the whole VBIOS init — before any PLL, before any memory work —
is `0x8174`–`0x817d`:

```
rd8  0x0e03           /* byte 3 of 0x0e00, i.e. bits 31:24 */
and  cl, 0xbf         /* clear bit 6 of that byte = bit 30 of 0x0e00 */
wr8  0x0e03
```

So **`0x0e00` bit 30 is set at power-on and the ROM's job is to clear it.**  The
measured un-POSTed value `0x40000000` is exactly that bit and nothing else.
`0x0e00 & 0x40000000` is therefore a one-bit, side-effect-free, positively
identified "the VBIOS init has not run on this chip" flag.  **PROVEN** (the ROM
code at `0x8174`, the driver's constant at `0xbf7fc`, the measured value).

The last write `HWPARInitializeSystemClock` makes to `0x0e00` is
`PInS[0x84]` (`0xe5708`–`0xe5716`), so on a chip whose clocks were brought up
from *this* board's VBIOS the register reads exactly `0x00034114`.  The safest
gate is therefore two-sided:

* refuse if `0x0e00 == 0 || 0x0e00 == 0x40000000`, and in particular if
  `0x0e00 & 0x40000000` — **un-POSTed, do not touch VRAM**;
* accept if `0x0e00 == PInS[0x84]` (or, without a PInS copy, if the field
  `(0x0e00 >> 1) & 7` is nonzero, which means some domain is sourced from a
  synthesiser rather than from the bare reference clock);
* anything else is a third state — treat it as un-POSTed and refuse.

**Confidence: high.**  It is the manufacturer's own criterion, the measured
un-POSTed value is one of the two constants it names, and the POSTed value is
derivable from the board's own ROM.

### 2.2 The independent confirmation: the six synthesiser registers

```
un-POSTed (measured):  0x0e10 = 0x0e14 = 0x0e18 = 0x0e1c = 0x0e20 = 0x0e2c = 0x80000000
```

`pllGetRegOffset` `0xeb1f0` maps synthesiser index 0..5 to
`0x0e10, 0x0e14, 0x0e18, 0x0e1c, 0x0e20, 0x0e2c`.  Bit 31 of such a register is
the load strobe (§4.3), so `0x80000000` is "strobe asserted, every divider field
zero" — the power-on state, a synthesiser held in reset.

`HSLPARRestoreCriticalData` `0xbd501`–`0xbd537` treats a saved value of **`0`
or `0x80000000`** as invalid and refuses to restore it.  So the vendor itself
uses exactly this pair as "this PLL was never programmed".  **PROVEN** (the
code) + measured.

Test: *every* one of the six reads `0` or `0x80000000` → un-POSTed.
**Confidence: high.**  A POSTed chip must have at least the two synthesisers
selected by `PInS[0x84]` programmed to something else; for this board that is
FS0 (`0x0e10`) and FS1 (`0x0e14`) (§4.2).

### 2.3 A third, cheap signature: the `0x1200`/`0x1300` block reads a constant

```
un-POSTed (measured):  every dword in 0x1200..0x13fc reads 0xfdfe853a
```

All 128 dwords of the two blocks return the same nonzero constant.  That is a
bus-idle/undriven read pattern, not register content: the 3-D state block is
not clocked.  On a working chip these are distinct per-register values (e.g.
`0x12b8` is the watermark word the driver sets to `0x00070601`).

Test: `readl(0x1200) == readl(0x12fc) && readl(0x1200) == readl(0x13fc) &&
readl(0x1200) != 0` → un-POSTed.  **INFERRED** (the interpretation);
**PROVEN** as an observation of this board.  **Confidence: medium** — useful as
a corroborating check, but the exact constant is likely to differ per host
chipset, so test for *equality across the block*, never for `0xfdfe853a`.

Two registers inside that block have an **exactly known POSTed value**, because
the ROM writes them with literal constants at `0x82e9` and `0x82f5` (the
`0x12xx` block at ROM `0x829e`, reached from `0x8227`, i.e. *before* the memory
controller):

```
0x12c0 = 0xffffffff      0x12c4 = 0x7fffffff
```

Un-POSTed both read `0xfdfe853a`.  So `readl(0x12c0) != 0xffffffff ||
readl(0x12c4) != 0x7fffffff` → un-POSTed.  **PROVEN** (the ROM writes and the
measurement); **Confidence: high**, and unlike §2.3 it is an exact value rather
than a pattern.  The same ROM block also sets `0x12f4 = 0x41f80000`,
`0x12c8 = 0`, `0x12cc = 0x0000ffff`, `0x12d0 = 0x000001ff`, `0x12d4 = 0`,
`0x12fc = 0`, `0x12e4 = 0`, `0x12e8 = 0`, `0x1314 = 0`, `0x1318 = 0` — which
corroborates the `0x12c0`/`0x12c4` values recorded independently in
[`parhelia-vbios.md`](parhelia-vbios.md) from the older image.

### 2.4 The vendor's own memory-controller health test — needs one write

`HSLPARMemoryControllerCheck` `0xc3790` is a read-only-in-spirit probe that the
vendor runs at runtime to decide whether to re-initialise the memory
controller:

```
if ((dev->status & 0xc0000000) != 0 && != 0x40000000) return;      /* 0xc37b5 */
if (PInS.version > 0x6ff) return;                                   /* 0xc37d1 */
half = PInS[0x8c] & 8;                                              /* 0xc37f5 */
acc  = (ReadMC(0x0f24, bank 0) & 3) << 0;                           /* 0xc3805 */
acc |= (ReadMC(0x0f24, bank 1) & 3) << 4;                           /* 0xc3824 */
if (!half) {
    acc |= (ReadMC(0x0f24, bank 2) & 3) << 8;                       /* 0xc3878 */
    acc |= (ReadMC(0x0f24, bank 3) & 3) << 12;                      /* 0xc389f */
}
if (acc != 0) HWPARInitializeMemoryController(dev);                 /* 0xc385f */
```

**PROVEN.**  `0x0f24` bits [1:0] are therefore a per-bank memory phase/lock
error status, and a nonzero value in any bank means the controller must be
re-initialised.  This board's PInS version is `0x060a`, so the `<= 0x6ff` gate
passes and the test applies.

The catch: a banked *read* still writes the bank-select register `0x0f00`
(`ReadMemoryControllerDword` `0xe3c80` saves `0x0f00`, writes the bank, reads
the register, restores `0x0f00`).  That write goes to the register aperture,
not to VRAM, so it should be as safe as the reads that were already
demonstrated — but it has not been demonstrated, so **do not put it in the
probe gate**.  Keep it for after the controller is up, as the "did it work"
check.  **Confidence in the decode: high.  Confidence that issuing it on an
un-POSTed chip is safe: unproven — treat as a write.**

### 2.5 What does *not* work as a POST test

* `0x0f00..0x0f5c` all read zero on the un-POSTed board, and the whole block is
  zero (0 of 64 dwords nonzero) — but `0x0f00` is the bank-select register and
  `WriteMemoryControllerDword` always restores its previous value, so a POSTed
  chip may legitimately read `0x0f00 == 0` too, and with no bank selected
  `0x0f04..` read zero regardless.  Weak; do not rely on it.
* `0x1c00 == 0`, `0x1c50 == 0`, `0x10b0 == 0`, `0x0920 == 0x80000000`,
  `0x0308 == 0x001f1f00`, `0x1c24 == 0x00000003` (all measured) distinguish
  "the vendor driver has not run" rather than "the chip was not POSTed", because
  those registers are written by `HSLPARInitializeHardware`, not by the ROM.
  Useful in a diagnostic dump, not as a gate.
* `0x08f0 == 0x01020000` (measured): bit 24 of the WARP-busy pair is set at
  power-on, so `warp_gate_ready()` would also refuse — but only *after* the
  memory controller is up, which is far too late.
* PCI `COMMAND == 0x0000` (measured) is the cheapest possible signal that
  firmware never POSTed the card, and is what kernel `9229adc` already uses.
  It is a statement about firmware, not about the chip: a driver that itself
  enables decode must re-test with §2.1 afterwards.

---

## 3. Does the chip need a reset first?  Yes, and it comes first

`HSLPARInitializeHardware` `0xbf7e1` issues the reset **before** anything else,
gated exactly on the §2.1 condition, and then falls through
(`0xc0042: call *0x4f20(%r12)`, `0xc0050: jmp 0xbf80e`) into the full init.  So
the order is reset → clocks → memory, never the other way round.
**PROVEN.**

### 3.1 `HSLPARHardwareReset` @`0xc5580`, decoded

For `0x0527`/`0x0528`/`0x2537` the reset is `HSLPARHardwareReset` `0xc5580`
(the dispatch table is decoded in `parhelia-p690-engine-reset-re.md` §3.1).
Decoded in full — this corrects the abbreviated version in that document's
§3.4:

```
 1. if ((dev->status & 0xc0000000) != 0 && != 0x40000000) return;   /* 0xc5589 */
 2. HSLPARSaveCriticalData(dev);                                     /* 0xc55b0 */
 3. pcix = ((dev->pcix_state & 0xe0) != 0x80);                       /* 0xc55b5 */
 4. save cfg 0x04, 0x0c, 0x10, 0x14, 0x30, 0x3c, 0x40;               /* 0xc55c2..0xc5645 */
    cF4 = cF8 = 0;
    if (pcix) { cF4 = cfg 0xF4; cF8 = cfg 0xF8; }                    /* 0xc57b7 */
 5. if (devid == 0x0528 || (devid - 0x2537) <= 2)
        mmio_write(0x0308, 3);                                       /* 0xc57f3 */
 6. old300 = mmio_read(0x0300);                                      /* 0xc5691 */
    new300 =  (old300 & 0x0000ffff)
            | ((cF4        &       0x7) << 16)   /* bits 18:16 */
            | ((cfg04 >> 2) &   0x80000)         /* bit  19    */
            | ((cF4  << 16) &  0x100000)         /* bit  20    */
            | ((cF4  << 11) &  0xe00000)         /* bits 23:21 */
            | ( cF4        & 0xff000000);        /* bits 31:24 */
 7. mmio_write(0x0300, new300 & ~0x100);         /* bit 8 low across the strobe */
 8. pci_write_config_dword(0xE0, 3);             /* D3hot      0xc56ea */
 9. pci_write_config_dword(0xE0, 0);             /* D0         0xc56fc */
10. HWDelay(10000);                              /* ~10 ms     0xc570b */
11. restore cfg 0x04, 0x0c, 0x10, 0x14, 0x30, 0x3c, 0x40;            /* 0xc5718..0xc578a */
    if (pcix) restore cfg 0xF8;                  /* 0xF4 is NOT restored  0xc57e0 */
12. mmio_write(0x0300, new300);                                      /* 0xc5793 */
13. HSLPARRestoreCriticalData(dev);                                  /* 0xc57a3 */
```

**PROVEN** for every step and address.  Notes that matter for us:

* There is **no delay between the `3` and the `0`** write to cfg `0xE0` on this
  variant (the SUNEX variant used by the P690 has `10 ms` on both sides — see
  `parhelia-p690-engine-reset-re.md` §3.3).  `0xE0` is the PMCSR on the
  measured `0x0528` (already recorded in `parhelia_drv.c`), so this is a
  D3hot→D0 PM reset, the same electrical operation Linux's `pci_pm_reset()`
  performs.
* Step 11 restores the **saved** cfg `0x04`.  On our board that value is
  `0x0000` — firmware never enabled decode — so a literal transcription would
  switch memory decode back off mid-bring-up.  A kernel implementation must
  restore its *own* COMMAND value (decode + bus master as the driver wants it),
  not the pre-reset one, and must re-enable decode after the reset before any
  further MMIO.
* `HSLPARSaveCriticalData`/`RestoreCriticalData` snapshot MMIO `0x0e20` and
  `0x0e2c` and restore each as `v | 0x80000000`, `HWDelay(100)`, `v` — the
  standard synthesiser load strobe (§4.3) — and skip a saved value of `0` or
  `0x80000000`.  On an un-POSTed board both read `0x80000000`, so nothing is
  restored and the two registers stay unprogrammed.  **Nothing in the HSLPAR
  path ever programmes `0x0e20`/`0x0e2c` from PInS** (a scan of every
  `mov $0xe20,%esi` / `mov $0xe2c,%esi` in the binary finds only
  `HSLPARSaveCriticalData`, `HSLPARRestoreCriticalData` and `pllGetRegOffset`).
  See §7 open items.
* `0x0300` bits 31:16 are rebuilt from PCI-X status dwords.  On a plain PCI
  board `pcix == 0` and `cF4 == 0`, so `new300 = (old300 & 0xffff) |
  ((cfg04 >> 2) & 0x80000)`.  Measured `0x0300 = 0x00000000` and
  `cfg 0x04 = 0x0000` on the cold board, so `new300 = 0` — i.e. on this board
  step 6/7/12 reduce to writing zero.  **INFERRED** that this is harmless; it
  is what the vendor computes from the same inputs.

### 3.2 The ROM's own reset, as a cross-check

`0x80fe`, the first thing the VBIOS does to the chip:

```
0x8103  save PCI cfg 0x04, 0x08, ... 0x50 as dwords (13 of them), pushed;
        the dword at cfg 0x2c is also kept in EDX
0x811d  save PCI cfg 0xf8
0x8122  wr32 MMIO 0x0308 <- 0x00000003
0x8135  PCI cfg 0xe0 (byte) <- 0x03
0x813d  PCI cfg 0xe0 (byte) <- 0x00          /* no delay between the two */
0x8144  delay 10.06 ms
0x814f  PCI cfg 0xf8 <- saved
0x8155  restore PCI cfg 0x50 down to 0x04, step -4, LIFO;
        cfg 0x48 is SKIPPED (0x815a)         /* the indirect data window */
        cfg 0x4c is written with the saved cfg 0x2c (0x8161)
```

**PROVEN.**  This agrees with `HSLPARHardwareReset` on every substantive point:
`0x0308 = 3` first, a `3 → 0` strobe on cfg `0xE0` with no delay in between,
about 10 ms afterwards, a restore that deliberately skips the indirect MMIO
window, and no PCIe FLR anywhere.  Two differences worth recording: the ROM
writes cfg `0xE0` as a **byte**, not a dword, and it restores a wider range
(`0x04..0x50` plus `0xf8`) than the driver's seven-dword set, while the driver
restores COMMAND last and the ROM restores it last only because its loop counts
down.  **INFERRED**: `cfg 0x4c` being written from the saved `cfg 0x2c` is a
subsystem-ID shadow the ROM re-establishes; the driver does not do this, so it
is probably only needed because a PM reset reloads `0x2c` from straps.

If the driver prefers Linux's own reset plumbing, the equivalent is
`pci_save_state()` + `pci_reset_function_locked()` + `pci_restore_state()`,
**plus** an explicit save/restore of the Matrox vendor dwords `0x40..0xFC`
(skipping `0x44`/`0x48`, the MGAINDEX/MGADATA window — writing them performs
MMIO accesses mid-restore), because `pci_save_state()` does not cover
vendor-specific config space.  That gap is the accepted explanation for the
`reset_on_probe=1` failure recorded in `HANDOVER.md`.

---

## 4. The clocks — `HWPARInitializeSystemClock` @`0xe5480`

### 4.1 The register block

| MMIO | role | source | provenance |
|---|---|---|---|
| `0x0e00` | clock-domain source select + ratio fields | `PInS[0x84]` | `0xe549d`, `0xe5708` |
| `0x0e10` | frequency synthesiser 0 | PInS PLL record 0 | `pllGetRegOffset` `0xeb243` |
| `0x0e14` | frequency synthesiser 1 | PInS PLL record 1 | `0xeb23c` |
| `0x0e18` | frequency synthesiser 2 | PInS PLL record 2 | `0xeb235` |
| `0x0e1c` | frequency synthesiser 3 | PInS PLL record 3 | `0xeb22e` |
| `0x0e20` | synthesiser 4 — the **first legacy VGA reference clock**, 25.175 MHz | `PInS+0x74` target, `PInS+0x78` divider | `0xeb227`; ROM `0x6e29` |
| `0x0e2c` | synthesiser 5 — the **second legacy VGA reference clock**, 28.311 MHz | `PInS+0x7c` target, `PInS+0x80` divider | `0xeb218`; ROM `0x6ea1` |

`HWPARInitialiseFSLimit` `0xe4b34` computes the register offset for index
0..3 as `0x0e10 + 4*i`, which agrees for those four; the jump table above is
authoritative for 4 and 5.  `0x0e24` and `0x0e28` are **never touched by either
the ROM or the HSLPAR driver path** — `0x0e24` is the P690's liveness register
and nothing else.  **PROVEN.**

The identity of `0x0e20`/`0x0e2c` is a correction to the naming in
`parhelia-p690-engine-reset-re.md` §3.4, which calls them "the system and
memory PLLs".  They are not: four independent facts from the ROM say they are
the two legacy VGA reference clocks.  (a) Their fallback targets are
`PInS+0x74 = 25175` kHz and `PInS+0x7c = 28311` kHz — the classic VGA clock 0
and clock 1.  (b) Each is programmed immediately after
`vga_misc_clocksel(0)` and `vga_misc_clocksel(1)` respectively (ROM `0x94c4`
writes MISC bits 3:2), while the *programmable* PLLs use select code 2
(`0x6d92`).  (c) The ROM records the resulting frequency in
`[cs:0x7fe6]` as 28 or 29, i.e. MHz — not hundreds of MHz.  (d) They are
programmed at init step `0x00d8`, **after** the memory controller is already
running at `0x00ba`; a memory clock cannot be programmed after the memory
controller.  **PROVEN** (all four observations).  So the driver never programmes
them because they are VGA legacy clocks it does not need; preserving them
across a reset is all it has to do, and a cold bring-up that does not want
legacy VGA text mode can leave them at `0x80000000`.

It is also a correction to [`parhelia-vbios.md`](parhelia-vbios.md), which says
"the four PLLs hang off the VGA MISC clock-select field … called with sel = 1
and 2".  The mapping is MISC select **0 → `0x0e20`**, **1 → `0x0e2c`**,
**2 → the programmable PLL** indexed by the ROM scratch byte `[cs:0x7ff0]`,
which `0x6f5f` derives from `0x0e00` by picking the first synthesiser no clock
domain has claimed.  On this board that is **FS2 (`0x0e18`)**, and the older,
post-POST ROM image has `0x02` in that scratch byte — a direct confirmation.
**PROVEN.**

### 4.2 `0x0e00` decoded

`IsScUseFS` `0xe5390` (which synthesiser a domain uses) and
`HSLPARGetSystemCLock` `0xbf370` (which frequency a domain ends up at) together
pin the layout:

| bits | field | encoding |
|---|---|---|
| 0 | (set in the `0x8081` scratch value; undecoded) | |
| 3:1 | domain **0** source | `0` none, `1..4` → FS0..FS3 (`0xbf3cc`, `0xbf554`/`0xbf6db`/`0xbf6f0`/`0xbf75d`; `0xc0193` in the PInS variant) |
| 4 | domain **0** ratio | with bit 28: `sel & 0x10000010 == 0x10` → ÷2, code `0x12` (`HSLPARGetPInSSystemCLock` `0xc0130`) |
| 6:5 | domain **1** ratio | `0` ×1 (code `0x11`), `1` ÷3 (`0x13`), `2` ×2/5 (`0x25`), `3` ÷2 (`0x12`) (`0xbf5e4`, `0xbf663`, `0xbf631`, `0xbf688`) |
| 7 | (set in the `0x8081` scratch value; undecoded) | |
| 11:8 | domain **1** source | `1`→FS0, `3`→FS1, `5`→FS2, `7`→FS3 (`0xbf430`, `0xbf747`/`0xbf6c5`/`0xbf71b`/`0xbf5c6`); even codes select the same FS in `IsScUseFS` but are not decoded by `HSLPARGetSystemCLock` (**INFERRED**: a second, divided tap) |
| 14:13 | domain **2** ratio | same table as bits 6:5 (`0xbf4fa`) |
| 15 | (set in the `0x8081` scratch value; undecoded) | |
| 19:16 | domain **2** source | same table as bits 11:8 (`0xbf4c2`) |

The ratio codes are numerator/denominator nibbles: `0x11` = 1/1, `0x12` = 1/2,
`0x13` = 1/3, `0x14` = 1/4, `0x25` = 2/5, implemented as `shr 1`, `shr 2` and
the reciprocal multiplies `0xaaaaaaab` (÷3) and `0xcccccccd` after doubling
(×2/5).  `IsScUseFS(sel, i)` returns true when *any* of the three source fields
names synthesiser `i`; for bits 11:8 and 19:16 it accepts the pair
`{2i+1, 2i+2}` (`0xe53b0`, `0xe53e8`, `0xe5444`, `0xe5416`).  **PROVEN.**

Note that the two decoders disagree in one place: `HSLPARGetSystemCLock`
`0xbf412` stores a ratio code of `0` for domain 0 and never applies bit 4, while
`HSLPARGetPInSSystemCLock` `0xc0130` does.  The PInS variant is the complete
one; treat the register variant as a reporting shortcut.  **PROVEN** (both
code paths); **INFERRED** (that the PInS variant is authoritative).

For this board, `PInS[0x84] = 0x00034114`, with the frequencies of §4.4:

| domain | source field | source | ratio field | ratio | result |
|---|---|---|---|---|---|
| 0 | bits 3:1 = `2` | **FS1** = 550000 kHz | bit 4 set | ÷2 | **275 MHz** |
| 1 | bits 11:8 = `1` | **FS0** = 250000 kHz | bits 6:5 = `0` | ×1 | **250 MHz** |
| 2 | bits 19:16 = `3` | **FS1** = 550000 kHz | bits 14:13 = `2` | ×2/5 | **220 MHz** |

`IsScUseFS` is therefore true for FS0 and FS1 only, and FS2/FS3 are left alone —
consistent with their PInS records being all zero (§4.4).  **275 / 250 /
220 MHz is exactly the retail Parhelia-8X profile** (275 MHz DDR memory,
220 MHz core), which is a strong independent confirmation that the whole decode
is right and that nothing has to be hardcoded: the board's own ROM carries its
clocks.  **PROVEN** (the arithmetic and the ROM content); **INFERRED** (that
domain 0 is the memory clock and domain 2 the core clock).

`HSLPARGetPInSSystemCLock` also fixes the unit question: the conversion applied
to every PInS frequency is `imul $0x10624dd3; shr $0x20; shr $0x6`, i.e. an
exact divide by 1000 (`0xc0120`, `0xc018d`, `0xc01f5`), so **PInS stores kHz**
and the reported value is MHz.  The output is three `{u16 MHz, u8 ratio_code,
u8 source_code}` records.  **PROVEN.**

For contrast, the P690 equivalent `HSLS90GetPInSSystemCLock` `0xdc370` uses the
same four frequency records but **three separate selector dwords at
`PInS+0x200`, `+0x204`, `+0x208`** (`0xdc396`, `0xdc39d`, `0xdc3a4`) instead of
the single packed `PInS[0x84]`, and its ratio set extends to `0x15` ÷5, `0x18`
÷8 and `0x16` ÷16.  On a 0528 those selectors are past the end of PInS, and
`PInS[0x84]` on `tools/p690.rom` is `0xffffffff` — the two families genuinely
use disjoint fields.  **PROVEN.**

### 4.3 The sequence

```
pins = HWPARGetPINS();
sel  = pins[0x84];                                       /* 0xe549d */
save 0x1c00, 0x1c04, 0x1800, 0x1a00;                     /* 0xe5495..0xe54dd */
old_e00 = read 0x0e00;                                   /* 0xe54ef */
write 0x1c00 = old_1c00 | 0x20;                          /* engine hold      0xe54f4 */
write 0x1c04 = old_1c04 | 0x20;                          /*                  0xe5508 */
write 0x1800 = old_1800 & ~1;                            /* head A off       0xe551c */
write 0x1a00 = old_1a00 & ~1;                            /* head B off       0xe5530 */
HWDelay(1000);                                           /*                  0xe5544 */

write 0x0e00 = old_e00 | 0x8081;                         /* 0xe5551 */
write 0x0e00 = 0x8081;                                   /* 0xe5568 */
write 0x0e00 = 0;                                        /* 0xe557a */
HWDelay(1000);                                           /* 0xe5589 */

for (i = 0; i < 4; i++) {                                /* 0xe5596, 0xe55e0, 0xe5630, 0xe5680 */
    if (!IsScUseFS(sel, i)) continue;
    div = pins[0x28 + 0x14*i];
    if (div) {
        write FSREG[i] = div | 0x80000000;               /* load strobe  0xe55b5 */
        HWDelay(100);                                    /*              0xe55c2 */
        write FSREG[i] = div;                            /*              0xe55d2 */
    } else {
        HWPARInitialiseFSController(dev, i, pins[0x24 + 0x14*i]);  /* 0xe57b5 */
    }
}
HWDelay(10000);                                          /* 0xe56d0 */

write 0x0e00 = 0x8081;                                   /* 0xe56dd */
write 0x0e00 = sel | 0x8081;                             /* 0xe56ef */
write 0x0e00 = sel;                                      /* 0xe5708 */

restore 0x1c00, 0x1c04, 0x1800, 0x1a00;                  /* 0xe571b..0xe575a */
```

**PROVEN**, every line and address.  Points to carry into the driver:

* `0x8081` (bits 0, 7, 15) is a *scratch* source-select value used while the
  synthesisers are being loaded — every domain is parked before and after the
  load, and the real value is installed with a `| 0x8081` step in between.  The
  bit meanings are not decoded; write the constant as the vendor does.
  **INFERRED** (that it means "park on the reference clock").
* There is **no lock-bit poll on the PInS-divider-word path.**  The only waits
  are the fixed `HWDelay(100)` between the two halves of the load strobe and
  the `HWDelay(10000)` (≈10 ms) after all synthesisers have been loaded.
  **PROVEN (negative result).**  The driver does poll the lock — but only on
  the *search* path, inside `HWPARWriteFSParameters` (§4.5), as does the ROM on
  both paths.  Since this board has no divider words, the path we implement is
  the one with the poll; a driver that ever does use a PInS divider word should
  verify it the way the ROM does at `0x6ddd`.
* The engine and both display controllers must be in hold across the clock
  change: `0x1c00 |= 0x20`, `0x1c04 |= 0x20`, `0x1800 &= ~1`, `0x1a00 &= ~1`.
  The memory-controller init (§5) puts them in the same hold, and the caller
  runs the two back to back, so a driver may hold once around both.
* The chip does **not** need `0x0e20`/`0x0e2c` for the domains this board uses.

### 4.3b The ROM's equivalent — `0x6d8c`, called from `0x81c3`

```
0x6d8c  rd8 0x1c00, 0x1c04 ; or cl,0x20 ; wr8    /* blank both heads */
0x52cb  delay 1006 us
0x6d92  vga_misc_clocksel(2)                     /* MISC bits 3:2 = 2 */
0x6d9a  for i = 0..3:
            div    = PInS[0x28 + 0x14*i];        /* ROM 0x7800 + byte[0x6d84+i] */
            reg    = word[0x6d7c + 2*i];         /* 0x0e10 / 0x0e14 / 0x0e18 / 0x0e1c */
            if (div != 0) {
0x6dca          wr32 reg = div | 0x80000000;     /* load strobe   */
0x6dd0          delay 201 us;
0x6dda          wr32 reg = div & 0x7fffffff;
0x6ddd          if (pll_locked(reg)) continue;   /* 0x72f4, §4.5 */
            }
0x6de4      target = PInS[0x24 + 0x14*i];
            if (target != 0) SetPixelClock(target, reg);   /* 0x7018, §4.4 */
0x6dfc  ...
0x6e0a  wr32 0x0e00 = 0x00008081;
0x6e13  wr32 0x0e00 = 0x00008081 | PInS[0x84];   /* = 0x0003c195 */
0x6e1c  wr32 0x0e00 = PInS[0x84];                /* = 0x00034114 */
0x6e1f  call 0x6f5f                              /* pick the free PLL for the pixel clock */
0x6e22  rd8 0x1c00, 0x1c04 ; and cl,0xdf ; wr8   /* unblank */
```

**PROVEN.**  The shape is the driver's, with three additions that matter:
the `0x0e00` park/merge/install triple is **identical** to the driver's
(`0x8081`, `0x8081|sel`, `sel`), so that idiom is confirmed from two
independent implementations; the divider-word path is **verified by a lock
poll** and falls back to a search if it does not lock; and when the PInS
divider word is zero — which is the case for every PLL on this board — the ROM
goes straight to its own frequency search.  That search is `SetPixelClock`, and
it is the piece the driver hides inside `HWPARInitialiseFSController`.

### 4.4 The synthesiser is a two-stage PLL — the model

Both implementations build one 32-bit candidate word out of four byte fields
and both derive the same register layout from it, so the silicon model is
settled.  Naming the fields by what they do rather than by which byte holds
them:

| field | role | register bits |
|---|---|---|
| **A** | VCO1 feedback multiplier, 1..64 | 5:0 |
| **B** | VCO2 feedback multiplier, 1..32 | 10:6 |
| **C** | stage-1 post divider, 1..32 | 15:11 |
| **D** | stage-2 post divider, 2..64 even; a bypass code means ÷2 | 21:16 |

```
VCO1     = 27000 kHz * A            must lie in [L0, L1]   (PInS limit words 0,1)
Fstage1  = VCO1 / C                 must lie in [L4, L5]   (PInS limit words 4,5)
VCO2     = Fstage1 * B              must lie in [L2, L3]   (PInS limit words 2,3)
Fout     = VCO2 / D
```

**PROVEN, three times over.**  The ROM's evaluator `0x7289` computes
`27000 * A * B / C / D` from the word `(D<<24)|(C<<16)|(B<<8)|A`; the driver's
`mtx381` `0xe4370` computes `((27000 * A / C) * B) / D` from the word
`(A<<24)|(B<<16)|(C<<8)|D` — **the same four fields in the reverse byte
order**, and the same three range tests in both.  `27000` appears as the literal
`0x6978` in the ROM at `0x70a6` and in the driver at `0xe4736`.

The two differ only in where integer truncation falls, and a bit-exact
reimplementation has to pick one: the ROM computes the single expression
`27000 * A * B / C / D`, while `mtx381` `0xe43b2`–`0xe43fa` truncates at the
stage-1 division, `((27000 * A) / C) * B / (D ? D : 2)`.  The same driver
function then uses the **untruncated** form `(27000 * A * B) / C` for its VCO2
window test (`0xe474e`–`0xe475b`), so both forms coexist in one place: exact
for the range check, double-truncated for the error figure.  The disagreement
is a few kHz on a 250 MHz clock and never changes which side of the ±0.5 %
boundary a candidate falls on for the two targets here.  **PROVEN**;
**INFERRED** that it never matters.

The full register layout of `0x0e10`/`0x0e14`/`0x0e18`/`0x0e1c`/`0x0e20`/`0x0e2c`,
with every field stored as a **negated** count:

| bits | contents | provenance |
|---|---|---|
| 5:0 | `A`, as `((A-1) & 0x20) \| ((-A) & 0x1f)` | ROM `0x7346`, driver `0xe45ff`, `pllHwInit` `0xe921d` |
| 10:6 | `B`, as `(-B) & 0x1f` (0 means 32) | ROM `0x7371`, driver `0xe4613` |
| 15:11 | `C`, as `(-C) & 0x1f` (0 means 32) | ROM `0x7383`, driver `0xe4633` |
| 21:16 | `D`: **bit 16 set alone = bypass, ÷2**; otherwise `(-D) & 0x3e`, `D` even | ROM `0x7367`/`0x735b`, driver `0xe4638`, `pllHwInit` `0xe925c` |
| 23:22 | unknown; **never written, always preserved** | both writers |
| **24** | **VCO1 locked** (read-only) | `pllUnlockStatus` `0xe9004` |
| **25** | **VCO2 locked** (read-only) | `pllUnlockStatus` `0xe9044` |
| 26 | VCO1 output driver enable | `pllGetVcoOutputDriver` `0xe8bdf`; cleared for the mode used here |
| 27 | VCO2 output driver enable | `0xe8c14`; likewise |
| 28 | from the PLL descriptor (0 on this family) | `pllHwInit` `0xe9299` |
| 29 | unknown; never written, preserve | |
| 30 | VCO2 input select | `pllGetVco2Input` `0xe8b65`; 0 on this family |
| **31** | **load strobe** — write 1 with the fields, then write 0 to latch | both writers |

**PROVEN.**  Feeding the same `(A, B, C, D)` through the ROM's encoder
(`0x7346`) and the driver's (`0xe4580`) produces byte-identical register
values, which is as strong a two-binary confirmation as this project gets.

The authoritative **decoder** — use this, not `HSLPARGetFSFreq`, for
verification code — is `pllGetDivider` `0xe8c40`:

```
A = ((v & 0x20) ? 64 : 32) - (v & 0x1f)
B = 32 - ((v >>  6) & 0x1f)
C = 32 - ((v >> 11) & 0x1f)
D = ((v >> 16) & 1) ? 2 : 64 - ((v >> 16) & 0x3e)
```

`HSLPARGetFSFreq` `0xbf280` is the same inverse but has two edge-case defects
(`A = 64` decodes as 0 at `0xbf2a6`, and `D = 64` decodes as ÷2 at `0xbf356`).
**PROVEN.**

Two values are blacklisted everywhere: **`A == 33`** and, on the fallback path,
**`D == 2`** — the latter because `HWPARWriteFSParameters` would encode a
literal 2 into the counter field where the primary path encodes it as the
bypass code (`pllHwInit` `0xe925c` maps `D == 2` to field value 1).  So on the
fallback path ÷2 is spelled `D = 0`.  A separate erratum in `mtx396` `0xe88f0`
requires that if `A` is 18 or 50, or `B`/`C`/`D` is 18, the corresponding VCO
stay below **85 %** of its declared maximum.  **PROVEN**; the physical reasons
are **INFERRED**.

### 4.4a The ROM's search — `SetPixelClock` @`0x7018`

Simple, self-contained, and the one to read first.

Bounds from the record's six limit words (each `× 1000` into kHz at
`0x7050`–`0x70a2`; the record offset comes from the table at ROM
`0x7014 = {0x2c, 0x40, 0x54, 0x68}`, i.e. `PInS + 0x2c + 0x14*i`):

```
A_min = L0 / 27000                              /* 0x70b0 */
A_max = L1 / 27000                              /* 0x70bd */
C_min = max(3, A_min * 27000 / L5)              /* 0x70cc, clamp 0x70da */
C_max = A_max * 27000 / L4                      /* 0x70f1 */
B_min = max(3, L3 * C_min / (A_max * 27000))    /* 0x7117, clamp 0x7121 */
B_max = L2 * C_max / (A_min * 27000)            /* 0x7145 */
```

Iteration is `A` outermost, then `B`, then `C`, then `D` innermost stepping by
2 from 0 to 0x40 (`0x7232`, `0x7245`, `0x725a`, `0x726f`).  Acceptance, in the
order applied:

1. relative error `|target - Fout| * 10000 / target < 0x33`, i.e. **better than
   0.51 %** — not an exact match (`0x72ce` computes it, `0x7183` compares);
2. the candidate is **written to the register and lock-tested** (`0x718b` →
   `0x7346`, `0x718e` → `0x72f4`); one that does not lock is skipped;
3. `A != 33` (`0x719e`) and `D != 2` (`0x71ae`);
4. `L0 <= 27000*A <= L1` (`0x71b4`);
5. `L4 <= 27000*A/C <= L5` (`0x71ce`);
6. `L2 <= 27000*A*B/C <= L3` (`0x71f2`).

**PROVEN**, every test and address.  Note the ordering defect: the register is
programmed at step 2, *before* the range checks of 4–6, so the ROM transiently
writes candidates its own limits reject.  Apply 3–6 first.

Reimplementing this exactly and taking the first candidate that passes every
*static* test (step 2 needs hardware) gives, for this board:

| target | A | B | C | D | achieved | error | register |
|---|---|---|---|---|---|---|---|
| `0x0e10` 250000 kHz | 24 | 17 | 11 | 4 | 250363 kHz | +0.15 % | `0x003cabc8` |
| `0x0e14` 550000 kHz | 23 | 16 | 9 | 0 (=÷2) | 552000 kHz | +0.36 % | `0x0001bc09` |

and, as a check of the reimplementation against a constant recovered
independently from the ROM's own mode tables, 108000 kHz gives
`A=24, B=3, C=3, D=6` → **exactly 108000 kHz**, register `0x003aef48`.
§10 has the code.

### 4.4b The driver's primary path — and it finds a better answer

`HWPARInitialiseFSController` `0xe4fc0` is **not** primarily a search.  At
`0xe5078` it calls `pllInit` `0xeaf50`, and at `0xe507d` a return of 0
(success) makes it return immediately; the four-nested-loop search is the
**fallback** that runs only when `pllInit` fails.  A chip with no VBIOS-supplied
divider word takes the `pllInit` path.  **PROVEN.**

`pllInit` range-checks and clamps the target, calls `pllHwOscOn` (§2.1), and
dispatches on the descriptor's algorithm selector (2 here) to **`mtx402`
`0xeaaf0`**, whose search is a different shape from the ROM's:

```
best = INT_MAX;
for (B = 32; B >= 2; B--)                                   /* 0xeab76, 0xeac39 */
  for (D = 2; D <= 64; D += 2) {                            /* 0xeaba0, 0xeabff */
      f1 = target * D / B;                                  /* wanted stage-1 output */
      if (f1 < L4 || f1 > L5) continue;                     /* 0xeabe3 */
      (A, C) = mtx400(27000, f1, &stage1desc);              /* 0xeacf2 */
      f = (27000*A/C) * B / D;
      if (pllIsInvalidDividers(...)) continue;              /* 0xead4e */
      if (|f - target| >= best) continue;                   /* 0xead6b */
      if (pllTestDividers(dev, dry_run, ...)) continue;     /* 0xeadba */
      best = |f - target|;  save (A, B, C, D);
  }
if (!found) return 8;
return pllTestDividers(dev, commit, ...);                   /* 0xeaea1 */
```

with `mtx400` `0xea080` choosing `A` and `C` for a wanted stage-1 output by
scanning `A` from 64 down to 2, skipping `A == 33`, and taking
`C = round(A * 27000 / f1)`.  Unlike the ROM's first-fit, this keeps the
**best** candidate.  `pllTestDividers` `0xe96a0` re-validates the ranges, runs
the `mtx396` erratum check, and calls **`pllHwInit` `0xe9170`**, the primary
register writer:

```
v = read(reg) | 0x80000000;
v = (v & ~0x0000003f) | <A field>;        /* 0xe921d */
v = (v & ~0x000f800)  | <C field>;        /* 0xe923b */
v = (v & 0xfffff83f)  | <B field>;        /* 0xe924c */
v = (v & 0xffc0ffff)  | <D field>;        /* 0xe925c, D == 2 -> bypass code 1 */
v = (v & ~0x40000000) | <bit 30 from descriptor, 0 here>;
v = (v & ~0x10000000) | <bit 28 from descriptor, 0 here>;
v &= 0xf3ffffff;                          /* clear bits 26,27  0xe92b3 */
write(reg, v);                            /* strobe still set */
v &= 0x7fffffff; write(reg, v);           /* release, no delay  0xe9503 */
HWDelay(10);
require VCO1 lock (bit 24) — 100 samples, 0 % unlocked, else error 0x1010
HWDelay(10);
require VCO2 lock (bit 25) — 100 samples, 0 % unlocked, else error 0x2010
```

**PROVEN.**  Note this path demands **100 of 100** locked samples where
`HWPARFSWaitLock` accepts 90 of 100 (§4.5).

Simulating `mtx402` + `mtx400` with this board's PInS limits gives a materially
better answer than the ROM's first-fit, and for FS1 an exact one:

| target | A | B | C | D | achieved | error | register |
|---|---|---|---|---|---|---|---|
| `0x0e10` 250000 kHz | 39 | 19 | 20 | 4 | 250087 kHz | +0.035 % | **`0x003c6379`** |
| `0x0e14` 550000 kHz | 50 | 22 | 27 | 2 (bypass) | **550000 kHz** | **0** | **`0x00012aae`** |

FS1's chain is `27000 × 50 = 1350000` kHz VCO1, `/27 = 50000` kHz stage 1
(exactly at the 50 MHz floor), `× 22 = 1100000` kHz VCO2, `÷2 = 550000` kHz —
and `A = 50` is one of the `mtx396` erratum values, so VCO1 must stay under
85 % of FS1's 1600 MHz ceiling: 1350 ≤ 1360, which it does, barely.  An exact
250000 kHz needs `27·A·B = 250·C·D`, whose only solutions require `A ∈ {25,
50}`, and every one of those violates either the 50 MHz stage-1 floor or FS0's
1200 MHz VCO ceiling — so 250087 kHz is the optimum under FS0's own limits.

**PROVEN** (the algorithm and the encoding); **INFERRED** (that these are the
values the silicon settles on, because the lock test cannot be evaluated
statically).  **These are the two values a driver should program**, with the
ROM's §4.4a pair as a fallback if they do not lock.  The POSTed register dump
of §9 item 1 would decide it outright.

Because neither candidate hits its target exactly, the three clock domains of
§4.2 land slightly off nominal: with the primary pair, 550000/2 = **275.0 MHz**
for the memory domain, 250087 → **250.1 MHz**, and 550000 × 2/5 =
**220.0 MHz** for the core — i.e. the retail figures to within 0.04 %.  (With
the ROM pair they would be 276.0, 250.4 and 220.8 MHz.)  A driver should report
what it programmed rather than the PInS target if anything downstream depends
on it; nothing in our driver does today.

### 4.4c The driver's fallback search, and its validator

When `pllInit` fails, `HWPARInitialiseFSController` `0xe508d`–`0xe5359` runs a
four-nested-loop search over `A ⊃ B ⊃ C ⊃ D` (`D` stepping by 2), with bounds
in a 0x38-byte limit structure that `HWPARInitialiseFSLimit` `0xe4b20` fills
from PInS:

| limit struct | contents | from |
|---|---|---|
| `+0x00` | MMIO register offset `0x0e10 + 4*i` | `0xe4b34` |
| `+0x04` | target kHz | 4th argument |
| `+0x08` | the candidate word under test | written by the caller at `0xe5219` |
| `+0x0c`..`+0x20` | `L0`..`L5`, the six PInS limit words × 1000 | `0xe4b5b`–`0xe4ba0` |
| `+0x24`/`+0x25` | `A` min/max | `L0/27000`, `L1/27000` |
| `+0x26`/`+0x27` | `B` min/max | derived, min clamped to 3 |
| `+0x28`/`+0x29` | `C` min/max | derived, min clamped to 3 |
| `+0x2a`/`+0x2b` | `D` min/max = 0 / 64 | constants `0xe4ba7`, `0xe4bae` |

reading `PInS + 0x2c/0x40/0x54/0x68 + {0,2,4,6,8,0xa}` for `i = 0..3`
(`0xe4b4c`, `0xe4c98`, `0xe4d38`, `0xe4ce8`) — the same records as §4.7.
For this board that gives `A` 22..44, `B` and `C` 3..23, `D` 0..64 for FS0, and
`A` 22..59, `B` and `C` 3..31 for FS1 — **identical to the ROM's bounds**
(§4.4a), which is one more cross-confirmation.  **PROVEN.**

The search collects at most **20** accepted candidates (every loop test is
`and`ed with `count <= 19`, `0xe50d8` and friends), sorts them with
`HWPARAddFSParamInTable` `0xe4810` — first by "contains no field equal to 18"
(`mtx378` `0xe4300`), then ascending by absolute relative error (`mtx379`
`0xe4490`) — and then writes them in that order until one locks.  The table
`mtx383` `0x34c680` is `.bss`, 24 slots, filled at run time; there is nothing
to dump from the file.  **PROVEN.**

Acceptance is `HWPARValidateFSParam` `0xe46d0`, and it matches the ROM's tests
exactly:

```
mtx382(dev, family, p, &err);                /* 0xe470e: (Fout-target)*10000/target */
ok = ((err + 0x32) < 0x65);                  /* |err| <= 50, i.e. +-0.5 %   0xe4718 */
if (!(L0    <= 27000*A     && 27000*A     <= L1   )) ok = 0;   /* 0xe475e */
if (!(50000 <= 27000*A/C   && 27000*A/C   <= 400000)) ok = 0;  /* 0xe4776, hardcoded */
if (!(L2    <= 27000*A*B/C && 27000*A*B/C <= L3   )) ok = 0;   /* 0xe478b */
if (A == 33) ok = 0;                                           /* 0xe47a6 */
if (D == 2)  ok = 0;                                           /* 0xe47ab */
```

**PROVEN.**  Same three quantities, same limit words, same tolerance boundary,
same two exclusions as the ROM.  Two differences that only a bit-exact
reimplementation would notice:

* the driver **hardcodes** the stage-1 window as 50000..400000 kHz
  (`lea -0xc350(%rcx),%eax ; cmp $0x55730,%eax` at `0xe4776`) where the ROM
  reads `L4`/`L5`.  On this board those are 50 and 400 MHz, so they agree — but
  in the driver `L4`/`L5` feed **only** the `B` search bounds in
  `HWPARInitialiseFSLimit`, never a range test;
* the error figure is computed as `((Fout - target) * 1000) / (target / 10)`
  (`0xe4439`–`0xe445e`), which truncates the denominator first and so rounds
  slightly differently from the ROM's `(Fout - target) * 10000 / target` — the
  accept/reject boundary is the same constant either way.

**PROVEN.**  If a driver only ever has to reach the two targets in §4.4b, none
of this changes the answer; it matters if the search is reused for arbitrary
frequencies.
### 4.5 The lock poll — ROM `0x72f4` and driver `HWPARFSWaitLock` @`0xe4510`

Present in both binaries, and byte-for-byte identical on the fallback path.
(The driver polls in exactly two places — `HWPARFSWaitLock`, and the primary
path's `pllUnlockStatus` `0xe8f70` / `mtx397` `0xe9080`, which test bits 24 and
25 **separately** at `0xe9004`/`0xe9120` and `0xe9044`/`0xe915a` and demand
**0 %** unlocked rather than ≤ 10 %.  Nothing in `HWPARValidateFSParam`,
`HWPARAddFSParamInTable` or `HWPARInitialiseFSController` touches the register:
a grep of the whole `0xe4300..0xeb900` range for the two bit tests finds only
those sites.  **PROVEN, negative result included.**)

The fallback poll:

```
0x72ff  if (reg == 0x0e20 || reg == 0x0e2c)
0x731b      reg = word[0x6d7c + 2 * [cs:0x7ff0]];   /* substitute the current pixel PLL */
0x731e  delay 201 us                                /* settle */
0x7324  reg += 3                                    /* byte 3 = bits 31:24 */
0x7329  hits = 0; for (i = 0; i < 100; i++)
0x732b      if ((rd8(reg + 3) & 0x03) == 0x03) hits++;
0x733c  locked = (hits >= 90);
```

So **bits 24 and 25 of a synthesiser register are its lock/status bits, both
must read 1**, sampled 100 times back to back with at least 90 hits after a
fixed 201 µs settle.  There is no time-based timeout: the settle time *is* the
delay and the poll is a fixed sample count.  **PROVEN.**

The measured un-POSTed values `0x0e10..0x0e2c = 0x80000000` have bits 24 and 25
**clear**, i.e. not locked — consistent, and a further read-only POST signal
(§2.2).

One quirk, recorded literally because it has no obvious explanation: when the
caller is programming `0x0e20` or `0x0e2c`, the lock test does **not** read
those registers; it reads the *pixel* PLL register selected by the ROM scratch
byte `[cs:0x7ff0]` (`0x72ff`–`0x731b`).  Do not copy that.  **PROVEN** (the
code); unexplained.

### 4.6 `0x0e20` / `0x0e2c` — the legacy VGA clocks, `0x6e29`

Programmed once, at init step `0x00d8`, i.e. **after** the memory controller:

```
0x6e29  reg = 0x0e20 ; flags = [cs:0x7ff5] ; vga_misc_clocksel(0)
        if ((flags & 1) == 0 || (flags & 0x10) != 0) goto divider_path
        else pick a target from flags (bit3 -> 162000, bit2 -> 108000,
             bit1 -> 65000, else 65000) and SetPixelClock
divider_path:
0x6e63  v = PInS[0x78];                       /* = 0 on this board */
0x6e69  if (v == 0) v = 0x001a9401;           /* built-in fallback, ROM 0x6d74 */
0x6e7c  wr32 0x0e20 = v | 0x80000000 ; delay 201 us ; wr32 0x0e20 = v & 0x7fffffff
0x6e8f  if (!pll_locked()) SetPixelClock(PInS[0x74] /* 25175 kHz */, 0x0e20)

0x6ea1  reg = 0x0e2c ; vga_misc_clocksel(1) ; same shape
0x6edb  v = PInS[0x80];                       /* = 0 on this board */
0x6ee1  if (v == 0) v = 0x00266cbb;           /* built-in fallback, ROM 0x6d78 */
0x6efa  wr32 0x0e2c = v | 0x80000000 ; delay 201 us ; wr32 0x0e2c = v & 0x7fffffff
0x6f0d  if (!pll_locked()) SetPixelClock(PInS[0x7c] /* 28311 kHz */, 0x0e2c)
```

**PROVEN.**  Note that the two fallback words `0x001a9401` and `0x00266cbb` are
**not** in the four-field encoding of §4.4 — decoded that way they give an
VCO1 multiplier of 1,
which violates the ROM's own VCO minimum.  So `0x0e20`/`0x0e2c` are a different
register class from `0x0e10..0x0e1c`; treat those two constants as opaque.
**PROVEN** (the arithmetic does not work out); **INFERRED** (the conclusion).
A driver that only needs the three clock domains of §4.2 does not need this
step at all — but a driver that wants legacy VGA text mode does.

### 4.7 The PInS PLL records

`HWPARInitialiseFSLimit` `0xe4b20` reads six `u16` limits in MHz and multiplies
each by 1000 (`imul $0x3e8`), giving a 20-byte record per synthesiser at
`PInS + 0x24 + 0x14*i`:

| offset in record | contents |
|---|---|
| `+0x00` | target frequency in kHz, used only when the divider word is 0 |
| `+0x04` | ready-made divider word, written straight to the FS register |
| `+0x08`, `+0x0a` | VCO min, max (MHz) |
| `+0x0c`, `+0x0e` | alternate VCO min, max (MHz) |
| `+0x10`, `+0x12` | output min, max (MHz) |

`HWPARInitialiseFSLimit` also carries the 27 MHz reference clock as the
constant `0x6978` = 27000 (`0xe4bd1`), the same reference the VBIOS pixel-clock
search uses ([`parhelia-vbios.md`](parhelia-vbios.md)).  **PROVEN.**

Read out of both ROM images (identical):

| i | FS reg | target kHz (`+0x24`) | divider word (`+0x28`) | VCO MHz | alt MHz | out MHz |
|---|---|---|---|---|---|---|
| 0 | `0x0e10` | `0x0003d090` = **250000** | **`0x00000000`** | 600–1200 | 600–1200 | 50–400 |
| 1 | `0x0e14` | `0x00086470` = **550000** | **`0x00000000`** | 600–1600 | 600–1600 | 50–400 |
| 2 | `0x0e18` | 0 | 0 | 600–1200 | 600–1200 | 50–400 |
| 3 | `0x0e1c` | 0 | 0 | 600–1200 | 600–1200 | 50–400 |

**PROVEN** (extracted from `captures/parhelia-0528-cold/parhelia-0528-vbios.rom`).
This also completes the record layout that
[`parhelia-vbios.md`](parhelia-vbios.md) left open: the ROM's PLL index table at
`0x6d84` points at `0x7800 + {0x28, 0x3c, 0x50, 0x64}`, which — now that PInS is
known to live at ROM `0x7800` — are exactly the **divider-word** fields
`PInS+0x28 + 0x14*i`.  The "20-byte record with a dword then six words of
limits" of that document is this record minus its first dword; the target
frequency at `+0x24 + 0x14*i` was outside its view.  So the VBIOS's own
"the default divider words are zero in the ROM image, they are patched at
runtime from straps" is only half right: they are zero, and the *driver*
computes them.

**Both divider words this board actually uses are zero.**  The short "write the
PInS word with bit 31, wait, rewrite" path is therefore *not* available on this
board: the frequency has to be searched for.  That is what §4.4 decodes, and it
is why the ROM path is the one to implement — the driver's
`HWPARInitialiseFSController` `0xe4fc0` does the same job in a far less
tractable way (a 0x118-byte `.rodata` template at `0x34b3c0`, four byte-ranged
fields at template offsets `0x24..0x2b`, candidates validated by
`HWPARValidateFSParam` `0xe46d0` and collected up to 20 at a time, then written
in turn by `HWPARWriteFSParameters` until one succeeds).  Both end at the same
register encoding (§4.4), so the simple one is the correct one.

Note that the limit ranges only make sense once the §4.4 relation is in hand:
`L0..L1` bound `N × 27 MHz`, `L4..L5` bound `N × 27 / B`, and `L2..L3` bound
`N × X × 27 / B`, none of which is `Fout`.  FS1's 550 MHz target being outside
both of its own "VCO" and "output" ranges is therefore not a contradiction —
`Fout` is `(N × X × 27 / B) / A`, and 550 MHz comes out of a 1104 MHz
post-multiplier value divided by 2.

What `HWPARWriteFSParameters` `0xe4580` does with a chosen parameter word
(partially decoded; the remainder is the open item in §7):

```
v = ReadReg(reg);
v = (v | 0x80000000) & ~0x4c000000;    /* assert strobe, clear bits 30,27,26  0xe45cb */
WriteReg(reg, v);
HWDelay(10);                                                         /* 0xe45de */
p = param_word;
v &= 0xfffff800;
v |= ((((p >> 24) - 1) & 0x20) | ((-(p >> 24)) & 0x1f));   /* bits  5:0  0xe45f4 */
v |= ((-(p >> 16)) & 0x1f) << 6;                           /* bits 10:6  0xe460c */
v |= ((-(s8)(p >> 8)) & 0x1f) << 11;                       /* bits 15:11 0xe461d */
v  = (v & 0xffc0ffff)
   | (((p & 0xff) ? 2 * ((-((p & 0xff) >> 1)) & 0x1f) : 1) << 16);   /* bits 21:16 0xe4638 */
v |= 0x80000000;                                                     /* 0xe4660 */
WriteReg(reg, v);                                                    /* 0xe4669 */
...
```

so an FS register holds four counter fields at bits 5:0, 10:6, 15:11 and 21:16,
each stored as a **negated** count, with bit 31 the load strobe.  **PROVEN** as
far as shown.  `HSLPARGetFSFreq` `0xbf280` is the inverse and is the way to
verify a programmed synthesiser by reading it back.

---

## 5. The memory controller — `HWPARInitializeMemoryController` @`0xe4110`

### 5.1 The banked window

The Parhelia-512 has a 256-bit memory interface built from **four independent
64-bit controllers**, addressed through a bank-select register:

* `SelectMemoryController(dev, mask)` `0xe3af0` writes **`0x0f00`** with one
  byte per bank ORed together: bank 0 → `0x00000001`, bank 1 → `0x00000100`,
  bank 2 → `0x00010000`, bank 3 → `0x01000000`.  **PROVEN** (the `cmov` chain
  at `0xe3af7`–`0xe3b27`).
* `WriteMemoryControllerDword(dev, reg, mask, val)` `0xe3bf0`: read and save
  `0x0f00`, select `mask`, write `reg`, re-read `0x0f00` and rewrite the saved
  value if it changed.  `ReadMemoryControllerDword` `0xe3c80` is the same shape
  around a read.  **PROVEN.**
* `WaitForMemoryIdle(dev, mask)` `0xe3d20`: for each of the banks
  `1, 2, 4, 8` present in `mask`, poll bank register **`0x0f08`** and loop
  **while bit 31 is *clear***, i.e. wait for it to become **set**, at most
  `0x186a0` = 100000 iterations; `0x0f00` is saved and restored around the whole
  loop.  So **`0x0f08` bit 31 is a ready/done flag, not a busy flag.**
  **PROVEN** — `testb $0x80,0x3(%r13)` / `setne %al` at `0xe3d73`, then
  `test $1,%al` / `sete %dl` at `0xe3d7d` so `dl` is "bit 31 clear", and
  `test %eax,%edx` / `jne` at `0xe3d8d` re-enters the loop while
  (bit 31 clear) **and** (count ≤ 99999).  The ROM agrees independently: at
  `0x83ec`–`0x83f9` it reads `0x0f08`, `test ecx,0x80000000`, `jnz` **exits**,
  and otherwise decrements a `0xffff` counter and re-reads.  Two binaries, same
  polarity.  The name "WaitForMemoryIdle" is the vendor's; the sense is
  "wait for ready".
* `HWResetMemoryPhaseError(dev)` `0xe3ff0`, per bank in the order 0,1,2,3:
  set `0x0f08` bit 2, write; then `WaitForMemoryIdle`; then for each bank set
  `0x0f24` bit 16, write, `HWDelay(10)`, clear bit 16, write; then for each bank
  clear `0x0f08` bit 2; then `HWDelay(100)`.  **PROVEN.**
  So `0x0f08` bit 2 is a training/hold bit and `0x0f24` bit 16 a phase-reset
  strobe, complementing the `0x0f24[1:0]` error status of §2.4.

**Do not confuse this with the P690.**  There `0x0ffc` is the bank select (two
banks), `0x0f00` is data, and the idle poll is `0x0ff8` bits 9,10.

### 5.2 Which banks get programmed

```
pins   = HWPARGetPINS();                                        /* 0xe41ca */
half   = pins[0x8c] & 8;                                        /* 0xe41d9, 0xe41f5 */
devid  = (HWReadConfigSpaceDword(0) >> 16);                     /* 0xe41e0 */
narrow = ((devid - 0x2537) <= 1);        /* 0x2537 LX, 0x2538 SUNEX */

                     GetMC(2, buf); SetMC(2, buf);   /* bank 1   0xe4205/0xe4215 */
if (!narrow)       { GetMC(1, buf); SetMC(1, buf); } /* bank 0   0xe42b0 */
if (!half) {
    if (!narrow)   { GetMC(4, buf); SetMC(4, buf); } /* bank 2   0xe42d5 */
                     GetMC(8, buf); SetMC(8, buf);   /* bank 3   0xe4236/0xe4251 */
}
```

**PROVEN.**  For `0x0527`/`0x0528`, `narrow == 0`, so all four banks are
programmed unless `PInS[0x8c]` bit 3 says the board is half populated.  This
board has `PInS[0x8c] = 0x9d7f0007`, bit 3 clear → **four banks**.  The bank
order is **1, 0, 2, 3** — bank 1 first.  **INFERRED**: bit 3 means "128-bit /
half-width memory"; **PROVEN**: the test and the branch structure.

The full function, with the holds:

```
save 0x1c00, 0x1c04, 0x1800, 0x1a00;              /* 0xe4125..0xe4165 */
write 0x1c00 = v | 0x20;                          /* 0xe416a */
write 0x1c04 = v | 0x20;                          /* 0xe417e */
write 0x1800 = v & ~1;                            /* 0xe4192 */
write 0x1a00 = v & ~1;                            /* 0xe41a6 */
HWDelay(1000);                                    /* 0xe41ba */
<the per-bank Get/Set above>
restore 0x1c00, 0x1c04, 0x1800, 0x1a00;           /* 0xe4256..0xe4295 */
HWResetMemoryPhaseError(dev);                     /* 0xe429a */
```

### 5.3 PInS → register

`HWPARGetMCRegisters(dev, memtype, out)` `0xe3b30` picks one of four 13-dword
(52-byte) PInS records by the same value that is later used as the bank mask:

| mask / `memtype` | bank | PInS record | provenance |
|---|---|---|---|
| `1` | 0 | `PInS + 0x0a4` | `0xe3b5d` |
| `2` | 1 | `PInS + 0x0d8` | `0xe3be0` |
| `4` | 2 | `PInS + 0x10c` | `0xe3b46` |
| `8` | 3 | `PInS + 0x140` | `0xe3b53` |

It then scatters the 13 dwords into a 64-byte struct, and
`HWPARSetMCRegisters` `0xe3dc0` reads them back out.  Composing the two
permutations away gives the direct, implementable mapping — **record index →
MMIO register**:

| record index | MMIO | record index | MMIO |
|---|---|---|---|
| 0 | `0x0f08` (memory configuration) | 7 | `0x0f10` |
| 1 | `0x0f04` | 8 | `0x0f48` |
| 2 | `0x0f0c` | 9 | `0x0f4c` |
| 3 | `0x0f20` | 10 | `0x0f54` |
| 4 | `0x0f14` | 11 | `0x0f58` |
| 5 | `0x0f18` | 12 | `0x0f50` |
| 6 | `0x0f1c` | | |

**PROVEN** (`HWPARGetMCRegisters` `0xe3b62`–`0xe3bd2` for the scatter,
`HWPARSetMCRegisters` `0xe3e3f`–`0xe3f5a` for the gather).

The four records in this board's ROM, all 13 dwords, little-endian:

| idx | MMIO | bank 0 (`+0x0a4`) | bank 1 (`+0x0d8`) | bank 2 (`+0x10c`) | bank 3 (`+0x140`) |
|---|---|---|---|---|---|
| 0 | `0x0f08` | `3978c200` | `3978c200` | `3978c200` | `3978c200` |
| 1 | `0x0f04` | `80041472` | `80041472` | `80041472` | `80041472` |
| 2 | `0x0f0c` | `19232b7a` | `19232b7a` | `19232b7a` | `19232b7a` |
| 3 | `0x0f20` | `00000025` | `00000025` | `00000005` | `00000005` |
| 4 | `0x0f14` | `10240902` | `10240902` | `10240902` | `10240902` |
| 5 | `0x0f18` | `00040902` | `00040902` | `00040902` | `00040902` |
| 6 | `0x0f1c` | `00000408` | `00000408` | `00000408` | `00000408` |
| 7 | `0x0f10` | `000e607f` | `000e607f` | `000e617f` | `000e617f` |
| 8 | `0x0f48` | `55555555` | `55555555` | `55555555` | `55555555` |
| 9 | `0x0f4c` | `55555555` | `55555555` | `55555555` | `55555555` |
| 10 | `0x0f54` | `77777777` | `77777777` | `77777777` | `77777777` |
| 11 | `0x0f58` | `01010000` | `01010000` | `01010000` | `01010000` |
| 12 | `0x0f50` | `00000000` | `00000000` | `00000000` | `00000000` |

**PROVEN** (extracted from the committed ROM capture; identical in the older
`vbios-matrox-parhelia` image).  Banks 2 and 3 differ from 0 and 1 in exactly
two words (`0x0f20` and `0x0f10`), which is why the vendor keeps four records
rather than one — a driver must not collapse them.

### 5.4 `HWPARSetMCRegisters` @`0xe3dc0` — the per-bank sequence

`cfg = record[0]` (the value destined for `0x0f08`) and
`train = ((cfg >> 16) & 0xfff) << 16`:

```
 1. SelectMemoryController(mask);                                 /* 0xe3dd4 */
 2. WriteMC(0x0f08, mask, 0x1004);                                /* 0xe3de9 */
 3. WaitForMemoryIdle(mask);                                      /* 0xe3df4 */
 4. WriteMC(0x0f5c, mask, 0x138);                                 /* 0xe3e09 */
 5. WriteMC(0x0f40, mask, 0);                                     /* 0xe3e1b */
 6. WriteMC(0x0f44, mask, 0);                                     /* 0xe3e2d */
 7. HWDelay(5000);                                                /* 0xe3e3a */
 8. WriteMC(0x0f04, mask, record[1]);                             /* 0xe3e4e */
 9. WriteMC(0x0f08, mask, train | 0x9004);                        /* 0xe3e7e */
10. WriteMC(0x0f0c, mask, record[2]);                             /* 0xe3e92 */
11. WriteMC(0x0f20, mask, record[3]);                             /* 0xe3ea6 */
12. WriteMC(0x0f14, mask, record[4]);                             /* 0xe3eba */
13. WriteMC(0x0f18, mask, record[5]);                             /* 0xe3ece */
14. WriteMC(0x0f1c, mask, record[6]);                             /* 0xe3ee2 */
15. WriteMC(0x0f10, mask, record[7]);                             /* 0xe3ef6 */
16. WriteMC(0x0f48, mask, record[8]);                             /* 0xe3f0a */
17. WriteMC(0x0f4c, mask, record[9]);                             /* 0xe3f1e */
18. WriteMC(0x0f54, mask, record[10]);                            /* 0xe3f32 */
19. WriteMC(0x0f58, mask, record[11]);                            /* 0xe3f46 */
20. WriteMC(0x0f50, mask, record[12]);                            /* 0xe3f5a */
21. WriteMC(0x0f5c, mask, 0x130);                                 /* 0xe3f6f */
22. HWDelay(10000);                                               /* 0xe3f7c */
23. WriteMC(0x0f5c, mask, 0x100);                                 /* 0xe3f91 */
24. WriteMC(0x0f08, mask, train | 0x9005);                        /* 0xe3fa4 */
25. HWDelay(1000);                                                /* 0xe3fb1 */
26. WriteMC(0x0f08, mask, record[0]);                             /* 0xe3fc5 */
27. HWDelay(1000);                                                /* 0xe3fdd */
```

**PROVEN**, every step and address.  Ordering constraints that are not
negotiable, because they are the actual DRAM initialisation:

* Step 2 puts the controller into a known state (`0x0f08 = 0x1004`) and
  step 3 waits for it to go idle **before** any timing register is touched.
* `0x0f5c` is the sequencer: `0x138` arms it (step 4), `0x130` launches the
  training/initialisation run (step 21), `0x100` parks it (step 23).  The
  10 ms between 21 and 23 is the training time.
* `0x0f08` is written **four times** with three different values — `0x1004`,
  `train|0x9004`, `train|0x9005`, then the real `record[0]` — and only the last
  one installs the board's configuration.  `train` re-uses bits 27:16 of
  `record[0]`; for this board `record[0] = 0x3978c200`, so
  `train = 0x09780000`, and the two intermediate values are `0x09789004` and
  `0x09789005`.  **PROVEN** (the `movzwl 0x2(%r13)` / `and $0xfff` /
  `shl $0x10` at `0xe3e53`–`0xe3e78`).
* `0x0f40` and `0x0f44` are cleared before the timing words and never written
  again; they are not in the PInS record.
* The whole sequence runs with the engine and both display controllers in the
  §5.2 hold, and `HWResetMemoryPhaseError` runs once *after* all banks and
  *after* the holds are released.

### 5.5 The ROM's own version — `0x837f`, and where it differs

```
0x837f  wr32 0x0f00 = 0x00000100 ; call 0x83dd with the PInS+0x0d8 record  /* bank 1 */
0x8391  wr32 0x0f00 = 0x00000001 ; call 0x83dd with the PInS+0x0a4 record  /* bank 0 */
0x83a3  wr32 0x0f00 = 0x00010000 ; call 0x83dd with the PInS+0x10c record  /* bank 2 */
0x83b5  wr32 0x0f00 = 0x01000000 ; call 0x83dd with the PInS+0x140 record  /* bank 3 */
0x83d0  wr32 0x12b8 = 0x00070601
0x83d9  wr32 0x12b0 = 0x00000000
```

and `0x83dd`, per bank:

```
0x83e6  wr32 0x0f08 = 0x00001004
0x83ec  poll 0x0f08 until bit 31 is SET, at most 0xffff reads, then continue anyway
0x8404  wr32 0x0f5c = 0x00000138
0x8410  wr32 0x0f40 = 0
0x841c  wr32 0x0f44 = 0                      /* NO delay here */
0x8427  wr32 0x0f04 = record[1]
0x843f  wr32 0x0f08 = (record[0] & 0x0fff0000) | 0x9004
0x844a  wr32 0x0f0c = record[2]
0x8455  wr32 0x0f20 = record[3]
0x8460  wr32 0x0f14 = record[4]
0x846b  wr32 0x0f18 = record[5]
0x8476  wr32 0x0f1c = record[6]
0x8481  wr32 0x0f10 = record[7]
0x848c  wr32 0x0f48 = record[8]
0x8497  wr32 0x0f4c = record[9]
0x84a2  wr32 0x0f54 = record[10]
0x84ad  wr32 0x0f58 = record[11]
0x84b8  wr32 0x0f50 = record[12]
0x84c4  wr32 0x0f5c = 0x00000130
0x84ca  delay 20.11 ms
0x84d6  wr32 0x0f5c = 0x00000100
0x84ee  wr32 0x0f08 = (record[0] & 0x0fff0000) | 0x9005
0x84f4  delay 20.11 ms
0x84fe  wr32 0x0f08 = record[0]
0x8504  delay 20.11 ms
```

**PROVEN.**  The register order, the record→register mapping and every magic
value — `0x1004`, `0x138`, `0x130`, `0x100`, `0x9004`, `0x9005` and the
`(record[0] >> 16) & 0xfff` re-use (which the ROM writes as
`and ecx,0x0fff0000 ; or ecx,0x9004` at `0x8431`/`0x8438`) — are **identical to
the driver's**.  That is an independent confirmation of the whole of §5.3 and
§5.4 from a second implementation, including the bank order **1, 0, 2, 3**.
The ROM never ORs bank masks together: exactly one bank at a time, and `0x0f00`
is only ever written, never read.

Four differences, all in timing and all worth knowing before writing code:

| | driver (`0xe3dc0`) | ROM (`0x83dd`) |
|---|---|---|
| after `0x0f44 = 0` | `HWDelay(5000)` ≈ 5 ms | **no delay** |
| after `0x0f5c = 0x130` | `HWDelay(10000)` ≈ 10 ms | **20.11 ms** |
| after `0x0f08 = …9005` | `HWDelay(1000)` ≈ 1 ms | **20.11 ms** |
| after `0x0f08 = record[0]` | `HWDelay(1000)` ≈ 1 ms | **20.11 ms** |
| ready poll bound | 100000 | 65535, then continues regardless |

The ROM's delays are real PIT-timed microseconds (§1.0b) while the driver's are
read counts (§1.1), so the ROM's are the trustworthy figures.  Take the **maximum
of the two at every point** — 5 ms after `0x0f44`, then 21 ms, 21 ms, 21 ms —
and the sequence is safe under both readings.  **INFERRED** (that taking the
maximum is correct); **PROVEN** (both sets of values).

The two writes after all four banks, `0x12b8 = 0x00070601` and `0x12b0 = 0`,
are the same pair the driver issues at `0xbff33`/`0xbff5b` (§6 steps 15 and 16),
so they belong to the memory-controller bring-up rather than to the engine
init.  The `0x12c0`/`0x12c4`/`0x12f4` block is *not* part of `0x837f`: it runs
earlier, at ROM `0x829e` (§2.3).  **PROVEN.**

### 5.6 What is *not* in the sequence

No memory sizing, no pattern write, no read-back verification: the vendor
trusts PInS completely.  It also never writes the frame-buffer aperture during
bring-up, which matters for us — the machine reset observed on a BAR0 write is
what the whole exercise is about, and nothing in §4 or §5 requires touching
BAR0.  VRAM must not be read or written until §5 has completed and §2.4 reports
all four banks clean.  **PROVEN (negative result)** for the vendor path;
**INFERRED** that §2.4 is a sufficient gate.

The ROM is the same and even more blunt about it.  The routine the init chain
calls at `0x00eb` to obtain the memory size is `0x8ac7` → `0x8ad2`, and
`0x8ad2` is two instructions:

```
0x8ad2  mov ax,0x100
0x8ad5  ret
```

— a hardcoded constant, which `0x8ac7` then shifts right by 4 and stores.
`get_BAR0_base` (`0x6c79`, which reads PCI cfg `0x10`) has exactly **one**
caller in the entire image, `0x5d3c`, the VBE GetModeInfo handler, where it
only fills `PhysBasePtr` into the ModeInfoBlock — nothing in the init chain
ever obtains the frame-buffer address at all.  The only aperture traffic in the
whole ROM init is the 48 KiB `rep stosb` zero-fill of the legacy `A000:2000`
window at `0x00f5` and the font copy at `0x8356`, both **after** the memory
controller is up and neither reading anything back.  **PROVEN.**

So there is no sizing algorithm to copy: the VRAM size must come from the BAR
length, as our driver already does.  And there is no vendor precedent for
touching BAR0 before §5 completes — in either implementation.

---

## 6. What follows, in vendor order

`HSLPARInitializeHardware` `0xbf780` in full, so the clock and memory steps sit
in their real context.  `rbx = dev + 0x3b78` is the driver's PInS copy.

```
 1. if ((dev->status & 0xc0000000) != 0 && != 0x40000000) return;     /* 0xbf7af */
 2. v = ReadReg(0x0e00);
    if (v == 0 || v == 0x40000000) HSLPARHardwareReset(dev);          /* 0xbf7f3, 0xc003f */
 3. dev->pcix_state |= 2;                                             /* 0xbf80e */
 4. HSLPARApplyVidReset(dev, all heads);                              /* 0xbf824 */
 5. 0x1c00 |= 0x00809020;  0x1c04 |= 0x20;  HWDelay(10);              /* 0xbf84d..0xbf88b */
 6. ctl1c50 = (PInS.version > 0x607) ? PInS[0x19c] : 0x606;           /* 0xbf890, 0xbfffa */
    wm      = (devid == 0x527 && rev <= 1) ? 1 : 0x00070601;          /* 0xbf89c */
 7. 0x1c08 = 2;  0x1c24 = 7;  0x1c50 = ctl1c50;  0x08fc = 0;          /* 0xbf8b9..0xbf8f6 */
 8. 0x1c58 = 0x40  (unconditional for 0x0527/0x0528)                  /* 0xbf8fb, 0xbf9a6 */
 9. 0x0920 &= 0x7fffffff;  0x10b0 = 0x40;                             /* 0xbf9b8..0xbf9eb */
10. HSLPARDisableDisplayOutputs; Chrontel/DVI dual-link setup; HSLPARInitCEM;
    four × { HSLPARDisableDisplayController; HSLPARSetPrimaryLUT };   /* 0xbfad1..0xbfee0 */
11. HWPARInitializeSystemClock(dev);            /* §4 */              /* 0xbfee8 */
12. HSLPARGetSystemCLock(dev, &dev[0x3984]);    /* read the clocks back */ /* 0xbfef5 */
13. HWPARInitializeMemoryController(dev);       /* §5 */              /* 0xbff00 */
14. if ((cfg 0xF4 & ~7) == 0) dev->pcix_state = (dev->pcix_state & 0x1f) | 0x80;  /* 0xbff05 */
15. 0x12b8 = wm;                                                      /* 0xbff33 */
16. if (!(devid == 0x527 && rev <= 1)) 0x12b0 = 0;                    /* 0xbff43 */
17. for (i = 1; i <= 0x13; i++) { v = ReadReg(i*16); WriteReg(i*16, v & 0x0fffffff); }  /* 0xbff6a */
18. 0x0000 = 0x10000000;  0x0004 = 0;  0x0008 = 0x10000020;  0x000c = 0x00010001;  /* 0xbff9f..0xbffdc */
```

**PROVEN**, every line and address.  Cross-checks against our driver:

* step 5 uses `0x00809020` where `parhelia_engine_init()` uses `0x00808020`,
  i.e. we never set bit 12.  Both vendor paths set it (the P690 copy is at
  `0xdd884`).  Already recorded in
  `parhelia-p690-engine-reset-re.md` §5.4 item 2; unchanged.
* step 7's `0x1c50` is never written by our driver.  This board's PInS version
  is `0x060a > 0x607`, so the value is `PInS[0x19c] = 0x08000707`, **not** the
  `0x606` fallback.  **PROVEN** (both the branch and the ROM content).
* step 15's `0x12b8 = 0x00070601` matches what the P690 path writes, so the
  constant is family-independent for anything past a `0x0527` rev 1.
* step 17 clears bits 31:28 of every host-aperture descriptor `0x010..0x130`
  before the baseline four dwords are installed.  On the un-POSTed board those
  descriptors hold garbage (`0x0010 = 0x07880000`, `0x0014 = 0x0ea6bb00`, …),
  so the masking pass is not cosmetic.

### 6.1 The ROM's equivalent, and where it differs

The ROM's `0x816d`/`0x8174` covers the same ground as steps 4–9 and 18, and
confirms several of them independently:

```
0x8177  rd8 0x0e03 ; and cl,0xbf ; wr8 0x0e03          /* clear 0x0e00 bit 30 (§2.1) */
0x8185  if (cfg 0xdd != 0) wr32 0x0308 = 0x00000083
0x8198  rd8  0x1c19 ; or  cl,0x94        ; wr8         /* = 0x1c00 bits 15:8  */
0x819b  rd32 0x1c1c ; or  ecx,0x00031400 ; wr32
0x819e  rd16 0x1c18 ; or  cx,(PInS[0x174] & 0x1ff) = 0x036       ; wr16
0x81a1  rd16 0x1c1c ; and cx,0xfe00 ; or (PInS[0x174] >> 9) & 0x1ff = 0x036 ; wr16
0x81a4  if (PInS version low byte >= 8) wr32 0x1c50 = PInS[0x19c] = 0x08000707
0x81bd  per-head 0x1902/0x1b02 and 0x1c00/0x1c04 clock-select poke
0x81c3  the clock/PLL init (§4.3b)
0x81cc  rd32 0x0920 ; and ecx,0x7fffffff ; wr32        /* == driver step 9 */
0x81e2  wr32 0x10b0 = 0x00000040                       /* == driver step 9 */
0x81eb  wr32 0x08fc = 0x00000000                       /* == driver step 7 */
0x81ee  if ([cs:0x7fed] & 0x30) { rd8 0x1c24 ; and cl,0xfc ; wr8 }   /* not taken here */
0x820a  wr32 0x1834 = 0x32000000 ;  0x821c  wr32 0x19d8 = 0x32000000
0x821f  if ([cs:0x7fed] & 1) the 0x12xx block at 0x829e (§2.3)
```

**PROVEN.**  `0x0920 &= 0x7fffffff`, `0x10b0 = 0x40`, `0x08fc = 0` and
`0x1c50 = PInS[0x19c]` therefore appear in both implementations, which settles
that the `0x1c50` write our driver omits is genuinely part of bring-up and not
an X-server peculiarity.  `0x1c18`/`0x1c1c` take two nine-bit fields out of
`PInS[0x174] = 0x01fc6c36` — both `0x036` on this board — which is a PInS
dependency neither this document nor
[`parhelia-registers.md`](parhelia-registers.md) had recorded.

One difference our driver should be aware of, in the descriptor baseline
(driver step 18 vs. ROM `0x8308`):

| register | driver `0xbff9f`.. | ROM `0x832a`.. |
|---|---|---|
| `0x0000` | `0x10000000` | `(cfg 0x10 /* BAR0 */ & 0x0e000000) \| 0x10000000` |
| `0x0004` | `0x00000000` | `0x08000000` |
| `0x0008` | `0x10000020` | `0x10000400` |
| `0x000c` | `0x00010001` | `0x00010001` |

The ROM folds the top BAR0 address bits into slot 0 where the driver writes a
flat constant; measured un-POSTed, `0x0000` already reads `0x10000000`.
**PROVEN** (both sets of writes); **INFERRED** that the driver can write the
flat value because it reprogrammes the descriptors itself later.  This matters
only if a cold bring-up is expected to leave the chip usable by the VBIOS.

---

## 7. Proposed implementation shape

**No driver code is written by this document by design** — the kernel side is
being implemented in parallel.  This is only the shape that matches the vendor
order, with the place in `parhelia_pci_probe()` for each piece.

New file `parhelia_post.c` (plus declarations in `parhelia_drv.h`), everything
family-dispatched on `chip->mc_family` / `chip->clock_family` so the P690 can be
added later from `parhelia-p690-engine-reset-re.md` §5:

| function | does | §|
|---|---|---|
| `parhelia_post_state(pdev)` | read-only classifier returning `POSTED` / `UNPOSTED` / `UNKNOWN` from `0x0e00`, the six FS registers and the `0x1200`/`0x1300` constant-read check | 2 |
| `parhelia_chip_reset_hslpar(pdev)` | the `0xc5580` sequence, with our own COMMAND value restored instead of the saved one | 3 |
| `parhelia_clock_hold(pdev, struct parhelia_hold *)` / `parhelia_clock_unhold()` | the `0x1c00`/`0x1c04`/`0x1800`/`0x1a00` save-hold-restore shared by §4 and §5 | 4.3, 5.2 |
| `parhelia_fs_program(pdev, reg, word)` | one synthesiser load strobe: `reg = word \| BIT(31)`, `udelay(201)`, `reg = word & ~BIT(31)` | 4.3, 4.4 |
| `parhelia_fs_locked(pdev, reg)` | the poll: ~200 µs settle, then 100 reads of `reg`, locked iff `(v & 0x03000000) == 0x03000000` in at least 90 of them (the primary path demands all 100) | 4.5 |
| `parhelia_fs_encode(A, B, C, D)` / `parhelia_fs_decode()` | the four negated-count fields of §4.4, and `pllGetDivider`'s inverse | 4.4 |
| `parhelia_fs_solve(pdev, reg, target_khz, const u16 lim[6])` | the search.  Implement **`mtx402`** (§4.4b) — `B` descending 32..2 outside `D` 2..64 step 2, `A`/`C` from the rounding pass, keep the *best* candidate — because it reaches 550000 kHz exactly where the ROM's first-fit does not.  Keep the ROM's simpler first-fit (§4.4a) as a fallback.  Program with a read-modify-write that preserves register bits 22, 23 and 29, then `parhelia_fs_locked()`; on failure take the next candidate | 4.4a, 4.4b |
| `parhelia_clock_init_hslpar(pdev)` | the `0xe5480` / `0x6d8c` sequence: hold, the `0x0e00` park triple, per selected synthesiser either the PInS divider word (verified by the lock poll) or `parhelia_fs_solve()`, 10 ms, the `0x0e00` install triple, release | 4.3, 4.3b |
| `parhelia_mc_select(pdev, mask)` / `parhelia_mc_write(pdev, reg, mask, val)` / `parhelia_mc_read()` | the `0x0f00` banked window | 5.1 |
| `parhelia_mc_wait_idle(pdev, mask)` | `0x0f08` bit 31, bounded | 5.1 |
| `parhelia_mc_program_bank(pdev, mask, const u32 rec[13])` | the 27-step `0xe3dc0` sequence, with the longer of the driver's and the ROM's delay at each point (5 ms, 21 ms, 21 ms, 21 ms) | 5.4, 5.5 |
| `parhelia_mc_phase_reset(pdev)` | `0xe3ff0` | 5.1 |
| `parhelia_mc_init_hslpar(pdev)` | bank selection from `PInS[0x8c]` and the device id, the four banks in the order 1,0,2,3, then `0x12b8 = 0x00070601` and `0x12b0 = 0` | 5.2, 5.5 |
| `parhelia_mc_healthy(pdev)` | the `0x0f24[1:0]` per-bank check of §2.4, used as the post-condition | 2.4 |

`parhelia_bios.c` needs two changes:

* **retain the PInS block.**  `parhelia_bios_init()` currently keeps only
  `ddc_pin_ids`, `pins_version` and `pins_offset` and `kfree()`s the ROM copy.
  Cold bring-up needs `PInS[0x24..0x73]` (the four PLL records),
  `PInS[0x84]`, `PInS[0x8c]`, `PInS[0xa4..0x173]` (the four memory records) and
  `PInS[0x19c]`.  Keep a `kmemdup()` of the PInS block itself, sized by its own
  length word, and add accessors; do not keep the whole ROM.
* **no length change is needed for HSLPAR.**  The existing
  `PARHELIA_PINS_DDC_IDS_END` requirement of `0x1b8` already covers every field
  above (the last is `PInS[0x19c]`), and this board's PInS length is `0x200`.
  The widening to `0x2c0` discussed in `parhelia-p690-engine-reset-re.md` §5.3
  is a P690-only need, and this board's PInS **does not extend that far** — see
  §8.

Where it goes in `parhelia_pci_probe()`:

1. before `pcim_enable_device()`: the existing PCI-COMMAND check (kernel
   `9229adc`) stays as it is — it is the only test available before MMIO is
   mapped;
2. after `pcim_iomap()` of the register BAR and after `parhelia_bios_init()`,
   and **before** `drmm_vram_helper_init()` — which is the first thing that can
   reach VRAM — insert:
   * `parhelia_post_state()`;
   * if `POSTED`: nothing changes, today's behaviour;
   * if `UNPOSTED` and `!cold_post`: fail the probe with the existing message;
   * if `UNPOSTED` and `cold_post=1`: `parhelia_chip_reset_hslpar()`,
     `parhelia_clock_hold()`, `parhelia_clock_init_hslpar()`,
     `parhelia_mc_init_hslpar()`, `parhelia_clock_unhold()`,
     `parhelia_mc_phase_reset()`, then `parhelia_mc_healthy()` and a
     `parhelia_post_state()` re-test.  Fail the probe if either is unhappy —
     never fall through to VRAM on a doubtful controller;
3. the `0x1c50 = PInS[0x19c]` and the `0x1c00 |= 0x00809020` corrections belong
   in `parhelia_engine_init()`, where the rest of §6 already lives, and are
   independent of cold bring-up;
4. keep the whole thing behind a module parameter (`cold_post=1`, defaulting
   off) until it has worked once, and gate VRAM access on it: the first read of
   BAR0 after a cold bring-up should be a small known-pattern write/read-back
   through the kernel aperture window, with the scanout still disabled.

---

## 8. What is still unknown

The clock and memory sequences are complete enough to implement.  What remains:

1. **Which candidate the search settles on.**  The two algorithms, the
   tolerance, the exclusions and the register encoding are all proven (§4.4),
   but every candidate is programmed and lock-tested, and whether a given
   `(A, B, C, D)` locks cannot be determined statically.  Two candidate pairs
   are on the table: `0x003c6379`/`0x00012aae` from the driver's primary
   `mtx402` path (§4.4b, and the one to prefer — it hits 550000 kHz exactly),
   and `0x003cabc8`/`0x0001bc09` from the ROM's first-fit (§4.4a).  A POSTed
   register dump (§9 item 1) settles it outright and is the single most
   valuable capture.
2. **`0x8081`** — the scratch value written to `0x0e00` around a synthesiser
   load.  Bits 0, 7 and 15; meaning undecoded, but confirmed identical in the
   ROM (`0x6e0a`) and the driver (`0xe5568`).
3. **The `0x0e20`/`0x0e2c` register class.**  Their fallback divider words
   `0x001a9401` and `0x00266cbb` are not in the §4.4 encoding, so those two
   synthesisers are programmed differently from `0x0e10..0x0e1c` and their
   field layout is unresolved.  Only matters for legacy VGA modes.
4. **`PInS[0x8c]` bit 3** is proven to gate banks 2 and 3; that it means
   "half-width memory" is inferred.  `PInS[0x8c] = 0x9d7f0007` on this board and
   the remaining bits are undecoded.
5. **`0x0f40` / `0x0f44`** are cleared during training and never written again;
   their function is unknown.  `0x0f5c`'s three values `0x138`/`0x130`/`0x100`
   are proven as a sequence but not decoded bit by bit.  Likewise the `0x9004`
   / `0x9005` low halves of the two intermediate `0x0f08` writes.
6. **`0x1c50 = PInS[0x19c] = 0x08000707`** is written by both vendor init paths
   and by the ROM (`0x8b3f`, gated on the PInS version byte `>= 8`), and by
   neither of ours; still undecoded (carried over from
   `parhelia-p690-engine-reset-re.md` §10).
7. **`N == 33` and `A == 2`** are excluded by the search (§4.4 test 3).  Assumed
   errata; unexplained.
8. Whether the `0x0f00` bank-select **write** required by §2.4 is safe on an
   un-POSTed chip.  Everything demonstrated so far is register-aperture
   *reads*.

### 8.1 Corrections this pass makes to existing documents

* **`parhelia-p690-engine-reset-re.md` §5.3 / §8 step 4** says to widen
  `parhelia_bios.c`'s PInS length requirement from `0x1b8` to `0x2c0` and dump
  `PInS[0x210..0x2bc]`.  **Do not do that.**  This board's PInS is length
  `0x200`, so `0x2c0` would make the parser *reject* a perfectly valid 0528
  PInS.  The 44-dword `HWS90` table is a P690 construct (its PInS is version
  `0x700`, length `0x400`); the bytes at `0x210..0x2bc` in a 0528 ROM are
  8086 real-mode instructions, not data — including, at ROM `0x7a40` onwards,
  the ROM's own `(dword >> 16) & 0xfff` memory-type match loop.  Any widening
  must be conditional on the PInS version.
* **`parhelia-p690-engine-reset-re.md` §3.4** calls MMIO `0x0e20`/`0x0e2c`
  "the system and memory PLLs".  They are the two **legacy VGA reference
  clocks** (25.175 and 28.311 MHz) — §4.1 gives four independent proofs.  The
  system and memory clocks come from `0x0e10..0x0e1c` routed by `0x0e00`.
* **`parhelia-engine-context-bringup.md`** (around line 1400) records
  "PInS at ROM offset 0x7800, version 0200, clocks 01610097 000c028b 00000328,
  first controller words 2000f400 5053ffff 2608c383 2403438a".  Three errors:
  `0x0200` is the **length**, the version is `0x060a`; the "first controller
  words" are the real-mode code bytes at ROM `0x7a10`, past the end of PInS,
  not a controller table; and the quoted "clocks" are not `PInS+0x200/4/8`
  either, which are also past the end.  The real memory-controller records are
  at `PInS+0xa4/0xd8/0x10c/0x140` (§5.3) and the real clock records at
  `PInS+0x24/0x38/0x4c/0x60` (§4.7).
* **`parhelia-vbios.md`** open item 2 ("byte 1 `X` of the PLL divider word") is
  **resolved**: the synthesiser is a **two-stage PLL** with four fields, not
  the two the document assumes.  `Fout = 27000 kHz × A × B / (C × D)`, where
  the document's `N` is the VCO1 multiplier `A` and its `P` is the stage-1
  divider `C`; the byte it could not place is the **second-stage multiplier
  `B`**, and there is a fourth field, the second-stage divider `D`, in the
  remaining byte (§4.4).  Its N/P-only model reproduced 108 and 162 MHz only
  because `B == C` makes the two models coincide.  Its clock-select claim ("the four PLLs hang off MISC bits
  3:2, called with sel = 1 and 2") should read: MISC select 0 → `0x0e20`,
  1 → `0x0e2c`, 2 → the programmable PLL chosen by `0x6f5f` (§4.1).  Its open
  item 5 ("contents of the `0x0f00` init block and the memory controller:
  needed to bring a card up from cold") is answered by §5.
* **`tools/vbios.py`** `scan_accesses()` uses a 17-byte lookahead between a
  `mov si, imm16` and the helper `call`, and therefore misses the `0x0e20` and
  `0x0e2c` accesses in `0x6e29`/`0x6ea1`, where the call is ~80 bytes later.
  Widening the window to about 80 bytes with a "no intervening `mov si`" guard
  recovers them.  Its `--pll` divider search should be replaced with the §4.4
  relation.
* **`parhelia_drv.c`**'s comment that "POST already established DRAM timings …
  Keep both families firmware-owned" is true only for a card the system
  firmware POSTed.  It is the correct default, but it is not a property of the
  silicon.

---

## 9. Hardware dumps that would close the gaps

All of these are register-aperture reads or ROM reads.  **None of them touches
BAR0.**

1. **A POSTed reference dump of the same board.**  Put the Parhelia in the
   primary VGA slot (or otherwise let the BIOS POST it), boot, and dump the
   whole 8 KiB register aperture in the same `OFFSET = VALUE` format as
   `parhelia-0528-regs-unposted.txt`, with no driver loaded.  This is the single
   most valuable capture: diffing it against the un-POSTed dump gives the exact
   POSTed value of every register in §4 and §5, turns the whole of §4.7 and
   §5.3 from "PInS says" into "the silicon holds", and settles open item 1 —
   `0x0e10` and `0x0e14` would then carry the **measured** synthesiser words
   for 250 MHz and 550 MHz, which the driver can program directly instead of
   re-running the search and hoping the same candidate locks.  Expected, if
   §4.4b is right: `0x0e10 = 0x003c6379` and `0x0e14 = 0x00012aae`, with bits
   24 and 25 additionally set (locked) and bits 22, 23, 29 whatever the
   silicon's defaults are; if the board was POSTed by its own ROM rather than
   by a driver, §4.4a's `0x003cabc8`/`0x0001bc09` instead.  Decode whatever
   comes back with `pllGetDivider`'s formula (§4.4) — that alone tells us which
   search the real POST path uses.  Also expected: `0x0e00 = 0x00034114`,
   `0x12c0 = 0xffffffff`, `0x12c4 = 0x7fffffff`, `0x1c50 = 0x08000707`.  Save
   as `captures/parhelia-0528-cold/parhelia-0528-regs-posted.txt`.
   Even a dump of just `0x0e00..0x0e2c` would close open item 1.
2. **The same POSTed dump restricted to the memory-controller window, per
   bank.**  For each bank mask in `1, 0x100, 0x10000, 0x1000000`: write `0x0f00`
   = mask, then read `0x0f04`, `0x0f08`, `0x0f0c`, `0x0f10`, `0x0f14`, `0x0f18`,
   `0x0f1c`, `0x0f20`, `0x0f24`, `0x0f40`, `0x0f44`, `0x0f48`, `0x0f4c`,
   `0x0f50`, `0x0f54`, `0x0f58`, `0x0f5c`; restore `0x0f00` to `0` afterwards.
   With `/root/parhelia-mmio` that is `0xf00=1` followed by `0xf04-0xf5c`.
   This is the direct proof of the §5.3 record→register mapping and of the
   `0x0f24[1:0]` health bits — and it is the only way to learn what the four
   banks really end up holding, since PInS words 3 and 7 differ per bank.
   Do it only on a **POSTed** card; on the un-POSTed one it is a write into the
   memory controller.
3. **PCI config space, both states.**  `lspci -xxx -s <bdf>` (or 64 dword
   reads of `0x00..0xFC`) on the un-POSTed card and on a POSTed one.  Needed
   for §3: the PM capability pointer (to confirm PMCSR really is at `0xE0`),
   `cfg 0x84`/`0x8c`/`0xF4`/`0xF8` for the `0x0300` recompute, and to see which
   vendor dwords POST actually sets.
4. **Nothing further from the ROM.**  The capture already in the tree is
   complete: PInS at `0x7800`, length `0x200`, version `0x060a`, and every field
   §4 and §5 need is present and quoted above.

Explicitly **not** wanted, and not to be attempted until §5 has completed and
§2.4 is clean: any read or write of BAR0, including a VRAM size probe.

---

## 10. Reproducing the divider search

A direct transcription of the **ROM's** `SetPixelClock` (§4.4a) and the register
encoder, for checking the computed words and for generating any other
frequency.  Not driver code — a reference implementation of the recovered
algorithm.  The driver's primary `mtx402` search (§4.4b) is a different shape
and produces the better pair; this is the simpler one and is what §4.4a's table
was generated with.  Field letters follow §4.4 (`A` VCO1 multiplier, `B` VCO2
multiplier, `C` stage-1 divider, `D` stage-2 divider), and note the ROM packs
them in the reverse byte order from the driver.

```python
REF = 27000                                     # kHz, ROM constant 0x6978

def fs_encode(A, B, C, D):                       # ROM 0x7346, driver 0xe4580
    af = (-A) & 0xff
    af = (af & ~0x20) if af >= 0xe0 else (af | 0x20)   # bit 5 = "A > 32"
    af &= 0x3f
    bf = (-B) & 0x1f
    cf = (-C) & 0x1f
    df = ((((-(D >> 1)) & 0xff) << 1) & 0x3e) if D else 1   # D == 0 -> bypass, /2
    return af | (bf << 6) | (cf << 11) | (df << 16)

def fs_decode(v):                                # pllGetDivider 0xe8c40
    A = (64 if v & 0x20 else 32) - (v & 0x1f)
    B = 32 - ((v >> 6) & 0x1f)
    C = 32 - ((v >> 11) & 0x1f)
    D = 2 if (v >> 16) & 1 else 64 - ((v >> 16) & 0x3e)
    return A, B, C, D, REF * A * B // C // D

def fs_search(target_khz, lim_mhz):              # ROM 0x7018
    L = [m * 1000 for m in lim_mhz]              # PInS record +0x08..+0x13
    a0, a1 = L[0] // REF, L[1] // REF
    c0, c1 = max(3, a0 * REF // L[5]), a1 * REF // L[4]
    b0, b1 = max(3, L[3] * c0 // (a1 * REF)), L[2] * c1 // (a0 * REF)
    for A in range(a0, a1 + 1):                  # ROM order: A, B, C, D
        for B in range(b0, b1 + 1):
            for C in range(c0, c1 + 1):
                for D in range(0, 0x42, 2):
                    f = REF * A * B // C // (D or 2)
                    if abs(target_khz - f) * 10000 // target_khz >= 0x33:
                        continue                 # 0.51 % tolerance
                    if A == 33 or D == 2:
                        continue                 # excluded
                    if not L[0] <= REF * A <= L[1]:
                        continue                 # VCO1
                    if not L[4] <= REF * A // C <= L[5]:
                        continue                 # stage-1 output
                    if not L[2] <= REF * A * B // C <= L[3]:
                        continue                 # VCO2
                    yield A, B, C, D, f, fs_encode(A, B, C, D)

# ROM first-fit for this board: FS0 -> 0x003cabc8, FS1 -> 0x0001bc09,
# and 108 MHz -> 0x003aef48 exactly.  The driver's mtx402 finds
# FS0 -> 0x003c6379 and FS1 -> 0x00012aae (exactly 550000 kHz).
next(fs_search(250000, [600, 1200, 600, 1200, 50, 400]))
next(fs_search(550000, [600, 1600, 600, 1600, 50, 400]))
fs_decode(0x00012aae)                            # -> (50, 22, 27, 2, 550000)
```

On hardware each yielded candidate must be programmed and lock-tested (§4.5)
before it is accepted; the first one that locks is the answer.

## 11. Cross-references

* [`parhelia-p690-engine-reset-re.md`](parhelia-p690-engine-reset-re.md) — the
  same problem for the P690 (`HWS90*`, `0x0ffc` bank select, the 44-entry
  `pinsDatasConvertor` table), the reset dispatch table, and the WARP gate.
* [`parhelia-vbios.md`](parhelia-vbios.md) — the ROM-side view: the PCI-config
  register window, the 27 MHz reference, the pixel-clock divider search, and
  the observation that the `0x0f00` block is written by the ROM's power-on init
  at `0x837f`.  Its open items 2 and 5 are closed here; see §8.1 for the
  corrections.
* [`parhelia-registers.md`](parhelia-registers.md) — the consolidated register
  reference.
* [`parhelia-engine-and-context-init-complete.md`](parhelia-engine-and-context-init-complete.md)
  — everything that happens after §6, i.e. the ring, descriptor bank, WARP
  upload and per-context defaults.


## Correction (2026-09-13)

Section 6.1 annotates `0x8198 rd8 0x1c19 ; or cl,0x94 ; wr8` as `0x1c00` bits
15:8.  `si = 0x1c19` is byte 1 of **`0x1c18`** (DAC1), not `0x1c00`.  That
misreading is why a driver init set `0x1c00` bits 15/23, which the board's
VBIOS never sets (POST reads `0x1c00 = 0x...60c2`).
