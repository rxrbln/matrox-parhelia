# P690 hardware fast depth clear

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

The Millennium P690 (PCI `102b:2539`, SUN90) has a working hardware depth
clear.  The vendor calls it *Z-init*, ships it disabled on this chip, and
`parhelia-p690-zcompression-clear-banks-re.md` recovered its command sequence
from the Windows HAL.  This document records **why** it is disabled, what the
block actually does on this silicon, and the measurements that let the Gallium
driver use it.  Evidence labels: **[PROVEN]** = read from the disassembly or
measured on hardware at the cited place; **[INFERRED]** = the most plausible
reading; **[OPEN]** = not resolved.

Hardware measurements were taken on a 128 MiB P690 with the `parhelia.ko` DRM
driver and `parhelia-drm-test --zinit-probe`.

## Summary

1. The feature is **not disabled by an erratum**.  It is a per-chip default in
   one expression: the vendor's `3D.EnableZinit` switch defaults to
   `(HwRevisionID & 0x3c0) != 0`, and mask `0x3c0` selects **only** PCI
   `102b:0528` (Parhelia-512).  Every P-series part — `2537`, `2538` and the
   P690 `2539` — falls outside it.  No binary in the vendor stack contains an
   errata string for this block. [PROVEN]
2. The block **exists and responds** on P690 silicon and is bit-exact.  Arming
   the bound depth surface with `DWGCTL 0x20a` and loading `0x12b4` with
   `DWGCTL 0x208` makes every tile of that surface read as the loaded value.
   A depth-tested, depth-writing scene over such a surface produces a colour
   and a depth image **byte-identical** to the same scene over a 2-D filled
   surface. [PROVEN, measured]
3. It writes **no memory**.  The state is on-chip and lazy: a tile is
   materialised the first time the Z unit touches it.  A tile no draw ever
   reaches keeps its old bytes, so anything that reads the surface outside the
   3-D Z unit needs the vendor's resolve rectangle first. [PROVEN, measured]
4. The measured coverage on this chip is **640 tiles of 4096 bytes =
   2 621 440 bytes**, whatever the pixel size, counted from the surface
   origin.  This is *not* the vendor's software limit of `0x3c0000`
   (3 932 160 bytes), which would over-run by 50 %. [PROVEN, measured]
5. A 1280x1024 **Z16** buffer is exactly 640 tiles, so the whole target of the
   ioquake3 benchmark is covered.  A 1280x1024 Z24S8 buffer is 1280 tiles and
   is not. [PROVEN, measured]
6. Only a **3-D draw that binds a different depth surface** destroys the
   tracked state.  A 2-D solid fill of an unrelated surface, and a 3-D draw
   that binds no depth surface at all, both leave it intact. [PROVEN, measured]
7. In ioquake3 at 1280x1024 in a 16-bit visual the clear drops from 1.69 ms to
   0.01 ms and the frame from 18.2 ms to 15.9 ms: **55.0 to 62.7 fps**.
   [PROVEN, measured]

---

## 1. Why the vendor ships it disabled

`matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` carries the real
symbol names in `.dynsym` while `.symtab` holds the obfuscated `mtxNNNN` ones,
so `readelf -sW` shows both for the same address.  The 3-D switch table is
built by `RegistrySwitchManager::RegistrySwitchManager` at `0x60d70`:

```
6101f: 41 f7 44 24 20 c0 03 00 00   test DWORD PTR [r12+0x20],0x3c0
61032: 41 0f 95 c6                  setne r14b
61043: lea  rcx,[rip+0xaf736]       # "EnableZinit"
61056: 45 0f b6 ce                  movzx r9d,r14b      ; the default
61060: call RegistrySwitch<unsigned char>::RegistrySwitch
61096: lea  rcx,[rip+0xaf6f2]       # "EnableZCompSundog"
610a9: 45 31 c9                     xor  r9d,r9d        ; default 0
```

`[r12+0x20]` is the one-hot chip word built by `HwRevisionID::Setup`
(`0x72620`): `0x0528` revisions map to `0x40/0x80/0x100/0x200`, i.e. exactly
mask `0x3c0`; `0x2539` maps to `0x10000/0x20000/0x40000`.  So **`EnableZinit`
can only ever default to 1 on a Parhelia-512.** [PROVEN]

The Windows HAL `mtxpserd.dll` has the same expression with one extra term,
`(caps & 0x3c0) && ([[ebx+0xc]+0x548] & 0x08000000)` at `0x10baaf..0x10bace`.
`[ebx+0xc]+0x548` is the miniport's packed memory-configuration word — bits
`[7:5]` are the memory type and `[26:25]` the memory size, decoded in
`mtxpserm.sys` at `0x84bf5` and `0x880ad` — and bit 27 is read nowhere else in
the whole stack and written by neither miniport, so it arrives from the BIOS.
[PROVEN that it is the memory-config word and a per-board BIOS bit; INFERRED
that it means "this board's memory geometry supports Z-init"].  It is moot for
the P690 because the `0x3c0` term is already zero.

