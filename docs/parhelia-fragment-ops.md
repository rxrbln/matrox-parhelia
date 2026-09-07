# Parhelia fragment operations — depth, stencil, alpha test, blend, masks, dither, ROP

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** The `reg = 0x1000 + slot*4` rule is refuted.
Everything the hardware does to a fragment **after** the fragment program has
produced a colour.  The fragment program / combiner ISA itself is a separate
document (`docs/parhelia-fragment-isa.md`) and is not repeated or revised here.

Sources, in order of weight:

* **[HW]** `captures/glsweep-0528-raw.txt` — live capture on a real Parhelia
  (PCI 0x0528) under the vendor 2009 stack.
* **[GL]** `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0`
  (the Linux ICD).  Addresses below are file/VA offsets in that binary; the
  disassembly used is `scratchpad/libGL64.asm`.
* **[WIN]** `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` (the Windows
  XPDM display/D3D driver, `scratchpad/pard.asm`) — an independent statement of
  the same silicon.
* **[SRC]** `matroxdriver-x86_64-cvs-20090511/kernel/src/MtxParhlParms.c` —
  vendor kernel-module parameters, shipped as source.

Tags: **[CONFIRMED]** proven by at least two independent lines of evidence or by
an exact byte-for-byte match against the live capture; **[INFERRED]** the best
reading, self-consistent but single-sourced; **[OPEN]** unresolved.

---

## 0. The headline: blending is fixed-function, not code generation

**Parhelia implements every fragment operation in this document in dedicated
fixed-function state, not as instructions appended to the fragment program.**
A driver never has to recompile or re-upload a shader because the blend mode,
depth func, stencil op, alpha ref, colour mask, dither or logic op changed.
[CONFIRMED]

The proof is in the live capture.  In the vendor stack a fragment program is
uploaded as a **1656-byte block record** (stream key `0x46`; see
`parhelia-gl-feature-map.md` §8, where four such blobs were decoded against the
combiner ISA).  Compare the two kinds of state change in the same sweep:

| operation | bytes emitted into the command stream | program upload? |
|---|---|---|
| `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` + `glEnable(GL_BLEND)` | **0xd0** total, of which 10 are `(index,value)` pairs | **none** |
| `glBlendFunc(ONE,ONE)` | **0x54**, 4 pairs | **none** |
| `glEnable(GL_ALPHA_TEST)`/`glAlphaFunc` | **0x50**, 4 pairs | **none** |
| `glEnable(GL_STENCIL_TEST)`/`glStencilOp` | **0xa4**, 5 pairs | **none** |
| `glTexEnvi(GL_REPLACE)` (a real combiner change) | 1928 bytes | **yes, one 0x664-byte blob + program-select** |

Only the texture-environment change — which really is the combiner — produces a
program.  Every fragment-op change produces plain register writes.

Two corroborations:

* In the ICD the whole group is a shadowed register file with per-register
  "changed?" comparisons (`libGL` @0x113b90–0x115000).  There is no code path
  from a blend/stencil/alpha state change into `GLFragmentShaderManager`.
* The hardware enum values are the **Direct3D 7/8 render-state enums verbatim**
  (§8).  A part whose blender is a shader would not encode `D3DBLEND_*`,
  `D3DCMPFUNC_*`, `D3DSTENCILOP_*` and `D3DBLENDOP_*` numerically in silicon.

Consequence for a from-scratch driver: the fragment-op state is ~20 registers
that can be set once per state change and left alone.  No shader-variant cache,
no blend-lowering pass.

---

## 1. Prerequisite correction: the command-stream grammar (and what it does to
   every register number previously published from the capture)

This section has to come before the register map, because the previously
published GL→register table is systematically wrong in a way that is now fully
explained.

### 1.1 The real grammar  [CONFIRMED — `libGL` @0x1c9c50]

The ICD→kernel stream is not "mostly `(index,value)` pairs with some block
opcodes".  It is uniformly:

```
[ u32 key ][ u32 count ]   <- 8-byte record header
[ count * elemsize bytes ] <- payload
```

`GLCommandBuffer::Reserve(buf, key, elemsize, count)` at `0x1c9c50`:

```
1c9c94:  rax = buf->last_header          ; [rbx+0x28]
1c9c98:  cmp DWORD PTR [rax], key        ; same key as the previous record?
1c9c9b:  je  1c9d30                      ;   -> yes
1c9ca1:  buf->last_header = cur          ;   -> no: start a NEW header
1c9ca5:  hdr[1] = count
1c9ca9:  hdr[0] = key
1c9cac:  cur += 8                        ; payload follows immediately
...
1c9d30:  add DWORD PTR [rax+4], count    ; COALESCE: just bump the count
```

The last line is the trap: **consecutive records with the same key are merged
into one header whose count is bumped**, so a run of N register writes appears
as one 8-byte header followed by N 8-byte pairs.

A naive pair decoder therefore eats the header as if it were a register write:
`(key, count)` is read as "register `0x1000+key*4` = count".

### 1.2 The two register banks  [CONFIRMED]

Register writes use **two different keys**, i.e. two different destination
register files, both with an 8-byte `(index, value)` payload:

| key | ICD shadow copy | indices used | what lives there |
|---|---|---|---|
| **0x0c (12)** | `hwmgr + index*4` | 0x07…0xab (108 of them) | the main 3D register file — depth, stencil, alpha test, dither, blend enable, ROP, colour mask, blend equation |
| **0x05 (5)** | `hwmgr + 0x900 + index*4` | 0x01…0x1c (24 of them) | a second, smaller file — **the four blend factors**, fragment-program select, and rasteriser bits |

