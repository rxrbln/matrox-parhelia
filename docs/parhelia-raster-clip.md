# Parhelia clipping and raster setup

What the hardware does between "transformed vertex" and "fragment": scissor,
viewport, depth range, guard band, user clip planes, frustum clipping, culling,
polygon mode/offset, fill rules, point size and line width — reverse-engineered
from the vendor user space for a from-scratch open driver.

Sources and tags:
**[ICD]** = `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0`
(disasm `scratchpad/libGL64.asm`; the 32-bit build `opengl6/libGL.so.1.3.0` /
`scratchpad/libGL.asm` is cross-checked where it matters).
**[KMOD]** = `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`
(`scratchpad/mtxk_r.asm`) and its Linux glue source under `kernel/src/`.
**[XSRV]** = `xserver/7_4_0/mtx_drv.so` (`scratchpad/mtx_drv.asm`).
**[WIN]** = `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`
(`scratchpad/pard.asm`) — an independent implementation of the same silicon.
**[HW]** = the live capture `captures/glsweep-0528-raw.txt` on a PCI 0x0528.

Every claim is tagged **CONFIRMED** (two independent sources agree, or the
capture proves it) or **INFERRED** (best reading, self-consistent, unproven).

---

## 0. The scissor conflict — RESOLVED, and neither side was right

The standing conflict (docs/parhelia-command-construction.md §"Unresolved
conflicts", docs/parhelia-gl-feature-map.md §5 item 6) was:

* the ICD disassembly emits `[2][0x2121a1a0][(f3<<16)|f0][(f1<<16)|f2]` into
  registers 0x1280/0x1284 with values clamped to ±1024 — read as a
  "guard-band scissor";
* the live capture shows `glScissor(32,32,64,64)` as a **4-dword pixel rect**
  `(32, 160, 64, 64)` and never touches 0x1280/0x1284.

Three findings settle it.

**(a) The pixel rect is the scissor. CONFIRMED.**
`glScissor` reaches the hardware as a record `[0x12][1][x][y][w][h]`, four
`int32` in window pixels with Y measured from the bottom, emitted by
`libGL64.asm:0x11115f` (and `0x10f264`, `0x115410`, `0x1bbefe`). The Y-flip is
explicit in the code — `0x1111e0: mov edx,[rbx+0xd4]` (drawable height) then
`0x1111eb: sub r8d,edi; sub r8d,r10d` = `H - h - y`, stored to payload word 1 at
`0x111252` — and reproduces the captured `(32, 160, 64, 64)` exactly for a
256-pixel-high window. With `GL_SCISSOR_TEST` disabled the same record carries
the full drawable (`0x11163c`: `x=0, y=0, w=[surf+0xd0], h=[surf+0xd4]`), which
is what the capture shows.

**(b) The `0x2121a1a0` packet is not a scissor at all — it is a packed
four-component colour. CONFIRMED, by [WIN].**
`MTXPARD.dll` emits the *identical* tag and packing at `pard.asm:0x42c86`, and
there the source is unambiguous: an ARGB8888 texel is unpacked and each
component scaled `c * 0x3ff / 255` (`imul edx,edx,0x3ff` then the `0x80808081`
divide-by-255 idiom, `0x42bf9`, `0x42c10`, `0x42c28`, `0x42c3e`), giving

```
reg 0x1280 = (A10 << 16) | R10
reg 0x1284 = (G10 << 16) | B10        each component 10-bit
```

The ICD's version is the same thing in floating point: four floats at
`ctx+0xa068`, each `clamp(f * 1024.0f, -1024.0f, +1023.0f)` truncated to int
(`0x1c0300`–`0x1c0443`; the 1024.0f multiplier is `.rodata:0x36f8a4`, verified
in the file), packed `(f3<<16)|f0` and `(f1<<16)|f2` — i.e. `(A,R)` and `(G,B)`.
The ±1024 "guard band" was simply the **signed Q1.10 range of a colour
constant**, matching the Q1.10 constant format already documented in
docs/parhelia-fragment-isa.md. Note the value can be negative, which a scissor
coordinate in this encoding could never usefully be, and cannot exceed ±1.0
NDC — useless as a guard band.

**(c) In this driver the packet is dead code. CONFIRMED.**
Its emission is gated on the byte `ctx+0xa038` (64-bit build) /`ctx+0xa020`
(32-bit build). Every write to that byte in either binary stores **0**:
`0x1c0097`, `0x1c01a7` (constructors), `0x1c0469`, `0x1c4f14`, `0x1c51a9`,
`0x1c5378`, `0x1c54f5` (after emission), and `0x1c06ed` which sets it from
`ctx+0xa039`, itself only ever written 0 (`0x1c009e`, `0x1c01ae`). Same picture
at `0x1577cf`, `0x1578bf`, `0x157ac4`, `0x157e33` in the 32-bit build. The flag
is never set, so the packet is never emitted — exactly as the capture shows.

**The hardware scissor registers are 0x1328 / 0x132C.** CONFIRMED from two
independent code paths that are not the ICD:

* [KMOD] `mtxk_r.asm:0x3c64b` builds the 3D context-init DMA packet
  `[4][0xbdcbcac0][…]` — index bytes `c0, ca, cb, bd` = regs **0x1300 (DWGCTL),
  0x1328, 0x132C, 0x12F4** — writing `0x0fff0fff` and `0` to the scissor pair
  and `31.0f` to 0x12F4. A second variant at `0x3c92a`
  (`[3][0x21cbcac0]`) omits 0x12F4 on parts without a guard band.
* [WIN] `pard.asm:0x2c3d9` emits `[0xc2c1cbca]` = regs 0x1328, 0x132C, 0x1304,
  0x1308 with `0`, `0x0fff0fff`, `0`, `0` — the same full-screen default with
  the two values the other way round.
* [XSRV] `ParhlXAASetClippingRectangle` (`mtx_drv.so:0x535e0`) packs
  `(top<<16)|left` and `(bottom<<16)|right` and sets DWGCTL bit23 (`0x800000`) —
  the 2D clip enable, hardware-proven in docs/parhelia-2d-accel-complete.md.

So the scissor is one pixel rectangle, two corner registers packed `(Y<<16)|X`,
gated by DWGCTL bit23. Which of 0x1328/0x132C is the min corner and which the
max is **not** settled — [KMOD] and [WIN] write the `0x0fff0fff` / `0` pair in
opposite orders. Both of those are "clip disabled" paths (DWGCTL bit23 clear),
so neither is authoritative; follow the XAA order (min first, at 0x1328) and
verify on hardware.

**Conversion the open driver needs.** The ICD hands the kernel
`(x, H−y−h, w, h)`. For a top-down render target that is already a top-left
origin box, so

```
SCISSOR_MIN = ((H-y-h) << 16) | x
SCISSOR_MAX = ((H-y)   << 16) | (x + w)
DWGCTL |= 0x800000
```

[INFERRED] — the arithmetic is forced, the register order is the open item
above.

---

## 1. The evidence base: the record grammar (corrects several older readings)

Everything below is stated in terms of the ICD → kernel record stream, so the
grammar has to be right. It is **not** a flat `(index, value)` pair stream, and
it is not the ad-hoc mix of "block opcodes and pairs" that
docs/parhelia-gl-feature-map.md §2 describes. It is uniform:

```
record := [u32 opcode][u32 count][count * elemsize bytes]      elemsize = f(opcode)
```

Allocated by **`libGL64.asm:0x1c9c50(buffer, opcode, elemsize, count)`**, which
writes the two header dwords and returns the payload pointer; closed by
`0x1c9ad0(buffer)`. Adjacent records with the same opcode are coalesced by
incrementing `count`. CONFIRMED — enumerating all 28 call sites in the 64-bit
ICD gives:

| opcode | elemsize | payload | meaning |
|---|---|---|---|
| 0x05 | 8 | `[u32 index][u32 value]` | register list, **bank B** |
| 0x06 | 8 | `[u16 unit][u16 index][u32 value]` | per-unit register list |
| 0x07 | 0x18 | 6 dwords | pipeline/shader key |
| 0x08 | 0x20 | 8 dwords | vertex-format commit |
| 0x0a | 0x48 | | |
| 0x0c | 8 | `[u32 index][u32 value]` | register list, **bank A (main)** |
| 0x0d | 0x20 | 8 dwords | render-target / surface block |
| 0x0e | 8 | `[u16 unit][u16 index][u32 value]` | per-unit register list |
| **0x12** | **0x10** | `x, y, w, h` (int32) | **SCISSOR** |
| 0x15 | 0x24 | 9 dwords | CLEAR |
| 0x19 | 0x20 | | |
| 0x1a | 0x18 | | |
| 0x1b | 0x28 | 10 dwords | DRAW |
| 0x1c | 0xc8 | | |
| 0x20 | 0x44 | `[u32 slot][16 floats]` | matrix / VS constant |
| 0x21 | 8 / 0x70 | | light / material |
| 0x22 | 0x48 | | |
| 0x23 | 0x10 | | |
| 0x24 | 8 | | |
| **0x25** | **0x14** | `[u32 i][float a,b,c,d]` ×6 | **USER CLIP PLANES** (§7) |
| **0x26** | **0x10** | `x, y, w, h` (int32) | **VIEWPORT** |
| **0x27** | **8** | 2 floats | **DEPTH RANGE (scale, bias)** |
| 0x35 | 0x10 | | |
| **0x36** | **8** | 2 floats | **eye-space near, far** (§5) |
| 0x37 | 0x10 | | |
| 0x41–0x43 | 8 | | |
| 0x44 | 0x30 | | |
| 0x45 / 0x46 | 0x670 / 0x678 | | fragment-program upload |
| 0x48 | 0x18 | | |
| 0x49 | 0x14 | | |
| 0x50 | 0x20 | | |
| 0x51 | 8 | | |

Self-check against the capture's priming frame (`captures/glsweep-0528-raw.txt`,
region 2 of `clear_color`): the stream frames exactly as
`(?,1)+4dw rect | (0x0d,1)+32B | (0x15,1)+36B CLEAR | (0x0d,1)+32B |
(0x35,1)+16B | (0x0c,0x24)+36 register pairs`, and those 36 pairs are precisely
`0x17=2, 0x26=1.0f, 0x9b=0, 0xa6=2048.0f` followed by indices `0x40..0x5f` all
`0xffffffff` — 4 + 32 = 36. CONFIRMED.

