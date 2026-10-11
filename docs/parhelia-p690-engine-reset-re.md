Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 engine / WARP reset — what the vendor drivers actually do

Static reverse engineering only.  No hardware was touched and no driver source
was changed for this document.

**Short answer.**  The Matrox drivers have **no engine-only or WARP-only reset**.
The single recovery path in all four binaries is a chip-level reset through PCI
config register `0xE0` (a `3 → 0` strobe, which on this family is the PMCSR
D3hot→D0 transition), and on the P690 that reset is **always followed by a full
clock and memory-controller re-initialization driven from the VBIOS PInS block**.
That second half is exactly what our `reset_on_probe=1` experiment was missing:
the vendor does not avoid losing the DRAM timings, it *reprograms* them, and the
44 register values it uses are in the VBIOS.  The complete table is decoded in
§5 below.

There is one cheaper, memory-safe gesture worth trying first: the Windows
miniport strobes `0x0920 = 0x80000000` then `0x0920 = 0` around out-of-band WARP
use (§4).  It is the only WARP-local reset gesture that exists in any of the
binaries.

---

## 1. Evidence identity

| file | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` | `52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a` |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserm.sys` | `da47324eae893160d0c1813984e1875a08a69fc72c47f123a666699e2d46b4b7` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |
| `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | `8f5ca1d85f1c13fda731c8a0ba28db063ec0e697b7bc5dfff300c1500d564e4f` |
| `matroxdriver-x86_64-cvs-20090511/kernel/src/reset.c` | `eb2e272e9e80d2902d583e71c35012208efcd6992c82e3ac285ee04402462a6b` |

All under `/srv/t2/xf86-video-parhelia/`.  Tags used below: **PROVEN** = read
directly out of at least one binary (usually two independently); **INFERRED** =
a reading of the code that is consistent but not forced by it.

`mtx_drv.so` (the proprietary X driver, v1.4.7) is the key new source for this
pass and was not used by the earlier reset notes: it is the **only binary with
symbol names** (2418 dynamic symbols, 649 of them `HSL*`/`HW*`), so every
function below can be named rather than guessed.  The Windows miniport
`mtxpserm.sys` carries the same code compiled for i386 and is used throughout as
an independent cross-check.  Note that the earlier
`parhelia-engine-and-context-init-complete.md` §1.2 cited a *different* miniport
image (`MTXPARM.sys`); its addresses do not apply to `mtxpserm.sys` and the
addresses here are all from the files in the table above.

---

## 2. Is there an engine-only reset?  No — and here is the proof

* `0x08f0` is **never written** by any of the five binaries.  A scan of every
  `memWriteDword` / `ClientWriteRegisterDword` / miniport `WriteRegDword` call
  site finds reads of `0x08f0` only.  Nothing clears bits 24/25 (the WARP-busy
  pair) by a register write.  **PROVEN.**
* Every vendor wait on the engine is an **unbounded** spin with no timeout and
  no recovery branch:
  * `mtx_parhelia.o` `mtx1391` @`0x9f6b0` (the WARP instruction loader — the
    function our `warp_gate_ready()` was derived from): `0x9f785` is
    `jne 0x9f785`, a literal infinite loop on a software in-flight counter,
    then unbounded polls of `0x08f0`, `0x0450`, `0x0908`, `0x0924`, `0x08f0`.
  * `mtx_drv.so` `CDPSHwManager::WaitBusMastering` @`0x6c750`: same shape,
    `0x6c770: jne 0x6c770`, then `0x08f0` bit 17, then `0x0450` equality.
  * `HSLPARWaitEngineNotBusy` @`0xba850` is a **two-instruction stub** that
    returns the cached status word — it does not touch hardware at all.
  **PROVEN.**
* `mtx_drv.so` has no symbol resembling an engine or WARP reset.  The 79
  `*Reset*` symbols are: I2C (`HALPResetI2C`, `HSLMSPI2CSoftReset`), the
  Chrontel TV encoder (`HSLCHRReset`, `HSLPARResetAllChrontelQuad`), the video
  path (`HSLPARApplyVidReset`, `HSLPARGenerateSoftVidReset`,
  `HSLPARSoftVidResetCRTCA/B`, `HSLPARTveApplyVidReset`), quad-head FPGA boards
  (`HSLPARApplyFPGAReset`), the memory controller (`HWS90ResetMemoryController`,
  `HWResetMemoryPhaseError`), software bookkeeping (`CDPS*::Reset`,
  `MemoryBank::ResetHeap`, `BmPrimInt::Reset`, `HSLPARResetBandwidthState`), and
  the two chip resets of §3.  **PROVEN.**
* The Linux vendor package's `kernel/src/reset.c` is a *user-space* tool whose
  `MTX_IOCTL_HARD_RESET` (`mtx_ioctl.c:329`) only decrements the module use
  count and calls `mtx_dev_cleanup()` under `#ifdef DEBUG`.  It touches no
  hardware.  It is a red herring for anyone grepping for "reset" in that tree.
  **PROVEN.**

So the vendor's answer to "the WARP store is busy" is *wait forever*.  That is
viable for them because their stack never submits an invalid program; it is not
viable for us.

---

## 3. The vendor reset sequence (chip level, PCI config `0xE0`)

### 3.1 Which variant the P690 uses

