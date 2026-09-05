# Parhelia block-load state file — index map

What lives in each entry of the 0x180 x 16-byte indexed **block state file**, how
the validators compute it, and how to ship it with a DMA block-load packet.

Recovered from `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`
(unstripped, `mtxNNNN` symbols).  Reads foundation-first: this document assumes
`docs/parhelia-state-slot-map.md`, which establishes the record stream, the
software shadow (HWSTATE) and the existence of the block file.  Symbol addresses
below are `.text` offsets in that object.

Naming used throughout:

| name | meaning |
|---|---|
| HWSTATE | `[[driverobj+0x30]+0x78]`, the GL state shadow |
| HWREG | `[[HWSTATE+0x8]+0x50]`, the object holding both block-file banks |
| **BANK** | one bank: `bitmapBase`; registers at `bitmapBase + 0x230 + index*0x10` |
| raster | `HWSTATE + 0x1bd8`; validators take this as `rsi`, so `raster[X] = HWSTATE[0x1bd8+X]` |
| DS | `[HWSTATE+0x1e90]` = `HWSTATE+0x0d01`, the depth/stencil sub-state block |
| VF | `[HWSTATE+0x1e80]` = `HWSTATE+0x0d98`, the viewport-depth + fog sub-state block |

**The block file's absolute MMIO address is still unknown and is deliberately not
guessed here.**  You do not need it: the DMA engine resolves the index.

---

## 1. Minimum viable state for one untextured Gouraud triangle

This is the vendor's own answer, not a reconstruction: `mtx2209` @0x8e000 (and
its twin `mtx867` @0x8eab0) is the context constructor, and it writes exactly
this set and marks every one of these indices dirty, so the very first flush
after context creation ships them.  Everything else in the file starts as
`{0,0,0,0}` (`mtx3107` @0x8fef0 constructs all 0x180 entries via `mtx2321`
@0x8f600, which stores four zero dwords).  CONFIRMED.

Emit in this order.  Every value is a 32-bit IEEE float unless noted.

### 1.1 One packet: indices 0x172 .. 0x17e (13 entries)

```
dword0 = 0x411a0037          ; 0x41000000 | (13<<17) | (13*4+3)
dword1 = 0x0000000d          ; count = 13
dword2 = 0x00000172          ; start index
dword3 = 0x00000001
dword4 = 0x00000000
then 13 * 4 dwords, in index order:
```

| idx | d0 | d1 | d2 | d3 | what it is |
|---|---|---|---|---|---|
| 0x172 | `3f800000` 1.0 | `00000000` 0.0 | `00000000` 0.0 | `3f800000` 1.0 | point/line size group; d3 = half-width |
| 0x173 | `3f800000` 1.0 | `3f800000` 1.0 | `3f000000` 0.5 | `bf000000` -0.5 | size clamp min/max + two constants |
| 0x174 | `45ff0000` 8160.0 | `45ff0000` 8160.0 | `c5ff0000` -8160.0 | `c5ff0000` -8160.0 | guard-band clip limits (+X,+Y,-X,-Y) |
| 0x175 | `43000000` 128.0 | `00000000` | `00000000` | `00000000` | d1..d3 copied from the surface block |
| 0x176 | 0 | 0 | 0 | 0 | not written by any code path found |
| 0x177 | 0 | 0 | 0 | 0 | not written by any code path found |
| 0x178 | `00000000` 0.0 | `00000000` 0.0 | `3f000000` 0.5 | `49400000` 786432.0 | polygon offset factor / units / line half-width / constant |
| 0x179 | `bf800000` -1.0 | `3e75fdf0` | `bd635847` | `3c1d955b` | EXP coefficient table |
| 0x17a | `3f800000` 1.0 | `baaec3ff` | `39218489` | `b77fe5fe` | EXP coefficient table |
| 0x17b | `3fb8aa3b` log2e | `bf38aa3b` | `3ef6384f` | `beb8aa3b` | LOG coefficient table |
| 0x17c | `3f800000` 1.0 | `3e93bb63` | `be76384f` | `3e530bb1` | LOG coefficient table |
| 0x17d | `4b000000` 2^23 | `3d4ccccd` 0.05 | `41f80000` 31.0 | `3d042108` 1/31 | fixed-point / range constants |
| 0x17e | `00000000` 0.0 | `3d800000` 0.0625 | `00000000` 0.0 | `00000000` 0.0 | sub-pixel / sample scale group |