Three consequences, all of which retire "unresolved" entries elsewhere:

1. **What earlier decodes read as `(0x0c, 1) → reg 0x1030 = 1` is a record
   header, not a register write.** In the captured `scissor` op the dwords at
   region-2 `0x18..0x24` are `0x0c, 1, 0x34, 0` = "bank-A list, 1 entry: index
   0x34 = 0" = reg 0x10D0 = 0, the stencil disable left over from the previous
   op. There is no `0x1030` write there. This dissolves
   docs/parhelia-gl-feature-map.md §6 "0x1030 is multiplexed or mis-sliced".
2. **`0x1014 = 4` and `0x1014 = 1` in the capture are record headers too** —
   `(0x05, 4)` is a *four-entry* bank-B register list, which is exactly what
   `glBlendFunc` emits (indices 0x09..0x0C), and `(0x05, 1)` a one-entry list.
   `0x1014` was never written. This dissolves feature-map contradiction 5.
3. **`0x10d8` really is stencil.** The two-float payload that looked like it sat
   at `0x10d8` is the `(0x36,1)` record in the *opcode* namespace; the bank-A
   index `0x36` → 0x10D8 is written from the stencil state at `0x11475e` /
   `0x1bbe46`. This dissolves feature-map contradiction 9.

**Two banks, two address spaces.** Bank A (opcode 0x0c) and bank B (opcode 0x05)
are *not* the same register file: bank-A index `0x1C` is fog enable
(capture-confirmed), while bank-B index `0x1C` is the back-face polygon mode
(§10).

`reg = 0x1000 + index*4` is CONFIRMED for the **DMA tag byte** namespace
(0x21 = end, 0x20 = pad → 0x1080, 0x24/0x25 → 0x1090/0x1094 fence, 0x2C →
0x10B0 primitive, 0xCA/0xCB → 0x1328/0x132C scissor, 0xBD → 0x12F4), and
independently so by [WIN]: MTXPARD's register-packet decoder at
`pard.asm:0x91840` unpacks the same stream and computes the offset literally —

```
918ed  and edi,0xff                ; index = low tag byte
918f5  lea edi,[edi*4+0x1000]      ; offset = 0x1000 + index*4
9192c  shr edx,0x8                 ; next tag byte
```

storing at `[regAperture + offset]` (`0x1b703`). Its `[opcode][count]` 8-byte
record header matches the ICD's too.

It is only **assumed** for bank A and bank B indices, which are the ICD's own
shadow indices. Treat every "0x10xx/0x12xx" address in this document that comes
from a bank index as INFERRED.

---

## 2. Scissor

**Path.** `glScissor(x,y,w,h)` → GL state → record `(0x12, 1)` → kernel → the
0x1328/0x132C pixel scissor + DWGCTL bit23. There is no second, "guard-band"
scissor path; see §0.

**Record payload** (emitter `0x11115f`, CONFIRMED):

| dword | value |
|---|---|
| 0 | `x`, optionally `/3` in triple-head mode |
| 1 | `drawableHeight - y - h` (Y from bottom) |
| 2 | `w`, optionally `/3` in triple-head mode |
| 3 | `h` |

Clamping before emission (`0x11119f`–`0x1111de`): the box is trimmed against the
drawable, `if (x+w > surfW) w -= (x+w-surfW)` and likewise for `y+h` against
`surfH`, using the drawable extents at `[surface+0x30]/[+0x34]`. Scissor
disabled emits the whole drawable.

**Triple-head.** When `[primobj+8] != 0` the ICD divides `x` and `w` by 3 with
the `0x55555556` reciprocal idiom (`0x111207`–`0x111244`, mirrored for the
viewport at `0x114a10`). That is the `regOGLTripleHead` switch
(`kernel/src/MtxParhlParms.c`, default FALSE); an open driver ignores it.

**Interaction with the viewport.** They are independent records; the hardware
intersects. The ICD does not fold the viewport into the scissor.

---

## 3. Viewport

**The transform is done by the hardware, not the CPU and not the vertex
program.** CONFIRMED three ways: the vertex ring holds raw object-space
`glVertex` values for every hardware draw [HW]; the ICD emits the viewport as
four integers and never packs a scale/bias vec4 (all eight `opcode 0x20`
constant uploads take matrices from the GL matrix stack — slot 1 = modelview,
slot 3 = projection, slots 0x10–0x13 = texture matrices, and nothing else); and
`glDepthRange` is delivered as a separate scale/bias record rather than being
multiplied into anything.

**State object** `state+0xe68` (CONFIRMED):

| offset | field |
|---|---|
| +0x00 | `x` (int, window px) |
| +0x04 | `y`, **already Y-flipped** |
| +0x08 | `w` |
| +0x0c | `h` |
| +0x10 | `zNear` (float, clamped to [0,1]) |
| +0x14 | `zFar` (float, clamped to [0,1]) |
| +0x18 | attribute-stack depth; +0x20 the 16-deep stack |
| +0x30 | drawable width |
| +0x34 | drawable height |

`glViewport` = `0x124450`; it rejects negative `w`/`h` with `GL_INVALID_VALUE`,
sets dirty bit `0x200`, and calls `SetViewport` at `0x188190`, which clamps
`w,h` to `[0, 4096]`, `x` to `[-4095, 4096]`, `y` to `[-4096, 4095]`, trims
`x+w` and `y+h` to 4096, and stores the flip:

```
188229: mov eax,[rbx+0x34]   ; drawable height
188234: sub eax,esi          ; - h
188236: sub eax,edx          ; - y
188238: mov [rbx+4],eax      ; vp.y = H - y - h
```

`glGetIntegerv(GL_VIEWPORT)` un-flips on read-back (`0x1882b0`, `0x13a529`), so
the flip is genuinely a storage convention. **Maximum viewport extent is 4096**,
consistent with the `0x0fff0fff` full-screen scissor the kernel and the Windows
driver both program. CONFIRMED.

**Record `(0x26, 1)`** is a verbatim copy of `vp[0..3]` — emitters `0x114922`
(main state validation, under dirty bit 0x200), `0x1107ad` (glAccum) and
`0x1bc016` (blit/DrawPixels). Four `int32`, window pixels, Y from bottom.
CONFIRMED, and it reproduces the captured `glViewport(0,0,128,128)` →
`0, 128, 128, 128` in a 256-high window.

**Register 0x1298 = 2048.0f is not a viewport scale.** It is
`GL_POINT_SIZE_MAX`. Bank-A index `0xa6` is written from one place only,
`0x1143f9`, inside the point-parameter block, and the point-state constructor at
`0x1cbb30` initialises `state+0xad0`: `+0x04 = 1.0` (point size), `+0x08 = 0.0`
(min), `+0x0c = 2048.0f` (max), `+0x10..0x18 = (1,0,0)` (distance attenuation),
`+0x1c = 1.0` (fade threshold). The ICD also answers
`glGet(GL_POINT_SIZE_RANGE)` with `1.0, 2048.0` (`0x13ed79`). CONFIRMED — this
corrects docs/parhelia-3d-state-complete.md ("2048.0f viewport/scale") and
docs/parhelia-gpu-access-map.md.

The only viewport-derived register writes in the whole ICD are the point
distance-attenuation coefficients, each multiplied by the viewport height
twice: bank-A `0x9e/0x9f/0xa0` = `a·h²`, `b·h²`, `c·h²` (`0x1123c2`–`0x112403`,
emitted at `0x11242d`, `0x112476`, `0x1124bf`). INFERRED as
0x1278/0x127c/0x1280.

