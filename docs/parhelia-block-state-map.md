# Parhelia block-load state file — index map

What lives in each entry of the 0x180 x 16-byte indexed **block state file**, how
the validators compute it, and how to ship it with a DMA block-load packet.

Two independent binaries were read: `matroxdriver-x86_64-cvs-20090511/kernel/
mtx_parhelia.o` (Linux, unstripped, `mtxNNNN` symbols; `.text` offsets below) and
`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` (Windows XDDM D3D8; VMAs
below).  Where they agree the row is marked **2-src**.

Reads foundation-first: assumes `docs/parhelia-state-slot-map.md`, which
establishes the record stream, the software shadow (HWSTATE) and the existence of
the block file.

## The one-line answer

**The block file is a 384-entry vec4 constant store for the WARP vertex engine.**
Fixed-function transform and lighting is lowered onto fixed slots in it: matrix
palette, normal matrices, materials, viewport, clip planes, fog, guard band,
point size, polygon offset, and exp/log coefficient tables.  Per-fragment state
— depth test, stencil, alpha test, blend, colour mask — is *not* in it and has no
index; that state travels in the byte-index register namespace.

Names used throughout:

| name | meaning |
|---|---|
| HWSTATE | `[[driverobj+0x30]+0x78]`, the GL state shadow |
| HWREG | `[[HWSTATE+0x8]+0x50]`, the object holding both block-file banks |
| **BANK** | one bank: `bitmapBase`; entries at `bitmapBase + 0x230 + index*0x10` |
| raster / STATE | `HWSTATE + 0x1bd8`; validators take it as `rsi`, so `raster[X] = HWSTATE[0x1bd8+X]` |
| SURF | `[HWSTATE+0x1e90]` = `HWSTATE+0x0d01` — packed surface + viewport + depth-format descriptor |
| FOG | `[HWSTATE+0x1e80]` = `HWSTATE+0x0d98` — packed fog parameter block |

**The block file's absolute MMIO address is still unknown and is deliberately not
guessed here.**  You do not need it: the DMA engine resolves the index.

---

## 1. Minimum viable state for one untextured Gouraud triangle

Every entry is `{0,0,0,0}` until written — `mtx3107` @0x8fef0 constructs all 0x180
entries through `mtx2321` @0x8f600, which stores four zero dwords.  CONFIRMED.
So anything the WARP program reads must be programmed explicitly.

Four packets, 26 entries, 104 payload dwords.  All values are 32-bit IEEE floats.

### 1.a Viewport — indices 0x160, 0x161 — **the one you cannot omit**

```
dword0 = 0x4104000b   ; 0x41000000 | (2<<17) | (2*4+3)
dword1 = 0x00000002
dword2 = 0x00000160
dword3 = 0x00000001
dword4 = 0x00000000
```

For a viewport of `W x H` at `(X, Y)` on an unscaled render target:

| idx | d0 | d1 | d2 | d3 |
|---|---|---|---|---|
| 0x160 | `+0.5*W` | `-0.5*H` | 0 | — (never written) |
| 0x161 | `X + 0.5*W` | `Y + 0.5*H` | 0 | `(float)H` |

That is the standard NDC→window transform with the Y flip in `0x160.d1`.
2-src CONFIRMED: `mtx2926` @0x87220 (Linux) and the setter at 0x119371/0x1193d3
(DLL) compute the same expressions.  The general form carries two surface scale
factors `fx = SURF[0x39]`, `fy = SURF[0x3d]`:
`0x160 = {0.5*W*fx, -0.5*H*fy}`, `0x161 = {(X1+X0)*fx + 0.5*W*fx, (Y1+Y0)*fy + 0.5*H*fy}`.
`fx = fy = 1.0` for an unscaled target (INFERRED).

Depth range folds into this viewport transform; there is no separate depth-range
entry.

### 1.b Transform — indices 0x000..0x006 and 0x149..0x14c

```
packet 1: dword0=0x410e001f, count=7, start=0x000    ; world0 * view, + normal matrix
packet 2: dword0=0x41080013, count=4, start=0x149    ; projection (or modelview*projection)
```

* `0x000..0x003` = the 4x4 `world[0] * view` matrix, 16 floats, row-major as
  stored.  For an untransformed triangle: identity.
* `0x004..0x006` = the 3x3 inverse-transpose of that matrix (normal matrix),
  written as three vec4s with `w` left untouched.  For identity: `{1,0,0,*}`,
  `{0,1,0,*}`, `{0,0,1,*}`.
* `0x149..0x14c` = the projection matrix (or `modelview*projection` in the fused
  path).  For an untransformed triangle: identity.

CONFIRMED (`mtx1394` @0x85220: `mtx3265` copies 16 dwords to `*[HWREG+0x238+8k]`
= entry `7k`, `mtx3731` copies the 3x3 to `*[HWREG+0x360+8k]` = entry `7k+4`,
then `mtx2702(bitmap, 7k, 7k+7)`; the projection path copies through
`[HWREG+0x358]` = entry 0x149 and marks `0x149..0x14d`).  The identity source
constant is at `.rodata+0x2bc0`.

### 1.c Setup constants — indices 0x172 .. 0x17e (13 entries)

```
dword0 = 0x411a0037   ; 0x41000000 | (13<<17) | (13*4+3)
dword1 = 0x0000000d ; dword2 = 0x00000172 ; dword3 = 1 ; dword4 = 0
```

