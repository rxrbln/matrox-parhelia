# Parhelia GL feature → register map (live capture, ground truth)

Source: `captures/glsweep-0528-raw.txt`, produced by `tools/parhelia-glsweep.c`
on a real Parhelia **PCI 0x0528** under the vendor 2009 driver stack
(`matroxdriver-x86_64-cvs-20090511`).  Decoder: `tools/parhelia-glsweep-decode.py`
(re-run it to regenerate every listing quoted here).

Tags: **[CONFIRMED]** = the capture proves it (a GL call produced exactly this
value, or two independent ops agree); **[INFERRED]** = best reading of the
capture, self-consistent but not proven; **[AMBIGUOUS]** = the decode or the
meaning is genuinely unresolved — do not build on it.

> **Coverage: complete.** The sweep now runs all 33 operations end to end,
> including all six texture ops, `matrix_rotate` and `swapbuffers`.
>
> Historical note, kept because the wrong diagnosis was well-evidenced: earlier
> runs aborted partway and the kernel logged
> `Failed to allocate locked pages (nb: 801)`, which fits a real hard limit
> (`MTX_MAX_ORDER` = 9 caps allocations at 512 pages while the request needs
> order 10) and led to the conclusion that texturing could not be captured on
> this box. That was wrong on both counts. The allocation failures are non-fatal
> noise, and the hang was in the **harness**: linked without `-lm`, libGL's
> `ceilf` bound to the copy exported by `libstdc++` instead of libm and the
> driver's mip-level loop inside `glTexImage2D` never terminated. See the
> appendix of docs/parhelia-kernel-abi.md. **Any harness built against this
> vendor libGL must link `-lm`.**
>
> Texture results are in §8.

## 1. What the mapped regions actually are

This is the largest correction the capture makes.  The seven `/dev/matrox/mtx0`
mappings are not one ring; they are four cooperating buffers:

| region | size | role |
|---|---|---|
| 0 | 4 KB | submission header.  **`+0x230` = length in bytes of the command stream just submitted** [CONFIRMED, see §2]; `+0x234` = 0xffffffff |
| 2 | 136 KB | **ICD → kernel command stream**, rebuilt from offset 0 on every submission |
| 3 | 4 KB | `+0x00` = monotonic submission serial (2, 3, 4, …) [CONFIRMED] |
| 4 | 3.2 MB | **vertex heap** — raw float vertices, 0x2000-byte block per submission |
| 5 | 688 KB | **per-draw descriptor / GPU DMA packet**, same 0x2000 block stride as region 4 |
| 1, 6 | | not touched by any op in this sweep |

Region 2 is **not** a raw `(register-index, value)` pair stream, although pairs
are its dominant record.  It is a variable-length record stream that *contains*
pair runs.  Records carrying 64-bit kernel pointers (`0xffffc200_xxxxxxxx`) and
4-dword screen rectangles cannot be pairs — decoding them as pairs is what
produced the bogus entries in the older `cmdbuf-draw-0528-decoded.txt` reading.

## 2. Command-stream grammar (region 2)

Every submission (each `glFinish` in the harness) **rebuilds the stream from
byte offset 0**.  `region0+0x230` holds its byte length, which validates the
decode exactly:

| op | region0+0x230 | records decoded |
|---|---|---|
| `clear_depth` | 0x2c = 44 B | 2 dwords opcode + 9-dword CLEAR = 11 dwords ✓ |
| `tri_flat` | 0x30 = 48 B | 2 dwords opcode + 10-dword DRAW = 12 dwords ✓ |
| `depth_on` | 0x88 | reg write + PIPELINE-KEY + VERTEX-FORMAT + DRAW ✓ |
| `blend_srcalpha` | 0xd0 | 10 reg writes + key + vfmt + DRAW ✓ |
| `quad` | 0x9b8 | full state re-emit + DRAW ✓ |

**The first two dwords of every stream are lost.**  After `glFinish` they always
read back `(0x000000ff, 0x00000001)` — the driver overwrites them.  Since the
stream length counts them, they hold the stream's *first* record opcode.  Its
identity is recoverable from context: `(0x15,1)` before a clear (seen intact in
`clear_color`), `(0x1b,1)` before a draw, `(0x12,1)` / `(0x26,1)` / `(0x27,1)`
before a rect / viewport / float-pair payload.  The decoder reports this as
`CLOBBERED first token`.

### Record types

