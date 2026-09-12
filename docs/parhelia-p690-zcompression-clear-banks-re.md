# P690 Z compression, Z-init fast clear, depth clear paths and AB/CD banks

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Static reverse engineering of the Sundog-class ("`*Sundog`" option) depth paths
for the Millennium P690 (PCI `102b:2539`, SUN90).  No hardware was touched and
no driver source was changed.  Evidence labels: **[PROVEN]** = directly read
from the disassembly at the cited address; **[INFERRED]** = the most plausible
reading of proven code, not yet confirmed on hardware; **[OPEN]** = not
resolved.

> **Superseded in part by `parhelia-p690-fast-depth-clear.md`.**  That document
> measures the Z-init block on P690 silicon and corrects three readings below:
> the coverage limit is **640 tiles of 4096 bytes (2 621 440 B)**, not the
> vendor's software `0x3c0000`; the Z-init load writes no memory at all, so the
> resolve of section 3.3 is mandatory for every consumer outside the Z unit;
> and the reason the feature ships disabled is the per-chip default expression
> alone (`mtx_drv.so` `RegistrySwitchManager` `0x6101f`, mask `0x3c0` =
> PCI `102b:0528` only), with no erratum recorded anywhere in either package.
> The `mtx_drv.so` `.dynsym` carries the real symbol names for the same
> addresses `.symtab` obfuscates, which is how those defaults were read.

## Inputs

| file | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` | `52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a` |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpsogl.dll` | `db0ab86fd25fe2cfb37bae5252d825aac8a2cc16d88191dedb165d4ba57dbcf0` |
| `xddm32_210_00_105_se_u_nowhql/mtxpser.inf` | `169f49f2a3643e01f47d650aece6b56c5d418d08912d8804c96a79a33051e405` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |

`mtxpserd.dll` addresses are image VAs (image base `0x10000`, file offset =
VA − `0x10000`).  `mtx_parhelia.o` addresses are `.text` offsets.  `mtxpser.inf`
binds `DEV_2537`, `DEV_2538` and `DEV_2539` (subsystems `0040/0080/0081`), so
`mtxpserd.dll` is the P690 display/3D HAL.

## Summary

1. **P690 is a "Sundog-class" chip for all `*Sundog` paths.**  The vendor
   capability word is one-hot per chip/revision (`mtx_parhelia.o` `mtx1916`
   `0x40a70`, twin `mtx427` `0x40b90`): `0x2539` rev 0/1/other →
   `0x10000/0x20000/0x40000`; `0x2537` → `0x400/0x800/0x1000`; `0x2538` →
   `0x2000/0x4000/0x8000`; `0x528` → `0x40..0x200`; `0x527` → `0x2..0x20`.
   Hence `caps & 0x7fc00` = Sundog+Sunex+SUN90 and `caps & 0x70000` = SUN90
   (P690) only. [PROVEN]
2. **Neither Z compression nor the Z-init fast clear is enabled by default on
   P690.**  `3D.EnableZCompSundog` defaults to 0; `3D.EnableZinit` defaults to
   1 only for `caps & 0x3c0` (the 0x528 family).  The INF sets no override.
   The vendor's default P690 depth clear is a **2D solid fill** — the same
   thing our driver does now. [PROVEN]
3. **Compression is not a surface-descriptor feature.**  The depth cfg format
   is `0x36` for every 32-bit Z format with or without compression, and no
   extra (tag) memory is allocated anywhere on the compression path.  The
   hardware is driven only by DWGCTL *control words* (`0x00a`, `0x20a`,
   `0xa0a`, `0x008/0x108/0x208/0x308`) plus the `0x12b4` Z-init value
   register, and a single depth surface at a time owns the state. [PROVEN for
   the software; the hardware storage of the state is OPEN]
4. **The 3D depth clear** (Linux `Use3DClearSundog`, never read by any vendor
   binary) exists in the P-series Windows HAL as a WARP rectangle with colour
   writes masked off; `0x12C8/0x12CC/0x12D0` are the ordinary depth/stencil
   state registers, *not* clear-value registers.  The clear value travels in
   the vertex Z (3D path), in FCOL `0x1314` with PLNWT `0x1324` as a byte mask
   (2D path), or in `0x12b4` (Z-init path). [PROVEN]
5. **"AB" and "CD" are the two local-VRAM ranges of the Linux memory manager,
   which on P690 are the engine's bank A (`0x00000000`) and bank B
   (`0x08000000`).**  `Force3DTextureTo{AB,CD}Sundog` only acts when
   `UseSundogMemoryStrategies` = 1 (default 0), and none of these options
   exists in the Windows P-series driver. [PROVEN chain in `mtx_parhelia.o`,
   see §5; equating AB/CD with two physical memory controllers is INFERRED]
6. **Depth surface format code on the P-series: `0x36`** for S8Z24, X8Z24 and
   every 32-bit Z variant; `0x2a` for D16, `0x2b` for D15S1.  `0x33/0x38/0x3a`
   are colour/texture codes.  The stencil width lives in `0x12C8[20:17]`.
   [PROVEN, full table in §6]

---

## 1. The option registry and its defaults

### 1.1 Windows `mtxpserd.dll`

