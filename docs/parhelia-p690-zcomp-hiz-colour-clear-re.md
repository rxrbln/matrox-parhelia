# P690 Z compression internals, hierarchical Z, colour clear and per-pixel traffic

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Offline static reverse engineering for the Millennium P690 (PCI `102b:2539`,
SUN90).  No hardware was touched and no driver source was changed.  This
document extends `parhelia-p690-zcompression-clear-banks-re.md` (command words,
ownership, decompress pass) and `parhelia-p690-fast-depth-clear.md` (Z-init
measured on silicon: 640 tiles of 4096 B, lazy, writes no memory).  Their
results are not repeated, only cited.

Evidence labels: **[PROVEN]** = read from the disassembly at the cited
address; **[INFERRED]** = the most plausible reading of proven code, not
confirmed on hardware; **[OPEN]** = not resolved.

## Inputs

| file | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` | `52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a` |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpsogl.dll` | `db0ab86fd25fe2cfb37bae5252d825aac8a2cc16d88191dedb165d4ba57dbcf0` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `xddm32_210_00_105_se_u_nowhql/mtxpser.inf` | `169f49f2a3643e01f47d650aece6b56c5d418d08912d8804c96a79a33051e405` |
| `xddm32_210_00_105_se_u_nowhql/mtxpar.inf` | `d66a32db2ff8f1a49b5ea60efd3662d0b8ecc7678a17d21f3379244435ef567b` |
| `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | `8f5ca1d85f1c13fda731c8a0ba28db063ec0e697b7bc5dfff300c1500d564e4f` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |

`mtxpserd.dll` / `MTXPARD.dll` addresses are image VAs (image base `0x10000`,
file offset = VA − `0x10000`).  ELF addresses are virtual addresses.  DMA
packet grammar as in the earlier documents: `[count][4 index bytes][values]`,
index `i` → MMIO `0x1000 + 4*i`, index `0x21` = unused slot.

## Summary

1. **`0x9f5b0` is not Z-compression state.**  It is the FAA (fragment
   antialiasing) retirement routine, the `mtxpserd.dll` twin of `MTXPARD.dll`
   `sub_9E000`.  The eight 18-bit words it reads back from the fragment buffer
   head are restored by `0x9ed40` into MMIO `0x1340..0x134c` and
   `0x13c0..0x13cc`.  `UseZCompInMultContextSundog` has nothing to do with it.
   [PROVEN, §1]
2. **The two unexplained ZComp gates are decoded.**  `[fmtinfo+4] == 3` is the
   *format class* "depth/stencil" (table at `0x84e60`), and
   `(byte [this+0x12]) & 7 == 2` is **bits [18:16] of the depth surface's
   `0x122c` configuration word = surface layout 2**.  The open driver's depth
   surfaces (`p3d_surface_cfg()`: layout 2, format `0x36`/`0x2a`) satisfy both
   for `0x36`. [PROVEN, §2]
3. **Z compression has no size limit in software**, unlike Z-init: the arming
   paths in `0x9f3b0` contain no extent test.  No tag memory is allocated and
   no register other than DWGCTL is involved; where the per-tile state lives
   remains open. [PROVEN for the code, §3]
4. **The vendor relies on `DWGCTL 0x00a` being a *suspend*, not a discard.**
   It emits `0x00a` before every 2-D operation (`0x106eb0`, 42 call sites),
   when another depth surface is bound (`0xa0110`), and on every context
   switch (`0x102c80`), while keeping the owner pointer, and re-arms with
   `0x20a` later without reloading or resolving.  This is the missing
   multi-client protocol for the Z-init fast clear. [PROVEN for the code;
   hardware retention INFERRED, probe P4]
5. **No hierarchical / coarse / early-Z control exists anywhere in the vendor
   binaries.**  No option string, no register, no DWGCTL word.  The measured
   "Z reject = colour-only = 655 Mpixel/s" does not decide early vs late
   rejection; a textured probe does. [PROVEN negative, §4]  The textured
   probe (§4.4) settles it: a depth-rejected textured fragment still pays
   for its texture fetch, so rejection is late. [MEASURED]
6. **There is no colour fast clear and no block-write mode.**  Colour clears
   are 2-D fills with `DWGCTL = 0x000005c0`, and the P-series fill emitter
   **splits every fill into 32-pixel-wide full-height column strips** unless
   `DirectX.DdFlags & 2` (default 0).  The open driver fills one rectangle.
   [PROVEN, §5]
7. **`MemLevels` (`0x12b0`) is six 5-bit fields.**  All nine vendor values
   decode cleanly: `[14:0]` = three fields set to 2 (default) or 4 (> 250 000
   pixel 2-D operations), `[19:15]` = a clock-dependent field 0..31, `[29:20]`
   = two fields fixed at 31. [PROVEN values, field meaning INFERRED, §6]
8. **Why ZComp ships disabled:** the default is 0 on every chip and OS with
   no revision gate, and the shipped code has two concrete correctness gaps:
   the CPU-lock decompress only handles format `0x13` although compression is
   armed for `0x13..0x19, 0x1b, 0x1c`, and nothing decompresses before a
   depth surface is sampled as a texture or blitted. [PROVEN for the gaps,
   reason INFERRED, §7]

---

## 1. `0x9f5b0` decoded: FAA retirement, not Z compression

### 1.1 Identity [PROVEN]

`this` (`ecx`) is the per-context fragment context at `ctx+0x2dcc`
(callers `0x102d01`, `0x9fe5c`: `lea ecx,[eax+0x2dcc]`).  Its fields match
`MTXPARD.dll` `sub_9E000` (documented in `parhelia-antialiasing.md` §9a.4)
instruction for instruction: state `[this+0xc4]` ∈ {1, 2}, enable
`[this+0xe8]`, owner context `[this+0xb4]`, device `[this+0xbc]`, AA control
word `[this+0xfc]`, saved slot-2 descriptor `[this+0x114]/[this+0x124]`.

```
9f5b6: mov eax,[esi+0xe8] / test / je 0x9f97e     ; not FAA -> zero +0xc8..+0xe4, ret 0
9f5cf: mov eax,[esi+0xc4] ; cmp 1 / cmp 2 ; jne 0x9f97e
9f5bf: mov edi,ds:0x1ede20                         ; Z tracking currently on?
9f605: call 0x9f270 (cmd,0,0)                      ; if on: DWGCTL 0x00a
9f61d: mov ebx,[ecx+0x10]                          ; helper block [[pDev+0x21e4]+0x10]
9f641: and eax,0xfff1ffff / or eax,0x10000e        ; 0x12c8: test off, func 7, 8 stencil bits
9f651: or  [ebx+0xc],0x24000000                    ; dirty
9f65e: and eax,0xffffc020 / or eax,0x20            ; 0x1224 (slot 0 cfg): pitch 32
9f663: or  edx,3 ; mov [ebx+0xa0],edx              ; AA control = [this+0xfc] | 3
9f682: mov [ebx+0xab8],[0x1d3f34]                  ; 0x122c = fragment buffer cfg
9f68e: mov [ebx+0xaa8],[0x1d3f30]                  ; 0x1228 = fragment buffer address
9f6af: mov [ebx+0xabc],[esi+0x124]                 ; 0x1234
9f6b5: mov [ebx+0xaac],[esi+0x114]                 ; 0x1230
9f69c: mov WORD [ebx+0x8c],5 ; 9f6a6: WORD [ebx+0x8e],3
9f6bb: call 0x30010 ; 9f6c2: call 0x2d2c0          ; flush state block, kick
9f6dc: call 0x9eb10 (.., 0xc00)                    ; DWGCTL 4/0x407 + 0xc07 syncs with fences
9f70c: 14-dword packet, tags 0x605e5d58/59/5a      ; one small triangle
9f817: [0xc0afafc0] 0x1300=0x207, 0x12bc=tag|1, 0x12bc=tag, 0x1300=normal
9f852: call 0x29ac0 ([pDev+0xa9e0],0,1,0)          ; lock the fragment buffer
9f857..9f91a: dwords 0..3 and 32..35, each & 0x3ffff -> [esi+0xc8..0xe4], then zeroed
9f940: call 0x294f0                                ; unlock
9f95b: call 0x9f270 (cmd,1,0)                      ; if it was on: DWGCTL 0x20a
9f966: and eax,0xfffffff3 / or eax,2 -> [ebx+0xb4] ; 0x12c8 func field back
```

`[pDev+0xa9e0]` is the device fragment buffer, built by `0x9f9a0` (cfg
`(cfg & 0xf6fbc020) | 0x36030020` into `0x1d3f34`, the twin of
`MTXPARD.dll` `sub_9E3F0`); the earlier zcompression document called it "the
32-pixel helper surface".

### 1.2 The restore `0x9ed40` [PROVEN]

```
9ed43: test [edi+0xe8] ; state [edi+0xc4] in {1,2}
9ed86: call 0x9eb10 (.., 0xc00)                     ; only if the retire result was 0
9edcc: count 9
9edd3: header 0xd3d2d1d0 -> 0x1340=[+0xc8] 0x1344=[+0xcc] 0x1348=[+0xd0] 0x134c=[+0xd4]
9edfe: header 0xf3f2f1f0 -> 0x13c0=[+0xd8] 0x13c4=[+0xdc] 0x13c8=[+0xe0] 0x13cc=[+0xe4]
```

These are the only packets in `mtxpserd.dll` that name indices `0xd0..0xd3`
and `0xf0..0xf3` (`grep 0xd3d2d1d0|0xf3f2f1f0`: `0x9edd3`, `0x9edfe` only).

### 1.3 Callers [PROVEN]

| site | condition |
|---|---|
| `0x102d07` → `0x102d13` | per-draw validator: the FAA owner `[pDev+0xa9e4]` is another context; retire it, restore this one, store `[pDev+0xa9e4] = ctx` (`0x102d23`) |
| `0x9fe62` → `0x9fe6c` | `0x9fe10`, the same owner switch, called from the Z-init clear at `0xa0e01` before the Z-init load |

So "the eight 18-bit values" are fragment-unit state of the FAA
(`AAMode = 0x53`) path.  They are saved per context because the fragment
unit is a single device resource; they carry no per-tile Z-compression or
hierarchical-Z information.  `UseZCompInMultContextSundog` is read only at
`0x9f400` and `0x9f49a`, inside `0x9f3b0` (§3). [PROVEN]

---

## 2. The ZComp gates in `0xa0a60`

### 2.1 `[fmtinfo+4] == 3` is the format class "depth/stencil" [PROVEN]

`0xa0b4d: mov ecx,[esi]` loads the depth surface's format-info object.  These
objects are built by `0x86700` (0x90 bytes, tag `PARD`) and kept in the array
`[dev+i*4]`, count `[dev+0xc4]`, searched by `0x84ec0` (`cmp [edx],fmt`):

```
86708: mov esi,ecx ; mov [esi],fmt
8670d: call 0x84de0 (fmt) ; 86716: mov [esi+0x4],eax     ; format class
8671f: mov [esi+0x8],arg2 ; 86725: mov [esi+0xc],1
86735: call 0x19840 (fmt, usage 0..9, mask) -> [esi+0x18..0x3c]   ; per-usage table records
```

`0x84de0` is `table_byte[fmt-1]` at `0x84e60` through the jump table at
`0x84e40`:

| internal formats | class |
|---|---|
| `0x01..0x05` | 1 |
| `0x06..0x10`, `0x2f` | 2 (colour) |
| **`0x11..0x1c`** | **3 (depth/stencil)** |
| `0x1d..0x20` | 4 |
| `0x21`, `0x24..0x26` | 5 |
| `0x22`, `0x23` | 6 |
| `0x27..0x2b` | 7 |
| `0x2c..0x2e` | 8 |

The same class field selects the colour packer in the colour clear
(`0x9c69e: cmp [edx+4],2` → `0x9b900`; `cmp 4` → `0x9b860`) and the CPU fill
for compressed formats (`0x111c0e: cmp [eax+4],1`, 4x4 blocks).  The
`[fmtinfo+4] == 3` test is therefore redundant with the explicit format list
`0x13..0x19, 0x1b, 0x1c` before it.  `[fmtinfo+0x60]` is non-zero for formats
that carry stencil (used at `0xa0cee`, `0xa0d43`) [INFERRED from its use].

### 2.2 `(byte [this+0x12]) & 7 == 2` is "depth surface layout 2" [PROVEN]

`this` is the clear sub-object at `ctx+0x168` (`0xa1557: lea ecx,[ebx+0x168]`
→ `0xa1290` → `0xa13ac: mov ecx,esi` → `0xa0a60`).  The SetRenderTarget
handler fills `this+0xc..+0x1f` from the depth surface:

```
a040c: mov [state+0x1d8],ebx                  ; new depth surface
a041f: lea esi,[eax+0xf0]  (eax = [zsurf+0x28]) ; a0425: lea edi,[ebp+0xc] ; a0428: rep movs x5
```

and the decompress pass proves what `+0xf0` holds by programming slot 1 from
the same five dwords:

```
a40a4: lea esi,[eax+0xf0] ; a40af: lea edi,[esp+0x18] ; rep movs x5
a4147: mov [ebp+0xaa8],ecx   ; ecx = [esp+0x18] -> 0x1228 (address)
a416c: mov [ebp+0xab8],eax   ; eax = [esp+0x1c] -> 0x122c (configuration)
```

So `this+0xc` = depth address, `this+0x10` = depth configuration word, and
byte `this+0x12` & 7 = configuration bits **[18:16]**.  In the Linux HAL the
same field selects the memory layout (`MmSurfDesc::GetHeightGranularityREVB`
`0x74360`, `MmSurfDesc::GetLocalAlignmentAndGranularityREVB` `0x74170`:
`and edx,0x70000`), §6.3.  ZComp is armed only for a depth surface in layout
2, the 16-row tiled layout of the P-series.

The open driver programs `p3d_surface_cfg() = pitch | (2 << 16) | fmt << 24`
(Mesa `reference/p3d_state.c:321`), i.e. layout 2 with format `0x36` (S8Z24) or
`0x2a` (D16).  `0x36` corresponds to the eligible internal formats; D16
(internal `0x11`) is never compressed by the vendor.

### 2.3 Z-init exception for depth-only clears [PROVEN]

`0xa0cd5..0xa0cf3`: a full-surface Z-init clear normally requires
depth+stencil (`flags == 6`) or a format without stencil.  It is also allowed
for a depth-only clear when the surface has **never** been Z-init cleared
(`[zsurf+0x28]+0x368` bit 3 clear; the bit is set by every Z-init load,
`0xa0a2f: or [eax+0x368],8`): the first clear may clobber undefined stencil.

---

## 3. Z compression storage and limits

### 3.1 No extent test on the compression path [PROVEN]

Compression is armed in `0x9f3b0` case A2 (`0x9f3e3..0x9f453`: `0x20a`,
`0x9f160` force → `0x108`, `a670 = zsurf`) and re-armed in case B2
(`0x9f4c7..0x9f4f0`).  Neither reads the surface width, height or pitch.  The
only size test in the whole depth-clear family is the Z-init budget
(`0xa0d95: cmp edx,0x3c0000`), and when it trips the vendor clears
`a670`/`a674` only around the 2-D fill of the tail rows and then sets **both**
back to the surface (`0xa0dec`, `0xa0df2`), so a compressed surface larger
than the Z-init coverage stays compressed.

### 3.2 What the hardware keeps [INFERRED / OPEN]

Established facts: no allocation, no descriptor change (format `0x36` with or
without compression), the only commands are DWGCTL words, the first clear
after arming is a 2-D fill into ordinary memory, and an explicit pass
(`0xa0a` mode, depth NEVER, full surface) is needed before any non-Z-unit
access.  The consistent model is **in-place tile compression with on-chip
per-tile flags**: `0x108` marks every tile "uncompressed, valid in memory",
the Z unit writes tiles back in a compressed form inside their own 4 KiB slot,
and `0xa0a` re-expands flagged tiles.  Whether those flags share the 640-entry
array that holds Z-init state (then compression would silently stop at tile
640) is **[OPEN]**; probe P7 measures it.

### 3.3 Owner protocol: `0x00a` suspends, `0x20a` resumes [PROVEN code]

| site | action |
|---|---|
| `0x106eb0` (start of every 2-D operation, 42 callers, e.g. `0x112386`, `0x111c99`) | `0x106ed6: call 0x9f270(cmd,0,0)` → `0x00a` (cached at `0x1d3f2c`, emitted only on change; preceded by the `0x207` sync while ZComp is active, `0x9f2b4..0x9f31a`) |
| `0xa0110` SetRenderTarget | new Z ≠ `a674` and `a674` ≠ 0 → `0x00a`; new Z = `a674` → `0x20a`; same for `a670` (`0xa012e..0xa0181`) |
| `0x102c80` per-draw validator | owner not among this context's Z/RT (`0x9b7c0` compares `[ctx+0x1d8]`, `[ctx+0x1d4]`) → `0x00a`; owner present → `0x20a` (`0x102c94..0x102d88`) |
| `0x9f5b0` FAA retirement | `0x00a` before its pass, `0x20a` after, if tracking was on |

In none of these places is the owner pointer cleared, the surface resolved or
decompressed, or `0x12b4` reloaded.  The vendor therefore assumes that the
Z unit's per-tile state (Z-init and compression) **survives `0x00a`** and a
3-D draw on another depth surface made while tracking is off.  This does not
contradict `parhelia-p690-fast-depth-clear.md` §3: its "`0x00a` discards"
variant never re-armed, and its destructive "draw on a second depth surface"
variant drew with tracking still on. [INFERRED; probe P4]

---

## 4. Hierarchical, coarse and early Z

### 4.1 Search [PROVEN negative]

`strings -a` and `strings -el` over every vendor binary in the tree (59 files:
the whole `xddm32_210_00_105_se_u_nowhql` package including `MSerX`, all
`matroxdriver-x86_64-cvs-20090511` X servers, `libglx.so`, the kernel object)
for `hiz|hier|coarse|early|zcull|cull|zopt|fastz|zreject|zmin|zmax|fastclear|
colorinit|cinit|fclr|blkmode|blockwrite|colcomp|colorcomp` returns only
`CullNoneMode` (triangle face culling), `CullParameter*EXT` (GL entry points)
and C++ "Class Hierarchy Descriptor" RTTI.  The complete `mtxpserd.dll`
option namespace (strings `0xfa000..0xfb200`) contains no Z option besides
`EnableZinit`, `EnableZCompSundog`, `ForceDepthToS8Z24Sundog`,
`DivByZeroRefRastSundog`, the three ZComp workaround switches, `AdjustZ`,
`StereoZ` and `TnLZFactor`.

Registers: the vendor writes exactly these Z-related registers — `0x12b4`
(Z-init value), `0x12c0/0x12c4` (whole-draw Z-range clip), `0x12c8/0x12cc/
0x12d0` (test/stencil/write, fields up to bit 20, `parhelia-pixel-pipeline-
complete.md` §3.4) — plus the DWGCTL control words `0x008/0x108/0x208/0x308`,
`0x00a/0x20a/0xa0a`, `0x207`.  The 3-D draw DWGCTL builder
(`MTXPARD.dll` `0x151137..0x1511b7`) selects bits 19/20 by a mode test and bit
21 by the guard band (31.0 vs 15.0), nothing by depth state.

### 4.2 Z-init / ZComp state carries no depth range [PROVEN]

Z-init loads one value for the whole surface (`0x12b4`), and no packet ever
reads per-tile values back or loads a minimum/maximum.  Neither mechanism can
reject a tile without reading it.

### 4.3 What the fill measurement says [INFERRED]

`parhelia-p690-q3-performance-analysis.md`: colour-only 655, Z LEQUAL no
write 655, Z LESS reject 655, Z write 298 Mpixel/s.  Read-only Z costs nothing
and rejected fragments cost the same as written ones, so 655 is the raster /
fragment-pipe peak, not a memory limit.  That measurement cannot separate
early from late rejection; a fragment that is expensive *before* the depth
test (trilinear texturing, 116 Mpixel/s) can — probe P1.

### 4.4 Probe P1 result: rejection is late [MEASURED]

`PROBE_TEX_Z=reject parhelia-drm-test --texture-fill-probe` (Mesa
`p690-zre` c198b03) clears the Z24 target to 0 and draws the textured
full-screen quads at z = 0.5 with depth LESS and no depth write, so every
fragment fails the depth test.  nano, 1920x1200 scanout in bank A,
256x256 texture with its mip chain, 20 quads per case, Mpixel/s:

| sampling | depth test + write, all pass | depth LESS, all rejected |
|---|---:|---:|
| 1:1 nearest | 199.9 | 456.0 |
| 1:1 bilinear | 183.9 | 374.5 |
| 2:1 minified, no mips | 83.1 | 114.0 |
| 4:1 minified, no mips | 26.1 | 28.6 |
| 16:1 bilinear mip-nearest | 294.7 | 655.4 |
| 1.5:1 trilinear | 77.3 | 98.5 |

The untextured reject rate on the same setup is 658.7 Mpixel/s.  The rejected
textured rate follows the sampling cost instead (28.6 at 4:1 without mips,
98.5 trilinear), so a rejected fragment still pays for its texture fetch and
the rejection only saves the colour and depth writes.  The P690 has no early
Z: a depth pre-pass or front-to-back sorting would not remove the texture
work that bounds the Quake 3 frame.

---

## 5. Colour clears on the P-series

### 5.1 The D3D colour clear is a 2-D fill [PROVEN]

`0xa14e0` (Clear entry, callers `0x16eb08`, `0x17ef30`) calls `0x9c670` on
`ctx+0x194` for `D3DCLEAR_TARGET` and `0xa1290` on `ctx+0x168` for depth.
`0x9c670` packs the colour by format class, clips each rectangle to the
viewport and calls the fill wrapper:

```
9c877: push 0xc0                          ; operation
9c881: push [[ctx+0x2c]+0xa88]            ; current PLNWT when ctx+0x4c, else
9c88a: push 0xffffffff                    ;   all planes
9c896: call 0xa1f20 -> 0x115ad0 -> 0x1122d0
```

No path in the colour clear touches DWGCTL `0x00a/0x20a/0x008`, `0x12b4` or
any other "cleared tile" state.  There is no colour equivalent of Z-init.

### 5.2 The 2-D fill emitter `0x1122d0` [PROVEN]

```
112397: mov eax,0xc0 ; 11239e: [req flags & 1] -> eax = req.op (0xc0 from every clear)
1123a1: or  eax,0x500                         ; DWGCTL = 0x000005c0
1123b0: bpp switch -> FCOL replicated (8: x4, 16: x2, 32: as is)
112479: call [[a67c+0x3f4]].get   ; 11247c: test al,2 -> skip strips   (DirectX.DdFlags, default 0)
112480: lea eax,[ebp+0x1f] ; and eax,0xffffffe0 ; sub eax,ebp   ; first strip to the next 32-px column
11249c: if 0: min(32, w)
1124d9: [blk+0xa84] = DWGCTL ; 1124cc: [blk+0xa94] = FCOL ; 1124e9: [blk+0xa88] = PLNWT
1124f9: [blk+0xaa0] = w0 | h<<16 ; 112505: [blk+0xa98] = x | y<<16
11252f: [blk+0xaa8] = dst address (0x1228) ; 112535: [blk+0xab8] = dst cfg (0x122c)
112542: call 0x30010                          ; state + first strip
112580..1125df: loop  header 0x2121e3c2: 0x1308 = x | y<<16, 0x138c = 32 | h<<16 ; x += 32
112630: remainder strip (x_end - x) | h<<16
11268b: unless req flags & 2: [0xc0afafc0] 0x1300=0x307, 0x12bc=tag|1, 0x12bc=tag, 0x1300=normal
1126c1/112732: if OGL.FastContextSwitching: 0x1148 = 0 ... kick ... 0x1148 = 1
```

So every vendor 2-D fill on the P690 is drawn as a series of **32-pixel-wide,
full-height column strips** aligned to 32-pixel columns, each a separate
`DSTXY`/`LEN` pair under one `DWGCTL = 0x5c0` (opcode 0, BOP GXcopy, source
and destination slot 1), followed by a `0x307` drain and fence.  The same
loop exists in `MTXPARD.dll` (`0x1105bd..0x110780`, same `DdFlags & 2` test),
i.e. on Parhelia-512 as well.  The strip width is 32 pixels at every depth;
at 32 bpp that is half a 64x16 tile, matching the 512-byte half-tile
interleave of `parhelia-p690-mip-chain-half-tiles.md` [INFERRED motive].
`DirectX.DdFlags` is created at `0x10d01e..0x10d03e` with default 0 and
validated into `+0x3f4` at `0x90e2f`.  `OGL.FastContextSwitching` (default 0)
is created at `0x10cd18..0x10cd38` and validated into `+0x3c0` at `0x91fd9`;
this identifies the option behind the `0x1148` bracket of the decompress pass
(`0xa4241`, `0xa42ac`).  The meaning of `0x1148` itself stays [OPEN].

For surfaces of memory class 3/4 (`[surf+0x90]`) only a whole-surface fill
rounded up to the width/height granularity (`0xa50c0`, `0xa5580`) goes to the
engine; partial fills there go to the CPU fill `0x111bb0` (lock and store).
The same rounding appears in `0x115b29..0x115bc5`.  Ordinary VRAM render
targets take the engine path for any rectangle. [PROVEN]

### 5.3 No block-write mode [PROVEN negative]

The P-series DWGCTL has no atype field: bits [7:4] are the BOP
(`parhelia-2d-accel-complete.md` §4).  Every colour and depth fill request in
the clear family carries operation `0xc0` (`0x9c877`, `0xa1498`, `0x9f467`,
`0x9f51e`, `0xa0db5`); the only DWGCTL literals stored into the 2-D shadow
`+0xa84` anywhere in `mtxpserd.dll` are `0x2c2`, `0x30000c`, `0x3000cc`,
`0x2000c1`.  The Linux `DeviceBitmap::ClearSurface` (`mtx_drv.so` `0x7c8f0`)
uses `DWGCTL = 0x00000cc0` (slot 3) with one rectangle and no strips.  No
pitch/alignment/bpp condition selects a different fill mode.

### 5.4 Fastest colour clear available [INFERRED]

Measured on this card: 2-D fill 468 Mpixel/s (32 bpp) / 771 (16 bpp); 3-D
colour-only quad 655 Mpixel/s (32 bpp).  A full-screen 3-D quad with depth
test and depth write off, constant colour and `0x1324` = colour mask is
expected to beat the 2-D fill at 32 bpp by ~40 % (probe P3), and the vendor
strip order may lift the 2-D fill itself (probe P2).

---

## 6. Other per-pixel traffic reducers

### 6.1 `MemLevels` `0x12b0` [PROVEN values, INFERRED fields]

| value | source | [4:0] | [9:5] | [14:10] | [19:15] | [24:20] | [29:25] |
|---|---|---|---|---|---|---|---|
| `0x3fff8842` | default (`mtxpserd` `0x10be83`, `mtx722` `0x7653f`) | 2 | 2 | 2 | 31 | 31 | 31 |
| `0x3fff9084` | 2-D ops > 250 000 px (`mtxpserd` `0x10fc5f..0x10fc6c`, `0xa77df..0xa780c`) | 4 | 4 | 4 | 31 | 31 | 31 |
| `0x3fff8000` | engine idle (`0x2d96a`; Linux `mtx398` `(cap?0:0x1f)<<15 \| 0x3ff00000`) | 0 | 0 | 0 | 31 | 31 | 31 |
| `0x3ffe0000` / `0x3ffd0000` / `0x3ffc8000` / `0x3ffa0000` / `0x3ff68000` / `0x3ff00000` | clock ladder (`0x2d948..0x2d960`, MTXPARD `0x2c49a..0x2c53a`) | 0 | 0 | 0 | 28 / 26 / 25 / 20 / 13 / 0 | 31 | 31 |

All nine values are exactly six 5-bit fields, bits 31:30 always 0.  Only the
low three fields change with operation size and only `[19:15]` with clock,
consistent with per-client FIFO watermark/priority levels.  The 250 000-pixel
switch in `mtxpserd.dll` is in the 2-D blit/stretch paths, not in the 3-D
draw path.  Measured: `0x3fff8842` vs `0x3fff9084` showed no difference on 3-D
fill (`parhelia-p690-q3-performance-analysis.md`).

### 6.2 `TEDLFCounters` `0x0bc0` [PROVEN, no new decode]

`0x1e4087` for `caps & 0x7fc00` (`mtxpserd` `0x10c4e9`), `0x4087` otherwise
(`0x10c513`).  Already measured without effect.

### 6.3 Surface layout field `cfg[18:16]` [PROVEN, Linux HAL]

`MmSurfDesc::GetHeightGranularityREVB` (`0x74360`) and
`GetLocalAlignmentAndGranularityREVB` (`0x74170`), local memory, Sundog class
(`[desc+0x10] & 0x7fc00`):

| layout | height granularity | local alignment, 32 bpp (arg 0 / 1) | reading |
|---|---|---|---|
| 0 | 1 row | `0x40` / `0x200` (any bpp) | linear |
| 1 | 2 rows | `0x100` (8 bpp `0x40`, 16 bpp `0x40`/`0x80`) | 2-row pairs |
| 2, 3 | **16 rows** (32 on 0528) | `0x800` / `0x1000` (halved for Sundog from `0x1000`/`0x2000`) | 4 KiB tiles, the render/texture layout |
| 4 | 32 rows | `0x400` / `0x1000` (halved) | 32-row tiles |
| 5 | 32 rows | 4 bpp only, `0x100` / `0x400` (halved) | 32-row tiles, compressed textures |

Non-local surfaces always use 16 rows.  Layouts 2 and 3 share geometry; the
2-D driver creates surfaces as 3 and rewrites to 2 when binding as a source
(`parhelia-2d-accel-complete.md` §3), and the vendor's own depth helper / FAA
buffer uses 3.  ZComp requires 2 (§2.2).  No layout carries a compression or
fast-clear attribute.

### 6.4 Colour compression, early alpha, render-target cache [PROVEN negative]

No colour-compression, colour fast-clear, early-alpha or render-target-cache
option, register or DWGCTL word exists in the P-series binaries (§4.1 search;
the only `*Compress*` options are `OGL.TextureCompress` and the FAA
`ForceCompressedMask`).  FAA (`AAMode`) adds traffic; it is not a reducer.
DCM priorities and `0x1c24` were already measured without effect
(`parhelia-p690-q3-performance-analysis.md`).

---

## 7. Why Z compression ships disabled

1. Default 0 on every chip and OS: `mtxpserd.dll` `0x10bb26`, `MTXPARD.dll`
   (same source), `mtx_parhelia.o` `mtx722`, `mtx_drv.so` `0x610a9`
   (`xor r9d,r9d`).  No INF sets it (`mtxpser.inf`, `mtxpar.inf` only set
   `3D.D3DEnable`).  No revision test exists on any ZComp path: the gates are
   `caps & 0x7fc00` (all Sundog-class parts `2537/2538/2539`, `0x9f278`,
   `0xa139b`), the format list, the format class and layout 2. [PROVEN]
2. The Linux stack never reads it. [PROVEN, earlier document]
3. Correctness gaps in the shipped code [PROVEN code facts]:
   * The CPU-lock decompress tests only internal format `0x13`
     (`0xf1f55: cmp [eax],0x13`) while compression is armed for `0x13..0x19,
     0x1b, 0x1c` (`0xa0b51..0xa0b79`).  An X8Z24 or partial-stencil surface
     would be locked compressed.
   * The decompress pass `0xa4070` has exactly four callers (`0x9f4b1`,
     `0x9f50d`, `0xa4ae8`, `0xf1f6e`): two clear paths, teardown and the lock.
     Nothing decompresses before a depth surface is bound as a texture or
     used as a blit source/destination.
   * Two of three workaround switches (`Use2DClearForceZUncompSundog`,
     `ZUncompAnd2DClearSceneInProgSundog`) exist to replace in-scene partial
     clears with decompress + 2-D fill, i.e. the 3-D partial clear under
     compression had problems worth a switch.
4. Hence the feature is best read as unfinished rather than broken silicon
   [INFERRED]; nothing in the binaries records an erratum.

---

## 8. Hardware probe proposals

Conventions as in the earlier documents: Z S8Z24 in slot 1, `0x122c =
0x36020000 | pitch` (layout 2), every control word sent as the pair
`DWGCTL = word; DWGCTL = normal` inside one register list, verify by GPU copy
and scanout, never raw BAR reads.  Respect the first-textured-draw warm-up
rule and reboot after any stall.  Ordered by risk, then expected value.

### Low risk (established encodings only)

**P1 — early or late Z rejection** (decides whether Z pre-pass / front-to-back
pays off for Q3).  1280x1024, 256x256 32-bit mip texture, trilinear 1.5:1
(baseline 116 Mpixel/s).  (a) Fill Z with a near plane (z = 0.1) through the
depth pipe; (b) draw N trilinear full-screen quads at z = 0.5 with
`0x12c8` depth LESS, write off.  Expected: ~116 Mpixel/s if rejection is
after texturing, toward 655 if before.  Repeat with alpha test enabled
(`0x12e4`), which normally forces late Z on other GPUs.

**P2 — 2-D fill strip order.**  Same 1280x1024 32 bpp and 16 bpp fills,
`DWGCTL = 0x5c0` (slot 1) or the current `0xc0` form: one `0x1308/0x138c`
pair vs the vendor sequence (first strip to the next 32-px boundary, then
`0x138c = 0x20 | h<<16` strips, then the remainder).  Expected: strips ≥
single rectangle; baseline 468 / 771 Mpixel/s.

**P3 — 3-D colour clear.**  Full-screen quad, `0x12c8` test off, `0x12d0`
depth write off, constant colour, `0x1324` = colour write mask, compare to
P2's best 2-D fill at 32 and 16 bpp.  Expected ~655 vs 468 Mpixel/s at 32
bpp.

**P4 — Z-init survives `0x00a`.**  On a ≤ 640-tile Z (A):
`0x20a`; `0x12b4 = v, 0x208`; `0x00a`; bind Z (B) and draw a depth-tested
scene on B; rebind A; `0x20a`; draw the `--zinit-probe` bands on A; resolve;
compare with the 2-D cleared reference.  Variant without the rebind draw.
Expected per the vendor protocol (§3.3): identical.  If it holds, multi-client
arbitration reduces to the kernel emitting `0x00a` at the start of any batch
whose depth relocation is not the armed surface and `0x20a` when it is.

**P5 — `MemLevels` field sweep (low-medium).**  Keep `[29:15]` at vendor
values; low three fields 2, 4, 8, 16 together; measure 2-D fill, Z-write fill
and blend.  Only vendor-shaped values; low expected value (0x842 vs 0x1084
already equal).

### High risk (never exercised on this card)

Before any of these the kernel must accept the vendor sync (`0x1300 =
0x207`, `0x12bc = tag|1`, `0x12bc = tag`); today `0x12bc` is kernel-owned.

**P6 — Z compression on a ≤ 640-tile surface.**  640x1024 S8Z24.  Sync;
`0x20a`; sync; `0x108`; 2-D fill clear (vendor order); depth test+write fill
test (baseline 298 Mpixel/s); sync; `0xa0a`; full-surface quad with `0x12c8 =
0x00104991`, `0x12cc = 0xff`, `0x12d0 = 0x100`, `0x1324 = 0`, z = 0,
`DWGCTL = 0x0030000c`; sync; `0x20a`; `0x00a`; GPU-copy Z and compare
bit-exact with the same scene without compression.  Expected win: Z-write
fill above 298 Mpixel/s.  Risk: `0xa0a` mode and the `0x207` sync are new on
this card.

**P7 — compression coverage.**  P6 on 1280x1024 S8Z24 (1280 tiles); compare
tiles 640..1279 separately.  Decides whether compression shares the 640-tile
array (§3.2).

**P8 — compression + Z-init (`0x008` load).**  After P6/P7: `0x20a`, `0x108`,
`0x12b4 = v`, `0x008`, scene, decompress, compare.

---

## 8a. Hardware results (nano, 2026-10-03) [MEASURED]

All on the P690 in nano (VIA Nano X2, 1920x1200 scanout in bank B since
kernel e630cb7), Mesa `p690-zre`.

**P1, early Z:** none; see section 4.4.  A depth-rejected textured fragment
pays for its texture fetch.

**P6, Z compression rate** (`PROBE_ZCOMP=1|2|3 parhelia-drm-test
--fill-rate-probe`, Mesa 99cf1b2).  Forms tried: arm `0x20a` then the
compressed reset `0x108` after the depth clear (1); the same followed by
the vendor's post-arm 2-D clear (2); arm, then the clear loaded as
compressed Z-init, `0x12b4` with DWGCTL `0x008` (3).  Each control batch
was fenced in place of the `0x207`/`0x12bc` sync, then `0x308`, `0x00a`.
No form locked the engine.  Every depth mode ran at exactly the
uncompressed rate, in Mpixel/s:

| target | Z LEQUAL+write | Z LEQUAL no write | Z LESS reject |
|---|---:|---:|---:|
| Z24, any form | 232.4 | 520.1 | 520.1 |
| D16, any form | 394.8 | 668.7 | 689.8 |

Either the words do not engage compression without a state the vendor
sets elsewhere, or compression does not reduce this memory traffic.  The
line was closed with no driver change because it cannot help the Quake 3
frame in any case: with the textured fill probe, 1.5:1 trilinear runs at
116.5 Mpixel/s with and without a depth buffer (2:1 minified 120.3 both).
Only cheap fragments pay for Z traffic (1:1 bilinear 262 -> 230, 16:1
trilinear 655 -> 230).  A capture of the Windows driver with
`3D.EnableZCompSundog=1` would show whether more state is needed.

**Z-init beyond the coverage:** the vendor's split, Z-init for the whole
tile rows inside the 640 measured tiles plus a 2-D fill of the rest, is
exact.  On a Z24X8 buffer it takes ioquake3 from 22.9/23.0 to 24.7/24.7 fps
(1280x1024, depth-24 visual, frame without lightmaps; Mesa 56e8dbc).

**Display bank:** scanning a dumb buffer out of bank A cost textured fill
about 30% (1:1 bilinear 182 vs 262, 1.5:1 trilinear 77.6 vs 116.5).  With
the scanout in bank B, depth-write fill drops (Z24 LEQUAL+write 304 -> 232),
since depth now shares bank B with display refresh.  Quake 3 gained 17%
(kernel e630cb7).

## 9. Still open, and what would settle it

| item | settles it |
|---|---|
| Early vs late Z rejection | probe P1 |
| Whether `0x00a` preserves Z-init/ZComp tile state | probe P4 |
| Where compression flags live, and their coverage | probes P6/P7; a Windows XP P690 capture with `3D.EnableZCompSundog=1` running a D3D depth test would show the vendor's exact stream including sync tags |
| Meaning of the six `MemLevels` fields | P5 plus a capture on a P690 at different GCLK |
| Meaning of `0x1148` (`OGL.FastContextSwitching` bracket) | Windows capture with that option set |
| Meaning of the eight FAA state words `0x1340..0x134c`, `0x13c0..0x13cc` | Windows capture with `AAMode = 0x53` (unchanged from `parhelia-antialiasing.md` §9a.6) |
| Which layout the vendor gives D3D depth buffers (2 or 3) | the descriptor builders behind `0xc2060` (callers `0xc2bfe`, `0xc2eaa`, `0xc3037`), or a Windows capture of `0x122c` |

---

## 10. Reproduction

```sh
cd xf86-video-parhelia
D=xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll
P=xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
M=matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so
dis() { objdump -d -Mintel --start-address=$2 --stop-address=$3 "$1"; }
sha256sum $D $P $M matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o \
  xddm32_210_00_105_se_u_nowhql/mtxpser.inf xddm32_210_00_105_se_u_nowhql/mtxpar.inf