**Software fallback path only.** When the ICD rasterises on the CPU it caches
the transform at `[prim+0x32c..0x340]` = `(float)w, (float)h, far-near,
(float)x, (float)y+h, near` (`0xf0e90`) and applies
`win = ndc/w * scale + bias` per vertex (`0xe77e0`, `0xea4b7`). Note the two
variants differ in the Y sign — `0xe7860` adds `vp.y` with no flip while
`0xea509` negates `h` and adds `vp.y+h` — so read that code as
render-target-orientation-specific, not as the hardware's rule.

---

## 4. Depth range

`glDepthRange` = `0x1244b0`. Both arguments go through `clamp01` (`0x12ce30`),
are stored as floats at `vp+0x10` / `vp+0x14`, and set the **same** dirty bit as
`glViewport` (`0x124503: or DWORD PTR [rbx+0x10],0x200`).

Emitter `0x10f3f0` (a dedicated function; a second inline copy at `0xf442c`):

```
10f41d: xmm0 = far
10f427: xmm0 -= near
10f434: xmm0 *= 0.5f            ; .rodata 0x36acf4 = 0x3f000000
10f438: payload[0] = (far-near)*0.5      <- SCALE
10f43c: xmm0 = near
10f441: xmm0 += far
10f446: xmm0 *= 0.5f
10f44a: payload[1] = (near+far)*0.5      <- BIAS
```

So `(0x27, 1)` = **`((f−n)/2, (n+f)/2)`**, i.e. the hardware computes
`z_window = z_ndc * scale + bias` from a clip-space `z_ndc ∈ [−1,1]`. Default
`(0,1)` gives `(0.5, 0.5)`, exactly as captured. CONFIRMED — and it settles that
the form is *not* `((f−n), n)`, which also yields `(1,0)`, not `(0.5,0.5)`.

**Composition with the viewport:** none, on the hardware path. The viewport rect
and the depth scale/bias are two records driven by one dirty bit, and the
hardware applies the x/y viewport map and the z map itself. On the CPU
rasteriser path they are composed into the cached `[prim+0x32c..0x340]` block
above.

---

## 5. The `(0x36,1)` record is the eye-space near/far pair, not point/line size

docs/parhelia-gl-feature-map.md guesses `(0x36,1)` = "line width + point size
(1.0, 1.0)". It is not. Emitter `0x114ec3`; the payload is recovered **from the
projection matrix**, which the ICD copies to the stack at `0x114ed5`–`0x114f57`:

```
if (m[11] == -1.0f)                       ; 0x114f5b vs .rodata 0x369928
    payload[0] = m[14] / (m[10] - 1.0f)
    payload[1] = payload[0] * (m[10]-1.0f) / (m[10]+1.0f)
else
    payload[0] = payload[1] = 1.0f        ; 0x114fd5
```

For the standard GL perspective matrix `m[10] = −(f+n)/(f−n)`,
`m[14] = −2fn/(f−n)`, this evaluates to `payload = (n, f)` — the **eye-space
near and far plane distances**. The `(1.0, 1.0)` seen in the capture is the
`else` branch, because the sweep never calls `glFrustum`. CONFIRMED by algebra
plus the `m[11] == −1` perspective test.

The hardware needs these for W-buffering / perspective-correct depth and for
eye-space fog. An open driver that only ever uses an orthographic projection can
emit `(1.0, 1.0)`.

---

## 6. The guard band

**Register 0x12F4, an IEEE float.** CONFIRMED from two independent binaries:

* [KMOD] `mtxk_r.asm:0x3c675` writes `0x41f80000` = **31.0f** to tag byte `0xbd`
  in the 3D context-init packet, immediately after the scissor pair — and the
  alternative packet at `0x3c92a` omits index `0xbd` entirely when the
  capability test `test DWORD PTR [dev+0xab50], 0x7fc00` fails. So the guard
  band is a per-revision feature.
* [WIN] `pard.asm:0x2c3f0` writes the same `[0x212121bd][31.0f]`, and
  `pard.asm:0x16fe01`–`0x16fe31` computes the base value from the **active
  texture-stage count**:

```
ebx = stageCount                    ; [ctx+0x128]
edx = (2 < stageCount) ? -1 : 0     ; cmp 2,ebx / sbb edx,edx
edx &= 0xff800000
edx += 0x42000000                   ; -> 0x41800000 = 16.0f  or  0x42000000 = 32.0f
[ctx+0x780] = edx
```

and a later fixup (`pard.asm:0x2cf90`) turns 32.0 → 31.0 and 16.0 → 15.0 before
programming the register.

So: **guard band = 32 with ≤2 texture stages, 16 with more, programmed one unit
smaller (31 / 15)**. CONFIRMED as values and as the stage dependency. The *unit*
— NDC multiples versus something else — is INFERRED; the shrink-with-stage-count
behaviour is the classic symptom of a setup-precision-limited guard band (fewer
bits left for the position when more attributes are iterated).

**Choosing guard band versus true clipping is a driver-level policy, driven by
registry switches, and it is documented in the vendor's own source**
(`kernel/src/MtxParhlParms.c`) — CONFIRMED, this is vendor comment text:

```
/* It controls the guard band clipping attribute :
   (0) Disables and the DirectX caps return no guardband : the TnL Module will clip on VP
   (1) Disables (the TnLModule will clip on VP) but the Guardband are left for the directX caps
   (2) Enables the GB clipping (default) */
MTX_MODULE_PARM_DECL(reg3DGuardBandClipping, 2);

/* It controls the clip reject functionnality
   (FALSE) Disabled : Default
   (TRUE) Enabled : Each primitives requiring clipping will be rejected. */
MTX_MODULE_PARM_DECL(reg3DClipReject, FALSE);

/* Clipping Type switch
    0 : Use default clipping type for each board
    1 : Force Parhelia Clipping on all boards
    2 : Force Sundog clipping for all boards. */
MTX_MODULE_PARM_DECL(reg3DClippingType, 0x00000000);
```

Reading: with the guard band on (the default), the transform/lighting unit does
**not** clip on the viewport — the hardware accepts geometry out to the guard
band and the raster scissor does the rest. With it off, "the TnL Module will
clip on VP", i.e. real geometric clipping. `reg3DClippingType` says there are
two hardware clipping behaviours in this family, "Parhelia" and "Sundog"
(the later part), selected per board.

The registry names are compiled into [KMOD] and [XSRV] (`strings`: `CullNoneMode`,
`GuardBandClipping`, `ClipReject`, `ClippingType`, `PolygonOffset`, `WideLines`);
`mtx_drv.asm:0x6185e` shows `GuardBandClipping` registered as a
`RegistrySwitch<unsigned int>` with default 2, `CullNoneMode` (`0x61759`) with
default 1.

**The software mirror: a per-axis clip-volume scale, not a fixed limit.**
CONFIRMED. `GLPrimitiveSoftwareClipDispatch::ValidateState` (`0xea660`) copies a
vec3 from `GLPrimitiveSoftware+0x34c` into every clip functor through virtual
slot +0x18 (`0xef340`), landing at the functor's `this+0x8..+0x10`. Those three
floats are the `gx, gy, gz` used by the outcode generator in §8. They are
produced by the viewport-update function `0xf0e90`:

```
f0f12  ax = |[dev+0xbc8c]| ; ay = |[dev+0xbc90]|        ; drawable origin
f0f33  gz = 1.0f                                        ; NO depth guard band
f0f39  gx = ([prim+0x344] - ax) / (float)vp.width  ; if <= 0 -> 1.0f
f0f6d  gy = ([prim+0x348] - ay) / (float)vp.height ; if <= 0 -> 1.0f
```

and the two numerators come from a device-capability query in the
`GLPrimitiveSoftware` constructor (`0xf0270`): `*[0x65cd78]` singleton, virtual
slot +0x48 returns a float rect, and
`prim+0x344 = rect[+0xc] − rect[+0x4]` (guard-band width in pixels),
`prim+0x348 = rect[+0x10] − rect[+0x8]` (height).

So the driver's guard band is "how many pixels wide the hardware's usable
raster area is, divided by the viewport size", clamped to at least 1.0 (exact
frustum). **`gz` is always 1.0: there is no depth guard band, near and far are
always clipped exactly.** The concrete pixel extents were not pinned — they come
from that virtual caps call — so do **not** assume ±2048.

**Not the guard band:** the ±1024 clamp in the ICD (§0b), and `0x1298`
(§3). The neighbouring limit constants the kernel programs in the same init
block are, in tag order (`mtxk_r.asm` `0xbab1b0b9`, `0xb4b3b2b5`, `0xbfc5c6c9`
in [WIN] `pard.asm:0x2c38e`–`0x2c3c3`): `0x12E4=0`, `0x12C0=0xffffffff`,
`0x12C4=0x7fffffff`, `0x12E8=0`, `0x12D4=0`, `0x12C8=0`, `0x12CC=0xffff`,
`0x12D0=0x1ff`, `0x1324=0xffffffff`. Their individual meanings are unresolved;
`0x7fffffff` is the ICD's "feature disabled" sentinel elsewhere (§10), so
`0x12C0/0x12C4` read as max/disabled limits. INFERRED.

