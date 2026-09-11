# P690 clocks, memory controller and 3D throughput tuning

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a static audit of the retained vendor binaries, the exact P690 option
ROM and the current open kernel/Mesa sources.  It does not touch the target or
change either driver.  Addresses in ELF files are virtual addresses; addresses
in `MTXPARD.dll` are image virtual addresses with image base `0x10000`.

## Inputs

| input | SHA-256 |
|---|---|
| `tools/p690.rom` | `47abaa677bfd95a7e6aa9a40e27c9108c0ab6d219bdc4d276de0532be14a1071` |
| `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | `8f5ca1d85f1c13fda731c8a0ba28db063ec0e697b7bc5dfff300c1500d564e4f` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll` | `41b2317309801a917506c29c2b5998713ddb9bdfa3d26c623ab56617d0c1d848` |

The current open sources compared below are
`linux-7.2/drivers/gpu/drm/parhelia/parhelia_engine.c`,
`parhelia_drv.c`, `parhelia_kms.c`, and Mesa's
`reference/p3d_state.c`.

## Result and recommended probes

There is no evidence that the open driver has underclocked this board.  The
ROM routes P690 GCLK through FS0 and requests 350000 kHz.  KMS deliberately
uses only FS2/FS3 for pixel clocks and does not alter FS0.  The open kernel
also preserves the POST-programmed SUN90 memory controller and the existing
priority fields.  Reprogramming FS0 or the 44 PInS memory timings during a
running session is therefore high risk and is not the next performance test.

Two exact vendor 3D throughput choices are absent from the open stack:

1. P690-class hardware uses `TEDLFCounters = 0x001e4087`, emitted once to
   MMIO `0x0bc0` during 3D engine initialization.  The current kernel never
   writes `0x0bc0`.  The familiar `0x00004087` value came from the older
   0x0528 path and is not the P690 default.
2. The Windows driver changes per-operation `MemLevels` at MMIO `0x12b0`
   from `0x3fff8842` to `0x3fff9084` above 250000 pixels.  Mesa always emits
   `0x3fff8842`.  A 640x480 Q3 target contains 307200 pixels and crosses the
   vendor threshold.

The safest first A/B is a userspace `MemLevels` override because `0x12b0` is
already ordinary validated draw state and the exact Q3-size branch is proven.
Compare a fixed q3dm1 run and uncapped glxgears with `0x3fff8842` versus
`0x3fff9084`, retaining image comparison and timeout checks.

The next bounded kernel A/B is a read-only load-time report of `0x0bc0`,
`0x0e04`, `0x0e30`, `0x1c0c`, and `0x1c10`, plus an opt-in load-time-only
module parameter that writes `0x001e4087` to `0x0bc0` during engine init on
PCI ID 0x2539.  Keep its default disabled until the exact before/after values and
static image are captured.  The module remains deployed from the temporary
tree as required by the project recovery rule.

## Exact P690 clock description

The PInS block starts at ROM file offset `0x76c0`, has signature `.A`, length
`0x400`, and version `0x0700`.  The system-clock fields are:

```
PInS+0x024 = 0x00055730 = 350000 kHz
PInS+0x028 = 0x00000000 (no precomputed FS0 word; vendor solves it)
PInS+0x200 = 0x00020902
PInS+0x204 = 0x00000002
PInS+0x208 = 0x00000002
PInS+0x20c = 0x60000000
```

This corrects the stale `parhelia-engine-context-bringup.md` measurement that
names an offset `0x7800`, version `0x0200`, and words `01610097 000c028b
00000328`.  Those bytes are not present in the ROM identified by the hash
above.  A later record-like area at `0x7838` must not be substituted for the
real `.A` PInS at `0x76c0`.

`HWS90InitializeSystemClock` in `mtx_drv.so` is at `0xe60d0` (size `0x392`).
It reads selectors at PInS `+0x200/+0x204/+0x208`, resets the three selectors
at MMIO `0xe00/0xe04/0xe08`, and calls `IsScUseFS_SUN90` (`0xe5f90`, size
`0x138`) for FS0 through FS3.  A used FS with a nonzero programmed word at
PInS `+0x28/+0x3c/+0x50/+0x64` is written to `0xe30+4*FS`, first with bit 31
set, then without it.  When that word is zero it calls
`HWS90InitialiseFSController` (`0xe5bb0`, size `0x33e`) with the target at
PInS `+0x24/+0x38/+0x4c/+0x60`.  It finally restores the PInS selectors and
writes PInS `+0x20c` to `0xe24` (`0xe63e4..0xe63f2`).

`IsScUseFS_SUN90` decodes each selector as `(selector >> 1) & 0xf`:

| FS | selector codes which consume it |
|---:|---|
| 0 | 1, 8, 9 |
| 1 | 2, 10, 11 |
| 2 | 3, 12, 13 |
| 3 | 4, 14, 15 |

