# Matrox Parhelia — render target, depth/stencil and framebuffer state

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** The `reg = 0x1000 + slot*4` rule is refuted. The render target of a live GL client was measured at `0x2f654f8`, not at the `0x1220` value.
Reverse-engineered for a from-scratch open driver, 2026-09.

**Primary source for this document: the vendor kernel core
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`** (obfuscated symbols
`mtxNNNN`, but with full relocations; disassembly cached in
`scratchpad/mtxk_r.asm`, intel syntax).  That object is where the render-target
registers are actually programmed: the vendor OpenGL ICD
(`opengl6/lib64/libGL.so.1.3.0`) never touches a register — it emits a
*high-level record stream* that the kernel translates.  Secondary sources:
`xserver/7_4_0/mtx_drv.so` (2D), the win32 `MTXPARD.dll` (`scratchpad/pard.asm`),
and the live captures in `captures/`.

Tags: **[CONFIRMED]** = read directly out of the disassembly, or two independent
binaries agree; **[INFERRED]** = best reading, self-consistent, not proven;
**[OPEN]** = not resolved, do not build on it.

---

## Answers at a glance

| question | answer | §  |
|---|---|---|
| colour RT registers | **`0x1220` base / `0x1224` cfg** (surface slot 0) — the 3D engine **reuses the 2D descriptor file**, it has none of its own | 1 |
| pitch / format / tiling | all inside cfg: pitch px [13:0] (mult. of 32), type [18:16], **format [29:24] — 6 bits**, tiling [31:30] | 1.2 |
| depth/stencil | **`0x1230` / `0x1234`** (slot 2), one surface for both, no separate stencil base; exact S8Z24 code still open | 2.1 |
| depth clear | a **3D rectangle**: `DWGCTL = 0xC4`, PLNWT forced `0xFFFFFFFF`, values in `0x12C8`/`0x12CC`/`0x12D0`, geometry through the WARP vertex planes — *not* a 2D fill | 2.3 |
| clear mask | 1 = colour, 2 = depth, **4 = stencil**, 8 = rect already in surface space | 2.2 |
| colour write mask | **`0x1324` PLNWT**, a 32-bit bit-plane mask (`0x12a0` is unproven) | 3.1 |
| multisample | **advertised, not implemented**; `GL_SAMPLE_BUFFERS` hard-wired to 0; FAA-16x is Windows-only | 4 |
| render to texture | just point `0x1220`/`0x1224` at the off-screen surface — nothing else is scanout-specific; no MRT | 5 |
| present | flush → `0x12BC`/`0x41C` fence → 2D blit (windowed) or CRTC page flip: `0x1824 + slot*4` then `0x1800[8:7]` | 6 |

---

## 0. The framing correction that everything else depends on

Two structural facts had to be fixed before any `0x12xx` claim could be trusted.
Both correct existing documents.

### 0.1 The ICD's stream "index" is NOT `0x1000 + index*4`  [CONFIRMED]

`docs/parhelia-gl-driver.md` and `docs/parhelia-gl-feature-map.md` §2 read the
136 KB region-2 stream as `(register-index, value)` pairs with
`reg = 0x1000 + index*4`, and `docs/parhelia-gl-feature-map.md` §5 then flagged
the contradictions (`glFogi` → "`0x1230` = 3", `glEnable(GL_LIGHTING)` →
"`0x1224` = 1") as unexplained.  They are explained now.

The kernel command-list interpreter is `mtx558` @0x44ef0.  Each record is

```
[u32 opcode][u32 count][payload: count * elemsize]
```

with `opcode` 0x00–0x51 dispatched through an 82-entry jump table at
`.rodata+0x1b08`.  The scalar-state opcode is **0x0c**, handled by
`mtx2617` @0x19950, whose payload elements are `(index, value)` 8-byte pairs:

```c
for each (idx, val):
    if (idx <= 0x100) {
        if (override_bitmap[idx-1]) continue;         /* ctx+0x414 */
        if (ctx->state[idx] == val) continue;         /* ctx+0x10 + idx*4 */
        ctx->state[idx] = val;
        if (idx > 0xab) continue;
        jumptable_at_rodata_0xba0[idx](ctx, val);     /* 172 entries */
    } else if (idx - 0x100 <= 0x100) {
        set/clear bit (idx-0x101) in ctx+0x414;       /* per-state override */
    }
```

So the index space is a **GL state-slot id 1..0x100**, and each slot has a
hand-written handler that pokes a *packed software shadow*, e.g.

| state slot | handler | what it writes |
|---|---|---|
| 0x07 (depth-test enable) | @0x19bbe | shadow `+0xc9b` (dword), dirty bit 0x04 at `+0xc87` |
| 0x0e (depth mask) | @0x19c8d | shadow `+0xc9f` (byte), same dirty bit |
| 0x17 (depth func) | @0x19dc0 | shadow `+0xca0` (dword), same dirty bit |
| **0x89** (lighting) | @0x1a78d | **bit 3 of shadow byte `+0x1be0`**, dirty bit 0x10 at `+0x1bed` |
| **0x8c** (fog mode) | @0x1a842 | **shadow `+0xd74` (dword)**, dirty bit 0x04 at `+0xd54` |

**No register offset appears anywhere in that path.**  The apparent
`0x1000 + idx*4` match for the low indices is a coincidence of ordering, not a
hardware mapping.

> **CORRECTION.** `docs/parhelia-gl-driver.md` ("Register model
> (triple-confirmed): engine register at MMIO `0x1000 + (index<<2)`, driven by …
> libGL emits state as `(key, index, value)` packets") and
> `docs/parhelia-gl-feature-map.md` §3 (the whole per-feature register table)
> apply that mapping to the ICD stream.  It is wrong **for the ICD stream**.
> The `0x1000 + index*4` rule is real, but only inside the *DMA command buffer*
> that the kernel builds (§0.2) — a different namespace.
>
> Consequence: **every register offset in `parhelia-gl-feature-map.md` §3 and in
> the "Rasterizer state" table of `parhelia-gl-driver.md` is unproven**, not just
> the ones with index ≥ 0x80.  `glFogi → 0x1230 = 3` and
> `glEnable(GL_LIGHTING) → 0x1224 = 1` are decoder artefacts; they are state
> slots 0x8c and 0x89, and neither touches `0x1230`/`0x1224`.
> The *values* in that table are still good data (depth func 1..8, blend factor
> enum, alpha ref 8-bit, …) — only the register column is void.

Corollary the other way: the surface registers `0x1220`–`0x123c` **are** real,
because they are emitted by the kernel into DMA packets by literal register
index (§1) — that evidence is completely independent of the ICD stream.

Other record opcodes recovered from the same jump table, useful for reading the
capture: 0x05 = scalar state, index ≤ 0x1c (`mtx1734`); **0x06 / 0x0e = 2-D
indexed state** `(u16 class ≤ 10, u16 sub, u32 val)` (`mtx3081`); 0x07 =
pipeline/shader key, 6-dword payload (`mtx651`); 0x08 = vertex format, 8 dwords
(`mtx2530`); 0x12 = clip rect, 4 dwords; **0x15 = CLEAR, 9 dwords (`mtx3513`)**;
0x1b = DRAW, 10 dwords (`mtx2196`); 0x1c = a second draw form, 10 dwords
(`mtx1385`); 0x26 = viewport; 0x27 = depth range, 2 floats; 0x46 = fragment
program upload, 0x678-byte elements (`mtx3577`).  All [CONFIRMED] from the
handler advance arithmetic.

Worked example — the `fog` op of `captures/glsweep-0528-raw.txt`, whose whole
104-byte stream now parses exactly:

```
op 0x0c count 2 : (0x1c, 1) fog enable, (0x8c, 3) fog mode = LINEAR
op 0x27 count 1 : 0.5f, 0.5f            depth range
op 0x0c count 1 : (0x08, 3)             polygon mode FILL (lazy cleanup)
op 0x1b count 1 : 10 dwords             DRAW
                                        = 2+4 + 2+2 + 2+2 + 2+10 = 26 dwords = 0x68 ✓