**A. Register pair** — 2 dwords `(index, value)`, MMIO reg = `0x1000 + index*4`.
The mapping is [CONFIRMED] for the low indices: eight independent GL calls land
on exactly the registers `docs/parhelia-gl-driver.md` predicted (§4).

**B. Block opcode + typed payload** — 2 dwords `(index, count)` followed by a
payload whose shape depends on the opcode.  For these the leading index selects
a *state block*; treating it as `0x1000+idx*4` is **[AMBIGUOUS]** and is not
assumed below.

| opcode | payload | meaning |
|---|---|---|
| `(0x15, 1)` | 9 dwords: mask, ARGB, depth float, stencil, `0x80808080`, x, y, w, h | CLEAR [CONFIRMED] |
| `(0x1b, 1)` | 10 dwords: GL prim enum, pad(unwritten), u64 kptr, u64 desc off, u64 desc off+0x1d, u64 vertex off | DRAW [CONFIRMED] |
| `(0x12, 1)` | 4 dwords x, y, w, h | scissor / clip rect, **screen pixels, Y from bottom** [CONFIRMED] |
| `(0x26, 1)` | 4 dwords x, y, w, h | viewport rect, same space [CONFIRMED] |
| `(0x27, 1)` | 2 floats | depth range as (scale, bias) = (0.5, 0.5) for the GL default [INFERRED] |
| `(0x36, 1)` | 2 floats | line width + point size (1.0, 1.0) [INFERRED] |
| `(0x23, 1)` | 4 floats | light-model ambient — value is exactly the GL default (0.2,0.2,0.2,1.0) [INFERRED] |
| `(0x21, N)` | N × 30 dwords | light / material blocks; block[0] = block index, block[4] = 3, then 8×1.0 (GL_LIGHT0 diffuse+specular defaults), a reversed 4×4 identity, and 180.0 (`GL_SPOT_CUTOFF` default) [INFERRED] |
| `(0x07, 1)` | 6 dwords: key, 0, 0, ffffffff ×3 | pipeline/shader-program key [CONFIRMED as a record, key encoding unresolved] |
| `(0x08, 1)` | 8 dwords: 0, 0, u64 kptr, **vertex stride in bytes**, 0, mask, 0 | vertex-format commit [CONFIRMED — the stride matches the region-4 data exactly in every op] |

Note the collision: indices `0x07`, `0x08`, `0x12`, `0x1b` are *also* real
register indices (`0x101c` depth enable, `0x1020` polygon mode, `0x1048`,
`0x106c` blend enable) and appear as plain pairs elsewhere in the same stream.
`depth_on` emits `(0x07,1)` twice — once as the depth-test enable, once as the
pipeline-key opcode.  Disambiguation is by payload signature only; the decoder
uses the `0xffffc200` pointer half, `0x80808080` and the `0xffffffff` triple.

## 3. Per-feature register map

Baseline = `tri_flat` (untextured flat triangle, depth/blend/stencil/alpha all
off).  **Read this with the harness's deferral in mind:** the ICD flushes state
lazily, so an op's stream also carries the *cleanup* of the previous op (e.g.
`blend_srcalpha` re-emits `0x101c=0`, `0x105c=2`, `0x1038=1` because
`depth_mask_off` ended with `glDepthMask(TRUE); glDepthFunc(LESS);
glDisable(GL_DEPTH_TEST)`).  Attribution below accounts for that.

### Clears

| GL | record |
|---|---|
| `glClearColor(.1,.2,.3,1)` + `glClear(GL_COLOR_BUFFER_BIT)` | CLEAR mask=**1**, argb=**0xff1a334d** |
| `glClear(GL_DEPTH_BUFFER_BIT)` | CLEAR mask=**2** |
| `glClear(COLOR|DEPTH)` | CLEAR mask=**3** — only the mask dword changed vs the previous clear |

Clear record = `[mask][ARGB8888][depth float][stencil][0x80808080][x][y][w][h]`,
rect in screen pixels.  Colour packs as A,R,G,B (0.1→0x1a, 0.2→0x33, 0.3→0x4d)
[CONFIRMED].  The `0x80808080` dword is constant in every clear — a write mask
or fill pattern [AMBIGUOUS].

### Primitives

`glBegin` mode is passed through to the driver **verbatim as the GL enum** in
the DRAW record — the ICD does **not** decompose:

