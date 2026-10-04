# FAA-16x on the original Parhelia (0x0527/0x0528) - static RE

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Static reverse engineering only: no vendor driver was run and no target was
touched.  This builds on `parhelia-antialiasing.md` (sections 0-9a),
`parhelia-faa-command-buffer-followup.md`, `parhelia-fragment-gating-re.md`
and `parhelia-gl-limits.md` section 6, and **corrects** several of their
FAA statements (section 8).

Labels: **PROVEN** = instruction-level evidence in a vendor binary (address
given), usually in both the Windows and the Linux binary; **DERIVED** = follows
from proven pieces by a short argument stated next to it; **INFERRED** =
consistent reading, not proven; **OPEN** = not recovered.

| input | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` (image base 0x10000) | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |

Chip-class word (`parhelia-gl-limits.md` 6.1): 0x0527 = `0x0002..0x0020`,
**0x0528 = `0x0040..0x0200`** (rev 6 = `0x0200`), LX/Sunex/P690 = `0x7fc00`.
The masks below therefore read: `0x3e` = 0x0527 only, `0x3c0` = 0x0528 only,
`0x7ffc0` = 0x0528 + LX class, `0x7fc00` = LX class only, `0x70000` = P690.

## 0. Answer up front

FAA is **not** switched on by a DWGCTL command on any family.  The
`DWGCTL = 0x20a/0x00a` one-shots in `sub_9DCC0` are the Z unit's tile
tracking (antialiasing doc 9a.0) and are skipped on the original family
anyway.  FAA on the original Parhelia is an ordinary state-block feature:

1. **`0x12fc` (RASTERMODE) is the AA control word** - the shadow
   `[block+0xa0]` the older docs could not place.  Bits `[1:0]` = FAA mode
   (0 off, 3 = fragment-buffer accumulation), `[31:14]` = fragment-buffer
   size / 256, `[6:5]` = ForceCompressedMask and bit 3 = AdjustZ (both
   0x0528-only).  `ThresholdAA` (`[block+0xa4]`) is **`0x12f8`**.  [PROVEN]
2. **The fragment buffer is bound in surface slot 2**: `0x1230` = its
   VRAM base, `0x1234` = `0x10000020` (format code 0x10, type 0, pitch 32).
   [PROVEN]
3. Entering mode 3 also rewrites **`0x12b0` (MemLevels)**
   `(v & ~0x18000) | 0xe0000`; a change of `0x12fc` bit 1 forces a
   pipeline flush.  [PROVEN]
4. The **resolve** (`FRAG_RESOLVE`) is `sub_9E860`: it re-establishes the
   accumulation state (slots incl. the fragment buffer, Z/stencil, the FAA
   `0x12fc` word, PLNWT all ones, full scissor, `DWGCTL = 0xc4`, WARP setup
   planes zeroed) and then issues **`DWGCTL = 9`** followed by the normal
   DWGCTL.  On the original family the helper is `sub_2E360`; the LX helper
   only differs by `DWGCTL = 0x2000c4` and the 31.0 guard band.  [PROVEN for
   the sequence, INFERRED that the `9` write performs the merge]

The Linux `mtx_parhelia.o` carries the same fragment-buffer class (symbol
twins in section 7), so items 1-4 are confirmed by two independently compiled
binaries.

## 1. How the AA control word reaches the chip

### 1.1 The state block is split into atoms sharing one dirty word [PROVEN]

The state-block constructor `sub_C97B0` stores `&block[0x0c]` (the dirty
word) at the head of every emitter atom: `block+0x84`, `+0xa8`, `+0xc0`,
`+0x1d0`, `+0x768`, `+0x9fc`, `+0xa80` (`0xc987c..0xc989c`).
`ValidateState` `sub_C9AC0` calls one emitter per atom; atom field `+n` is
therefore block offset `atom + n`.

The raster atom `block+0x84`, emitter `sub_C9250` (dirty `0x60000000`):

```
dirty 0x20000000: count 4, indices ca cb bf be    (c92a0)
    0x1328 = [atom+0x04] = block+0x88      scissor min
    0x132c = [atom+0x08] = block+0x8c      scissor max
    0x12fc = [atom+0x1c] = block+0xa0      AA control / RASTERMODE
    0x12f8 = [atom+0x20] = block+0xa4      ThresholdAA
dirty 0x40000000: count 4, indices ba b9 bc bb    (c92f6)
    0x12e8 = block+0x90   0x12e4 = block+0x9c   0x12f0 = block+0x98   0x12ec = block+0x94
```

The block initialiser `sub_2C9F0` clears `[block+0xa0]` (`0x2ca6c`) and marks
`0x20000000` in its valid mask, and the FAA writers of `[block+0xa0]`
(`0xa04c9`, `0xa05de..0xa0621`, `0x9e0b9`) set `0x20000000` or
`0x24000000`; the size write at `0x9e809` relies on the `0x6400000c` set
by the fragment-context constructor (`0x9b5b2`).
`parhelia-fragment-gating-re.md` 5.4b already lists
`c9ad1 -> 0x1328,0x132c,0x12fc,0x12f8`; the FAA docs did not connect it.

The surface atom `block+0xa80`, emitter `sub_C98B0`:

```
dirty 0x04: count 4, 88 89 8c 8d  0x1220=[+0x24] 0x1224=[+0x34] 0x1230=[+0x2c] 0x1234=[+0x3c]
dirty 0x08: count 2, 8a 8b        0x1228=[+0x28] 0x122c=[+0x38]
dirty 0x10: count 2, 8e 8f        0x1238=[+0x30] 0x123c=[+0x40]
dirty 0x20: count 2, c6 c5        0x1318=[+0x10] 0x1314=[+0x14]
dirty 0x02: count 1, c9           0x1324=[+0x08]   (PLNWT)
dirty 0x80: count 4, c0 af af ac  0x1300=0x307, 0x12bc=tag|1, 0x12bc=tag, 0x12b0=[+0x54]
dirty 0x01: count 1, c0           0x1300=[+0x04]   (DWGCTL)
```

### 1.2 The `0x12fc` bit layout [PROVEN unless marked]

| bits | content | writer |
|---|---|---|
| `[1:0]` | FAA mode: 0 off, 1, 2, 3 (mode 3 = fragment buffer bound) | `sub_A0490` `0xa05cb..0xa0621` |
| 2 | OPEN (set in the Linux per-draw `0x0bb80024`) | - |
| 3 | AdjustZ registry value, **only if `caps & 0x3c0`** (0x0528) | `0xa059c..0xa05c8`; Linux `mtx2215` `0x11f3d` uses a device-class query instead |
| 4 | D3D render-state record `+0x0c` bit 5 (meaning OPEN) | `0xa057c..0xa0593` |
| `[6:5]` | ForceCompressedMask registry value, **only if `caps & 0x3c0`** | `sub_1396A0` `0x139740..0x139771` |
| `[13:7]` | preserved, OPEN | - |
| `[31:14]` | fragment-buffer size / 256 | `sub_9E690` `0x9e7e3..0x9e809`; Linux `mtx1858` `0xf28e..0xf2b2` (`size >> 8 << 14`) |

Mode selection (`sub_A0490`, mode chooser `sub_9FFE0`, jump table `0xa0684`):

| chooser result | `fragctx+0xa4` | `0x12fc[1:0]` |
|---|---|---|
| 0 (FAA with buffer) | 3 | 3, or 1 when the state record `+0x10` is 0 or 1 (`0xa05f3`) |
| 3 | 2 | 2 |
| 1, 2 | 0 | 0 |

`[1:0]` is forced to 0 when `fragctx+0x140 == 1` (`0xa0616`).  The values 1
and 2 are reachable but their hardware meaning is OPEN; the documented
`AAMode` nibble 2 path (destination alpha, compressed mask) is the most likely
user of mode 2 [INFERRED].

The ordinary non-FAA word that both our reference emitter and the Linux
capture use, `0x0bb80024`, has `[1:0] = 0`, `[6:5] = 1` and `[31:14] =
0x2ee0` (a size field of 3 072 000 bytes); points use `0x04000024`
(`0x1000` = 1 MiB) and the Linux reset value is `0x18004014` (`0x6001`).
With `[1:0] = 0` the size field is inert [INFERRED from the reference emitter
working on the 0528 with these values]; where the Linux stack gets the
non-zero sizes is OPEN.

### 1.3 Side effects of a mode change [PROVEN]

`sub_9AB30` (called at `0xa0670` with the old word, the raster atom and the
surface atom):

* bit 1 of `0x12fc` changed: set `0x100` in the SetupHardware flush flags
  (`[[block+0x30]+0x20]`), which makes `sub_2EC10` run `sub_2E190`: a
  `DWGCTL = (flags & ~0x408) | 6` flush with fence pair `0x12bc = tag|1, tag`
  and DWGCTL restore (`0x2e1e4`).
* `[1:0]` became 3: `0x12b0 = (MemLevels & ~0x18000) | 0xe0000`
  (`0x9ab66..0x9ab79`), otherwise back to the registry `MemLevels`
  (accessor `+0x2f8`, `0x9ab8d`).  Dirty `0x80` on the surface atom, i.e.
  emitted in the `0x307` fence packet of 1.1.  With the default
  `0x3fff8842` this gives `0x3ffe0842`.

None of this is gated by chip class.

## 2. The fragment buffer [PROVEN unless marked]

### 2.1 Allocation

`sub_9E690` (`fragctx = ctx+0x2dcc`), caps from `[ctx+0x1c]`:

| class | cap |
|---|---|
| `caps & 0x70000` (P690) | 32 MiB |
| `caps & 0x7fc00` (LX) | 8 MiB |
| **else (0x0527, 0x0528)** | **16 MiB** |

On allocation failure the size drops by 2 MiB and is retried while it is at
least 6 MiB (`0x9e760..0x9e79a`): **6 MiB is a retry floor, not a minimum** -
a first request below 6 MiB (4 MiB at 1024x768) is allowed.  The requested
size is the `FragSize * max(w*h/786432, sqrt(w*w+h*h)/1280) * 1 MiB` formula
of `sub_9FBB0` with `FragSize` 4.0 on the original family (5.4 on LX).  At
1920x1200 that is 11.7 MiB, under the 16 MiB cap.

`sub_9E570` builds the allocation descriptor at `fragctx+0x90`:

```
+0x90 address   = allocator result [+0x78] (low 2 bits cleared)    9e5e2, 9e657
+0x94 config    = (old & 0xd0f8c020) | 0x10000020 = 0x10000020     9e59f, old = 0
+0x98 layout    = (old & 0xf001c001) | 0x00010001
+0x9c size      = bytes
+0xa0 caps word
request: +0x04 = 8 (memory class), +0x10 = 0x00010004, +0x1c = bytes
```

Config `0x10000020` = format code `0x10` (an 8-bpp code in `mtx969`'s table),
type 0, pitch 32 pixels, bit 23 clear.  The buffer is then mapped and cleared
to zero by the CPU (`0x9e7a2..0x9e7d1`; Linux `LocMemSet` at `0xf25f`).

### 2.2 Binding: surface slot 2 [PROVEN]

`sub_A0490` case 0 (`0xa0536`):

```
surface atom +0x2c = fragctx+0x90   -> 0x1230 (slot 2 base)
surface atom +0x3c = fragctx+0x94   -> 0x1234 (slot 2 config) = 0x10000020
surface atom dirty |= 0x04          -> emitted with 0x1220/0x1224 (count 4, 88 89 8c 8d)
```

Linux twin `mtx2215` `0x12070`: `atom+0x30 = fragctx+0xc0`, `atom+0x40 =
fragctx+0xc4`, `dirty |= 4`, where the Linux shadow keeps slot `n` at
`+0x28+4n` (base) / `+0x38+4n` (config) (gating doc: `0x1230 <- dev[0xb30]`,
`0x1234 <- dev[0xb40]`), i.e. the same slot 2.

The base value has the encoding of every other surface slot base (the vendor
uses the allocator's descriptor address the same way for all surfaces)
[DERIVED].  Required alignment, and whether the buffer must lie inside the
16 MiB engine window of the colour target (as depth must,
`parhelia-depth-window-above-colour`), are OPEN.

Our open driver already writes slot 2 as `{0, 0x32020000}` (the null
descriptor) on every draw (`parhelia3d/p3d_state.c`), so slot 2 is free on
the 0528.

### 2.3 The companion state surface (not the fragment buffer)

`sub_9E3F0` allocates a second, device-wide 256-byte surface
(`pDev+0xa9e0`, memory class 5, config `0x36030020` = S8Z24 code, type 3,
pitch 32) and keeps its descriptor in `0x1d1f30..0x1d1f40`.  It is used only
by the context-switch retire pass (section 4).

## 3. Accumulation on a target

Per draw, while the target's FAA mode is 3, the primitive emitters call
`sub_9D1B0` (13 call sites `0x140122`, `0x16ad43` ... `0x17eff6`), which sets
`fragctx+0xe8 = 1` ("fragments pending") and snapshots the live state:
`0x12c8/0x12cc/0x12d0` (`block+0xb4..0xbc`) into `fragctx+0x100..0x108` and
all slot base/config words (`block+0xaa4..0xad0`) into `fragctx+0x10c..0x138`
(`0x9d210..0x9d32a`).  `fragctx+0xfc` holds the FAA `0x12fc` word
(`0xa062c`).  On the 0x0527 the pending flag is not set when
`fragctx+0x154 != 0` (`0x9d1f7`, `caps & 0x7ffc0`).

No DWGCTL change is needed to accumulate on the original family: the
`sub_9DCC0` packet (`caps & 0x7fc00`) is skipped there, and only the cached
word `ds:0x1ebe20` is updated.

## 4. Resolve and retire

### 4.1 Resolve: `sub_9E860` [PROVEN sequence]

Runs when `fragctx+0xe8` is set and the target state is 1 or 2.  Callers: the
mode chooser on target / mode change (`0xa0146`, `0xa0366`, `0xa03c9`,
`0xa040a`, `0xa0445`) and the D3D clear (`0x9f851`, `0x9fb16`).

1. If another context owns the fragment unit (`pDev+0xa9e4`), retire it
   (`sub_9E000`) and load this context's saved fragment-unit words
   (`sub_9D790`), see 4.2.
2. `sub_2EC10(block, 0, 1)`: flush the whole state block.
3. Re-emit slots 0 and 2 from the snapshot
   (`0x1220/0x1224/0x1230/0x1234 = fragctx+0x10c/+0x11c/+0x114/+0x124`),
   then slot 1 (`0x1228/0x122c = +0x110/+0x120`).  Slot 2 is the fragment
   buffer again.
4. Family helper (`0x9e9b9`): LX class `sub_2E560`; original family:
   `sub_2DC20(1)` when `[block+0x30]+0x2c` is set (only does work on the
   0x0527, `caps & 0x3e`), then **`sub_2E360(fc, 100, 104, 108)`**:

```
count 4, c9 20 b3 b4:          0x1324 = 0xffffffff   (pad 0)
                               0x12cc = saved +0x104  0x12d0 = saved +0x108
