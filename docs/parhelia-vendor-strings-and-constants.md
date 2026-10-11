Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# Parhelia vendor binaries — strings, symbols, tables and constants (mining pass)

Every vendor binary in the tree was swept with `strings -a -n 4 -t x`,
`strings -a -el`, `nm`/`readelf`/`objdump -p`, and python scanners for
12-byte WARP words, register-tag tables, DWGCTL literals and float pools.
Scratch: `~/tmp/strings-mine/` (per-binary string dumps `str/*.a.txt` /
`*.u.txt`, `kernel_blob_inventory.txt`, `board_table.txt`, `parm_boot.dis`,
`blobscan.py`).  Only binaries were read; no proprietary C source.

Tags: **[BIN binary @offset]** = file offset in the named binary; **[INF]** =
inferred; **[HW]** = from `captures/live-ring-0528/`.  Offsets are *file*
offsets unless marked VMA.

Binaries covered (30): `mtx_parhelia.o`, `kernel-orig-56/mtx.ko`,
`xserver/{6_8_2,7_0_0..7_4_99}/{mtx_drv,libglx,libmtxcfg,v4l_drv}`,
`opengl6/{lib64/,}libGL.so.1.3.0`, `MtxPar/{MTXPARD.dll,MTXOGL.dll,MTXPARM.sys,
mtxparmx.sys,MtxEscape.dll,MtxCIP2.dll}`, `MtxPser/*`, `MSerX/*`, `MTXCI.DLL`,
`P690.DLL`, `Extio2.dll`, all `.inf/.ini/.cfg`, `vbios-matrox-parhelia`,
`tools/p690.rom`.

---

## 1. New facts not in any existing doc (ranked by value for our own triangle)

### 1.1 Every captured live WARP instruction exists verbatim in `mtx_parhelia.o` `.data`; the canned base-register / constant-address programs are complete blobs at known offsets

All **146 unique 12-byte instructions** of the eight captured slots
(`captures/live-ring-0528/warp-programs/slot-*.txt`) are found byte-for-byte
in `mtx_parhelia.o` (138 of them also in `MTXPARD.dll`, 5 in `MTXPARM.sys`,
0 in `MTXOGL.dll`/`libGL`).  [BIN, scan `blobscan.py`]

The programs that use the ops the ISA doc calls unknown (`ctl33`, `ctl35`,
`ctl05`, `ctl27`, `ctl2b`, `ctl0d`, `sop8`, `op6e`, `mova` with `b1`/`a0`)
are **whole canned programs in the anonymous tail of `.data`** (no ELF
symbol covers `0x150190–0x164d80`; the docs so far cite only `0x161d38` and
`0x1630a0` there).  END-delimited table, kernel file offsets
(`.data` VMA = off − 0xdce00 + 0x14be0):

| start | end | instr | first triplet | matches captured slot (run) | what [INF] |
|---|---|---|---|---|---|
| 0x160ce0 | 0x160d4c | 9 | `0008020f 00440200 b01a21a2` | – | mov-template stub |
| 0x161cc0 | 0x162f68 | 398 | `00200200 00040000 2080e000` | – | big control program; **dword-identical to `MTXPARD.dll` @0x192470** [BIN] |
| 0x162fa0 / 0x163020 / 0x1630a0 / 0x163120 | +0x6c | 9,9,9,9 | `00003e00 00440000 38a36e22` | – | stage program family, 4 copies |
| 0x1631a0, 0x163240, 0x163300, 0x1634a0, 0x1635a0, 0x1636c0, 0x163920, 0x163a80, 0x163c00 | | 11,13,15,19,21,23,27,29,31 | `00043e00 00440000 38a36e22` | – | same family, +2 instr per step (texcoord-count variants [INF]) |
| 0x1633b4, 0x1637d4, 0x163d74 | | 18, 26, 34 | NOP-led | – | interleaved variants |
| **0x163f20** | 0x1641c0 | 56 | `0008013f 30443000 301a21a2` | – | long form of the `sop8/ctl33/mova/ctl42/br` program |
| **0x1641e0** | 0x16427c | 13 | same | **16c05600 (13/13)** | the live "kind 3" program (`sop8 r3; ctl33; mova T1.x/.y; ctl42; br; …`) |
| 0x1642a0 | 0x164330 | 12 | same | – | short form |
| **0x1643c0** | 0x164480 | 16 | `01d00600 00040000 19800000` | **16c05000 (16/16)** | six `ctl33` + `mova T0..T2` + `ctl0d imm=-1` + `op6e` — the base-register installer |
| 0x1644a0 | 0x164554 | 15 | `0028001f 00440200 305a2000` | 16c05c00 (6) | mova-T# prologue variant |
| 0x164554 | 0x164680 | 25 | NOP-led | 16c05c00 (14) | |
| **0x1646a0** | 0x1647f0 | 28 | `0038005f 00440200 305a2000` | **16c05c00 (28/28)** | `mova T0..T7 ← v0/v1`, six `ctl33`, **`ctl35 imm=0xe8,0xec,0xf0,0xf4,0xf8,0xfc`**, `ctl05` — the constant-address program |

The same three live programs in the Windows display driver:
`MTXPARD.dll` @0x190d50 (28 instr = kernel 0x1646a0), @0x1910a0 (16-instr
run = kernel 0x1643c0), @0x190ea0/@0x190fd0 regions (13/14-instr runs of the
kind-3 program). [BIN MTXPARD.dll]  Miniport copy of the mova prologue:
`MTXPARM.sys` @0x627b8 (`0008000f 40440200 305a2092 …`). [BIN]

Vendor transform program of the live draw (slot 16c06200, `smul`/`sop2`
lines) lives at kernel **0x15d6ac–0x15dab8** and its `bb9a21a2` family at
**0x1608ac–0x1608d0** — also in the anonymous tail. [BIN]

Clipper/stage instructions of slots 16c02000/16c03e00/16c06800 come from the
*symbol-covered* template region (`mtx2101` @0xe8380, `mtx3596` @0xebd80,
`mtx749` @0xe5f00, `mtx1571` @0xe4cc0, `mtx1873`/`mtx1003`/`mtx1451`
@0x14a720–0x14a92c, `mtx1699` @0x147a7c, `mtx2650`/`mtx897`/`mtx943`/`mtx2869`
@0x14c3f0–0x14c448, `mtx1376` @0x109cc0).  Full map: `kernel_blob_inventory.txt`.

**Value:** the exact vendor bytes of every canned program that installs
`b0..b3`, `a0` and the `0xe8..0xfc` constant addresses can be lifted from
these offsets without decoding a single field (see §3 contradictions for
the `--sym` names the decoder should target).

### 1.2 Negative result: no vendor binary names the WARP control ops, base registers or `imm` fields

