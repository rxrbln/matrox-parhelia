# Parhelia hardware for a new GL driver

What a from-scratch OpenGL driver (classic Mesa/DRI-style, or a Gallium backend)
needs from the hardware, mapped to the reverse-engineered registers. Companion
to `parhelia-accel.md` (register catalogue) and `parhelia-3d-driving.md`
(submission + first triangle). Evidence tags: [DRV]=`mtx_drv.so` disasm,
[GL]=`libGL.so` disasm, [HW?]=needs on-hardware MMIO capture.

Register model (triple-confirmed): engine register at MMIO `0x1000 + (index<<2)`,
driven by **direct MMIO** (`ExecuteEmulDma` replays each write) or the DMA
busmaster ring; sync by polling `0x8f0` bit17 idle + fence `0x450`.

## Command submission & sync — DONE

Direct MMIO: `*(bar1 + 0x1000 + index*4) = value`, then poll `0x8f0` bit17.
Fire a primitive with the draw-fence `0x12bc`. See `parhelia-3d-driving.md` §1.

**DMA ring — what the vendor GL actually does** (live mmiotrace,
captures/mmio-draw-0528.txt), superseding the arm-path sketch previously here:
per draw, arm the engine (`0x12b8=0x00070601`, `0x10b0=0x10`, `0x300=0x1f0b0140`,
`0x384=0` DMAPAD (not a soft-trap), `0x308=0x83`), then submit via **`0x404` = PRIMADDRESS =
command-buffer bus address** and **`0x408` = PRIMEND = `end|3`** written once per
~0x20-byte packet (low 2 bits = DMA mode 3 is the trigger); `0x314=1` kicks, poll
`0x8f0`/`0x450`. Note `0x404` is the *start*, `0x408` the *end* — the earlier
"PRIMEND 0x404 / PRIMADDRESS 0x400" labelling was inverted. Bus addresses are
PCI-board-specific; see parhelia-3d-driving.md "Address-space caveat".
**A GL driver can still start with pure direct-MMIO** and add DMA batching later.

## Framebuffer / render target / depth — DONE [DRV]

**Four surface slots**, each a (base, format|dims) register pair (`SurfDesc2d::Init`):

| Slot | base / fmt | role |
|---|---|---|
| 0 | 0x1220 / 0x1224 | source / texture |
| 1 | 0x1228 / 0x122c | **colour render target** |
| 2 | 0x1230 / 0x1234 | **depth / Z (S8Z24)** |
| 3 | 0x1238 / 0x123c | scratch / 2D-fill dst |

base = VRAM byte offset (colour dst uses 0 = screen; depth uses a real offset).
`fmt|dims` = `(ctrl<<24) | (fmt<<24 in [29:24]) | pitch[13:0]`; format codes 8bpp
`0x10`, 16bpp `0x28`, 32bpp `0x32`. Colour origin/extent via DSTXY `0x1308` / LEN
`0x138c`.

**Depth buffer:** base `0x1230`, format `0x1234`. **S8Z24** = 24-bit Z (mask
`0xffffff`, glClearDepth 1.0 → `0xFFFFFF`) + 8-bit stencil in the *same* buffer
(no separate stencil base). Exact S8Z24 format enum → capture `0x1234` on a 24/8
visual. **Depth clear:** point a slot at the Z surface, DWGCTL `0x1300 = 0xcc0`
(fill), rect in `0x138c`.

## Vertex submission — DONE (incl. per-vertex colour)

Up to **4 vertex slots**, selected by token low-byte `0x58+v` (reg `0x1160+4v`).
Each attribute "plane" is 4 floats written to `{0x1160+4v, 0x1174, 0x1178,
plane_reg}`; the plane-register write latches it. Plane by token high-byte
(`plane_reg = 0x1000+(plane<<2)`), all proven [DRV, kernel template `mtx3212`]:

| Plane | Reg | Attribute | Default (proves it) |
|---|---|---|---|
| 0x60 | 0x1180 | position `[w,z,y,x]` (screen space, w=1) | (1,0,0,0) |
| **0x61** | **0x1184** | **diffuse colour RGBA** | (1,1,1,1) = GL default |
| **0x62** | **0x1188** | **specular colour RGBA** | (0,0,0,0) = GL default |
| 0x63–0x67 | 0x118c–0x119c | texcoord 0–4 | |