---

## 7. User clip planes and `GL_EXT_clip_volume_hint`

**Six planes.** `GL_MAX_CLIP_PLANES` (0x0D32) returns 6 from all four getters
(`0x13d48f` int, `0x136986` bool, `0x140848` float `6.0f`, `0x139e25` double),
and every range check is `lea eax,[enum-0x3000]; cmp eax,0x5` — `0x1276d1`
(glIsEnabled), `0x1560e0` (glEnable/Disable), `0x156160` (glGetClipPlane),
`0x155fcf` (glClipPlane), `0x17e997` (display-list compile). CONFIRMED.

**`glClipPlane` transforms on the CPU, exactly per spec.** `glClipPlane` =
vtable slot +0x80 → `0x123710` → `0x155fa0(ctx+0xf0, plane, GLdouble*)`:

```
156043  4 x cvtsd2ss                     ; double -> float plane[4]
156047  rsi = [ctx+0xf0+0x90]            ; = ctx+0x180 = current modelview
156094  call 1c6cc0(tmp, modelview)      ; 4x4 inverse (adjugate 1c69c0 / det 1c68d0)
15609f  call 1c6d40(tmpT, tmp)           ; transpose
1560ad  call 1c6630(out, plane, tmpT)    ; mat * vec, column-major
1560c1  store 4 floats at ctx+0xf8 + i*16
1560c6  or [ctx+0x330],0x40 ; or [ctx+0x334],0x40   ; dirty bit 0x40
```

i.e. `plane_eye = (M_modelview^-1)^T · plane_object`, single precision.
`glGetClipPlane` (`0x156160`) returns the stored eye-space plane without
inverting — spec-conformant. CONFIRMED.

**Context layout** (CONFIRMED):

| address | contents |
|---|---|
| `ctx+0x0f0` | transform/clip state sub-object |
| `ctx+0x0f8 + i*16` | plane *i*, four floats, **eye space**, i = 0..5 |
| `ctx+0x158 + i` | plane *i* enable byte |
| `ctx+0x180` | pointer to the current modelview matrix |
| `ctx+0x330` / `+0x334` | dirty masks; **bit 0x40 = clip planes** |

**They are uploaded to the hardware, as a dedicated record.** State validation
under dirty bit 0x40 (`0x1118d0` → `0x112a48`, second identical copy at
`0x112d68` → `0x112f1e`) emits

```
record (0x25, 6), elemsize 0x14 = 20 bytes
    payload[i] = { u32 planeIndex, float a, float b, float c, float d }   i = 0..5
then, if the enable mask changed:
record (0x0c, 1) = bank-A index 0x98, value = OR of (1<<i) over the enable bytes
```

(`0x112a4c: mov ecx,0x6`, `0x112a51: mov edx,0x14`, `0x112a56: mov esi,0x25`;
mask built at `0x112aa3`–`0x112ab9`, emitted at `0x112ac2`.) All six are always
sent; the mask selects. Planes are in **eye space**, so the hardware applies
them after the modelview and before/with the projection. CONFIRMED as the
record; the consumer of record 0x25 and of bank-A index 0x98 (→ 0x1260 under
the bank-A address assumption) was not traced, so **do not treat 0x25 as an MMIO
register number** — it is a record opcode.

`GLVSClipPlaneDesc` (RTTI `.rodata:0x36d060`, ctor `0x168930`) additionally binds
each enabled plane's vec4 as a **vertex-shader bound parameter**
(`0x17e9a0: lea eax,[r13-0x3000]; shl rax,4; lea rdx,[rax+rbx+8]`), which is how
user clipping reaches the WARP program on the hardware-T&L-VS path. CONFIRMED
as the binding; the parameter "kind" value `0x87c2` returned by `0x1696b0` is
UNRESOLVED.

**There is also a software user-clip pass**, `0xe9a00`, called first in every
`Tri_*` clipper (`0xee1e9`): it collects the enabled planes from
`state+0xf8`/`+0x158`, returns immediately if none is enabled, takes the
**eye-space** vertex position from vertex-stream descriptor `+0x3a8`, evaluates
`dot4(plane, eyePos)` per vertex (`0x1b9d30`) and splits edges with `0xe93c0`.
So user clipping is a real geometric clip on the software path, and a shader
binding on the hardware path. CONFIRMED.

**`GL_EXT_clip_volume_hint` is a stub.** `glHint` = vtable +0x158 → `0x123b90` →
`0x1be7f0(ctx+0xa70, target, mode)`, a pure store with no mode validation:

| target | field |
|---|---|
| `GL_FOG_HINT` 0x0C54 | ctx+0xa70 |
| `GL_PERSPECTIVE_CORRECTION_HINT` 0x0C52 | ctx+0xa74 |
| `GL_POINT_SMOOTH_HINT` 0x0C50 | ctx+0xa78 |
| `GL_LINE_SMOOTH_HINT` 0x0C51 | ctx+0xa7c |
| `GL_POLYGON_SMOOTH_HINT` 0x0C53 | ctx+0xa80 |
| **`GL_CLIP_VOLUME_CLIPPING_HINT_EXT` 0x80F0** | **ctx+0xa84** |
| `GL_TEXTURE_COMPRESSION_HINT_ARB` 0x84EF | ctx+0xa88 |

All seven default to `GL_DONT_CARE` (`0x1be900`) and get a 16-level attribute
stack. An exhaustive scan of 32-bit accesses to `ctx+0xa84` finds **only** the
`glHint` write, the four `glGet` read-backs (`0x136cf9`, `0x13a25f`, `0x13d826`,
`0x140c5a`) and the push/pop-attrib copies. **No draw-path, clipper or
state-emission code reads it**, so `GL_FASTEST` does not let this driver skip
clipping. CONFIRMED — the extension is advertised and its state round-trips, and
that is all it does.

For completeness, the full advertised extension string (`glGetString` impl
`0x120d40`, string at file offset 0x46e0e0) contains only three other
clip/raster-relevant entries: `GL_Autodesk_valid_back_buffer_hint`,
`GL_EXT_polygon_offset` and `GL_MTX_fragment_shader`. `GL_EXT_cull_vertex` is
**not** advertised even though `glCullParameterdvEXT`/`fvEXT` are exported
(`0x1ac1d0`/`0x1ac1f0`).

---

## 8. Frustum clipping: hardware versus driver

**The ICD carries a complete software clipper.** The C++ RTTI names in
`libGL.so.1.3.0` (`strings -t x`, 0x36a6e0–0x36ab80) list the whole family, and
the source file is named:

```
GLPrimitiveSoftwareClipping.cpp
GLPrimitiveSoftwareClip_Interface
GLPrimitiveSoftwareClip_Null
GLPrimitiveSoftwareClip_Point_to_Point / _Point_to_Sizedpoint
GLPrimitiveSoftwareClip_Line_to_Line   / _Line_to_Wideline
GLPrimitiveSoftwareClip_Tri_to_Tri / _Tri_to_Line / _Tri_to_Wideline
                                   / _Tri_to_Point / _Tri_to_Sizedpoint
GLPrimitiveSoftwareClip_Selection_* / _Feedback_*
GLPrimitiveSoftwareClipDispatch::ValidateState(GLPrimitiveSoftware&)
GLvoid GLPrimitiveSoftwareClip_Interface::LineToWideLine_CopyToVertexBuffer(...)
```

`ValidateState` is `libGL64.asm:0xea660` and it is the single point where the
driver decides which primitives it will hand to the hardware and which it will
take over. Decoded (CONFIRMED):

1. `[state+0x78a8]` is `glRenderMode`: `GL_RENDER` 0x1c00 / `GL_FEEDBACK` 0x1c01
   / `GL_SELECT` 0x1c02 select the plain, `_Feedback_*` and `_Selection_*`
   handler sets. Feedback and selection are entirely software.
2. In `GL_RENDER`, line width (`state+0xaa8`) and point size (`state+0xad4`)
   are tested against 1.0 (see §11).
3. Polygon mode front/back (`state+0x9e0+0x0c` / `+0x10`) selects
   `Tri_to_Tri` (`GL_FILL` 0x1b02), `Tri_to_Line` (0x1b01) or `Tri_to_Point`
   (0x1b00) — i.e. the *clipper* is polygon-mode aware, because clipping a
   line-mode triangle is not the same operation as clipping a filled one.

### The outcode: eight bits, and w ≤ 0 is explicit

`0xe8440` produces a 16-bit clip code per vertex from the clip-space position at
vertex-stream descriptor `+0x3f0`, and stores it at descriptor `+0x5a0`
(`0xe85ab: mov WORD PTR [rdi],ax`). Bit assignment, CONFIRMED instruction by
instruction:

| bit | test | site |
|---|---|---|
| 0 (0x01) | `(w_bits & 0x7f800000) == 0` — **w is zero or denormal** | 0xe8537 |
| 1 (0x02) | `signbit(w)` — **w < 0** | 0xe853d |
| 2 (0x04) | `x < 0` | 0xe8547 |
| 3 (0x08) | `x > gx*|w|` | 0xe8559 |
| 4 (0x10) | `y < 0` | 0xe8566 |
| 5 (0x20) | `y > gy*|w|` | 0xe857e |
| 6 (0x40) | `z < 0` | 0xe858e |
| 7 (0x80) | `z > gz*|w|` | 0xe85a1 |

Two things follow. First, **`w ≤ 0` has its own two bits and is handled
separately from the six frustum planes** — see the accept/reject test below.
Second, the ICD's software clip volume is **`0 ≤ x ≤ g·w`, not `−w ≤ x ≤ w`**:
this path works in a DirectX-style `[0, w]` clip space, which is also why the
software viewport transform is `win = (c/w)*scale + bias` with no ×0.5 (§3).
The hardware path is different — the ICD uploads the *raw* GL projection matrix
into constant slot 3, and the depth-range record is `((f−n)/2, (n+f)/2)`, which
is only correct for a `[−1,1]` NDC. So: **software path `[0,w]`, hardware path
GL `[−w,w]`.** The former is CONFIRMED; the latter is INFERRED from the depth
record's algebra and should be checked before relying on it.

### Trivial accept / reject, `Tri_to_Tri` at `0xee1d0`

```
ee350:  if (c0 & c1 & c2) != 0            -> TRIVIAL REJECT (all three outside one plane)
ee380:  c0 &= 0xfc ; c1 &= 0xfc ; c2 &= 0xfc
ee398:  if all zero                        -> TRIVIAL ACCEPT, emit unclipped
        else clip against planes 6, 2, 4, 3, 5, 7 in that order
```

Mask `0xFC` is the six frustum planes (bits 2..7); the two `w` bits take part
only in the trivial-reject AND. Exactly six plane indices (2..7) are ever passed
to the single-plane clipper `0xe8d60` anywhere in the binary — user planes go
through the separate `0xe9a00` pass (§7). CONFIRMED.

`0xe8d60` derives `axis = (idx>>1) − 1` and `sign = (idx^1) & 1` (`0xe8ea0`),
tests inside as `!((code >> idx) & 1)` (`0xe8e5e`), and `0xe8b00` solves the
split parameter

```
upper plane:  t = (g[axis]*wA - A[axis]) / ((B-A)[axis] - g[axis]*(wB-wA))
lower plane:  t = -A[axis] / (B-A)[axis]
```

(the `xor BYTE PTR [rsp+0x13],0x80` at `0xe8c9e` is the float negation).
CONFIRMED.

**What the driver must do, versus the hardware.** Near-plane and `w ≤ 0`
handling is not visible as a hardware feature anywhere in these binaries; what
*is* visible is that the ICD's own clipper always computes bits 0 and 1 and uses
them to reject, and that `gz` is forced to 1.0 (§6) so the near and far planes
are never guard-banded. The vendor switch text — "the TnLModule will clip on VP"
— puts real geometric clipping in the kernel-side transform module rather than
in libGL. For an open driver the safe bring-up choice is therefore to clip
against the near plane (`w > ε` and `z ≥ 0` in whatever convention you feed the
chip) on the CPU, exactly as `0xe8440`/`0xe8b00` do, and let the guard band plus
the raster scissor absorb the x/y sides. UNRESOLVED as to what the silicon does
unaided.

**The clipper contains no ±1024 or ±32 constant.** The whole float pool
belonging to `GLPrimitiveSoftwareClipping.cpp` is `.rodata:0x36acf4..0x36ad20` =
`0.5, 16.0, 0.0625, 0.5000010132789612, 1.5, 0.03125, −0.5` plus a sign mask;
an exhaustive `.rodata` scan for ±1024/±1023/±2048/±4096 finds only `0x36f8a4`
(the colour scale of §0), `0x36bc84` (2048.0, a point/line-size clamp) and the
point/line limit tables at `0x39fa18`/`0x39fa2c` (`{1.0, 2048.0, 0.25, 10.0,
0.25}`). CONFIRMED — the guard band is a computed scale (§6), never a hard-coded
coordinate limit.

---

## 9. Culling and winding

**State** at `state+0x9e0` (CONFIRMED, from the branch structure at
`libGL64.asm:0x112105`–`0x112bf3`):

| offset | field |
|---|---|
| +0x00 | `GL_CULL_FACE` enable (byte) |
| +0x04 | `glCullFace` mode: `GL_FRONT` 0x404 / `GL_BACK` 0x405 / `GL_FRONT_AND_BACK` 0x408 |
| +0x08 | `glFrontFace`: `GL_CW` 0x900 / `GL_CCW` 0x901 |
| +0x0c | polygon mode, front |
| +0x10 | polygon mode, back |

**`glFrontFace` → bank-B index 0x05.** `0x112147: value = 3` when the front face
is `GL_CCW`, `0x112b48: value = 2` when it is `GL_CW`. CONFIRMED as the
encoding; **2 = CW, 3 = CCW** — which is exactly what
docs/parhelia-gl-driver.md claimed for register 0x1014, so that entry is
*correct* and the capture never contradicted it (§1 consequence 2). The MMIO
address is 0x1014 only under the bank-B ⇒ `0x1000+index*4` assumption.

**`glCullFace` + enable → bank-A index 0x16 (reg 0x1058).** The decision table,
read directly off the branches (CONFIRMED):

| cull enable | `glCullFace` | `glFrontFace` | 0x1058 | site |
|---|---|---|---|---|
| off | — | — (front mode == back mode) | **1** | 0x112b8a |
| off | — | — (front mode != back mode) | **2** | 0x11287c |
| on | `GL_FRONT` | `GL_CW` | **2** | 0x112be1 |
| on | `GL_FRONT` | `GL_CCW` | **3** | 0x1121d4 |
| on | `GL_BACK` | `GL_CCW` | **2** | 0x112be1 |
| on | `GL_BACK` | `GL_CW` | **3** | 0x1121d4 |
| (reset path) | | | 1 | 0xf4348 |

So **`glFrontFace` is folded into the 0x1058 value**: the register selects a
*window-space winding* to discard, not a GL face. The capture's
`glDisable(GL_CULL_FACE) → 1` and `glCullFace(GL_BACK) → 2` are both reproduced.

**The encoding is Direct3D's `D3DCULL`.** CONFIRMED by [WIN]: MTXPARD stores
`D3DCULL_NONE / D3DCULL_CW / D3DCULL_CCW` = **1 / 2 / 3** verbatim, with no
remap table (`pard.asm:0xadc9b`). So

```
0x1058 = 1  no culling
       = 2  discard clockwise-wound triangles
       = 3  discard counter-clockwise-wound triangles
```

which is exactly consistent with the table above — `glCullFace(GL_BACK)` with
the default `GL_CCW` front face means "discard the clockwise ones" → 2. The
Linux ICD is computing a D3D-shaped cull mode from the GL pair.

`GL_FRONT_AND_BACK` is not separately encoded here and must be handled by
dropping the primitive — UNRESOLVED, verify before relying on it.

Note the surprising row: with culling **off** but different front/back polygon
modes, the ICD still programs `0x1058 = 2`, and swaps which face's mode goes to
which register depending on `glFrontFace` (`0x11289e`). Read together with §10
this says the register is really "winding select for the two-sided raster
state", of which culling is one use. INFERRED.

`CullNoneMode` (registry, default 1, [XSRV] `0x61759`) exists as a switch but
its consumer was not located. UNRESOLVED.

---

## 10. Polygon mode, polygon offset, fill rules

### Polygon mode — front and back are separate registers

| face | record | index | value |
|---|---|---|---|
| front | bank A (0x0c) | 0x08 → reg 0x1020 | 1 = `GL_POINT`, 2 = `GL_LINE`, 3 = `GL_FILL` |
| back | bank B (0x05) | 0x1C | same enum, or **0x7fffffff = none** |

Emitters `0x112237` (front) and `0x1122a1` (back). CONFIRMED, including the
capture's `GL_LINE → 2` / `GL_FILL → 3`. Like the cull mode, this is the
Direct3D enum: [WIN] stores `D3DFILL_POINT / _WIREFRAME / _SOLID` = 1 / 2 / 3
verbatim (`pard.asm:0xadc11`). CONFIRMED.

The `0x7fffffff` sentinel is written to the back register whenever culling is
enabled (`0x11216b` presets it), i.e. "there is no back face to rasterise".
CONFIRMED in the ICD — but note it is **Linux-ICD-specific**: MTXPARD packs fill
and cull into 2-bit fields of one dword (`ctx+0x1f3c`, fill bits 4–5, cull bits
8–9) and `0x7fffffff` appears nowhere in its 3D state code. Since MTXPARD's
`ctx+0x1f3c` is a driver-side shadow that is never seen as a register write in
that binary, the two are not necessarily in conflict, but the sentinel is only
attested from one source.