| looked for | result |
|---|---|
| `warp` (any case), ASCII or UTF-16, all 30 binaries | **0 hits** |
| `ctl`, `sop`, `smul`, `mova`, `b0..b3`, `a0.x`, `T0..T7` mnemonics | none except D3D-level `c[a0.x + %d]`, `a%d`, `r%d`, `v%d`, `c%d`, `oPos/oFog/oPts/oT%d/oD%d` in the **D3D vs.1.x token disassembler** of `MTXPARD.dll` [BIN MTXPARD.dll @0x180910, @0x1807e0–0x180814, @0x180790 `vs.%d.%d`, swizzle table @0x1805d4–0x180618] |
| classic MGA register names (`DWGCTL`, `PLNWT`, `SECADDRESS`, `SOFTRAP`, `DWGSYNC`, `MACCESS` …) | **absent from every binary** (only `HSLPAR_Register_PriSurfAddr_Get/Set`, `HSLPARSetDEMREQCTLtoDCMA/B`, `HSLPARSetDEMVGAHCntMode` as function names) |
| opcode/mnemonic tables | only the Matrox VS macro table `??? slt sge rsq rcp nop mul mov min max mad m4x4 m4x3 m3x4 m3x3 m3x2 logp log lit frc expp exp dst dp4 dp3 add mdp3 mindex mfrac mmult mexp mlog mwrmask mswizzle mextract minsert mfloor mround mcross mclamp mpow` [BIN MTXPARD.dll @0x18f124–0x18f227, handler-pointer table @0x18f0c0] (already in `parhelia-vertex-isa.md`) |
| M-series (`MxMD.dll`, "phoenix") | a **predicate-prefix table** `(p0) (p1) (p2) (p3) (p4) (p5) (!p0)…(!p5)`, 48-byte entries `{3, mask 1<<n, 0, "(pN) "[16], "(pN) "[16]}` [BIN MxMD.dll @0x1df9cc…] — a *different* ISA (M91xx), not Parhelia; plus registry names `PsBmMicrocode`, `PsBmConstants`, `CeBmMicrocode`, `CeBmConstants`, `RESET_FRAG_PTR`, `EAR30PSCTL2/EAR30PSSYNC/EAR98` [BIN MxMD.dll @0x1d76xx, @0x1e4924] |

So the `ctl33/35/05/27/2b/0d`, `sop2/8`, `op6e` semantics must keep coming
from code paths and hardware, not from text.

### 1.3 The miniport self-test image is the vocabulary of the unknown control ops

`MTXPARM.sys` file **0x627b8–0x637f0** (VMA 0x727b8, see §3), 346
instructions to the first END, decoded with `tools/parhelia-warpdis2.py`
(`~/tmp/strings-mine/parm_boot.dis`):

| mnemonic | count | | mnemonic | count |
|---|---|---|---|---|
| ctl05 | 71 | | ctl0a | 8 |
| ctl06 | 66 | | nop | 6 |
| ctl43 | 39 | | br | 6 |
| ctl41 | 38 | | ctl33 | 5 |
| ctl04 | 37 | | mov | 3 |
| ctl42 | 32 | | ctl02 | 2 |
| brn | 32 | | end | 1 |

Structure: `mov T0/T1/T2 ← v0.yyyy/.zzzz/.wwww`; five `ctl33` (operands
`00040004,00040000,00040100,00040104,00040108`); then repeated blocks
`ctl43 imm=0xff/0xff00/0xff0000/0xff000000 ; br → 347 (fail exit)`, and
loops `ctl43 → ctl42 imm=0x20 → ctl41 imm=<32-bit pattern e.g. 0xa7468c4e,
0x822fe4ed> → ctl06 ×2 → ctl04 → brn back`.  Tail rows
`10000501 000f0000 05000000 … 48000518 000f0000 05000000` (8 rows, stride
0x08 in w0 bits 27:24, 0x01/02/04/08/11/12/14/18 in w0 low byte) precede END
[BIN MTXPARM.sys @0x63784–0x637e0].  [INF] this is a memory/latch
walking self-test: `ctl41` loads a pattern, `ctl42` a count, `ctl43` a mask,
`ctl06/ctl04/ctl05` step, `brn` loops — a ready-made corpus for
differential analysis of those ops.  Second mini-image: `0208010f 00440200
305a2249 / 0218011f …` + END at 0x637fc–0x63820.

### 1.4 The kernel template library, by ELF symbol

150 `.data` OBJECT symbols between 0xe4cc0 and 0x139000 contain WARP words;
the most-hit by the live capture (hits = distinct captured instructions
found inside):

| symbol | file off | size | hits | symbol | file off | size | hits |
|---|---|---|---|---|---|---|---|
| mtx749 | 0xe5f00 | 0x14dc | 17 | mtx3928 | 0x130b60 | 0x10bc | 27 |
| mtx1066 | 0xed240 | 0xffc | 21 | mtx3387 | 0x12fbe0 | 0xf6c | 26 |
| mtx2369 | 0xee240 | 0xd08 | 21 | mtx1343 | 0xf8a00 | 0x10ec | 26 |
| mtx457 | 0xefc80 | 0xd20 | 25 | mtx3733 | 0xfc6a0 | 0xb34 | 26 |
| mtx775 | 0xf09a0 | 0xd20 | 21 | mtx2164 | 0x12b460 | 0x1008 | 26 |
| mtx3596 | 0xebd80 | 0x14ac | 17 | mtx847 | 0x135ba0 | 0xd08 | 26 |
| mtx2101 | 0xe8380 | 0x10c8 | 8 | mtx1376 | 0x109cc0 | 0x13c8 | 25 |
| mtx1610 | 0xe9460 | 0x10c8 | 5 (has END @0xe9760) | mtx3597 | 0x10c240 | 0xf54 | 25 |

Only `mtx1610` (@0xe9760) and the anonymous tail contain END words; every
other symbol is a *fragment* table (no END) — i.e. the kernel assembles
programs from fragments and only the tail holds complete programs. [BIN]
Full 150-row table: `~/tmp/strings-mine/kernel_blob_inventory.txt`.

### 1.5 Kernel float constant pool (`.rodata.cst4/8/16`) and its Windows twin

