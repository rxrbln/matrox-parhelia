# Matrox Parhelia — render target, depth/stencil and framebuffer state

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
`Surface2d::Enable`.

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

*(section written from the dedicated sweep of libGL / mtx_drv / MTXPARD — see
the verdict below)*

What this pass established independently, from the kernel core:

**There is a per-axis render-target scale factor.**  Each drawable carries two
floats at `+0x214` (x) and `+0x218` (y), initialised to `1.0f` (`mtx3834`
@0xe600).  `mtx2480` @0x129e0 sets them from the *bound surfaces*: it reads
`colour_surface->[0x210+0x20]` / `+0x24` and `depth_surface->[0x210+0x20]` /
`+0x24`, **requires the colour and depth scales to be identical** (`ucomiss`,
mismatch → bail), and only then stores them on the drawable
(`drawable+0x238` = colour surface, `drawable+0x240` = depth surface
[CONFIRMED by that same comparison]).

`mtx1602` @0x11210 then multiplies every clear rectangle's four coordinates by
those two floats before submitting, unless the clear record has bit 0x08 set.

A per-axis scale ≠ 1.0, applied to a colour+depth pair that must agree, is a
**supersampled render target** — the render surface is larger than the window and
is downscaled on presentation.  [INFERRED, but the mechanism is unambiguous.]
It is the only anti-aliasing-shaped machinery in the kernel core: there is no
sample-count register, no sample mask, and no resolve primitive anywhere in
`mtx_parhelia.o`.