```

(The two clobbered leading dwords are the *first* record's opcode **and** its
count — `parhelia-gl-feature-map.md` §2 assumes the count survives as 1.  It does
not; here it was 2.)

### 0.2 The kernel's DMA blocks are length-prefixed  [CONFIRMED, 6 witnesses]

Inside the secondary command buffer the kernel emits

```
[u32 n-1][u32 header][data…]  … repeated, n = total payload dwords
```

where each `header` carries four register indices consumed LSB-first and
`reg = 0x1000 + index*4` (index 0x20 = DMAPAD, 0x21 = end-of-packet), exactly as
`docs/parhelia-kernel-abi.md` §6.2 describes for the *primary* ring.  The extra
leading dword is a payload length **minus one**.

Witnesses: `mtx1619` @0x412c0 (5 blocks, n-1 = 4/2/2/1/3 against 5/3/3/2/4
payload dwords), `mtx2954` @0xf8e0 (4 and 2), `mtx1192` @0x3dcb0 (9 and 0x1d
against 10 and 30 payload dwords — note these span *two* and *six* headers, which
is what proves the field counts payload dwords and not data dwords), `mtx2187`
@0x3ccb0 (4, 7, 0x22), and the live region-5 descriptor in
`captures/glsweep-0528-raw.txt` (`[3]=2`, `[4]=0x21212425`, two data dwords).

> **CORRECTION.** `docs/parhelia-gl-feature-map.md` §6 records region-5
> `[3] = 2` as "constant"; it is the length prefix of the `0x21212425` packet
> that follows.

---

## 1. The colour render target

### 1.1 The 3D engine reuses the 2D surface descriptors  [CONFIRMED]

There is **one** surface-descriptor file with **four slots**, shared by the 2D
engine, the DPS/stretch engine and the 3D/WARP pipeline:

| slot | base | cfg | DMA header seen |
|---|---|---|---|
| 0 | `0x1220` (idx 0x88) | `0x1224` (idx 0x89) | `0x8d8c8988`, `0x8988bfb9` |
| 1 | `0x1228` (idx 0x8a) | `0x122c` (idx 0x8b) | `0x21218b8a`, `0x21c58b8a` |
| 2 | `0x1230` (idx 0x8c) | `0x1234` (idx 0x8d) | `0x8d8c8988` |
| 3 | `0x1238` (idx 0x8e) | `0x123c` (idx 0x8f) | `0x21218f8e` |

The kernel keeps them in a software shadow inside the device object; `mtx1619`
@0x412c0 is the dirty-bit flush (the kernel twin of `mtx_drv.so`'s
`HwThread2d::SetupHardware`):

| shadow off | register | dirty bit in `*[shadow+0]` |
|---|---|---|
| `+0x08` | `0x1300` DWGCTL | 0x01 |
| `+0x0c` | `0x1324` PLNWT | 0x02 |
| `+0x14` / `+0x18` | `0x1318` BCOL / `0x1314` FCOL | 0x20 |
| `+0x1c`/`+0x20`/`+0x24` | `0x1308` DSTXY / `0x1304` SRC / `0x138c` LEN | 0x40 |
| `+0x28 + 4*i` | surface **base**[i] | 0x04 (slots 0+2), 0x08 (slot 1), 0x10 (slot 3) |
| `+0x38 + 4*i` | surface **cfg**[i] | same |
| `+0x58` | `0x12b0` | 0x80 (soft-trap block) |

In the device object this shadow sits at `+0xb00`, i.e. **base[i] = `dev+0xb28+4i`,
cfg[i] = `dev+0xb38+4i`**, and `mtx2372` @0xcc30 copies all four triples into a
per-operation record:

```c
rec[0x144 + 4*i] = dev[0xb28 + 4*i];   /* base */
rec[0x154 + 4*i] = dev[0xb38 + 4*i];   /* cfg  */
rec[0x164 + 4*i] = dev[0xb48 + 4*i];   /* dims */   /* i = 0..3 */
```

so a surface descriptor is a **triple {base, cfg, dims}**.  Only base and cfg are
emitted to registers by the paths found; `dims` (14-bit width and height fields,
masked `& 0x3fff` in `mtx641` @0xc6d0) is used for bounds/size maths.
[CONFIRMED for the layout, [INFERRED] that `dims` has no register.]

### 1.2 Descriptor encoding  [CONFIRMED]

**base** (`0x1220 + 8*slot`) = plain **VRAM byte offset**.  Bits[1:0] are a
memory-space selector (`0x3` = non-local/AGP), per `mtx_drv.so`
`Surface2d::Enable`.  [Re-confirmed from four witnesses in
`parhelia-surface-address-translation.md` §1.]

> **CORRECTION.** The `pitch` field below is the surface **width**, not a row
> stride: the 3D render target is **tiled**, 64x32 pixels per 8192-byte tile,
> tiles row-major.  See `parhelia-surface-address-translation.md` §5, which
> derives it exactly from a measured 16128-pixel footprint.  §3 there also shows
> the 3D colour/depth binding (slots 0 and 2) is emitted **by the kernel into the
> primary stream**, which is why it never appears in a client capture.

**cfg** (`0x1224 + 8*slot`):

```
bits [13:0]   pitch in PIXELS, multiple of 32
bits [18:16]  surface type (1 or 2 or 3 seen; bit16 = writable/render target)
bit  23       byte/word swap
bits [29:24]  pixel format code          <-- 6 bits, mask 0x3f
bits [31:30]  tiling (0 = linear)
```

The 6-bit width of the format field is now nailed: `mtx969` @0x5b2e0 is the
kernel's "bits-per-pixel of this descriptor" helper and starts with
`edx = desc->cfg & 0x3f000000`.  Its complete table [CONFIRMED]:

| bpp | format codes |
|---|---|
| 1 | 0x00, 0x02, 0x04, 0x05 |
| 4 | 0x08 |
| 8 | 0x09, 0x0a, 0x0b, 0x0c, 0x10, 0x11, 0x12 |
| 16 | 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2a, 0x2b, 0x2c, 0x2d, **0x2e** |
| 32 | 0x30, 0x32, 0x33, 0x34, 0x36, 0x38, 0x3a, 0x3b |

The codes the vendor actually selects for a *colour* surface, from `mtx3388`
@0x4fff0 (a surface-format builder driven by the source's bit depth):

| source bpp | format code |
|---|---|
| 8 | **0x10** |
| 16 | **0x20** |
| 30 (10:10:10 "GigaColor") | **0x34** |
| otherwise (24/32) | **0x30** |

and `mtx1192` @0x3dcb0 force-rewrites the colour target's format to **0x32**
(`cfg = (cfg & 0xc0ffffff) | 0x32000000`) for its 32-bit path — i.e. 0x30 and
0x32 are both 32-bit colour codes, differing in a component detail [OPEN which].

> **CORRECTION.** `docs/parhelia-gl-driver.md` and
> `docs/parhelia-fragment-isa.md` list DXT/S3TC as code **0x2e** with 4 bpp, with
> a "suspect" note citing MTXPARD.  The Linux kernel core independently
> classifies 0x2e as **16 bpp**.  Two binaries now agree: 0x2e is not a 4-bpp
> compressed code.
>
> `docs/parhelia-2d-accel-complete.md` §3 gives the format field as bits
> [29:24] — correct — but only lists 8/16/32 = 0x11/0x28/0x32.  The field is
> 6 bits wide and the full enum above supersedes that short list.

### 1.3 Which slot is the colour target for 3D  [CONFIRMED]

Slot roles are **not** fixed in hardware: DWGCTL `0x1300` selects them per
operation, bits [11:10] = destination slot, bits [9:8] = source slot
(`docs/parhelia-2d-accel-complete.md` §4, CORRECTION 2 — independently
re-confirmed here by `mtx1637` @0x23c20, a blit with `DWGCTL |= 0x202`, i.e.
src = slot 2, dst = slot 0, binding base/cfg of exactly those two slots).

For the **3D/GL pipeline** the vendor uses:

* **slot 0 = colour render target** (`0x1220` / `0x1224`)
* **slot 2 = depth/stencil** (`0x1230` / `0x1234`)
* slot 1 = the other colour buffer of the drawable (front when 0 is back)

Evidence:
1. The GL bind emits slot 0 and slot 2 **in one DMA packet** — header
   `0x8d8c8988` = indices 0x88, 0x89, 0x8c, 0x8d — and slot 1 separately, in
   `mtx2954` @0xf8e0, `mtx1619` @0x412c0 and two more sites.  A colour+depth pair
   is the only reading under which those two always change together.
2. The GL clear (§2) draws with `DWGCTL = 0xC4`: dst slot 0, src slot 0.
3. The win32 driver agrees independently: MTXPARD's WARP draw setup binds via
   `0x170f50(slot_base = 0x1220, depth_base = 0x1230, count = 2)` (already
   recorded in `docs/parhelia-gl-driver.md`).

> **CORRECTION.** `docs/parhelia-gl-driver.md` "Framebuffer / render target /
> depth" assigns slot 0 = "source / texture", slot 1 = "**colour render
> target**", slot 3 = scratch.  For the 3D pipeline the colour target is
> **slot 0**, not slot 1.  The four-slot table itself, the base/cfg pairing and
> "slot 2 = depth" are correct.  (Slot 1 as a colour target is right for the
> *DPS* 2D path — `CDPSSurface::Fill` uses `DWGCTL 0x5C0` = dst slot 1 — which is
> where that entry came from.)

The colour target's *origin and extent* are not in the descriptor: rendering is
positioned by DSTXY `0x1308` / LEN `0x138c` for rectangle ops and by the scissor
`0x1328`/`0x132c` plus the vertex coordinates for WARP ops.

### 1.4 Multi-rectangle clipping is mandatory  [CONFIRMED]

Both the CLEAR handler (`mtx3513` @0x4a0b0) and the DRAW handler (`mtx2196`
@0x4acb0) run the same loop: `mtx3516` initialises a clip-rectangle iterator,
`mtx710` advances it, and `mtx2254` @0x4da00 writes each rectangle into the
state shadow (`+0xce9`/`+0xced`/`+0xcf1`/`+0xcf5` = x, y, w, h, dirty bit 0x10 at
`+0xc87`) plus the drawable origin (`+0xcd1`/`+0xcd5`, dirty bit 0x80), then the
primitive is re-emitted **once per rectangle**.

So the render target is the *screen* surface, windows are rendered in place at a
translated origin, and X's clip list is applied as a hardware scissor per pass.
An open driver that renders to a private off-screen surface simply runs the loop
once with a full-surface rectangle.

---

## 2. Depth / stencil

### 2.1 Surface  [CONFIRMED / partly OPEN]

Depth lives in **surface slot 2**, `0x1230` = base (VRAM byte offset),
`0x1234` = cfg with the same {pitch, type, format, tiling} layout as colour
(§1.2).  There is **no separate stencil base** anywhere in the kernel core:
one surface, one descriptor.  [CONFIRMED — the depth clear (§2.3) takes a single
surface object and clears depth and stencil from it in one operation, gated by
two independent mask bits.]

The depth surface object carries its own geometry: `zsurf+0x1d4` = bytes per
pixel, `zsurf+0x1dc` = width, `zsurf+0x1e0` = height (`mtx237` @0x10b80).

**The exact format code for S8Z24 is still [OPEN].** `mtx3388`'s table (§1.2) is
the *colour* selector; no depth-specific format builder was found in the kernel
core, which means the ICD computes the depth descriptor and passes it down.
What is known is the constraint: the code must be one of the 32-bit-class codes
0x30/0x32/0x33/0x34/0x36/0x38/0x3a/0x3b for 24/8, or a 16-bit-class code
(0x2x) for Z16.  Pinning it needs one MMIO capture of `0x1234` on a 24/8 visual —
the same capture item `docs/parhelia-gl-driver.md` already lists.

> Note for the reader of `docs/parhelia-gl-driver.md`: its statement
> "S8Z24 = 24-bit Z (mask 0xffffff, glClearDepth 1.0 → 0xFFFFFF) + 8-bit stencil
> in the *same* buffer (no separate stencil base)" is **confirmed** by this pass.
> Its follow-on recipe — "Depth clear: point a slot at the Z surface, DWGCTL
> `0x1300 = 0xcc0` (fill), rect in `0x138c`" — is **wrong**; see §2.3.

### 2.2 The clear record and its mask  [CONFIRMED]

ICD record opcode **0x15**, payload elements of **9 dwords**
(`mtx558` @0x45d83 advances `8 + count*36`; `mtx3513` steps `rbp += 0x24`):

```
[0] mask      [1] ARGB8888      [2] depth (float)  [3] stencil
[4] 0x80808080
[5] x0        [6] y0            [7] x1             [8] y1
```

**Mask bits** — recovered from `mtx3513` @0x4a0b0, `mtx3354` @0x11720,
`mtx1602` @0x11210 and `mtx237` @0x10b80:

| bit | meaning |
|---|---|
| 0x01 | clear **colour** |
| 0x02 | clear **depth** |
| 0x04 | clear **stencil** |
| 0x08 | rectangle is already in surface space (skip window translate and scale) |
| 0x10 | extra pass via `mtx415` @0x497b0 [OPEN] |

`mtx1602` isolates `mask & 6` and hands it to the depth path; `mtx237` tests
`== 6` for "depth **and** stencil together".  This **reconciles**
`docs/parhelia-gl-feature-map.md`'s observation of masks 1, 2 and 3 (colour,
depth, both): the encoding is a bitmask, and the sweep simply never exercised
`glClear(GL_STENCIL_BUFFER_BIT)`, which is bit 2.

> **CORRECTION.** `docs/parhelia-gl-feature-map.md` reads dwords [5..8] as
> `x, y, w, h`.  They are **two corners** `(x0,y0)` and `(x1,y1)`: `mtx3513`
> adds the drawable's screen origin to `[5],[6]` *and* to `[7],[8]`
> (four calls to `mtx2396`, then the same four subtractions on the way out).  An
> origin is never added to a width.  For a full-window clear at origin the two
> readings coincide, which is why the sweep could not tell them apart.
> `mtx1602`/`mtx237` then clamp the corners against the drawable extent
> (`drawable+0x260…+0x26c`) and the surface size, which only makes sense for
> corners.

### 2.3 How the depth buffer is actually cleared — a 3D rectangle, not a 2D fill  [CONFIRMED]

`mtx237` @0x10b80 (depth/stencil clear):

```c
zsurf = ctx->[8]->[0x248];                 /* the depth surface object */
clamp rect to drawable extent and to (zsurf->[0x1dc], zsurf->[0x1e0]);
if (full-surface rect && (mask&6) == 6 …)
    if (w * h * zsurf->[0x1d4] <= 0x300000)          /* 3 MB */
        pDev->[0x2188] = zsurf;                      /* arm fast clear   */
        mtx2954(ctx->[8]);                           /* bind + rect      */