Because the two faces have independent registers, differing modes do **not**
force a software fallback by themselves — but the ICD does have
`GLPrimitiveSoftwarePrimType_DifferentPolygonModes` and
`..._DifferentPolygonModes_Flat` classes for the cases it cannot express (RTTI
at 0x36b240/0x36b2c0). Which cases those are is UNRESOLVED.

Note bank-B index `0x1C` and bank-A index `0x1C` (fog enable, capture-confirmed)
are different registers — the cleanest single proof that the two banks are
distinct address spaces (§1).

### Polygon offset — a hardware feature, five registers, values unscaled

`glPolygonOffset` is vtable slot +0x130 → `0x123850` → `0x1ce100`, which is just

```
1ce100: movss DWORD PTR [rdi+0x1c],xmm0   ; factor -> state+0x9fc
1ce105: movss DWORD PTR [rdi+0x20],xmm1   ; units  -> state+0xa00
```

plus `or DWORD PTR [ctx+0x10],0x4` — dirty bit 2. CONFIRMED. Completing the
polygon-state object of §9:

| offset in `state+0x9e0` | field |
|---|---|
| +0x16 / +0x17 / +0x18 | `GL_POLYGON_OFFSET_FILL` / `_LINE` / `_POINT` enable bytes |
| +0x1c | offset **factor** (float) |
| +0x20 | offset **units** (float) |

(`+0x16` = `GL_POLYGON_OFFSET_FILL` is CONFIRMED against `glIsEnabled`
`0x1276ff`; the `_LINE`/`_POINT` assignment of `+0x17`/`+0x18` is INFERRED from
the emission order below.)

The validation branch at `0x11256d` emits the depth range and then five bank-B
records back to back (CONFIRMED, all `opcode 0x05`, elemsize 8):

| bank-B index | source | value |
|---|---|---|
| 0x15 | `poly+0x1c` | offset factor, **raw IEEE float** |
| 0x16 | `poly+0x20` | offset units, **raw IEEE float** |
| 0x17 | `poly+0x18` | offset enable (point) |
| 0x18 | `poly+0x17` | offset enable (line) |
| 0x19 | `poly+0x16` | offset enable (fill) |