count 7, c0 cb ca b2 20 bf:    0x1300 = 0x000000c4
                               0x132c = 0x0fff0fff    0x1328 = 0
                               0x12c8 = saved +0x100  (pad 0)
                               0x12fc = saved +0x0fc  (FAA mode bits set)
count 0x22, WARP setup planes, 28 writes in this order (0x2e481..0x2e534):
  0x116c 0x1174 0x1178 0x1180 | 0x116c 0x1178 0x1180 0x1164 |
  0x1180=1/16 0x1168 0x1178=1/16 0x1180 | 0x116c 0x1174 0x1178 0x118c |
  0x116c 0x1174 0x1178 0x1190 | 0x116c 0x1174 0x1178 0x1194 |
  0x116c 0x1174 0x117c 0x1198, all others 0 - every register ends at 0,
  the two 1/16 (0x3d800000) writes are overwritten in the same packet
dirty |= 0x6400000f
```

5. `count 2, c0 c0: 0x1300 = 0x9, 0x1300 = <normal DWGCTL>` (`0x9ea4c`).
6. Stamp colour and depth surfaces (`sub_9D6E0` flags `0x100`, `0x200`:
   `0x1300 = 0x107 / 0x207`, `0x12bc = tag|1`, `tag`, restore).
7. Mark the whole block dirty (`-1`), `sub_2CE60(0x2400000f)` (bit 28 added
   on LX), `fragctx+0xe8 = 0`.

The LX helper `sub_2E560` emits the same words with `DWGCTL = 0x2000c4` and
additionally `0x12f4 = 31.0f`.  Linux twin: `mtx2954` (`0xf8e0`), family
test `testl $0x7fc00,0x20(%r8)` at `0xfa53`, original helper `mtx2187`, then
the `0x1300 = 9` pair at `0xfaba`.

Reading: the DWGCTL low nibble 4/9 pair with full scissor, zeroed setup
planes and the FAA word live is the vendor's `FRAG_RESOLVE` [INFERRED; it is
the only operation between snapshot and "pending = 0" that touches the
hardware, and it runs before every clear and target change].  Whether
`0x1300 = 0xc4` alone already starts work is OPEN.

### 4.2 Retire / activate: context switch only [PROVEN sequence]

`sub_9E000` (Linux `mtx2153`) saves a context's fragment-unit state: it binds
the companion surface (2.3) as slot 1 with `0x12c8 = (v & ~0xe0000) |
0x10000e`, `0x12fc = fragctx+0xfc | 3`, scissor max `0x00030005`, issues
`sub_9D560(0xc00)` (`DWGCTL 4, 0x407, 0xc07` fences), draws one small WARP
triangle (`0x1160..0x1180`, depth 0.5), fences with `DWGCTL 0x207`, then reads
dwords 0..3 and 32..35 of the companion surface (`& 0x3ffff`) into
`fragctx+0xc8..0xe4` and zeroes them.  `sub_9D790` (Linux `mtx3272`) loads
those eight words into `0x1340..0x134c` (indices `d0..d3`) and
`0x13c0..0x13cc` (`f0..f3`).

This pair runs only when a second D3D context takes over the fragment unit
(`0x101097`, `0x9e8b2`); for the first owner nothing is written to
`0x1340/0x13c0` (`sub_9E860` skips both when `pDev+0xa9e4` is 0).  A
single-owner open driver can therefore omit it [DERIVED], at the price of
resolving before handing FAA to another context.  The meaning of the eight
18-bit words is OPEN (allocation pointers into the fragment buffer would fit
an 18-bit field: 16 MiB / 2^18 = 64 bytes [INFERRED]).

## 5. Constraints for a driver

| item | original family | evidence |
|---|---|---|
| guard band 32 -> 31 while FAA is live | **not applied** (gated `caps & 0x7fc00`) | `sub_2CF90` `0x2cf92`; `sub_2E360` does not write `0x12f4` [PROVEN] |
| fragment buffer cap | 16 MiB, retry floor 6 MiB | 2.1 [PROVEN] |
| FragSize default | 4.0 | `0x109823` [PROVEN] |
| plane mask | resolve sets `0x1324 = 0xffffffff`; Linux comment "do not execute PLNWTMASK hack in FAA mode" | [PROVEN] / [V] |
| MemLevels | `0x12b0` adjusted while mode 3 | 1.3 [PROVEN] |
| render-to-texture / special RT | no FAA (`rt+0x360 & 0x10`, `rt+0x364 & 4`) | antialiasing doc 8 [PROVEN] |
| destination alpha | compressed-mask path, `0x12fc[6:5]` ForceCompressedMask (0x0528 only) | 1.2 [PROVEN], meaning [INFERRED] |
| clear | resolve before clearing an FAA target | `0x9f851`, `0x9fb16` [PROVEN] |
| target change, context change | resolve; retire/activate on context change | 4.1, 4.2 [PROVEN] |
| blending / stencil changes | the registry switches exist but are never read; no resolve on blend/stencil in 2.10.00.105 | antialiasing doc 5.3 [PROVEN negative] |
| 0x0527 only | FAA additionally needs either a per-target flag or `0x12e8 & 0x7fffff == 0x2021` (blend on, src ZERO, dst ONE) | `0xa0185..0xa01e3`; Linux `mtx2137` `0x11a94` [PROVEN]; 0x0528 takes the LX branch |
| Z compression / Z-init interplay | none on the original (those packets are `0x7fc00`-gated) | 9a.0 [PROVEN] |
| `0x3c0000` per-RT rule (`sub_9F7D6`) | belongs to Z-init (`pDev+0xa670/a674`), not FAA | 9a.0 correction [DERIVED] |

## 6. The 4x mode (no fragment buffer)

`AAMode` nibble 0/1 (bits `[3:1] = 0`) never allocates a fragment buffer and
never sets `0x12fc[1:0]`.  Instead the D3D path treats it as ordered-grid
**supersampling**: per-axis scale `1 + 0.25 * AAMode[27:24]` /
`1 + 0.25 * AAMode[31:28]`, default 2.0 (`0x40000000`), clamped so that the
scaled width/height stays at most 2048 (`0x45000000`), rounded down to
quarter steps (`sub_D8F20` `0xd8f2e..0xd9060`); the guard-band extents
(`+-8160.0`) are divided by the same scale (`0xd6d16..0xd6e12`)
[PROVEN for the arithmetic].  Rendering into an enlarged target followed by a
filtered downscale is the consistent reading [INFERRED; the downscale blit was
not traced].

For the open driver this is a pure software technique that needs no new
register: render at 2x2 and box-filter on resolve.  It is cheaper to support
first and risk-free, but costs 4x fill and stays limited to 1024-pixel-wide
targets at 2x (2048-pixel engine limit), so it gives no AA at 1920x1200.

## 7. Linux cross-check

`mtx_parhelia.o` does not read the `AAMode` switch (antialiasing doc 2.3), but
it does contain the whole fragment-buffer class, compiled from the same core:

| Linux | MTXPARD | role |
|---|---|---|
| `mtx3712` (per-draw validate) -> `mtx2215` (`0x11e50`) | `sub_A0490` | `0x12fc` FAA bits, slot 2 binding |
| `mtx2137` (`0x119a0`) | `sub_9FFE0` | mode chooser, same `0x3c0`/`0x7fc00` gates, 0x0527 blend test `0x2021` |
| `mtx1858` (`0xefb0`) | `sub_9E690` | size clamp 32/8/16 MiB, 2 MiB retry, `0x12fc[31:14] = size >> 8` |
| `mtx2372` (`0xcc30`) | `sub_9D1B0` | snapshot |
| `mtx2954` (`0xf8e0`) | `sub_9E860` | resolve, `0x1300 = 9` |
| `mtx2153` (`0xcf80`) | `sub_9E000` | retire, same WARP triangle `0x605e5d58` |
| `mtx3272` (`0xd7b0`) | `sub_9D790` | load `0x1340..` / `0x13c0..` |

The Linux per-draw path therefore runs the FAA raster setter on every draw,
with the mode selector coming from a state field that nothing sets (mode off,
`[1:0] = 0`), which matches the captured `0x0bb80024`.  The Linux GL stack
exposes no multisample visual, so it cannot be driven to FAA without patching;
the field feeding `mtx2137` (`[arg+0x0c] & 0xf == 3`, `[arg+0x29] == 3`) is the
place a vendor-stack experiment would set [OPEN which GL state slot reaches
it].

## 8. Corrections to the existing FAA documents

* `[block+0xa0]` / `[block+0xa4]` are emitted as **`0x12fc` / `0x12f8`** by
  `sub_C9250`; the "emitter not found" statements in
  `parhelia-antialiasing.md` 4.1, 9a.1, 9a.6 and the follow-up doc are
  superseded.  The gating doc's "bits [1:0] = cull select" for `0x12fc` is
  wrong: they are the FAA mode (cull is `0x1130` bit 0, as
  `parhelia-3d-integrated.md` states).
* The fragment buffer is bound in **slot 2** (`0x1230/0x1234`) while
  fragments accumulate; antialiasing doc 9a.6 "only the retirement pass binds
  it" is superseded.
* Antialiasing doc 9a.3/9a.4: the `0x36030020` descriptor in
  `0x1d1f30..0x1d1f40` is the 256-byte **companion state surface**
  (`pDev+0xa9e0`), not the fragment buffer; retire binds that surface as
  slot 1 and keeps the fragment buffer in slot 2.
* Antialiasing doc 9a.5: `sub_9E860` is the **resolve**, not a plain restore;
  it re-emits the FAA `0x12fc` word and ends with `DWGCTL = 9`.  The two
  1/16 writes of the WARP packet go to `0x1180` and `0x1178` and are
  overwritten by later writes of the same packet; the order is kept.
* The 6 MiB figure is a retry floor, not a minimum allocation.
* The guard-band reduction to 31.0/15.0 applies to the LX class only.

## 9. Minimal 0528 sequence and what is still open

Single context, one colour target, FAA on [evidence per line]:

```
alloc  fb = VRAM, size S = min(FragSize*scale*1MiB, 16 MiB), zero-filled    PROVEN (size/clear), OPEN (alignment, window)
bind   0x1230 = base(fb), 0x1234 = 0x10000020  (with 0x1220/0x1224, 4-reg group)  PROVEN
       0x12f8 = 0x007fffff (ThresholdAA default)                             PROVEN
       0x12fc = (S >> 8) << 14 | 0x20 [6:5]=ForceCompressedMask | 3          PROVEN fields, 0x20/bit 3 0528-only
       DWGCTL (flags & ~0x408) | 6 flush with 0x100, fence pair              PROVEN (sub_2E190)
       0x12b0 = (MemLevels & ~0x18000) | 0xe0000 in the 0x307 fence packet  PROVEN
