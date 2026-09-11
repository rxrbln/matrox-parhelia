Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 texture pipes: vendor allocation, 0x11e8 pairing and a second pipe

This is a static audit of the retained vendor binaries. No target was touched
and no driver source was changed. It answers why `0x1200 = 0x00040103`,
`0x0004010f` and a second unit on pipe 1 lock up the P690 engine. It also
records what the vendor programs instead to use more than one texture pipe on
P690-class silicon.

Tags: **PROVEN** means read from machine code or table bytes at the cited
address. **INFERRED** means derived from proven facts but not executed on
hardware. Windows addresses are image virtual addresses (image base
`0x10000`). Linux addresses are `.text`/`.data` VMAs of the unlinked object.

## Evidence identity

| file | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` (`PSER`) | `52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a` |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserm.sys` (`PSERM`) | `da47324eae893160d0c1813984e1875a08a69fc72c47f123a666699e2d46b4b7` |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpsogl.dll` | `db0ab86fd25fe2cfb37bae5252d825aac8a2cc16d88191dedb165d4ba57dbcf0` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` (`PARD`) | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (`KRN`) | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |

## 1. Answer

1. **The P690 has no pipe 0+1 gang.** Both vendor stacks allocate texture
   pipes from per-class candidate tables. For the LX class (`0x2537`, `0x2538`
   and `0x2539`), the only mask that puts one stage on two pipes is **`0x5`**,
   pipes 0 and 2. Masks `0x3`, `0xc` and `0xf` point to an all-zero cost
   table, so the vendor never programs them on these chips. For a
   single-stage draw, the vendor programs:

   ```
   0x1200 = 0x00040105   unit 0: pipes 0+2, [11:8]=1, [18:16]=4
   0x11e8 = 0x00500000   unit-0 bank (Linux GL form, with 0x12f4 = 32.0)
   0x1204 = 0x0001000a   disabled unit 1 parks the leftover pipes 1+3
   0x11e8 = 0
   0x1208 = 0 ; 0x11e8 = 0 ; 0x120c = 0 ; 0x11e8 = 0
   ```

   **PROVEN** allocation and emission. The `0x11e8` value is PROVEN for the
   Linux GL path and INFERRED for this particular mask; see §5.

2. **Multi-stage draws use one pipe per stage from fixed LX permutations.**
   Two stages use pipes `{0,2}`, which is the working `0x40101`/`0x40104`
   route. Three stages use pipes `{0,1,3}`. Four stages use `{0,1,2,3}`. A
   two-stage draw is never placed on pipes 0 and 1 (**PROVEN**).

3. **`0x11e8` is written per unit on LX-class chips.** Both vendors write it
   right after each `0x1200+4u` and program four flag bits:
   - bit 20 is paired with the `0x12f4` guard band.
   - bit 22 is set when pipe 0 has an owner.
   - bits 21 and 24 are set on both members of a fully owned pipe pair
     (0,1) or (2,3).

   Our driver writes one `0x11e8 = 0` for all units (**PROVEN** difference).

4. **The vendor also does one-time LX setup that we omit:**
   - per-unit bank priming with `0x11e8 = 0x00100000`
   - on Windows, `TEDLFCounters` (MMIO `0x0bc0`) `= 0x001e4087`

5. **The P690 has 4 texture pipes.** The LX four-stage and 2D filter paths
   program four distinct pipe bits, so all four pipes are addressable
   (**PROVEN**). Only the (0,2) combination can be ganged. The vendor's rate
   model halves the LX per-clock cap relative to Parhelia (**PROVEN** code;
   the hardware meaning is INFERRED).

## 2. Chip class used by every branch below

`PSERM` classifier at `0x187ec..0x1891f` stores a one-hot class word:

| device | class bits |
|---|---|
| 0x0527 | 0x2 / 0x4 / 0x8 / 0x20 / 0x10 |
| 0x0528 | 0x40 / 0x80 / 0x200 / 0x100 |
| 0x2537 (Sundog) | rev1 `0x400`, rev2 `0x1000`, other `0x800` |
| 0x2538 (Sunex) | rev0 `0x2000`, other `0x4000`/`0x8000` |
| 0x2539 (P690) | rev0 `0x10000` (`0x188fb`), rev1 `0x20000`, other `0x40000` (`0x18904..0x18912`) |

