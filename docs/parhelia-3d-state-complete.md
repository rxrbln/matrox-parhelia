# Parhelia 3D pipeline state — consolidated RE (2026-09)

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** The `reg = 0x1000 + slot*4` rule is refuted (`0x12a0` is named in no vendor binary).
> ## STALE IN TWO SPECIFIC PLACES — both would break a draw
>
> This file elevates itself over others ("where the two disagree, this wins")
> while its own appendix admits part of its register list is mis-sliced payload.
> Two concrete hazards:
> * **Do not replay the `0x1100-0x117c = 0xffffffff` "attribute file init"
>   after binding.** `0x1100` is the WARP entry point; replaying the sweep
>   overwrites your entry with a bogus value.
> * **Do not program `0x1298` as draw state.** It is `GL_POINT_SIZE_MAX`, and
>   the address itself is refuted — the 2048.0 is a clamp folded into block
>   `0x173`.


Provenance: reversed from `matroxdriver-x86_64-cvs-20090511/opengl6/libGL.so.1.3.0`
(gl64.asm), the win32 D3D driver `MTXPARD.DLL` (pard.asm), and the Linux X HAL
`mtx_drv.so` (mtx.asm). Complements docs/parhelia-gl-driver.md,
parhelia-fragment-isa.md, parhelia-vertex-isa.md, parhelia-3d-driving.md and the
WARP microcode/draw-submission RE (see [[parhelia-warp-draw-protocol]]).
Tags: [C]=confirmed from disasm, [P]=probable, [CAP]=capture-only.

## 1. Fragment combiner [SUPERSEDED]

**This section was wrong in several fields and has been replaced by
`docs/parhelia-fragment-isa.md` (revision 2026-09-05).** Read that file; do not
use the bit tables that used to be here. Short summary of the corrected model:

- 10 instruction slots = 5 combiner stages x {colour, alpha}; the slot address
  lives in `0x1264[4:0]` and the driver's instruction list maps to slots
  `0,2,4,8,10, 1,3,5,9,11`. Instruction count is padded to 5 or 10.
- `0x1264` = slot[4:0] + **three** source operands, each 5-bit register selector
  + 4-bit modifier, at [13:5], [22:14], [31:23]. `0x07038000` is not a control
  base, it is just "srcB = c1, srcC = c0".
- `0x126c` = three 3-bit channel-replicate fields [2:0]/[5:3]/[8:6], opcode
  [14:9], **result scale** [19:17] as a signed shift (0 x1, 1 x2, 2 x4, 3 x8,
  7 /2, 6 /4 — not a write mask), destination register [23:20], saturate [24].
- `0x1278`-`0x1284` are the two general **constant registers** c0 and c1, four
  signed Q1.10 (x1024) components each — not a dedicated colour matrix.
- `0x1260`/`0x1268` are the **alpha** instruction of the same slot (identical
  layout); stage-flag bit0 = "separate alpha present". Texture-address stages
  are a separate 4-entry descriptor array, and their opcodes are still unknown.
- Opcode bases (`0x126c[14:9]`): MUL 0x0000, MOV 0x0400, SUB 0x0600, MAD 0x0800,
  LRP 0x0a00, ADD 0x0c00, DP3 0x1000, DP4 0x2000, CND 0x4c00, **CMV 0x5c00**,
  CMP 0x7c00. Operand roles: MAD = A*C+B, MUL = A*C, ADD = A+B, SUB = A-B,
  DP3/DP4 = dot(A,C), CND/CMV/CMP select between B and C on A.
- `0x00400803` is MAD with source-A replicate = red, not a "colour-matrix
  multiply" opcode.

## 2. Viewport / scissor — conflict RESOLVED [C]

**Real scissor/viewport clip = 0x1280 / 0x1284** (indices 0xa0/0xa1). libGL
emits them via DMA header 0x2121a1a0 (idx a0→0x1280, a1→0x1284; two 0x21 pads),
two lazy-flush emitters gl64 0x1c0453 / 0x1c4efe. Packing per reg = `(Y<<16)|X`.
Coordinate space = **NDC × ~1024 signed fixed-point**, clamped to guardband
[-1024.0 (0xc4800000), +1023.0 (0x447fc000)]; viewport scale/bias folded in on
the CPU via mulss (ctx+0xa068). NOT screen pixels.