The reset entry point is a device jump-table slot, installed by device ID:

`mtx_drv.so` `0xbeacc`–`0xbecae` (inside the device jump-table builder), slot
`dev+0x4f20`:

| device ID | init function (`dev+0x4e80`) | clocks | reset function (`dev+0x4f20`) |
|---|---|---|---|
| `0x0527` Parhelia-512 | `HSLPARInitializeHardware` @`0xbf780` | `HSLPAR*` | `HSLPARHardwareReset` @`0xc5580` |
| `0x0528` Parhelia-8x | `HSLPARInitializeHardware` | `HSLPAR*` | `HSLPARHardwareReset` |
| `0x2537` LX / P650 | `HSLPARInitializeHardware` | `HSLPAR*` | `HSLPARHardwareReset` |
| `0x2538` SUNEX | `HSLPARInitializeHardware` | `HSLPAR*` | **`HSLSNXHardwareReset`** @`0xc53e0` |
| **`0x2539` P690** | **`HSLS90InitializeHardware`** @`0xdd7c0` | **`HSLS90*`** | **`HSLSNXHardwareReset`** @`0xc53e0` |

Cross-check, `mtxpserm.sys` `0x356f9`–`0x35785`: `0x2539`/`0x2538` →
`0x323c6` (the SNX reset), everything else → `0x341f3` (the PAR reset);
`0x2539` → `0x4d285` (S90 init), else `0x4d?` / `0x32dbf`.  **PROVEN, two
binaries.**

So the P690 is *not* handled like the rest of the "LX family": `0x2537` still
uses the Parhelia code path.  The split that matters for reset is
`{0x2538, 0x2539}` vs. the rest, and the split that matters for clocks and
memory is `{0x2539}` alone.  See §6.

### 3.2 When the vendor fires it

`HSLS90InitializeHardware` @`0xdd7c0` (P690), first thing it does:

```
0xdd81d   v = ReadReg(0x0e24)
0xdd833   if (v == 0 || v == 0x80000000)  ->  0xde091: call dev[0x4f20]()   (the reset)
0xdd84a   ... continue with the full hardware init ...
```

`HSLPARInitializeHardware` @`0xbf780` (all other chips) uses a different
register and constant:

```
0xbf7e1   v = ReadReg(0x0e00)
0xbf7f3   if (v == 0 || v == 0x40000000)  ->  0xc0042: call dev[0x4f20]()
```

Cross-check `mtxpserm.sys` `0x4d2bc`–`0x4d2de`: reads `0x0e24`, compares against
`0` and `0x80000000`, calls `*0x4e44(%ebx)`.  **PROVEN, two binaries.**

`InitializeHardware` itself is reached from `HALInitHardware` @`0x97d20`
(driver start), from `HSLPARClearDisplay` @`0xcf220` (call at `0xcf4c0`) and —
importantly — from **`HSLPARSetMode` @`0xd11e0` (call at `0xd12e3`)**.  So the
vendor re-tests chip liveness and can re-reset on *every mode set*, at runtime,
with clients alive.  **PROVEN.**

The liveness test is a clock-domain test (`0x0e24` is in the PLL block
`0x0e10..0x0e2c`), i.e. it catches "this chip was never POSTed / lost its
clocks".  It does **not** look at `0x08f0` and would not fire on our hung-WARP
case.  The mechanism exists; the trigger does not.  **INFERRED** (that the
constants mean "clock dead") / **PROVEN** (the code).

### 3.3 `HSLSNXHardwareReset` — the P690 reset, decoded

`mtx_drv.so` `0xc53e0` (`mtx1900`), cross-checked against `mtxpserm.sys`
`0x323c6`.  Ready to be transcribed into C:

```
 1.  for (i = 0; i < 64; i++)                        /* cfg 0x00 .. 0xFC */
         cfg_save[i] = pci_read_config_dword(i * 4);

 2.  old300 = mmio_read(0x0300);
     c84 = cfg_save[0x84/4];   c8c = cfg_save[0x8C/4];
     new300 =  (old300 & 0xf888fff8u)
             | ( c84        & 0x7u)              /* bits  0..2  */
             | (((c84 >>  6) & 0x7u) << 16)      /* bits 16..18 */
             | (((c84 >>  9) & 0x7u) << 20)      /* bits 20..22 */
             | (((c8c >> 12) & 0x7u) << 24)      /* bits 24..26 */
             | 0x600u;                           /* bits 9,10   */
     /* device-conditional: */
     if (!(devid == 0x2538 && subvariant == 0)) new300 |=  0x100u;   /* bit 8  */
     if (  devid == 0x2538 && subvariant == 2 ) new300 &= ~0x400u;   /* bit 10 */
     /* on 0x2539 both conditions leave bits 8,9,10 set -> |0x700 */

 3.  mmio_write(0x0300, new300);

 4.  pci_write_config_dword(0xE0, 3);            /* D3hot */
 5.  udelay(10000);
 6.  pci_write_config_dword(0xE0, 0);            /* D0    */
 7.  udelay(10000);

 8.  for (i = 0; i < 64; i++) {                  /* restore, skipping: */
         off = i * 4;
         if (off == 0x04 || off == 0x44 || off == 0x48) continue;
         pci_write_config_dword(off, cfg_save[i]);
     }
 9.  pci_write_config_dword(0x04, cfg_save[1]);  /* COMMAND last */

10.  mmio_write(0x0300, new300);
```