This is identical to `KRN mtx1916` (`parhelia-gl-limits.md` §6.1). Every
texture decision below tests `class & 0x7fc00`, which is the whole LX family
including P690. No texture-pipe code tests the P690-only mask `0x70000` or the
Sundog-only mask `0x1c00`. The `0x70000` tests in the Windows priming function
(`0x2ea59..0x2ecec`) mask the `0x1200[18:16]` type field against `0x10000`,
not the chip class (**PROVEN**).

`PSER` contains no PCI IDs, so it reads the class from the miniport. The
exact copy path into `[pDev+0x1c]`, `[+0xab50]`, `[+0x8c4]` and `[+0x900]` was
not traced. That they carry the same word is INFERRED from the identical mask
use and the identical Linux classifier.

The texture-stage compiler is the same in PARD and PSER. A normalized
instruction diff of PARD `0x16fe00..0x17e000` against PSER
`0x171dc0..0x17ffc0` differs only in jump-table data. PSER `0x17b720`
(validator) is instruction-identical to PARD `0x179760`. The OpenGL ICD
`mtxpsogl.dll` contains neither the candidate tables nor any `0x1200` value
builder, because Windows OpenGL texture stages are compiled in the display
driver. The Linux kernel carries byte-identical LX tables (§3.2). The m9x
driver (`m9xdriver-...build_98`) binds only `PCI_DEVICE_ID_M9X = 0x0540`
(`kernel/common/m9x_common.h:42`) and has no LX tables.

## 3. The pipe allocator (PROVEN)

### 3.1 Flow in PSER

| step | address | function |
|---|---|---|
| candidate list | `0x179470` | calls `0x1772a0` at `0x179491` for the list, applies each candidate with `0x177310` (`0x1794aa`), keeps it if the per-stage validator loop `0x177830` (`0x1794b1`) passes; survivors go to `[obj+0x2158]`, count in `[obj+0x2154]` |
| table select | `0x1772a0` | sub-table = `12*a + 4*b + c` from stage fields `0xdc8/0xef4/0x1020` (dependency topology, INFERRED; 0 for independent stages), times `0x44`; **LX: `[0x1dbe58 + 4*nstages]`**, other: `[0x1d8668 + 4*nstages]` (`0x1772e8..0x177305`) |
| apply mask | `0x1767e0` | `stage+0x78 = mask`; hw `0x1200 = (old & 0xfffff0f0) \| ((a4 & 0xf) << 8) \| (mask & 0xf)`, where `a4` bit0 is kept and bits 3..1 = OR of dependent stages' masks (`0x17680f..0x17683b`) |
| validate | `0x17b720` (called from `0x177883`) | see §3.3 |
| choose | `0x1795e0..0x179663` | applies each survivor (`call 0x1767e0` at `0x179607`), checks resources (`0x177560`, at most `0xa` slots), scores with `0x1771d0`, and takes a candidate only if its score is **strictly greater** (`fcomp`; `test ah,0x41` at `0x17963a`). Ties keep the earlier table entry |
| LX `0x11e8` flags | `0x177350` | called at `0x1798a6`; §5.1 |
| TA pipe per slot | `0x177770` | physical first pipe of each stage's mask (table column A below) becomes the TA source pipe (`[obj+0x10f4+8k]`) |

### 3.2 Candidate tables (sub-table 0 = independent stages)

Each entry has 8 bytes: `mask[stage0..3]` then `owner-stage[pipe0..3]`
(`f` = none). Dump from PSER LX `0x1dbe58 -> {0x1d9e78, 0x1da4d8, 0x1dab38,
0x1db198, 0x1db7f8}` and other `0x1d8668 -> {0x1d6688, ...}`:

```
LX  n=1 @1da4d8: m[5,.,.,.] own[0,f,0,f] | m[1,.,.,.] own[0,f,f,f]
LX  n=2 @1dab38: m[1,4]     own[0,f,1,f]
LX  n=3 @1db198: m[1,2,8]   own[0,1,f,2]
LX  n=4 @1db7f8: m[1,2,4,8] own[0,1,2,3]

non-LX n=1 @1d6ce8: m[f] | m[3] | m[1]
non-LX n=2 @1d7348: m[3,c] | m[3,4] | m[1,c] | m[1,4]
non-LX n=3 @1d79a8: m[3,4,8] | m[1,2,c] | m[1,2,8]
non-LX n=4 @1d8008: m[1,2,4,8]
```