| op | DRAW prim | region-5 desc `[1]`,`[2]` |
|---|---|---|
| tri_flat / gouraud / second | 4 `GL_TRIANGLES` | 0, 3 |
| points | 0 `GL_POINTS` | 1, 3 |
| lines | 1 `GL_LINES` | 0, 2 |
| quad | 7 `GL_QUADS` | 0, 4 |
| tristrip | 5 `GL_TRIANGLE_STRIP` | 1, 5 |
| trifan | 6 `GL_TRIANGLE_FAN` | 1, 5 |

Region-5 descriptor `[2]` = vertex count, `+1` when `[1]` = 1 (the flag is set
for POINTS/STRIP/FAN) [INFERRED].

Flat vs Gouraud shading programs **no register at all** — the only difference is
the per-vertex colour in region 4 [CONFIRMED].  Same for `tri_second`.

### Depth

| GL call | reg | value | tag |
|---|---|---|---|
| `glEnable(GL_DEPTH_TEST)` | **0x101c** | 1 | CONFIRMED |
| `glDisable(GL_DEPTH_TEST)` | 0x101c | 0 | CONFIRMED |
| `glDepthFunc(GL_GEQUAL)` | **0x105c** | 7 | CONFIRMED |
| `glDepthFunc(GL_LESS)` | 0x105c | 2 | CONFIRMED |
| `glDepthMask(GL_FALSE/TRUE)` | **0x1038** | 0 / 1 | CONFIRMED |

Depth-func encoding = `GL_func - GL_NEVER + 1`, i.e. NEVER=1, LESS=2, EQUAL=3,
LEQUAL=4, GREATER=5, NOTEQUAL=6, GEQUAL=7, ALWAYS=8 [CONFIRMED by two points].

Enabling depth also emits PIPELINE-KEY **0x11** (baseline 0x10) and grows the
vertex stride 20 → 24 bytes (Z appears).

### Blending

`glEnable(GL_BLEND)` → **0x106c = 1**; `glDisable(GL_BLEND)` → 0x106c = 0
[CONFIRMED, both directions observed].

| GL | 0x1024 | 0x1028 | 0x102c | 0x1030 |
|---|---|---|---|---|
| `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` | 5 | 5 | 6 | 6 |
| `glBlendFunc(ONE, ONE)` | 2 | 2 | 2 | 2 |

Four factor registers = src RGB / src A / dst RGB / dst A (exact pairing not
separable, both components of a call always take the same value here).  The
values match the **documented factor enum exactly** (ZERO=1 ONE=2 … SRC_ALPHA=5
ONE_MINUS_SRC_ALPHA=6) [CONFIRMED].

`blend_srcalpha` also emits `0x1014 = 4` (only occurrence in the sweep) and a
second `0x1030 = 3` — see §5 [AMBIGUOUS].  Blending switches PIPELINE-KEY to
0x51 and the vertex stride to 28 bytes (alpha appears, from `glColor4f`).

### Alpha test

| GL call | reg | value | tag |
|---|---|---|---|
| `glEnable(GL_ALPHA_TEST)` | **0x103c** | 1 | CONFIRMED |
| `glDisable(GL_ALPHA_TEST)` | 0x103c | 0 | CONFIRMED |
| `glAlphaFunc(GL_GREATER, …)` | **0x1064** | 5 | CONFIRMED (same 1..8 enum as depth) |
| `glAlphaFunc(…, 0.5f)` | **0x1060** | 0x80 | CONFIRMED — reference is **8-bit**, `round(0.5*255)` |

Alpha test needs **no** pipeline-key or vertex-format change.

### Stencil

| GL call | reg | value | tag |
|---|---|---|---|
| `glEnable(GL_STENCIL_TEST)` | **0x10d0** | 1 | CONFIRMED |
| `glDisable(GL_STENCIL_TEST)` | 0x10d0 | 0 | CONFIRMED |
| `glStencilFunc(GL_ALWAYS, **1**, 0xff)` — ref | **0x10e4** | 1 | CONFIRMED |
| `glStencilFunc(…, 0x**ff**)` — mask | **0x10e8** | 0xff | CONFIRMED |
| `glStencilOp(KEEP, KEEP, REPLACE)` | **0x10dc** | 3 | INFERRED (single data point; 3 = REPLACE in some op enum) |