`MTXPARD.dll` (the Parhelia-family HAL) is the same source built for the other
INF: its `EnableZinit` default at `0x109bff` is byte-for-byte identical and it
carries **both** Z-init budgets (`0x3c0000` at `0x9f7e5`, `0x300000` at
`0x9faae`).  There is therefore **no "older HAL enabled it, newer HAL disabled
it" regression** — one codebase, one policy. [PROVEN]

No string containing `errat`, `workaround`, `known issue` or `HW bug` exists in
`mtxpserd.dll`, `mtxpsogl.dll`, `MTXPARD.dll`, `mtx_parhelia.o`, `mtx_drv.so`
or any `.inf`/`.cfg` in either package.  Matrox encodes its errata as switch
*names*; the ones around this block (`Use2DClearForceZUncompSundog`,
`ZUncompAnd2DClearSceneInProgSundog`, `UseZCompInMultContextSundog`) all
belong to **Z compression**, which is a P690-only feature that does not exist
on Parhelia-512 at all and defaults to 0 everywhere. [PROVEN]

Two further details argue that the P690 Z-init block was characterised on real
silicon rather than left untested: the P690 path (`mtxpserd.dll` `0x9eff0`)
is a *second, more elaborate* emitter than the Parhelia one (`0x9ea30`), and
its parameters were re-tuned — tile 32x16 instead of 32x32, chunk granularity
16 rows instead of 32, budget `0x3c0000` instead of `0x300000`
(`0xa0d95`/`0xa0dc2` against `0xa104f`/`0xa109b`). [PROVEN]

Finally, the Linux vendor stack sets `Use3DClearSundog = 1` by default on
P690 (`mtx_drv.so` `0x6131e`): on this chip it routes depth clears through the
3-D pipe rather than the 2-D engine.  Section 5 shows why that matters.
[PROVEN]

## 2. The commands

`register = 0x1000 + 4 * dma_index`, so `0x12b4` is index `0xad` and `0x1300`
(DWGCTL) is index `0xc0`.  The vendor emits each command as one register list
that ends by restoring the running draw word, so the command acts once
(`mtx_parhelia.o` `mtx2784` `0x104f0`, `mtx1447` `0x10730`; `mtxpserd.dll`
`0x9eff0`).

| DWGCTL | name here | effect [PROVEN on hardware unless noted] |
|---|---|---|
| `0x0000020a` | `P3D_ZTRACK_ON` | arm Z tracking for the depth surface bound in slot 1 |
| `0x0000000a` | `P3D_ZTRACK_OFF` | stop tracking and **discard** any pending tile state |
| `0x00000208` | `P3D_ZINIT_LOAD` | every tracked tile now reads as `0x12b4`, uncompressed |
| `0x00000308` | `P3D_ZINIT_RESET` | reset the tile state without loading a value |
| `0x00000008` / `0x00000108` | — | the same two with Z compression engaged (bit `0x200` clear means *not* compressed); not exercised |
| `0x00000207` + two `0x12bc` writes | — | pipeline sync, emitted by the vendor only while Z compression is active |

`0x12b4` takes the same packed word as the 2-D fill's FCOL: `(z16 << 16) | z16`
for D16, `(stencil << 24) | z24` for S8Z24 (`mtxpserd.dll` `0x9c220`).

The full clear this driver emits is two register lists in one batch:

```
0x1228 = depth base, 0x122c = depth cfg
0x1300 = 0x0000020a, 0x1300 = 0x000000c0        ; arm
0x1228 = depth base, 0x122c = depth cfg
0x12b4 = value, 0x1300 = 0x00000208, 0x1300 = 0x000000c0   ; load
```

The trailing draw word is not cosmetic: this kernel replays the last DWGCTL of
a batch after its terminal DWGSYNC (`parhelia_submit.c`
`parhelia_batch_dwgctl_range()`), so a batch that ended on a Z command would
execute it twice.

## 3. What the hardware does — measured

`parhelia-drm-test --zinit-probe /dev/dri/cardN [cpp]` renders the same battery
of depth-test bands twice: once over a 2-D filled surface and once over a
surface deliberately filled with the complement of the clear value and then
cleared in hardware.  It compares the colour image (which reports the depth
test outcomes) and the depth image byte for byte.  `ZINIT_VARIANTS=1` drops one
command at a time; `ZINIT_W`/`ZINIT_H` set the surface size; `ZINIT_RATE=N`
times the two clears against each other.