The parameter container constructor is `0x10b5b4` (called at `0x92a14`, result
stored at `pDev+0xa680`).  Each registry parameter is created by `0x10a8d0`
with arguments `(ok*, key, name, 0, default)`; the default is stored at
`param+0x10` and returned by vtable slot 3 (`0x10a9a0`: `mov eax,[ecx+0x10]`)
when the registry value `key.name` is absent (`0x10da36`, fallback at
`0x10daf9`).  A validated copy is built by `0x90be0` (result at
`pDev+0xa67c`); each wrapper is created by `0x906b0` and read through vtable
slot `+0xc` (`0x10a9a0` again).  [PROVEN]

| key.name | param ctor | default | param slot | validated slot | readers in `mtxpserd.dll` |
|---|---|---|---|---|---|
| `DirectX.EnableZinit` | `0x10bae2` | `(caps & 0x3c0) && ([[ebx+0xc]+0x548] & 0x08000000)` (`0x10baaf..0x10bace`) | `+0x30` | `+0x2a8` | `0xa0c96` (Sundog clear), `0xa0f65` (Parhelia clear) |
| `3D.EnableZCompSundog` | `0x10bb26` | 0 | `+0x40` | `+0x2b0` | `0xa0b9d`, `0xa0bc9` |
| `3D.ForceDepthToS8Z24Sundog` | `0x10bb69` | 0 | `+0x44` | `+0x2b4` | `0x87950` |
| `3D.DivByZeroRefRastSundog` | `0x10bbac` | 1 | `+0x48` | `+0x2b8` | none found |
| `3D.Use2DClearForceZUncompSundog` | `0x10bbf0` | 0 | `+0x4c` | `+0x2bc` | `0xa0c73` |
| `3D.UseZCompInMultContextSundog` | `0x10bc33` | 1 | `+0x50` | `+0x2c0` | `0x9f400`, `0x9f49a` |
| `3D.ZUncompAnd2DClearSceneInProgSundog` | `0x10bc77` | 0 | `+0x54` | `+0x2c4` | `0xa0c48` |
| `OGL.ForceWindowIntensityInCD` | `0x10cb2c` | 0 | `+0x118` | `+0x3a0` | `0x38c02` |
| `OGL.ForceIntensityInCDSundog` | `0x10cb72` | 0 | `+0x11c` | `+0x3a4` | `0x3a448` (memory-policy builder `0x3a160`) |

`Use3DClearSundog`, `UseSundogMemoryStrategies`, `Force3DTextureTo{AB,CD}Sundog`
and `Force3DTextureTextiledTo{AB,CD}Sundog` do **not** exist in the P-series
Windows binaries (`strings -a` and `strings -el` of all `MtxPser`/`MtxPar`
files).  `mtxpsogl.dll` (the OpenGL ICD) contains none of the `*Sundog`,
`Zinit` or `ZComp` names; its "OGL" registry options are owned by the display
driver above.  No INF or `.cfg`/`.ini` in the package mentions any of these
names; `mtxpser.inf` only sets `3D.D3DEnable=1` among the `3D.*` keys.
[PROVEN]

`ForceDepthToS8Z24Sundog` has exactly one consumer: the DirectDraw pixel-format
translator `0x87930` (its `this+0xc0` is set to `pDev+0xa67c` at `0x87a26`).
For a pixel format with `DDPF_ZBUFFER` (`0x400`) and without `DDPF_D3DFORMAT`
(`0x200000`) it returns internal format `0x13` (S8Z24) when the option is set
(`0x8793c..0x87967`).  It forces every legacy Z-buffer request to S8Z24; it
does not change a register. [PROVEN]

### 1.2 Linux `mtx_parhelia.o`

`mtx722` (`0x74950`) registers the same names (plus the Linux-only ones) with
`mtx2302(obj, &ok, key, name, 0, default)`; `mtx796` (`0x355e0`) builds the
validated object at `dev+0x2198`.  [PROVEN]

| name | param slot, default | validated slot | readers |
|---|---|---|---|
| `EnableZinit` | `+0x60`, `(caps & 0x3c0) != 0` (`0x74c02` `testl $0x3c0,0x20(%r12)`) | `+0x458` | `mtx237` `0x10c8f` (depth clear), `mtx741` `0x10f9f` |
| `EnableZCompSundog` | `+0x70`, 0 | `+0x460` | none |
| `ForceDepthToS8Z24Sundog` | `+0x78`, 0 (`0x765c0`) | `+0x468` | none |
| `DivByZeroRefRastSundog` | `+0x80`, 1 | `+0x470` | `mtx2501` `0x3fcfb` |
| `Force3DTextureToABSundog` / `…ToCDSundog` | `+0x88` / `+0x90`, 0 | `+0x478` / `+0x480` | `mtx3285`, `mtx3708` |
| `Force3DTextureTextiledTo{AB,CD}Sundog` | `+0x98` / `+0xa0`, 0 | `+0x488` / `+0x490` | `mtx3285`, `mtx3708` |
| `Use2DClearForceZUncompSundog` | `+0xa8`, 0 | `+0x498` | none |
| `Use3DClearSundog` | `+0xb0`, 1 | `+0x4a0` | none |
| `UseZCompInMultContextSundog` | `+0xb8`, 1 | `+0x4a8` | none |
| `UseSundogMemoryStrategies` | `+0xc0`, 0 | `+0x4b0` | `mtx3721`, `mtx1464`, `mtx3952` |
| `ZUncompAnd2DClearSceneInProgSundog` | `+0xc8`, 0 | `+0x4b8` | none |