| idx | d0 | d1 | d2 | d3 |
|---|---|---|---|---|
| 0x172 | `3f800000` 1.0 | `00000000` | `00000000` | `3f800000` 1.0 |
| 0x173 | `3f800000` 1.0 | `3f800000` 1.0 | `3f000000` 0.5 | `bf000000` -0.5 |
| 0x174 | `45ff0000` 8160.0 | `45ff0000` 8160.0 | `c5ff0000` -8160.0 | `c5ff0000` -8160.0 |
| 0x175 | `43000000` 128.0 | `00000000` | `00000000` | `00000000` |
| 0x176 | 0 | 0 | 0 | 0 |
| 0x177 | 0 | 0 | 0 | 0 |
| 0x178 | `00000000` | `00000000` | `3f000000` 0.5 | `49400000` 786432.0 |
| 0x179 | `bf800000` -1.0 | `3e75fdf0` | `bd635847` | `3c1d955b` |
| 0x17a | `3f800000` 1.0 | `baaec3ff` | `39218489` | `b77fe5fe` |
| 0x17b | `3fb8aa3b` log2e | `bf38aa3b` | `3ef6384f` | `beb8aa3b` |
| 0x17c | `3f800000` 1.0 | `3e93bb63` | `be76384f` | `3e530bb1` |
| 0x17d | `4b000000` 2^23 | `3d4ccccd` 0.05 | `41f80000` 31.0 | `3d042108` 1/31 |
| 0x17e | `00000000` | `3d800000` 0.0625 | `00000000` | `00000000` |

This is exactly what the vendor context constructor `mtx2209` @0x8e000 (and its
twin `mtx867` @0x8eab0) writes and marks dirty, so the first flush after context
creation ships it.  CONFIRMED, read instruction by instruction @0x8e434..0x8e685
and @0x8e7c2..0x8ea5e.  The 0x179/0x17a bit patterns are the IEEE encodings of
`pow(ln2,n)/(±n!)`, computed at runtime @0x8e8db..0x8e9d7 (INFERRED to that
extent); the four immediates in 0x17b/0x17c and everything else are literals.

0x174 (`±8160.0`) and 0x17d are **byte-identical in the D3D driver** — the
strongest independent confirmation of the index numbering in this document.

> The existing `parhelia-state-slot-map.md` §6.3 lists four of these rows wrongly
> (it read the register `ebx` as 1.0 where it holds 0.0).  **Corrections:
> 0x172 = `{1,0,0,1}` not `{0,1,1,0}`; 0x175 = `{128,0,0,0}` not `{128,1,1,1}`;
> 0x178 = `{0,0,0.5,786432}` not `{1,1,0,786432}`; 0x17e = `{0,0.0625,0,0}` not
> `{1,0.0625,1,1}`.**  Both binaries agree with the corrected values.

### 1.d Not in this file — do not look for an index

Depth test / func / mask, stencil (all), alpha test enable / func / ref, blend
enable / factors / equation, colour mask, cull face, front face, scissor and the
viewport *rectangle* are **not** block-file state.  See §6.  Program them through
the byte-index namespace (`reg = 0x1000 + index*4`, ring tags `0x2121xxxx` /
`0xc0afafc0`) exactly as the repo already documents.

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
base+0x230   entries: 0x180 x 0x10 bytes
base+0x1a30  end
```

The two banks inside HWREG:

```
HWREG+0x07e8   bank A bitmap      HWREG+0x0a18   bank A entries
HWREG+0x2218   bank B bitmap      HWREG+0x2448   bank B entries
bankB_addr = bankA_addr + 0x1a30
```
`entry(bank, idx) = bitmapBase + 0x230 + idx*0x10`, four dwords, four distinct
values.  Verified on every mirrored store pair in `mtx740`, `mtx2497`, `mtx2926`,
`mtx2522`, `mtx3654` and `mtx2209`.  The DLL's scan loop terminates on
`cmp edi,0x180`, and its entry stride is `shl eax,4` — same geometry.  2-src.

---

## 3. The validators

`mtx3712` @0x188f0 is the validate pass.  It dispatches one validator per dirty
group.  **Only the raster group reaches the block file:**

| dirty group | validator | target | block file? |
|---|---|---|---|
| `[HWSTATE+0xc87]` surface/viewport/depth-stencil | `mtx360` @0x12370 | `[dev+0x40]+0x88` and SURF | no |
| `[HWSTATE+0xc08]` blend/alpha/colour-mask | `mtx2480` @0x129e0 | `[dev+0x40]+0x60/0xb00` | no |
| `[HWSTATE+0xd54]` fog enables | @0x189bc | `[dev+0x40]+0xa8` | no |
| `[HWSTATE+0x1b44]` | `mtx3024` @0xb560 | `[dev+0x3288]` | no |
| `[HWSTATE+0xdbb]` texture/fragment | `mtx297` | `[dev+0x58]` | no |
| **`[HWSTATE+0x1bd8]` raster** | **`mtx3654` @0x8b510** | `rdi = HWREG` | **yes** |

CONFIRMED: of the 81 `mtx2702` (mark-dirty) call sites in the object, none is
inside `mtx360` or `mtx2480`.  The complete caller set is `mtx4027`, `mtx1322`,
`mtx558`, `mtx2441`, `mtx1394`, `mtx740`, `mtx2497`, `mtx2588`, `mtx2926`,
`mtx2522`, `mtx309`, `mtx3654`, `mtx2209`, `mtx867`.

`mtx3654` is called as `mtx3654(rdi=HWREG, rsi=raster, rdx=HWSTATE+0x1e98,
rcx=[dev+0x40]+0x1c0)` and passes the same `rdi`/`rsi` to the leaf validators.

### 3.1 `mtx740` @0x864f0 — polygon offset → **0x178 d0, d1**

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
units *= SURF[0x15]                 ; HWSTATE[0xd16], per-depth-format resolution
if !(raster[0x08] & 1) {            ; HWSTATE[0x1be0] bit0
    factor *= [HWREG+0x6f4]
    units  *= (float)[HWREG+0x6f0] * [HWREG+0x6f4]
}
block[0x178].d0 = factor            ; both banks
block[0x178].d1 = units             ; both banks
```