draw   unchanged primitives                                                  DERIVED
resolve 0x1324 = ~0; 0x12cc/0x12d0 = stencil; 0x1300 = 0xc4;
       0x132c = 0x0fff0fff; 0x1328 = 0; 0x12c8 = depth ctl; 0x12fc = FAA word;
       WARP 0x1164..0x1198 packet replayed in vendor order (4.1);
       0x1300 = 9; 0x1300 = normal DWGCTL; fences 0x107/0x207             PROVEN sequence, INFERRED effect
off    0x12fc[1:0] = 0 (bit-1 flush again), 0x12b0 = MemLevels,
       slot 2 = {0, 0x32020000}                                              DERIVED
```

OPEN: alignment and engine-window placement of the fragment buffer; meaning
of `0x12fc` modes 1/2, bit 2, bit 4 and the non-FAA size field; whether the
`0x1300 = 0xc4` write starts work; the fragment record layout; the eight
`0x1340/0x13c0` words; ThresholdAA units.  One target probe series (below)
settles most of them without a Windows capture.

## 10. Proposed hardware probe plan (needs an exclusive slot)

All steps on nano with the 0528, under the hardware lock and watchdog, after
the mandatory warm-up, one step per submission, stop and power-cycle at the
first stall; no 0x11e8 writes.  Test data are traceable: every colour and
every fragment-buffer dword is pre-filled with an address-derived pattern
(not zero) except where zero is the vendor requirement, and the geometry
covers the whole target plus partial-coverage edges at every subpixel phase.

1. **Bind only.** Slot 2 = zero-filled 1 MiB buffer, config `0x10000020`,
   `0x12fc` FAA bits 0, size field `S >> 8`.  Draw the known-good textured
   quad; expect byte-identical colour/depth.  Read back the buffer: it must
   stay all zero.
2. **Accumulate.**  Same, `0x12fc[1:0] = 3`, `[6:5] = 1`, MemLevels
   adjusted, preceded by the bit-1 flush.  Draw one opaque, untextured,
   flat-coloured triangle with edges at non-integer slopes over a background
   with a per-pixel pattern.  Read back colour and the first and last 64 KiB
   of the fragment buffer: interior pixels must carry the triangle colour,
   edge pixels either untouched or written, and the buffer must gain non-zero
   records near offset 0.  Do not read the `0x1340/0x13c0` words over
   MMIO while the engine runs; whether they are readable at all is OPEN.
3. **Resolve.**  Issue the section 4.1 sequence once.  Read back colour edge
   pixels: a working resolve leaves intermediate (coverage-weighted) values
   between triangle and background colour; compare with a 16-sample
   reference rendered on llvmpipe.
4. **Repeat without resolve and with two overlapping triangles** in both
   depth orders (sort independence), then with depth test on.
5. Only then: target change and clear interplay, and a 1920x1200 buffer near
   the 16 MiB cap to test placement relative to the colour target's window.

Implementation: `parhelia-drm-test --faa-probe` (`PROBE_STEP=1..5`) in the
Mesa branch `p0528-faa0528`; each step compares against an FAA-off
reference draw and dumps colour and fragment-buffer head/tail files.

A stall in step 1 or 2 indicates base/window placement (try the buffer
directly above the colour target inside its 16 MiB window, as for depth).

## 11. Hardware measurements on the 0528 (2026-10-03)

`--faa-probe` on a 102b:0528 rev 6, 256x256 target, gouraud triangle over a
per-pixel background; all files in `campaign/results/faa0528/`.

| setup | result |
|---|---|
| slot 2 bound, `0x12fc[1:0] = 0` | byte-identical to FAA off, buffer untouched [MEASURED] |
| mode 3, no depth surface bound | engine stall on the first draw [MEASURED] |
| mode 3, S8Z24 depth in slot 1 | no stall; 16-byte records `00RRGGBB, xxff8000, ffxxxxxx, 4020ffff` plus runs of the pixel depth (`0x00800000` for z 0.5) and `0x0100000x` words; colour spans near edges left unwritten, some records written into the colour target next to the edge pixels [MEASURED] |
| buffer moved into the 16 MiB window above colour | same; window placement is not the cause [MEASURED] |
| repeated runs | the record start offset drifts (0x10, 0x8010, 0xc100): persistent allocation pointers; after a `DWGCTL 9` resolve the next draw starts at 0 again [MEASURED] |
| `DWGCTL 9` resolve | adds records instead of merging them; colour unchanged [MEASURED] |
| mode 1 | inert (no fragments, image unchanged) [MEASURED] |
| mode 2 | partially covered edge pixels drawn fully, alpha 0xff, buffer untouched [MEASURED] |

So the fragment unit needs a bound depth surface, and mode 1 is most likely
the "armed, no samples requested" state selected by `0xa05f3` [INFERRED].
The next probe adds the vendor's fresh-context pointer load (`sub_9D790`),
which needs indices f0..f3 (`0x13c0`) admitted by the kernel.

### 11.1 The tile-spill model (2026-10-03, later runs)

Further runs with the vendor's fresh-context pointer load (zero
`0x1340..0x134c`/`0x13c0..0x13cc`, admitted by the probe kernel) and dumps
of all three surfaces replace the "window swap" reading above:

* A pixel colour found in the fragment buffer sits at its own colour-target
  offset minus k x 8 KiB with the low 13 bits unchanged: whole 8 KiB tiles
  (one 64x32 tile at 32 bpp) of colour and of depth are moved into the
  buffer, block by block in allocation order (colour tile `0x18000` to buffer
  `0x0`, ...) [MEASURED, run w3].
* The surface tile left behind holds 16-byte records at the pixel positions:
  in colour `00RRGGBB, MMff8000, ffxxxxxx, 4020ffff` (MM varies like a
  coverage byte), in depth words `0x010000nn` with nn counting up to the
  number of buffer blocks used, i.e. block references [MEASURED r2z; field
  meanings INFERRED].
* After the pointer load, allocation starts at buffer offset 0 and nothing
  lands outside the surfaces; without it the start drifts between runs.
* `DWGCTL 9` with the restored accumulation state rasterises once more in
  FAA mode (it allocates one more block) instead of merging [MEASURED].
* `0x400`/`0x800` DWGCTL flushes occur only in `sub_9E000` (retire) and
  `sub_9D790` (activate) as `sub_9D560(0xc00)`; every other `sub_9D560`
  caller passes `0x100`/`0x200`/`0x300` (surface stamps) [C]. Untagged, the
  `0x407`/`0xc07` pair stalled the 0528 [MEASURED]; the probe kernel
  therefore builds it itself with its fence counters, the next measurement.

No consumer of spilled tiles outside the fragment-context class was found
in `MTXPARD.dll`; Present and blits reach FAA only through the resolve
before clears and target changes (section 4.1) [C, negative search].

### 11.2 The tagged flush and further resolve attempts (module faa0528-3)

* The `sub_9D560(0xc00)` flush, built by the kernel with its counters, runs
  without a stall and only writes out cached fragment records (about 500
  dwords appear in the buffer; colour and depth unchanged): a fragment-cache
  writeback, not the merge [MEASURED].
* Flush then the `0xc4 .. DWGCTL 9` resolve changes six colour pixels; with
  `0x12fc[1:0] = 0` during the resolve nothing changes [MEASURED].
* A 2-D engine copy of an accumulated target returns the same raw tiles,
  so reads are not resolved lazily either [MEASURED].

The operation that turns spilled tiles back into pixels is still OPEN.

### 11.3 Record fields (decoded from the f2 dumps, LINCOL runs pending)

| word | content | evidence |
|---|---|---|
| w0 | `00RRGGBB` fragment colour, alpha dropped | equals the FAA-off colour of the pixel [MEASURED] |
| w1 | `[31:24]` 8-bit sample mask, `[23:0]` `0xff8000` (three records `0x800000`, the depth 0.5 of the triangle) | [MEASURED values, meaning INFERRED] |
| w2 | `0xff`, `[23:12]` pixel x, `[11:0]` about y/2 | x exact against colour-matched pixels, y ambiguous by one [MEASURED/INFERRED] |
| w3 | `0x4020ffff` | constant in every record [MEASURED] |

In the depth surface the depth of each partially covered pixel is replaced
by `0x010000nn` (bit 24 set, small n) [MEASURED]. No vendor 2-D or 3-D
operation that reads the fragment buffer outside the fragment-context class
was found [C, negative search]; the merge semantics are being established
by decoding the records against position-encoded colours (`FAA_LINCOL`)
and a software blend of the records over the target.

### 11.4 Exact record decode (position-coded colours, run g1)

With fragment colours R = x/256, G = y/256 every record names its pixel:

| word | content | evidence |
|---|---|---|
| w0 | `00RRGGBB` fragment colour | [MEASURED] |
| w1 | `[31:24]` **8-sample** coverage mask; `[23:0]` `0xff8000` (a few `0x800000`) | popcount/8 matches the analytic area of the pixel to 0.058 mean over 432 records, i.e. within one sample [MEASURED] |
| w2 | `0xff << 24 \| x << 12 \| y >> 1` | 432 of 434 records exact [MEASURED] |
| w3 | `0x4020ffff` | [MEASURED] |

Records are stored in allocation order at odd 16-byte slots; the depth word
of each partially covered pixel becomes `0x010000nn` with nn close to the
record's offset / 256 [MEASURED, the exact link OPEN].  Blending the
records over the target with c = popcount/8 in software gives
coverage-weighted values on about 300 of 540 high-contrast edge pixels
(mean error 0.17-0.2 against the area, MEASURED); the rest belong to tiles
whose writes were diverted and cannot be restored from this placement.
Two overlapping depth-tested triangles in mode 3 stall the engine on the
first draw [MEASURED, g4].  The FAA unit is therefore an 8-sample coverage
A-buffer; a working accumulation layout (placement/addressing of the
buffer so that no surface tile is diverted) is still OPEN.

### 11.5 Own-object placement (run h1/h4)

* Records: 291 in the buffer, 247 written into the colour target as four
  adjacent pixels; the decode of 11.4 holds for both, one record per single
  pixel with an 8-sample mask [MEASURED].
* Colour pixels that reach the buffer lie at their colour offset minus a
  multiple of 8 KiB, and several colour tiles land on the same two or three
  buffer blocks: an 8 KiB-granular aliasing between colour tiles and the
  start of the buffer that loses data in both directions [MEASURED]. The
  addressing the fragment unit expects is still OPEN.
* Blending the surviving records over the surviving pixels in software
  (c = popcount/8) matches the analytic edge coverage to 0.144 mean, 448 of
  568 measured edge pixels within one sample [MEASURED]: the record model is
  right.
* Two overlapping triangles accumulate without a stall in this placement
  (they stalled with the buffer inside the depth object); the resolve then
  stalls [MEASURED].

### 11.6 Buffer use is fixed at about 24 KiB (runs d0-d3, e1, e2)

* Shifting the buffer by 8/16 KiB moves the whole pattern with the base;
  a +256 B base stalls the engine: the base must be 8 KiB aligned
  [MEASURED].
* With a 1 MiB, 1.18 MiB or 4 MiB buffer (object and `0x12fc[31:14]`
  together) one draw always writes only the first three 8 KiB blocks:
  the size field does not partition the buffer for the unit [MEASURED].
* Relocated pixels keep the low 13 bits of their colour offset, so each
  block is indexed like one 64x32 tile; pixels of different tiles that
  share an in-tile offset overwrite each other [MEASURED, the per-pipe
  reading INFERRED].
* Linux `mtx2187` emits the same words as `sub_2E360` [C].

The buffer therefore behaves like the write-back target of a per-pipe tile
cache rather than a growing fragment heap in this setup; what the vendor
binds or enables so that these lines go back to the colour target is the
open item.

### 11.7 The resolve works when it follows the draw in one command stream

Issuing the accumulation draw and the `sub_2E360`/`DWGCTL 9` resolve in a
single submission (no kernel drain between them, as in the vendor's
command buffer) makes the hardware merge fragments: 193 high-contrast edge
pixels of the probe triangle come out at exactly k/8 of the way from
background to triangle colour, matching the analytic pixel coverage to
0.07 on average [MEASURED, run o1, module faa0528-3].  The resolve is
therefore the vendor's `sub_9E860` sequence; what still fails are pixels in
tiles whose data was already relocated or overwritten during accumulation,
and any draw with several fragments per pixel stalls (runs g4, o2).

### 11.8 Fences between draw and resolve (runs w1-w1d, modules faa0528-7/-8)

All runs issue the draw, one in-stream tagged fence
`[4, 0xc0afafc0, op, tag|1, tag, resume]` and the resolve in one submit.

| op | result |
|----|--------|
| `0x307`, `0x207`, `0x107` | lockup in the resolve (completed tag = the in-stream fence) [MEASURED] |
| `0xc07` | no lockup, but the resolve no longer merges: 161 of 750 edge pixels intermediate, mean error 0.336 (no fence: 0.07) [MEASURED] |
| retire pass (`sub_9E000`, fence `0x207`) then resolve | lockup in the resolve [MEASURED, w2] |
| retire pass only | no lockup [MEASURED, w3, w3b, w11] |

### 11.9 What the retire pass reads (runs w5b, w6, w3b, w11; module faa0528-9)

With the 256-byte companion pre-filled with markers `0xc0d000nn`:

* The setup-plane triangle of `sub_9E000` writes S8Z24 values into dwords
  0..2 only: stencil `0xc0 -> 0xc1` (increment) and Z24 = one 18-bit
  word of the fragment unit.  Dword 3 and dwords 32..35 (row 1 at the
  32-pixel companion pitch) are never written; the vendor reads them
  anyway and zeroes its copy afterwards, so its saved words 3..7 are
  0 [MEASURED].
* Loading `0x110, 0x220, 0x330, ...` into `0x1340..` and retiring reads
  back `0x110 0x220 0x330`: dwords 0..2 are `0x1340`, `0x1344`, `0x1348`,
  the live registers [MEASURED].
* After power-on the three words are `1, 1, 1`, not 0; after a
  multi-fragment draw about `0x51, 0x7d, 0x4f` (`0x53, 0x7d, 0x51` on
  another boot) [MEASURED].  Three counters match the three 8 KiB buffer
  blocks the tiles spill into (11.6) [INFERRED].

### 11.10 The control run regressed on faa0528-9

Loading `1,1,1` (w8), `0x51,0x7d,0x4f,0,...` (w4), or the 0xc00 flush
alone (w9c) ahead of a multi-fragment draw plus resolve all lock up, but
so does the same draw plus resolve with nothing added (w10, twice), which
passed on faa0528-3/-4 (o4, x2).  These runs therefore do not show that the
pointer load is at fault.  Multi-fragment draw plus retire without the
resolve never locked up [MEASURED].

### 11.11 How the vendor keeps FAA alive between draws (static)

The contradiction "the vendor arms `0x307` per validate, yet `0x307`
before the resolve locks it up" does not hold:

* The `ValidateState` arm at `0xc9a37` (`[4, 0xacafafc0]`: `DWGCTL = 0x307`,
  `0x12bc = tag|1`, `0x12bc = tag`, `0x12b0 = MemLevels`) only runs when
  the surface atom's dirty word has bit 7 set (`0xc9a05: cmp byte [dirty],0;
  jns 0xc9a65`).  That bit is set when a surface slot or `MemLevels`
  (`0x12b0`) changes (`0x10ddc6`, `0x10e757`, and the surface binders), not
  per draw.  Enabling FAA changes `0x12b0` (`|0xe0000`), so the vendor arms
  once when the FAA target is bound, then streams FAA draws without any
  fence until the resolve.
* The tagged end fences of the 2D-on-3D paths (BITBLT `0x10d900`, the
  `0x110160` path) pick `0x707` instead of `0x307` when the raster atom has
  FAA mode bits set: `0x10e78d test byte [blk+0xa0],3` / `0x10e797`,
  `0x110244` / `0x11024e`.  `0x707` is also the second op of the kernel's
  `full_sync` drain and appears in engine init (`0x2c44d`).
* Further FAA-dependent state: with FAA on, `0x2cfa1` lowers
  `[blk+0x780]` from 32.0 to 31.0 (16.0 to 15.0) and sets dirty bit 28;
  `0x2d2b6` sets bit 4 in `[ctx+0x3c]` [PROVEN, meaning OPEN].
* `0x1340..0x134c`/`0x13c0..0x13cc` are written only by the activate
  path (`sub_9D790`) on a context switch: saved words, or zeros for a
  never-used context; the first owner never loads (4.2).

A vendor FAA frame is therefore:
activate (only on a context switch) -> validate with dirty bit 7 (surfaces
incl. slot 2, `0x307` arm with the FAA `MemLevels`) -> FAA draws with
only state/vertex packets between them (blits inside use `0x707`) ->
`sub_9E860` resolve before a clear, target change or present -> the next
validate re-arms `0x307` with the restored `MemLevels`.

Consequence for the Linux kernel: the per-batch `0x307` drain is what ends
the FAA session between our batches.  Module faa0528-10 (`3263d41`) emits
`0x707` for the drain of batches that leave FAA armed (`faa_boundary`
bit 3 keeps `0x307`) and admits `0x707` as an in-stream fence; the next
hardware tests are the in-stream `0x707` fence between draw and resolve,
then draw and resolve in separate batches.

### 11.12 The vendor resolve submit, word for word [PROVEN, static]

`sub_9E860` runs only while the FAA session flag `[ctx+0xe8]` is set
(set by the session start `0x9d1b0`, cleared at the end of the resolve):

1. On an owner change: retire the old owner and activate this one (4.2);
   otherwise, when the global at `0x1d3ab0` is above 1, the engine preamble
   `0x2dc20`.
2. `0x2ec10(state, 0, 1)`: an ordinary validate of the current FAA state.
3. `[4, 0x8d8c8988]` slots 0 and 2 (`0x1220`, `0x1224`, `0x1230`,
   `0x1234`) and `[2, 0x21218b8a]` slot 1 (`0x1228`, `0x122c`), as saved at
   session start.
4. `sub_2E360`: `0x1324 = ~0`, `0x12cc`, `0x12d0`; `0x1300 = 0xc4`,
   `0x132c = 0x0fff0fff`, `0x1328 = 0`, `0x12c8`; `0x12fc`; the 34-dword
   setup-plane packet; then all state groups dirty (`|= 0x6400000f`).
5. `[2, 0x2121c0c0]`: `DWGCTL = 9`, then the resume DWGCTL.
6. `0x9d6e0(colour, 0x100)` and `0x9d6e0(depth, 0x200)`: tagged fences
   `[4, 0xc0afafc0, 0x107 | 0x207, tag|1, tag, resume]`.
7. All dirty words set to `~0`, state re-emitted (`0x2ce60`), session flag
   cleared.

The session start `0x9d1b0` sets `[depth+0x368] |= 8` and `[state+0x2c] = 1`,
which makes the next validate emit the full engine preamble `0x2dc20`
(guard band `0x12f4 = 31.0`, `DWGCTL 0x2010cc`, combiner defaults), and
saves the slot words used in step 3.

Our probe issues steps 3-5 in the same stream as the draw.  Adding step 6
(runs b5, b6) does not prevent the lockup, which comes before the
first post-resolve fence completes.

### 11.13 Repeatability (module faa0528-10, `faa_boundary=8`)

Over these boots, with one placement throughout (colour `0x1986000`,
fragment buffer `0x19c6000`, depth `0x2000000`), the draw alone never
locked up (6 runs), while a submit with draw and resolve locked up on
the first resolve of a boot in 3 of 10 cases and on a later resolve in 6
of 8 [MEASURED].  Placement is not the cause; the lockup is inside the
draw-plus-resolve span.  What carries over from one resolve to the next
is OPEN.

### 11.14 Frames in one process (module faa0528-10)

The vendor zeroes the fragment buffer once, when it allocates it
(`sub_9E690`, CPU clear at `0x9e7a2`).  `sub_9E690` is called only from the
size setup `0x9fbb0` and from `0x9ff30` on the `[fragctx+0xc4]` state 3/5
transitions, never per frame or per resolve [PROVEN].

Ten frames of draw + resolve + `0x107`/`0x207` fences on one set of
objects in one process [MEASURED]:

| variant | frames completed before the lockup |
|---|---|
| buffer zeroed once | 4 |
| buffer re-zeroed before every frame | 4 (same fence numbers) |
| 500 ms idle before every frame | 1 |
| no host transfers between frames | 1 |

Re-zeroing the buffer is not the cause, and an idle wait does not help.

### 11.15 Summary of the hardware results (modules faa0528-10 .. -15, 2026-10-04)

**Fragment unit words** (`0x1340..0x134c`, `0x13c0..0x13cc`, written by the
vendor only in its activate path):

* `0x1340 + 4i` is the allocation pointer of pixel pipe *i* (four pipes), in
  256-byte units: with explicit loads the records of the four pipes start
  exactly at the loaded values times 256 (`0x12190`, `0x24250`, `0x363d0` for
  `0x121`, `0x242`, `0x363`; `0x400030`.. for `0x4000`..) [MEASURED].  The
  long-known 24 KiB ceiling `0x5130` is the first dumped word `0x51` times 256
  plus the slot.
* `0x13c0 + 4i` places the pipes when the pointers are zero: any non-zero
  set spreads the records over the buffer, all zero stacks the four pipes at
  offset 0; addresses wrap at the buffer end [MEASURED placement]; the exact
  word-to-pipe mapping (base of pipe *i* or end of pipe *i-1*) is OPEN.
* Power-on pointers are small (`1, 1, 1` or `0, 0, 0` read back), so without a
  load all pipes allocate in the same 24 KiB and overwrite each other's chunks:
  the data loss of 11.6 [INFERRED].  The resolve leaves the pointers at 0.
* The vendor loads zeros for a fresh owner (4.2); what separates its pipes
  then is OPEN.

**Buffer format.**  The allocation unit is a 256-byte chunk of 8 rows by 8
dwords.  Records (4 dwords: colour, coverage mask, position, `0x4020ffff`)
sit in rows 0/1/4/5, columns 4-7; colour pixels of the target tile the chunk
belongs to sit in rows 6/7.  Each pipe's colour data stays within its own
chunk range [MEASURED layout, meaning INFERRED].  Pixels whose colour is in
a chunk that the resolve does not write back remain at the background value
("holes", 161-195 of 373 edge pixels at half size).

**Z tile tracking** is required state: with X stopped and no Z commands the
first FAA frame locks up in 4 of 4 boots; with tracking on (`DWGCTL 0x20a`)
and a Z-init load (`0x12b4` + `DWGCTL 0x008`) ahead of the FAA submit it
completes in 14 of 19.  The Z-init reset `0x108` makes it worse (5 of 6 boots
lock up in frame 1).  Z-init does not rewrite depth memory: references of
earlier frames stay there (they are not the cause: frame 1 also fails).

**Resolve behaviour.**  A counter-only marker fence between draw and resolve
always completes: the draw finishes and the resolve (`DWGCTL 9`) is what does
not, with the engine left on the post-resolve `0x107` fence.  The resolve
merges (k/8 steps, error 0.07 for completed pixels) only in the same stream as
the draw; any flush fence between them (`0x107`, `0x207`, `0x307`) locks it
up and `0xc07` ends the session.  No dumped state (record counts, chunk
extents, depth references) separates frames whose resolve completes from those
that lock up.  Variants without effect: kernel strobes and stage CALLs removed
(worse), `full_sync`, MemLevels through the vendor's tagged arm and other
MemLevels bits, ThresholdAA `0x007fffff`, a 4 or 16 MiB buffer, a resolve
scissored to the bounding box, X stopped, the hand-over and arm flushes.

**Stable envelope.**  With X stopped, `FAA_ZINIT=2` (tracking on + Z-init at
the head of each frame, tracking off after the resolve), the vendor's post
fences and the default 1.18 MB buffer: a triangle of 0.25 or 0.4 of the probe
size (up to about 800 non-zero buffer dwords) ran 10 of 10 frames on 5 of 5
boots; from 0.5 upward lockups appear (frame 2-8), the full triangle locks
up within 1-7 frames.  FAA is not usable for real scenes on the 0528 until the
resolve lockup is understood; the vendor command stream recorded under
emulation is the next reference.

### 11.16 Against the emulated vendor stream (series 22-24, modules faa0528-14/-15)

The vendor's own FAA frames, recorded by executing MTXPARD under emulation
(faaemu, [DERIVED]), differ from the probe in the FAA draw state and in the
vertex path, not in the depth surface:

* The depth surface descriptor is identical (`0x36020100`, the 0x36 code is
  the D24X4S4 class).  Under FAA the vendor sets `0x12c8 = 0x000e00e7`
  (stencil bits 7), stencil masks `0xfe` / `0x1fe` (stencil bit 0, depth
  word bit 24, belongs to the fragment unit), ThresholdAA `0x007fffff`, the
  draw DWGCTL with bit 20 (`0x00b60ec4`), and leaves DWGCTL at `0xc4` after
  the resolve and its `0x107`/`0x207` fences, followed by `0x307` and
  `4, 0x407`.  The probe now reproduces all of this (`FAA_VFRAME`,
  `FAA_MLARM`, `FAA_POSTFLUSH=3`, 0x12fc `0x04868027` as the vendor).
* With that state the lockups move later (half size: frames 5, 6, 9 instead
  of 1-3; full: 6, 1, 2) but remain [MEASURED].  Moving the fragment buffer
  to another window or loading the WARP stages once per FAA session changes
  nothing.
* Draws alone never lock up.  Accumulating N frames into the armed buffer
  and resolving once: N = 2 completed in 3 of 4 boots, N = 5 locked up in the
  resolve in 4 of 4 [MEASURED].  The resolve lockup becomes certain as more
  fragments accumulate before it.
* The vendor's vertex path is the remaining difference: pretransformed
  5-dword vertices, `0x1100 = 0x108`, `0x10c0 = 2`, `0x1130 = 0x02002207`,
  VTXFMT `0x23` / `0x00a87210`, and `0x41` block-file constant loads; the
  probe uses the kernel's WARP microcode stage CALLs.  Replicating it needs
  the kernel to admit exactly those words and blocks on the original family
  (not implemented).

## 12. The vendor FAA frame pair, packet by packet (static, original family)

Scope: `MTXPARD.dll` with `caps & 0x7fc00 == 0` (0x0527/0x0528), one D3D
context that owns the fragment unit, an FAA target, cross-checked against
`mtx_parhelia.o`.  Notation: `H` = `[D+0x30]` (the SetupHardware object
`sub_2EC10` runs on), `blk` = `[D+0x2c]` (state block), `F` = `[H+0x20]`
(pending flush flags), `tag` = the hardware fence counter `[[pDev]+4]+0x1c`
after `+= 0x10`, `resume` = the last DWGCTL written (`[[pDev]+4]+0x4c`).
Bracketed groups are the vendor's register-list packets; register =
`0x1000 + 4 * index`, index `0x21` = pad.

### 12.1 Corrections to sections 0, 3, 11.11 and 11.12

* **`sub_2DC20` is not an engine preamble on the 0528.**  It tests
  `caps & 0x3e` (0x0527 only) at `0x2dc2e..0x2dc3b`; on the 0528 it jumps to
  `0x2e136`, which emits only `[0x1378 = <unwritten dword>]` (a strobe, see
  `parhelia-minimal-draw-recipe.md`) when `[H+0x3c]` bit 2 is set, and returns
  without clearing `[H+0x2c]`.  The guard band 31.0, `DWGCTL 0x2010cc`,
  combiner defaults and `0x12f4` words of 11.12 belong to the 0x0527 branch.
  In the resolve, `[H+0x3c]` has just been zeroed by `sub_2EB80` (`0x2ebf4`)
  inside `sub_2EC10`, so both `sub_2DC20(1)` calls of `sub_9E860` (`0x9e8f7`,
  `0x9e9ee`) emit nothing on the 0528 in the steady state.  [PROVEN]
* **`sub_9D1B0` is not a session start.**  It runs after *every* primitive
  emission (13 call sites, e.g. `0x140122` right after the section submit
  `0x140111`), FAA or not: `[depth+0x368] |= 8` and `[H+0x2c] = 1`
  unconditionally, then, only with `fragctx+0xa4 == 3`, pending flag and
  snapshot.  `[depth+0x368]` bit 3 is read only by the clear (`0x9f733`,
  `0x9fa06`, may Z-init skip the stencil bits); `[H+0x2c]` only gates the
  `sub_2DC20` call.  Neither reaches a register on the 0528.  [PROVEN]
* **`sub_2CE60` emits nothing.**  It ORs the mask into the current block's
  dirty words (`0x2ce60..0x2ce95`); the re-emission happens in the next
  validate (12.3 E).  [PROVEN]
* **`sub_9DCC0` does emit on the original family.**  The non-LX branch
  (`0x9dd6f`) compares the wanted Z tile-tracking state with `ds:0x1ebe20`
  and, when it differs, emits `[DWGCTL = 0x20a (on) | 0x00a (off), resume]`
  (`0x9dd8e..0x9dddd`); only the LX `0x207` fence and `0x800` bit are
  family-gated.  Linux `mtx1538` (`0xcda0`, branch `0xcea6`) is the same.
  Sections 0 and 3 ("skipped on the original family") are wrong.  [PROVEN]
* `0x2cfa1` (guard band 31.0) and `0x2d2b6` (`[H+0x3c]` bit 4) are both
  behind `caps & 0x7fc00` (`0x2cf92`, `0x2d2af`): LX only.  [PROVEN]
* `[fragctx+0xe8]` only gates `sub_9E860` (`0x9e86f`).  [PROVEN]

### 12.2 The building blocks [PROVEN]

**Flush `sub_2E190`** (run by `sub_2EC10` when `F & 0x700`, `0x2ec6f`):
`F' = (F & ~0x408) | 6`, then one of
`[DWGCTL = F', 0x12bc = tag|1, 0x12bc = tag, DWGCTL = resume]` (`[H+0x28]`),
`[DWGCTL = F', 0x12bc = tag|1, DWGCTL = resume]` (`[H+0x24]`), or untagged
`[DWGCTL = F', DWGCTL = resume]`; and when the old `F` had bit 10 a second
group `[DWGCTL = 4, DWGCTL = (F' & 0xff) | 0x400, 0x12bc = tag|1,
0x12bc = tag, DWGCTL = resume]` (`0x2e2bb..0x2e34f`).  `F = 0x100`
(the `0x12fc` bit-1 change of `sub_9AB30`) gives `0x106`; `F = 0x701`
gives `0x307` followed by `4, 0x407`.  `0x701` is set by a state-block switch
(`sub_2CD90`, `0x2ce51`), by `sub_2D220` (3-D re-entry, below) and by the 3-D
hand-over `sub_2C070` (`0x2c0f7`, `0x2c129`).