`GL_ALWAYS` itself was not emitted — it is the ICD's default, so the stencil
*compare function* register is not identified by this capture.  `0x10d4` is
written =1 in the base state and is the likely candidate [INFERRED].
Stencil sets PIPELINE-KEY 0x11 and stride 24.

### Scissor, viewport, depth range

| GL call | record |
|---|---|
| `glScissor(32, 32, 64, 64)` | RECT **x=32 y=160 w=64 h=64** (opcode clobbered; `(0x12,1)` from the `linewidth`/`quad` streams) |
| `glDisable(GL_SCISSOR_TEST)` | RECT x=0 y=0 w=256 h=256 (full window) |
| `glViewport(0, 0, 128, 128)` | `(0x26,1)` + RECT **x=0 y=128 w=128 h=128** |
| `glViewport(0, 0, 256, 256)` | `(0x26,1)` + RECT 0,0,256,256 |
| (depth range default) | `(0x27,1)` + floats 0.5, 0.5 |

**Both rects are in screen pixels with Y measured from the bottom**
(`glScissor` y=32, h=64, window 256 → 256−32−64 = 160) [CONFIRMED].

### Cull face / polygon mode

| GL call | reg | value | tag |
|---|---|---|---|
| `glEnable(GL_CULL_FACE)` + `glCullFace(GL_BACK)` | **0x1058** | 2 | CONFIRMED |
| `glDisable(GL_CULL_FACE)` | 0x1058 | 1 | CONFIRMED |
| `glPolygonMode(FRONT_AND_BACK, GL_LINE)` | **0x1020** | 2 | CONFIRMED |
| `glPolygonMode(FRONT_AND_BACK, GL_FILL)` | 0x1020 | 3 | CONFIRMED |

### Fog

| GL call | reg | value | tag |
|---|---|---|---|
| `glEnable(GL_FOG)` | **0x1070** | 1 | CONFIRMED |
| `glDisable(GL_FOG)` | 0x1070 | 0 | CONFIRMED |
| `glFogi(GL_FOG_MODE, GL_LINEAR)` | **0x1230** | 3 | CONFIRMED (value), see §5 for the register conflict |

### Lighting

`glEnable(GL_LIGHTING)` + `glEnable(GL_LIGHT0)` →
- `0x1224 = 1` [CONFIRMED as the write, see §5 for the register conflict]
- `(0x21, 2)` + a 30-dword light/material block
- PIPELINE-KEY **0x5**
- vertex stride 24 with the third..fifth floats = (0, 0, 1) = the `glNormal3f`
  normal, **not** a colour → the vendor stack feeds normals to the hardware and
  does lighting on the GPU, not on the CPU [INFERRED but strong]

### Line width — the software path

`glLineWidth(4.0)` does **not** program a width register.  The ICD:
- emits PIPELINE-KEY **0x852** (unlike any other op),
- sets vertex stride to **32 bytes**,
- writes region 4 with **pre-transformed window-space** vertices
  (`0.1 → 140.8 = (0.1+1)/2*256`, Y flipped) laid out
  `[x_win][y_win][z][w][r][g][b][a]`,
- expands the one line into **four parallel 1-pixel lines** at y = 115.2,
  114.2, 116.2, 113.2 [CONFIRMED],
- and leaves `(0x36,1)` line width at 1.0.

So wide lines are CPU-emulated with a different, pre-transformed vertex path.
`region0+0x230` did not advance for this op and no DRAW record is present, so
the submission that carried this geometry is not fully visible — the following
`quad` op reuses the same region-4/5 0xc000 block [AMBIGUOUS].

The `quad` stream (0x9b8 bytes) is a **full state re-emit** — light/material
blocks, ambient, rects, sizes — before returning PIPELINE-KEY to 0x10 and stride
to 20.  Reading it as a per-feature delta is misleading; it is the ICD
rebuilding its whole context after the wide-line detour.

## 4. Vertex formats (region 4) — the format is dynamic

`docs/parhelia-3d-state-complete.md` states `[x, y, r, g, b]`, stride 20.  That
is **confirmed** as the baseline, and the capture shows it is only one of
several formats the ICD selects, each announced by the `(0x08,1)` record:

| stride | layout | seen in |
|---|---|---|
| 20 | x, y, r, g, b | tri_flat, gouraud, points, lines, quad, tristrip, trifan |
| 24 | x, y, **z**, r, g, b | depth_on … depth_mask_off, stencil, scissor, cullface, polymode, fog, viewport |
| 28 | x, y, z, r, g, b, **a** | blend_srcalpha, blend_additive, alphatest |
| 24 | x, y, z, **nx, ny, nz** | lighting [INFERRED] |
| 32 | x_win, y_win, z, w, r, g, b, a (window space) | linewidth (SW wide-line path) |