`mtx_parhelia.o` `.rodata.cst4` @0xc7f04–0xc8130 (unique values, in order):
`-0.5 -0.0625 0.5 0.0625 0.25 0.249023(3e7f0000) 1024 1 -1 16777215 255
0.015625 0 1023 32768 65536 16777216 1048576 786432 1280 1.5259e-05 π 180 16
128 0.00456621(3b95a025) 0.00625893(3bcd17b6) 0.00392157(=1/255)
0.00153632 0.00318811 0.00791071(3c019bec) 65535 2048 0.75 4 2 -16 2047
1.06383 2.38e-07 15 -4 0.046875 8160 -8160 6144 100 8 1.4427(log2 e) -6 24
-120 720 -5040`.  `.rodata.cst8`: `1 1 1 1 0 0 2 3 4 5 6 7 2 3 4 5 6 7`.
`.rodata.cst16`: sign-mask vectors `80000000 0 0 0`, abs-mask `ffffffff
7fffffff 0 0`. [BIN]
The triple `3c019bec 3bcd17b6 3b95a025` recurs in **`MTXPARD.dll`
@0x18cf5c/0x18cf68/0x18cf6c** and `3fb8aa3b` @0x18df4c — a shared HAL
constant set. [BIN]  `-6, 24, -120, 720, -5040` = signed factorials
(series for `mexp`/sincos macros [INF]); `±8160`, `6144`, `±2047`, `1023`
are raster/guard-band limits (8160 already in `parhelia-raster-clip.md`).

### 1.6 Kernel tunables: names, variables and compiled-in defaults

`mtx.ko` parameter table `g_aoMtxModParhlParms` (63 × {name*, var*}) @0x88440
[BIN mtx.ko], defaults from `.data` @0x883c8 [BIN]:

| registry name | variable | default |
|---|---|---|
| MmFlags | regSysMmFlags | 2 |
| AAMode | reg3DAAMode | **0x52** |
| TrinityEnable | reg3DTrinityEnable | 1 |
| ThresholdAA | reg3DThresholdAA | 0x7fe000 |
| FragSize | reg3DFragSize | 0x3f800000 (1.0f) |
| HwTransformLight / HwPureDevice | reg3D… | 1 / 1 |
| PhysicalMemory | regSysPhysicalMemory | 0x100 |
| UseTextiled / LightingType / VECacheMode / TnLZFactor | reg3D… | 1 / 1 / 1 / 1 |
| GuardBandClipping | reg3DGuardBandClipping | 2 |
| VertexShader / VertexShaderOptions | reg3D… | 1 / **0x11** |
| TEEnable / TEPipeSelect / TEDefaultColor / TEMultiplePipes / TEFilteringOptions | reg3DTE… | 1 / **4** / 0xffff80c0 / **7** / 0xffffffff |
| FlipOnVBlank | reg3DFlipOnVBlank | 3 |
| ForceCompressedMask / ForcePLNWMASK4FAA / ResolveOnStencilOpChange / VolumetricShadow | reg3D… | 1 each |
| WideLines / LineStipple / FastBitmap / FastRasterPos / PolygonOffset | regOGL… | 1 each |
| all `.bss` ones (BlitSoft, ForceAGPTextures, SwapTexturesVidMemOptions, UseIndirectPipes, HqCubemaps, VertexBuffers, ForcePCIAllocation, PipeIdEnable, PerformCRegCheck, ClipReject, TEFastTrilinear, AdjustZ, ResolveAlphaBlendedFrags, ClippingType, TELodBias, TextureCompress, Debug, TripleHead, VSyncInt, GigaColorFullScreen/Render, MmDebugFlags, BmDebugFlags, BmDebugInfo, DebugPatches, BmDbgOutLvl, SyncDrawing) | | 0 |

Module parameters (`.modinfo`): `agp` "1 = Enable AGP transfers (default);
0 = Use PCI transfers"; `agprate` "1x (default), 2x or 4x"; **`localbm` "1 =
Bus mastering from on-board memory; 0 = Bus mastering is using fast system
memory (PCI, AGP) (default)"**; `fw`, `enhmemacc` (int, undocumented). [BIN
mtx.ko .modinfo]  Generic table `g_aoMtxModGenParms` @0x88160: localbm, agp,
agprate, fw, enhmemacc. [BIN]  `localbm=0` default is consistent with
`parhelia-vram-ring-hazard` (ring in sysmem).

Quirk-name family in kernel `.rodata` @0xc7980–0xc7ee6 and `MTXPARD.dll`
@0xf8dcc–0xf92ac: `Force3DTextureTextiledToABSundog`, `…ToCDSundog`,
`ZUncompAnd2DClearSceneInProgSundog`, `EnableZCompSundog`,
`DivByZeroRefRastSundog`, `Force3DTextureToAB/CDSundog`,
`Use2DClearForceZUncompSundog`, `Use3DClearSundog`,
`UseZCompInMultContextSundog`, `ForceDepthToS8Z24Sundog`,
`ForceIntensityInCDSundog`, `UseSundogMemoryStrategies`, `EnableZinit`,
`ColRndMode`, `MemLevels`, `CullNoneMode`, `TEDLFCounters`, `PowerM`, `EOBTag`,
`Cap66Mhz`, `CompBypass`, `BmLocal`, `PciRead`. [BIN]

### 1.7 PInS-driven power-on register block — the exact register list

`pinsDatasConvertor` (44 × {dst, reg, type}) [BIN mtx_drv.so @0x14b5a0,
VMA 0x34b5a0]:

| type 3 (dword) registers | type 1 / type 2 (two halves) registers |
|---|---|
| 0xf00 0xf04 0xf08 0xf10 0xf1c 0xf20 0xf24 0xf28 0xf2c 0xf44 0xf94 0xf98 0xf9c 0xfa0 0xfc0 0xfc4 0xfc8 0xfcc 0xff0 0xff8 | 0xf14 0xf18 0xf48 0xf4c 0xf58 0xf5c 0xf68 0xf6c 0xf70 0xf74 0xf8c 0xf90 |

`parhelia-vbios.md` marks the `0x0f00` block "contents inferred"; this is the
vendor's own list of which of those registers are PInS-derived (32 distinct,
0xf00–0xff8).  Companion functions: `HWPARGetMCRegisters/SetMCRegisters`,
`HWPARInitializeMemoryController`, `HWPARGetPINS`, `HWPARGenereMPTable`,
`WaitForMemoryIdle`, `HWResetMemoryPhaseError` (dynsym). [BIN mtx_drv.so]

### 1.8 Register-default / switch table `oParheliaRegister`

`{0x1c24, 0x03041f00}, {0x1800, 0x00000001}, END 0xffffffff` [BIN mtx_drv.so
@0x13d480, 0x400 bytes, only two entries].  `0x1c24 = 0x03041f00` is the DVO
control value the X driver restores; `0x1800 = 1` re-enables DC head 0
(matches memory note "0x1800 1 restores scanout").

### 1.9 Complete board tables

`mtx_drv.so` 81-entry `{dev, subsys, name[0x80]}` table @0x12fa00–0x132508
(stride 0x88) [BIN]; chipset table @0x12e720 (`0x527 0x528 0x2537 0x2538
0x2539` → name strings "P-Series" ×5 @0xf729c…); `mtx.ko`
`mtx_supported_cards` @0x881c0: vendor 0x102b × {0x0527,0x0528,0x2537,0x2538,
0x2539} × class {0x030000, 0x038000}, mask 0xffffff00 [BIN].  Selected rows
(full list `~/tmp/strings-mine/board_table.txt`):