The Linux vendor stack therefore implements neither Z compression nor a
Sundog 3D clear; its only Sundog-specific memory behaviour is the optional
strategy of §5.  Its fast-clear arm (`pDev->[0x2188]`, documented in
`parhelia-rendertarget.md` §2.3) is behind `EnableZinit` at `0x10c8f` and is
therefore off on P690. [PROVEN]

`m9xdriver-x86_64-1.4.3-…` binds only PCI device `0x0540`
(`kernel/common/m9x_common.h:42`, `kernel/src/m9x_base.c:41`).  Its
`m9x2537`/`m9x2539` strings are obfuscated symbol names, not device IDs, and
it contains no Sundog strings.  It is not a source for this topic. [PROVEN]

---

## 2. DWGCTL control words and the packet form

The Sundog-class HAL has three one-shot DWGCTL command emitters.  All use the
DMA packet grammar `[count][4 packed indices, 0x21 = pad][count values]`,
index `i` → MMIO `0x1000 + 4*i`.  "normal" below is the running DWGCTL,
`[[pdev+0x44]+4]+0x4c`, rewritten immediately so the command is one-shot.

### 2.1 Mode command `0x9f270(cmd, a, b)` [PROVEN]

```
if (caps & 0x7fc00) {                           /* 0x9f278 */
        mode = 0x00a | (a ? 0x200 : 0) | (b ? 0x800 : 0);
        if (mode == cached_0x1d3f2c) return;    /* 0x9f2a6 */
        cached_0x1d3f2c = mode;
        if (zcomp_active_0x1ede18)              /* sync only with ZComp */
                emit(3, 0x21afafc0, 0x207, tag|1, tag);   /* 0x1300,0x12bc,0x12bc */
        emit(2, 0x2121c0c0, mode, normal);      /* 0x1300 twice */
} else {                                        /* Parhelia path */
        mode = a ? 0x20a : 0x00a;               /* cache 0x1ede20 */
        emit(2, 0x2121c0c0, mode, normal);
}
```

`tag` is `[[pdev+0x44]+4]+0x1c` after adding `0x10`.  The same routine exists
in `MTXPARD.dll` as `sub_9DCC0` (`parhelia-faa-command-buffer-followup.md`).

### 2.2 Z state reset `0x9f160(cmd, force)` [PROVEN]

```
w = (zcomp_active ? 0 : 0x200) | 0x108;         /* 0x9f16b..0x9f1fa */
if ((cached_0x1d3f28 & 0x200) == (w & 0x200) && !force) return;
if (zcomp_active) emit sync (as above);
emit(2, 0x2121c0c0, w, normal);                  /* 0x108 or 0x308 */
cached_0x1d3f28 = w;  flag_0x1ede24 = 1;
```

### 2.3 Z-init load `0x9eff0(cmd, value, depthf, zsurf, reset)` [PROVEN]

Reached only from `0xa0a20` for `caps & 0x7fc00` (`0xa0a51`):

```
bit9 = zcomp_active ? 0 : 0x200;
if (zcomp_active) emit sync;
if (reset)
        emit(2, 0x2121c0c0, bit9 | 0x108, normal);            /* 0x108 / 0x308 */
else {
        zsurf->[0x78] = depthf;                               /* float clear depth */
        emit(3, 0x21c0c0ad, value, bit9 | 0x008, normal);     /* 0x12b4, 0x1300, 0x1300 */
}
flag_0x1ede24 = 1;
```

`value` is the packed Z/S word produced by `0x9c220` (§4.3).

### 2.4 What the words mean

| word | issued by | reading |
|---|---|---|
| `0x0000000a` | `9f270(0,0)` | Z tracking **off** for the bound depth surface [PROVEN usage, INFERRED meaning] |
| `0x0000020a` | `9f270(1,0)` | Z tracking **on** for the depth surface bound in slot 1 [same] |
| `0x00000a0a` | `9f270(1,1)`, only in the decompress pass `0xa4070` | tracking on + **decompress** while the rectangle is drawn [INFERRED] |
| `0x00000108` | `9f160` with ZComp active | reset tile state for a newly armed **compressed** surface [INFERRED] |
| `0x00000308` | `9eff0(reset)` / `9f160` without ZComp | reset/discard Z-init tile state, no compression [INFERRED] |
| `0x00000008` + `0x12b4` | Z-init load with ZComp | set all tracked tiles to "cleared to `0x12b4`" [INFERRED] |
| `0x00000208` + `0x12b4` | Z-init load without ZComp | same, Z-init only [INFERRED] |
| `0x00000207` + `0x12bc` pair | before every mode change while ZComp is active | pipeline sync with fence tag [PROVEN form] |

So opcode `0xa` switches tracking, opcode `8` loads or resets tile state, bit 9
of opcode 8 means "not compressed", and bit 8 of opcode 8 means "reset, no
value".  **Correction:** `parhelia-faa-command-buffer-followup.md` names
`0x20a` "enter fragment accumulation".  In `mtxpserd.dll` the same routine is
called with `(1,0)` from the depth-clear, draw-validate (`0x102c80`) and
context-switch paths, always keyed to the Z surfaces at `pDev+0xa670/0xa674`.
The FAA naming is not supported by this binary; FAA may share the mechanism,
which is OPEN.