Coordinates in the first four formats are the **raw object-space `glVertex`
values** — no transform, no viewport scale.  Only the software wide-line path
pre-transforms.  Verified per op: e.g. `tri_gouraud` v0 = `(0.5, 0.9, 1,0,0)`
= red, v1 = `(0.1,0.1, 0,1,0)` = green, v2 = `(0.9,0.1, 0,0,1)` = blue.

## 5. Cross-reference: confirmations and contradictions

### CONFIRMED — the live capture proves these static guesses right

| doc claim | evidence |
|---|---|
| `docs/parhelia-gl-driver.md`: engine reg = `0x1000 + index*4` | holds for every low index the capture pins by GL semantics |
| glEnable(DEPTH_TEST) = **0x101c** | `glEnable`/`glDisable(GL_DEPTH_TEST)` → 1 / 0 |
| glDepthFunc = **0x105c**, NEVER..ALWAYS = 1..8 | GEQUAL→7, LESS→2 |
| glDepthMask = **0x1038** | FALSE→0, TRUE→1 |
| glAlphaFunc enable/func = **0x103c / 0x1064** | enable→1/0, GREATER→5 |
| glStencilFunc/Op in **0x10d0–0x10ec** | 0x10d0 enable, 0x10dc op, 0x10e4 ref, 0x10e8 mask |
| glCullFace+enable = **0x1058**, 1=off, 2/3=winding | disable→1, `glCullFace(GL_BACK)`→2 |
| glPolygonMode = **0x1020**, 1=point 2=line 3=fill | LINE→2, FILL→3 |
| blend factor enum ZERO=1 ONE=2 … SRC_ALPHA=5 1−SRC_ALPHA=6 | `glBlendFunc(ONE,ONE)`→2,2,2,2; `(SRC_ALPHA,1−SRC_ALPHA)`→5,5,6,6 |
| vertex format `[x,y,r,g,b]` stride 20 | exact, for the untextured non-depth case |
| `0x1100–0x117c = 0xffffffff` attribute-file init, `0x1298 = 2048.0f` | present verbatim in the context state of this capture too |

**New** (not in any doc): **`0x1060` = alpha reference, 8-bit** (`glAlphaFunc(…,
0.5)` → 0x80); **`0x106c` = blend enable** (1/0); **`0x1070` = fog enable** (1/0).

### CONTRADICTIONS — capture wins

1. **Region 2 is not a flat pair stream.**  `parhelia-3d-state-complete.md`
   ("The content decodes as a stream of (register-index, value) dword pairs")
   and `parhelia-vertex-isa.md:311` describe the whole buffer that way.  It is a
   record stream: draw and vertex-format records embed 64-bit kernel pointers
   (`0xffffc200_05c59004`, `0xffffc200_0591d004`), clear records embed an ARGB
   colour and a screen rect, and rect records embed four pixel coordinates —
   none of which can be `(index, value)` pairs (a would-be index of `0x05c59004`
   or `0x00000100` is out of the 0..0xff range).  The register list published in
   that appendix is therefore partly real register writes and partly
   mis-sliced payload dwords.

2. **`0x1230` is not the depth-surface base.**  `parhelia-gl-driver.md` §slot2
   and `parhelia-3d-state-complete.md` §5 call `0x1230/0x1234` the depth/Z
   (S8Z24) surface base/format.  In the capture `glFogi(GL_FOG_MODE, GL_LINEAR)`
   writes `0x1230 = 3`, and the base state has `0x1230 = 1`.  Those are small
   enums, not VRAM addresses.  Either `0x1230` is not a surface base, or the
   stream index `0x8c` does not map to `0x1000+idx*4`.  Both readings break the
   documented meaning.

3. **`0x1224` is not the slot-0 texture descriptor.**  `glEnable(GL_LIGHTING)`
   writes `0x1224 = 1` (stream index `0x89`), with no texturing in play.  Same
   caveat as (2): either the register meaning or the uniform index mapping is
   wrong for indices ≥ 0x80.  Treat every `0x12xx` claim derived from an index
   ≥ 0x80 as unproven.