| dev | subsys | name | dev | subsys | name |
|---|---|---|---|---|---|
| 0x0528 | 0x1020 | Parhelia 128MB Dual DVI | 0x0528 | 0x1030 | Parhelia 256MB Dual DVI |
| 0x0528 | 0x1571 | Parhelia 256MB PCI Dual Link | 0x0528 | 0x14e1 | Parhelia 256MB PCI + Vin |
| 0x0528 | 0x2021 | QID Pro 256MB PCI | 0x0528 | 0x2011 | HR256 PCI |
| 0x0528 | 0x2421 | Matrox VPX-OUT8 256MB | 0x0528 | 0x14d1 | PSL 128MB PCI [sync on green analog] (GEMS) |
| 0x0528 | 0x1050 | Sono s20 128MB Dual DVI | 0x0528 | 0x1521/0x1531 | TheatreVUE T30/T20 PPP |
| 0x0528 | 0x1541 | Med2MP-DVI PPP(1 clock per pixel) | 0x0528 | 0x1511/0x1501 | ATC-4MP PDL / PSL |
| 0x0527 | 0x0840/0x0850 | Parhelia 128MB/256MB Dual DVI | 0x0527 | 0x0880 | P-256 Edge Overlap Controller |
| 0x2537 | 0x1820/0x1830 | P750 64MB / P650 64MB | 0x2537 | 0x1c10 | QID Quad DVI 128MB |
| 0x2538 | 0x0947 | Parhelia APVe 128MB | 0x2538 | 0x2c27 | PCIe Engineering Sample |
| 0x2539 | 0x0040/0x0081/0x0080 | P690 128MB / LP / Plus 256MB LP PCIe x16 | 0x2539 | 0x0180 | Display Wall IP Decode 128MB |

Windows INF subsystem lists (`mtxpar.inf`, `mtxpser.inf`, `MSerX.inf`) add
the marketing names (`QID`, `PJ-4OLP`, `EpicA TC2/TC4`, `RG-200DL/400SL`,
`Extio F1220/F1400/F1420/F1240`, M9120…M9188, Extio F2208/F2408) — see §2.

Bridge quirks: `MtxSetup.ini` `[AddReg.Extio.1]`/`[AddReg.P690]` OR
`0x60000000` into `HKLM\SYSTEM\CurrentControlSet\Services\PCI\Parameters`
keys **`104C8231`** (TI PCI-X bridge) and **`12D8E110`** (Pericom PI7C9X110
PCIe→PCI bridge), set `System.SerrPatch=1`, `System.PowerStateFlags=0xffffffff`;
`P690.DLL` exports `_IsPciPatchForPericomOnP690Needed@0` /
`_ApplyPciPatchForPericomOnP690@0`. [BIN P690.DLL export table; MtxSetup.ini]
`MTXPARM.sys` registry names `HintReg44/HintReg48/HintReg4C`, `HintForce64`,
`HintID`, `HintPatch`, `PCIAlign`, `AGPFlush/AGPPowerOff/AGPDelay/AGPFlags/
AGPDepth/AGPSafe/AGPRetry/AGPCount`, `BusMastering`, `System.PatchTable/
PatchCount`, `Engine.PinsBackup/BiosVerBackup/OnBoardId`. [BIN MTXPARM.sys
@0x49ba0–0x4ab98]

### 1.10 Windows vs Linux GL feature exposure (ICD strings)

`MTXOGL.dll` @0x481000–0x482400 lists, beyond the Linux set: `GL_EXT_vertex_shader`,
`GL_ARB_shadow`, `GL_ARB_depth_texture`, `GL_EXT_shadow_funcs`,
`GL_ARB_texture_non_power_of_two`, `GL_ARB_texture_mirrored_repeat`,
`GL_IBM_texture_mirrored_repeat`, `GL_ARB_window_pos`, `GL_NV_texture_rectangle`,
`GL_NV_blend_square`, `GL_MTX_precision_dpi`, `GL_WIN_swap_hint`,
`WGL_MTX_framelock`, `WGL_MTX_video_preview`, `WGL_I3D_genlock`,
`WGL_ARB_pbuffer/pixel_format/multisample`, `WGL_EXT_swap_control`. [BIN]
Linux `libGL` (32 = 64 bit, identical list @0x46e0e0 in lib64): 
`GL_ARB_multisample multitexture point_parameters texture_border_clamp
texture_compression texture_cube_map texture_env_add/combine/dot3
transpose_matrix GL_S3_s3tc GL_ATI_element_array GL_ATI_vertex_array_object
GL_Autodesk_valid_back_buffer_hint GL_EXT_bgra blend_color blend_func_separate
blend_logic_op blend_minmax blend_subtract clip_volume_hint
compiled_vertex_array draw_range_elements element_array fog_coord
multi_draw_arrays packed_pixels point_parameters polygon_offset rescale_normal
secondary_color separate_specular_color stencil_wrap subtexture texture3D
texture_compression_s3tc texture_cube_map texture_edge_clamp texture_env_add
texture_filter_anisotropic texture_lod_bias texture_object vertex_array
vertex_array_object texture_env_combine texture_env_dot3 GL_KTX_buffer_region
GL_MTX_fragment_shader GL_NV_texgen_reflection GL_SGIS_multitexture
GL_SGIS_texture_lod`. [BIN libGL64 @0x46e0e0]  So shadow maps, NPOT and
`GL_EXT_vertex_shader` are hardware-capable but not exposed on Linux.

`libGL64` RTTI (full set in `~/tmp/strings-mine/libgl_rtti.txt`):
`GLVSInstruction_{1Op,2Op,3Op,Add,Clamp,Cross,Dot3,Dot4,Exp2,ExtractComponent,
Floor,Frac,Index,InsertComponent,Log2,Madd,MatrixMultiply,Max,Min,Mov,Mul,
Negate,Power,Recip,RecipSqrt,Round,SetGE,SetLT,Sub,Swizzle,WriteMask}`,
`GLVS*Desc` (MVP, Modelview, Projection, TextureMatrix, TexGen{Object,Eye}Plane,
Material*, Light*, Fog*, ClipPlane, LocalConstant, Invariant, Output),
`GLVertexShader{Declaration,Function}{,DX}`, `GLPrimitiveHardware{,VS}`,
`GLSendStateHardwareTnL{,VS}`, `FragmentShaderInvariant` (`MAX_OP`,
`INVARIANT0/1`), 45 "Error in Matrox Fragment Shader - …" messages
@0x36de88–0x36ea68 (same set in `MTXOGL.dll` @0x485b00–0x4866ac). [BIN]