The dependent sub-tables for LX (`n=3` subs 4..20, `n=4` subs 1..22) only
permute single pipes and never contain `3`, `c` or `f`. The complete dump is
in the reproduction script (§11).

The Linux kernel carries the same tables. `KRN mtx661` (`.data` `0x1b6c0`)
points to `mtx2890`, `mtx3997`, `mtx2422`, `mtx3991` and `mtx1278` for n=0..4.
All five are byte-identical to the PSER LX tables. `mtx1918` (`0x6eff0`)
selects them on `[dev+0x6c8] & 0x7fc00` (`0x6f021`).

### 3.3 Validator `PSER 0x17b720` (Linux twin `KRN mtx1414` @`0x710a0`)

```
pipeSelect = TEPipeSelect ([holder+0x334], 0x17b731); multi = TEMultiplePipes ([+0x33c], 0x17b748)
mask 0x3 : reject unless pipeSelect >= 2 && (multi & 1)
mask 0xc : reject unless pipeSelect >= 4 && (multi & 2)
mask 0xf : reject unless pipeSelect >= 4 && (multi & 3)      (code tests 0x3, not bit 2)
kind 0xe : mask must be 8;  kind 1 : needs a4 & 1
mask in {3,c,5}: kind must be 0 (TEX), 4 (TEXBEM/L), 0x10 or 0x11 (TEXREG2AR/GB)
mask 0xf       : kind must be 0
mask in {3,c,5,f}: no dependent stages ([stage+0xe0] == 0)
LX only (0x17b85c): reject short programs that put a dependency source on
  pipe 0 feeding pipe 1 (a4==2) or pipe 2 feeding pipe 3 (a4==8)
```

The kind comes from `0x1d6388[ps_opcode]`: `0x42` TEX gives 0, `0x40`
TEXCOORD gives 2, `0x41` TEXKILL gives 3, and so on. **Mask 5 is not gated by
`TEPipeSelect`/`TEMultiplePipes`**, so the registry defaults (`4`, `7`) do
not affect LX dual-pipe allocation.

### 3.4 Pipe property and rate tables

Each 12-byte entry is `{multi, firstPipe, costTable}`. The LX table is at
`0x1dbe70` and is used at `0x17b98a`. The non-LX table is at `0x1d8680`, used
at `0x17b993`; `firstPipe` is read at `0x1d8684`, `0x17778a`:

```
LX     1:{0,0,single} 2:{0,1,single} 4:{0,2,single} 8:{0,3,single} 5:{1,0,PAIR}
       3,c,f and all others: cost table 0x1f23f8 (all zero -> score 0)
non-LX 1,2,4,8 single; 3:{1,0,PAIR} c:{1,2,PAIR} f:{1,0,QUAD}; 5: zero table
```

Linux `mtx1861` (`.data` `0x1b5c0`, 16-byte entries) marks mask 5 as the only
LX multi entry. Its cost pointer is `mtx3193`; the single-pipe pointer is
`mtx1951` and invalid masks point to `mtx1285`.

Per-stage rate `0x17b8d0` uses
`idx = kind*16 + {f=0:4, f=1:8, f=2:12, else 0} + 2*(MIP==2) + (MIN==3||MAG==3)`.
Here `f = 0x11e0[20:19]` (`0x17c0e3..0x17c111`). MIN, MAG and MIP are
`stage+0x114/0x118/0x11c`, the same fields packed into `0x11c0` at
`0x17c2cb..0x17c30e`. The entry is the pair `{a,b}`, and the rate is
`a + (b-a)*(1 - w*h/2^22)`. For kind 0 and `f=0`:

| filter | single `0x1dd330` | pair `0x1dc930` | quad `0x1dbf30` |
|---|---|---|---|
| bilinear, no mip-linear (idx 4) | 4/4 | 4/4 | 0/0 |
| trilinear, MIP=2 (idx 6) | **2/4** | **4/4** | 4/4 |
| aniso (idx 5) | 2/2 | 2/2 | 4/4 |
| trilinear+aniso (idx 7) | 1/2 | 2/2 | 4/4 |