---

## 3. Z compression (`EnableZCompSundog`) and Z-init (`EnableZinit`)

### 3.1 State [PROVEN]

| variable | meaning |
|---|---|
| `pDev+0xa670` | the one depth surface that owns compression state ("ZComp surface") |
| `pDev+0xa674` | the one depth surface that owns Z-init (fast clear) state |
| `0x1ede18` | "compression active for the current clear" (set in the clear, §3.2) |
| `0x1ede24` | "tile state reset already done in this scene"; cleared at end of scene (`0xde5d7`, next to `dev+0x850c = 0` = scene-in-progress flag) |
| `0x1d5ab0` | element count of the list object at `0x1d5aa8` (methods `0x10329c`, `0x1037f4`); compared with 1 — **INFERRED** number of live D3D contexts |

`EnableZCompSundog` has exactly the two readers listed in §1.1, both inside the
Sundog clear `0xa0a60`.  Nothing on the allocation, surface-creation or
descriptor paths reads it: **no tag or compression memory is allocated** and
the depth descriptor is unchanged.  Where the hardware keeps per-tile state is
OPEN; the only size limit in the code is the Z-init coverage limit of §3.3.

### 3.2 When a surface becomes compressed [PROVEN]

`0xa0a60` (the D3D clear used when `caps & 0x7fc00`, selected at `0xa139b`)
sets `0x1ede18 = EnableZCompSundog` only if all hold (`0xa0b4d..0xa0be2`):

* internal depth format ∈ {`0x13..0x19`, `0x1b`, `0x1c`} — every 32-bit Z
  format except `0x1a`; never D16/D15S1;
* the format descriptor field `+4` equals 3;
* `(byte [this+0x12]) & 7 == 2` (meaning OPEN).

Ownership (`0x9f3b0`, the non-Z-init clear helper):

```
A: zsurf != a670
   A1 a670 != 0             -> 0x00a; 2D fill            (other surface owns state)
   A2 a670 == 0 && zcomp:
        if (nctx == 1 || UseZCompInMultContextSundog) {
                0x20a; 0x108 (9f160 force); a670 = zsurf;
                first clear in scene -> 2D fill, else continue at C
        } else  0x00a; 2D fill
   A3 a670 == 0 && !zcomp   -> 0x00a; 2D fill
B: zsurf == a670
   B1 nctx != 1 && !UseZCompInMultContextSundog -> decompress (0xa4070); 0x00a; 2D fill
   B2 otherwise -> 0x20a; if !flag_0x1ede24 { 0x108; 2D fill } else continue at C
C: if (Use2DClear…flag)   { if zsurf == a670 decompress; 2D fill }
   else if (use2D arg)    2D fill
   else                   3D clear (0xa4320), bracketed by 0x20a/0x00a when owner
```

`use2D arg` is 1 unless a scene is in progress (`dev+0x850c`), compression is
active, and the clear is partial or not depth+stencil (`0xa0be8..0xa0c66`).
The "force 2D" flag is `Use2DClearForceZUncompSundog`, or
`ZUncompAnd2DClearSceneInProgSundog` in that same in-scene partial case.  The
names match the code: "use a 2D clear, forcing a Z uncompress first", and
"in a scene, uncompress and 2D-clear instead of the 3D clear".

The per-draw validator `0x102c80` (called from every D3D draw entry
`0x16d85b…0x180f45`) re-arms: `0x00a` if the owning surface is not this
context's Z; `0x20a` if it is (or if it is the Z-init surface); it also saves
the previous context's state through `0x9f5b0` when `pDev+0xa9e4` changes.

### 3.3 Z-init fast clear (only with `EnableZinit`, off by default on P690) [PROVEN]

In `0xa0a60` (`0xa0c90..0xa0e28`), for a full-surface clear that is
depth+stencil, or of a format without stencil, or of a surface not flagged
`+0x368 & 8`:

```
if (a674 != zsurf && a674 != 0) { 0x00a; normal clear path }
0x20a;                                  /* 9f270(1,0) at 0xa0d5c */
a674 = zsurf;  if (!a670 && zcomp) a670 = zsurf;
if (W*H*Bpp > 0x3c0000) {               /* 0xa0d95 */
        R = (0x3c0000 / (Bpp*512)) / ((W+31)>>5) * 16;
        a674 = a670 = 0;
        2D fill rows [R, H) normally;   /* 0xa0dde */
        a674 = a670 = zsurf;
}
Z-init load: 0x12b4 = value; DWGCTL 0x008 (ZComp) or 0x208 (Z-init only)
```

The Sundog-class coverage is `0x3c0000` bytes (983040 pixels of 32-bit Z) in
32×16-pixel units; the Parhelia path `0xa0e90` uses `0x300000` bytes in 32×32
units (`0xa105e`, `shl 0xa`).  Such a fixed byte limit, independent of surface
count, points to fixed on-chip Z-init state [INFERRED].  **Measured on P690
the coverage is 640 tiles of 4096 bytes = `0x280000`, two thirds of the
software limit; see `parhelia-p690-fast-depth-clear.md` section 4.**

Leaving the Z-init surface (`0xa4640`, argument 1 = resolve):