Results at 256x256, for clear values `0`, mid, max and max-3, at Z16 and Z24S8:

| variant | colour | depth | reading |
|---|---|---|---|
| full sequence | identical | identical | the clear is exact |
| without `0x20a` | differs | differs, poison survives | the arm is mandatory |
| without the resolve rectangle | identical | identical | every tile was touched by a band |
| without `0x308`/`0x00a` | identical | identical | neither is needed for correctness |
| arm and load only | identical | identical | the load alone is the clear |
| `0x00a` immediately after the load | differs, **exactly as without the arm** | same | `0x00a` discards pending state |
| only half the bands drawn, with resolve | identical | identical | the resolve materialises untouched tiles |
| only half the bands drawn, no resolve | identical | **the undrawn half keeps the poison** | the state is lazy, not a memory write |
| 2-D fill of another surface midway | identical | identical | a 2-D fill does not disturb it |
| depth-less 3-D draw midway | identical | identical | a draw without a Z surface does not disturb it |
| 3-D draw on a second depth surface midway | differs | differs | **this destroys the state** |

So the model is: `0x20a` arms tracking for the surface in slot 1; `0x208`
marks every tracked tile "cleared to `0x12b4`"; the Z unit materialises a tile
on first access, correctly for both the test and the write; `0x00a` throws the
rest away; and a full-surface depth-EQUAL rectangle at the clear value, with
depth writes and no colour writes, flushes whatever is left to memory.
Binding another depth surface into slot 1 for a 3-D draw loses it. [PROVEN]

## 4. Coverage: 640 tiles

The arm packet carries no extent, so the hardware covers a fixed amount from
the surface origin.  Measured with `ZINIT_VARIANTS=1` and the "no draws,
resolve only" variant, which reports exactly the bytes the clear failed to
reach.  A tile is 4096 bytes on P690 (64x16 pixels at 32 bpp, 128x16 at
16 bpp):

| surface | total tiles | uncovered bytes | covered tiles |
|---|---|---|---|
| 640x1024 Z24S8 | 640 | 0 | 640 |
| 704x1024 Z24S8 | 704 | 262 144 | 640 |
| 768x1024 Z24S8 | 768 | 524 288 | 640 |
| 1024x1024 Z24S8 | 1024 | 1 572 864 | 640 |
| 1280x768 Z24S8 | 960 | 1 310 720 | 640 |
| 1280x1024 Z24S8 | 1280 | 2 621 440 | 640 |
| 1280x1024 Z16 | 640 | 0 | 640 |
| 1280x1200 Z16 | 750 | 450 560 | 640 |