(Two further register-shaped keys exist and are out of scope here: key 0x0e =
per-texture-stage file, payload `(u16 stage, u16 index, u32 value)`, 112 entries;
key 0x06 = 36 entries with the same 2×u16 header.)

Bank 0x0c index *i* maps to MMIO `0x1000 + 4*i`.  [CONFIRMED for the
fragment-op range] — three independent lines agree:

1. The ICD's shadow copy of bank 0x0c is literally a register-file image at
   `hwmgr + i*4` (e.g. index 0x34 ↔ shadow +0xd0, index 0xab ↔ shadow +0x2ac).
2. MTXPARD's DMA interpreter computes the register address from a packet index
   with exactly this rule and clamps the window to `0x1000 ≤ reg < 0x1400`:
   ```
   918f5:  lea edi,[edi*4+0x1000]     ; reg = 0x1000 + index*4
   91a42:  cmp edi,0x1000 / jb ...    ; the 3D window is 0x1000..0x13ff
   91a4a:  cmp edi,0x1400 / jae ...
   ```
3. The ICD's own bank-0x0c index whitelist (rodata @0x39f778) **skips indices
   0x1f, 0x20 and 0x21** — exactly the DMA pad registers 0x1080/0x1084 that
   MTXPARD's interpreter special-cases and discards.  A namespace that avoids
   the DMA pads is the DMA namespace.

Bank 0x05's absolute MMIO base is **[OPEN]** — see §11.

### 1.3 Re-decoding the capture, byte for byte

`blend_srcalpha`, raw stream (the first token is overwritten by the driver after
`glFinish`; its identity follows from the record that succeeds it):

| offset | dwords | old (wrong) reading | correct reading |
|---|---|---|---|
| 0x00 | *(clobbered)* | — | header **key 0x0c, count 1** |
| 0x08 | `1b 01` | `0x106c = 1` | pair: bank-C 0x1b = 1 → **blend enable** ✓ |
| 0x10 | `05 04` | `0x1014 = 4` ← "unexplained" | header **key 0x05, count 4** |
| 0x18 | `0a 05` | `0x1028 = 5` | pair: bank-5 0x0a = 5 → **src RGB factor** |
| 0x20 | `09 05` | `0x1024 = 5` | pair: bank-5 0x09 = 5 → **src alpha factor** |
| 0x28 | `0c 06` | `0x1030 = 6` | pair: bank-5 0x0c = 6 → **dst RGB factor** |
| 0x30 | `0b 06` | `0x102c = 6` | pair: bank-5 0x0b = 6 → **dst alpha factor** |
| 0x38 | `0c 03` | `0x1030 = 3` ← "second write" | header **key 0x0c, count 3** |
| 0x40 | `07 00` | `0x101c = 0` | pair: depth test off ✓ |
| 0x48 | `17 02` | `0x105c = 2` | pair: depth func LESS ✓ |
| 0x50 | `0e 01` | `0x1038 = 1` | pair: depth write mask ✓ |

Every count matches the number of pairs that follow it exactly.  The same
re-reading works for every other op in the sweep (`alphatest` = header
`(0x0c,4)` + 4 pairs; `stencil` = header `(0x0c,5)` + 5 pairs; `blend_additive`
= header `(0x05,4)` + 4 pairs).

**This resolves the ambiguity flagged in `docs/parhelia-gl-feature-map.md` §6.**
`0x1030` is neither multiplexed nor mis-sliced in the sense suspected: the
values 1/2/3 attributed to it by `scissor`, `cullface`, `fog`, `lighting` and
`polymode` are **record headers of key 0x0c whose count happened to be 1, 2 or
3**.  It is the third possibility the question offered — two different records
landing at the same index — with the twist that one of them is not a record at
all but a header.

The real `0x1030` is the raster-op register (§7), always written with a
4-bit ROP code, default 0xC.

---

## 2. Register map — fragment operations

Bank 0x0c (MMIO `0x1000 + 4*index`):

| MMIO | idx | function | encoding | tag |
|---|---|---|---|---|
| 0x101c | 0x07 | depth test enable | 0/1 | CONFIRMED |
| 0x1024 | 0x09 | shade mode | 1 = FLAT, 2 = GOURAUD, 0x7fffffff = invalid | CONFIRMED |
| 0x1028 | 0x0a | line stipple | `(repeat<<16) \| pattern` | CONFIRMED |
| **0x1030** | **0x0c** | **raster op (ROP4)** | 4-bit truth table, **0xC = COPY** (default) | CONFIRMED (value) / INFERRED (address) |
| 0x1038 | 0x0e | depth write mask | 0/1 | CONFIRMED |
| 0x103c | 0x0f | alpha test enable | 0/1 | CONFIRMED |
| 0x105c | 0x17 | depth compare func | 1..8 = NEVER..ALWAYS | CONFIRMED |
| 0x1060 | 0x18 | alpha reference | **8-bit**, `(int)(ref*255 + 0.5)` | CONFIRMED |
| 0x1064 | 0x19 | alpha compare func | 1..8 = NEVER..ALWAYS | CONFIRMED |
| **0x1068** | **0x1a** | **dither enable** | 0/1 | CONFIRMED |
| 0x106c | 0x1b | blend enable | 0/1 | CONFIRMED |
| 0x1070 | 0x1c | fog enable | 0/1 | CONFIRMED |
| 0x1074 | 0x1d | separate specular colour | 0/1 | CONFIRMED |
| **0x10d0** | 0x34 | stencil test enable | 0/1 | CONFIRMED |
| **0x10d4** | 0x35 | stencil **fail** op | 1..8 (§5) | CONFIRMED |
| **0x10d8** | 0x36 | stencil **z-fail** op | 1..8 | CONFIRMED |
| **0x10dc** | 0x37 | stencil **z-pass** op | 1..8 | CONFIRMED |
| **0x10e0** | 0x38 | stencil compare func | 1..8 = NEVER..ALWAYS | CONFIRMED |
| **0x10e4** | 0x39 | stencil reference | integer | CONFIRMED |
| **0x10e8** | 0x3a | stencil value (compare) mask | integer | CONFIRMED |
| **0x10ec** | 0x3b | stencil **write** mask | integer | CONFIRMED |
| **0x12a0** | 0xa8 | colour write mask | 4 bits, bit0=R bit1=G bit2=B bit3=A; 0xf = all | CONFIRMED |
| **0x12ac** | 0xab | blend equation | 1..5 (§8.3), 0x7fffffff = unsupported | CONFIRMED |