```
0x20a (if not cached)
draw full-surface rectangle, helper block [[pdev+0x21e4]+0x10]:
        0x12c8 = 0x00000003 | stencil_bits << 17   (depth test ON, EQUAL)
        0x12d0 = 0x00000100                         (depth write, no stencil)
        0x1324 = 0                                  (no colour write)
        DWGCTL = 0x003000cc,  vertex Z = zsurf->[0x78] (the clear depth)
a674 = 0
if (!zcomp) DWGCTL 0x308 (9eff0 reset)
0x00a
```

With argument 0 (surface destruction, `0xdd0b1`) the rectangle is skipped.
A depth-EQUAL rectangle with depth write therefore materialises the clear value
into every tile that still reads as "cleared" [INFERRED].

### 3.4 The Z-uncompress pass `0xa4070` [PROVEN sequence]

Only when `0x1ede18` is set:

```
0xa0a                                  /* 9f270(1,1) at 0xa40dc */
helper block [[pdev+0x21e4]+0x14] (initialised by 0x2ddf0):
        0x12c8 = 0x00104991   depth test ON func NEVER, stencil ON func GT,
                              sfail/zfail/pass REPLACE, 8 stencil bits
        0x12cc = 0x000000ff   (mask 0xff, ref 0)
        0x12d0 = 0x00000100   (depth write on, stencil write mask 0)
        0x1324 = 0            (PLNWT, no colour write)
        0x1228/0x122c = Z surface base/cfg (slot 1), 0x1224 pitch = Z width
        DWGCTL = 0x0030000c,  guard band field 0x41f80000 (31.0)
full-surface rectangle, vertex Z = 0:
        tag 0x605e5d58: 0x1160=1.0 0x1174=Z 0x1178=0    0x1180=0
        tag 0x605e5d59: 0x1164=1.0 0x1174=Z 0x1178=0    0x1180=W
        tag 0x605f5d5a: 0x1168=1.0 0x1174=Z 0x117c=H    0x1180=0
optional 0x1148 = 0 before / 1 after (validated option +0x3c0, identity OPEN)
0x20a                                  /* 9f270(1,0) at 0xa430b */
```

No fragment can pass depth NEVER, so the pass writes no fragment data; the
decompression must be done by the Z unit while mode `0xa0a` is active
[INFERRED].

It runs [PROVEN]:

* before a CPU lock of an S8Z24 surface while compression is active (`0xf1f61`
  in the lock path `0xf1ef0`);
* before a 2D clear of the owning surface when forced (§3.2 C) and when
  multi-context compression is disallowed (§3.2 B1);
* at device/context teardown when `a670 != 0` (`0xa4ae8` in `0xa4a90`,
  followed by `a670 = 0; 0x00a`).

It does **not** run on surface destruction (`0xa1c50`: `a670 = 0; 0x00a`),
on a context switch (`0x102c80` only issues `0x00a/0x20a`), or for scanout
(depth is never scanned out).  Everything that reads Z outside the 3D Z unit —
CPU, 2D blit/fill of a partial rectangle, sampling Z as a texture — must see a
decompressed surface. [INFERRED from the placement of the calls]

### 3.5 Is it on by default on P690?

No.  Z compression needs `3D.EnableZCompSundog = 1`; Z-init needs
`3D.EnableZinit = 1` (the option lives in the `3D` section, not `DirectX`:
the `DirectX` string at `mtxpserd.dll` `0x10ac64` belongs to the preceding
`DirectX.HwPureDevice`); neither default is true for `caps = 0x10000..0x40000`,
the INF does not set them, and the Linux vendor driver does not implement
compression at all.  The vendor shipped both disabled on Sundog-class chips;
treat them as experimental. [PROVEN]

---

## 4. Depth clear paths on the Sundog class

### 4.1 Default P690 path: 2D solid fill [PROVEN]

With both options at their defaults, `0xa0a60` → `0x9f3b0` → case A3 →
`DWGCTL 0x00a` (emitted only if the cached word differs) → `0xa1f20` →
DeviceBitmap fill `0x115ad0` → 2D fill emitter `0x1122d0`, which programs
FCOL `0x1314` = packed value (`+0xa94`), PLNWT `0x1324` = byte mask
(`+0xa88`), DSTXY `0x1308`, LEN `0x138c`, and the destination base/cfg.  The
request operation code passed by the clear is `0xc0`.  This is our current
`ph_clear_zs` 2D fill, and our `ph_zs_write_mask()` equals the vendor mask.

### 4.2 The 3D clear `0xa4320` [PROVEN]

Helper block `[[pdev+0x21e4]+0x18]` (initialised by `0x2df00`: `0x12c8 = 6`,
`0x12cc = 0xff`, `0x12d0 = 0x1ff`, PLNWT 0, DWGCTL `0x0030000c`,
`0x1224 = 0x32030000`, guard band `31.0`).  The stencil-bit field is set
from the format (`0x9c010`, below), then by clear mask (`0xa43a0..0xa4405`):

| mask | `0x12c8` (S8Z24 case) | `0x12cc` | `0x12d0` |
|---|---|---|---|
| depth only | `(x & ~0x10) \| 0xf` → `0x0010000f` (depth ALWAYS, stencil off) | `0x000000ff` | `0x000001ff` |
| depth+stencil | `(x & 0xfffe49ff) \| 0x49ff` → `0x001049ff` (depth ALWAYS; stencil ALWAYS, REPLACE ×3) | `(sref << 8) \| 0xff` | `0x000001ff` |
| stencil only | `(x & 0xfffe49fe) \| 0x49f0` → `0x001049f6` (depth test off) | `(sref << 8) \| 0xff` | `0x000001ff` |