Combine `0x1771d0` returns `min(min over stages, cap)`. The cap is 4.0, or
2.0 when more than 5 combiner slots are used. **On LX the cap is multiplied by
0.5, and with more than 2 stages the stage minimum is halved too**
(`0x17723e..0x17725e`).

**Cross-check against the 0528 captures:**
- Bilinear: quad scores 0, pair beats single on a tie, giving `0x00040103`
  with parked `0x0001000c`.
- Mip-nearest gives `0x00040103`.
- Trilinear: quad ties pair and comes first, giving `0x0004010f`.

All three match `parhelia-p690-mipmap-retirement-re.md`, so the decode is
confirmed on real vendor output.

**For P690:** single-stage bilinear and trilinear both score `min(x, 2.0)`,
so mask 5 (first entry) wins every tie. The vendor therefore runs every
independent single-texture stage on **pipes 0+2** (**PROVEN** by the table
order and selection rule). Its model says a single pipe drops to half rate on
large trilinear textures and a pair does not.

### 3.5 Where the other `0x1200` bits come from

In `0x17c0c2..0x17c0de`,
`0x1200 = (old & 0xe700ffff) | (((bc&1)<<1 | (c0&1)) << 27) | (code << 19) | (type << 16)`.
Here `code` is the TA opcode class and, on LX, code 1 is forced to 0
(`0x17c060`). The vendor-built enabled values in every capture carry
`[11:8] = 1` and `[18:16] = 4` (5 when projective).

## 4. Parking and emission (PROVEN)

The draw emitter is `PSER 0xcb8xx`. Its Linux twin is `KRN mtx1381`
@`0x3f680`, a live call from `0x41dc7`. Per active unit it emits:

```
{0x1200+4u, 0x11dc, 0x11e0, 0x11e4}       tag 0x797877(80+u)
{0x11c0, 0x11c4, 0x11d4, 0x11d8}          tag 0x76757170
{0x11cc, 0x11d0, 0x11c8, 0x11e8|PAD}      0x11e8 on LX, DMAPAD 0x1080 otherwise
                                          (PSER 0xcb973..0xcb999, KRN 0x3f72f..0x3f753)
```

It then parks the leftover pipes on the first unused unit: if the OR of the
active masks `!= 0xf`, it sets `unit[count].ctl = 0x10000 | (~union & 0xf)` and
`e8 = 0`, then clears the remaining units (PSER `0xcbac8..0xcbb0f`, KRN
`0x3f878..0x3f8d5`). The LX tail uses 9-dword packets from
`tagB[count] = 0x1d401c` and `tagC[count] = 0x1d4030` (KRN `.data+0xbf0` and
`+0xbd0`, initialized by `mtx82` @`0x407b0`):

```
tagB = {0x7a817a80, 0x7a812020, 0x20202020, 0x20202020, 0x20202020}  -> (0x1200,0x11e8,0x1204,0x11e8)
tagC = {0x7a837a82, 0x7a837a82, 0x7a837a82, 0x7a832020, 0x20202020}  -> (0x1208,0x11e8,0x120c,0x11e8)
non-LX tagA = {0x83828180, 0x83828120, 0x83822020, 0x83202020, 0x20202020}
```

So every unit's `0x11e8` is written after its own `0x1200+4u`. With mask 5 the
park value is `0x0001000a`. With two stages on `{1,4}` the parked unit is
`0x1208 = 0x0001000a`.

## 5. `0x11e8` on LX-class chips (PROVEN writers, INFERRED meaning)

### 5.1 Windows draw path `PSER 0x177350`

Owners are taken from the chosen candidate's `own[]` bytes (`[obj+0xc1c]+4..7`):

```
b4 = owner(pipe0) exists && its mask has pipe0 && !(a4 & 0xc)       -> bit 22
b0 = owner(p0),owner(p1) both exist && both a4&1 (0x1200 bit 8)      -> bit 21, pair (0,1)
b2 = same pair, both TA codes "plain" (0x176850)                     -> bit 24, pair (0,1)
b1, b3 = the same for pair (2,3)
per stage (0x177506..0x177555):
  e8 = (e8 & 0xfedfffff) | (pair01 if mask&3 else pair23 bits -> 21, 24)
  e8 = (e8 & 0xffafffff) | (b4 << 22)       ; bit 20 cleared
guard band shadow [obj+0x9a0] = 0x41800000 (16.0)   (0x1774d8)
```