* `[HWREG+0x6f4]` is a **sign** from a depth-format LUT at `.data+0x8b20` indexed
  by `min(SURF[0x0c], 8)` (`mtx3654` @0x8cc55):
  `{0.0, 0.0, -1.0, 1.0, -1.0, 1.0, 1.0, 1.0, 0.0}`.  Default 1.0 (`mtx3995`
  @0x851ab).  CONFIRMED.
* `[HWREG+0x6f0]` is an integer read once at init from a driver configuration
  object through a vtable call (`mtx2163` @0x84e4d).  Not a GL value; use 1.
  CONFIRMED as a config read, value INFERRED.
* Key 0x05 slots 0x17/0x18/0x19 are the three enable predicates above, not
  registers.  CONFIRMED.

*Disagreement with the D3D driver:* MTXPARD writes a pair of floats into
0x178 d0/d1 from a point-size code path and puts D3D depth bias in 0x17c d2/d3
instead.  The Linux gate above is unambiguous (it tests the polygon-offset enable
bits against the polygon mode), so 0x178 d0/d1 = polygon offset is CONFIRMED for
the GL stack; whether the WARP program reads it as offset or as size depends on
which fixed-function program is bound.  Flagged, not resolved.

### 3.2 `mtx2497` @0x867a0 — point/line size → **0x172 d3**

Called as `mtx2497(rdi=HWREG, rsi=raster, rdx=[dev+0x40]+0x1c0, xmm0=SURF[0x39])`,
i.e. the viewport X scale.

```
v = raster[0x258]                       ; HWSTATE[0x1e30], key 0x0c slot 0x9a
if !(raster[0x09] & 2) {                ; HWSTATE[0x1be1] bit1 = slot 0x9d
    if (scaleSelect == 0) v *= SURF[0x39]
    v = max(block[0x173].d0, min(v, block[0x173].d1))   ; clamp to the size range
}
if ((HWREG[0xe4]&3)==1 && !(raster[0x09]&2) && !(HWREG[0xe0]&0x40)) {
    n = (int)(v + 0.5)
    HWREG[0xe2] bit1 = (n is even)
    v = 0.5 * (float)n                  ; snapped half-width
}
block[0x172].d3 = v
```
CONFIRMED (@0x86900..0x86998).  `block[0x173].d0/d1` are read back out of
**bank A** through the pointer table — the file is its own state, and the
validator compares against it.

### 3.3 `mtx3654` @0x8b510 — the raster validator

All rows CONFIRMED unless tagged.

| block | dwords | source | arithmetic | site |
|---|---|---|---|---|
| **0x14e** | 0,1 | `FOG[0x05]` gate, `FOG[0x06]`, `FOG[0x0a]` | `d0 = 1/(end-start)`, `d1 = -start/(end-start)` — **linear fog**, 2-src | 0x8b6ad |
| **0x154** | 0..3 | `FOG[0x04]` gate, `FOG[0x0f]`=start, `FOG[0x13]`=end, `FOG[0x17]`=density, `FOG[0x1b]`=mode | `d0 = start`; `d1 = end/(end-start)`; `d2 = 1/(end-start)`; `d3 = density*log2e` (mode!=2) or `density²*log2e` (mode==2, EXP2) — 2-src | 0x8ccce, 0x8d394 |
| **0x155** | 3 | `raster[0x254]` = `HWSTATE[0x1e2c]`, key 0x0c slot 0xaa | verbatim | 0x8d230 |
| **0x172** | 0,1,2 | `raster[0x264/0x268/0x26c]` = `HWSTATE[0x1e3c/0x1e40/0x1e44]`, slots 0x9e/0x9f/0xa0 | `v * (1.0 / SURF[0x39])` | 0x8bb29 |
| **0x173** | 0 | `raster[0x25c]` = `HWSTATE[0x1e34]`, slot 0x9b | `v * SURF[0x39]` | 0x8bbd7 |
| **0x173** | 1 | `raster[0x260]` = `HWSTATE[0x1e38]`, slot 0xa6 | `min(v * SURF[0x39], 2048.0)` | 0x8bbdc |
| **0x175** | 1,2,3 | `[HWSTATE+0x1ad8]+0x60/0x64/0x68` | copied verbatim as dwords | 0x8bff6 |
| **0x14d** | 1 | `[HWSTATE+0x0c6b]+0x0c` (int) | `(float)(int)v` | 0x8bf46 |
| **0x178** | 2 | `raster[0x278]` = `HWSTATE[0x1e50]`, key 0x05 slot 0x1b | `v < 2.0 ? 0.5 : 0.5*trunc(v)` | 0x8c715 |
| **0x17e** | 0,2,3 | `WORD [raster+0x27e]` = `HWSTATE[0x1e56]` | `d3 = (float)w * 16.0`; `d2 = 0.5/d3`; `d0 = 0.0` | 0x8b9c2 |
| **0x139+4k** | 4x4 | `[raster + 0x78 + (t+0x22)*8]`, k = 0..3 | 16-dword matrix copy via `mtx3265` | 0x8c360 |
| **0x149** | 4x4 | `[raster+0x80]` = `[HWSTATE+0x1c58]` | 16-dword matrix copy | 0x8cc7f |