Colour is protected by PLNWT `0x1324 = 0`; slot 1 is the Z surface; slot-0
pitch is set to the Z width; the rectangle (same three vertex tags as §3.4)
carries **vertex Z = the float clear depth** (`rec+8`).  It is used only in the
in-scene, compression-active case of §3.2 C.  Since our measured depth-write
fill (298 Mpixel/s) is below the 2D fill (468 Mpixel/s), a 3D clear is not a
win without compression.

`0x2e010` builds a third helper block (`+0x10`, used by the Z-init resolve):
`0x12c8 = 3`, `0x12d0 = 0x100`, DWGCTL `0x003000cc`.

**Correction to `parhelia-rendertarget.md` §2.3:** the Linux clear emitter
`mtx2187` re-emits `0x12C8/0x12CC/0x12D0/0x12FC` from the current state
shadow; they are the DEPTH_STENCIL control, stencil mask/ref and write-mask
registers (`parhelia-pixel-pipeline-complete.md` §3.4) and the raster mode
word, not clear values.

### 4.3 Packed clear value `0x9c220` [PROVEN]

Switch on the internal depth format (`jmp [eax*4+0x9c5e8]`, `eax = fmt-0x11`):

| fmt | layout | value | mask when depth / stencil cleared |
|---|---|---|---|
| `0x11` | D16 | `z16 \| z16<<16`, `z16 = min(d*65536, 0xffff)` | `0xffffffff` |
| `0x12` | D15S1 | `((z15<<1)\|s) * 0x10001` | `0xfffefffe` / `0x00010001` |
| `0x13` | S8Z24 | `s<<24 \| z24`, `z24 = min(d*16777216, 0xffffff)` | `0x00ffffff` / `stencil_write_mask<<24` |
| `0x14` | X8Z24 | `z24` | `0xffffffff` |
| `0x16..0x1c` | Z24 + 1..7 stencil bits at the top | `s<<(32-n) \| z24` | `0x00ffffff` / top n bits |
| `0x15` | special 7-bit variant (`s<<25`, rescale by `/0x7f`) | — | `0x01000000` base |

For our S8Z24 clear, `0x12b4` (Z-init) or FCOL (2D) = `(stencil << 24) |
round(depth * 16777216)` clamped to `0xffffff`; depth 1.0 → `0x00ffffff`.

`0x9c010` returns the stencil-bit count written to `0x12c8[20:17]`:
`0x11→0, 0x12→1, 0x13→8, 0x14→0, 0x15→7, 0x16→1, 0x17→2, 0x18→3, 0x19→4,
0x1a→5, 0x1b→6` (table `0x9c068`).  [PROVEN]

---

## 5. "AB" and "CD" memory

### 5.1 The Linux strategy tables [PROVEN]

`mtx1464` (`0x60a10`) builds a 9 × 23 table of allocation-policy pointers
(`[mgr+0x198][0] + 0x100 + 8*(class*23 + usage)`, loops at `0x60bfb` and
`0x60cee`).  For `caps & 0x7fc00` it calls `mtx3285` (or `mtx3708`) **only if
`UseSundogMemoryStrategies`** is set (`0x60db5`, `0x60dde`); otherwise it uses
`mtx1318` when the memory description has a second range (`[cfg+0x70] != 0`,
`0x60c4d`) or `mtx2025` when it has not.

In `mtx3285` (`0x5d160`), usage slot 9 (`+0x148`, 3D textures) and slot 10
(`+0x150`, textiled textures) are:

```
AB only -> policy mgr+0xa8      CD only -> policy mgr+0xb0      else -> mgr+0x138
```

(`0x5d223..0x5d290`, `0x5d306..0x5d35d`, and the second class at
`0x5d433..0x5d5b1`).  Setting both AB and CD is the same as setting neither.

### 5.2 What `mgr+0xa8` and `mgr+0xb0` are [PROVEN chain]

`mtx1638` (`0x5dd00`) registers, for memory class `i`, the heaps in vector 0
of the class object `[[mgr+0x198]+8*i]` with policy `mgr+0xa8` (`0x5de30`,
`mtx4031`) and the heaps in vector 1 (`+0x20`) with policy `mgr+0xb0`
(`0x5de00`, `mtx3000`).

`mtx2157` (`0x5f470`) fills those vectors from the memory description
`cfg = [[pdev]+0xa0]`: vector 0 from `(cfg+0x30, +0x34)` / `(cfg+0x40,
+0x44)`, vector 1 from `(cfg+0x80, +0x84)` / `(cfg+0x90, +0x94)` only when the
sizes are non-zero (`0x5fe65`, `0x5fbed`).  Every heap is then split by the
32-MiB/8-MiB interval splitter `mtx2744` (`parhelia-p690-native-target-regions-re.md`).

The planner `mtx1876` (`0x56290`) writes `cfg` from the two bank sizes
`[dev+0x10]->+0xb8` (bank A) and `+0xbc` (bank B).  For 64 MiB + 64 MiB
(`0x565d8`):