### 5.2 The other vendor paths agree

- Linux GL: `mtx958` @`0x69650` sets `e8[u] = mtx1783[count*4+u]` (`0x69729`)
  and `0x12f4 = count<3 ? 32.0 : 16.0` on LX (`0x6974a..0x69772`).
- `mtx1783` (`.data 0x15860`), the PSER 2D texture engine table `0x1d63f0`
  and PARD `0x1d43f0` hold the same values:

  ```
  count 1, 2 : 0x00500000 x4                 (bit 22 | bit 20)
  count 3    : 0x01600000, 0x01600000, 0x00400000, 0x00400000
  count 4    : 0x01600000 x4                 (bits 24 | 22 | 21)
  ```

- The PSER 2D engine `0x172469..0x1724d1` writes `0x11e8` per unit only when
  `caps & 0x7fc00`, together with `0x12f4 = count<=2 ? 32.0 : 16.0`.

Section 5.1 reproduces these rows exactly for masks `{1}`, `{1,4}`, `{1,2,8}`
and `{1,2,4,8}`:
- Bits 21 and 24 appear only on units sitting in a fully owned pair.
- Bit 22 is always set.
- Bit 20 follows the `0x12f4 = 32.0` guard band.

Two consequences follow:
- Bit 20 is tied to the guard band. This correlation is PROVEN across three
  writers; what the bit does in hardware is INFERRED.
- For mask 5 the Windows form gives `0x00400000` with `0x12f4 = 16.0`. The
  Linux-table form gives `0x00500000` with `0x12f4 = 32.0`. Pipes 0 and 2 are
  in different pairs, so bits 21 and 24 stay clear.

### 5.3 Priming

- Engine or context init, LX only, **inside** the per-unit loop:
  `0x1200+4u = 0x00040100|(1<<u)`, `0x11c0 = 0`, `0x11c4 = 0x249`,
  `0x11c8 = 0`, `0x11cc = 0xffffffff`, `0x11d0..0x11d8 = 0`,
  **`0x11e8 = 0x00100000`** (PSER `0x2d60f..0x2d669`, KRN `mtx398`
  `0x32a77..0x32a98`). Then pad `0x1374`, `0x1200 = 0x0001000f` and
  `0x1204..0x120c = 0`.
- The priming pseudo-draw (KRN `mtx2663` @`0x3d3e0` from
  `mtx1192`/`mtx2423`/`mtx465` with DWGCTL `0x5000f5`/`0x1000f4`; PSER twin
  `0x2e6c0..0x2f0xx`):
  - programs four dummy 1x1 units `0x0004010u` (or `0x0005010u`)
  - computes the §5.1 flags, which gives `0x11e8 = 0x01600000` per unit
    (KRN `0x3dc81` / PSER `0x2eca0` bit 22, bit 20 cleared at
    `0x3d87d..0x3d898` / `0x2eed1..0x2eed9`)
  - emits `(0x1200,0x11e8,...,0x120c,0x11e8)` with the tags above, then
    `0x12f4 = 16.0` and `0x1300`

## 6. One-time and registry settings

| item | P690 value | evidence |
|---|---|---|
| `TEDLFCounters` -> MMIO `0x0bc0` | **Windows: `0x001e4087`** for all LX (`PSER 0x10c4ce test [ebx+0x1c],0x7fc00`, `0x10c4e9` / `0x10c513` for `0x4087`; written at `0x303af` from `[dev+0xa67c]+0x34c`) | PROVEN |
| same, Linux 2009 driver | `0x001e4087` only for Sundog (`KRN mtx722 0x75943 test [r12+0x20],0x1c00`); **P690 gets `0x00004087`** (`0x764aa`); written by `mtx1475 0x3e830` | PROVEN; corrects the "capable/P690 branch" wording in `parhelia-p690-clock-fifo-texture-performance-re.md` |
| `TEPipeSelect` (4) | gates masks 3/c/f only (§3.3). Its other readers are `0x88edb`, a WORD store to `[+0x482]` that matches the DX7 `wMaxSimultaneousTextures` override in `parhelia-gl-limits.md` §6.2, and priming `0x2e74e`, where the result is discarded | PROVEN |
| `TEMultiplePipes` (7) | gates masks 3/c/f only; no effect on mask 5 | PROVEN |
| `TEEnable` (1) | 0 forces unit count 0 (`KRN 0x3f69b..0x3f6ac`) | PROVEN |
| `TEFastTrilinear` (0) | stored (PSER `[esi+0xb8]`, KRN holder `+0x5a0`); no reader found in either binary | PROVEN absence |