Bank 0x05 (absolute base **[OPEN]**, see §11):

| idx | function | encoding | tag |
|---|---|---|---|
| 0x09 | blend **source alpha** factor | factor enum (§8.1) | CONFIRMED |
| 0x0a | blend **source RGB** factor | factor enum | CONFIRMED |
| 0x0b | blend **dest alpha** factor | factor enum | CONFIRMED |
| 0x0c | blend **dest RGB** factor | factor enum | CONFIRMED |
| 0x14 | fragment-program select (program id) | integer | CONFIRMED |
| 0x01,0x02,0x04,0x05,0x07,0x08,0x0d,0x0e,0x12,0x13,0x15–0x1c | rasteriser / pixel-pipe config (cull winding mirror, stipple active, point/line params, 0x1b defaults to 1.0f, 0x1c defaults to 0x7fffffff) | — | INFERRED |

Block records (not register writes) that carry fragment-op state:

| key | payload | meaning | tag |
|---|---|---|---|
| **0x35** | 4 floats | **blend constant colour** (`GL_EXT_blend_color`) | CONFIRMED |
| 0x15 | 9 dwords | CLEAR (§9) | CONFIRMED |
| 0x36 | 2 floats | line width, point size | CONFIRMED |
| 0x0d | 32 bytes | render-target / draw-buffer binding | INFERRED |

---

## 3. Where each GL entry point lands (ICD state layout)

Useful when cross-checking the disassembly.  `ctx` = the GL state object
(`GLContext + 0x20`).

Depth block, `ctx+0xff8`:  `+0x00` enable, `+0x04` GL func enum, `+0x08` write
mask.  `glClearDepth` stores a **double** at `ctx+0x1008`.

Stencil block, `ctx+0xbe8`:  `+0x00` enable, `+0x04` GL func, `+0x08` ref,
`+0x0c` value mask, `+0x10/+0x14` fail (GL enum / hw code), `+0x18/+0x1c`
z-fail, `+0x20/+0x24` z-pass, `+0x28` clear value, `+0x2c` write mask.
(`glStencilOp` @0x123d00, `glStencilFunc` @0x124030, `glStencilMask` @0x1240c0.)

Alpha/blend/mask/ROP block, `ctx+0x1028`: `+0x00` alpha enable, `+0x04` alpha GL
func, `+0x08` alpha ref (float), `+0x0c/+0x10` srcRGB, `+0x14/+0x18` srcAlpha,
`+0x1c/+0x20` dstRGB, `+0x24/+0x28` dstAlpha (GL enum / hw code each),
`+0x2c` blend enable, `+0x30..0x3f` blend constant colour (4 floats),
`+0x40` blend equation GL enum, `+0x44` dither enable, `+0x48` draw buffer,
`+0x4c` logic-op enable, `+0x50/+0x54` logic op (GL enum / hw ROP),
`+0x68..0x6b` colour mask R,G,B,A.
(`glBlendFunc` @0x124750, `glBlendFuncSeparateEXT` @0x124820,
`glBlendEquation` @0x11d700, `glLogicOp` @0x11d730, `glColorMask` @0x124b00,
`glBlendColor` @0x128bc0, dirty mask at `ctx+0x10b0`.)

The whole group is flushed by one routine at `0x113b90–0x115400` (and a second,
identical copy at `0x1bb400–0x1bc600` used by another draw path), which compares
each value against a shadow and emits only what changed.  That deferral is why
an op's stream in the capture also carries the *previous* op's teardown.

---

## 4. Depth

### 4.1 Test, function, write mask  [CONFIRMED]

* `0x101c` = 0/1.
* `0x105c` = `GL_func − GL_NEVER + 1`, i.e. NEVER 1, LESS 2, EQUAL 3, LEQUAL 4,
  GREATER 5, NOTEQUAL 6, GEQUAL 7, ALWAYS 8.  The ICD does this with a jump
  table at rodata `0x36bcc4`; every case body is `mov ebp, <1..8>`
  (`0x1152b0`…`0x113d8b`).  An out-of-range enum writes the sentinel
  **0x7fffffff**, which the vendor uses for "state the hardware cannot express".
* `0x1038` = depth write mask, 0/1 (a single bit, not a per-bit Z mask).

### 4.2 Depth surface and clear  [CONFIRMED grammar, INFERRED mechanism]

The depth buffer is surface slot 2 (`0x1230` base / `0x1234` format, S8Z24 —
see `parhelia-gl-driver.md`); stencil shares that buffer, there is no separate
stencil base.

`glClear` does **not** touch the depth registers.  It emits one CLEAR block
record, key `0x15`, 9 dwords (`libGL` @0x1281c0):

```
[0] mask     bit0 = colour, bit1 = depth, bit2 = stencil, bit4 = accum
[1] colour   ARGB8888, each component (int)(f*255 + 0.5)
[2] depth    float (glClearDepth is kept as a double and narrowed here)
[3] stencil  integer
[4] accum    ARGB8888 of ((f*0.5)+0.5)*255 -> 0x80808080 for the GL default
[5..8] x, y, w, h   screen pixels
```