| field | value | range |
|---|---|---|
| `+0x30/+0x34` | `0` / `0x3000000 − [+0xc]` | class 0, bank A `[0, 48 MiB − reserve)` |
| `+0x40/+0x44` | `0x3000000` / `0x1000000` | class 1, bank A `[48, 64) MiB` |
| `+0x80/+0x84` | `0x08000000 + [+4]` / `0x3000000 − [+4] − [+0x14]` | class 0, bank B, engine `0x08200000…` (with `[+4] = 0x200000`) |
| `+0x90/+0x94` | `0x0b000000` / `0x1000000` | class 1, bank B, engine `[176, 192) MiB` |

Therefore **AB = the bank-A range (engine `0x00000000..0x03ffffff`) and CD =
the bank-B range (engine `0x08000000..0x0bffffff`)** of each memory class on a
128-MiB P690.  That the vendor names them after pairs of memory channels
(A+B, C+D), i.e. that banks A and B sit on the two SUN90 memory controllers
(controller 1 / 2 in `HWS90InitializeMemoryController`), is INFERRED.
Semantics of the default texture policy `mgr+0x138` (a composite policy
object built by `mtx3348`) and of the reserve fields `[+4]`, `[+0xc]`,
`[+0x14]` are OPEN.

### 5.3 Vendor default placement on P690

* **Linux:** `UseSundogMemoryStrategies = 0` → `mtx1318`.  Its class-0 row
  sends slot 0 and slot 21 to CD, slots 3, 12, 14, 18, 19 to AB, and 3D
  textures (slot 9) to `mgr+0x138` (CD only when `System.DebugPatches & 1`,
  `0x5dfac`).  The usage-index → surface-type mapping (which slot is colour,
  which is Z) is OPEN, so the vendor's colour/Z bank is not yet proven.
* **Windows P-series:** no AB/CD options.  The memory-policy builder
  `0x3a160` uses `Q = 0x2000000` on `caps & 0x70000` (`0x3a171`) and
  `ForceIntensityInCDSundog` (default 0) only redirects one surface class
  (`0x3a448`).  Its usage mapping is also OPEN.

For our layout (colour `[32,64)` MiB = bank A, Z24S8 `[96,128)` MiB = bank B,
textures in bank A) the relevant question — whether separating texture and
colour into different banks helps — is a hardware measurement (§7, T1).

---

## 6. Depth surface format code [PROVEN]

The static initialiser `0x14f20` fills 0x34-byte records
`{flags, fmt, usage, hwcode, …}` at `0x1e05d0..0x1e5710` (via `0x14e00` and
direct stores; reconstructed by replaying the initialiser).  Depth rows:

| internal fmt | usage 4 (Z buffer) | usage 5 | usage 0 (2D view) | usage 1 / 7 |
|---|---|---|---|---|
| `0x11` D16 | `0x2a` | `0x2a` | `0x20` | `0x28` |
| `0x12` D15S1 | `0x2b` | `0x2b` | `0x20` | `0x28` |
| `0x13` S8Z24 | **`0x36`** | `0x36` | `0x30` | `0x32` |
| `0x14` X8Z24 | `0x36` | `0x36` | `0x30` | `0x32` (usage 2 also `0x32`) |
| `0x15..0x1c` | `0x36` | `0x36` | `0x30` | `0x32` |

Usage 4 occurs only for depth formats.  `0x33` belongs to colour formats
`0x0c/0x0e/0x23`, `0x38` to `0x23` (usage 3), `0x3a` to `0x25`, `0x3b` to
`0x2b`.  The only other place a depth code is hard-wired, `0x9f9a0`, builds
the 32-pixel helper surface at `pDev+0xa9e0` with cfg `(cfg & 0xf6fbc020) |
0x36030020` (format `0x36`, layout 3, pitch 32).  Our `0x36` for S8Z24 is
exactly the vendor code; compression does not change it.

**Correction to `parhelia-own-render-target.md` §3.3:** the depth code is
present in the P-series table (`0x36`); `ForceDepthToS8Z24Sundog` is read by
`mtxpserd.dll` (`0x87950`), only not by the Linux binaries.  The function
called "Z-uncompress / scene-begin pass `mtx2153`" there rebinds slot 1 to a
32-pixel helper surface and slot 2 to the context depth; it is not a Z
decompress (the Linux kernel has no compression consumers), and matches the
role of `mtxpserd` `0x9f5b0` (small helper draw plus a readback of eight
18-bit values, purpose OPEN).

---

## 7. Hardware test proposal (ordered, minimal)

Conventions: Z is S8Z24 bound in slot 1 as today (`0x1228` = bank-encoded
base, `0x122c = 0x36000000 | layout<<16 | pitch`).  "normal" = the DWGCTL our
batch uses for the following draw.  A control word is always written as the
pair `DWGCTL = word; DWGCTL = normal` inside one packet.  If a test hangs, stop
the sequence; each step only relies on the previous ones.  Verify through GPU
copy to an anchor and the scanout, never through raw BAR reads of bank B.

**T1 — bank placement (no new hardware feature).**  Keep 640×480, measure the
existing fill tests with these placements (engine addresses):

| run | colour | Z24S8 | textures |
|---|---|---|---|
| a (today) | `0x02000000` (A) | `0x0a000000` (B) | bank A |
| b | `0x02000000` (A) | free bank-A offset in `[0, 32)` MiB | bank A |
| c | `0x02000000` (A) | `0x0a000000` (B) | `0x08000000..` (B) |
| d | `0x0a000000` (B) | `0x02000000` (A) | bank A |