## 7. Why our values lock up (INFERRED from the tables)

- `0x00040103` (pipes 0+1) and `0x0004010f` do not occur on the LX class: the
  cost table is zero and no candidate contains them.
- A second unit on pipe 1 with two stages (`0x00040102`) is never generated
  either. The vendor pairs pipes 0 and 1 only with three or four stages, and
  then sets `0x11e8` bits 21 and 24 on both units and uses `0x12f4 = 16.0`.
- Our driver writes a single `0x11e8 = 0`. A pair (0,1) owned by two
  different units therefore runs without the pair flags. That is the most
  likely cause of the lock-up, but it is untested.
- The working `{0x40101, 0x40104}` route is exactly the LX two-stage table.

## 8. Texture pipe count

- **PROVEN:** 4 pipe bits are programmed simultaneously on LX-class chips:
  - the LX four-stage table `{1,2,4,8}`
  - the PSER 2D filter blit `0x1dfca..0x1e1de`: `0x1200 = 0x00040e01`,
    `0x1204 = 0x00880102`, `0x1208 = 0x00880104`, `0x120c = 0x00800108`, each
    followed by `0x11e8 = 0x01600000` under `caps & 0x7fc00`
    (`0x1e043..0x1e1b1`)
- **PROVEN:** exactly one gang, {0,2}, and no quad on LX. The LX rate cap is
  half the Parhelia cap.
- **INFERRED:**
  - LX is organised as two pipe pairs (0,1)/(2,3); these share the resource
    that `0x11e8` bits 21/24 describe.
  - Pipes 0 and 2 are the two halves that can work one stage together.
  - The halved cap reflects half the per-clock pixel width of Parhelia-512.

## 9. Differences from the current open driver

| vendor (LX) | open driver (`reference/p3d_state.c`) |
|---|---|
| single stage: `0x1200 = 0x00040105`, park `0x1204 = 0x0001000a` | `0x00040101`, `0x1204 = 0` |
| `0x11e8` written after each `0x1200+4u`, `0x00500000` (1–2 stages) | one global `0x11e8 = texture_cache_probe` (0); the comment calling it a "global latch" contradicts both vendor emitters |
| two stages: park `0x1208 = 0x0001000a` | `0x1208 = 0` |
| per-unit priming `0x11c4 = 0x249` and `0x11e8 = 0x00100000` | none |
| `0x0bc0`: Windows `0x001e4087`, Linux `0x00004087` | not written by default (module parameter exists) |
| `0x12f4` paired with `0x11e8` bit 20 | always 32.0 |

## 10. Hardware test proposal (minimal, ordered)

Run the same P690 textured-fill probe and the known-good triangle at every
step. Verify on scanout, use a fence timeout, and stop after the first
non-retiring submit. Change one class of input per step. Never try `0x3`,
`0xc` or `0xf` masks on P690.

1. **Baseline:** current state (`0x00040101`, global `0x11e8 = 0`). Record
   Mpix/s at 1:1 bilinear (about 262) and the image.
2. **TEDLF:** set `0x0bc0 = 0x001e4087` once at engine init through the
   existing module parameter and read it back. Repeat step 1. If step 5 later
   fails, repeat with `0x00004087` (the Linux P690 value).