4. **`0x1024` is not `glCullFace`.**  `parhelia-gl-driver.md` lists
   "glCullFace | 0x1024 | (main bank)".  Cull face demonstrably lands on
   `0x1058`; `0x1024` is a blend factor (2 for `GL_ONE`, 5 for `GL_SRC_ALPHA`).

5. **`0x1014` is not (only) `glFrontFace`.**  Documented as "2=CW, 3=CCW".  No
   `glFrontFace` call was made anywhere in the sweep, yet `glEnable(GL_BLEND)`
   writes `0x1014 = 4` — a value outside the documented enum.

6. **Scissor/viewport are passed as pixel rectangles, not `(Y<<16)|X` NDC×1024.**
   `parhelia-3d-state-complete.md` §2 resolves scissor/viewport to `0x1280/0x1284`
   packed `(Y<<16)|X` in NDC×1024.  The ICD hands the driver four separate
   dwords in **screen pixels** (`glScissor(32,32,64,64)` → 32, 160, 64, 64;
   `glViewport(0,0,128,128)` → 0, 128, 128, 128), and `0x1280/0x1284`
   (indices 0xa0/0xa1) never appear in the stream.  The NDC packing may still be
   what the *kernel* writes to the register — but it is not what GL emits, so the
   claimed CPU-side folding of viewport scale/bias is not what this stack does.

7. **The ICD does not decompose primitives.**  `parhelia-3d-state-complete.md`
   §3 says "Triangle-list is the only HW primitive.  Strips/fans/lines/points
   are CPU-decomposed to triangles."  The DRAW record carries the raw GL enum
   (0,1,4,5,6,7 for POINTS/LINES/TRIANGLES/STRIP/FAN/QUADS) with the untouched
   vertex list behind it.  The only CPU decomposition observed anywhere is the
   **wide-line** path (§3), and that expands to lines, not triangles.

8. **Viewport transform is not CPU-applied.**  Same section claims software
   T&L with per-vertex CPU transform.  Region 4 holds the raw object-space
   `glVertex` values for every normal draw; only the wide-line path stores
   window coordinates.  Together with the normals in the lighting op, this says
   the vendor stack runs transform **and lighting** on the WARP unit.

9. **`0x10d8` is not part of a contiguous stencil block.**  It carries a
   *two-float* payload (line width, point size), so `0x10d0–0x10ec` is not eight
   uniform stencil registers.

10. **"z and w are not streamed" is wrong.**  `parhelia-vertex-isa.md`
    concludes the WARP vertex is x, y, r, g, b only, with z compared against a
    constant 0.0 and w a constant 1.0.  The capture shows the ICD *grows* the
    vertex when it needs to: `depth_on` (`glVertex3f(…, 0.5f)`) emits stride 24
    with a real z = 0.5 per vertex, `blend_srcalpha` emits stride 28 with a real
    per-vertex alpha, and the wide-line path emits stride 32 with an explicit w.
    The 20-byte `[x,y,r,g,b]` form is the *minimal* format, not the only one,
    and the stride is announced per submission by the `(0x08,1)` record.

## 6. Unresolved / do not build on

- **`0x1030` (index 0x0c) is multiplexed or mis-sliced.**  It provably carries a
  blend destination factor (`glBlendFunc(ONE,ONE)` sets all four of
  0x1024/0x1028/0x102c/0x1030 to 2), yet it is also written =1 by `scissor`,
  `cullface`, `fog` and `lighting`, =2 by `polymode_line`, and a *second* time
  =3 within `blend_srcalpha`.  No single meaning fits.  [AMBIGUOUS]
- **PIPELINE-KEY values** 0x10 (base), 0x11 (depth or stencil), 0x51 (blend),
  0x05 (lighting), 0x852 (wide line).  Not a clean bitmask (lighting's 0x05
  drops bit 4 that every other state has).  Most likely a shader/pipeline
  program handle.  [AMBIGUOUS]
- **The two kernel buffers** behind `0xffffc200_05c59004` (draw) and
  `0xffffc200_0591d004` (vertex-format descriptor) are not mapped into the
  client, so their contents are outside this capture.
- **The clobbered first token** of every stream (§2).
- **`linewidth`'s leading 128 bytes** decode at no consistent phase; the decoder
  reports `UNDECODED BLOCK`.  It contains packed words (0x00000603, 0x0000060b,
  0x00180003, 0x000b0009, 0x000b0008) that look like a program/attribute
  declaration, not register pairs.