Steps 4–7 in `mtx_drv.so` are `ClientWriteConfigSpaceDword(0xE0, 3)`,
`ClientWait(0x2710)`, `ClientWriteConfigSpaceDword(0xE0, 0)`, `ClientWait(0x2710)`
(`0xc54cf`–`0xc5509`); in `mtxpserm.sys` `0x3249d`–`0x324c2`.  `0x2710` = 10000;
the wait unit is microseconds in every other use of `ClientWait` in this driver
(**INFERRED**), giving 10 ms per half.

The skip list `{0x04, 0x44, 0x48}` in the miniport (`0x324cd`–`0x324da`) is
deliberate: `0x44`/`0x48` are **MGAINDEX/MGADATA**, the indirect MMIO window —
writing them would perform MMIO accesses mid-restore.  `0x04` (COMMAND) is
written last so that memory/IO/bus-master decoding is only re-enabled after the
BARs are back.  **PROVEN.**  (The `mtx_drv.so` copy restores the same 64 dwords
without an explicit skip list; the miniport is the safer template.)

`0x0E0` is the PMCSR on this family: our own driver already recorded
(`parhelia_drv.c:292`) that on the measured `0x0528` PMCSR sits at cfg `0xE0`,
so `3 → 0` is a **PCI PM D3hot→D0 soft reset**, not a Matrox-private register.
That means `pci_reset_function*()` performs the *same electrical operation*
(Linux `pci_pm_reset()` when no FLR is available).  The difference is entirely
in what is saved and restored around it, and in what is re-initialized
afterwards.  **PROVEN** (our measurement) + **INFERRED** (that `0x2539` has
PMCSR at the same offset — the vendor hardcodes `0xE0` for every device ID in
this family, which is consistent with it).

### 3.4 `HSLPARHardwareReset` — the other variant (`0x0527/0x0528/0x2537`)

`mtx_drv.so` `0xc5580` (`mtx2096`).  Same strobe, narrower save set:

```
HSLPARSaveCriticalData(dev);                       /* 0xba960 */
pcix = ((dev[0x3a8] & 0xe0) != 0x80);
save cfg 0x04, 0x0c, 0x10, 0x14, 0x30, 0x3c, 0x40  [ and 0xf4, 0xf8 if pcix ]
if (devid == 0x0528 || (devid - 0x2537) <= 2)  mmio_write(0x0308, 3);
old300 = mmio_read(0x0300);  new300 = <same shape, from the saved dwords>
mmio_write(0x0300, new300& ~0x100);                /* bit 8 cleared across the strobe */
pci_write_config_dword(0xE0, 3);
pci_write_config_dword(0xE0, 0);
udelay(10000);
restore cfg 0x04, 0x0c, 0x10, 0x14, 0x30, 0x3c, 0x40 [ , 0xf8 ]
mmio_write(0x0300, new300);                        /* bit 8 restored */
HSLPARRestoreCriticalData(dev);                    /* 0xbd4d0 */
```

`HSLPARSaveCriticalData` @`0xba960` / `HSLPARRestoreCriticalData` @`0xbd4d0`:

* on `0x2538`/`0x2539` only: save/restore MMIO **`0x0360`** and **`0x0364`**;
* on every chip: save MMIO **`0x0e20`** and **`0x0e2c`** (the system and memory
  PLLs), and restore each by writing `v | 0x80000000` (the LOAD strobe), waiting
  100, then writing `v` — the same PLL programming protocol
  `parhelia-registers.md` documents for the pixel PLLs.
* A saved value of `0` or `0x80000000` is treated as "invalid, do not restore".

**PROVEN.**  Note the P690 never reaches this code (§3.1): it gets the 64-dword
config restore instead, and its clocks are rebuilt from PInS by the init that
follows (§5).

### 3.5 `0x0308 = 3`

Only the `HSLPARHardwareReset` variant writes it, and only for
`0x0528`/`0x2537`/`0x2538`/`0x2539`, as *part of* the reset (`0xc57f3`).  It is
not a reset on its own: the arm path writes `0x0308 = 0x83` immediately
afterwards.  This confirms the verdict already recorded in
`parhelia-engine-and-context-init-complete.md` §5 for `tools/parhelia-3dtri.c`.
**PROVEN.**

---

## 4. What clears a busy WARP store, and what the gate waits for

### 4.1 The gate (this is what `warp_gate_ready()` reimplements)

`mtx_parhelia.o` `mtx1391` @`0x9f6b0`, the WARP instruction-store loader.  In
order (`0x9f776`–`0x9f974`):

| step | register | condition | address |
|---|---|---|---|
| 1 | software in-flight counter | `== 0` (infinite spin) | `0x9f781` |
| 2 | `0x08f0` | bit 17 (`0x00020000`) set — primary DMA idle | `0x9f7ae`, `0x9f7e6` |
| 3 | `0x0450` | `== ` the driver's expected soft-trap tag | `0x9f816`, `0x9f82e` |
| 4 | `0x0908` | `(v & 0xfff7ffbf) == 0x63a78001` | `0x9f866`, `0x9f874` |
| 5 | `0x0908` **and** `0x0924` re-checked together | `(0x0924 & 0xfff) == 0x100` **and** step 4 | `0x9f8af`, `0x9f8e2`, `0x9f8f0`, `0x9f902` |
| 6 | `0x08f0` | `(v & 0x03000000) == 0` — both WARP-busy bits clear | `0x9f933`, `0x9f96c` |