### 1.11 Other newly surfaced data objects (display side, sizes from dynsym)

`mtx_drv.so` `.dynsym` has **2250 real names** (`.symtab` is obfuscated
`mtxNNNN`; the 6.8.2 `mtx_drv.o` is 2605 obfuscated vs 49 real).  Tables
with sizes: `VesaParam(0x2100)`, `VgaState(830)`, `EstabParam(3072)`,
`DTVParam(2508)`, `DTable_Chrontel7009(320)`, `DTable_Chrontel7303(200)`,
`DTable_DMTr10(456)`, `CVE2{ITU656,Luma,MACROV,PARM}Table`,
`Mav{Gamma,ProcAmp,TV,TV_YUV,TimingColorBar,Timing,VScaleTV}Table`,
`g_aoTVCHRRegValues(3120)`, `g_aoCHRTVModeInfo(8840)`, `Si164Param = 39 00 89`,
`Si1162Param = 30 00 90 89 31`, `Si1178Param = 44 4c 01 80 00 89 60 34 35`,
`ChrDVIParam = 00 2d 07 26 30 e0 00`, `ChrVerBDVIParam`, `ChrVerCDVIParam`,
`oLutShiftData8/10`, `ucCompBits`, `g_adwParhlXaaBopTable` = bit-reversed
ROP nibble map `0 8 4 c 2 a 6 e 1 9 5 d 3 b 7 f` [BIN @0x133e20],
`m_aoLevels`, `g_oBiasTable_FPU` (256 bytes of doubles 184.0 … stepping −4:
gamma bias table), 31 `MTXFLOATVAR_*` HAL float constants (`0.0625 0.098 0.25
0.257 0.439 0.5 0.504 0.9 1.5 213.3333 …`, gamma/colour-space math, VMA
0x333f00). [BIN mtx_drv.so]

---

## 2. Per-binary tables

### 2.1 `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (2.14 MB, GCC 3.4.6, built "Mon May 11 04:21:22 EDT 2009", v1.4.7.0, `.matroxinfo` @0x166400)

| item | offset | content |
|---|---|---|
| debug strings | 0xc75a0–0xc78c0 | `[mtx] *ERROR* Parhelia OS: Failed to allocate/retrieve Common/Shared (locked) data buffer (sz: %lu)`, `Common buffer sizes doesn't match with device ones`, `[mtx] Parhelia patches applied: %s` |
| texture asserts | 0xc7918 | `This texture cannot be used for this usage` / `…for this operation` |
| quirk/param names | 0xc7980–0xc7ee6 | §1.6 |
| `ERROR: Pure virtual method called` | 0xc79f8 | C++ runtime |
| float pool | 0xc7f04–0xc8220 | §1.5 |
| .data template fragments | 0xe4cc0–0x139000 | 150 symbols, §1.4 |
| .data anonymous canned programs | 0x150190–0x164d80 | §1.1 |
| DWGCTL-style literals in .text | `0x005000f5` ×6 @0x3dd7f…0x3eaa9; `0x002010cc` ×3 @0x32b70/0x3c582/0x3c594; `0x20292820` ×2 @0x32411/0x3303c; `0x4106000f` ×4 @0xa068d…0xa0b9f; `0x20000002` ×33 | [BIN] |
| non-`mtxNNNN` symbols | | `AddAfd AddAfs AddEfd AddEfm AddEfs aDI1L aDI2L aDL1L aDL2L aPDI2L aPI1L aPI2L aPL1L aPL2L aSDI2L aSDL2L aSI1L aSI2L aSL1L aSL2L aSPL2L mtx_parhl_driver` (12-byte objects @0x14c19c–0x14c1fc; add-function templates [INF]) |
| register-tag tables (≤0x21 runs, 0xffff-terminated) | none found | the tag lists are built at runtime [INF] |

### 2.2 `kernel-orig-56/mtx.ko` (same driver + glue, vermagic 2.6.30-dist SMP)

Real-name glue symbols: `Client*` (IoRead/Write, PciConfig*, RegisterRead/Write
Byte/Word/Dword, MemAlloc, MicroDelay, Schedule), `MtxDevice_Get{BmFlags,
FrameBuffer,PciHandle,Private,RegisterHandle,Registers}`,
`MtxRegion_{Alloc,Free,FindFromBase,FindFromVirtual,GetLinear,GetPhys,GetSize,
Inc/DecrementRefCount}`, `MtxIoctl_{Register,UnRegister,GetMinor}`,
`mtx_ioctl_{driver_value_request,get_buffer,get_memory_info,hard_reset,
release_buffer}`, `mtx_vm_map_{agp,io,pci,sys}`, `mtx_mem_alloc_{agp,pci}`,
`mtx_agp_{acquire,apply_rate_patches,chipset_info_print,enable,init_finish,
test_device}`, `pciFind{AGP,PM}Capability`, `OsAllocLockedPages`,
`Os{Enter,Exit}FPUMode`, `MtxParhlParameterLookup` (@0xc5d70, 0x19a),
`MtxParm_InitLut`, `dbgOut{Level,Modules,Options,Files,FileListInvert}`. [BIN]
Parameter/board tables: §1.6, §1.9.

### 2.3 `xserver/7_0_0/mtx_drv.so` (1.52 MB; `.dynsym` real names)