The GL→mask conversion is explicit at `0x1281d2`:
`GL_COLOR_BUFFER_BIT(0x4000)→1`, `GL_DEPTH_BUFFER_BIT(0x100)→2`,
`GL_STENCIL_BUFFER_BIT(0x400)→4`, `GL_ACCUM_BUFFER_BIT(0x200)→0x10`.

**This also resolves the "constant 0x80808080" flagged [AMBIGUOUS] in
`parhelia-gl-feature-map.md`.**  It is not a write mask or a fill pattern: it is
the **accumulation-buffer clear value**, encoded as `((v*0.5)+0.5)*255` per
component (the ×0.5+0.5 constant is the float 0.5 at rodata `0x36acf4`), which
for the GL default accum clear colour (0,0,0,0) is exactly 0x80808080.

The clear itself is executed below the ICD as a rectangle fill of the bound
surface (DWGCTL fill, `parhelia-gl-driver.md` "Depth clear").

### 4.3 Hierarchical Z / fast Z clear  [CONFIRMED absent]

There is **no** hierarchical-Z, Z-compression or fast-Z-clear mechanism anywhere
in this stack: no such register is written by the ICD, none appears in
MTXPARD's register set, and no module parameter mentions one.  A from-scratch
driver has nothing to invalidate on a depth clear, a depth-buffer rebind, or a
switch between depth-write-on and depth-write-off.

What *does* need invalidating is the **fragment (FAA) buffer** — see §10.  That
is a multisample/antialiasing resolve, not a Z-cache flush, but it is the only
"you must issue an extra operation when depth/stencil/blend state changes"
constraint on this part.

---

## 5. Stencil

Eight consecutive registers, `0x10d0`–`0x10ec`, in the order **enable, fail,
zfail, zpass, func, ref, value-mask, write-mask**.  All eight are single-sided:
there is only one stencil state block in the ICD and no front/back duplication
anywhere — **Parhelia has no two-sided stencil** (consistent with the ICD not
advertising `GL_EXT_stencil_two_side`).  [CONFIRMED]

Compare function `0x10e0` uses the same 1..8 NEVER..ALWAYS enum as depth and
alpha (jump table at rodata `0x36bce4`).  In the live capture `0x10e0` never
appears because the sweep only used `GL_ALWAYS`, which is the reset default and
so is filtered out by the ICD's shadow compare — that is why the earlier capture
could not identify it.

### 5.1 The stencil-op enum, including `GL_EXT_stencil_wrap`  [CONFIRMED]

Decoded from `glStencilOp` at `libGL` 0x123d00, which resolves all three
arguments through the same ladder and stores GL enum + hw code side by side:

| GL op | GL enum | hw code | disasm |
|---|---|---|---|
| `GL_KEEP` | 0x1E00 | **1** | 0x123e8c |
| `GL_ZERO` | 0x0000 | **2** | 0x123e97 |
| `GL_REPLACE` | 0x1E01 | **3** | 0x123de4 |
| `GL_INCR` (saturating) | 0x1E02 | **4** | 0x123e70 |
| `GL_DECR` (saturating) | 0x1E03 | **5** | 0x123eb5 |
| `GL_INVERT` | 0x150A | **6** | 0x123ec5 |
| **`GL_INCR_WRAP_EXT`** | 0x8507 | **7** | 0x123fe5 |
| **`GL_DECR_WRAP_EXT`** | 0x8508 | **8** | 0x123da2 |

So `GL_EXT_stencil_wrap` is **native hardware**, not emulation: codes 7 and 8
are the wrapping increment/decrement, codes 4 and 5 the clamping ones.  The
capture's single data point (`glStencilOp(KEEP,KEEP,REPLACE)` → `0x10dc = 3`)
matches, and upgrades that entry from [INFERRED] to [CONFIRMED] while also
identifying which of the three op registers it was: **z-pass**.

Independent cross-check [WIN]: this is exactly `D3DSTENCILOP`
(KEEP 1, ZERO 2, REPLACE 3, INCRSAT 4, DECRSAT 5, INVERT 6, INCR 7, DECR 8).

### 5.2 Masks

`0x10e8` is the **compare** mask (third argument of `glStencilFunc`), `0x10ec`
the **write** mask (`glStencilMask`).  Both are plain integers; the capture's
`0x10e8 = 0xff` is `glStencilFunc(…, 0xff)`.  [CONFIRMED]

`glClearStencil` does not touch a register; it feeds dword [3] of the CLEAR
record (§4.2).

---

## 6. Alpha test

> **Register-level encoding: `docs/parhelia-blend-alpha-encoding.md` §2.**
> There is no `0x1060`/`0x1064` register: `0x18` and `0x19` are ICD state
> slots, and the kernel folds enable (slot `0x0f`), function and reference
> into the single register `0x12e4` — bit 0, bits [3:1], bits [15:8].
> The 8-bit `(int)(ref*255 + 0.5)` conversion below is unchanged.

* `0x103c` = enable, 0/1.
* `0x1064` = func, the same 1..8 NEVER..ALWAYS enum (jump table rodata
  `0x36bca4`, bodies `0x1152f9`…`0x114b8f`).
* `0x1060` = reference, **8 bits**.  The conversion is explicit at
  `libGL` 0x114bd3:
  ```
  eax = 255 ; cvtsi2sd xmm1,rax
  cvtss2sd xmm0, [alpha_ref_float]
  mulsd    xmm0, xmm1
  addsd    xmm0, 0.5
  cvttsd2si eax, xmm0          ; = (int)(ref*255 + 0.5), stored as a byte
  ```
  `glAlphaFunc(GL_GREATER, 0.5f)` → 0x80.  **The 8-bit reading in
  `parhelia-gl-feature-map.md` is confirmed**, and the rounding is
  round-half-up, not truncation.  [CONFIRMED]