`mtx3654` also *reads* 0x134/0x135/0x137 (three-dwords-vs-zero tests that set the
software flags `HWREG+0x6e4/0x6e5`) without writing them.

Predicate bits: `raster[0x09] bit0` (slot 0x9c) is copied to `HWREG+0x6e9` and
gates the 0x172 path; `bit1` (slot 0x9d) bypasses the size scale and clamp.
Neither is a register.  CONFIRMED (@0x8bab9).

### 3.4 `mtx1394` @0x85220 — matrix palette → **0x000..0x0df**, projection → **0x149..0x14c**

Sources: `[STATE+0x78]` = view, `[STATE+0x80]` = projection, `[STATE+0x88+8k]` =
world matrix *k*.  Per matrix, up to 32 of them:

```
M = world[k] * view                       ; mtx1749
*[HWREG+0x238 + 8k] <- M                  ; entries 7k .. 7k+3   (mtx3265, 16 dwords)
N = transpose(inverse(M))
*[HWREG+0x360 + 8k] <- N[3x3]             ; entries 7k+4 .. 7k+6 (mtx3731, 3x3, w untouched)
mtx2702(bitmapA, 7k, 7k+7)
```
CONFIRMED, first index is 0 (`xor esi,esi; mov edx,0x7` @0x856fa).  32 x 7 = 0xe0,
so the palette is exactly `0x000..0x0df` and the texture-stage array begins at
0xe0 — which the DLL confirms independently (`0xe0 + 7k`, k = 0..11, loop at
0x121b81 running to `0xe0 + 12*7 = 0x134`).  2-src.

The projection path copies `[STATE+0x80]` (or `modelview*projection`) through
`[HWREG+0x358]` into 0x149..0x14c and marks `0x149..0x14d`.

`mtx1394` also keeps two non-block 4x4 scratch matrices at `HWREG+0x1b4` and
`HWREG+0x1f4`; the latter is `transpose(inverse(view*projection))` and is what
§3.6 uses.

### 3.5 `mtx2926` @0x87220 — viewport → **0x160, 0x161**

Source `S = SURF`, packed at odd offsets: `S[0x21],S[0x25]` = X0,Y0;
`S[0x29],S[0x2d]` = W,H; `S[0x31],S[0x35]` = X1,Y1; `S[0x39],S[0x3d]` = fx,fy.
(`mtx360` is what writes that rect into SURF.)

```
if (dword[HWREG+0xec] != 0):            ; transform enabled
    0x160.d0 =  0.5*W*fx
    0x160.d1 = -0.5*H*fy
    0x160.d2 =  raw dword [STATE+0x28]
    0x161.d0 =  (X1+X0)*fx + 0.5*W*fx
    0x161.d1 =  (Y1+Y0)*fy + 0.5*H*fy   ; -0.046875 when (HWREG[0xe4]&0x1f)==5
    0x161.d2 =  raw dword [STATE+0x2c]
    0x161.d3 =  (float)H                ; bank A only — see §6
else:                                   ; pre-transformed vertices
    0x160 = {fx, fy}; 0x161 = {X1*fx, Y1*fy}   ; bank A only, skipped if identity
```
CONFIRMED; the DLL setters at 0x119371/0x1193d3 compute the same.  2-src.
`0x160.d3` is never written by anything.

### 3.6 `mtx2522` @0x87790 — user clip planes → **0x16c .. 0x171**

Plane array `P = STATE + 0x1c8`, six planes of 16 bytes.  Enable mask
`E = (HWSTATE[0x1be7] >> 1) & 0x3f`; enabled planes are **compacted** into
consecutive slots.  That mask closes a loose end in
`parhelia-state-slot-map.md` §3.2: **key 0x0c slot 0x98**, which writes
`[0x1be7]` bits 1-6 = `v & 0x3f`, is the `glEnable(GL_CLIP_PLANEi)` bitmask.  Per enabled plane `i` at slot `s`:

```
dst = [HWREG + 0x3d00 + s*8]                     ; bank A entry 0x16c+s
dst = M * P[i]      where M = HWREG+0x1f4 = transpose(inverse(view*proj))
        d0 = p·M[0..3]   d1 = p·M[4..7]   d2 = p·M[8..11]   d3 = p·M[12..15]
copy to HWREG + 0x3b08 + s*0x10                  ; bank B entry 0x16c+s
mark 0x16c+s in both bitmaps
```
CONFIRMED.  The DLL agrees (loop at 0x11fb5c with `mov ebp,0x16c`, six entries,
gated on a 6-bit enable mask).  2-src.

### 3.7 `mtx1322` / `mtx2441` — materials → **0x134..0x137** and **0x156..0x159**

`mtx2441(rdi=ctx, rsi=array, edx=count)` walks records of stride 0x48:
`{u32 type; float m[16]; float Power;}`, and before every compare it executes
`m[11] = Power` — packing the scalar at byte 64 into `w` of the third vec4.  That
is `D3DMATERIAL{Diffuse, Ambient, Specular, Emissive, Power}` with
`Power → Specular.w`:

| entry | field |
|---|---|
| 0x134 / 0x156 | Diffuse |
| 0x135 / 0x157 | **Ambient** |
| 0x136 / 0x158 | Specular (`w` = Power / shininess) |
| 0x137 / 0x159 | Emissive |

`type==1` → 0x134..0x137; `type==2` → 0x156..0x159; `type==3` → both.  Front and
back material for two-sided lighting.  CONFIRMED for the mechanism and the
Power-packing; the GL/D3D field naming is INFERRED but the layout match is exact.
Cached pointers: `[HWSTATE+0x1e68]` = `&bankA[0x134]`,
`[HWSTATE+0x1e70]` = `&bankA[0x156]`, `[HWSTATE+0x1e58]` = `HWREG+0x7e8`
(set by `mtx1440`, called with `rdi = HWSTATE+0x1bd8` @0x1826c).  Bank A only.

### 3.8 `mtx2588` @0x86bf0 — ambient product → **0x138**

```
if      (HWREG[0x6e6] && dword[HWREG+0x114] != 0)  V[i] = STATE[0x244+4i]
else if (HWREG[0x6e6] && HWREG[0x6e4])             V[i] = STATE[0x244+4i] * entry0x135[i]
else                                               V    = {0,0,0,0}
block[0x138] = V                                   ; bank A only
```
`entry0x135` is the material Ambient of §3.7, so 0x138 is
`globalAmbient x materialAmbient` — the ambient lighting term.  CONFIRMED
mechanically; naming INFERRED, but it is corroborated by two independent
decodings meeting at 0x135.

The same function also derives the four colour-material source selectors
(`0=material, 1=COLOR1, 2=COLOR2`) into non-block scalars at
`HWREG+0x104/0x108/0x10c/0x110/0x114/0x118/0x11c/0x120`.

### 3.9 Pointer tables — third independent confirmation of the addressing

`mtx2209` builds three tables inside HWREG; every entry satisfies
`ptr = HWREG + 0xa18 + index*0x10`:

| table | contents | block index |
|---|---|---|
| `[HWREG+0x238 + n*8]`, n=0..0x1f | `HWREG + 0xa18 + n*0x70` | `7n` (matrix) |
| `[HWREG+0x360 + n*8]`, n=0..0x1f | `HWREG + 0xa58 + n*0x70` | `7n + 4` (normal matrix) |
| `[HWREG+0x338 + k*8]`, k=0..3 | `HWREG + 0x1da8 + k*0x40` | `0x139 + 4k` |
| `[HWREG+0x358]` | `HWREG + 0x1ea8` | `0x149` |
| `[HWREG+0x3c48 …]` | see `parhelia-state-slot-map.md` §4.1 | 0x134..0x178 |

---

## 4. Block-index map

CONFIRMED = the index and its writer were read.  INFERRED = the meaning is
deduced from the arithmetic.  2-src = both binaries agree.