**3-D re-entry `sub_2D220`** (`0x2ec5d`: when `[[pDev]+4]+0xf8 != 2`): all
dirty words `~0`, `F |= 0x701`, `[H+0x3c] = ~0`, owner word = 2.  The 3-D
hand-over `sub_2C070` leaves the owner word at 1 (`0x2c618`).

**Validate `sub_C9AC0`**, atom order: raster `sub_C9250`
(`[0x1328, 0x132c, 0x12fc, 0x12f8]`, `[0x12e8, 0x12e4, 0x12f0, 0x12ec]`),
`+0xa8`, `+0xc0`, `+0x1d0`, `+0x9fc`, surface `sub_C98B0`
(`[0x1220, 0x1224, 0x1230, 0x1234]`, `[0x1228, 0x122c]`,
`[0x1238, 0x123c]`, `[0x1318, 0x1314]`, `[0x1324]`; dirty bit 7:
`[DWGCTL = 0x307, 0x12bc = tag|1, 0x12bc = tag, 0x12b0 = MemLevels]`
(`0xc9a37`) and then always `[DWGCTL = atom+4]`), then `+0x768`
(`0x12f4`, WARP), each only for its dirty bits.

**Surface fence `sub_9D6E0`**: `[DWGCTL = op|7, 0x12bc = tag|1,
0x12bc = tag, DWGCTL = resume]` (`0x9d757..0x9d77e`).

