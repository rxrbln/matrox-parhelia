# Driving the Parhelia hardware — submission, geometry, shading

Working notes toward issuing a draw to the Parhelia engine from software (the
goal: a colour triangle through the hardware). This consolidates the deep passes
that target *how to actually drive the pipeline*, separate from the register
catalogue in `parhelia-accel.md`. Evidence tags as before ([DRV]/[GL]/[SRC]/[HW?]).

## 1. Submission — SOLVED [DRV, from the kernel core]

The kernel's ioctl-27 busmaster handler (`mtx3257`, arm path `mtx2508` in
`kernel/mtx_parhelia.o`) reveals the primary-DMA submission registers. **A
userspace tool mapping BAR1 can drive this directly** — the only kernel-provided
piece is a GPU-visible buffer, and VRAM (BAR0) works for that.

| Reg | Role |
|---|---|
| **0x400 PRIMADDRESS** | primary DMA base `| mode` (low 5 bits). Writing it is the **trigger**. |
| **0x404 PRIMEND** | primary DMA end (4-byte aligned). |
| **0x408 PRIMPTR** | ring write-pointer doorbell (fast path). |
| **0x450** | completion fence / DMA read pointer — poll until it reaches the submitted mark. |
| **0x8f0 bit1** | engine ready-to-arm. |
| **0x31c** | engine idle when `(val & 0x57) == 0x50`. |

**Two mechanisms:**

- **Arm (engine idle) — the simple, MGA-style path, recommended first:**
  ```
  wait (0x8f0 & 2) and (0x31c & 0x57)==0x50     # engine ready/idle
  write 0x404 = pushbuf_end
  write 0x400 = pushbuf_start | mode            # this write triggers the fetch
  poll 0x450 until it reaches the fence value
  ```