| index | holds | layout | writer |
|---|---|---|---|
| **0x000..0x0df** | **matrix palette**: 32 slots of 7 entries — `world[k]*view` at `7k..7k+3`, its 3x3 inverse-transpose at `7k+4..7k+6` | 4x4 + 3x3 | `mtx1394`; CONFIRMED, 2-src on the geometry |
| **0x0e0..0x133** | **12 light slots**, 7 entries each at `0xe0 + 7k` (NOT texture stages -- see note) | `r 7k` | `mtx309` @0x89cb0; CONFIRMED, 2-src (DLL loop 0x121b81) |
| **0x134..0x137** | **front material**: Diffuse, Ambient, Specular(w=Power), Emissive | 4 vec4 | `mtx1322`/`mtx2441`; CONFIRMED, naming INFERRED |
| **0x138** | **ambient product** `globalAmbient * materialAmbient` | vec4 | `mtx2588`; CONFIRMED |
| **0x139..0x148** | four 4x4 matrices at `0x139 + 4k` — per-unit texture transforms | 16 floats each | `mtx3654` @0x8c360; CONFIRMED, role INFERRED |
| **0x149..0x14c** | **projection** (or `modelview*projection`); init = identity | 4x4 | `mtx1394`, `mtx3654`; CONFIRMED, 2-src |
| 0x14d | d1 = `(float)(int)[HWSTATE+0xc77]`; d3 init 32.0 | | `mtx3654` @0x8bf46; CONFIRMED, 2-src on the arithmetic |
| **0x14e** | **linear fog**: `{1/(end-start), -start/(end-start)}` | d0,d1 | `mtx3654` @0x8b6ad; CONFIRMED, 2-src |
| **0x154** | **fog parameters**: `{start, end/(end-start), 1/(end-start), density*log2e}` (`density²*log2e` for EXP2) | vec4 | `mtx3654` @0x8ccce; CONFIRMED, 2-src |
| 0x155 | d3 = key 0x0c slot 0xaa verbatim | | `mtx3654` @0x8d230; CONFIRMED |
| **0x156..0x159** | **back material**, same layout as 0x134 | 4 vec4 | `mtx2441`; CONFIRMED |
| **0x160** | **viewport scale** `{0.5*W*fx, -0.5*H*fy, raw, —}` | | `mtx2926`; CONFIRMED, 2-src |
| **0x161** | **viewport bias** `{(X+0.5W)*fx, (Y+0.5H)*fy, raw, (float)H}` | | `mtx2926`; CONFIRMED, 2-src |
| **0x16c..0x171** | **six user clip planes**, transformed by `inv(view*proj)ᵀ`, compacted | vec4 each | `mtx2522`; CONFIRMED, 2-src |
| **0x172** | point/line size group: d0..d2 = slots 0x9e/0x9f/0xa0 `/ SURF[0x39]`; **d3 = the clamped, optionally half-and-snapped size** | | `mtx3654`, `mtx2497`; CONFIRMED |
| **0x173** | **size clamp range**: d0 = min (slot 0x9b `* SURF[0x39]`), d1 = max (slot 0xa6 `* SURF[0x39]`, capped 2048.0); d2/d3 constants 0.5 / -0.5 | | `mtx3654` @0x8bbd7; CONFIRMED |
| **0x174** | **guard-band clip limits** `{+X, +Y, -X, -Y}` = `{8160, 8160, -8160, -8160}` | | init only; CONFIRMED, **byte-identical in both binaries** |
| 0x175 | d0 = 128.0 constant; d1..d3 copied from `[HWSTATE+0x1ad8]+0x60..0x68` | | `mtx3654` @0x8bff6; CONFIRMED, 2-src |
| 0x176, 0x177 | never written | zero | — |
| **0x178** | **polygon offset** d0 factor, d1 units (both scaled, §3.1); d2 = `0.5*trunc(slot 0x1b)`, min 0.5 (line half-width); d3 constant 786432.0 | | `mtx740`, `mtx3654`; CONFIRMED for the GL stack, see the note in §3.1 |
| 0x179, 0x17a | EXP coefficient table `{c1..c4}` / `{1.0, c5..c7}`, `cn = pow(ln2,n)/(±n!)` | | init only; literals CONFIRMED, role INFERRED |
| 0x17b, 0x17c | LOG coefficient table `{log2e, -log2e/2, log2e/3, -log2e/4}` / `{1.0, log2e/5, -log2e/6, log2e/7}` | | init only; literals CONFIRMED, role INFERRED. The D3D driver reprograms 0x17c d2/d3 from its depth-bias state |
| 0x17d | range constants `{2^23, 0.05, 31.0, 1/31}` | | init only; CONFIRMED, byte-identical in both binaries |
| 0x17e | sub-pixel / sample group: d3 = `(float)WORD[HWSTATE+0x1e56] * 16.0`, d2 = `0.5/d3`, d0 = 0.0, d1 constant 0.0625 | | `mtx3654` @0x8b9c2; CONFIRMED |
| 0x17f | one dword from a state block (DLL setter 0x11d58d); never written on the GL side | | INFERRED |

---


> **Correction (2026-09-05): `0x0e0..0x133` is the light array, not texture
> stages.** `mtx309` (@0x89cb0) walks `count = [HWREG+0x478]` pointers at
> `[HWREG+0x480 + 8n]` and dispatches on `*ptr` in {1,2,3} -- the D3DLIGHTTYPE
> enum, point / spot / directional (`cmp $0x2 -> 0x8a4e0`, `jle -> 0x8a693`,
> `cmp $0x3 -> 0x89ea4`) -- marking `mtx2702(bitmap, 0xe0+7n, 0xe6+7n)` from
> three separate `+= 7` cursors (spot base 0 @0x89e52, point base `7*nspot`
> @0x89e6d, directional base `7*(nspot+npoint)`). Twelve slots x 7 vec4 =
> `0xe0..0x133`. This is also *why* the relocator's stride is 7: the light block
> is 7 registers wide and sits at `r 7k`.
>
> **The 384-entry block file is not a flat constant store.** It is a mixed state
> block, and only its middle window is addressable by a shader:
>
> | range | size | what it is | shader-addressable |
> |---|---|---|---|
> | `0x000..0x0df` | 224 | matrix palette, 32 slots x 7 (`world[k]*view` + normal matrix), `mtx1394` | no |
> | `0x0e0..0x15f` | 128 | **the WARP source register file, `r = N - 0xE0`** | yes |
> | `0x160..0x17f` | 32 | fixed-function setup/clip/raster state: viewport scale+bias, six user clip planes, guard band +/-8160, point/line size and clamp, polygon offset, sub-pixel/sample, EXP/LOG tables | no |
>
> The source address is 7 bits (see `parhelia-vertex-isa.md` §4, resolved), so
> **at most 128 of the 384 entries can be named by any WARP operand.** The tail
> is consumed by units downstream of WARP, which is why the guard band, the
> point-size clamp and the polygon-offset scale live in this file without ever
> being read by a shader instruction.
>
> The `- 0xE0` rule rests on six anchors with a writer on the host side and a
> reader in the microcode (lights `0xe0+7k` <-> `r7k`; front material
> `0x134`/`0x137` <-> `r84`/`r87`; ambient product `0x138` <-> `r88`; texture
> matrices `0x139+4k` <-> `r0x59+4k`; **projection matrix `0x149..0x14c` <->
> `r105..r108`**; fog `0x154`/`0x155` <-> `r116`/`r117`; back material
> `0x156..0x159` <-> `r118..r121`), plus a statistical check: the two dead bands
> in register use (`r111..r115`, `r122..r127`, zero uses across 24,156
> arithmetic instructions) line up exactly with the two never-written block holes
> (`0x14f..0x153`, `0x15a..0x15f`).
>
> **Still open:** extending the same line down to `r0` collides. `mtx309` puts
> light 0 at `0xe0..0xe6` = `r0..r6`, but the unlit transform fragment `0x50`
> (`mtx1621`) reads `r3,r2,r1,r0` and `r7..r10`, and the colour-material
> epilogue reads `r5`/`r6` as vertex colour. Clip planes are worse: `mtx2522`
> writes them to `0x16c..0x171`, which the rule puts at `r140..r145` --
> unreachable with a 7-bit source -- while the fragments that structurally *are*
> the clip-plane test read `r12..r17`. So either there is an undecoded file/bank
> select (candidates: the 2-bit fields adjacent to each source field, `f1a`
> w1[1:0], `f0e` w0[26:25], `f0bc` w0[19:18]), or the low registers are a
> separate attribute/temporary file and the linear rule only holds above them.
> Only the `0x0e0..0x15f` window is established.
>
> **Refinement (2026-09-05): the block file is addressed through relocatable
> record windows, not as a flat array.** `mtx1342` defaults its payload source
> to the block file itself (`lea rcx,[r11+0x230]` @0x9005c), but when a tracking
> record is in play it redirects:
>
>     902da:  mov edx, DWORD PTR [rcx+0x10]   ; the record's base index
>     902ec:  src = [rec+0x8] - 16*[rec+0x10]
>
> So register `n` in a record's own space lands at block index
> `[rec+0x10] + n`. There are six 64-byte tracking records. This dissolves an
> apparent contradiction: the constant-set token (0x43) clamps its index to
> `0..0xff` (`cmp ecx,0x100; cmovae ecx,0xff` @0x4547c), which is checked against
> a *record* (`cmp DWORD PTR [rbx+0x10],ecx` @0x454b8), not against the 384-entry
> file — a `0..0xff` record space and a file window based at `0xE0` are not in
> conflict. Under this reading the `− 0xE0` rule is the identity of one record's
> base rather than a property of the file, which is corroborated by `mtx2209`
> @0x8e850 laying out 32 units x 7 entries from index 0 and ending at exactly
> `0xE0`.
>
> The `−0xE0` rule is therefore **not refuted**, but "flat window" is the wrong
> mental model, and the fixed slots `mtx309` marks at `0xE0+7k` and the
> hard-coded `mtx2702` slots up to `0x17e` are best read as what a
> driver-generated fixed-function program uses when no user record owns that
> range. Mapping the six records at `+0xb0` is its own pass and has not been
> done.
>
> Unestablished: at `0x90142` the per-record mask is indexed
> `[record + word*4 + 0x1c]` with `word` running to 11, which for `word >= 9`
> reads past the 64-byte record stride — either an upstream over-read or a
> mis-assigned field.