# 1. FAA retirement / restore and their callers
dis $D 0x9f5b0 0x9f993; dis $D 0x9ed40 0x9ee30; dis $D 0x9f9a0 0x9fab0
dis $D 0x102c80 0x102da0; dis $D 0x9fe10 0x9fea0
objdump -d -Mintel $D | grep -E '0xd3d2d1d0|0xf3f2f1f0'

# 2. ZComp gates, format class, layout source
dis $D 0xa0a60 0xa0e90; dis $D 0x84de0 0x84e90; dis $D 0x86700 0x86760
dis $D 0xa0400 0xa0450; dis $D 0xa4070 0xa4180; dis $D 0xa1290 0xa1300; dis $D 0xa14e0 0xa1570
python3 -c "d=open('$D','rb').read(); print(d[0x84e60-0x10000:0x84e60-0x10000+0x2f].hex())"

# 3. arming without extent test, owner protocol
dis $D 0x9f3b0 0x9f5a9; dis $D 0x9f270 0x9f3a0; dis $D 0xa0110 0xa0190; dis $D 0x106eb0 0x106ee0
objdump -d -Mintel $D | grep -c 'call   0x106eb0$'

# 4. negative search
for f in $(find . \( -iname '*.dll' -o -iname '*.sys' -o -iname '*.o' -o -iname '*.so' \) \
           -not -path './src/*' -not -path './tools/*' -not -path './m9xdriver*'); do
  { strings -a $f; strings -a -el $f; } | grep -i -E \
    'hiz|hier|coarse|early|zcull|cull|zopt|fastz|zreject|zmin|zmax|fast.?clear|colorinit|cinit|fclr|blkmode|block.?write|colcomp'