Alpha test needs no pipeline-key or vertex-format change — it is purely a
back-end test.

---

## 7. Raster operations (`GL_EXT_blend_logic_op`) — register 0x1030

`glLogicOp` (`libGL` @0x11d730) translates the GL enum to a 4-bit code and
stores it; the flush then writes it to bank-C index 0x0c:

```
114e63:  cmp BYTE PTR [blk+0x4c], 0    ; GL_COLOR_LOGIC_OP enabled?
114e69:  je  113c22                    ;  no  -> emit (idx 0x0c) = 0x0c
114e6f:  ebp = [blk+0x54]              ;  yes -> emit (idx 0x0c) = hw ROP
114e9d:  ... Reserve(key=0x0c) ; [rax]=0x0c ; [rax+4]=ebp
```

So **logic-op "enable" is not a bit**: when `GL_COLOR_LOGIC_OP` is off the
driver writes ROP = 0xC (`COPY`, i.e. "source only"), and the same register
takes the real ROP when it is on.  The reset value is also 0xC
(`libGL` @0x1108fb, @0x1bc3b7).  A from-scratch driver must do the same.

| GL logic op | GL enum | hw ROP | Boolean function |
|---|---|---|---|
| `GL_CLEAR` | 0x1500 | 0x0 | 0 |
| `GL_AND` | 0x1501 | 0x8 | s & d |
| `GL_AND_REVERSE` | 0x1502 | 0x4 | s & ~d |
| `GL_COPY` | 0x1503 | **0xC** | s |
| `GL_AND_INVERTED` | 0x1504 | 0x2 | ~s & d |
| `GL_NOOP` | 0x1505 | 0xA | d |
| `GL_XOR` | 0x1506 | 0x6 | s ^ d |
| `GL_OR` | 0x1507 | 0xE | s \| d |
| `GL_NOR` | 0x1508 | 0x1 | ~(s \| d) |
| `GL_EQUIV` | 0x1509 | 0x9 | ~(s ^ d) |
| `GL_INVERT` | 0x150A | **0x6 (vendor bug — should be 0x5)** | ~d |
| `GL_OR_REVERSE` | 0x150B | 0xD | s \| ~d |
| `GL_COPY_INVERTED` | 0x150C | 0x3 | ~s |
| `GL_OR_INVERTED` | 0x150D | 0xB | ~s \| d |
| `GL_NAND` | 0x150E | 0x7 | ~(s & d) |
| `GL_SET` | 0x150F | 0xF | 1 |

The code is the standard 4-bit truth table indexed by `(src,dst)` — the same
encoding as the MGA 2D `DWGCTL` ROP field — and equals `bitreverse4(GL_enum −
0x1500)` for 15 of the 16 entries.  [CONFIRMED]

The exception is real and worth knowing about: the jump table at rodata
`0x36bd8c` gives `GL_XOR` and `GL_INVERT` the **same** target (`0x11d74f`,
`mov eax,6`).  The vendor ICD therefore renders `glLogicOp(GL_INVERT)` as XOR.
The correct hardware code for `~d` is **0x5**; a new driver should emit 0x5 and
not copy the bug.  [CONFIRMED as a vendor behaviour; 0x5 is INFERRED from the
truth-table encoding, which the other 15 entries prove.]

Interaction with blending is **[OPEN]**: the ICD leaves `0x106c` (blend enable)
untouched when a logic op is enabled, so it does not enforce the GL rule that
logic op supersedes blending.  Whether the hardware prioritises one over the
other, or applies the ROP to the blend result, was not determined.  Safest
driver behaviour: disable blending while a non-COPY logic op is active.

---

## 8. Blending

### 8.1 Factors — the complete enum  [CONFIRMED]

The ICD has two translators, `libGL` @0x11d4b0 (**source**) and @0x11d5e0
(**destination**); each writes a code into a caller-supplied slot and returns
0 for an enum that is not legal in that position.

| GL factor | GL enum | hw code | legal as src | legal as dst |
|---|---|---|---|---|
| `GL_ZERO` | 0x0000 | **1** | yes | yes |
| `GL_ONE` | 0x0001 | **2** | yes | yes |
| `GL_SRC_COLOR` | 0x0300 | **3** | no | yes |
| `GL_ONE_MINUS_SRC_COLOR` | 0x0301 | **4** | no | yes |
| `GL_SRC_ALPHA` | 0x0302 | **5** | yes | yes |
| `GL_ONE_MINUS_SRC_ALPHA` | 0x0303 | **6** | yes | yes |
| `GL_DST_ALPHA` | 0x0304 | **7** | yes | yes |
| `GL_ONE_MINUS_DST_ALPHA` | 0x0305 | **8** | yes | yes |
| `GL_DST_COLOR` | 0x0306 | **9** | yes | no |
| `GL_ONE_MINUS_DST_COLOR` | 0x0307 | **0xa** | yes | no |
| `GL_SRC_ALPHA_SATURATE` | 0x0308 | **0xb** | yes | no |
| — | — | 0xc, 0xd | (unused by GL) | |
| `GL_CONSTANT_COLOR_EXT` | 0x8001 | **0xe** | yes | yes |
| `GL_ONE_MINUS_CONSTANT_COLOR_EXT` | 0x8002 | **0xf** | yes | yes |
| `GL_CONSTANT_ALPHA_EXT` | 0x8003 | **0x10** | yes | yes |
| `GL_ONE_MINUS_CONSTANT_ALPHA_EXT` | 0x8004 | **0x11** | yes | yes |