**Primitive:** the hardware draws triangles via **DWGCTL `0x1300` low-nibble
opcode 6/7** (WARP vertex-list triangle; kernel uses `0xc07`), fired by the
draw-fence `0x12bc`. `0x4c2` (opcode 2) is the textured-trapezoid 2D path, not a
T&L primitive. **Triangle strip/fan, lines, points are decomposed to triangles
in software** by the GL layer (libGL's `GLPrimitiveSoftware*` classes) — the
hardware primitive is always a triangle. So `glColor` + Gouraud triangles work
directly (per-vertex diffuse plane 0x61); wide lines / points become triangles.

**Two on-hardware capture items** (structure proven, exact values not static):
colour component order within the 4-float plane (`[a,b,g,r]` assumed), and
opcode 6-vs-7 (likely smooth vs flat shading).

## Rasterizer state — offsets DONE, some encodings pending

| GL | Reg | Encoding |
|---|---|---|
| glDepthFunc | 0x105c | NEVER..ALWAYS = 1..8 [GL] |
| glEnable(DEPTH_TEST) | 0x101c | 0/1 |
| glDepthMask | 0x1038 | mask |
| glStencilFunc/Op | 0x10d0-0x10ec | 8 regs [GL] |
| glAlphaFunc | 0x103c / 0x1064 | enable / func |
| glBlendEquation | 0x12ac | ADD/SUB/REVSUB/MIN/MAX = 1..5 [GL] |
| glColorMask | 0x12a0 | **4-bit RGBA write-enable** (bit0=R…bit3=A; 0xf=all). NB: writing 0 blanks output. |
| glCullFace + enable | 0x1058 | 1=off, 2/3=cull a winding |
| glFrontFace | 0x1014 | 2=CW, 3=CCW |
| glPolygonMode | 0x1020 | 1=point, 2=line, 3=fill |
| glBlendFunc factors | bank 0x5 idx 0x09-0x0c | enum→code table below; **bank base = capture** |
| glCullFace | 0x1024 | (main bank) |
| glScissor / glViewport | 0x1280 / 0x1284 | `(Y<<16)|X` per corner, NDC×1024, guard band ±1024 |
| depth buffer base/format | 0x1230 / 0x1234 | see Framebuffer section |

glDepthRange + viewport scale/bias are **CPU-applied** in the fixed-function
path — no dedicated engine registers (a hardware-VS path may use VS constants;
capture 0x1268/0x126c/0x1298 on glDepthRange to confirm/deny).

**Software-handled (no register)** [DRV]: fog (per-vertex factor computed on the
CPU, `0xf96f0`, GL_EXP/EXP2/LINEAR), point size, line width (points/lines drawn
as CPU-expanded triangles), and polygon offset (depth bias in triangle setup). A
master 3D pipeline-control bitfield lives at **0x1000**/0x1004 (highest write
fan-out; full bit decode incomplete).

**Register banks** [GL]: libGL emits state as `(key, index, value)` packets;
`key` selects a register bank. **key=0xc = main bank** (`reg = 0x1000 + index*4`,
proven by 4 matches). **key=0x5 = a second bank** (blend factors, ~19 regs) whose
absolute base is not static — a GL driver resolves it with one capture (watch the
4 contiguous offsets that receive codes 0x05/0x06 on
`glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`).

**Blend-factor enum → hw code** [GL, complete]: ZERO=1, ONE=2, SRC_COLOR=3,
1-SRC_COLOR=4, SRC_ALPHA=5, 1-SRC_ALPHA=6, DST_ALPHA=7, 1-DST_ALPHA=8,
DST_COLOR=9, 1-DST_COLOR=0xa, SRC_ALPHA_SATURATE=0xb, CONSTANT_COLOR=0xe,
1-CONSTANT_COLOR=0xf, CONSTANT_ALPHA=0x10, 1-CONSTANT_ALPHA=0x11.

## Fragment / pixel shader — DONE (a DX8 ps.1.x-shaped ISA)

The pixel shader is a programmable two-phase combiner (`GL_MTX_fragment_shader`),
reverse-engineered in full — see **`parhelia-fragment-isa.md`**. Opcode set
MOV/ADD/SUB/MUL/DP3/DP4/MAD/LRP/CND/CMV/CMP (0–10); input selectors tex0-3=0-3,
temp0/output=4, temp1=5, constant=0xE, **iterated primary colour=0x10**,
secondary=0x11; DX8 ps.1.x modifiers (Scale/Saturate, per-arg Convert/Replicate).
Stage words: `0x1264` = input selectors + control | slot, `0x126c` = output
(bits[23:20]) + opcode-base; pass-through identity = `0x1264=0`, `0x126c=0x400400`.
Colour-matrix coeffs `0x1278-0x1284` (signed-11-bit ×1024, two per reg). Texture
pipes `0x1200-0x120c`: active `0x00040101`, idle `0x00010000`. (The one residual:
the concrete opcode-base bits per ALU op are packed by the kernel module.)

## Texturing — DONE (linear); wrap modes TBD [DRV]

Corrected register roles (`CDPSTextureEngine::UpdateHardware`):
- **0x11dc = texture base** — 32-bit byte offset in VRAM (`MmSurface[+0x90]`).
- **0x11e0 = control word** — `[13:0]`=pitch (32-pixel-aligned width in pixels),
  bit17 set, `[29:24]`=format code, `[31:30]`=tiling (0=linear).
- **0x11e4 = height** — `0x00010000 | (height & 0x3fff)`.
- **0x11e8 = filter** — min[7:0] / mag[15:8] / mip[23:16] (build the word directly:
  nearest/linear/mip per nibble; the driver doesn't translate GL enums).

**Format codes** (proven, corrected): 8bpp `0x10` (0x11 in texture desc), 15bpp
1:5:5:5 `0x26`, 16bpp 5:6:5/4:4:4:4 `0x28`, **32bpp 8:8:8:8 `0x32`**, 2:10:10:10
`0x34`, DXT/S3TC `0x2e`. (This also corrects the framebuffer descriptor: 32bpp
is 0x32, not 0x28.)

**Texel layout (linear):** `dst = base + y*pitchBytes + x*bpp`, `pitchBytes =
((width+31)&~31) * bpp/8`. Directly implements `glTexImage2D`. Tiling
(bits[31:30]≠0, power-of-two dims, swizzled) is gated by `reg3DUseTextiled` and
should be **forced off** for a first driver. **Wrap/clamp modes** aren't in the
scaler path (it always clamps) — likely in the fixed regs 0x11c8–0x11d8; a
capture item (or check `libglx.so`).

## Transform & lighting — mechanism DONE [GL]

Matrices/uniforms upload to **vertex-shader constant RAM** via `key=0x20`
command packets, payload `[slot:u32][16 floats]` (68 bytes; multiple matrices
coalesce). MVP = `transpose(A·B)` (evaluator @0x1695b0). Constant **slots** seen:
0x1, 0x3, 0x10–0x13 (a 4×4 = 4 consecutive vec4 slots). Single vec4 = `key=0x12`;
bulk vertex data = `key=0x40`; shader microcode = `key=0x45/0x46`. The absolute
constant-RAM base is not static, but **slot indexing means a driver never needs
it** — emit `[slot][16 floats]` and the microcode's constant bindings place it.

**Simplest route for a first driver:** skip this — **pre-transform on the CPU**
and submit screen-space vertices via the proven position plane (w=1). Hardware
T&L is an optimisation on top.

## The one inherent capture item

The pixel-shader **input selector for iterated primary colour** (`0x1264`
bits[9:5]) is produced by libGL's draw-time table walk and is not a static
constant [HW?]. One MMIO capture (watch `0x1264`/`0x126c`/`0x11c0-0x11e8`/DWGCTL
while the proprietary libGL draws an untextured `glColor` triangle) pins it. Same
capture reveals the per-vertex-colour plane if the static pass can't.

## Assessment for a GL driver

**Proven and sufficient today** for: command submission + sync, a colour render
target, screen-space (pre-transformed) triangles, depth/stencil/alpha/blend-eq
state, the fragment combiner + texture-env, and the DPS 2D fast paths (fill,
copy). That already supports a **pre-transformed, textured or flat-shaded GL
driver** (glDrawArrays of screen-space verts, depth test, texturing, alpha/blend).

**All four gaps now proven:** per-vertex colour (glColor → diffuse plane 0x1184)
+ primitive types; texture upload + descriptor (glTexImage2D, format codes);
blend factors (glBlendFunc, full enum→code table) + T&L matrix upload; and the
depth buffer (slot 2, 0x1230/0x1234, S8Z24) + scissor/viewport (0x1280/0x1284).

**A fixed-function GL driver that transforms on the CPU is fully specified:**
submit screen-space vertices with per-vertex colour/texcoords, bind colour +
depth surfaces, set depth/stencil/alpha/blend/scissor state, draw WARP triangles,
submit by direct MMIO + fence. Remaining static limits are all non-blocking
capture items (a few absolute register bases behind the ICD's thread-local
sender — blend-factor bank 0x5, constant RAM; two enum orderings — colour
components, opcode 6/7; the exact S8Z24 format enum).

**Beyond fixed-function:** the programmable **vertex ISA** is being reversed
separately (`parhelia-vertex-isa.md`) — the last piece for a hardware-T&L /
vertex-shader driver.

## WARP vertex/raster path — RE'd from win32 MTXPARD.dll (2026-08-18)

Reversed to explain why the standalone tool's WARP triangle rasterises nothing on
P690 while its 2D blit fill draws correctly. Source: `MtxPar/MTXPARD.dll`, the
XPDM display driver (vertex emitter @0x170500, WARP draw setup @0x10e8xx).
Confidence [WIN].

**Command-buffer replay model (confirms ExecuteEmulDma).** Register writes are not
direct MMIO at draw time; they are appended as `(offset, value)` 8-byte records
into a command buffer at `[ctx+0x10]` by helper `0x170e10` (stdcall, args
value+offset), then replayed to hardware. A Mesa/driver submitter batches
`(reg,val)` pairs, then replays.

**Attribute-plane / latch-register model — the key structural finding.** The WARP
engine has several per-vertex attribute planes, each committed by its OWN latch
register, all sharing the same staging registers:

- staging: `0x1160 + 4*v` (component 0, per vertex index v=0,1,2), `0x1174`
  (comp 1), `0x1178` (comp 2; `0x117c` seen for the 3rd vertex in some paths)
- **`0x1180` latch = POSITION** — per vertex: w=1.0(`0x3f800000`)→`0x1160+4v`,
  z=0→`0x1174`, y→`0x1178`, x→latch `0x1180`. (Matches our tool.)
- **`0x1184` latch = DIFFUSE colour.**
- **`0x118c` latch = a 4th attribute plane** (texcoord/specular), written per
  vertex from a 0x48-byte-stride vertex array, **conditional** on a vertex-format
  flag. **Our standalone tool omits this plane entirely** — a prime suspect for
  the non-rasterising triangle when the WARP program expects it.

**`0x1264` is a bitfield, not a plain write.** The fragment stage input selector
lives in `0x1264` bits[4:0]; MTXPARD updates only those bits
(`new = shadow ^ ((shadow ^ arg) & 0x1f)`) and preserves the upper bits from a
persistent shadow. **Our tool clobbers `0x1264 = 0`**, selecting fragment input 0
instead of the iterated primary colour (selector `0x10`). Fix: set the low-5-bit
selector to `0x10`, preserve upper bits.

**Conditional combiner block** `0x1260`/`0x1268` (state bit0), and
`0x1278`/`0x127c`/`0x1280`/`0x1284` (state bits1,2 = texture stages) — not needed
for a flat/Gouraud triangle. Note `0x1280`/`0x1284` here is a scissor DISTINCT
from the `0x12c8..0x12d4` window our tool sets.

**WARP draw setup** (caller @0x10e8xx, separate from the vertex emitter): surface
slot 0 `0x1220`/`0x1224` (base/cfg from shadow), depth slot `0x1230`, `0x1300`
DWGCTL (value from shadow), `0x1324`, bound via helper
`0x170f50(slot_base=0x1220, depth_base=0x1230, count=2)`; the raster is fired via
`0x12bc` (as our tool does).

**Concrete next experiments for `tools/parhelia-3dtri.c`** (all static-derived,
untested): (1) set `0x1264` low-5-bits selector = `0x10` instead of 0;
(2) add the `0x118c` attribute plane per vertex (try `[s,t,0,1]` or match diffuse);
(3) confirm the surface-slot bind order (0x1220/0x1224 + depth 0x1230) matches the
`0x170f50` helper. The 2D blit path is already proven on HW, so submission and
surface base are NOT the gap — it is one of these WARP-raster inputs.