**Z-init load `sub_9F470` -> `sub_9D480`** (original family, `0x9f4a6`):
`[0x12b4 = packed depth, DWGCTL = 0x008, DWGCTL = resume]`, or with `reset`
`[DWGCTL = 0x108, DWGCTL = resume]`.  Linux `mtx2060` -> `mtx1447`
(`0x107ad`: `0x21c0c0ad`, value, `8`; `0x10777`: `0x108`).

`3D.EnableZinit` defaults to `(caps & 0x3c0) && (memconfig & 0x08000000)` in
`MTXPARD.dll` (`0x109bff..0x109c34`) and to `caps & 0x3c0` in the Linux
`mtx_drv.so` (`parhelia-p690-fast-depth-clear.md` section 1), so the
vendor's 0528 runs its depth clears through Z-init by default (Linux
unconditionally, Windows when the BIOS memory-config bit is set).

### 12.3 One frame and the next

**A. Arming** (`sub_A0490`, called from the per-draw state pass `0xb03f4`
before the validate, `0x1400c0` -> `0x1400c7`), host only: block `+0xa0` =
FAA word, slot 2 = `{fragbuf, 0x10000020}` (surface dirty 4), surfaces
linked to the fragment context (`sub_9D330`), `sub_9AB30`: `F |= 0x100` if
`0x12fc` bit 1 changed, MemLevels `(v & ~0x18000) | 0xe0000` and dirty bit 7
when `[1:0]` became 3.  [PROVEN]