The src/dst legality split is the OpenGL 1.1 rule, not a hardware limit — the
hardware codes are one enum.  Note the gap at 0xc/0xd: those are
`D3DBLEND_BOTHSRCALPHA` / `D3DBLEND_BOTHINVSRCALPHA`, which OpenGL cannot
express.  **Codes 1..0xf are `D3DBLEND` verbatim** — the strongest single piece
of evidence that this is a fixed-function DX8-era blender.

This confirms and completes the table in `parhelia-gl-driver.md`.

### 8.2 Four independent factor registers (`GL_EXT_blend_func_separate`)  [CONFIRMED]

`glBlendFuncSeparateEXT(srcRGB, dstRGB, srcA, dstA)` at `libGL` 0x124820 stores
four independent hw codes; the flush routine sends them to four bank-5 indices:

| GL argument | ICD field | bank-5 index |
|---|---|---|
| src **alpha** | blk+0x18 | **0x09** |
| src **RGB** | blk+0x10 | **0x0a** |
| dst **alpha** | blk+0x28 | **0x0b** |
| dst **RGB** | blk+0x20 | **0x0c** |

i.e. the file is laid out as `(srcA, srcRGB, dstA, dstRGB)`.  `glBlendFunc`
simply writes the RGB value into both halves.  Separate RGB/alpha blending is
therefore native, with no penalty.

(The values previously attributed to `0x1024/0x1028/0x102c/0x1030` in
`parhelia-gl-feature-map.md` are these four, mis-addressed — see §1.3.
`0x1024` is really the shade mode and `0x1028` the line stipple.)

### 8.3 Blend equation — verifying the 0x12ac claim  [CONFIRMED]

`docs/parhelia-gl-driver.md` claims *"glBlendEquation | 0x12ac |
ADD/SUB/REVSUB/MIN/MAX = 1..5"*.  **Both halves are correct.**

Accept/reject is done in `glBlendEquation` (`libGL` @0x11d700):

```
11d700:  lea eax,[esi-0x8006]
11d706:  cmp eax,5 / ja reject
11d713:  rax = 1 << eax
11d716:  test al,0x37          ; accept mask = {0,1,2,4,5} -> 0x8006..0x800b except 0x8009
11d725:  [ctx+0x40] = esi      ; keep the GL enum
```

and the GL→hw mapping is done in the flush (jump table rodata `0x36bd04`,
bodies at `0x11522f`…):

| GL equation | GL enum | hw code |
|---|---|---|
| `GL_FUNC_ADD_EXT` | 0x8006 | **1** |
| `GL_FUNC_SUBTRACT_EXT` | 0x800A | **2** |
| `GL_FUNC_REVERSE_SUBTRACT_EXT` | 0x800B | **3** |
| `GL_MIN_EXT` | 0x8007 | **4** |
| `GL_MAX_EXT` | 0x8008 | **5** |
| anything else | — | 0x7fffffff (sentinel) |

The destination is bank-C index **0xab = MMIO 0x12ac** (`libGL` @0x114dfa,
shadow offset 0x2ac = 0xab*4).  So `GL_EXT_blend_minmax` and
`GL_EXT_blend_subtract` are hardware, one register, one write.

Independent cross-check [WIN]: `D3DBLENDOP` is ADD 1, SUBTRACT 2,
REVSUBTRACT 3, MIN 4, MAX 5 — identical.

Note the asymmetry with the previous section: unlike the *factors*, the blend
*equation* is a single register — there is no separate RGB/alpha equation
(no `GL_EXT_blend_equation_separate`, which matches the extension list).

### 8.4 Blend constant colour (`GL_EXT_blend_color`)  [CONFIRMED]

`glBlendColor` (`libGL` @0x128bc0) clamps each component to [0,1] (helper
0x12ce30) and stores **four floats** at `ctx+0x1058`; the flush sends them as a
**block record, key 0x35, 16 bytes, count 1** (`libGL` @0x11506b) — *not* as a
packed register write:

```
11506b:  Reserve(buf, key=0x35, elemsize=0x10, count=1)
115083:  copy 4 dwords from blk+0x30
```

So the constant colour is delivered to the hardware as four IEEE floats in the
range [0,1], in R,G,B,A order.  Its MMIO destination is decided below the ICD
and is **[OPEN]** together with the bank-5 base.  A driver that programs the
hardware directly should expect a 4×float (or the driver's own choice of a
packed form) constant-colour register adjacent to the blend factors.

### 8.5 What blending costs elsewhere

Enabling blending changes the ICD's PIPELINE-KEY from 0x11 to 0x51 and grows the
vertex stride from 24 to 28 bytes.  That is **not** a fragment-program change:
the extra 4 bytes are the per-vertex alpha that `glColor4f` now has to deliver,
and the pipeline key selects the *vertex/WARP* program that emits it.  No
fragment program is uploaded (§0).  [CONFIRMED]

---

## 9. Colour write mask and dithering

### 9.1 Colour mask = 0x12a0, 4 bits  [CONFIRMED]

`docs/parhelia-gl-driver.md` claims *"glColorMask | 0x12a0 | 4-bit RGBA
write-enable (bit0=R…bit3=A; 0xf=all)"*.  **Verified.**

`glColorMask` (`libGL` @0x124b00 → helper @0x11d4a0) stores four bytes at
`blk+0x68..0x6b` in R,G,B,A order.  The flush assembles them
(`libGL` @0x113c74):

```
113c81:  eax = [blk+0x48]        ; draw buffer; GL_NONE -> mask stays 0
113c88:  if blk[0x68] ebp |= 1   ; R
113c99:  if blk[0x69] ebp |= 2   ; G
113ca5:  if blk[0x6a] ebp |= 4   ; B
113cb1:  if blk[0x6b] ebp |= 8   ; A
113ce3:  Reserve(key=0x0c); [rax]=0xa8; [rax+4]=ebp    ; -> 0x12a0
```