Only then: `mmio_write(0x0918, byteCursor / 12)` (`0x9f9d5`; the `/12` is the
`0xaaaaaaab` multiply at `0x9f97d`), followed by the instruction dwords through
`0x091c`.  **PROVEN.**

Our kernel gate tests the same three registers with the same masks, so "WARP
store busy" is the vendor's step 2/5/6 never being satisfied.

### 4.2 The one WARP-local reset gesture in the binaries

`mtxpserm.sys` `0x57288` — the miniport's WARP self-test (it re-purposes the
WARP to run a canned program and read the result back through the port).  It
brackets the whole operation:

```
0x572a7  for (i = 0; i < 0x140; i += 0x10)              /* descriptor bank 0x0000..0x013c */
             save[i] = ReadReg(i),  WriteReg(i, save[i] & 0xe0000000);
0x572cf  save 0x0004, 0x0008, 0x000c, 0x0010
0x572ff  WriteReg(0x0000, (saved0x10 & 0x0e000000) | 0x10000000);
0x57315  WriteReg(0x0004, 0x08000000);
0x57325  WriteReg(0x0008, 0x10000400);
0x57332  WriteReg(0x000c, 0x00010001);
0x5733f  saved920 = ReadReg(0x0920);
0x5734f  WriteReg(0x0920, 0x80000000);     <-- the strobe
0x5735b  WriteReg(0x0920, 0x00000000);
0x5736a  ... build the packets, stream the image to 0x0918/0x091c, draw, read back ...
0x5737a  WriteReg(0x0920, saved920);
0x57384  restore 0x000c, 0x0008, 0x0004
0x573ad  restore the whole descriptor bank
```

**PROVEN** (the writes).  **INFERRED**: that `0x0920` bit 31 written high then
low resets the WARP unit.  It is the only candidate in any binary, it is issued
*before* the idle gate rather than after, and bit 0 of the same register selects
the WARP's output destination (setup unit vs. readback port, see
`parhelia-engine-and-context-init-complete.md` §2.4), so the register is clearly
WARP-unit control.  Whether the strobe clears a latched `0x08f0` bits 24/25 is
**untested** — and it is the cheapest experiment we have (§8).

Note also that our own `parhelia_engine_init()` already writes
`0x0920 &= 0x7fffffff` (from the vendor init, `mtx_drv.so` `0xdd939`–`0xdd95d`),
i.e. it clears bit 31 without ever having set it.  Setting it first is a
one-line change.

### 4.3 What definitively does clear it

The chip reset of §3.  `parhelia-engine-and-context-init-complete.md` §3.3
reached the same conclusion from three binaries; this pass adds the fourth
(`mtx_drv.so`) and the negative scan of §2, and finds nothing new.  **PROVEN
(negative result).**

---

## 5. After the reset: what must be re-initialized

### 5.1 The P690 re-initializes the clocks and the memory controller — from the VBIOS

This is the finding that changes what we should do.

`HSLS90InitializeHardware` @`0xdd7c0` calls, on its straight-line path (no
branch jumps over it; verified by scanning every jump target in
`0xdd7c0..0xde0ff`), **`HSLS90InitMemClk` @`0xdcef0`** at `0xdde7a`.  The
Windows miniport does the same (`0x4d285` → `0x4d17e` → `0x53b5b`/`0x53b72`).
**PROVEN, two binaries.**

`HSLS90InitMemClk` @`0xdcef0`:

```
save 0x1c00, 0x1c04, 0x1800, 0x1a00
WriteReg(0x1c00, save1c00 | 0x20);      /* hold / power down clock controller A */
WriteReg(0x1c04, save1c04 | 0x20);      /*                              ... B   */
WriteReg(0x1800, save1800 & ~1);        /* disable display controller A */
WriteReg(0x1a00, save1a00 & ~1);        /* disable display controller B */
udelay(1000);
HWS90ResetMemoryController(dev);        /* 0xe5890 */
HWS90InitializeSystemClock(dev);        /* 0xe60d0 */
HWS90InitializeMemoryController(dev);   /* 0xe59d0 */
HSLS90GetSystemCLock(dev);
restore 0x1c00, 0x1c04, 0x1800, 0x1a00
```

### 5.2 The banked memory-controller window

The P690 memory controller is MMIO, addressed through a bank-select register:

* `SelectS90MemoryController` @`0xe57d0`: `WriteReg(0x0ffc, mask & 3)` — bank 1,
  bank 2, or both (`3`).
* `WriteS90MemoryControllerDword(dev, reg, mask, val)` @`0xe5800` and
  `ReadS90MemoryControllerDword` @`0xe58b0`: save `0x0ffc`, select, access `reg`,
  re-read `0x0ffc` and rewrite it if it changed.
* `WaitForS90MemoryIdle` @`0xe5950`: save `0x0ffc`, then up to 100000 times read
  bank-1 register `0x0ff8` and **loop while `(v & 0x0600) == 0x0600`** — i.e.
  wait for bits 9 and 10 to clear; restore `0x0ffc`.