The exact ROM selectors therefore consume FS0, and the zero at PInS `+0x28`
makes the vendor solve FS0 for 350000 kHz.  Independently,
`HSLS90GetGclk` (`0xdc910`) reads `0xe04`, extracts the same selector field,
and passes it to `HSLS90TranslateGclkSelIntoFS` (`0xdc870`): codes 1, 2, 3,
and 4 map to FS0, FS1, FS2, and FS3.  PInS `+0x204 = 2` thus selects code 1,
FS0, for GCLK.

The current KMS SUN90 solver's first legal result for 350000 kHz is
`b0=31, b1=15, b2=9, b3=2`, 348750 kHz, packed as `0x0001439e`.  This is an
open-solver result, not a claim that POST currently contains that exact word;
the proposed read-only `0xe30` report settles it without changing the clock.

`HSLS90ChangeGclkSpeed` (`0xdc160`, size `0x206`) is not a lightweight clock
switch: after changing its PInS shadow and clock selector it calls both
`HWS90InitializeSystemClock` (`0xdc22a`) and
`HWS90InitializeMemoryController` (`0xdc232`).  That supports preserving FS0
and rules out a live GCLK rewrite as an initial performance probe.

## Exact SUN90 memory-controller image

`HSLS90InitMemClk` is at `mtx_drv.so` `0xdcef0` (size `0x144`).  It saves
`0x1c00`, `0x1c04`, `0x1800`, and `0x1a00`; holds both display clocks with
bit `0x20`; disables both heads by clearing bit 0; resets the controller;
initializes system clock and memory controller; reads the resulting system
clock; and restores all four saved display registers.  Partial execution is
unsafe because scanout and memory are deliberately stopped during the
transaction.

`HWS90InitializeMemoryController` is at `0xe59d0` (size `0x1d1`).  Its 44
triples are stored at `mtx_drv.so` VMA `0x34b580`; each triple is
`{PInS offset after +0x210, register, controller}`.  Applied to the exact ROM,
the initial image is:

| PInS | ctrl | reg | value |
|---:|---:|---:|---:|
| `+210` | 3 | `0xf00` | `0x00083e80` |
| `+214` | 3 | `0xf04` | `0x000000cc` |
| `+218` | 3 | `0xf08` | `0x00000075` |
| `+21c` | 3 | `0xf10` | `0x000b681e` |
| `+220` | 3 | `0xf1c` | `0x0000000c` |
| `+224..+234` | 3 | `0xf20,0xf24,0xf28,0xf2c,0xf44` | `0x00008000` each |
| `+238` | 3 | `0xf94` | `0x84210000` |
| `+23c` | 3 | `0xf98` | `0xdd22ee11` |
| `+240` | 3 | `0xf9c` | `0x7788bb44` |
| `+244` | 3 | `0xfa0` | `0x00000000` |
| `+248` | 3 | `0xfc0` | `0x00000a62` |
| `+24c` | 3 | `0xfc4` | `0x00000004` |
| `+250` | 3 | `0xfc8` | `0x00000000` |
| `+254` | 3 | `0xfcc` | `0x00000000` |
| `+258` | 3 | `0xff0` | `0x001f1f1f` |
| `+25c` | 3 | `0xff8` | `0x00000001` |
| `+260` | 1 | `0xf14` | `0x3072666e` |
| `+264` | 1 | `0xf18` | `0xab000078` |
| `+268,+26c` | 1 | `0xf48,0xf4c` | `0x00000249` each |
| `+270..+284` | 1 | `0xf58,0xf5c,0xf68,0xf6c,0xf70,0xf74` | `0x00000000` each |
| `+288,+28c` | 1 | `0xf8c,0xf90` | `0x00000249` each |
| `+290` | 2 | `0xf14` | `0x3072666e` |
| `+294` | 2 | `0xf18` | `0xab000078` |
| `+298,+29c` | 2 | `0xf48,0xf4c` | `0x00000249` each |
| `+2a0..+2b4` | 2 | `0xf58,0xf5c,0xf68,0xf6c,0xf70,0xf74` | `0x00000000` each |
| `+2b8,+2bc` | 2 | `0xf8c,0xf90` | `0x00000249` each |

After the table, the function writes `1` through controller 3 to `0xff4`,
delays 5, and waits idle.  It reads controller-1 `0xf00`, sets bit 30, writes
the result to controller-3 `0xf00`, waits idle, and resets the controller.
It then rewrites controller-1 `0xf48/0xf4c/0xf58/0xf5c` from PInS
`+0x268/+0x26c/+0x270/+0x274`, and the controller-2 counterparts from
`+0x298/+0x29c/+0x2a0/+0x2a4`, before another `0xff4=1`, delay and idle wait.

`WaitForS90MemoryIdle` (`0xe5950`, size `0x7e`) polls controller-1 `0xff8`
until byte 1 has both bits `0x06`, with a maximum of 100000 iterations.  The
current open kernel intentionally performs none of this sequence.  It logs
that POST owns SUN90 timing, and no open engine-init write targets `0xf00` to
`0xffc`.  This is the correct policy while the board boots reliably and is
not evidence of a performance omission.

## FIFO, priority and texture-fetch controls