mtx2060(ctx, mask, zsurf, …, depth_float = record[2]);
```

`mtx2954` @0xf8e0 binds slot 0 + slot 2 (`0x8d8c8988`) and slot 1
(`0x21218b8a`) from the record's base/cfg arrays, then calls the clear emitter
`mtx2187` @0x3ccb0, which emits exactly three DMA blocks:

```
block 1  n-1 = 4   header 0xb4b320c9  ->  0x1324 PLNWT = 0xFFFFFFFF
                                          0x1080 DMAPAD = 0
                                          0x12CC = rec[0x13c]
                                          0x12D0 = rec[0x140]
block 2  n-1 = 7   header 0xb2cacbc0  ->  0x1300 DWGCTL = 0x000000C4
                                          0x132C SCISSOR_MAX = 0x0FFF0FFF
                                          0x1328 SCISSOR_MIN = 0
                                          0x12C8 = rec[0x138]
                   header 0x2121bf20  ->  0x1080 DMAPAD = 0
                                          0x12FC = rec[0x134]
block 3  n-1 = 0x22  headers 0x605e5d5b, 0x59605e5b, …
                                      ->  the WARP vertex-plane registers
                                          0x1164/0x116c/0x1174/0x1178/0x1180…
```

So the clear is **a hardware rectangle primitive pushed through the WARP vertex
path**, with:

* `DWGCTL = 0xC4` — **opcode 4**, BOP 0xC (GXcopy), src slot 0, dst slot 0.
  Opcode 4 is a *new* DWGCTL opcode not in `docs/parhelia-2d-accel-complete.md`
  §4 (which lists 0 fill, 1 line, 2 blit, 3 colour-expand, 0xC textured rect);
  it is the 3D clear/fast-clear primitive. [CONFIRMED value, [INFERRED] name]
* `PLNWT 0x1324 = 0xFFFFFFFF` forced, so the clear ignores the current write
  masks;
* the scissor opened to the full 4096×4096 guard rectangle;
* four clear-value registers **`0x12C8`, `0x12CC`, `0x12D0`, `0x12FC`**, sourced
  from the per-operation record at `+0x138`, `+0x13c`, `+0x140`, `+0x134`.
  `mtx2372` @0xcc30 fills the first three from the device engine shadow
  (`dev->[0x40]->{0x98, 0x9c, 0xa0}`), and `mtx2215` fills `+0x134`
  (→ `0x12FC`) from a small mode bitfield, so **`0x12FC` is a mode word, not a
  clear colour**.  Which of `0x12C8`/`0x12CC`/`0x12D0` is depth, which is
  stencil and which is colour is **[OPEN]** — their power-on values from the 2D
  init list (`0x12C8 = 0x1FF`, `0x12CC = 0xFFFF`, `0x12D0 = 0`,
  `docs/parhelia-2d-accel-complete.md` §6 step 6) suggest range/limit semantics
  rather than plain values, and one MMIO capture of a `glClearDepth(0.5)` clear
  settles it.

> **CORRECTION.** `docs/parhelia-gl-driver.md`'s depth-clear recipe — "point a
> slot at the Z surface, DWGCTL `0x1300 = 0xcc0` (fill), rect in `0x138c`" — is
> not what the vendor does.  `0xcc0` is a 2D solid fill (opcode 0, BOP 0xC, dst
> slot 3); the real path is DWGCTL opcode 4 with WARP geometry and the
> `0x12C8`/`0x12CC`/`0x12D0` value registers.  A 2D fill into the Z surface may
> still *work* for a first driver — that is exactly what the open driver's
> proven 2D fill does — but it will bypass the fast-clear hardware and it is not
> how depth+stencil are cleared together.

**Fast clear.**  The `pDev->[0x2188]` slot plus the 3 MB size gate is a
hierarchical/fast-Z arm: only one surface at a time can hold it, and
`mtx2372`/`mtx2060` set `zsurf->[0x1d0] = 1` to mark it dirty.  [INFERRED —
the semantics of `0x2188` are read off its single-slot, full-surface-only,
size-capped usage, not from a register.]

**Colour clear** goes through the *other* engine object: `mtx3354` @0x11720
dispatches on `record[0] & 8` and then to `mtx2036` @0xf520 (object `ctx+0x1d8`)
or `mtx1602` @0x11210 (object `ctx+0x1a8`) — two parallel engine front-ends,
[INFERRED] 3D/WARP and 2D/blit respectively.

---

## 3. Write masks and the fragment-output path

### 3.1 `0x1324` PLNWT is the real, confirmed plane write mask  [CONFIRMED]

`0x1324` (index 0xC9) is a full 32-bit **plane write mask** in the shared engine
state (`mtx1619` shadow `+0x0c`, dirty bit 0x02).  Facts:

* engine init writes `0x1324 = 0xFFFFFFFF` (`mtx_drv.so`
  `SetupHardwareForDisplayDriver`);
* the DPS path programs it from a per-operation value (`mtx1389` @0x255e0:
  `shadow[0xb0c] = rec[0x48]`), and the 2D path bpp-replicates the value exactly
  like FCOL/BCOL (8bpp `c|c<<8|c<<16|c<<24`, 16bpp `c|c<<16`);
* the GL clear force-overrides it to `0xFFFFFFFF` (§2.3).

That is a *bit-plane* mask, i.e. it can express `glColorMask` for any surface
format by replicating the per-channel enables into the channel's bit range, and
it is the mask that must be restored after a clear.

`docs/parhelia-gl-driver.md` and `docs/parhelia-3d-state-complete.md` §5 give
`glColorMask = 0x12a0`, "4-bit RGBA write-enable (bit0=R…bit3=A; 0xf=all)".
Nothing in the kernel core writes `0x12a0`, and that claim comes from the ICD
static analysis whose index→register mapping §0.1 invalidates.  Treat
**`0x12a0` as [OPEN]** and `0x1324` as the mask an open driver should use first.

### 3.2 `glDepthMask`  [CONFIRMED as state, register OPEN]

`glDepthMask` is ICD state slot **0x0e**; the kernel stores it as a *byte* at
software-shadow `+0xc9f`, adjacent to depth-test enable (`+0xc9b`, slot 0x07) and
depth func (`+0xca0`, slot 0x17) — a compact "depth state" sub-struct with one
common dirty bit (0x04 at `+0xc87`).  `mtx1900` @0x17830 then materialises that
sub-struct into a flat hardware-state array at `ctx+0x10`
(`+0x1c` ← depth enable, `+0x38` ← depth mask, `+0x5c` ← depth func).  The
final array→register emitter was not located in this pass; the register offsets
for depth test/func/mask are therefore **[OPEN]**, and the values 0/1 and the
1..8 function enum from the live capture remain the trustworthy part.

### 3.3 Where the fragment program's output lands  [CONFIRMED, from existing work]

Nothing in this pass contradicts `docs/parhelia-fragment-isa.md`: the combiner's
destination register `r0` (code 4) is the pixel colour, `0x1050` selects which
uploaded fragment program a draw uses (proved in
`docs/parhelia-gl-feature-map.md` §8 by the program-id match against the
`{0x46, 1, program_id, 0}` upload header), and the result then goes through
blend → PLNWT → surface slot 0.  There is **no** multiple-render-target
mechanism: one colour slot is bound per draw, and the fragment ISA has a single
output register.

---

## 4. Multisampling

**Verdict: `GL_ARB_multisample` is advertised but not implemented in the Linux
stack.  Parhelia's FAA-16x hardware is real and is parameterised in the shared
Matrox core, but no Linux component ever turns it on.**  [CONFIRMED]

### 4.1 The GL side is pure state tracking  [CONFIRMED]

* `glSampleCoverageARB` @0x1b09b0 in `libGL.so.1.3.0` is a plain alias of
  `glSampleCoverage` @0x1ac0f0, which dispatches through the per-context table
  slot `+0x9b0`.  The one real implementation behind that slot is **0x128ab0**:
  clamp the value to [0,1], store it at `ctx+0x7c80`, store `invert` at
  `ctx+0x7c84`, `or ctx[0x10], 0x10000`, return.  **No MMIO write, no DMA
  packet, no command-stream record.**  The other table entries are a `repz ret`
  (no current context), a `GL_INVALID_OPERATION` setter (inside `glBegin`), and
  the display-list recorder.
* The multisample state block `ctx+0x7c70` is initialised once by 0x1c7380:
  `GL_MULTISAMPLE = 1`, **`GL_SAMPLE_BUFFERS = 0`**, `GL_SAMPLES = 0x10` (a
  vestigial FAA-16x constant), alpha-to-coverage / alpha-to-one /
  sample-coverage = 0, coverage value 1.0.  `GL_SAMPLE_BUFFERS` and
  `GL_SAMPLES` are **written at exactly that one site in the whole library** and
  read only by `glGet*` / `glIsEnabled` / `glPushAttrib` / `glPopAttrib`.  By the
  ARB_multisample spec, `SAMPLE_BUFFERS = 0` means multisample rasterisation is
  never in effect.
* Dirty bit `0x10000` set by every multisample state change is tested only by
  `glPushAttrib` (0x126320) and `glPopAttrib` (0x126d49) as an attribute-group
  mask.  **There is no hardware-emit consumer.**
* `ctx+0x7c98` is a "state locked by driver" latch from the Windows codebase:
  when non-zero, `glEnable`/`glDisable(GL_MULTISAMPLE)` silently skip the store.

### 4.2 GLX reports nothing  [CONFIRMED]

`glXGetFBConfigAttrib` @0x35d0a0 has no case for `GLX_SAMPLE_BUFFERS` (100000)
or `GLX_SAMPLES` (100001) — both fall through to `GLX_BAD_ATTRIBUTE`.  The
client GLX extension string is `GLX_ARB_get_proc_address GLX_EXT_visual_rating
GLX_EXT_visual_info GLX_SGI_swap_control`; the vendor server module
`xserver/7_4_0/libglx.so`, which builds the visual/fbconfig table
(`glxMTXInitVisuals` / `glxMTXSetVisualConfigs`), contains **zero** references to
100000/100001.  No sample buffer is ever allocated: `MTXGLXAllocateClientBuffer`
in `mtx_drv.so` allocates one page-aligned type-3 buffer and nothing else.

The `GL_EXTENSIONS` string is a fixed `.data` blob at 0x66e0e0 returned verbatim
by `glGetString` (0x120e2b) — no per-chip trimming — which is why
`GL_ARB_multisample` is advertised on every Parhelia/P650/P690 regardless.

### 4.3 The FAA-16x machinery exists but is dead on Linux  [CONFIRMED]

`kernel/src/MtxParhlParms.c` declares seven fragment-antialiasing tunables:

```c
reg3DAAMode                    0x52
reg3DThresholdAA               0x007FE000   /* sort-independent AA, fragment buffer */
reg3DFragSize                  0x3F800000   /* 1.0f */
reg3DForceCompressedMask       TRUE   /* different AA mode when dest alpha enabled */
reg3DForcePLNWMASK4FAA         TRUE   /* do not execute the PLNWTMASK hack in FAA mode */
reg3DResolveAlphaBlendedFrags  FALSE  /* send FRAG_RESOLVE even with alpha blending */
reg3DResolveOnStencilOpChange  TRUE
```

Their names appear as strings in `mtx_drv.so` (registration `mtx724` @0x60d70,
which has **no caller**) and in `mtx_parhelia.o`, and in **no** form in
`libGL.so.1.3.0`.  The win32 `MTXPARD.dll` really reads them (`AAMode` @0x109740
default 0x54, `ThresholdAA` @0x1097b2, `FragSize` @0x10983e = 4.0f or ~4.35f
depending on a chip-revision field) — so FAA is driven on Windows, through the
D3D path, which shares nothing with the Linux ICD.

The only register link recoverable statically is indirect:
`reg3DForcePLNWMASK4FAA` ("do not execute PLNWTMASK hack in FAA mode") ties the
FAA path to **PLNWT `0x1324`** (§3.1).  The FAA control, fragment-buffer and
`FRAG_RESOLVE` register indices are **[OPEN]** — they have no Linux consumer at
all, and on Windows they sit behind the MTXPARD D3D object graph.

### 4.4 A separate, *live* mechanism: the per-axis render-target scale  [CONFIRMED mechanism, INFERRED purpose]

Independently of all of the above, the kernel core has a scaled-render-target
path that **is** consumed.  Each drawable carries two floats at `+0x214` (x) and
`+0x218` (y), initialised to `1.0f` (`mtx3834` @0xe600).
`mtx2480` @0x129e0 sets them from two of the drawable's bound surfaces: it reads
`drawable->[0x238]->[0x210+0x20]` / `+0x24` and `drawable->[0x240]->[0x210+0x20]`
/ `+0x24` and **requires the two to be identical** (`ucomiss`, mismatch → bail
out and leave the scale at 1.0) before storing them on the drawable.  The
drawable holds six surface pointers, zeroed together by `mtx3834` @0xe600:
`+0x220`, `+0x228`, `+0x230`, `+0x238`, `+0x240`, `+0x248`; of these **`+0x248`
is the depth surface** [CONFIRMED — `mtx237` and `mtx1602` take it as the Z
surface and read its width/height/bpp from `+0x1dc`/`+0x1e0`/`+0x1d4`], and
`+0x238`/`+0x240` are two colour buffers [INFERRED].

`mtx1602` @0x11210 then multiplies every clear rectangle's four coordinates by
those two floats before submitting, unless the clear record has mask bit 0x08
set.

A per-axis scale applied to a surface pair that must agree is a **supersampled
render target** — the render surface larger than the window, downscaled on
presentation.  [INFERRED.]  It is the only anti-aliasing-shaped machinery in the
kernel core that is actually wired up: there is no sample-count register, no
sample mask and no resolve primitive anywhere in `mtx_parhelia.o`.  With
`GL_SAMPLE_BUFFERS` pinned to 0 by the ICD (§4.1) nothing in the OpenGL path ever
sets the scale away from 1.0, so on Linux this is dormant too — but it is the
mechanism an open driver would use for supersampled AA, and it costs nothing:
allocate colour and depth larger, keep the two scales equal, and scale the
clear/scissor rectangles.

### 4.5 Practical consequence

Nothing in the Matrox Linux stack can serve as a reference for programming
Parhelia FAA-16x.  For an open driver: report `GLX_SAMPLE_BUFFERS = 0` honestly,
or implement **supersampling** on top of §4.4 and §1 (a larger colour+depth pair
plus a scaled blit on present) — which needs no new register knowledge at all.

---

## 5. Render to texture / off-screen surfaces

**Nothing in the render-target path is tied to scanout.**  A surface descriptor
is `{VRAM byte offset, pitch|type|format|tiling, dims}`; the 3D pipeline reaches
it only through slot 0 (colour) and slot 2 (depth).  Pointing the engine at an
off-screen surface is therefore exactly the same three-register write as pointing
it at the screen:

```
0x1220 = offscreen_vram_byte_offset
0x1224 = (pitch_px & 0x3fff) | (1 << 16) | (fmt << 24)      /* pitch mult. of 32 px */
0x1230 = z_vram_byte_offset
0x1234 = (z_pitch_px & 0x3fff) | (1 << 16) | (z_fmt << 24)
```

plus a full-surface scissor (`0x1328 = 0`, `0x132c = 0x0fff0fff`) instead of the
X clip-rectangle loop of §1.4.  [CONFIRMED that this is the whole mechanism —
the vendor's own off-screen paths (`mtx1637` blit, `mtx3388` host transfer,
`mtx2954` clear) do nothing else; [INFERRED] that a WARP draw needs no extra
step, since the clear already draws WARP geometry into a slot-0/slot-2 pair that
is programmed the same way.]

Surface memory comes from ioctl **`0xc0804618`** (`mtx1545` @0x12a0,
`OsAllocSurfaceBlockMemory`): a 128-byte argument whose `arg[0x20] == 1` selects
a 0x78-byte *surface* record from heap `pDev->[0x2138]`, returning physical
address, pitch and size (see `docs/parhelia-kernel-abi.md` §3.2).  An open driver
on DRM/TTM allocates however it likes; only the descriptor encoding matters.

**Consequences for a future FBO implementation.**

* There is exactly **one** colour slot bound per draw and one fragment output
  register, so `GL_ARB_draw_buffers`-style MRT is not available. [CONFIRMED]
* Colour and depth are independent surfaces with independent pitches; they do
  **not** have to be the same size, but the vendor requires their per-axis scale
  factors (§4) to match. [CONFIRMED]
* The vendor stack itself exposes no FBO: `GL_EXT_framebuffer_object` is absent
  from the extension string, `glXCreatePbuffer` / `glXDestroyPbuffer` and
  `glCopyTexImage*` / `glCopyTexSubImage*` are the only off-screen entry points
  exported by `libGL.so.1.3.0`.  So there is no vendor precedent to copy for
  render-to-texture; there is also no hardware obstacle. [CONFIRMED for the
  exports, [INFERRED] for "no obstacle"]
* Textures are addressed by a *different* register file (`0x11dc` base,
  `0x11e0` control, `0x11e4` height — `docs/parhelia-fragment-isa.md` §11), whose
  pitch rule is the same 32-pixel alignment.  Rendering to a surface and then
  sampling it needs only that the same VRAM offset and pitch be programmed into
  both descriptors, with a fence in between. [INFERRED]

---

## 6. Getting a finished frame on screen

Traced end to end through `libGL.so.1.3.0` and `mtx_parhelia.o`.  The ICD never
talks to the X server for a swap and never touches a register itself: everything
goes through the same `_IO('F',0x21)` = **`0x4621`** escape doorbell, and the
kernel does either a 2D blit or a real CRTC page flip.

### 6.1 `glXSwapBuffers`  [CONFIRMED]

`glXSwapBuffers` @0x35f410 resolves the drawable client-side (no GLX protocol
round trip, no `XSync`) and tail-calls the swap at **0x35a4a0**, which:

1. **flushes the pending command stream** for every context bound to the drawable
   (0x120cc0 → 0x1c9be0 → escape command 1, the same command-list submit that
   carries draws);
2. emits stream opcode **0x3e** with 0/1 when the per-context flag `[gl+0xf8]`
   disagrees — a front/back render-target toggle [CONFIRMED opcode, INFERRED
   meaning];
3. branches on `[drawable+0x58]`:
   * `== 0` → **BLIT**: escape command **0x10** (kernel `mtx986` @0x50980);
   * `!= 0` → **PAGE FLIP**: swap the device-level surface handles
     `[dev+0x48] ↔ [dev+0x50]`, then escape command **0x1c**
     (kernel `mtx646` @0x50680);
4. retries while the escape returns `-4` ("flip queue full");
5. optionally clears the new back buffer (`GL_COLOR_BUFFER_BIT`).

Escape payloads:

```
FLIP (0x1c):  +0x00 u64 new-front surface handle
              +0x08 u32 swap interval