and the reset value is 0xf (`libGL` @0x110a53).  Note the extra behaviour worth
copying: **`glDrawBuffer(GL_NONE)` is implemented by writing colour mask 0**,
not by a separate register.

The Windows-side name for this register is `PLNWTMASK` (it appears in the
vendor's module-parameter list as `reg3DForcePLNWMASK4FAA`, "Do not execute
PLNWTMASK hack in FAA mode") — see §10.

### 9.2 Dither = 0x1068  [CONFIRMED — new]

Not previously identified anywhere.  `glEnable/glDisable(GL_DITHER)` (enum
0x0BD0) writes a byte at `ctx+0x106c` (`libGL` @0x1256ff / @0x125982), which the
flush sends to bank-C index **0x1a = MMIO 0x1068** (`libGL` @0x114e1b):

```
125978:  cmp esi,0xbd0            ; GL_DITHER
125982:  mov BYTE PTR [rdi+0x106c],0
114e1b:  movzx ebp,BYTE PTR [r12+0x44]
114e4e:  [rax] = 0x1a             ; -> 0x1068
```

It is a plain 0/1 enable; no dither-matrix or pattern register exists.  GL's
default is enabled.

---

## 10. The one real ordering constraint: the FAA fragment buffer

Parhelia's antialiasing is **FAA-16x**, a sort-independent fragment buffer:
partially covered pixels are written to a side buffer and merged into the
framebuffer by an explicit **resolve** pass.  This is the only mechanism in the
whole stack that makes fragment-op state changes non-local.  [SRC, CONFIRMED]

From `kernel/src/MtxParhlParms.c`:

```c
/* Select the AA settings */                         reg3DAAMode                     = 0x52
/* threshold for sort-independent AA w/ frag buffer*/ reg3DThresholdAA               = 0x007FE000
/* ... */                                            reg3DFragSize                   = 0x3F800000
/* Force a different AA mode when destination alpha is enabled */
                                                     reg3DForceCompressedMask        = TRUE
/* Do not execute PLNWTMASK hack in FAA mode */      reg3DForcePLNWMASK4FAA          = TRUE
/* Send a FRAG_RESOLVE even if alphablending was enable when rendering the
 * scene and no fragments should be cumulated */     reg3DResolveAlphaBlendedFrags   = FALSE
/* Resolve due to stencil op changing */             reg3DResolveOnStencilOpChange   = TRUE
```

What a driver has to take from this:

* A **FRAG_RESOLVE** must be issued before the fragment buffer's contents can be
  observed, and the vendor issues one **when the stencil op changes**
  (`reg3DResolveOnStencilOpChange` defaults to TRUE).  Changing stencil ops
  mid-scene with FAA on without a resolve corrupts the merge.
* **Destination-alpha blending conflicts with the normal FAA coverage mask**
  (`reg3DForceCompressedMask` switches to a different AA mode when destination
  alpha is enabled) — the coverage mask and the destination alpha compete for
  the same storage.
* The **colour write mask (PLNWTMASK) has a workaround that must be suppressed
  in FAA mode**.
* Alpha-blended fragments are, by default, *not* accumulated into the fragment
  buffer.

**A first driver should leave FAA off** (`reg3DAAMode` disabled).  With FAA off
all fragment operations are ordinary immediate-mode back-end operations with no
resolve, no invalidation and no ordering constraint.  Every register in §2 can
be written at any point between draws.

---

## 11. Open items

1. **The absolute MMIO base of register bank 0x05.**  [OPEN]  This is the one
   thing a from-scratch driver still cannot do statically: it knows the four
   blend-factor registers are indices 0x09–0x0c of a 24-entry file (indices
   0x01–0x1c), and it knows their contents exactly, but not where the file is.
   Constraints established here: the DMA index space is 8 bits and covers
   exactly MMIO `0x1000`–`0x13fc` (MTXPARD @0x91a42); bank 0x0c occupies
   indices 0x07–0xab of it; therefore bank 0x05 must live somewhere in the
   remaining index space, most plausibly above 0xab.
   *How to close it in one shot:* with an mmiotrace or DMA-buffer dump active,
   run a program that does nothing but `glBlendFunc(GL_ONE, GL_ONE)` then
   `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` between two `glFinish`
   calls, and watch which four consecutive registers change 2,2,2,2 → 5,5,6,6.
   The same capture pins the blend-constant-colour register (key 0x35).
2. **Logic op versus blending priority** in the hardware (§7).  [OPEN]
3. **Bank-C indices 0x22–0x27** (fog colour/params in the ICD) overlap
   registers that MTXPARD's DMA interpreter treats as DMA control/trap
   (`0x108c`, `0x1090`, `0x109c`).  Fog is out of scope for this document and
   none of the fragment-op registers fall in that range, but do not assume the
   `0x1000+4i` rule there without a capture.
4. The **fragment-program-select register** (bank 5 index 0x14) inherits the
   bank-5 base problem; the value written is the program id and that part is
   confirmed by the capture.

---

## 12. Corrections to existing documents

**`docs/parhelia-gl-feature-map.md`**

* §3 "Blending": the table `0x1024/0x1028/0x102c/0x1030` = src RGB / src A /
  dst RGB / dst A is **wrong in the addresses**.  The four values are correct
  and the factor enum is correct, but they are indices 0x0a/0x09/0x0c/0x0b of
  **register bank 0x05**, not `0x1000+4i` of the main bank.  `0x1024` is the
  shade mode and `0x1028` is the line stipple.  (§1.3, §8.2)
* §3 / §5 item 5: *"`glEnable(GL_BLEND)` writes `0x1014 = 4` — a value outside
  the documented enum"*.  There is no such write.  Those eight bytes are the
  **record header `(key 0x05, count 4)`** introducing the four blend-factor
  pairs.  `0x1014` is not written by any op in the sweep.  (§1.3)
* §6 first bullet, the flagged ambiguity: *"`0x1030` (index 0x0c) is
  multiplexed or mis-sliced"*.  Resolved.  Every value of 1/2/3 attributed to
  `0x1030` by `scissor`, `cullface`, `fog`, `lighting`, `polymode` and the
  "second write" inside `blend_srcalpha` is a **record header of key 0x0c**
  whose count was 1, 2 or 3.  The real `0x1030` is the raster-op register and
  the ICD writes it exactly once per state change, with 0xC when
  `GL_COLOR_LOGIC_OP` is off.  (§1.3, §7)
* §3 "Clears": the constant `0x80808080` dword, flagged [AMBIGUOUS] as "a write
  mask or fill pattern", is the **accumulation-buffer clear value**
  `((v*0.5)+0.5)*255` per component.  Also, the clear mask has a fourth bit:
  bit2 = stencil, bit4 = accum.  (§4.2)
* §5 contradiction 9: *"`0x10d8` carries a two-float payload, so `0x10d0`–
  `0x10ec` is not eight uniform stencil registers"*.  It **is** eight uniform
  stencil registers.  The two-float payload is a **block record with key
  `0x36`** (line width + point size), not a write to `0x10d8`.  `0x10d8` is the
  stencil z-fail op.  (§1.1, §5)
* §3 "Stencil": `glStencilOp(KEEP,KEEP,REPLACE) → 0x10dc = 3` is upgraded from
  [INFERRED] to [CONFIRMED], and `0x10dc` identified as the **z-pass** op;
  `0x10d4` is fail and `0x10d8` z-fail.  The stencil compare function is
  `0x10e0`, **not** `0x10d4` as the section speculated.  (§5)
* §8 "One caveat to reconcile": *"the pair immediately before the program
  select is `0x1014 = 1`, and `0x1014` is not in the record-opcode enum"*.
  It is the **record header `(key 0x05, count 1)`**; the pair that follows,
  `(0x14, 3)`, is bank-5 index 0x14 = fragment-program select.  Consequently
  *"reg 0x1050 selects which uploaded fragment program a draw uses"* is wrong:
  the program-select register is bank-5 index 0x14, whose address is [OPEN].
  (§1.1, §11)

**`docs/parhelia-gl-driver.md`**

* *"glBlendEquation | 0x12ac | ADD/SUB/REVSUB/MIN/MAX = 1..5"* — **verified
  correct**, both the address and the enum, including which GL enum maps to
  which code (SUBTRACT 2 / REVERSE_SUBTRACT 3 / MIN 4 / MAX 5).  (§8.3)
* *"glColorMask | 0x12a0 | 4-bit RGBA write-enable"* — **verified correct**,
  bit0=R…bit3=A, reset 0xf.  (§9.1)
* *"glBlendFunc factors | bank 0x5 idx 0x09-0x0c | bank base = capture"* —
  **verified correct**, and now with the exact index↔component assignment
  (0x09 srcA, 0x0a srcRGB, 0x0b dstA, 0x0c dstRGB).  The base is still a
  capture item.  (§8.2)
* Blend-factor enum table — **verified and completed**; the src/dst legality
  split is added.  (§8.1)
* *"glCullFace | 0x1024 | (main bank)"* — wrong (as `parhelia-gl-feature-map.md`
  already noted).  `0x1024` is the **shade mode** (FLAT 1 / GOURAUD 2).  Cull
  face is `0x1058`.  (§2)
* *"Software-handled (no register): … polygon offset (depth bias in triangle
  setup)"* — contradicted by the vendor's own switch
  `regOGLPolygonOffset = TRUE`, *"(1) Enables Hardware PolygonOffset"*.  Wide
  lines (`regOGLWideLines`) and line stipple (`regOGLLineStipple`, hardware
  register `0x1028`) are likewise hardware.  [SRC]