**640 tiles, i.e. 2 621 440 bytes, in every case.** [PROVEN]  The vendor's
`0x3c0000` = 3 932 160 bytes would have left the last 320 tiles of a 1280x768
Z24S8 buffer uncleared, so the software limit cannot be taken on trust for this
chip.  Whether the shortfall is a P690 variant, a different tile geometry than
the vendor assumed (the vendor's formula divides by a 32-pixel-wide tile, this
chip's is 64 or 128 wide), or a smaller on-chip array, is **[OPEN]**; the
measured number is what the driver uses.

The driver therefore uses the hardware clear only when the whole depth
allocation fits in 640 tiles.  A larger buffer keeps the 2-D fill; the
vendor's "clear the first N rows in hardware and 2-D fill the rest" hybrid is
not implemented, because the uncovered region is a ragged tail in tile order
rather than a rectangle whenever 640 is not a multiple of the tile row.

## 5. Why a partial clear must not be a 2-D fill

A 2-D fill of part of a surface whose tiles the Z unit still owns is wrong: the
fill writes memory, and the Z unit later materialises its own value over the
same tiles.  The correct partial clear is a **rectangle through the depth
pipe** — depth test ALWAYS, depth writes on, colour writes masked off — which
the Z unit handles like any other draw.  This is exactly the vendor's
`Use3DClearSundog = 1` default on P690, and it is what makes the whole feature
pay off: ioquake3 issues a small scissored `glClear(GL_DEPTH_BUFFER_BIT)` for
its view model several times a frame, and routing those through the 2-D fill
forced a full-surface resolve once per frame, which cost most of the gain
back.  With the depth-pipe rectangle the trace shows one hardware clear and
about four depth-pipe rectangles per frame and **no resolves at all**.

| ioquake3, 1280x1024, 16-bit visual | fps | ms/frame |
|---|---|---|
| 2-D fill depth clear | 55.0 | 18.2 |
| hardware clear, partial clears still 2-D filled | 57.0 | 17.5 |
| hardware clear, partial clears through the depth pipe | **62.7** | **15.9** |

Isolated clear cost, 300 iterations (`ZINIT_RATE=300`):

| surface | 2-D fill | hardware |
|---|---|---|
| 1280x1024 Z16 | 1.69 ms, 777 Mpixel/s | 0.01 ms |
| 1280x768 Z24S8 | 2.04 ms, 482 Mpixel/s | 0.01 ms |

## 6. Driver rules

The Gallium driver (`PARHELIA_ZINIT_CLEAR=1`) applies the hardware clear when
**all** of the following hold:

* the clear covers the whole surface, at the origin, with write mask
  `0xffffffff` — the command carries neither an extent nor a mask, so a
  depth-only clear of a Z24S8 surface is excluded (it would zero the stencil
  byte), exactly as the vendor requires (`mtxpserd.dll` `0xa0cd5`);
* the whole allocation fits in 640 tiles;
* a colour render target at least as wide as the depth surface is bound, so
  the resolve rectangle has a slot-0 surface to rasterise against with its
  colour writes masked off.

While a clear is outstanding the driver resolves before anything that reads the
surface outside the Z unit — a transfer map, a blit, a copy region, a
`flush_resource`, binding it as a sampler view, a masked or stencil-bearing
clear — and before handing the Z unit to a **different** depth surface.  A
framebuffer with no depth surface is not a handover (section 3), so the common
"bind a depth-less framebuffer for a moment" pattern costs nothing.

### Verified on hardware

* `--zinit-probe` at Z16 and Z24S8, four clear values each, all variants.
* A GL client that clears depth, draws, and reads the depth buffer back with
  `glReadPixels(GL_DEPTH_COMPONENT)`: byte-identical depth and colour sums with
  and without the feature, for clear values 1.0, 0.5, 0.25 and 0.0, with and
  without an intervening scissored depth clear, on both a depth-16 and a
  depth-24 X server.  This exercises the resolve path end to end.
* Five `gltest` scenes (triangles, quads, tristrips, textured and untextured,
  clear values 1.0, 0.75, 0.5, 0.25) compared on the **scanout**: bit-identical
  framebuffer dumps with and without the feature.

### Why it is not on by default

The tracked state is one global hardware resource and the driver can only see
its own contexts.  Two GL clients that both render with a depth buffer at the
same time will destroy each other's pending clear, which shows as transient
depth artefacts in one of them until its next clear.  Nothing in the X server
path triggers this — glamor's 2-D fills, blits and depth-less draws are all
harmless (section 3) — so the single-3-D-client case, which is the benchmark
and the normal desktop, is safe.

Turning it on by default needs arbitration of the single armed surface across
DRM clients.  The kernel already sees every depth-surface binding (relocation
index `0x8a`) and could publish "who owns the Z unit" as a `GET_PARAM`, or
refuse the arm while more than one client has a depth surface; neither is
implemented. **[OPEN]**

## 7. Z compression

Not attempted.  `EnableZCompSundog` defaults to 0 on every chip and every OS,
it exists only on the P-series, and engaging it (DWGCTL `0x008`/`0x108`, bit
`0x200` clear) drags in the `0x207` sync packet, the decompress pass
`0xa4070`, and three documented mid-scene workarounds.  The `0x12bc` fence
register it needs is kernel-owned in this driver's ABI
(`parhelia_validate_reglist()` rejects index `0xaf`), so a compression
experiment needs kernel work first.  The fast clear above delivers its win
without it. **[OPEN]**

## 8. Reproduction

```sh
# vendor evidence
M=matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so
readelf -sW $M | grep -i 'RegistrySwitchManager\|HwRevisionID'
objdump -d -Mintel --start-address=0x60d70 --stop-address=0x614b0 $M  # switch table
objdump -d -Mintel --start-address=0x72620 --stop-address=0x72740 $M  # HwRevisionID::Setup
K=matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -dr --start-address=0x104f0 --stop-address=0x10800 $K         # the Z-init emitters
D=xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll
objdump -d -Mintel --start-address=0x10ba90 --stop-address=0x10bb40 $D # EnableZinit default

# hardware
parhelia-drm-test --zinit-probe /dev/dri/card0 2          # Z16
parhelia-drm-test --zinit-probe /dev/dri/card0 4          # Z24S8
ZINIT_VARIANTS=1 parhelia-drm-test --zinit-probe /dev/dri/card0 2
ZINIT_W=1280 ZINIT_H=1024 ZINIT_RATE=300 \
	parhelia-drm-test --zinit-probe /dev/dri/card0 2
PARHELIA_ZINIT_CLEAR=1 q3bench.sh
```
