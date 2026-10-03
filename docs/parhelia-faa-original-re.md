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

### 11.11 How the vendor keeps FAA alive between draws [PROVEN, static]

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