CONFIRMED for every literal: read from `mtx2209` @0x8e434..0x8e685 and
@0x8e7c2..0x8ea5e.  The 0x179/0x17a values marked with bit patterns only are
computed at runtime as `pow(ln2,n)/(±n!)` (`0x8e8db`..`0x8e9d7`); the bit
patterns given are the exact IEEE encodings of those expressions and are
INFERRED to that extent.

> The existing `parhelia-state-slot-map.md` §6.3 lists four of these rows with
> the wrong dword order or wrong constants (it read the register `ebx` as 1.0
> where it is 0.0).  **Corrections: 0x172 is `{1,0,0,1}` not `{0,1,1,0}`;
> 0x175 is `{128,0,0,0}` not `{128,1,1,1}`; 0x178 is `{0,0,0.5,786432}` not
> `{1,1,0,786432}`; 0x17e is `{0,0.0625,0,0}` not `{1,0.0625,1,1}`.**  All four
> are re-read instruction by instruction here.

### 1.2 One packet: indices 0x14d .. 0x14e (2 entries)

```
dword0 = 0x4104000b ; dword1 = 2 ; dword2 = 0x14d ; dword3 = 1 ; dword4 = 0
```

| idx | d0 | d1 | d2 | d3 |
|---|---|---|---|---|
| 0x14d | 0 | `(float)VF-derived int`, see §3 | 0 | `42000000` 32.0 |
| 0x14e | `3f800000` 1.0 | `00000000` 0.0 | 0 | 0 |

0x14e is **depth range**: `d0 = 1/(zFar-zNear)`, `d1 = -zNear/(zFar-zNear)`.
For the GL default range `[0,1]` that is `{1.0, 0.0}`.  CONFIRMED
(`mtx3654` @0x8b6ad..0x8b709).  `mtx2209` only initialises 0x14d d3 = 32.0;
0x14e is left zero by the constructor, so **a from-scratch driver must program
0x14e itself before the first draw** — leaving it `{0,0}` collapses all depth.

### 1.3 One packet: indices 0x160 .. 0x161 (2 entries)

```
dword0 = 0x4104000b ; dword1 = 2 ; dword2 = 0x160 ; dword3 = 1 ; dword4 = 0
0x160 = { 0, 0, 0, 3f800000 }     ; d3 = 1.0
0x161 = { 0, 0, 0, 00000000 }     ; d3 = 0.0
```
CONFIRMED (`mtx2209` @0x8e6d5/0x8e6e5, mirrored at 0x8e6ed/0x8e6f4).

### 1.4 Matrix blocks — needed once transform is on

Six 4x4 float matrices, each occupying four consecutive entries.  `mtx2209`
@0x8e8a3 memcpy's a 0x40-byte identity matrix (`.rodata+0x2bc0`,
`{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}`) into three of them.  For a triangle with
an identity modelview/projection, program:

```
0x149..0x14c = identity 4x4      (packet: count=4, start=0x149, dword0=0x41080013)
```

CONFIRMED that 0x149 is a matrix head (`mtx3654` @0x8cc7f copies 16 dwords
through `[HWREG+0x358]` and then marks 0x149..0x14c).

### 1.5 Not in this file

**Depth test / depth func / depth mask, stencil, alpha test, blend factors,
blend equation, colour mask are NOT block-file state.**  See §6.  Program those
through the byte-index namespace (`reg = 0x1000 + index*4`, ring tag
`0xc0afafc0` / `0x2121xxxx`) exactly as the repo already documents.