- **Ring (engine running):** append a 5-dword record
  `{0x20292820, 0, start, end, 0}` (`0x20292820` = "execute secondary buffer
  [start,end)") to the primary ring, then bump `0x408`.

**Pushbuffer contents** are the indexed-register packets documented in
`parhelia-accel.md` (§"How commands reach the engine"): tag dword packing up to
four register indices `(offset−0x1000)>>2`, followed by their data dwords.

**Simplest path of all — direct MMIO (no DMA):** the PIO fallback
`HwDevice::ExecuteEmulDma` replays the same packets by writing each register
directly via MMIO. If that works for the draw registers (being confirmed), a
userspace tool can skip PRIMADDRESS/PRIMEND entirely: write the state + geometry
registers directly and kick. This is the first thing to try on hardware.

### Address-space caveat

`start`/`end`/`0x408` are in the GPU/AGP address space. A pushbuffer in **VRAM**
is addressed by its offset within video memory (reachable via BAR0 from
userspace); a pushbuffer in system RAM must be in the **AGP aperture**, which
only the kernel driver can map. So: VRAM pushbuffer = no kernel needed; AGP
pushbuffer = needs the Matrox kernel module's GET_BUFFER ioctl. Confirm on
hardware whether the engine will fetch a primary buffer from VRAM.

## 2. Geometry + per-vertex colour — SOLVED [DRV]

Up to **4 vertex slots** (v=0..3), selected by token low-byte `0x58+v` (reg
`0x1160+4v`). Each attribute **plane** is 4 floats written to `{0x1160+4v,
0x1174, 0x1178, plane_reg}`; the plane-register write latches it. Planes (proven
from the kernel vertex template `mtx3212`, by their OpenGL default values):

| Plane | Reg | Attribute | 4-float order | Default |
|---|---|---|---|---|
| 0x60 | 0x1180 | position | `[w, z, y, x]` (w=1 screen) | (1,0,0,0) |
| 0x61 | 0x1184 | **diffuse colour** | `[a, b, g, r]` (order tbd) | (1,1,1,1) |
| 0x62 | 0x1188 | specular colour | `[a, b, g, r]` | (0,0,0,0) |
| 0x63–0x67 | 0x118c–0x119c | texcoord 0–4 | `[q, r, t, s]` | |

**Draw opcode:** DWGCTL `0x1300` low nibble = primitive opcode. **6/7 = WARP
vertex-list triangle** (kernel uses `0xc07`) — the T&L primitive that carries
per-vertex colour, no texture needed. (0x4c2 = opcode 2 = textured trapezoid, the
2D DPS path.) **Fire:** write the draw-fence `0x12bc` with a running index
(stepped by 8); `ExecuteEmulDma` special-cases it (reads 0x450/0x41c, writes
0x384 — the soft-trap that kicks rasterisation).

**Primitive types:** the hardware draws only triangles; strip/fan/lines/points
are decomposed to triangles in software by the GL layer.

Implemented in `tools/parhelia-tri.c` (`--3d`) as a real Gouraud triangle.

## 3. Shading

**With the WARP triangle (§2), colour comes from the per-vertex diffuse plane
(0x1184) directly** — the pixel shader in pass-through + texture pipes idle lets
it through. So a coloured triangle no longer needs a texture (that was only true
of the 0x4c2 textured-trapezoid path). The minimal pipeline state to paint the
iterated colour, no texture bound [DRV, proven]:

```
0x1200 0x1204 0x1208 0x120c = 0x00010000   # all 4 texture pipes idle (no texture)
0x1264 = 0x00000000                        # pass-through pixel shader: in=0, slot=0
0x126c = 0x00400400                        # output reg 4, enable bit
0x101c = 0                                 # depth test off
0x12a0 = 0xf                               # glColorMask: RGBA write-enable (0 blanks output!)
0x103c = 0                                 # alpha test off
```

This is `CDPSPixelShader::BuildPassThroughShader` (@0x6d7e0) + the texture-engine
`Reset` (@0x6fa30) idle state + depth/blend/alpha disabled. With the combiner in
its reset state this passes the iterated primary colour through unchanged.

**The one static-analysis limit:** the pixel-shader **input selector** for
"iterated primary colour" (`0x1264` bits[9:5]) is assembled by libGL's
draw-time table walk and is *not* a static constant anywhere — it cannot be
recovered from the binaries. `tools/parhelia-tri.c --3d` uses the reset default
(selector 0) and seeds FCOL; the true value is one MMIO capture away.

**Capture recipe** (the proper way to resolve a draw-time-table value): on real
hardware, dump these registers while the proprietary `libGL` draws a single
untextured `glColor3f(...)`+triangle — **0x1264, 0x126c, 0x11c0–0x11e8,
0x1200–0x120c, and DWGCTL (0x1300)**. The values that differ from the reset
defaults above are the answer. Combiner reference values [DRV]: MODULATE =
`0x33f9e7ff`, REPLACE = `0x10f9e7ff` (op in bits[29:24]).

**Also note:** the mtx_drv vertex path (§2) carries no per-vertex colour — true
Gouraud lives only in libGL, which the capture would also reveal (the colour
register index / vertex-colour plane). For a *flat* colour without the capture,
bind a 1×1 solid-colour source texture (texture base → combiner 0x11dc/0x11e0,
control 0x11e4) and sample it.

For a *rectangle* rather than a triangle, DPS **solid fill** needs no texture or
shader at all — see §4; that path is fully proven.

## 4. Worked sequences — PROVEN, directly MMIO-replayable [DRV]

`HwDevice::ExecuteEmulDma` (@0x71d20) replays the command stream by writing each
register directly (`memWriteDword(0x1000 + index*4, value)`) — so every sequence
below runs from userspace with plain MMIO writes, no kernel. Sync: poll 0x8f0
bit17 idle (and the 0x450 fence).

**Solid-colour rectangle** (`CDPSSurface::Fill`, 7 writes — the minimal draw):
```
0x1228  dst_desc.dw0 = 0                       # base = VRAM offset 0
0x122c  dst_desc.dw1 = 0x20000 | (fmt<<24) | pitch   # fmt: 8bpp=0x10, 16bpp=0x28, 32bpp=0x32; pitch=(w+31)&~31
0x1300  DWGCTL       = 0x5c0                   # SOLID (uses FCOL)
0x1314  FCOL         = colour (byte/word-replicated per bpp)
0x1304  = 0
0x1308  DSTXY        = (top<<16) | left
0x138c  LEN          = ((bottom-top)<<16) | (right-left)
```
Implemented in `tools/parhelia-tri.c` (`--engine`, using h=1 rects as triangle
spans). The exact width/pitch bit position in dw1 is a best reconstruction (the
driver builds it from a caller struct not fully traced) — tune on hardware.

**Textured copy** (`CDPSSurface::Blit`, 8 writes): src desc 0x1220/0x1224, dst
desc 0x1228/0x122c, DWGCTL 0x1300=0x4c2, DSTXY 0x1308, SRCXY 0x1304, LEN 0x138c.

**Textured triangle**: §2 (3 vertices) + DWGCTL 0x1300=0x4c2 + fire 0x12bc.

### Surface descriptor (0x1220/0x1224 src, 0x1228/0x122c dst)

Two dwords: **dw0 = base** (0 = start of the bound allocation; VRAM offset 0 =
framebuffer), **dw1** = `[13:0]` pitch (32-pixel-aligned width in pixels), bit17
set, `[29:24]` format code, `[31:30]` tiling (0=linear). Format codes [DRV]: 8bpp
`0x10`, 15bpp `0x26`, 16bpp `0x28`, **32bpp `0x32`**, 2:10:10:10 `0x34`, DXT
`0x2e`. (Fully resolved by the texture-descriptor pass — no longer a soft spot.)

## Bring-up ladder — all implemented in `tools/parhelia-tri.c`

Build: `cc -O2 -o parhelia-tri tools/parhelia-tri.c`. Run as root on a card in a
known mode. Each rung validates more of the stack; climb in order.

1. **`--probe`** — map BAR0/BAR1 via sysfs, read chip status / fence / busmaster.
   Confirms the aperture decodes and the engine is idle. Pure reads, safe.
2. **`--fb`** — software-rasterise the triangle into the mapped framebuffer.
   Confirms BAR0 is the live framebuffer. No engine.
3. **`--engine`** — the proven DPS solid-fill sequence (§4), direct MMIO, as
   triangle spans. Validates submission + fence end-to-end on a fully-proven op.
4. **`--3d`** — the real vertex pipeline: minimal state (§3) + 3 vertices (§2) +
   DWGCTL 0x4c2 + fire 0x12bc, direct MMIO. Draws a screen-space triangle through
   the 3D rasteriser. Colour is the reset default until the §3 capture pins the
   iterated-colour selector.

Everything is direct-MMIO (no kernel). If the arm-path DMA is needed instead
(§1), the pushbuffer can live in VRAM (BAR0).

## Status

Submission (§1), geometry (§2), and the worked sequences (§4) are **proven and
implemented**. Shading (§3) is proven down to one draw-time-table value that is
inherently a hardware-capture item — the recipe for which is above. This is
enough to drive the pipeline; the capture turns the triangle the intended
colour.