Two conclusions. **The offset is applied by the hardware, not the CPU** — which
matches the vendor switch `regOGLPolygonOffset` ("(0) Disables use what is
available / (1) Enables Hardware PolygonOffset", default TRUE) — and **the
driver does no scaling at all**: `factor` and `units` are copied through as the
application gave them, so the implementation-dependent `r` (the smallest
resolvable depth difference, `1/2^24` for a 24-bit buffer) and the
maximum-depth-slope term `m` are both computed in silicon. CONFIRMED by absence
over the whole emission chain, and corroborated by [WIN]: MTXPARD keeps
`D3DRS_DEPTHBIAS` (`ctx+0x2104`) and `D3DRS_SLOPESCALEDEPTHBIAS`
(`ctx+0x2100`) as **raw float bits** (`0xadd55`, `0xadd8a`), maps the legacy
`D3DRS_ZBIAS` into both (`0xadcec`, slope-scale = `zbias * 0.25f`), and never
computes `factor × max-depth-slope` either.

The one place `1/2^24` (`0x33800000`) *is* used as a number is
`libGL64.asm:0xe78f0`, in the ICD's own CPU path — i.e. the software offset
applied when `glPolygonMode` decomposed a polygon into points or lines and the
corresponding `GL_POLYGON_OFFSET_POINT` / `_LINE` enable is set. CONFIRMED
(it is referenced by no instruction at all in MTXPARD).

The coupling with the depth range is deliberate: when dirty bit 2 (polygon
offset) is set, the generic depth-range emission at `0x1118b1`/`0x112833` is
**skipped**, because this block re-emits it (`0x112577`) immediately before the
offset registers. An open driver should treat depth range and polygon offset as
one state group.

Under the bank-B ⇒ `0x1000+index*4` assumption these would be 0x1054–0x1064 —
but bank-A already has capture-confirmed, unrelated meanings at 0x1058
(cull), 0x105C (depth func), 0x1060 (alpha ref) and 0x1064 (alpha func), so
either bank B has a different base or a different stride. **Do not guess the
MMIO addresses**; this is the cleanest available test case for pinning bank B's
base with one capture (§14).

### Fill rules and subpixel precision

**Important scope caveat first:** everything in this subsection lives inside the
ICD's *software* primitive block (`0xea000`–`0xeb000`), next to
`GLPrimitiveSoftwareClipDispatch::ValidateState` and the CPU window-space
transform, and **MTXPARD contains no equivalent** — no `x*16+0.5` snap, no
odd/even point rule, no half-pixel bias anywhere in its 3D path. So these are
almost certainly rules the ICD applies to **CPU-generated window-space
vertices** (wide lines, sized points, software-clipped primitives) and not
something the hardware setup engine needs from object-space vertices.
[INFERRED] — the callers of `0xea420` were not traced. An open driver should
not pre-snap coordinates on the hardware path on this evidence alone.

**The CPU raster grid is 1/16 pixel.** The snap helper `0xea420` is

```
snap(f) = floorf(f * 16.0f + 0.5f) * 0.0625f
```

with `16.0f` at `.rodata:0x36acf8` and `0.0625f` at `0x36acfc` — four subpixel
bits. CONFIRMED as the code. `0.03125f` (half a subpixel, `0x36ad08`) is used
alongside it.

**The tie-break bias is `0.5f + 1 ulp`.** `.rodata:0x36ad00 = 0x3f000011 =
0.5000010132789612`, added to the window x after the viewport transform at
`0xea4f2`. A half-pixel offset deliberately perturbed by one ulp is the standard
way to make an exactly-on-boundary sample fall consistently to one side, i.e.
a fill-rule tie-break. CONFIRMED as the constant and its use; INFERRED as to
which edges it favours.

**Point centring parity.** `0xea4a3`: `size_i = (int)(pointSize + 0.5f)`, then
`test al,1` — odd and even rounded point sizes take different centring paths, the
usual GL rule (odd sizes centre on a pixel centre, even sizes on a pixel corner).
CONFIRMED as the branch; the two geometries were not both decoded.

---

## 11. Point size and line width

**Neither is a register on the hardware path.** The `(0x36,1)` record that was
thought to carry them is the near/far pair (§5). What exists is:

* bank-A `0x9a` (→0x1268) = point size, `0x9b` (→0x126c) = point size min,
  `0xa6` (→0x1298) = point size max (`0x114302`, `0x1143a1`, `0x1143ea`), with
  `max = max(pointMax, pointMin)` computed first, and for antialiased points an
  extra clamp to `[0.25, 10.0]` (`.rodata 0x36bc88`, `0x36bc8c`). CONFIRMED as
  the emission. **The hardware does rasterise sized points**: [WIN] keeps
  `POINTSIZE` / `POINTSIZE_MIN` / `POINTSIZE_MAX` / `POINTSCALE_A,B,C` at
  `ctx+0x20e8..0x20fc` and `POINTSPRITEENABLE` / `POINTSCALEENABLE` as bits 8/9
  of `ctx+0x1f38` (`0xae0d9`–`0xae15f`) as pure hardware state and does **no**
  CPU point expansion at all. So the Linux ICD's "any size ≠ 1.0 → CPU
  renderer" is a **driver policy choice, not a hardware limit**. CONFIRMED.
* bank-A `0x9e/0x9f/0xa0` = `GL_POINT_DISTANCE_ATTENUATION` `(a,b,c)` each times
  the viewport height squared (§3).

**The software takeover threshold** is in `GLPrimitiveSoftwareClipDispatch::
ValidateState` (`0xea660`). Line state lives at `state+0xaa0`: `+0x08` = width
(float), `+0x0c` = a boolean that selects between two regimes — most plausibly
`GL_LINE_SMOOTH` [INFERRED], since it is exactly the flag that makes any
non-unit width matter:

| `state+0xaac` | test | result |
|---|---|---|
| non-zero | `lineWidth != 1.0f` (`0xea6ea`) | `Clip_Line_to_Wideline` |
| zero | `lineWidth >= 1.5f` (`0xea8f6`, `.rodata 0x36ad04 = 1.5f`) | `Clip_Line_to_Wideline` |
| | otherwise | `Clip_Line_to_Line`, hardware 1-pixel lines |

CONFIRMED. **1.5f is the threshold** in the ordinary case: widths that round to
1 stay on the hardware path. Points work the same way — `pointSize != 1.0f`
(`0xea717`) selects `Clip_Point_to_Sizedpoint` — and `Tri_to_Wideline` /
`Tri_to_Sizedpoint` are the `glPolygonMode(GL_LINE)` / `(GL_POINT)`
equivalents. CONFIRMED.

**The expansion, exactly.** `LineToWideLine_CopyToVertexBuffer` starts at
`0xe7190` (assert `iLineWidth > 1`, `.rodata:0x36ad50`, site `0xe77c7`):

```
e71f7   xmm4 = lineWidth                       ; state+0xaa8
e7240   [rsp+0x67] = (|dx| >= |dy|)            ; major axis
e7245   if (state+0xaac != 0) goto e74e1       ; smooth/perpendicular variant
e7252   xmm4 += 0.5f                           ; .rodata 0x36acf4
e725a   N = (int)xmm4                          ;  N = round(width)
e725e   [rsp+0x60] = N
e7262   if (N-1 <= 0) assert-fail
e7285   [rsp+0x5c] = (N-1)/2                   ; dec / shr 31 / add / sar 1
        loop k = 1 .. (N-1)/2 : emit line at -k and at +k
        if (N & 1) == 0 : emit one more at -(N/2)
```

The offset is subtracted from **y** for an x-major line and from **x** for a
y-major line (the two loop bodies at `0xe7391` and `0xe749c`, selected by
`[rsp+0x67]`); `0xe774d` is the "−k" body, `0xe73ae`/`0xe73ba` the "+k" one.

So for **width 4**: `N = 4`, `(N−1)/2 = 1`, giving the base line, `−1`, `+1`,
and — because `N` is even — one extra at `−2`. Offsets `{0, −1, +1, −2}`.
That is **precisely** the captured
`y = 115.2, 114.2, 116.2, 113.2` for a nominal `y = 115.2`. CONFIRMED, in both
directions: the code predicts the capture and the capture pins the code.

When `state+0xaac != 0` the driver instead builds a true perpendicular offset:
`0xe7501`–`0xe7679` normalises `(dx, dy)` with `pow(dx²+dy², e)`
(`.rodata:0x3697b8`) and displaces both endpoints by `±width/2` along the
normal. CONFIRMED as the code path; not exercised by any capture.

The expansion writes **window-space** vertices — it has to, because it offsets
in pixels — which is exactly why the capture sees this path alone using the
32-byte `[x_win][y_win][z][w][r][g][b][a]` vertex format, having applied the
viewport transform on the CPU first (`0xe77e0`, §3).

---

## 12. Corrections this document makes to existing docs

| doc | claim | correction |
|---|---|---|
| parhelia-gl-driver.md, parhelia-3d-state-complete.md §2, parhelia-mesa-driver.md | `glScissor/glViewport = 0x1280/0x1284`, `(Y<<16)\|X` NDC×1024, guard band ±1024 | Wrong. Scissor is a pixel rect → 0x1328/0x132C; the 0x1280/0x1284 packet is a packed 10-bit colour and is dead code here (§0) |
| parhelia-3d-state-complete.md §2 | "Separate device/window clip in SCREEN PIXELS = 0x1304/0x1308" | 0x1304/0x1308 are SRC / DSTXY. The pixel clip is 0x1328/0x132C (§0) |
| parhelia-3d-state-complete.md §3, parhelia-gpu-access-map.md | `0x1298 = 2048.0f` "viewport/scale" | It is `GL_POINT_SIZE_MAX` (§3) |
| parhelia-3d-state-complete.md §3 | "Viewport transform + glDepthRange are CPU-applied … no dedicated scale/bias engine registers" | The hardware applies both; the driver sends a pixel rect and a z scale/bias (§3, §4) |
| parhelia-gl-feature-map.md §3 | `(0x36,1)` = line width + point size | It is eye-space (near, far) from the projection matrix (§5) |
| parhelia-gl-feature-map.md §6 | "0x1030 is multiplexed or mis-sliced" | It was a record header `(0x0c, 1)`, not a register write (§1) |
| parhelia-gl-feature-map.md §5 item 5 | "0x1014 is not (only) glFrontFace"; `0x1014 = 4` observed | `0x1014 = 4` was the header of a 4-entry bank-B list (the blend factors). `glFrontFace` → bank-B index 0x05, 2 = CW, 3 = CCW, as originally documented (§9) |
| parhelia-gl-feature-map.md §5 item 9 | "0x10d8 is not part of a contiguous stencil block" | It is stencil; the two-float payload was the `(0x36,1)` record in the opcode namespace (§1) |
| parhelia-gl-feature-map.md §2 | region 2 is "pairs plus block opcodes", first two dwords lost | It is a uniform `[opcode][count][count×elemsize]` stream; the "lost" dwords are just the first record's header (§1) |
| parhelia-command-construction.md §"Scissor" | `[2][0x2121a1a0]…` "confirms the documented guard-band scissor encoding" | It confirms a colour-constant write (§0) |
| parhelia-gl-driver.md §"Rasterizer state" | "polygon offset (depth bias in triangle setup)" is software-handled with no register | Polygon offset is a hardware feature: five bank-B registers, factor/units unscaled (§10) |
| parhelia-gl-driver.md §"Rasterizer state" | "glDepthRange + viewport scale/bias are CPU-applied … no dedicated engine registers" | Both are hardware; a viewport rect record and a z scale/bias record (§3, §4) |
| parhelia-3d-state-complete.md §3 | guard band `0x12f4` = 31.0/15.0/32.0 "distinct from the ±1024 clip clamp" | Correct about 0x12F4, and now confirmed in the Linux kernel core too; the ±1024 clamp is not a clip clamp at all (§0, §6) |
| parhelia-accel.md | "Depth range (fed to shader consts) 0x1268/0x126C/0x1298" | 0x1268/0x126C/0x1298 are point size / point size min / point size max; depth range is record `(0x27,1)` (§3, §4, §11) |

---

## 13. What an open driver has to do

Minimum, in submission order, for correct clipping and raster setup:

1. **Scissor** — program 0x1328 / 0x132C as a `(Y<<16)|X` corner pair in
   render-target pixels and set DWGCTL (0x1300) bit23 when
   `GL_SCISSOR_TEST` is on; clear bit23 otherwise. Intersect with the drawable
   yourself, as the ICD does.
2. **Viewport** — hand the hardware the window-pixel rect (bottom-origin `y`, or
   equivalently top-origin `H−y−h`); do not pre-transform vertices. Clamp to
   4096.
3. **Depth range** — `scale = (f−n)/2`, `bias = (n+f)/2`, floats.
4. **Near/far** — `(n, f)` in eye space from the projection matrix if you use a
   perspective projection; `(1.0, 1.0)` otherwise.
5. **Guard band** — write 0x12F4 = 31.0f with ≤2 texture stages, 15.0f above
   that, and only on parts whose capability word passes the kernel's
   `0x7fc00` test. If you skip it, clip on the viewport on the CPU.
6. **Winding / culling** — bank-B index 0x05 = 2 (CW) / 3 (CCW); bank-A index
   0x16 = 1 (no cull) / 2 / 3 (discard one window-space winding, with
   `glFrontFace` folded in).
7. **Polygon mode** — bank-A index 0x08 for the front face, bank-B index 0x1C
   for the back, `1/2/3` = point/line/fill and `0x7fffffff` = none.
8. **Polygon offset** — send `factor` and `units` as raw floats plus the three
   enable flags, and re-send the depth range with them; do no scaling.
9. **User clip planes** — six, in **eye space**
   (`(M_modelview^-1)^T · plane`), uploaded as six `{index, a, b, c, d}`
   entries plus a 6-bit enable mask. On a first driver, clipping them on the
   CPU (as the ICD's own `0xe9a00` pass does) is a legitimate shortcut.
10. **Points and lines** — hardware for width/size that round to 1; CPU
    expansion above that, submitting window-space vertices, `N = round(width)`
    parallel lines at offsets `0, ±1, … , −(N/2)`.
11. **Fill rule** — snap to 1/16 pixel, bias by `0.5f + 1 ulp`.

Everything above except item 5, the exact 0x1328/0x132C ordering and the bank-B
base is reachable with the record/DMA formats already documented in
docs/parhelia-command-construction.md.

A minimal but correct first driver can go further and simply set the scissor to
the drawable, `gx = gy = gz = 1.0` (no guard band), and clip every primitive on
the CPU against `0 ≤ {x,y,z} ≤ w` plus the user planes — the ICD contains a
complete implementation of exactly that and falls back to it whenever the
hardware cannot express the state.

---

## 14. Open items and the experiments that would close them

1. **Which of 0x1328 / 0x132C is the min corner.** [KMOD] and [WIN] disagree,
   and both write it with clipping disabled, so neither settles it.
   *Experiment:* under X, call `ParhlXAASetClippingRectangle` (or run any XAA
   op with a clip) and read back 0x1328/0x132C; or `glScissor` a small box and
   dump both registers.
2. **The base of register bank B.** Bank A is `0x1000 + index*4` for every index
   the capture pins by GL semantics; bank B's base is unknown, and its indices
   collide with bank A's (index 0x1C is fog enable in bank A and the back-face
   polygon mode in bank B). *Experiment:* `glPolygonOffset(3.0f, 7.0f)` writes
   two raw floats to bank-B indices 0x15 and 0x16 — two unmistakable bit
   patterns; mmiotrace the draw and find where `0x40400000` and `0x40e00000`
   land. That pins the base with one run and simultaneously confirms the
   polygon-offset registers.
3. **Guard-band pixel extents — CLOSED (2026-09-05): ±8160.0f.** The formula
   `g = (guardExtent − origin)/viewport` was already CONFIRMED; the numerator is
   now pinned by three independent sources, two of them different binaries:
   `MTXPARD .data 0x1d1e30..0x1d1e3c` and inline in the clipper at `0xd6cf6`;
   the kernel's capability blob handed to the ICD (`caps[0x04..0x10]`, "0 or
   ±8160.0f", zeroed by the `GuardBandClipping` registry switch); and block index
   **0x174** = guard-band clip limits `{8160, 8160, −8160, −8160}`, init only,
   byte-identical in both binaries. Still DECODED (no hardware test), but no
   longer an unknown, and the earlier "do not assume ±2048" caution is resolved
   in the other direction. **Do not conflate this with `0x12f4`** (31.0/15.0),
   which is a different quantity.
4. **What register 0x12F4 actually means.** The values (32/16, programmed as
   31/15) and the dependence on the active texture-stage count are CONFIRMED in
   two binaries; the unit is not.
5. **`w ≤ 0` and near-plane behaviour in silicon.** The ICD always computes
   outcode bits 0 and 1 and rejects on them, and `gz` is pinned to 1.0, so the
   vendor stack never asks the hardware to survive a negative `w`. Assume it
   cannot.
6. **The consumer of record 0x25 (clip planes) and bank-A index 0x98 (the
   6-bit enable mask).** Both are ICD→kernel tokens; the kernel-side translation
   into WARP microcode or registers was not traced.
7. **`GL_FRONT_AND_BACK` culling.** Not distinctly encoded in the 0x1058 value;
   presumably the primitive is dropped earlier. Verify before relying on it.
8. **`CullNoneMode`** (registry switch, default 1) — its consumer was not found.
9. **The `0x12C0`–`0x12D4`, `0x12E4`, `0x12E8`, `0x1324` limit constants** the
   kernel programs next to the scissor and guard band (§6). Values known,
   meanings not.
10. **`GLVSClipPlaneDesc`'s parameter kind `0x87c2`** (`0x1696b0`) — the
    binding slot user clip planes occupy in a WARP vertex program.

## Depth range, z-clip registers and guard band - MEASURED on the P690 (2026-10-03)

`parhelia-drm-test --zclip-guard-probe` (Mesa p690-raster `69bc188`,
`a804a96`): a window-space quad whose z runs linearly from -0.5 (top) to
1.5 (bottom), 32-bit colour, native Z24, pass-through WARP program; counts of
written pixels per band (`z<0` 4096, `0..1` 10240, `z>1` 4096 pixels).

* **Depth test off**: every fragment is written, whatever z and whatever
  `0x12c0/0x12c4` hold.  Out-of-range z is never discarded per fragment.
* **z is clamped to [0, 1] before the depth test and write** (saturate,
  not wrap): func `ALWAYS` writes all bands; `GEQUAL` against a cleared
  `0xffffff` passes exactly the `z>1` band (clamped to 1.0) and nothing else;
  `LESS` against a cleared `0x400000` (0.25) passes all of `z<0` (clamped to
  0; a wrap would fail half of it) plus exactly the 16 rows below 0.25
  (2048 pixels, the exact count).
* **`0x12c0` / `0x12c4` are a per-fragment z-clip window on the unclamped
  z, signed 4.28 fixed point (1.0 = `0x10000000`), applied only while the
  depth test is enabled** (any function, including `ALWAYS`):
  `[0, 0x10000000]` with `ALWAYS` writes exactly the `0..1` band and
  discards `z<0` and `z>1`; `[0x08000000, 0x10000000]` writes exactly the
  rows with z >= 0.5 (5120 pixels, exact); `zmax 0x01000000` (z <= 1/16)
  discards the whole visible part of the band, `zmax 0x20000000` (2.0) keeps
  `z = 1.5`.  The vendor's `0x80000000 / 0x7fffffff` is "no z clip", which
  together with the clamp is exactly `GL_DEPTH_CLAMP` behaviour.  Our earlier
  guess `[0, 0x00ffffff]` (Z24 units) discards everything.
* The vendor GL (`opengl6/libGL.so.1.3.0`) exports no depth-clamp
  extension, and `mtx_parhelia.o` has no clamp/z-clip registry switch; the
  vendor always emits the full window (D3D device fields `0x20b0/0x20b4`).

So GL near/far clipping for vertices with w > 0 can be done per fragment by
the hardware: program `[0, 0x10000000]` while the depth test is on (or turn
the test on with `ALWAYS` and no depth write when a depth buffer is bound).
Without a depth buffer there is no z discard; those draws need geometric
clipping.  Not yet measured: the exact inclusive/exclusive edge and the
behaviour on the original family (DERIVED/UNTESTED there).

**Guard band `0x12f4`**: with the vendor's LX value 31.0 (`0x41f80000`) and
with 64.0, a quad whose left and right edges lie 0, 32, ... 8160 pixels off
a 256-pixel target is rasterised fully and its red ramp (0 at the left edge,
1 at the right edge) is exact to within 0.5 LSB at x = 8, 128 and 248 for
every offset - window-space vertices up to 8160 pixels outside interpolate
correctly.  With `0x12f4 = 1024.0` quads of 0..256 pixels off screen are
not drawn at their edges and the z quad loses pixels: the register is not a
pixel bound and must keep the vendor value.  The colour error seen earlier
with the draw module's guard band (fog-linear-floor) therefore is not an
x/y raster limit of window-space vertices; its cause (perspective vertices
handed through unclipped, the w/z path) is still unmeasured.

## The power-on RGBA second-triangle failure: bisect status (2026-10-03)

MEASURED over 9 power cycles with `parhelia-up.sh`'s warm-up (the stack every
cycle brings up): Mesa's 7-dword RGBA quads (any untextured draw into a
target with alpha: vertex-count, a self-written 10x10 `sqbox`, constant or
per-vertex colour) lose their **second** triangle - 45 of 100 pixels - in
every power-on state, and keep losing it across processes, X restarts
(either X module) and 11 minutes of idle time.