* `HWS90ResetMemoryController` @`0xe5890`: `WriteS90MC(0x0ff4, bank 3, 0)`.

(The Parhelia-family equivalent, `SelectMemoryController` @`0xe3af0`, uses
register `0x0f00` with one byte per bank — on the P690 `0x0f00` is a *data*
register instead.  Do not mix the two.)  **PROVEN.**

### 5.3 `HWS90InitializeMemoryController` @`0xe59d0` — the PInS-driven table

```
pins = HWPARGetPINS(dev);                    /* the VBIOS PInS block */
for (i = 0; i < 44; i++)
    WriteS90MC(tbl[i].reg, tbl[i].bank, *(u32 *)(pins + tbl[i].pins_off));
WriteS90MC(0x0ff4, bank 3, ctrl | 1);        /* start */
udelay(5);
WaitForS90MemoryIdle(dev);
v = ReadS90MC(0x0f00, bank 1);
WriteS90MC(0x0f00, bank 3, v | 0x40000000);  /* set bit 30 */
WaitForS90MemoryIdle(dev);
HWS90ResetMemoryController(dev);             /* 0x0ff4 bank 3 = 0 */
WriteS90MC(0x0f48, bank 1, pins[0x268]);   WriteS90MC(0x0f4c, bank 1, pins[0x26c]);
WriteS90MC(0x0f58, bank 1, pins[0x270]);   WriteS90MC(0x0f5c, bank 1, pins[0x274]);
WriteS90MC(0x0f48, bank 2, pins[0x298]);   WriteS90MC(0x0f4c, bank 2, pins[0x29c]);
WriteS90MC(0x0f58, bank 2, pins[0x2a0]);   WriteS90MC(0x0f5c, bank 2, pins[0x2a4]);
WriteS90MC(0x0ff4, bank 3, ctrl | 1);
udelay(5);
WaitForS90MemoryIdle(dev);
```

`ctrl` is a value the compiler keeps in a register that the disassembly does not
show being initialized in this function; `HWS90ResetMemoryController` writes `0`
to the same register, so `ctrl | 1 == 1` is the likely value.  **INFERRED**
(the value), **PROVEN** (the sequence).

The 44-entry table lives at `mtx_drv.so` symbol **`pinsDatasConvertor`**
(`0x34b580`, `.data`, file offset `0x14b580`) and at `mtxpserm.sys` `.data`
`0x72c28` (file offset `0x62c28`).  The two are **byte-identical**, which is as
strong a cross-binary confirmation as this project gets:

| # | PInS offset | MC register | bank | | # | PInS offset | MC register | bank |
|---|---|---|---|---|---|---|---|---|
| 0 | `0x210` | `0x0f00` | 3 | | 22 | `0x268` | `0x0f48` | 1 |
| 1 | `0x214` | `0x0f04` | 3 | | 23 | `0x26c` | `0x0f4c` | 1 |
| 2 | `0x218` | `0x0f08` | 3 | | 24 | `0x270` | `0x0f58` | 1 |
| 3 | `0x21c` | `0x0f10` | 3 | | 25 | `0x274` | `0x0f5c` | 1 |
| 4 | `0x220` | `0x0f1c` | 3 | | 26 | `0x278` | `0x0f68` | 1 |
| 5 | `0x224` | `0x0f20` | 3 | | 27 | `0x27c` | `0x0f6c` | 1 |
| 6 | `0x228` | `0x0f24` | 3 | | 28 | `0x280` | `0x0f70` | 1 |
| 7 | `0x22c` | `0x0f28` | 3 | | 29 | `0x284` | `0x0f74` | 1 |
| 8 | `0x230` | `0x0f2c` | 3 | | 30 | `0x288` | `0x0f8c` | 1 |
| 9 | `0x234` | `0x0f44` | 3 | | 31 | `0x28c` | `0x0f90` | 1 |
| 10 | `0x238` | `0x0f94` | 3 | | 32 | `0x290` | `0x0f14` | 2 |
| 11 | `0x23c` | `0x0f98` | 3 | | 33 | `0x294` | `0x0f18` | 2 |
| 12 | `0x240` | `0x0f9c` | 3 | | 34 | `0x298` | `0x0f48` | 2 |
| 13 | `0x244` | `0x0fa0` | 3 | | 35 | `0x29c` | `0x0f4c` | 2 |
| 14 | `0x248` | `0x0fc0` | 3 | | 36 | `0x2a0` | `0x0f58` | 2 |
| 15 | `0x24c` | `0x0fc4` | 3 | | 37 | `0x2a4` | `0x0f5c` | 2 |
| 16 | `0x250` | `0x0fc8` | 3 | | 38 | `0x2a8` | `0x0f68` | 2 |
| 17 | `0x254` | `0x0fcc` | 3 | | 39 | `0x2ac` | `0x0f6c` | 2 |
| 18 | `0x258` | `0x0ff0` | 3 | | 40 | `0x2b0` | `0x0f70` | 2 |
| 19 | `0x25c` | `0x0ff8` | 3 | | 41 | `0x2b4` | `0x0f74` | 2 |
| 20 | `0x260` | `0x0f14` | 1 | | 42 | `0x2b8` | `0x0f8c` | 2 |
| 21 | `0x264` | `0x0f18` | 1 | | 43 | `0x2bc` | `0x0f90` | 2 |