Expected if AB/CD are independent controllers: b slower than a for depth
test+write; c faster than a for textured+depth; d ≈ a.  This decides the
default placement policy before any Z trick.

**T2 — control-word acceptance.**  With Z bound and cleared by our 2D fill:
`0x1300 = 0x0000020a` (+normal), `0x1300 = 0x00000308` (+normal), draw the
existing depth-test scene, `0x1300 = 0x0000000a` (+normal).  Expected: no
lockup and a pixel-identical result to the same scene without the three
words (the reset discards any Z-init state).

**T3 — Z-init fast clear (vendor `EnableZinit` path, §3.3).**

1. 2D-fill Z with `0x00000000` (depth 0.0), FCOL `0x1314 = 0`,
   PLNWT `0x1324 = 0xffffffff`.
2. `0x1300 = 0x0000020a` (+normal).
3. `0x12b4 = 0x00ffffff`, `0x1300 = 0x00000208` (+normal).
4. Draw a full-screen quad at z = 0.5 with `0x12c8 = 0x001000e5` (test on,
   LESS, 8 stencil bits), `0x12d0 = 0x000001ff`.  Pass: all pixels drawn.
   Z-init ignored: nothing drawn.
5. Resolve: full-screen quad with `0x12c8 = 0x00100003` (EQUAL),
   `0x12cc = 0x000000ff`, `0x12d0 = 0x00000100`, `0x1324 = 0`, z = 1.0; then
   `0x1300 = 0x00000308` (+normal), `0x1300 = 0x0000000a` (+normal).
6. GPU-copy Z and check z24 ≈ `0x800000` under the step-4 quad and
   `0x00ffffff` elsewhere.

Then time 1000 × (steps 2–3) against 1000 × 2D fills, and the depth-test fill
rate after each kind of clear.  Keep `W*H*4 ≤ 0x3c0000`; above it, 2D-fill
rows `[R, H)` with `R = (0x3c0000/2048)/((W+31)>>5)*16` (1280×1024 → R = 768).
Step 5 is mandatory before any 2D, CPU or texture access to that Z.

**T4 — Z compression (vendor `EnableZCompSundog` path, §3.2/3.4).**  Only
after T3 passes.  Before every mode change emit the vendor sync
(`0x1300 = 0x00000207`, `0x12bc = tag|1`, `0x12bc = tag`; or our existing
fence packet).

1. Arm: `0x1300 = 0x0000020a` (+normal), `0x1300 = 0x00000108` (+normal).
2. Clear with our 2D fill (vendor does this right after arming).
3. Draw the depth-test+write fill test; measure (baseline 298 Mpixel/s).
4. Decompress: `0x1300 = 0x00000a0a` (+normal); full-screen quad with
   `0x12c8 = 0x00104991`, `0x12cc = 0x000000ff`, `0x12d0 = 0x00000100`,
   `0x1324 = 0`, z = 0; `0x1300 = 0x0000020a` (+normal);
   `0x1300 = 0x0000000a` (+normal).
5. GPU-copy Z and compare bit-exactly with the same scene rendered without
   steps 1 and 4.

Decompress before CPU map, partial 2D fill/blit, sampling Z, binding another
Z surface as owner, and teardown.  Context switches only need
`0x0000000a`/`0x0000020a`.

**T5 — 3D clear under compression (optional).**  With T4 armed and inside a
frame: draw a full-screen quad with z = clear depth, `0x12c8 = 0x0010000f`
(depth only) or `0x001049ff` with `0x12cc = (sref<<8) | 0xff` (depth+stencil),
`0x12d0 = 0x000001ff`, `0x1324 = 0`.  Compare its cost with the 2D fill;
without compression this path is not expected to win (298 < 468 Mpixel/s).

## 8. Reproduction

```sh
D=xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll
objdump -d -Mintel --start-address=0x10b9f0 --stop-address=0x10bd20 $D  # options
objdump -d -Mintel --start-address=0x9eff0  --stop-address=0x9f660  $D  # 9eff0/9f160/9f270/9f3b0
objdump -d -Mintel --start-address=0xa0a20  --stop-address=0xa0e90  $D  # Sundog clear
objdump -d -Mintel --start-address=0xa4070  --stop-address=0xa4a90  $D  # decompress, 3D clear, resolve
objdump -d -Mintel --start-address=0x9c010  --stop-address=0x9c5e8  $D  # stencil bits, clear value
objdump -d -Mintel --start-address=0x2ddf0  --stop-address=0x2e120  $D  # helper blocks
objdump -d -Mintel --start-address=0x87930  --stop-address=0x879a0  $D  # ForceDepthToS8Z24Sundog
objdump -d -Mintel --start-address=0x14f20  --stop-address=0x19834  $D  # format table initialiser
K=matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -dr --start-address=0x40a70 --stop-address=0x40cb0 $K   # caps one-hot (mtx1916/mtx427)
objdump -dr --start-address=0x74950 --stop-address=0x765f0 $K   # option registry (mtx722)
objdump -dr --start-address=0x5d160 --stop-address=0x5e440 $K   # AB/CD strategies
objdump -dr --start-address=0x56290 --stop-address=0x56740 $K   # bank range planner (mtx1876)
```