| item | offset (file) | content |
|---|---|---|
| X options table | 0xf70d7–0xf72f6 | `SWcursor HWcursor PciRetry noAccel ShadowFB Rotate TvOut TvStandard TvScaling SelectedRegion OutputRegion AntiFlickerLevel Brightness ColorEnable Contrast Gamma Saturation Crtc2Half Busmastering DmaEmulated Crtc2Ram MergedFB Monitor2HSync Monitor2VRefresh Monitor2Position MetaModes DigitalScreen{,1,2} TripleHead QuadHead DVIDL NoLogo OffScreen PackedPixel PackedPixelType QIDTV GNLK MVGA PseudoXinerama` |
| messages | 0xf7b20… | `Wrong resolution for Digital Output #2. Clock must be the same for both outputs.`; `Parhelia Pro does not support TripleHead.`; `Local Busmastering is enabled` / `Emulated Busmastering` / ` Falling back to Local DMA `; `TV output could not be activated because bus mastering is not available.`; `Fail to allocate apperture for primary surface.` + ` the option regSysMmFlags=0`; `OpenGL is disabled on this screen because it is not hardware accelerated`; `HAL: Pins in use => DEFAULT` |
| VidParm dump format | 0xf7618–0xf7d08 | `ulBpp / ulPixClock (kHz) / ulFBPitch (pixels) / ulDstOrg / ulDisplayOrg / flSignalMode 0x%08X / flOutput 0x%08X` |
| PLL debug (`hwsharing/pll.c`, `hwsharing/sun90pll.c`) | 0x1121e9–0x113021 | `Program PLL with M1=%d,P1=%d,M2=%d,P2=%d`, `ERROR PLL%d : VCO%d doesn't lock`, `ERROR FS%d : Do not lock`, `Algorithm 1/2 : Compute PLL lOutFreq %d KHz from inFreq %d KHz`, `Intermadiate Freq %d KHz`, `M1/M2/P1/P2 reg value %02Xh`, `SUN90 PLL not supported` (same set in `MTXPARM.sys` @0x4a27d–0x4a9d9, source path `c:\build\sources2.06.03\driver\hal\hwsharing\pll.c`) |
| HAL function names (dynsym, 649 `HSL*`) | | `HSLPAR{ApplyVidReset,ApplyEar63,ApplyVidParmPatch,ComputeDSTORG,ComputePitch,ChangeGclkSpeed,DetectEAR66,Disable/EnableDAC1/2,DCMA/DCMB,DVO1/2,SI164,TstClk,VDP,GetFSFreq,GetFreeFS,GetGclk,SetDVOClock,SetMinimumDVOClk,PowerUpDVO1Clock,PixelClockDetuning,SelectDisplayControllerClock,SetClkDiv2CRTCA/B,SetClockSrcDCMA/B,SetDAC1/2OnDCMA,SetDEMREQCTLtoDCMA/B,SetDEMVGAHCntMode,TranslateDcmClkSelIntoFS,TranslateGclkSelIntoFS,ResetPriA/B/AB,SenseLoad,SenseLoadOnInternalDAC,SetSyncPolDAC/DVO,WaitEngineNotBusy,IsDvoLocked,MemoryControllerCheck,HardwareReset,LowPower,PM_ACPI/APM/DPMS,GetCrtcPreloadCount,GetCrtcPixelClk,GetPriorityLevels,SetPriorityLevels,_Register_PriSurfAddr_Get/Set}`, `HSLS90*`, `HSLSI164{Enable,Disable}{,DualLinkMode}`, `HSLCHR*` (Chrontel TV), `HSLCVE2*`, `HSLMAV*`, `HSLSE128K/SE256` (EEPROM), `HSLPWMTC664Detect`, `HSLAV9110SetPixelPLL`, `SFA*` (feature authorisation: `SFA_AMF_Authorize`, `SFATOOL_IsMedicalProductGrade`, `SFATOOL_IsQIDGrade`), `HWPAR*`/`HWS90*` (memory controller, FS PLLs), `pll*`/`sun90pll*`, `Parhelia{Get,Set}E2P*` (EEPROM bit-bang), `Parhl{Init,Cleanup}Driver`, `Parhl{Enable,Disable}Device/UserProcess`, `Parhl{Allocate,Free}DeviceBuffer`, `ParhlXAA{Init,Cleanup}`, `MTXVideo*` (Xv), `MTXGLX*`, `OsPostBusmasteringBuffer{,Video}`, `OsGetCommonBuffers`, `OsAllocApertureBlockMemory`, `OsAllocSurfaceBlockMemory` |
| in-register accessors | 0x42800–0x42ec0 (VMA) | `MTX_OUT_REG32/16/8`, `MTX_IN_REG8/16/32` |
| data tables | | §1.7, §1.8, §1.9, §1.11 |

### 2.4 `opengl6/lib64/libGL.so.1.3.0` (ICD, C++)

Source files named: `FragmentShaderInvariant.cpp GLDisplayList.cpp
GLDisplayListPrimitive.cpp GLFragmentShader.cpp GLFragmentShaderManager.cpp
GLPrimitiveHardwareVA.cpp GLPrimitiveSoftware.cpp GLPrimitiveSoftwareClipping.cpp
GLSBaseComHandle.h GLSCommand.h GLSType.cpp GLSUserComHandle.cpp version.c`
[BIN].  Asserts: `uiInstruction < FragmentShaderState::MAX_OP`,
`(eArg == FragmentOp::INVARIANT0) || (eArg == FragmentOp::INVARIANT1)`,
`uiOpcodeArgCount>0`, `0 && "Texture Op Modifier set to invalid option"`.
No WARP words, no register literals (§1.2).  Extension list §1.10.

### 2.5 `MtxPar/MTXPARD.dll` (display driver 6.14.01.1799, "2.06.03.001")

| item | offset | content |
|---|---|---|
| D3D vs.1.x disassembler | 0x1805d4–0x180930 (dup @0x1879e4–0x187cf8) | swizzle table `.* .x .y .xy .z .xz .yz .xyz .w .xw .yw .xyw .zw .xzw .yzw`, `%.8X    ; COMMENT`, `%.8X    ; FOOTER`, `vs.%d.%d`, `a%d r%d oPts oFog oPos Error[%d] oT%d oD%d`, `c[a0.x + %d]`, `c%d v%d r%d`, `x y z w` |
| mnemonic table | 0x18f124–0x18f227 | §1.2 (D3D + `m*` macros) |
| registry names | 0xf8c40–0xf9600 | AAMode ThresholdAA FragSize ForceCompressedMask ResolveOnStencilOpChange AdjustZ ResolveAlphaBlendedFrags ForceAGPTextures AGPSize SwapTexturesVidMemOptions HwTransformLight np2Conditional MipmapLodBias PrimaryLockToUserMemEnable YUVLockToUserMemEnable HwPureDevice EnableZinit *Sundog PhysicalMemory ExtendedVolumeTexture HqCubemaps UseTextiled VertexBuffers ShareNonLocalVidMem MemLevels LightingType VECacheMode CullNoneMode TnLZFactor ForcePCIAllocation GuardBandClipping VertexShader D3DEnable VertexShaderOptions TrinityEnable PerformCRegCheck ClipReject VolumetricShadow ClippingType TEEnable TEPipeSelect TEDefaultColor TEMultiplePipes TEFastTrilinear TEFilteringOptions TELodBias TEDLFCounters TripleHead PolygonOffset WideLines SmoothWideLines RCoordExpansion LineStipple TextureCompress DisplayListMode Debug Overlay Stereo StereoDisplay StereoZ StereoDisplayMode VSyncEvent DMAWriteEvent VeryFastDrawPixels MultiMonitor AlternateVendor Version ForceMonitor ForceWindowIntensityInCD DitheringDefault RoundAAPoints ContextTraceDisabled UseSystemMemForSecDma IndependentBackBuffers FastContextSwitching TripleBuffer HwPixelBuffer PolygonOptForEmageon VertexArrayObjectSupport FastTexLoadLuminance PuntAll2d StretchBlt MmFlags VirtualMemory PCIExCtrl DdFlags DdLockFlags MmDebugFlags MmMaxApertureSize BmDebugFlags BmDebugInfo BmDbgOutLvl DdBlitSoft DebugPatches VSyncInt GigaColorFullScreen GigaColorRender DxVaSupport DxVaSupportIndicator DxVaNineSupport AlphaTextGamma TextAntiAliasing FlipOnVBlank NoAGPSurface OverlayEmulationEnable OverlaySyncFlipEnable PVOutputAlwaysOn ForceOffscreenPlainUserMemLock (byte-identical set in `mtxpserd.dll`) |
| surface-buffer trace | 0x7ec50 / 0x7ecb0 | `DeclareSurfaceBuffer %X %X %X %X` / `UnDeclareSurfaceBuffer %X` |
| chip-ID registry | 0x18c114 | `MapDrvRegPathChipID`, `Chip` |
| WARP programs | 0x190d50–0x192464 | canned programs (§1.1); 0x192470–0x1937c0 = 398-instr blob identical to kernel 0x161cc0; 0x1a5bf0–0x1bd218 ≈ 0x17634 bytes of further WARP words (7983 12-byte units, no internal END until 0x1bd218) [BIN] |
| float pool | 0x18cf5c–0x18cf6c, 0x18df4c | §1.5 |
| literals | `0x005000f5` @0x1ea33; `0x002010cc` @0x1c2f7/0x1de04/0xa3441/0xfeac1; `0x20292820` @0xafe96; `0x4106000f` ×4 @0x6be7f–0x6c423 | [BIN] |
| UTF-16 | 0x7eac0 `MTX_IN_FILE.in`, 0x14d6f0 `c:\tesx`, 0x14dba0 `c:\idctx.bmp` | debug file hooks |