The current engine initialization ORs `0x0808` into both `0x1c0c` and
`0x1c10`.  Vendor `HSLPARSetPriorityLevels` (`mtx_drv.so` `0xce500`) changes
the low three bits and bits 8 through 10 of those registers.  The open RMW
therefore enables the recovered bits without erasing POST's other priority
fields.  No saved P690 live register dump records a contrary value.

`0x12b0` is per-validate `MemLevels`, not a mode-set memory timing.  The
baseline default is independently present in the Linux parameter constructor
at `mtx_parhelia.o` `0x7653f` (`0x3fff8842`).  Windows has two generic large
operation tests:

```
MTXPARD 0x0a622f: cmp width*height, 0x0003d090  # 250000
MTXPARD 0x0a624c: mov eax, 0x3fff9084

MTXPARD 0x10ddaf: cmp width*height, 0x0003d090
MTXPARD 0x10ddbc: shadow MemLevels = 0x3fff9084
MTXPARD 0x10ddf4: emit MMIO 0x12b0 through the register writer
```

Mesa `reference/p3d_state.c` always writes `0x3fff8842`.  The threshold is a
direct match for the remaining Q3 workload and is the highest-confidence
performance probe in this audit.

Windows also has a clock-dependent initialization ladder at
`MTXPARD.dll` `0x2c49a..0x2c53a`.  If capability bit `0x10` is clear it uses
`0x3fff8000`; otherwise its computed clock ratio selects:

| ratio threshold | value |
|---:|---:|
| invalid or `>=770` | `0x3ff00000` |
| `>=750` | `0x3ff68000` |
| `>=700` | `0x3ffa0000` |
| `>=667` | `0x3ffc8000` |
| `>=600` | `0x3ffd0000` |
| `>=500` | `0x3ffe0000` |
| below 500 | `0x3ff00000` |

This is an initialization/idle value under an unresolved capability bit.  It
does not replace the proven per-operation `0x3fff9084` branch and should not
be copied into Mesa.

`0x0bc0` is the strongest missing once-per-engine setting.  Both vendor
stacks obtain it from their capability/tunable object and write it directly:

```
Linux mtx1475  0x3e805: caps vtable call at +0x20
Linux mtx1475  0x3e830: esi = 0x0bc0
Linux mtx1475  0x3e835: MMIO write call

Windows          0x2efa4: capability vtable call at +0x0c
Windows          0x2efaf: push 0x0bc0
Windows          0x2efbe: direct register writer
```

The value is hardware-class dependent.  Windows parameter setup tests
`caps & 0x0007fc00` at `0x10a61e`; the capable/P690 branch constructs
`TEDLFCounters` with `0x001e4087` at `0x10a639`, while the baseline branch
uses `0x00004087` at `0x10a663`.  The Linux binary independently carries the
same capable default in `mtx722` at `0x75969` and the baseline default at
`0x764aa`.  P690 is in the capable `0x7fc00` family in the vendor FAA, tile
geometry, and texture paths.  An old 0x0528 MMIO capture shows `0x00004087`;
that capture must not be treated as the P690 value.

`0x11e8` is not a remaining optimization candidate.  Windows tables call it
a texture-cache partition and use `0x00500000` for one or two active stages,
but it was not present in the working Linux GL capture.  Prior hardware tests
found that `0x00500000` had no useful effect on P690 sampled alpha and latched
the older 0x0528 path.  Production Mesa correctly leaves the test-only
`texture_cache_probe` unset.

Likewise, `reg3DVECacheMode=TRUE` in `MtxParhlParms.c` refers to the WARP
vertex-engine instruction cache.  The open driver uploads its canned WARP
programs during engine initialization and reuses their entries; no sampled
texture-cache register follows from that parameter.  `reg3DTEFastTrilinear`
defaults to zero and mip sampling remains a known P690 timeout route, so it is
not a safe Q3 optimization.

## Q3-specific search result

No meaningful `Quake`, executable-name, or application-profile string occurs
in the saved `MTXOGL.dll` or `MTXPARD.dll`.  The OGL defaults `FastBitmap` and
`FastRasterPos` are generic GL bitmap/raster operations, while Q3's world and
HUD use textured triangle geometry.  They do not explain the current world
rendering rate.  `VeryFastDrawPixels` is disabled by default and applies to a
different GL transfer path.

The 250000-pixel `MemLevels` branch is not a game fingerprint, but it is a
generic vendor fast path that Q3 at 640x480 deterministically takes.  It is the
only newly recovered draw-time choice here with an exact trigger, exact value,
and direct mapping to current Mesa state.

## Reproduction commands

```sh
sha256sum tools/p690.rom \
  matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so \
  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll

objdump -d -Mintel --start-address=0xe5890 --stop-address=0xe6470 \
  matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so
objdump -s --start-address=0x34b580 --stop-address=0x34b790 \
  matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so
objdump -d -Mintel --start-address=0x3e7b0 --stop-address=0x3e94c \
  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -d -Mintel --start-address=0x75960 --stop-address=0x759a0 \
  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -d -Mintel --start-address=0xa61d0 --stop-address=0xa6290 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
objdump -d -Mintel --start-address=0x10a5c0 --stop-address=0x10a690 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
```