---

## 13. Minimum programming sequence for a from-scratch driver

With FAA off, per state change, into the main register file:

```
0x101c = depth_test_enable                  0/1
0x105c = depth_func                         1..8   (NEVER..ALWAYS)
0x1038 = depth_write                        0/1

0x10d0 = stencil_enable                     0/1
0x10e0 = stencil_func                       1..8
0x10e4 = stencil_ref
0x10e8 = stencil_compare_mask
0x10ec = stencil_write_mask
0x10d4 = stencil_fail_op                    1..8   (KEEP..DECR_WRAP)
0x10d8 = stencil_zfail_op                   1..8
0x10dc = stencil_zpass_op                   1..8

0x103c = alpha_test_enable                  0/1
0x1064 = alpha_func                         1..8
0x1060 = alpha_ref                          (int)(ref*255 + 0.5)

0x106c = blend_enable                       0/1
0x12ac = blend_equation                     1..5   (ADD,SUB,REVSUB,MIN,MAX)
bank5[0x0a] = src_rgb_factor                1..0x11
bank5[0x09] = src_alpha_factor              1..0x11
bank5[0x0c] = dst_rgb_factor                1..0x11
bank5[0x0b] = dst_alpha_factor              1..0x11
<blend constant colour>                     4 floats, clamped to [0,1]

0x1030 = rop                                0xC when logic op is off
0x12a0 = colour_write_mask                  bit0 R .. bit3 A, 0 for DrawBuffer(NONE)
0x1068 = dither_enable                      0/1
```

and the clear is a block operation, not a register sequence:
`mask (1=colour, 2=depth, 4=stencil, 0x10=accum), ARGB8888 colour, float depth,
int stencil, ARGB8888 accum, x, y, w, h`.