### 2.6 `MtxPar/MTXPARM.sys` (miniport 6.14.01.1799)

Board name strings (UTF-16 @0x492c0): `Matrox Parhelia Series / Parhelia /
Parhelia LX / MP-PCIe-9064 / MP-PCIe-2364 / Millennium P-Series / MM-09 /
MM-07 / MM-05 / MM-10 / Medical Series`; `PARHA***`, `MTXPARV`,
`xIntegrated, xyz MHz`.  Registry: `HardwareInformation.{BiosString,DacType,
MemorySize,ChipType}`, `System.PatchTable/PatchCount`, `PciSize`,
`VgaCompatible`, `OnBoardId`, `ProtectFrameBuffer`,
`OffscreenPlainYUY2ForcePitchEqualToWidth`, `PciAlignOffset`, `DeviceBitmapsCaching`,
`ForceMonitorType`, Hint*/AGP* (§1.9), `BusMastering`, `Engine.{BiosVerBackup,
OnBoardId,PinsBackup}`, `\Device\PhysicalMemory`, `GenuineIntel`. [BIN]
WARP self-test image @0x627b8 (§1.3).  Only 16/44 captured `w2` words occur
(no draw templates here).

### 2.7 `MtxPar/MTXOGL.dll` (ICD 2.06.00.002, 2008) and `MtxPser/mtxpsogl.dll`

45 fragment-shader error strings @0x485b00; extension lists §1.10; **no WARP
words, no 12-byte templates** — the Windows ICD hands programs to the display
driver.  `mtxpsogl.dll` is the same build shifted +0x1000.

### 2.8 Later-family siblings (P-series 2010, M-series 2011)