**B. Every draw** (`sub_101010` then the primitive):

1. Z tracking `sub_9DCC0`: `[DWGCTL = 0x20a | 0x00a, resume]` only on a
   state change; on when the Z-init owner `pDev+0xa674` is the bound depth
   surface.
2. Retire/activate only if another context owns the unit (4.2).
3. `sub_2EC10(blk, 0, 1)`: the 12.2 flush if `F & 0x700` (first FAA draw:
   `[DWGCTL 0x106, resume]`); `sub_2D270`/`sub_2EB80` (texture-format
   dependent, untextured: nothing); validate of the dirty atoms.  On the
   first FAA draw this is `0x12fc` = FAA word, `0x12f8`, slots 0/2/1 and
   `[DWGCTL 0x307, 0x12bc tag|1, tag, 0x12b0 = FAA MemLevels]`, DWGCTL.
4. Primitive packets, section submit.
5. `sub_9D1B0` (host): pending flag, snapshot of `0x12c8..0x12d0` and the
   slots.

Further draws of the frame carry only dirty state: no fence, no DWGCTL with
bit 8-10 (11.11).  [PROVEN]

**C. Resolve `sub_9E860`** (callers: surface access `sub_C2320` from
`0x2c0bd`, `0xa338d`, `0xed6aa`; the Z-init clear `0x9fb16`; target or mode
change in `sub_9FFE0`):

1. (owner switch only) retire/activate; `sub_2DC20` -> nothing (12.1).
2. `sub_2EC10(blk, 0, 1)`: dirty state only.
3. `[0x1220, 0x1224, 0x1230, 0x1234]` = snapshot slots 0 and 2,
   `[0x1228, 0x122c]` = snapshot slot 1.
4. If `[H+0x30]` (`0x9e9b2`): `sub_2E360` = `[0x1324 = ~0, 0x12cc, 0x12d0]`,
   `[DWGCTL = 0xc4, 0x132c = 0x0fff0fff, 0x1328 = 0, 0x12c8]`,
   `[0x12fc = FAA word]`, the 34-dword setup-plane packet.
5. `[DWGCTL = 9, DWGCTL = resume]`.
6. `[DWGCTL 0x107, tag|1, tag, resume]`, `[DWGCTL 0x207, tag|1, tag, resume]`.
7. Host: all dirty `~0`, current block `|= 0x2400000f`, pending = 0.

`0x12fc` stays in FAA mode in the hardware after the resolve.  [PROVEN]

**D. Frame boundary.**  Three vendor paths touch the engine between C and
the next frame's first draw:

* *D3D blit / present through the 3-D state* (`0x10d900`, `0x110160`):
  ends with `[DWGCTL = 0x707, tag|1, tag, resume]` while `[blk+0xa0] & 3`
  (`0x10e78d`), else `0x307`.  [PROVEN]
* *Hand-over to 2-D* `sub_2C070` (callers `0x1b59c`, `0x3f7aa`, `0x4111e`,
  `0x8b3e6`, `0x94c8d`, `0x94fa6`; `0x8b3e6` only when the owner word is not
  1, i.e. after 3-D use; that a present takes this path is INFERRED): resolve pending FAA
  surfaces (`0x2c0bd`); `F |= 0x701`; `sub_2EC10(default block, 0x7f, 0)` =
  `[DWGCTL 0x307, resume]`, `[DWGCTL 4, DWGCTL 0x407, tag|1, tag, resume]`,
  `[0x1378]`, null draw `sub_2E760` (`0x1324 = 0x12e8 = 0x12d4 = 0x12cc =
  0x12c8 = 0x12d0 = 0`; `DWGCTL 0x1000f4`, `0x12fc = 0`, `0x12e4 = 1`, slot 0
  format forced to `0x32`, tiny setup-plane triangle), `sub_2E960(1)`,
  `[DWGCTL 0x207, tag|1, tag, resume]` (`F |= 0x201`, `[H+0x28] = 1`,
  `0x2ebae`), validate of the default block; `sub_9DCC0(0)` =
  `[DWGCTL 0x00a]` if tracking was on; then the engine initialisation words
  (`0x2c185..0x2c5e9`) including `[0x1324, 0x1318, 0x1314, 0x12fc = 0]`
  (`0x2c3c3..0x2c3d6`), `0x12f4 = 31.0`, `DWGCTL 0x2010cc`, the sync
  `[DWGCTL 0x307, DWGCTL 0xc]`, `[DWGCTL 0x707, 0x1378 = 0, 0x12bc tag|1,
  tag]` (`0x2c42f..0x2c461`), `0x12b8 = 0x70601`, the same sync again and
  `0x12b0` = clock-derived MemLevels; owner word = 1.  So the vendor disarms
  FAA (`0x12fc = 0`) and drains `0x307 / 0x407 / 0x207 / 0x307 / 0xc /
  0x707` before any 2-D work.  [PROVEN sequence]
* *Clear* (`sub_9FF30`): colour through `sub_9B0C0` -> `sub_A0970` (blit
  clear, D3D blit fence as above); depth on the original family through
  `sub_9F8E0` (`0x9fe08`; the `0x9f4b0` path is LX only).  With
  `EnableZinit`, a full-surface rectangle and (Z and stencil cleared, or no
  stencil content): `[DWGCTL 0x20a]` (`0x9fa94`, if not on), a conventional
  clear of the rows beyond 3 MiB (`0x9faae..0x9faf9`), the resolve
  (`0x9fb16`, no-op when C already ran), then `[0x12b4 = packed depth,
  DWGCTL 0x008, DWGCTL resume]` (`0x9fb31`).  Otherwise
  `[DWGCTL 0x00a]` if another surface owned Z-init, and a conventional
  `sub_A0970` clear without resolve.  Linux `mtx237` (`0x10b80`):
  `mtx1538(1)` `0x10d28`, budget `0x300000` `0x10d4d`, `mtx2954` `0x10d64`,
  `mtx2060` `0x10da2`, same order.  [PROVEN]

**E. Next frame, first draw.**  `sub_9DCC0` first (`[DWGCTL 0x20a]` if the
Z-init owner is bound and tracking is off); then, after the hand-over (owner
word 1), `sub_2D220` fires: dirty `~0`, `F = 0x701` -> `[DWGCTL 0x307,
resume]`, `[DWGCTL 4, DWGCTL 0x407, tag|1, tag, resume]`, then the full
validate: `0x12fc` = FAA word again, slot 2 = fragment buffer,
`[DWGCTL 0x307, tag|1, tag, 0x12b0 = FAA MemLevels]`, DWGCTL.  Without a
hand-over (blit-only present), the first validate after C still re-emits
everything because C left all dirty words `~0`, including the tagged
`0x307` arm.  Draws and the resolve follow as B and C.  [PROVEN]

### 12.4 Vendor frame vs `--faa-probe` (Mesa `p0528-faa0528`, kernel -10)