All values are 32-bit little-endian dwords read from `PInS + offset`.  Our
`parhelia_bios.c` already locates the PInS block (`bios.pins_offset`); it needs
the length check widened from `0x1b8` to `0x2c0` to expose these.  **PROVEN.**

### 5.4 Engine-side re-initialization after a reset

Everything below `HSLS90InitializeHardware` runs again after the strobe
(`0xde099` jumps back to `0xdd84a`, the instruction after the liveness test), so
nothing on the engine side survives.  In vendor order:

1. `HSLPARApplyVidReset(dev, all heads)` — the `0x1c00` bit-24 pulse we already
   implement in `parhelia_apply_vid_reset()`.
2. `0x1c00 |= 0x00809020`, `0x1c04 |= 0x20`, delay 10 (`0xdd884`–`0xdd8c2`;
   identical on the Parhelia path at `0xbf84d`–`0xbf88b`).
   **Our `parhelia_engine_init()` uses `0x00808020` and so never sets bit 12
   (`0x1000`).**  Both vendor paths set it.  Worth correcting or at least
   explaining. **PROVEN.**
3. `0x1c08 = 2`, `0x1c24 = 7`, **`0x1c50 = PInS[0x19c]`** (`0xdd8f1`), `0x08fc = 0`.
   *Our driver never writes `0x1c50`.*  On the `0x0527`/`0x0528` path the same
   write uses `0x606` as the default when the PInS version is `<= 0x607`
   (`0xbf81f`).  **PROVEN**; its meaning is not decoded.
4. `0x1c58 = 0x40` or `0x1c58 = 0`, selected by the board-wiring byte
   `dev[0x4a9c]` (= `PInS[0x1a1]`) and the PInS output bytes at `PInS+0x90..0x97`
   (`0xdd927`, `0xddfdc`–`0xde000`; the Parhelia copy at `0xbf9a6`/`0xc0069`
   makes the two values explicit).  `0x0527`/`0x0528` take the `0x40` branch
   unconditionally.
5. `0x0920 &= 0x7fffffff`, `0x10b0 = 0x40`.
6. `HSLPARDisableDisplayOutputs`, per-head `HSLPARDisableDisplayController` and
   `HSLPARSetPrimaryLUT` (four heads), `HSLS90InitMemClk` (§5.1),
   `0x1c0c |= 0x0808`, `0x1c10 |= 0x0808`.
7. Then the ring arm, descriptor bank, WARP program upload, block/constant file
   and per-context defaults, exactly as
   `parhelia-engine-and-context-init-complete.md` §4 phases 1–3 describe.  In
   particular the WARP instruction store is empty: **every** canned program and
   every client program must be re-uploaded, and every context's `0x1100`/
   `0x1104`/`0x10f8`/`0x10c0..0x10d8` entry registers rewritten, because the
   entry values are instruction indices into a store that no longer holds them.
8. `0x0bc0` (TEDLFCounters), `0x12b8 = 0x00070601`, `0x12b0 = 0` (watermark /
   MemLevels), the host-aperture descriptor baseline `0x0000..0x013c`, `0x0300`,
   `0x0308 = 0x83`, the writeback page address, `0x0404`/`0x0400`/`0x0408`,
   `0x08fc = 0x00010000`.

**What survives:** the PCI configuration space (because we restore it), and
nothing else.  DWGCTL/PIPECTL, the block-state file, the constant file, the
fence/writeback register, the host-aperture descriptors, the watermark and the
WARP store are all lost.  For our driver that means a reset must be followed by
`parhelia_engine_init()` in full plus
`parhelia_state_shadow_invalidate()` plus a re-upload of the resident WARP
programs plus invalidation of every client's WARP slot — i.e. all in-flight
contexts must be failed, which is what a DRM device wedge/recovery cycle does
anyway.

---

## 6. Chip-class caveats

* The reset **variant** is chosen by the **raw PCI device ID** at `dev+0x29e`,
  not by a capability mask: `{0x2538, 0x2539}` → `HSLSNXHardwareReset`,
  everything else → `HSLPARHardwareReset` (`0xbeae1`–`0xbecae`).  **PROVEN.**
* The **clock and memory** functions are `HSLS90*`/`HWS90*` for `0x2539` only.
  `0x2537` and `0x2538` use the Parhelia `HWPARInitializeMemoryController`
  @`0xe4110`, which reads `PInS[0x8c]` and calls
  `HWPARGetMCRegisters`/`HWPARSetMCRegisters` with its own `0x2537/0x2538`
  sub-branch (`0xe41f9`).  So "LX family" is **not** one class here: `0x2537`
  behaves like a Parhelia, `0x2538` is a hybrid (SNX reset + PAR clocks), and
  `0x2539` is its own class.  **PROVEN.**
* The liveness register differs by class: `0x0e00` vs `{0, 0x40000000}` on the
  Parhelia path, `0x0e24` vs `{0, 0x80000000}` on the P690 path.  **PROVEN.**
* `HSLPARSaveCriticalData`/`RestoreCriticalData` save MMIO `0x0360`/`0x0364`
  **only** for `0x2538`/`0x2539`, plus the PLLs `0x0e20`/`0x0e2c` for every
  chip.  The P690 never calls them, but the registers are still P690-specific
  state worth snapshotting around any reset we write.  **PROVEN.**