So the complete "first triangle" block-file program is: §1.1, §1.2, §1.3 and, if
you transform on the GPU, §1.4.  Four packets, 21 entries, 84 payload dwords.

---

## 2. File and bank geometry — CONFIRMED

`mtx3107` @0x8fef0 constructs a bank; `mtx1785` @0x8ffb0 / `mtx3105` @0x8ffe0
walk it from `base+0x1a30` down to `base+0x230` in 0x10 steps.  One bank:

```
base+0x000   dirty bitmap 0 : u32 summary; u32 word[13]     <- the live one
base+0x038   dirty bitmap 1 : u32 summary; u32 word[12]
base+0x06c   dirty bitmap 2 : u32 summary; u32 word[12]
base+0x0a0   u32 flag, +0xa4 ring head, +0xa8 ring tail
base+0x0b0   client-upload slot ring: 6 entries x 0x40      (see §5.3)
base+0x230   registers: 0x180 entries x 0x10 bytes
base+0x1a30  end
```

The two banks inside HWREG:

```
HWREG+0x07e8   bank A bitmap      HWREG+0x0a18   bank A registers
HWREG+0x2218   bank B bitmap      HWREG+0x2448   bank B registers
```
`bankB_addr = bankA_addr + 0x1a30` exactly, verified on every mirrored store
pair in `mtx740`, `mtx2497`, `mtx3654` and `mtx2209`.

`entry(bank, idx) = bitmapBase + 0x230 + idx*0x10`, four dwords, four distinct
values.  Independently re-confirmed by the pointer tables of §3.4.

---

## 3. The validators

`mtx3712` @0x188f0 is the validate pass.  It dispatches one validator per dirty
group.  **Only the raster group reaches the block file**:

| dirty group | validator | target object | block file? |
|---|---|---|---|
| `[HWSTATE+0xc87]` depth/stencil | `mtx360` @0x12370 | `[dev+0x40]+0x88` | no |
| `[HWSTATE+0xc08]` blend/alpha/colour-mask | `mtx2480` @0x129e0 | `[dev+0x40]+0x60/0xb00` | no |
| `[HWSTATE+0xd54]` fog enables | (@0x189bc) | `[dev+0x40]+0xa8` | no |
| `[HWSTATE+0x1b44]` | `mtx3024` @0xb560 | `[dev+0x3288]` | no |
| `[HWSTATE+0xdbb]` texture/fragment | `mtx297` | `[dev+0x58]` | no |
| **`[HWSTATE+0x1bd8]` raster** | **`mtx3654` @0x8b510** | `rdi = HWREG` | **yes** |

CONFIRMED: of the 81 `mtx2702` (mark-dirty) call sites in the whole object, none
is inside `mtx360` or `mtx2480`.  The complete set of callers is `mtx4027`,
`mtx1322`, `mtx558`, `mtx2441`, `mtx1394`, `mtx740`, `mtx2497`, `mtx2588`,
`mtx2926`, `mtx2522`, `mtx309`, `mtx3654`, `mtx2209`, `mtx867`.

`mtx3654` is called as `mtx3654(rdi=HWREG, rsi=raster, rdx=HWSTATE+0x1e98,
rcx=[dev+0x40]+0x1c0)` and calls the leaf validators with the same `rdi/rsi`.

### 3.1 `mtx740` @0x864f0 — polygon offset -> **0x178 d0, d1**

The anchor, re-verified end to end.

Gate (CONFIRMED, and it doubles as proof of two enum tables):

```
e = raster[0x09]        ; HWSTATE[0x1be1]  polygon-offset enables
m = raster[0x0c] & 0x30 ; HWSTATE[0x1be4] bits 4-5, polygon mode
run only if   (e&0x20 && m==0x30)   ; OFFSET_FILL  && mode==3 (FILL)
           || (e&0x10 && m==0x20)   ; OFFSET_LINE  && mode==2 (LINE)
           || (e&0x08 && m==0x10)   ; OFFSET_POINT && mode==1 (POINT)
```