done | sort -u
dis $P 0x151130 0x1511c0

# 5. colour clear and the 2-D fill emitter
dis $D 0x9c670 0x9c8c0; dis $D 0xa1f20 0xa1f90; dis $D 0x115ad0 0x115bf0
dis $D 0x1122d0 0x112780; dis $D 0x111bb0 0x111ed0; dis $P 0x110580 0x110790
dis $D 0x10cd00 0x10cd40; dis $D 0x10d000 0x10d040; dis $D 0x90e19 0x90e35; dis $D 0x91fc3 0x91fdf
objdump -d -Mintel $D | grep -E 'mov    DWORD PTR \[e..\+0xa84\],0x'
dis $M 0x7c8f0 0x7cae4

# 6. MemLevels, TEDLFCounters, layout tables
objdump -d -Mintel $D | grep -E '0x3d090|0x3fff9084|0x3fff8842|0x3fff8000|0x1e4087'
dis $D 0x2d900 0x2d9a0; dis $D 0x10fc40 0x10fc80; dis $D 0xa77c0 0xa7810
dis $M 0x74170 0x74490
python3 -c "
for v in (0x3fff8842,0x3fff9084,0x3fff8000,0x3ff00000,0x3ff68000,0x3ffa0000,0x3ffc8000,0x3ffd0000,0x3ffe0000):
  print(hex(v), [(v>>(5*i))&31 for i in range(6)])"

# 7. lock-path decompress format test, decompress callers, INF keys
dis $D 0xf1f40 0xf1f80
objdump -d -Mintel $D | grep -E 'call   0xa4070$'
grep -i -E '3D\.|DirectX\.|OGL\.' xddm32_210_00_105_se_u_nowhql/mtxpser.inf xddm32_210_00_105_se_u_nowhql/mtxpar.inf
```