Heals (the next Mesa RGBA draw is complete): a Mesa draw through the kernel
pass-through with a 5-dword x,y,r,g,b stream (`sqbox` with an alpha-less
visual, with or without the hardware transform), fog-modes,
arb_vertex_program fogcoord-dp3, fp-fog.

Does not heal: parhelia-drm-test pass-through draws of any colour, 5, 6 or 7
dwords, distinct and covering, native or not, dithered or not; a degenerate
or covering second-colour draw; `--render`; `--native-transfer-probe`; Mesa's
`ph_prime_warp()` (with three identical or twelve distinct vertices);
glsl-fs-fogcolor-statechange; two-sided-lighting-separate-specular; the
combiner pads in both halves; a fresh rmmod/insmod (the state survives
module reload).

Not the trigger: the probe's own T2-free RGBA quads - large and the exact
10x10 geometry, mmap or native target, dithered - draw both triangles in
the same power-on state in which Mesa's identical-looking batch fails, and
Mesa without its context prime fails the same.  The remaining differences
between the failing Mesa batch and the working probe batch are the 2-D
clear in the same submit (which writes `0x12c0 = 0xffffffff`, DWGCTL fill),
the batch-boundary relocation, the `0x11e8` write and the target geometry;
the healing Mesa batch shares all of them.  Open.

### Resolved: the latch exists per primitive slot of a draw packet

Found offline after the window, reproduced without a power cycle (MEASURED,
Mesa p690-raster `d2c9891`, `1482232`): in the failing state Mesa passes
with `PARHELIA_PRIMS_PER_PACKET=1`, and the probe reproduces it once its
quad is one two-primitive packet (`COLOUR_ALPHA_PPP=2`).  A two-primitive
alpha-0 RGBA pass-through packet poisons both slots; a one-primitive
pass-through prime restores only slot 0 (18336 + 0 px, Mesa vertex-count
fails), a two-primitive prime both (18336 + 17955 px, vertex-count passes).
Every non-healer above drew one primitive per packet (drm-test, `--render`,
Mesa's old prime); every healer was a Mesa two-triangle pass-through or
colour-1 packet.  So the WARP T2 latch of section 12 in
`parhelia-p690-warp-isa-re.md` is per primitive slot, power-on leaves slot 1
rejecting, and Mesa now primes every slot (`1482232`).  The kernel's own
warm-up and any non-Mesa client should do the same: a pass-through packet of
as many black triangles as the widest packet any client emits.