| binary | finding |
|---|---|
| `MtxPser/mtxpserd.dll`, `mtxpserm.sys` | identifier sets equal to MTXPARD/MTXPARM (diff = 0 real identifiers); WARP tables at +0x2000 (canned programs @0x192d50…, 398-blob @0x194470, big region @0x1a7bf0) |
| `MSerX/MxMD.dll` ("phoenix", `c:\build\phoenix\wxp\sources2.10.00\driver_phoenix\…\MxMD.pdb`) | D3D format names table @0x1d6f20 (`R8G8B8 … A32B32G32R32F CXV8U8 VERTEXDATA INDEX16 INDEX32 MULTI2_ARGB8`), registry names incl. `PsBmMicrocode PsBmConstants CeBmMicrocode CeBmConstants BmBuffers BmCacheFifoSize BmRequestSize EAR30PSCTL2 EAR30PSSYNC EAR98 ShaderOpt HZFlags Queries IndexBufferCacheSize MatchRefRastBugs L8_3D_L16_3D TLSOpt TearFree Multisample`, predicate table `(p0)…(!p5)` @0x1df9cc, `RESET_FRAG_PTR` @0x1e4924; **0 WARP END words** — different engine |
| `MSerX/MxMGLX.dll` | ARB program assembler tokens `DIRECTIVE_{ALIAS,OUTPUT,ADDRESS,TEMP,PARAM,ATTRIB,OPTION,END}`, `VERTEX_PROGRAM_1_0`, `FRAGMENT_PROGRAM_1_0`, `XYZW_SWIZZLE`, `RGBA_SWIZZLE`, `*_SAT` for `KIL TXB TXP TEX SWZ MAD LRP CMP XPD SUB SLT SGE MUL MIN MAX DST DPH DP4 DP3 ADD POW SCS SIN RSQ RCP LG2 EX2 COS MOV LIT FRC FLR ABS` @0x80ee54–0x80fadc, GLSL tokens — M-series only, absent from `MTXOGL.dll` |
| `MSerX/MxMM.sys` | M-series miniport; 0 WARP words |
| `MTXCI.DLL`, `MtxCIP2.dll`, `Extio2.dll`, `MtxEscape.dll`, `P690.DLL`, `mtxparmx.sys` | installers / COM UI / Pericom patch (§1.9); `mtxparmx.sys` is a 5.5 KB stub (`\??\`, `IoCreateSymbolicLink`) |

### 2.9 Config files

| file | content |
|---|---|
| `mtxpar.inf` | 21 subsystem IDs for 0x0527/0x0528/0x2537/0x2538 (§1.9); `AddReg.MTXPARH`: `System.FastWrite=0`, `System.MmMaxApertureSize=0`, `System.ForceSBA=1`, `3D.D3DEnable=1`; ICD key `OpenGLDrivers\MTXOGL Version=2 DriverVersion=1 Flags=1` |
| `mtxpser.inf` | 0x2537/0x2538/0x2539 subsystems (`2C31 3007 30C7 3011 3001 3061 3081 0040 0080 0081 00C0 0140 0101 00C2`) |
| `MSerX.inf` | 0x0540 subsystems `2080 2100 2140 2180 21C0 2241 2240 22C0 2280 2301 2302 2304`; `System.MaxAdapterCount=4` |
| `MtxSetup.ini` | per-device default mode tables (`Mga.PixelWidths`, `Mga.SingleResolutions` binary 16-bit pairs), `[AddReg.QID]` = `DefaultSettings.X/YResolution=4096`, `DualHead.BuildDefaultConfigList=0`; bridge quirks §1.9; `Extio2` = `DirectX.IndirectLockForceBlitSwToLocal=1` |
| `vbios-matrox-parhelia` / `tools/p690.rom` | `IBM COMPATIBLE MATROX/PARHELIA VGA/VBE BIOS (V1.6) b34`, `Revision: 0.34`, LSI Logic 1990-1991 core / `MATROX/P650/P690/P750 VGA/VBE BIOS (V1.8) b16`, `Revision: 0.16` |

---

## 3. Contradictions / corrections to existing docs

| doc | statement | binary evidence |
|---|---|---|
| `parhelia-engine-and-context-init-complete.md` §table "miniport self-test image" | `MTXPARM.sys .data VMA 0x627b8 (file 0x527b8)`, `0x1074 = 351 instr` | the image is at **file 0x627b8** (`.data` file 0x4ac80–0x64aa0, so VMA = **0x727b8**); file 0x527b8 holds `00000000 00000000 00000038 00000002 …` (not WARP). 346 instr to the first END @0x637e4; 351 only if the second 2-instr image + its END (0x637fc–0x63820) are counted. [BIN MTXPARM.sys, `objdump -h`] |
| memory note "mtx_drv.so has real symbol names" | | true only for `.dynsym` (2250 names); `.symtab` is `mtxNNNN`-obfuscated like the kernel. The 6.8.2 `mtx_drv.o` has 49 real vs 2605 obfuscated. [BIN] |
| `parhelia-warp-programs-decoded.md` / ISA doc decode `.data` blobs by symbol (`--sym mtx1621`) | | `mtx1621` is 60 bytes at VMA 0x80c00 (file 0x149020 region); the live canned programs (§1.1) are in the **symbol-less** tail 0x150190–0x164d80 and cannot be reached by `--sym` — use file offsets. [BIN readelf] |
| `parhelia-vbios.md` "0x0f00–0x0f5c power-on init block … contents inferred" | | block extends to **0xff8** and the PInS-fed registers are exactly the 32 in §1.7 (`pinsDatasConvertor`). [BIN mtx_drv.so @0x14b5a0] |
| `parhelia-variant-routing.md` "SiI164 probed at I2C 0x70/0x76" | | HAL parameter bytes `Si164Param = 39 00 89`, `Si1162Param = 30 00 90 89 31`, `Si1178Param = 44 4c 01 80 00 89 60 34 35` — 0x39<<1 = 0x72, 0x30/0x31<<1 = 0x60/0x62 [INF on encoding]; worth re-checking the probe list against these. [BIN mtx_drv.so dynsym] |
| ISA doc §2 assumption that vendor mnemonics might exist in text | | none exist in any binary (§1.2); the only textual ISA is the D3D-level macro set. |
| `parhelia-warp-isa-complete.md` §3.5 "values of base registers 1..3 and how they are set — open" | | the setter programs are complete blobs at kernel 0x1643c0 (16 instr) / 0x1646a0 (28 instr) / 0x1641e0 (13 instr) and Windows 0x190d50/0x1910a0 (§1.1); the `ctl35` operand bytes `3a..3f` in w1[29:24] paired with `imm 0xe8..0xfc` step 4 are the six constant-address writes. [BIN] |

---

## 4. Absence table (things searched for and not present anywhere)

| pattern | binaries | result |
|---|---|---|
| `warp`, `WARP`, `microcode`, `ucode`, `firmware` | all 30 | 0 (only `PsBmMicrocode/CeBmMicrocode` registry names in M-series `MxMD.dll`) |
| `DWGCTL PLNWT SECADDRESS SECEND PRIMADDRESS SOFTRAP DWGSYNC MACCESS PITCH YDST CXBNDRY FCOL BCOL TEXCTL TEXORG WFLAG WMISC WVRTX WIADDR WGETMSB WACCEPTSEQ SETUP OPMODE ICLEAR IEN` | all 30 | 0 as strings |
| primitive triple `{0x20,0x3d,0x5c}` (bytes or dwords) | kernel, MTXPARD, MTXOGL, libGL, mtx_drv | 0 |
| format run `0x30 0x32 0x36 0x36` as dwords | same | 0 (byte run `30 32 36` only inside text/strings) |
| `0x00a60ec4` | same | 0 |
| register-tag tables (≥8 dwords ≤0x21, 0xffff-terminated) | same | only one 29-entry run in `MTXOGL.dll` @0x4eb748 (RTTI/CRT data [INF]) |
| GL extension strings | `MTXPARD.dll`, kernel | 0 |
| chip IDs `0x0530`, `0x0525`, `0x2527` | `mtx_drv.so` tables | 0 (`0x0540` only as data @0x14a384/0x14a404 inside a WARP template, coincidence) |

---

## 5. Coverage — what this pass did and did not reach

| covered fully (strings ASCII+UTF-16, symbols, table scans) | partially | not reached |
|---|---|---|
| `mtx_parhelia.o`, `mtx.ko`, `xserver/7_0_0/mtx_drv.so`, `libGL.so.1.3.0` (64-bit), `MTXPARD.dll`, `MTXPARM.sys`, `MTXOGL.dll`, `mtxparmx.sys`, `P690.DLL`, all `.inf`, `MtxSetup.ini`, `MtxPDesk.cfg`, VBIOS images | `mtxpserd.dll`/`mtxpserm.sys`/`mtxpsogl.dll` (identifier diff + blob offsets only), `MxMD.dll`/`MxMGLX.dll`/`MxMM.sys` (string sweep only), 32-bit `libGL` (extension list compared only), `MTXPARD.dll` WARP region 0x1a5bf0–0x1bd218 (located, not decoded), `libglx.so`/`libmtxcfg.so` (names only) | `xserver/7_1_0…7_4_99` copies (assumed identical builds, sizes equal ±141 B), `6_8_2/libglx.a`, `libmtxcfg.a`, `v4l_drv.so`, `.cab`/`.msi`/`Setup.exe` archives (contain the same files), `MtxCIP2.dll`, `MTXCI.DLL`, `Extio2.dll`, `MtxEscape.dll` (installer/COM only, no hardware text), `MtxPDSE.cfg`, `m9xdriver-*` tree (M9, out of scope) |

Highest-value follow-ups from this pass, in order: (1) lift the three canned
programs at kernel 0x1643c0 / 0x1646a0 / 0x1641e0 and diff them against the
kind-1/0/3 packets of the live capture (`e0 160 80 800` / `0 1 2 3`) to pin
which `ctl33` operand byte (`00040100/104/108/10c/110/114`) carries which
base register; (2) run `parhelia-warpdis2.py` over the miniport self-test
(§1.3) and the 398-instr blob (kernel 0x161cc0 = MTXPARD 0x192470) for
`ctl41/42/43/04/05/06` operand statistics; (3) decode the 0x17634-byte
`MTXPARD.dll` region @0x1a5bf0.