* The `& 0x7fc00` tests (`dev->[0x2440]` in `mtx_parhelia.o`, `dev->[0xc9b8]` in
  `mtx_drv.so` at `0x502b8`/`0x50527`/`0x508fd`, `ctx->[0x1c]` in `MTXPARD.dll`)
  are a **rendering-capability** bit group (they gate `0x11e8`, `0x12b8`,
  `0x1300` and the anti-aliasing paths — see `parhelia-engine-context-bringup.md`
  and `parhelia-antialiasing.md`).  **No reset or memory-controller code path
  consults them.**  Do not use that mask to gate a reset.  **PROVEN.**

---

## 7. Does the vendor use a PCI function-level reset?

Yes in substance, no in the Linux sense of the term:

* It writes PCI config `0xE0` with `3` then `0`.  On this family that register is
  the **PMCSR**, so the operation is a D3hot→D0 PM reset — the same thing
  `pci_reset_function_locked()` falls back to when there is no FLR capability.
  There is no PCIe FLR use anywhere in the binaries.
* It **does** reprogram the memory controller afterwards, on the P690 from the
  **VBIOS PInS block** (§5.3), as an unconditional part of the same
  `InitializeHardware` that issued the reset.  It also re-runs the system-clock
  init, and on the non-P690 chips re-strobes the `0x0e20`/`0x0e2c` PLLs from a
  snapshot.
* It also does a **wider config-space save/restore** than Linux does: all 64
  dwords `0x00..0xFC`, i.e. including the Matrox vendor-specific range
  `0x40..0xFC`.  Linux's `pci_save_state()` covers the standard header plus
  known capabilities and leaves vendor-specific dwords alone.

This is, most likely, the whole explanation of the `reset_on_probe=1` disaster:
the reset itself was fine and is what the vendor does, but we restored less
config space than the vendor does and then never re-initialized the memory
controller at all.  **INFERRED**, but it is the only reading consistent with
both the binaries and the observed symptom.

---

## 8. What to try on hardware, in order

Each step is independent of the next; stop at the first that clears
`0x08f0 & 0x03000000` and lets `warp_gate_ready()` pass.  Steps 1–3 cannot
affect the memory controller at all.

1. **The `0x0920` bit-31 strobe** (`parhelia-engine-and-context-init-complete.md`
   §6 open item 5, now with the exact vendor bracket from §4.2).  On a board
   that is sitting at `0x08f0 = 0x03020000`:
   `/root/parhelia-mmio 0x920=0x80000000`, `/root/parhelia-mmio 0x920=0`, then
   `/root/parhelia-mmio 0x8f0`.  Two register writes, no DMA, no memory access.
   If bits 24/25 clear, we are done and a module reload recovers a hang.
2. **The strobe inside the vendor's bracket.**  If step 1 alone does not clear
   it, repeat it with the descriptor-bank save/retarget/restore of §4.2 — save
   `0x0000..0x013c`, rewrite each slot as `v & 0xe0000000`, set slot 0 to
   `{(old0x10 & 0x0e000000) | 0x10000000, 0x08000000, 0x10000400, 0x00010001}`,
   strobe, restore.  The vendor never issues the strobe with a live descriptor
   bank, and the WARP fetches through it.
3. **Video reset + engine re-init without a chip reset.**  Run
   `parhelia_apply_vid_reset()` for both heads followed by the rest of
   `parhelia_engine_init()` and a full WARP re-upload.  This is free to try and
   is what the vendor does *around* the reset; on its own it is not expected to
   clear a latched WARP-busy, but it costs one module reload to find out.
4. **The full vendor chip reset, memory-controller re-init included.**  This is
   the one with real expected value, and it must be implemented as a unit:
   1. stop scanout (disable both display controllers) and fail every in-flight
      submission;
   2. snapshot MMIO `0x0300`, `0x0360`, `0x0364`, `0x0e20`, `0x0e24`, `0x0e2c`
      and PCI config dwords `0x00..0xFC`;
   3. the sequence of §3.3 verbatim (`0x0300` recompute, `cfg 0xE0 = 3`, 10 ms,
      `cfg 0xE0 = 0`, 10 ms, restore `0x00..0xFC` skipping `0x04/0x44/0x48`,
      then `0x04`, then `0x0300`);
   4. `HSLS90InitMemClk` of §5.1 — CRTC hold, 1 ms, `0x0ff4` bank 3 `= 0`,
      system clock, the 44 PInS registers of §5.3, the eight tuning writes, the
      idle waits — then release the CRTC hold;
   5. `parhelia_engine_init()` in full, WARP resident programs re-uploaded,
      state shadow invalidated, all client WARP slots invalidated.
   Gate the whole thing behind a module parameter until it is proven, and verify
   VRAM integrity immediately afterwards with a known pattern written and read
   back through `UPLOAD_RENDER`/`DOWNLOAD_RENDER` **before** re-enabling
   scanout.  Widen `parhelia_bios.c`'s PInS length requirement to `0x2c0` first
   and dump `PInS[0x210..0x2bc]` on the target to confirm the 44 values are
   present and non-zero — if they are absent, this board's VBIOS cannot drive
   the re-init and step 4 must not be attempted.