| step | vendor (0528) | probe |
|---|---|---|
| depth clear | Z-init: `0x20a` once, `0x12b4` + `DWGCTL 0x008` per frame, no pixel writes | 2-D solid fill (`p3d_emit_clear`, slots 0/1 = depth) in its own submit, `0x12fc` still FAA, slot 2 still bound |
| Z tile tracking | explicit `0x20a`/`0x00a`, off before 2-D work | never written (power-on or previous client's state) |
| between frames | `0x12fc = 0`, sync `0x307 / 4,0x407 / 0x207 / 0x307,0xc / 0x707` before 2-D work; D3D blits end `0x707` | FAA stays armed through host transfers and the depth fill; kernel drain `0x307` (`faa_boundary=8`) or `0x707` |
| re-entry | `0x307` + `4, 0x407` + full validate with tagged `0x307` arm | full state batch, kernel opening fence |
| arm flush | `DWGCTL 0x106` on the `0x12fc` bit-1 change | none |
| ThresholdAA | `0x007fffff` (Windows) / `0x007fe000` (Linux param) | `0x007fe000` |
| draw .. resolve | no fence between them | same (one submit) |
| resolve, post fences | C.3-C.6 | same |

### 12.5 Candidates for the carried-over state

1. **FAA armed during foreign engine work.**  The vendor never lets 2-D work
   or another client run with `0x12fc[1:0] = 3`: `sub_2C070` resolves,
   flushes the fragment cache (`0x407`) and writes `0x12fc = 0` first.  The
   probe leaves the resolve's FAA word live through the host transfers, the
   2-D depth fill and anything else using the engine (X, fbcon) while it
   sleeps, which fits the shorter runs with a 500 ms idle (b10) and the
   boot-to-boot spread of the first resolve [INFERRED].  Probe: end every
   FAA submit after the `0x107/0x207` fences with the vendor hand-over core
   in the same stream, `[DWGCTL 0x307, resume]`, `[DWGCTL 4, DWGCTL 0x407,
   tag|1, tag, resume]` (kernel-built), `[0x12fc = 0x0bb80024 (mode 0)]`,
   `[DWGCTL 0x207, tag|1, tag, resume]`, and start the next FAA submit with
   `[DWGCTL 0x307]`, `[4, 0x407 + tags]` before the state (12.3 E).  Run the
   b8 loop with X stopped and fbcon unbound, then with them running.
2. **Z tile-tracking state.**  FAA rewrites depth words of partly covered
   pixels into `0x010000nn` fragment references (11.4) and per-tile state
   survives a mode-0 redraw (run g3); the vendor clears depth by resetting
   the Z unit's tile state (`0x20a` + `0x12b4`/`0x008`) every frame, the
   probe by a 2-D fill of the memory only (on the P690 the tracked state is
   on-chip and survives 2-D fills of other surfaces; whether a fill of the
   tracked surface itself resets it is OPEN).  Probe: replace `faa_clear_depth` with
   `parhelia_batch_depth_control(.., P3D_ZTRACK_ON, ..)` once per process and
   `parhelia_batch_depth_control(.., P3D_ZINIT_LOAD_ORIGINAL, load, 0x00ffffff,
   resume)` per frame, the latter at the head of the FAA submit (vendor
   order: clear, validate, draws, resolve); on exit `P3D_ZINIT_RESET_ORIGINAL`
   then `P3D_ZTRACK_OFF`.  [INFERRED]
3. **No fragment-cache writeback between resolve and the next accumulation.**
   With a present, the vendor always issues a DWGCTL with bit 10 between
   frames (`0x707` blit end, or `4, 0x407`), the probe only with
   `FAA_FRAMERETIRE=2`, which also redraws into the companion.  Probe:
   `full_sync=1` on the module (every drain `0x307, 0x004, 0x707`) with the
   b8 loop, or one in-stream `[4, 0x407]` kernel-built fence after the
   `0x207` post fence.  [INFERRED]

OPEN: the meaning of the memory-config bit 27 that gates Windows
`EnableZinit`; which 2-D entry points a D3D present takes on Windows
(`sub_2C070` vs the D3D blit), so whether a vendor frame pair contains the
hand-over or only the `0x707` blit fence.

## 13. The vendor FAA stream under emulation (faaemu, 2026-10-04)

`tools/faaemu/` runs the unmodified MTXPARD.dll and MTXPARM.sys under
Unicorn (PE loader, WIN32K/VIDEOPRT/ntoskrnl stubs, MMIO and 20-slot BAR0
model, fences retire at once) and drives them like dxg and the D3D9 runtime:
PDEV, surface, DirectDraw, D3D context, then per frame `DrawPrimitives2`
(clear, XYZRHW+diffuse draw) and a `DdBlt` present.  Captures and diffs are
in `captures/faaemu/` (`3D.AAMode=0x53` FAA, `0x54` AA off).  Register
writes are the vendor's own code; their context depends on the model.
[DERIVED]

- Arm: `0x12fc = 0x04868027` (size field 0x121a for a 256x256 target),
  `0x12f8 = 0x007fffff`, slot 2 `0x1230`/`0x1234 = 0x10000020`, packet
  `[0x307, tag|1, tag, 0x12b0]` then draw DWGCTL `0x00b60ec4`.
- Under FAA `0x12c8` carries stencil bits 7 (`0x000e00e7`, AA off 4) and
  the stencil masks are `0xfe/0x1fe` (AA off `0xf0/0x1f0`): stencil bit 0
  (depth word bit 24) belongs to the fragment unit.  Depth format for AA
  is D24X4S4 or D16 only; the slot-1 config `0x36020100` equals ours.
- Resolve runs inside the present blit's source access: re-validate, slots,
  `[0x1324, 0x12cc, 0x12d0]`, `[0xc4, 0x132c, 0x1328, 0x12c8, 0x12fc]`, the
  34-dword plane packet, `[9, 0xc4]`, fences `0x107`/`0x207` each resuming
  `0xc4`, `[0x307][4, 0x407]`, then the 2-D blit with FAA still armed.
- The resolve unlinks the target; it is relinked only by dirty state (blend,
  colour write, MSAA, ZFUNC).  With constant state only frame 0 resolves;
  with `EnableZinit` frame N resolves at frame N+1's Z-init clear.
- The vendor draws through its pretransformed path: 5-dword vertices, header
  `0x97080001`, VTXFMT `0x23/0x00a87210`, `0x1100 = 0x108`, `0x10c0 = 2`,
  `0x1130 = 0x02002207`, WARP microcode as `0x41` block loads.
- The 0528 rev 6 chip class is `0x100`, not `0x200` (miniport
  0x1888b..0x1889f).  [PROVEN]

Hardware replica (faa0528 series23/24, module faa0528-15): the vendor draw
state above makes `0x12fc` identical and moves lockups from frame 1 to frames
2-9, but does not remove them.  Draws never lock up, including five frames
accumulated without a resolve; the single resolve then locks up every time,
so the failure grows with the fragments the resolve must walk.  The largest
remaining difference is the vertex/WARP path the resolve's plane packet runs
through.  [MEASURED]

## 14. Everything the emulated vendor capture contains (extraction, 2026-10-04)

Sources: `captures/faaemu/*.json.gz` (keys `primary`, `submits`, `exec`,
`mmio`), decodes `*.flat.txt` / `*.annotated.txt`, diffs `diff-*.txt`.
`mmio` entries are `[R|W, offset, value, size, eip, phase]`.  [DERIVED
unless marked]

### 14.1 Direct MMIO outside the command stream

| phase | writes | content |
|---|---|---|
| miniport init | 0x920 = 0 (on-chip WARP store), memory/clock setup | `0x920` is read then written 0 in `ioctl 232280` and again in `ioctl 23231c` |
| `ioctl 23212c` | `0x404 = 0x08080000`, `0x400 = write-back page` | primary ring lives in VRAM (engine address `0x08080000`); `0x408` is the put pointer, one write per submit |
| `ioctl 232294` | `0x8f8/0x8fc = 0x40000/0x40000`, then `0x10000/0x50000` | meaning OPEN; written once before the first submits |
| `ioctl 232380`, `ddraw`, `context`, every clear/draw | host-aperture descriptor slots `0x10..0x13c` (`d0 = 0x10000000 | coord`, `d1` = engine address, `d2` = surface config, `d3` = dims), each with `0x314 = 1` | BAR0 views for CPU access; context creation programs all 20 slots |
| every FAA draw phase | `0x918` = instruction index, `0x91c` x 3n = WARP instructions | on-chip store: four programs at index 0 (15, 14, 29, 47 instructions), then 0x2f (19) and 0x42 (12); identical in every frame and identical with AA off (`noaa-aamode54`) |

The draw entry `0x1100 = 0x108` is store index `0x42` x 4.  Programs:
`captures/faaemu/warp-store-<n>-idx<i>.hex` (3 dwords per instruction).
Because the WARP path is the same with and without AA, it is not part of
the FAA mechanism; it remains the largest difference between our draws and
the vendor's, which matters only if the resolve depends on vertex-path state.

### 14.2 The command stream, per phase (faa-default)

* **create / context**: section submits that load engine defaults and
  WARP constants (`0x41` block loads, `0x10f0/0x1100/0x10c0..0x10d8`).
* **frameN-clear**: 2-D fills (`DWGCTL 0x5c0`) of colour and of depth
  (plane mask `0x01ffffff`: Z and stencil bit 0 cleared), `0x307` fences.
* **frame0-draw**: arm (`0x12fc = 0x04868027`, `0x12f8 = 0x007fffff`,
  `0x12c8 = 0x000e00e7`, masks `0xfe/0x1fe`, slot 2 = buffer
  `0x10000020`), `[0x307, tag|1, tag, 0x12b0 = 0x3ffe0842]`, draw
  `DWGCTL 0x00b60ec4`, pretransformed draw packet `0x97080001` (5 dwords
  per vertex), end-of-call `[0x307, tag|1, tag, resume]`.
* **frame0-present**: re-validate (`PRIMWORD 0x0002305c`, `VTXBASE0..7`,
  strides), slots 0/2/1, `[0x1324 = ~0, 0x12cc, 0x12d0]`,
  `[0xc4, 0x132c = 0x0fff0fff, 0x1328 = 0, 0x12c8, 0x12fc]`, 28 setup-plane
  writes (all end at 0), `[DWGCTL 9, 0xc4]`, `[0x107 ...]`, `[0x207 ...]`,
  `[0x307, 0xc4][4, 0x407, tags, 0xc4]`, then the 2-D BITBLT
  (`DWGCTL 0x2c2`, 32x64 strips) to the primary with FAA still armed.
* **frame1/2**: draws without a resolve while state stays constant (the
  resolve unlinks the target; only dirty blend/colour-write/MSAA/ZFUNC
  state relinks it), present blit only.

### 14.3 Vendor vs probe: status of every difference

| difference | probe status | hardware result |
|---|---|---|
| stencil field 7, masks `0xfe/0x1fe`, ThresholdAA `0x7fffff`, draw DWGCTL bit 20, resume `0xc4` | replicated (`FAA_VFRAME`) | lockups later, not gone (series23) |
| `0x12fc` word identical (`0x04868027`) | replicated | same |
| post-resolve `[0x307][4, 0x407]` | replicated (`FAA_POSTFLUSH=3`) | same |
| MemLevels through the tagged arm, after raster and surface bindings | old `FAA_MLARM` prepended it before those bindings; corrected in the resume clone | ordering correction alone still locks |
| PRIMWORD bit 18 clear before the setup planes | replicated (`FAA_RESOLVEPRIM`, Mesa clone) | full 6/3/4, half none/7/none; A5 still locks (series25) |
| separated pipe regions (`0x1340`/`0x13c0`) with all of the above | replicated (B1, Q layouts) | full: 1 in 4 of 4; half: 1, 3, 4, 3 - worse than stacked pipes (series26) |
| resolve once after N frames | tested | N = 5 always locks in the resolve |
| depth surface descriptor | already identical (`0x36020100`) | - |
| on-chip WARP store programs, entry `0x108`, VTXFMT `0x23/0x00a87210`, 5-dword vertices | **not replicated** (kernel owns memory WARP programs) | - ; not FAA-specific (14.1) |
| per-batch kernel drain: vendor has no drain inside an FAA frame other than its own tagged fences; our kernel ends every batch with `0x307` under `faa_boundary=8` (all series 23-26) | now tested with corrected VFRAME arm ordering and `0x707` | locks on frame 2; adding captured end-of-draw `0x307` locks on frame 3 |
| command sections in VRAM (class 0), primary ring in VRAM | **not replicated** (our sections are host memory) | - |
| vendor depth clear by plane-masked 2-D fill vs our full-word fill | equivalent for bit 24 | - |

The earlier comparison missed the tagged arm's ordering and normalised away
the address bank bits. Matching individual register values did not establish
the same state at the arm or the same surface address contract. See 14.4.

### 14.4 Resume: raw bank pairing and pixel centres (2026-10-04)

`tools/faaemu/streamdiff.py` now preserves chronological batches when CALL
IDs repeat, and can retain addresses and counters with `--raw-addresses`
and `--raw-tags`. `audit.py` reports the actual raster and surface state at
each arm and resolve. The previous parser grouped equal CALL IDs across
batches, which could interleave different frames. [PROVEN source defect]

Running MTXPARD through the emulator with 16 draws, 128 overlapping copies,
and repeated state validation gives the same FAA raster/surface/MemLevels
contract as the original capture. The overlapping capture sends 384
triangles (23,040 vertex bytes). Blend toggles re-link the target for a
resolve on each frame; re-sending unchanged state alone does not. The
`--trace-faa` hooks record host context state at lifecycle entries. Command
register writes now reach the model's MMIO readback file; replaying the
overlap capture after that correction produces the same 880 frame events.
FAA allocation pointers still stay zero in this model: it does not generate
fragments or model resolve timing. [CAPTURED; emulator limits]

The vendor binds colour `0x08240000`, fragments `0x09ede400` (both bank B),
and depth `0x01000000` (bank A). Our earlier probes put all three in bank A.
The 16 MiB placement variants therefore did not test this bank pairing.
The old `FAA_MLARM` also armed against the preceding state; the corrected
probe expands the original MemLevels packet in place, after the surfaces.
Correct ordering alone still locks on the post-resolve `0x107`. [MEASURED]

An isolated kernel option, `faa_vendor_banks=1`, pins FAA colour into B's
first 16 MiB window, fragments into B's second, and depth into A's second.
It applies only to armed original-family FAA batches, uses the ordinary
reservations/pin ranges/relocations, and leaves validators intact. On 0528,
with the corrected arm and the captured end-of-draw `0x307`:

- Control, option off: locks on frame 3, 217 fragment records plus 691
  spilled LINCOL colour words in the fragment buffer.
- Vendor pairing: ten full-size resolves complete, 635 records and zero
  spilled LINCOL words. Another ten complete on the same boot.
- Five accumulated draws followed by one resolve complete on a new boot;
  twenty moving draws also complete on that boot.

This is evidence for an address-layout aliasing mechanism, rather than
command-fetch location or the WARP store being necessary explanations for
these lockups. Which individual bank relationship is required is being
isolated; do not infer that all three placements are independently required.

The intact image exposes a separate pixel-centre mismatch: the FAA geometry
is shifted by half a pixel in both axes against the ordinary probe. A
`FAA_VERTEX_BIAS=-0.5` on FAA positions, retaining colour attributes, yields
ten resolves with **zero interior and exterior differences**. 449 of 540
high-contrast edge pixels have intermediate values; mean coverage error is
0.056. A verified fresh-boot 1024x768 run also completes ten resolves with
zero interior/exterior differences; using its constant blue channel gives
2,474 intermediate edge pixels and mean coverage error 0.048. [MEASURED]

An overlapping two-triangle run initially completed ten resolves but its
second draw-order case was refused with `-EBUSY` during placement. Draining
earlier async pins before moving a surface, using the same bounded retry as
ordinary depth placement, fixes that refusal. On a verified new boot both
orders complete ten resolves, have zero interior/exterior differences, and
their resolved images differ in **zero pixels**. [MEASURED]

Changing only the fragment placement to bank A completes ten resolves but
corrupts interior pixels (806 on the first frame, 372 on the tenth); changing
only depth to bank B locks at the first resolve with `DWGCTL=0x107`. The
captured B/B/A pairing is therefore significant for correctness, not merely
an arbitrary choice that happens to avoid a lock. Colour itself need not be
in B: A colour in its ordinary available window, B fragments, and A depth
in the next window also completes ten resolves with zero interior/exterior
differences. Its image is byte-identical to the corrected B/B/A image.
The colour-in-A variant initially could not allocate in the reserved first
window; that refusal is not a GPU failure. The passing retry uses colour
`0x01002000`, fragments `0x09000000`, and depth `0x02000000`.

The reverse pairing also passes on a verified new boot: colour B,
fragments A (`0x03000000`), depth B. Its image is byte-identical to the
corrected vendor-pair image. Thus the supported rule is **opposite VRAM
banks for fragments and depth**, not an absolute requirement for fragment
bank B or colour bank B. The two tested same-bank fragment/depth cases
produce corrupt images or resolve lockups. [MEASURED]

This explains the prior aliasing evidence: FAA colour words landed in the
fragment buffer and too few fragment records survived. A resolve must
consume depth-held fragment references; corrupted metadata is a credible
explanation for its density-dependent hang at the following `0x107`.
The exact internal addressing/link-cycle mechanism remains an inference;
the controlled bank tests establish the allocation contract on this card.
The successful tests retain host-memory command sections and the existing
kernel WARP programs, so neither transport change is required for them.

Final source `aab6776` (probe) and `f08c892` (isolated kernel), with variant
4 (colour A / fragments B / depth A), completes **50 full-size resolves**
on a verified new boot. First and last images have zero interior/exterior
differences. Probe selftests: 136 pass; Valgrind: zero memory-access errors
(the existing NIR selftest leaks remain). The emulator chronology/readback
regression checks also pass. [MEASURED, `LAYOUT4-FINAL50-1`]

Integration still needs a default-off GL FAA lifecycle: allocate the
fragment buffer in the bank opposite the pinned depth surface, preserve
ordinary colour placement where possible, account for the pixel-centre
convention, and resolve/retire on render-target changes. The diagnostic
does not advertise GL multisampling or enable FAA in the shared stack.

Artifacts: `campaign/results/faaemu-resume/` and
`campaign/results/faa0528/{BANKCTL-*,BANKPAIR-*,BANKBIAS-*,LAYOUT*}`;
isolated source repos `wip/faa-resume/{mesa-driver,linux-drm}`. These are
probe results; the production GL path is not enabled.

Recovery caveat: the shared power-cycle script used to skip a healthy recent
cycle even with `FORCE=1`. That guard is corrected, and subsequent runners
record and compare boot IDs. Empty earlier cycle logs identify same-boot
repeats, not independent boots.


### 14.5 Per-application GL experiment and glxgears (2026-10-04)

The next isolated Mesa branch is based on main `9369ebc`, commit `20306f8`
in `wip/faa-gl/mesa-driver`. `PARHELIA_FAA=1` enables the experiment per
application; it defaults off. A 16 MiB fragment BO, the corrected draw arm,
reserved stencil bit, -0.5 XY convention and resolve/hand-over on flush,
clear or target change now operate on actual glxgears. The GL entry path
currently accepts opaque, untextured, LESS-tested writable Z24 triangles
with application stencil off; it uses CPU vertex processing and separate
synchronous batches. General textured, blending, stencil and multiple-owner
GL use is not validated. No multisample visuals are advertised.

Kernel `967dea2` changes only the diagnostic placement predicate: a batch
which resolves and turns FAA off still needs its fragment/depth roles,
even though its last raster word has mode zero. It uses any FAA write for
placement and keeps the existing last-word rule for the terminal drain.
The screenshot boot uses `faa_vendor_banks=1 faa_bank_variant=4
faa_boundary=0`; ordinary colour stays in its available A window.

Real `/usr/X11/bin/glxgears` produces paired 600x600 images. A capture shim
freezes only the application's animation clock and reads its actual GL_BACK
pixels; it does no rendering or antialiasing. Captured 1,170 vertices (390
triangles) have identical Z/RGB and only the expected -0.5 FAA XY bias.
The executed stream has one arm, 196 draw packets and one DWGCTL 9 resolve;
colour `01986000`, fragment `09000000` (B), depth `02000000` (A). The
FAA image has visibly smoother silhouettes and hole edges. Ten successive
swaps resolve successfully; frame 10 is byte-identical to frame one at the
fixed angle. [MEASURED]

Untraced performance boot: 30 warm-up + 500 timed frames, three repeats,
600x600 with swap interval zero. Medians are 398.712 FPS ordinary defaults,
266.028 FPS ordinary CPU/synchronous/no-aggregation, 210.286 FPS FAA with
the same CPU/submission settings. FAA adds 0.996 ms/frame, **26.5% frame
time** against that matched baseline (21.0% fewer FPS). Against normal
defaults the current prototype costs 47.3% of FPS, including its CPU vertex
and submission changes. This is end-to-end GL/X frame timing, not isolated
hardware resolve time. All 1,590 FAA swaps in the three runs finish, with
no Parhelia wedge in the kernel log. [MEASURED]

The matched benchmark masks only the async-submission capability to select
Mesa's existing synchronous fallback in both clients; it changes no register
values and bypasses no validation. The animation-clock freeze does not
modify CLOCK_MONOTONIC used for timings. Kernel trace/dump options are off;
logs are on tmpfs during measurement. Full evidence, images, timings,
builder checks and all 144 passing Mesa selftests are under
`campaign/results/faa-gl/`. The isolated kernel/Mesa artifacts and source
patches are under `campaign/agents/faa0528/0003-*`. The shared stack is
restored after diagnostics and remains FAA-disabled.

For textures, the fragment-buffer bank contract and resolve lifecycle are
expected to be shared, but textured WARP/interpolation, sampler/combiner
state and the vendor's texture-format-dependent validation hooks in 12.3
must be exercised together. The untextured record evidence does not prove
textured shading semantics. Removing the current GL texture guard is not
such a validation. Texture filtering/mipmap minification and polygon-edge
FAA address different sources of aliasing.


### 14.6 Textured GL FAA, depth contracts and Quake 3 (2026-10-04)

Mesa `72131b1` on isolated branch `faa-textured` extends the default-off
`PARHELIA_FAA=1` path to ordinary textured triangles. Kernel `0f7d564`,
following merge `9cd8564` onto main `f288453`, advertises automatic FAA
placement through GET_PARAM 33. Diagnostic bank options are no longer
required. Shared sources and deployments remain unchanged.

Two additional contracts explain the new failures. First, move the 16 MiB
fragment allocation into the bank opposite depth **before** pinning the
required depth window. The initial allocation occupied that window; the
ordinary allocation fallback produced different draw/resolve depth
addresses and corrupted the image. The FAA allocator now requires the
window and retains the same depth address through resolve. [MEASURED]

Second, the vendor does not disable the hardware Z unit for D3D ZENABLE=0.
Textured emulator captures emit ZFUNC `0x000e00ef` (enabled ALWAYS) and
ZWRITE `0xfe` (depth writes off). Direct hardware Z disable wedges FAA;
the vendor mapping passes 100 textured frames and the overlapping-draw
checks. Regular enabled depth comparisons and write bits are preserved.
The emulator now accepts `--texture opaque|alpha` and repeated
`--render-state NAME=VALUE`. These are command captures, not rendered
images or timing evidence. [MEASURED]

FAA owns physical stencil bit zero. Native mappings are CPU shadows, so
preserving the application's bit requires download before saving and again
before restoring/uploading current depth after handover. CPU-only restore
failed with 0xa4 instead of 0xa5. Correct transfers preserve all 65,536
stencil pixels across ten FAA/stencil transitions, including a subsequent
8-bit EQUAL draw. Colour-only drawables use private Z24 metadata storage
while retaining disabled application depth semantics. [MEASURED]

GPU checks pass nearest, linear, real minified LOD 1, two-texture modulation,
vertex-alpha blending, alpha-channel visuals, readback without glFinish,
and near/far depth occlusion in both orders. Suppressed depth writes and
disabled depth tests allow a subsequent farther draw as required. All 144
ordinary Mesa selftests and focused ASan/UBSan builder checks pass.
Actual Quake 3 completes repeated 387-frame timedemos without a wedge.
Paired SDL swap-200 captures show coherent textured gameplay with smoother
roof and silhouette edges. [MEASURED]

At 640x480, swap interval zero, untraced kernel, one 387-frame run per path:
ordinary defaults 35.4 FPS (28.2 ms/frame), matched ordinary CPU/synchronous/
no-aggregation 22.6 FPS (44.2 ms/frame), FAA 16.0 FPS (62.5 ms/frame).
About 47% of the added frame time is reproduced by prototype restrictions
without FAA; the remaining 18.3 ms/frame includes metadata, resolve,
handover and other FAA work. This is not isolated hardware resolve timing
or a promise of recoverable FPS. Removing eager handovers around GPU alpha
micro passes still gave 16.0 FPS and was reverted. Next work is compatible
batching/async ordering, hardware transforms with viewport bias, and phase
profiling. [MEASURED]

Coverage remains experimental: RGB32 plus Z24/private Z24 on the original
family. Application stencil, alpha test, partial colour masks, logic ops,
other formats, points/lines and special CPU/multipass paths hand over to
ordinary rendering; those draws do not all receive FAA. Multiple owners
remain unvalidated, and no MSAA visual is advertised. FAA addresses geometry
edges rather than texture-detail aliasing.

Committed evidence and reproducible capture/benchmark helpers are in
`captures/faa-textured-20261004/`; complete working results and negative
allocation/no-Z experiments are in `campaign/results/faa-textured/`.