Conflict roots (both earlier claims were context-confusions):
- 0x1280/0x1284 are a **format-multiplexed vertex-parameter register file**:
  clip-rect in the viewport-flush path, packed diffuse colour in the colour-draw
  path (MTXPARD 0x42c86), colour-matrix coeffs in the combiner path (§1). Same
  regs, different context — not a contradiction.
- 0x12c0/0x12c4/0x12c8 = 0xffffffff/0x7fffffff/0 are clip-plane/guardband LIMIT
  constants (max/disabled), not the scissor.

**Separate device/window clip in SCREEN PIXELS = 0x1304 / 0x1308** (Matrox
CXLEFT/CXRIGHT/YTOP/YBOT lineage), packed `(Y<<16)|X`, set on RT/viewport change
(MTXPARD 0x1305xx). [P] this is the coarse pixel scissor; 0x1280/0x1284 the fine
guardband clip. (2D engine also uses 0x1328/0x132C SCISSOR gated by DWGCTL bit23,
per docs/parhelia-2d-accel-complete.md.)

## 3. Guardband / viewport transform / primitives [C]
- Guardband 0x12f4: 31.0 (0x41f80000) untextured/≤2 stages; 15.0 (0x41700000)
  textured/>2 stages; default 32.0 (0x42000000). Distinct from the ±1024 clip
  clamp. (MTXPARD 0x113abf select, 0x1704f4 32/16 alt.)
- Viewport transform + glDepthRange are **CPU-applied** (software T&L, MTXPARD
  0x113729-0x113871 per-vertex fadds/fildl/fstps + matmul 0x10d860). No dedicated
  scale/bias engine registers.
- **Triangle-list is the only HW primitive** (DWGCTL 0x1300 low byte 0x07;
  kernel writes 0xc07, pard 0x307/0x707). Strips/fans/lines/points are
  CPU-decomposed to triangles.

## 4. Constants / matrices [P]
- Generic indexed constant window: reg = 0x1200 + slot*4 (MTXPARD 0x474300,
  type-gated (*obj & 0x70000)==0x10000).
- A 10-register transform/constant block 0x11c0-0x11e4 uploaded from a struct
  (MTXPARD 0x16fe9c-0x16ff33).
- key=0x20 [slot][16 floats] constant packet + MVP slot→register mapping: [CAP].
  Given CPU T&L (§3), the MVP appears CPU-applied; HW fixed-function-T&L with VS
  constants is not proven.

## 5. Texturing / blend / depth / stencil — register map (offsets known;
exact bit encodings partly [CAP])

Known register offsets (from docs/parhelia-gl-driver.md + this push):
- Texture base 0x11dc (VRAM byte offset), control 0x11e4; texture pipes 0x1200-
  0x120c: active 0x00040101, idle 0x00010000 (per stage).
- Colormask 0x12a0 (4-bit RGBA write-enable; 0 blanks output).
- Depth test enable 0x101c; alpha test 0x103c / func 0x1064.
- Blend equation 0x12ac (ADD/SUB/REVSUB/MIN/MAX = 1..5).
- Blend-factor enum→hw code (from parhelia-gl-driver.md): ZERO=1 ONE=2
  SRC_COLOR=3 1-SRC_COLOR=4 SRC_ALPHA=5 1-SRC_ALPHA=6 DST_ALPHA=7 1-DST_ALPHA=8
  DST_COLOR=9 1-DST_COLOR=0xa SRC_ALPHA_SAT=0xb CONST_COLOR=0xe 1-CONST_COLOR=0xf
  CONST_ALPHA=0x10 1-CONST_ALPHA=0x11. Factor bank = WriteBuffer key 0x5, whose
  absolute base is resolved at libGL runtime dispatch — [CAP] the base.
- S8Z24 depth surface base/format = **slot 1, `0x1228`/`0x122c`** (not slot 2),
  format enum **0x36** = 32 bpp. See `parhelia-engine-context-bringup.md` §7.