Arithmetic (@0x865e6..0x86679):

```
factor = raster[0x270]              ; HWSTATE[0x1e48], key 0x05 slot 0x15
units  = raster[0x274]              ; HWSTATE[0x1e4c], key 0x05 slot 0x16
units *= DS[0x15]                   ; HWSTATE[0xd16], per-depth-format resolution
if !(raster[0x08] & 1) {            ; HWSTATE[0x1be0] bit0
    factor *= [HWREG+0x6f4]
    units  *= (float)[HWREG+0x6f0] * [HWREG+0x6f4]
}
block[0x178].d0 = factor            ; both banks
block[0x178].d1 = units             ; both banks
```

* `[HWREG+0x6f4]` is a **sign** taken from a depth-format LUT at `.data+0x8b20`
  indexed by `min(DS[0x0c], 8)` (`mtx3654` @0x8cc55):
  `{0.0, 0.0, -1.0, 1.0, -1.0, 1.0, 1.0, 1.0, 0.0}`.  Default 1.0 (`mtx3995`
  @0x851ab).  CONFIRMED.
* `[HWREG+0x6f0]` is an integer read once at init from a driver configuration
  object through a vtable call (`mtx2163` @0x84e4d).  Not a GL value; treat it
  as a units multiplier and use 1.  CONFIRMED as a config read, value INFERRED.
* Slots 0x17/0x18/0x19 of key 0x05 are the three enable predicates above — not
  registers.  CONFIRMED.

### 3.2 `mtx2497` @0x867a0 — point/line size -> **0x172 d3**

Called as `mtx2497(rdi=HWREG, rsi=raster, rdx=[dev+0x40]+0x1c0, xmm0=DS[0x39])`.
`DS[0x39]` (`HWSTATE[0xd3a]`) is a float scale carried in the depth/stencil
block; it multiplies sizes and divides distances, which is the signature of a
supersample/AA scale factor (INFERRED as to meaning, CONFIRMED as to use).

```
v = raster[0x258]                       ; HWSTATE[0x1e30], key 0x0c slot 0x9a
if !(raster[0x09] & 2) {                ; HWSTATE[0x1be1] bit1 = slot 0x9d
    if (scaleSelect == 0) v *= DS[0x39]
    v = max(block[0x173].d0, min(v, block[0x173].d1))   ; clamp to the size range
}
if ((HWREG[0xe4]&3)==1 && !(raster[0x09]&2) && !(HWREG[0xe0]&0x40)) {
    n = (int)(v + 0.5)
    HWREG[0xe2] bit1 = (n is even)
    v = 0.5 * (float)n                  ; snapped half-width
}
block[0x172].d3 = v
```
CONFIRMED (@0x86900..0x86998).  Note `block[0x173].d0/d1` are read back out of
**bank A** through the pointer table — the file is its own state, the validator
compares against it.

### 3.3 `mtx3654` @0x8b510 — the raster validator

All rows CONFIRMED unless tagged.