5. **Only if 1–4 all fail:** accept the reboot, but make it cheap — detect the
   latched state in `parhelia_engine_init()` (`0x08f0 & 0x03000000`) and fail
   the probe with an explicit "engine latched, reboot required" message instead
   of the current `-ETIMEDOUT` from `warp_gate_ready()`, so the state is
   unambiguous in the log.

Step 4 is what would let us stop rebooting after every engine hang; steps 1–2
are what would let us stop rebooting *cheaply*, and cost almost nothing to test.

---

## 9. Reproduction

```sh
D=/srv/t2/xf86-video-parhelia
W=$D/xddm32_210_00_105_se_u_nowhql
L=$D/matroxdriver-x86_64-cvs-20090511

sha256sum $W/MtxPser/mtxpserd.dll $W/MtxPser/mtxpserm.sys \
          $W/MtxPar/MTXPARD.dll $L/kernel/mtx_parhelia.o \
          $L/xserver/7_4_0/mtx_drv.so

# named vendor functions (the only binary with symbols)
nm -D --defined-only $L/xserver/7_4_0/mtx_drv.so | sort -k1 > /tmp/mtxdrv.syms
objdump -d --no-show-raw-insn -C $L/xserver/7_4_0/mtx_drv.so > /tmp/mtxdrv.dis
objdump -d --no-show-raw-insn $L/kernel/mtx_parhelia.o        > /tmp/mtxp.dis   # -dr for relocs
objdump -d --no-show-raw-insn $W/MtxPser/mtxpserm.sys         > /tmp/parm.dis

# the reset variants, the dispatch that selects them, and the P690 init
sed -n '/^00000000000c53e0/,/^$/p' /tmp/mtxdrv.dis    # HSLSNXHardwareReset  (P690/SUNEX)
sed -n '/^00000000000c5580/,/^$/p' /tmp/mtxdrv.dis    # HSLPARHardwareReset  (512/8x/LX)
sed -n '/^00000000000dd7c0/,/^$/p' /tmp/mtxdrv.dis    # HSLS90InitializeHardware
sed -n '/^00000000000dcef0/,/^$/p' /tmp/mtxdrv.dis    # HSLS90InitMemClk
sed -n '/^00000000000e59d0/,/^$/p' /tmp/mtxdrv.dis    # HWS90InitializeMemoryController
grep -n 'HSLSNXHardwareReset@@Base\|HSLPARHardwareReset@@Base' /tmp/mtxdrv.dis  # the dispatch

# the WARP loader gate in the Linux kernel core
awk '/^000000000009f6b0/,/^$/' /tmp/mtxp.dis | sed -n '1,200p'

# the miniport cross-check: SNX reset, P690 init, WARP self-test strobe
awk '/^ +323c6:/,/^ +3251e:/' /tmp/parm.dis
awk '/^ +4d285:/,/^ +4d2f0:/' /tmp/parm.dis
awk '/^ +57288:/,/^ +573d1:/' /tmp/parm.dis

# 0x08f0 is never written anywhere
grep -n '\$0x8f0,%esi' /tmp/mtxp.dis /tmp/mtxdrv.dis   # every hit is followed by a Read
grep -n 'push  *\$0x8f0'  /tmp/parm.dis                # likewise
```

The 44-entry memory-controller table, from either binary (they agree byte for
byte):

```python
import struct
# Linux X driver: symbol pinsDatasConvertor, VMA 0x34b580, .data VMA 0x32e420 @ file 0x12e420
f = open('.../xserver/7_4_0/mtx_drv.so','rb').read()
fo = 0x12e420 + (0x34b580 - 0x32e420)
# Windows miniport: .data VMA 0x5b300 @ file 0x4b300, table at VMA 0x72c28
# f = open('.../MtxPser/mtxpserm.sys','rb').read(); fo = 0x4b300 + (0x72c28 - 0x5b300)
for i in range(44):
    d0, reg, bank = struct.unpack_from('<III', f, fo + i*12)
    print(f'{i:3d}  pins+0x{0x210+d0:04x}  reg=0x{reg:04x}  bank={bank}')
```

---

## 10. Open items

* Does the `0x0920` bit-31 strobe clear `0x08f0` bits 24/25?  Untested; §8
  step 1.
* `ctrl` in `HWS90InitializeMemoryController`'s `0x0ff4` writes — assumed `1`.
* `0x1c50 = PInS[0x19c]` is written by both vendor init paths and by neither of
  ours; its function is undecoded.
* `0x0360`/`0x0364` (saved as "critical" only for `0x2538`/`0x2539`) are
  undecoded.
* Whether cfg `0xE0` is PMCSR on `0x2539` as it is on `0x0528` — read the PM
  capability pointer on the target to confirm before implementing §8 step 4.
* `HWS90InitializeSystemClock` @`0xe60d0` and `HWS90InitialiseFSController`
  @`0xe5bb0` are not decoded here; they are needed for §8 step 4 item 4 and are
  the obvious next RE target.
* Our `0x1c00` OR mask is `0x00808020` where both vendor paths use
  `0x00809020` (§5.4 item 2).  Bit 12 has not been identified; the driver works
  without it, so this is a low-risk correction to make together with a reset
  implementation rather than on its own.