RESIDUAL (the RE agents for this section died on API errors before deepening):
the concrete texture FORMAT enums (RGBA8/RGB565/… → hw code), texture dim/log2
register + wrap (repeat/clamp/mirror) + min/mag/mip filter bit encodings, and the
D3D render-state → register bit encodings. Strong anchor for the latter, found
before the trace timed out: **a flat render-state array at MTXPARD
stateobj+0x4a4 (53 dwords, zeroed by ctor 0x86760)** — the per-D3DRS values are
written into this array and flushed to the 0x10xx/0x12xx registers; tracing the
per-token writers off 0x86760 is the way to recover the blend/depth/stencil bit
encodings. Left as the next RE step.

## Appendix: LIVE-CAPTURED draw state (ground truth) [CAP]

> **Superseded in part by [[parhelia-gl-feature-map]]** (per-GL-feature sweep,
> same board).  That capture shows the 136KB mapping is a *record* stream, not a
> flat pair stream, so part of the register list below is mis-sliced payload;
> it also contradicts §2 (scissor/viewport), §3 (primitive decomposition, CPU
> viewport transform) and §5 (0x1024, 0x1230).

Everything above is static RE. This appendix is what a **real vendor GL draw
actually programmed**, captured on hardware 2026-09-04 (0x0528, PCI 256MB board,
vendor 2009 stack). Where the two disagree, this wins.

**Platform caveat:** captured on a **PCI/PCI-X** board (no AGP/PCIe capability).
The register protocol is bus-agnostic, the bus addresses are not — see
docs/parhelia-3d-driving.md "Address-space caveat".

### Submission (mmiotrace, captures/mmio-draw-0528*.txt)
Per draw: arm `0x10b0=0x10`, `0x12b8=0x00070601`, `0x300=0x1f0b0140`, `0x384=0`
(DMAPAD, not a soft-trap)
(soft-trap), `0x308=0x83`; submit `0x404` = command-buffer bus address, then
`0x408 = end|3` repeatedly (end advances 0x20/packet); `0x314=1` kicks; poll
`0x8f0`/`0x450`. The **0x91c FIFO carries the WARP microcode ROM once at init
only** — it is NOT the per-draw geometry path.

### Command buffer (captures/cmdbuf-draw-0528-*.bin/-decoded.txt)
Captured by diffing the `/dev/matrox/mtx0` mapping between `glBegin` and
`glFlush`: 1539 bytes changed in the 136KB ring mapping, a 1109-dword span. The
content decodes as a stream of **(register-index, value) dword pairs**,
index = `(reg-0x1000)/4` — the same indexed register file the ring consumes.

State for one Gouraud triangle, first-seen order:

```
0x1034=1  0x1020  0x10d4=1  0x1030  0x105c=2  0x1098  0x126c=0
0x1298=0x45000000            ; 2048.0f  viewport/scale
0x1100-0x117c = 0xffffffff   ; whole vertex/attribute register file initialised
0x1118=1
0x1048=0x3f800000 (1.0f)  0x1200=0x80  0x1004=0x3f800000  0x1014=2  0x104c=1
0x10d8=1  0x1230=1  0x1090=1  0x108c=1  0x1084=0x18  0x1018=2  0x101c=1
0x1024=2  0x1028=2  0x102c=2  0x1088=2  0x109c=1  0x1094=6  0x1080=6  0x1058=1
```

The **`0x1100-0x117c=0xffffffff` attribute-file init** and the **`0x1298` scale**
are the state a from-scratch draw is most likely to be missing.

### Vertex format
`[x, y, r, g, b]` — 2 position floats + 3 colour floats, **stride 20 bytes**.
z and w are pipeline constants (w=1.0 const, z compared against 0.0), not
streamed. See docs/parhelia-vertex-isa.md.

### Per-feature mapping — method
Re-run the same PRE/POST command-buffer diff around each op in the `gltest.c`
sweep (clear, flat tri, gouraud, quad, lines, depth, blend, texture) and diff the
resulting buffers against each other; the delta is that feature's state. This
turns the [C]/[P] encodings above into confirmed values.