| block | dwords | source | arithmetic | site |
|---|---|---|---|---|
| **0x14e** | 0,1 | `VF[0x05]` gate, `VF[0x06]`=zNear, `VF[0x0a]`=zFar | `d0 = 1/(far-near)`, `d1 = -near/(far-near)` | 0x8b6ad |
| **0x154** | 0..3 | `VF[0x04]` gate, `VF[0x0f]`=start, `VF[0x13]`=end, `VF[0x17]`=density, `VF[0x1b]`=mode | `d0 = start`; `d1 = end/(end-start)`; `d2 = 1/(end-start)`; `d3 = density*log2e` (mode!=2) or `density²*log2e` (mode==2) | 0x8ccce, 0x8d394 |
| **0x155** | 3 | `raster[0x254]` = `HWSTATE[0x1e2c]`, key 0x0c slot 0xaa | verbatim | 0x8d230 |
| **0x172** | 0,1,2 | `raster[0x264/0x268/0x26c]` = `HWSTATE[0x1e3c/0x1e40/0x1e44]`, key 0x0c slots 0x9e/0x9f/0xa0 | `v * (1.0 / DS[0x39])` | 0x8bb29 |
| **0x173** | 0 | `raster[0x25c]` = `HWSTATE[0x1e34]`, key 0x0c slot 0x9b | `v * DS[0x39]` | 0x8bbd7 |
| **0x173** | 1 | `raster[0x260]` = `HWSTATE[0x1e38]`, key 0x0c slot 0xa6 | `min(v * DS[0x39], 2048.0)` | 0x8bbdc |
| **0x175** | 1,2,3 | `[HWSTATE+0x1ad8]+0x60/0x64/0x68` | copied verbatim as dwords | 0x8bff6 |
| **0x14d** | 1 | `[HWSTATE+0x0c6b]+0x0c` (int) | `(float)(int)v` | 0x8bf46 |
| **0x178** | 2 | `raster[0x278]` = `HWSTATE[0x1e50]`, key 0x05 slot 0x1b | `v < 2.0 ? 0.5 : 0.5*trunc(v)` | 0x8c715 |
| **0x17e** | 0,2,3 | `WORD [raster+0x27e]` = `HWSTATE[0x1e56]` | `d3 = (float)w * 16.0`; `d2 = 0.5/d3`; `d0 = 0.0` | 0x8b9c2 |
| **0x139+4k** | 4x4 | `[raster + 0x78 + (t+0x22)*8]` | 16-dword matrix copy via `mtx3265`, k = 0..3 | 0x8c360 |
| **0x149** | 4x4 | `[raster+0x80]` = `[HWSTATE+0x1c58]` | 16-dword matrix copy | 0x8cc7f |

`mtx3654` also *reads* 0x134/0x135/0x137 (comparing three dwords against zero to
set the software flags `HWREG+0x6e4/0x6e5`) without writing them — those are
produced by `mtx1322`/`mtx2441`.

The predicate bits: `raster[0x09] bit0` (key 0x0c slot 0x9c) is copied to
`HWREG+0x6e9` and gates the 0x172 path; `bit1` (slot 0x9d) bypasses the size
scale/clamp.  Neither is a register.  CONFIRMED (@0x8bab9).

### 3.4 Pointer tables — third independent confirmation of the addressing

`mtx2209` builds three tables inside HWREG, and every entry satisfies
`ptr = HWREG + 0xa18 + index*0x10`:

| table | contents | block index |
|---|---|---|
| `[HWREG+0x238 + n*8]`, n=0..0x1f | `HWREG + 0xa18 + n*0x70` | `7n` |
| `[HWREG+0x360 + n*8]`, n=0..0x1f | `HWREG + 0xa58 + n*0x70` | `7n + 4` |
| `[HWREG+0x338 + k*8]`, k=0..3 | `HWREG + 0x1da8 + k*0x40` | `0x139 + 4k` |
| `[HWREG+0x358]` | `HWREG + 0x1ea8` | `0x149` |
| `[HWREG+0x3c48 + …]` | see `parhelia-state-slot-map.md` §4.1 | 0x134..0x178 |

So block indices 0x000..0x131 are a regular array of **7-entry groups** (the
`[HWREG+0x3c88+k*8]` sub-range k=0..11 covers `0xe0 + 7k`, the twelve texture
stages), and 0x139..0x14c is a run of **4-entry 4x4 matrices**.  Group structure
CONFIRMED; "texture stage" naming INFERRED.

---

## 4. Block-index map

CONFIRMED = the index and the writing code were read.  INFERRED = the GL meaning
is deduced from the arithmetic.