BLIT (0x10):  +0x00 u32 flags=1   +0x08 context id
              +0x10 u64 SRC (back)  +0x18 u64 DST (front)
              +0x20/+0x28 ptrs to src/dst rects {x, y, …, w@+0xc, h@+0x10}
              +0x40 u32 0xffffffff
```

Note the escape-command namespace (`.rodata+0x1fe8`, commands 3..31, dispatched
by `mtx3683` @0x4de50) is **distinct** from the command-list opcode namespace of
§0.1 (`.rodata+0x1b08`).  Command 1 is the only one that reaches `mtx558`.

### 6.2 The page-flip primitive  [CONFIRMED from code, not HW-verified]

`mtx646` @0x50680 queues the flip; `mtx3795` @0x315b0 parks it in a 32-entry
queue and `mtx742` @0x313e0 writes a **tag packet into the bus-master DMA
stream** and kicks the ring at `0x408`.  The flip is then executed **from the
vertical-blank interrupt**: `mtx987` @0xa0960 (`mtx_parhl_driver.irq_fast`,
`.data+0x30`) → `mtx4025` @0x3a590, which bumps the per-head vblank counter,
reads the DMA completion tag from **`0x41C`** (masked `& ~7`) and only then calls
`mtx2073` @0x343a0 — so a queued flip fires only after the render stream that
precedes it has retired.

`mtx2073` is the actual register write:

| head / plane | control register | slot field |
|---|---|---|
| head 0 graphics | `0x1800` | bits [8:7], bit 6 = latch-immediately, mask `&= 0xFFFFFE3F` |
| head 1 graphics | `0x1A00` | same |
| head 0 second plane | `0x19A8` | bits [10:9] |
| head 1 second plane | `0x1BA8` | bits [10:9] |

The buffer addresses live in a **4-deep START bank** written by `mtx3863`
@0x34a60 as `value = offset & 0x07FFF800` (2 KB aligned):

```
head 0 graphics    : 0x1824 + slot*4
head 1 graphics    : 0x1A24 + slot*4
head 0 2nd plane   : 0x19C8 + slot*4
head 1 2nd plane   : 0x1BC8 + slot*4
```

Completion test, `mtx416` @0x34170: `pending = (reg >> 7) & 3`,
`active = (reg >> 16) & 3`, retired when they are equal.

So the flip primitive is: *write the new scanout base into one of four START
slots, then point `DC_CTRL[8:7]` at that slot*; the hardware latches at the next
retrace unless bit 6 is set.  Bit-6 polarity is pinned by the policy plumbing:
`reg3DFlipOnVBlank = 0` (never sync) → bit 6 = 1.

Only slot 0 (`0x1824`) is hardware-confirmed in this repo; slots 1–3 are
code-derived only.

### 6.3 Vsync and swap interval  [CONFIRMED]

* `mtx646` reads a policy through a device vtable and combines it with the app's
  interval: policy 0 = never sync, 1 = always (forced to ≥1), ≥2 = application
  defined.  That is exactly **`reg3DFlipOnVBlank`** (default **3**) in
  `matroxdriver-x86_64-cvs-20090511/kernel/src/MtxParhlParms.c:200`.
  `regSysVSyncInt` (default FALSE, line 279) enables the VSync interrupt.
* `glXSwapIntervalSGI` is not an exported symbol — reachable only via
  `glXGetProcAddress` (→ 0x35f2b0).  It rejects `n <= 0` and stores `n` at
  `[device+0x44]`, which is what lands in the flip payload `+0x08`.
  `GLX_SGI_swap_control` is advertised.
* Interval > 1 counts vblanks in the ISR, not in a busy wait.

### 6.4 Buffer allocation  [CONFIRMED]

Once per **device** (not per drawable), in ICD `0x358ac0`, all three through
escape command **0x0c** ("create surface", 0x1c-byte descriptor
`{flags, ?, format, width, height, ?}`, returning an opaque kernel handle):

| buffer | descriptor | ICD field |
|---|---|---|
| front / primary | flags `0x21` (bit5 = displayable), `+0x04 = 1` | `[device+0x48]` |
| back | front's geometry, bit5 cleared, bit7 set | `[device+0x50]` |
| **depth** | **format `0x4f` / `0x4b`**, front's width/height | `[device+0x58]` |

The ICD never learns a VRAM offset or a pitch — it holds handles only, and the
kernel resolves them into the `{base, cfg, dims}` descriptors of §1.  Buffering
is **double**: the ICD allocates one back buffer, the kernel keeps two scanout
slot indices (`[[dev+0xd8]+0x6b0] + 0x30 / + 0x34`), even though the START bank
has four slots.  The back buffer is device-global and shared by every GLX
drawable on that device.

> **Lead for the open S8Z24 question of §2.1:** `0x4f` and `0x4b` are the ICD's
> *depth* surface-create format enums.  They are not the 6-bit hardware code in
> `0x1234` (which must be a 32- or 16-bit-class code from the `mtx969` table),
> but the surface-create handler `mtx2359` (escape command 0x0c) is where one
> is translated into the other.  That is the shortest remaining path to the
> S8Z24 enum without a hardware capture.

### 6.5 Fencing  [CONFIRMED]

Two mechanisms, both already documented in `docs/parhelia-kernel-abi.md` §7 and
re-confirmed here:

* client side — the per-context stream flush before the swap is queued, plus
  escape command 3 as an explicit "wait idle" spin (ICD 0x358783);
* hardware — the `0x12BC` / `0x41C` tag fence.  `mtx2434` on meeting register
  `0x12BC` in the stream waits for `0x450` bit 22, writes `tag & ~1`, and only
  polls `0x41C` if bit 0 of the tag was set.  The flip path uses the
  **non-blocking** half: `mtx742` embeds the tag, and the vblank ISR reads
  `0x41C` to decide when the queued flip may be applied.

So: for an open driver, "finished frame → visible" is *flush the command stream,
emit a fence tag, then either blit slot-0-surface → scanout surface with the 2D
engine, or write the new base into a START slot and point `DC_CTRL[8:7]` at it*.
No CPU spin is required in either case.

---

## 7. Minimal implementable recipe

Everything below is register-level and can be driven either by direct MMIO
(`0x1000 + index*4`) or through the DMA ring.

**Bind a colour + depth target** (one packet, header `0x8d8c8988`):

```
0x1220 = colour_base            /* VRAM byte offset; bits[1:0]=3 for AGP/system */
0x1224 = (pitch_px & 0x3fff) | (1<<16) | (fmt << 24)
0x1230 = depth_base
0x1234 = (z_pitch_px & 0x3fff) | (1<<16) | (z_fmt << 24)
```
pitch is in **pixels, rounded up to a multiple of 32**; `fmt` from the §1.2
table (0x30 or 0x32 for 32bpp colour, 0x20 for 16bpp, 0x10 for 8bpp).

**Scissor** — `0x1328 = (top<<16)|left`, `0x132c = (bottom<<16)|right`, enabled
by DWGCTL bit 23; open it to `0x0FFF0FFF` for a full-surface pass.

**Write mask** — `0x1324 = 0xFFFFFFFF` (bit-plane mask; bpp-replicate a
per-channel mask into it for `glColorMask`).

**Clear** — either the vendor way (DWGCTL `0xC4`, PLNWT `0xFFFFFFFF`, full
scissor, values in `0x12C8`/`0x12CC`/`0x12D0`, geometry through the WARP vertex
planes), or, for a first driver, the already-hardware-proven 2D solid fill
(`docs/parhelia-2d-accel-complete.md` Op 1) run once into the colour surface and
once into the depth surface with the appropriate bit pattern.  The 2D route is
slower and cannot clear depth+stencil in one pass, but it is known to work on
real silicon.

**Present** — flush, fence on `0x12BC`/`0x41C`, then blit or write
`0x1824 + slot*4` and point `0x1800[8:7]` at that slot.

---

## 8. Open items, ranked by how much they block an open driver

1. **The S8Z24 format code in `0x1234`** — blocks depth entirely.  Shortest
   static path: escape command 0x0c handler `mtx2359`, which turns the ICD's
   depth format enum (`0x4f` / `0x4b`) into a hardware descriptor.  Fallback: one
   MMIO capture on a 24/8 visual.
2. **Which of `0x12C8` / `0x12CC` / `0x12D0` is depth, stencil and colour** in
   the clear.  One capture of a `glClearDepth(0.5)` + `glClearStencil(0x5a)`
   clear settles it.
3. **The state-shadow → register emitter.**  §0.1 recovered the ICD state slot →
   software shadow mapping and §3.2 the shadow → flat array copy (`mtx1900`
   @0x17830); the array → register step is the missing link, and it is what would
   finally give *proven* offsets for depth test/func/mask, colour mask, blend and
   stencil.  Until then those offsets are unproven in **every** document in this
   repo.
4. **DWGCTL opcode 4** — full field decode (the clear/3D-rect primitive).
5. **`0x12FC`** — a small mode bitfield fed from `mtx2215`; bits 0, 1 and 4 are
   manipulated, meaning unknown.
6. **The third per-slot descriptor word** (`dev+0xb48+4i`, 14-bit width/height)
   — no register emitter found; may be software-only.
7. **The FAA-16x control / fragment-buffer / `FRAG_RESOLVE` registers** — no
   Linux consumer exists at all (§4.3).  Only reachable by reversing
   `MTXPARD.dll`'s D3D path from the `AAMode` / `ThresholdAA` / `FragSize` switch
   consumers, or by an MMIO trace under Windows.  Not needed if supersampling
   (§4.4) is used instead.