3. **Per-unit priming** (once per context, before the first textured draw):
   for `u = 0..3` emit `0x1200+4u = 0x00040100|(1<<u)`, `0x11c0 = 0`,
   `0x11c4 = 0x00000249`, `0x11c8 = 0`, `0x11cc = 0xffffffff`,
   `0x11d0 = 0x11d4 = 0x11d8 = 0`, `0x11e8 = 0x00100000`. Then emit
   `0x1374 = 0`, `0x1200 = 0x0001000f`, `0x1204 = 0x1208 = 0x120c = 0`.
   Repeat step 1.
4. **Banked `0x11e8` on the known pipe:**
   - Remove the global `0x11e8` write. Draw with `0x121c = 0`,
     `0x1200 = 0x00040101`, the unchanged surface, filter and border writes,
     then `0x11e8 = 0x00500000` in the same bank.
   - Follow with `0x1204 = 0`, `0x11e8 = 0`; `0x1208 = 0`, `0x11e8 = 0`;
     `0x120c = 0`, `0x11e8 = 0`. Keep `0x12f4 = 0x42000000`.
   - 4b: the same, but with the vendor park `0x1204 = 0x0001000e`.
5. **Second pipe:**
   - Use step 4 (or 4b if it retired), changing only `0x1200 = 0x00040105`.
   - If 4b retired, park with `0x1204 = 0x0001000a`.
   - Keep the TA words (`0x1264/0x1260` source pipe 0 = PSER `firstPipe` for
     mask 5).
   - Expect the same image and a higher fill rate. The rate gain is
     INFERRED; the vendor's model predicts it for large or trilinear
     textures.
6. **Windows variant:** as step 5, with `0x11e8 = 0x00400000` and
   `0x12f4 = 0x41800000`.
7. **Trilinear:** once mip retirement works, repeat steps 5/6 with
   `0x11c0` MIP=2 and compare against `0x00040101`.
8. **Two textures, vendor form:**
   - `0x1200 = 0x00040101`, `0x11e8 = 0x00500000`
   - `0x1204 = 0x00040104`, `0x11e8 = 0x00500000`
   - `0x1208 = 0x0001000a`, `0x11e8 = 0`
   - `0x120c = 0`, `0x11e8 = 0`
9. **Optional, last:** a three-stage `{0x40101, 0x40102, 0x40108}` draw with
   `0x11e8 = {0x01600000, 0x01600000, 0x00400000}`, `0x120c = 0x00010004`,
   `0x11e8 = 0` and `0x12f4 = 0x41800000`. This tests the (0,1) pairing
   explanation.

## 11. Reproduction

```sh
D=xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll
K=matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -d -Mintel --start-address=0x17b720 --stop-address=0x17b8cd $D   # validator
objdump -d -Mintel --start-address=0x1772a0 --stop-address=0x177306 $D   # table select
objdump -d -Mintel --start-address=0x177350 --stop-address=0x17755f $D   # LX 0x11e8 flags
objdump -d -Mintel --start-address=0x179510 --stop-address=0x1796ea $D   # selection
objdump -d -Mintel --start-address=0x1767e0 --stop-address=0x176845 $D   # mask -> 0x1200
objdump -d -Mintel --start-address=0xcb8f0 --stop-address=0xcbbbc $D     # emitter + park
objdump -d -Mintel --start-address=0x2d5b3 --stop-address=0x2d6fc $D     # LX priming
objdump -d -Mintel --start-address=0x10c4ce --stop-address=0x10c540 $D   # TEDLF default
objdump -dr -Mintel --start-address=0x3f680 --stop-address=0x3fc70 $K    # mtx1381
objdump -dr -Mintel --start-address=0x6eff0 --stop-address=0x6f080 $K    # mtx1918
objdump -dr -Mintel --start-address=0x710a0 --stop-address=0x71340 $K    # mtx1414
objdump -s -j .data --start-address=0x15860 --stop-address=0x158b0 $K    # mtx1783
python3 - <<'EOF'                                   # LX candidate tables
import struct
d=open('xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll','rb').read()
r=lambda va,n: d[0x191b00+va-0x1a1b00:][:n]
for n,p in enumerate(struct.unpack('<5I', r(0x1dbe58,20))):
    for s in range(24):
        t=p+s*0x44; c=struct.unpack('<I', r(t,4))[0]
        for i in range(c):
            e=r(t+4+8*i,8); print(n,s,e[:4].hex(),e[4:].hex())
EOF
```