| index | holds | dword layout | fed by |
|---|---|---|---|
| 0x000..0x131 | 7-register groups, `group g` at `7g` | — | texture stages (`mtx309` @0x89cb0 marks `0xe0..0xe6`); CONFIRMED index, INFERRED role |
| 0x134..0x137 | 4x4 matrix, read by `mtx3654` as three-dword-vs-zero tests | 16 floats | `mtx1322`, `mtx2441`; CONFIRMED |
| 0x138 | one entry | 4 floats | `mtx2588` @0x86bf0; CONFIRMED index |
| 0x139..0x148 | four 4x4 matrices, `0x139+4k`, k=0..3 | 16 floats each | `mtx3654` @0x8c360, source selected per unit; CONFIRMED |
| 0x149..0x14c | 4x4 matrix; init = identity | 16 floats | `mtx3654` @0x8cc7f, `mtx1394` @0x85220; CONFIRMED |
| 0x14d | d1 = `(float)(int)[HWSTATE+0xc77]`; d3 init 32.0 | | `mtx3654` @0x8bf46; CONFIRMED |
| **0x14e** | **depth range** | d0 `1/(far-near)`, d1 `-near/(far-near)` | `mtx3654` @0x8b6ad, `mtx309`; CONFIRMED |
| **0x154** | **fog parameters** | d0 start, d1 `end/(end-start)`, d2 `1/(end-start)`, d3 `density*log2e` (or `density²*log2e` for EXP2) | `mtx3654` @0x8ccce; CONFIRMED mechanically, GL naming INFERRED (the log2e factor and the linear-fog reciprocal are unambiguous) |
| 0x155 | d3 = key 0x0c slot 0xaa verbatim | | `mtx3654` @0x8d230; CONFIRMED |
| 0x156..0x159 | 4x4 matrix | 16 floats | `mtx1322`, `mtx2441`; CONFIRMED |
| 0x160, 0x161 | d3 = 1.0 / 0.0 at init | | `mtx2926` @0x87220; CONFIRMED |
| 0x16c..0x171 | six entries reached through `[HWREG+0x3d00+k*8]` | | `mtx2522` @0x87790; CONFIRMED |
| **0x172** | **point/line size group** | d0..d2 = slots 0x9e/0x9f/0xa0 `/ DS[0x39]`; **d3 = the clamped, optionally half-and-snapped size** | `mtx3654`, `mtx2497`; CONFIRMED |
| **0x173** | **size clamp range** | d0 = min (slot 0x9b `* DS[0x39]`), d1 = max (slot 0xa6 `* DS[0x39]`, capped 2048.0), d2/d3 constants 0.5 / -0.5 | `mtx3654` @0x8bbd7; CONFIRMED |
| **0x174** | **guard band** | `{+X, +Y, -X, -Y}` = `{8160, 8160, -8160, -8160}` | init only (`mtx2209` @0x8e7d0); CONFIRMED literal, "guard band" INFERRED |
| 0x175 | d0 = 128.0 constant; d1..d3 copied from `[HWSTATE+0x1ad8]+0x60..0x68` | | `mtx3654` @0x8bff6; CONFIRMED |
| 0x176, 0x177 | never written | zero | — |
| **0x178** | **polygon offset + line half-width** | d0 factor, d1 units (both scaled, §3.1), d2 = `0.5*trunc(slot 0x1b)` clamped to ≥ 0.5, d3 constant 786432.0 | `mtx740`, `mtx3654`; CONFIRMED |
| 0x179, 0x17a | EXP coefficient table | `{c1,c2,c3,c4}` / `{1.0,c5,c6,c7}`, `cn = pow(ln2,n)/(±n!)` | init only; CONFIRMED literals, role INFERRED |
| 0x17b, 0x17c | LOG coefficient table | `{log2e, -log2e/2, log2e/3, -log2e/4}` / `{1.0, log2e/5, -log2e/6, log2e/7}` | init only; CONFIRMED literals, role INFERRED |
| 0x17d | range constants | `{2^23, 0.05, 31.0, 1/31}` | init only; CONFIRMED |
| 0x17e | sub-pixel / sample group | d3 = `(float)WORD[HWSTATE+0x1e56] * 16.0`, d2 = `0.5/d3`, d0 = 0.0, d1 constant 0.0625 | `mtx3654` @0x8b9c2; CONFIRMED |
| 0x17f | never written | zero | — |