## 5. Dirty-bitmap protocol

### 5.1 Marking — `mtx2702` @0x8f560 (DLL equivalent at 0xf99c0)

`mtx2702(rdi = bitmapBase, esi = firstIndex, edx = lastIndex)`; `lastIndex == 0`
means "same as first".  It sets bits `first..last` **inclusive**:

```
word(i)     = *(u32*)(bitmapBase + 4 + (i>>5)*4)
bit(i)      = 1 << (i & 31)
summary bit = 1 << (i >> 5)     ; OR'd into *(u32*)bitmapBase
```
2-src CONFIRMED.  The summary word is the flush trigger: `mtx693` @0x437c9 calls
the emitter only when `*(u32*)bitmapBase != 0`.

Over-marking is normal and harmless: a 7-entry matrix record marks `[7k, 7k+7]`
(8 bits) and a 4-entry matrix marks `[0x149, 0x14d]` (5 bits), because the range
is inclusive.  One neighbouring, already-valid entry is re-uploaded.

### 5.2 Emission — `mtx1342` @0x90010 (DLL equivalent at 0xf9c40)

Scan indices 0..0x17f, coalesce each maximal run of consecutive dirty indices,
split every run into packets of at most **0x3f (63) entries** — not 0x40.  Linux:
`cmp ebx,0x40 ; cmovae ebx,0x3f` @0x901f0.  DLL: `cmp edi,0x3f ; jb ; mov ebp,0x3f`
@0xf9e39.  The cap exists because the count field is 6 bits wide.  Per packet:

```
dword 0 : 0x41000000 | ((count << 17) & 0x007e0000) | ((count*4 + 3) & 0xffff)
dword 1 : count                 ; entries
dword 2 : start index           ; 0..0x17f
dword 3 : 1
dword 4 : 0
payload : count * 16 bytes, copied from bitmapBase + 0x230 + start*0x10
ring    : advanced by (count*4 + 5) dwords
```
2-src CONFIRMED, field for field.  The DLL builds the tag as
`((0x2080 | (count & 0x3f)) << 17) | ((count*4+3) & 0xffff)` — and
`0x2080 << 17 == 0x41000000` exactly, which is why no literal `0x41000000` header
appears anywhere in that binary.  A second DLL encoder at 0x13b103 derives both
fields from the total dword count and agrees algebraically.

Ring space is checked as `(runLength*4 + 0x23)` dwords before the first packet of
a run.

**Entries need not be contiguous.**  Non-adjacent dirty indices simply produce
separate packets; a 200-index run produces four packets of 63, 63, 63, 11.
Nothing forbids one packet per entry (`count = 1`, `dword0 = 0x41020007`) — the
vendor coalesces only to save ring space.

`mtx1342` does **not** clear the dirty bits; the caller does.

Useful headers:

| count | dword0 | payload dwords |
|---|---|---|
| 1 | `0x41020007` | 4 |
| 2 | `0x4104000b` | 8 |
| 4 | `0x41080013` | 16 |
| 7 | `0x410e001f` | 28 |
| 13 | `0x411a0037` | 52 |
| 63 | `0x417e00ff` | 252 |

### 5.3 Client-supplied payload, and the direct-constant path

Before scanning, `mtx1342` ORs the six dwords at `bitmapBase + 0xc8 + n*0x40`
(n = 0..5) into a word-presence mask.  If a dirty index falls in a word that mask
claims, it walks the slot ring at `bitmapBase + 0xb0 + n*0x40` — entry layout
`{u8 hasExtra @0, void* data @8, u32 startIndex @0x10, u32 extra @0x14,
u32 wordMask @0x18, u32 word[] @0x1c}` — and on a hit **replaces the payload
source with the client's buffer** (`src = data - startIndex*0x10`, @0x902da).

Separately, `mtx4027` @0x144a0 and `mtx558` @0x44ef0 share an identical inner
loop that writes the file at a **runtime index**:

```
base = [HWSTATE+0x1e60]
repeat count times:
    node = <20-byte payload>
    idx  = dword [node+0x10]                          ; entirely client-supplied
    memcpy(base + 0x230 + (idx<<4), node, 0x10)
    mtx2702(base, idx, 0)
```
The record is `{u128 value; u32 blockIndex;}` and there is **no range check** in
either caller — any of the 384 entries can be written directly.  CONFIRMED.  This
is the fixed-function-bypass constant upload (the `SetVertexShaderConstant`
equivalent), and it is the simplest path for an open driver that wants to drive
its own WARP program: build the vec4s, emit the block-load packet, done.

---

## 6. Which bank, and do you need two?

`mtx3995` @0x84fb0 selects the bank to flush and stores it into
`[[HWREG+0x798]+0x40 + 0x260]`:

```
if (HWREG[0x7a0] != 0)  flush bank A (HWREG+0x7e8)
else                    flush bank B (HWREG+0x2218)
```
`mtx2209` leaves `HWREG[0x7a0] = 0`, so **bank B is the default flushed copy**.
`HWREG[0x7a0]` is set by `mtx3654` @0x8bece and cleared @0x8cde2 after a merge
that folds bank A's secondary bitmaps into bank B's.  CONFIRMED.

Validators keep **bank A** as their comparison shadow — every pointer in the
`[HWREG+0x3c48 …]`, `[HWREG+0x238 …]` and `[HWREG+0x3d00 …]` tables points into
bank A, and every "has it changed?" read comes from there — and write both
copies.  The mirroring is not exact.  Four confirmed asymmetries:

* `mtx2497` @0x86970/0x86977 writes the **snapped** half-size to bank A's
  `0x172 d3` and the **un-snapped** clamped value to bank B's.
* `mtx2209` initialises bank A `0x173 = {1.0, 1.0, 0.5, -0.5}` but bank B
  `0x173 = {-0.5, -0.5, 1.0, 1.0}` (@0x8e455 vs @0x8e4ad).  The D3D driver keeps
  two whole shadow copies too (`ctx+0x8b4` and `ctx+0x2298`, delta 0x19e4) and
  shows **the same 0x173 discrepancy between them**.  2-src, unexplained.
* `mtx2926` writes `0x161 d3` (the viewport height) to bank A only, yet marks
  bank B's 0x161 dirty.
* `mtx1394`, `mtx1322`/`mtx2441` and `mtx2588` write bank A only.

**For an open driver: keep one shadow and one bitmap.**  The block-load packet
carries no bank selector — the two banks are a host-side double-buffer inside the
vendor's HWREG object, not a hardware resource.  Program the bank-A values (they
are the ones the validators keep self-consistent) and emit them.

---

## 7. What is still open

* The absolute MMIO address of the file.  Unchanged from
  `parhelia-state-slot-map.md` §5.3 and still resolvable only by experiment:
  write one index with a known payload via an opcode-0x41 packet, then dump the
  register window and diff.  Not needed to emit the packet.
* Whether 0x178 d0/d1 is read by the WARP program as polygon offset (Linux GL) or
  as a point-size pair (D3D).  Both stacks write two floats there from different
  API state; the bound fixed-function program decides.  This is the one place the
  two binaries disagree outright.
* The meaning of 0x14d, 0x155, 0x175 d1..d3, 0x17e, 0x17f, and which of
  0x139/0x13d/0x141/0x145 is which texture unit.  The selector is
  `[entry+0x18] & 7` over a 20-byte-stride array at `[HWSTATE+0x1ad8]` (entries
  `0x1ae4 + j*0x14`, the per-unit dirty words of `parhelia-state-slot-map.md`
  §3.5); resolving it needs a capture, not more disassembly.
* Which physical bank `[HWSTATE+0x1e60]` names in the direct-constant path — its
  store is in a different object file.  INFERRED to be the run-time-selected
  bank.
* `mtx309` @0x89cb0 writes 0x14e and the `0xe0..0xe6` light-slot group; only
  its index set was read here.
* The byte-index → MMIO `0x1000 + index*4` mapping is *not* re-derived here; it
  still rests on the earlier Linux-side work.  The D3D driver confirms the packet
  encoding of that namespace (`dword0 = dwords_following - 1`, then interleaved
  tag dwords packing four byte-indices with `0x21` as the pad, followed by one
  data dword per non-pad index) but not the address arithmetic.

---

Copyright (c) 2026 René Rebe <rene@exactco.de>