- **`quad`'s material blocks** at 0x660 and 0x798 (136 and 424 bytes) likewise.
- **Region 5 descriptor** `[0]=0x8f000001`, `[3]=2`, `[6]=0x05f58000` are
  constant across all 22 draws; `[4]=0x21212425` is the DMA index tag
  (0x25→0x1094, 0x24→0x1090, two 0x21 pads), `[5]` is the submission serial.
  Note the pad index here is **0x21**, which is a *DMA-tag* convention — in the
  region-2 stream `0x21` is the light/material block opcode, a different
  namespace.

## 7. Reproducing

```
python3 tools/parhelia-glsweep-decode.py captures/glsweep-0528-raw.txt
```
first runs a self-test on the known-good pair run the priming frame leaves in
the buffer (`0x0cc` → reg 0x1298 = 2048.0f, `0x0d4`… → regs 0x1100–0x117c all
0xffffffff) and must print `PASS`, then prints the per-op record stream and the
per-feature delta against the `tri_flat` baseline.  To extend coverage, fix the sweep so it survives past
`lighting` and re-capture — texturing is the largest gap.

## 8. Texturing — CAPTURED (supersedes the coverage caveat in the header)

The texture operations were captured once the harness was linked with `-lm`
(see the appendix in docs/parhelia-kernel-abi.md). The sweep now completes all
33 operations. Raw: `captures/glsweep-0528-raw.txt`, plus three controlled
variants used to identify fields: `captures/tex_32x16_0.txt`,
`captures/tex_64x64_0.txt` (RGBA), `captures/tex_64x64_1.txt` (RGB).

### Where texture data goes  [HW]

Texture pixels land in **region 6, the 4 MB heap** — the region previously
recorded as "not used per draw". They appear at **first use, not at upload**:
`tex_upload` changes nothing, and `tex_tri_nearest` changes exactly **16384
bytes = 64 x 64 x 4**.

That byte count is itself a result: a 64x64 texture occupies 16384 bytes for
**both** `GL_RGB` and `GL_RGBA`, i.e. **the driver stores 32 bits per texel
regardless of the requested internal format**. This matches ioquake3 reporting
"texture bits: 32" on this hardware.

### The texture descriptor record  [HW]

`tex_tri_nearest` carries a descriptor in the region 2 command stream. Fields
identified by differential capture — changing exactly one property at a time:

| Offset | 64x64 | 32x16 | Meaning |
|---|---|---|---|
| +0x08/+0x0c | 0xffffc200_008a8004 | (varies) | 64-bit kernel pointer to the texture object |
| +0x2c | 0x40 | **0x20** | **width** |
| +0x30 | 0x40 | **0x10** | **height** |
| +0x34 | 1 | 1 | level count / depth (constant here) |

Width and height are plain dword counts, not encoded log2 or pitch values.

**No format field was found.** Two runs identical in every respect except
`GL_RGB` versus `GL_RGBA` produce **byte-identical descriptors** (only the
kernel pointer differs, and that varies run to run anyway). Consistent with the
32-bit-always storage above: at this level there is nothing to distinguish them.

### Texturing's effect on vertex and pipeline state  [HW]

- **Vertex stride grows to 32 bytes (8 floats)** for a textured triangle, from
  24 bytes untextured. Note this contradicts the prediction of 28 bytes from the
  ICD's static stride table for a 2-float texcoord — the ICD selected the
  12-byte texcoord entry, not the 8-byte one.
- **PIPELINE-KEY gains bit 0x20000** when texturing is enabled: `0x20011`
  textured versus `0x11` untextured. The key drops back to `0x11` after
  `glDisable(GL_TEXTURE_2D)`.

### Still not decoded

`tex_tri_nearest` carries a **1928-byte block** the decoder cannot frame, which
contains the sampler state proper (filter, wrap, base address, pitch). The
per-feature deltas that should isolate those are small — `tex_linear` 48 bytes,
`tex_repeat` 28 bytes, `tex_env_modulate` 12 bytes in region 2 — but they decode
as record payload rather than `(index, value)` pairs, and do **not** name any of
the texture registers recovered statically (0x11c0 wrap, 0x11c4 filter, 0x11dc
base, 0x11e0 control, 0x11e4 height). So either those registers are written
through a path this capture does not see, or the static register assignment is
wrong. Unresolved — do not assume either.