### Priority GL state — where it actually is

| GL state | block index | note |
|---|---|---|
| depth range | **0x14e d0,d1** | CONFIRMED |
| polygon offset factor/units | **0x178 d0,d1** | CONFIRMED |
| polygon mode | — | gate only; `HWSTATE[0x1be4]` bits 4-5, 1=POINT 2=LINE 3=FILL, re-confirmed by `mtx740` |
| point size / line width | **0x172 d3**, bounds in **0x173 d0,d1** | CONFIRMED |
| line width (second path) | **0x178 d2** | key 0x05 slot 0x1b, half-width |
| fog start/end/density/mode | **0x154** | CONFIRMED |
| guard band / clip limits | **0x174** | CONFIRMED literal |
| depth test / func / mask | **not in this file** | `mtx360`, byte-index namespace |
| stencil (all) | **not in this file** | `mtx360` |
| alpha test enable / func / ref | **not in this file** | `mtx2480` |
| blend enable / factors / equation | **not in this file** | `mtx2480` |
| colour mask | **not in this file** | `mtx2480` |
| cull face / front face | **not in this file** | consumed as gates and by `mtx2480`/`mtx360` groups |
| scissor | **not in this file** | RECT records, `0x1328/0x132c` (see `parhelia-raster-clip.md`) |
| viewport | **not in this file** | RECT record (key 0x26) |

That negative half is the single most useful result here: **the block file is
float setup state — transform matrices, depth range, fog, sizes, guard band,
transcendental tables.  Per-fragment test/blend state is integer state and
travels in the byte-index namespace.**  A driver that tries to express depth
func or blend factors as block entries will find no index to write.

---

## 5. Dirty-bitmap protocol

### 5.1 Marking — `mtx2702` @0x8f560

`mtx2702(rdi = bitmapBase, esi = firstIndex, edx = lastIndex)`, where
`lastIndex == 0` means "same as first".  It sets bits `first..last` **inclusive**
in `bitmapBase[4 + (i>>5)*4]` and ORs a word-presence mask into `bitmapBase[0]`:

```
word(i)     = *(u32*)(bitmapBase + 4 + (i>>5)*4)
bit(i)      = 1 << (i & 31)
summary bit = 1 << (i >> 5)     ; OR'd into *(u32*)bitmapBase
```
CONFIRMED.  The summary word is the flush trigger: `mtx693` @0x437c9 calls the
emitter only when `*(u32*)bitmapBase != 0`.

### 5.2 Emission — `mtx1342` @0x90010

`mtx1342(r11 = bitmapBase, rsi = ring)` scans indices 0..0x17f, coalesces each
maximal run of consecutive dirty indices, and splits every run into packets of
at most **0x3f (63) entries** — not 0x40; the code is
`cmp ebx,0x40 ; cmovae ebx,0x3f` at 0x901f0.  That matters because the count is
carried in a 6-bit field.  Per packet:

```
dword 0 : 0x41000000 | ((count << 17) & 0x007e0000) | ((count*4 + 3) & 0xffff)
dword 1 : count                 ; entries
dword 2 : start index           ; 0..0x17f
dword 3 : 1
dword 4 : 0
payload : count * 16 bytes, copied from bitmapBase + 0x230 + start*0x10
ring    : advanced by (count*4 + 5) dwords
```
CONFIRMED (0x901d6..0x9025f).  Ring space is checked as
`(runLength*4 + 0x23)` dwords before the first packet of a run.

**Entries need not be contiguous.**  Non-adjacent dirty indices simply produce
separate packets; a run that spans 200 indices produces four packets of 63,63,63,11.
Nothing forbids a driver from emitting one packet per entry (`count = 1`,
`dword0 = 0x41020007`) — the vendor coalesces only to save ring space.

`mtx1342` does **not** clear the dirty bits; the caller does.

### 5.3 Client-supplied payload

Before scanning, `mtx1342` ORs the six dwords at `bitmapBase + 0xc8 + n*0x40`
(n = 0..5) into a word-presence mask.  If a dirty index falls in a word that
mask claims, it walks the slot ring at `bitmapBase + 0xb0 + n*0x40` — entry
layout `{u8 hasExtra @0, void* data @8, u32 startIndex @0x10, u32 extra @0x14,
u32 wordMask @0x18, u32 word[] @0x1c}` — and, on a hit, **replaces the payload
source with the client's buffer** (`src = data - startIndex*0x10`, 0x902da).
This is the destination of record key 0x42 (`mtx558` @0x45462), which registers
`(index, count, pointer)`.  CONFIRMED as to the substitution; the tie to key 0x42
is INFERRED (same triple, same 0x40-stride ring).

An open driver can ignore this path entirely and always ship its own shadow.

---

## 6. Which bank, and do you need two?

`mtx3995` @0x84fb0 selects the bank to flush and stores it into
`[[HWREG+0x798]+0x40 + 0x260]`:

```
if (HWREG[0x7a0] != 0)  flush bank A (HWREG+0x7e8)
else                    flush bank B (HWREG+0x2218)
```
`mtx2209` leaves `HWREG[0x7a0] = 0`, so **bank B is the default flushed copy**.
`HWREG[0x7a0]` is set to 1 by `mtx3654` @0x8bece and cleared at 0x8cde2 after a
merge pass that folds bank A's secondary bitmaps into bank B's.  CONFIRMED.

Validators keep **bank A** as their comparison shadow (every pointer in the
`[HWREG+0x3c48…]` table points into bank A, and every "has it changed?" read
comes from there) and write both copies.

They are *mostly* mirrors, but not exactly.  Two confirmed asymmetries:

* `mtx2497` @0x86970/0x86977 writes the **snapped** half-size `0.5*round(v)` to
  bank A's `0x172 d3` and the **un-snapped** clamped `v` to bank B's.
* `mtx2209` initialises bank A `0x173 = {1.0, 1.0, 0.5, -0.5}` but bank B
  `0x173 = {-0.5, -0.5, 0.5, 0.5}` (@0x8e455 vs @0x8e4ad).

**For an open driver: keep one shadow and one bitmap.**  The block-load packet
carries no bank selector — the two banks are a host-side double-buffer inside
the vendor's HWREG object, not a hardware resource.  Program the bank-A values
(they are the ones the validators keep self-consistent) and emit them.

---

## 7. What is still open

* The absolute MMIO address of the file.  Unchanged from
  `parhelia-state-slot-map.md` §5.3 and still resolvable only by experiment:
  write one index with a known payload via an opcode-0x41 packet, then dump the
  register window and diff.  Not needed to emit the packet.
* The GL meaning of 0x14d, 0x155, 0x160/0x161, 0x16c..0x171, 0x175 d1..d3 and
  0x17e — the arithmetic is recovered, the GL name is not.
* Which of the four matrix blocks 0x139..0x148 is modelview vs projection vs
  texture-unit k.  The selector is `[entry+0x18] & 7` on a 20-byte-stride array
  at `[HWSTATE+0x1ad8]`, entries `0x1ae4 + j*0x14` (the per-unit dirty words of
  `parhelia-state-slot-map.md` §3.5); resolving it needs a capture, not more
  disassembly.
* `mtx309` @0x89cb0 writes 0x14e and the `0xe0..0xe6` texture-stage group; only
  its index set was read here.

---

Copyright (c) 2026 René Rebe <rene@exactco.de>
