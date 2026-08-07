# Parhelia-512 fragment / pixel-shader ISA

Reverse-engineering of the Parhelia programmable **pixel (fragment) shader** — the
combiner exposed by the OpenGL driver as `GL_MTX_fragment_shader`, and driven by
the X driver's `CDPSPixelShader` class for video colour-conversion / procamp.
This is the pixel-side counterpart to `parhelia-vertex-isa.md`.

Sources:
- `xserver/7_4_99/mtx_drv.so` — **not stripped** (symbols renamed `mtxNNNN`, but
  the PLT thunks keep demangled `CDPSPixelShader::*` names). Gives the exact
  **hardware stage-register values** and the stage→register upload.
- `opengl6/lib64/libGL.so.1.3.0` — stripped, RTTI + `__PRETTY_FUNCTION__` asserts
  intact. Gives the `GL_MTX_fragment_shader` **API encoders** (enum → internal
  opcode/selector).
- `opengl6/include/glext.h` — **ships the full `GL_MTX_fragment_shader` enum
  block** (ground truth for every token below).

Evidence tags: [HW]=proven in mtx_drv disasm, [GL]=proven in libGL disasm,
[HDR]=confirmed by the shipped glext.h, [EMIT?]=pending / hardware-capture.

**Headline:** the Parhelia pixel shader is a **DirectX ps.1.x-shaped two-phase
combiner** — a *texture-address* phase (dependent texture reads: TEX, TEXBEM,
TEXM3x3, TEXDP3…) followed by an *arithmetic* phase of RGBA + Alpha ops
(MOV/ADD/SUB/MUL/DP3/DP4/MAD/LRP/CND/CMV/CMP). Each arithmetic instruction is
programmed into a small block of engine registers **0x1260-0x126c** (+ colour
matrix coeffs 0x1278-0x1284). The register file is **tex0..3, temp0(=output),
temp1, constant, iterated colour0/colour1**.

## Program model

The GL side assembles instructions into a `GLFragmentShader` object, one
**148-byte (0x94) record per instruction** (`imul rax,rax,0x94`, libGL
`0x10ee51`/`0x1b641e` etc.; record array base `shader+0xe0` [GL]). Two phases,
gated by the phase field at object **+0x18**: `glTexAddressOp` requires
**phase == 2**, the arithmetic ops require **phase == 0** (mismatch → GL error
`0x502`) [GL]:

| Phase | API | ops | glext tokens |
|---|---|---|---|
| texture-address | `glTexAddressOp{1..4}MTX` | dependent texture reads | `GL_OP_TEX_*` 0x8A8E-0x8A9D |
| arithmetic (colour) | `glFragProcRGBAOp{1,2,3}MTX` / `glFragProcAlphaOp{1,2,3}MTX` | RGB / alpha ALU | `GL_OP_MOV..CMP_MTX` 0x8AA0-0x8AAA |

RGB and Alpha are **co-issued** per instruction: `glFragProcRGBAOp*` writes the
RGB channels, `glFragProcAlphaOp*` writes the alpha channel of the *same*
instruction slot (both use the same op/selector encoders). The `Op1/Op2/Op3`
suffix = source count (1/2/3), matching the op's arity.

**Instruction limits:** the colour-phase counter at object **+0xdc** is bounded
`<= 9` (libGL `0x1b6126`/`0x1b61b8`, `cmp eax,9`) ⇒ up to ~10 arithmetic ops per
program [GL]. Authoritative queryable caps are `GL_MAX_FP_OPS_MTX` (0x8A85),
`GL_MAX_FP_TEXTURES_MTX`, `GL_MAX_FP_COLORS_MTX`, `GL_MAX_TA_OPS_MTX` [HDR].

## Hardware stage registers  [HW, established]

Engine reg = `0x1000 + (index<<2)`. `CDPSPixelShader::UpdateHardware`
(mtx_drv `mtx1136` @0x6d820) walks the in-memory stage array and emits
register/value pairs into the DMA command buffer. Per stage it always writes
**0x1264** then **0x126c**, and conditionally the texture and colour-matrix regs
based on a per-stage flag byte:

| Stage flag (byte @struct+0x0c) | emits |
|---|---|
| bit0 (`0x1`) | `0x1260`, `0x1268` (texture-address operands) |
| bits[2:1] (`0x6`) | `0x1278`,`0x127c`,`0x1280`,`0x1284` (colour-matrix coeffs) |
| always | `0x1264`, `0x126c` |

The in-memory stage struct is **36 bytes** (`this + index*36`), field→register map
proven verbatim in `UpdateHardware`:

| struct off | → HW reg | meaning |
|---|---|---|
| +0x0c | (flag byte) | which optional regs to emit |
| +0x10 | **0x1264** | input selectors + control; low 5 bits OR'd with slot index at upload (`edx&=~0x1f; edx|=slot` @0x6d896) |
| +0x14 | **0x126c** | output register + opcode-base |
| +0x18 | 0x1260 | texture-address word A |
| +0x1c | 0x1268 | texture-address word B |
| +0x20..+0x2c | 0x1278/0x127c/0x1280/0x1284 | colour matrix coeffs |

### 0x1264 — source / control word  [HW]

```
bits [4:0]    slot index      (merged at upload: reg |= slot&0x1f)
bits [9:5]    source selector A   ((sel<<5) & 0x3e0)
bits [18:14]  source selector B   ((sel<<14) & 0x7c000)
control base  0x07038000 for arithmetic stages (bits 15-17,24-26)
              high bits 0x70000000 (28-30) set on some stages
```
Evidence: `BuildPassThroughShader` `shl edx,5; and edx,0x3e0` (@0x6d7e6) writes
selector A; `BuildColorConvertShader`/`BuildAntiFlickerShader`
`shl esi,5; and esi,0x3e0; or esi,0x7038000` (@0x6e2fc) and the selector-B field
`shl r8d,0xe; and r8d,0x7c000` (@0x6e3f9). The two 5-bit selector fields are the
two source operands of the stage.

### 0x126c — destination / opcode word  [HW]

```
bits [23:20]  output register     ((out<<20) & 0xf00000)
low bits      opcode base
```
Opcode-base constants seen in the X-driver colour pipeline: **`0x400`** = MOV /
pass-through copy (`BuildShader` identity pad writes `[stage+0x14]=0x400400`,
i.e. out=4, base=0x400, @0x6e64a; `BuildPassThroughShader` `or ch,0x4`),
**`0x803`** = colour-matrix multiply (`or r11d,0x803` @0x6dd9b), plus `0x5c00`
and `0x70101c0`-class control words for the multi-stage CSC path. (These are the
*video* uses of the shader; the general GL op→base table is below.)

### Colour-matrix coefficients 0x1278-0x1284  [HW]

Signed **11-bit fixed point, ×1024**, **two coeffs per 32-bit reg**:
`cvttss2si` of `coef*1024.0` then `& 0x7ff` for bits[10:0], or `<<16 & 0x7ff0000`
for bits[26:16] (`BuildColorConvertShader` @0x6dd..0x6e2). xmm6 = 1024.0 scale
(`mov r13d,0x400; cvtsi2ss` @0x6dd7b).

## Fragment opcode set  [GL + HDR, complete]

The op-enum → internal opcode mapper is libGL `0x1b3a50`: a jump table on
`enum - 0x8AA0`, index `n` → internal opcode `n` (each handler `mov [rcx],n`),
a clean 1:1. `glFragProcRGBAOp` (`0x1b60e0`) and `glFragProcAlphaOp` (`0x1b64e0`)
**both** call it, so RGB and alpha share one opcode set. The internal opcode is
stored at shader **+0xa8**. Names are ground-truth from glext.h:

| glext token | enum | internal opcode | #src | API |
|---|---|---|---|---|
| `GL_OP_MOV_MTX` | 0x8AA0 | 0 | 1 | RGBAOp1/AlphaOp1 |
| `GL_OP_ADD_MTX` | 0x8AA1 | 1 | 2 | ...Op2 |
| `GL_OP_SUB_MTX` | 0x8AA2 | 2 | 2 | ...Op2 |
| `GL_OP_MUL_MTX` | 0x8AA3 | 3 | 2 | ...Op2 |
| `GL_OP_DP3_MTX` | 0x8AA4 | 4 | 2 | ...Op2 |
| `GL_OP_DP4_MTX` | 0x8AA5 | 5 | 2 | ...Op2 |
| `GL_OP_MAD_MTX` | 0x8AA6 | 6 | 3 | ...Op3 |
| `GL_OP_LRP_MTX` | 0x8AA7 | 7 | 3 | ...Op3 (lerp) |
| `GL_OP_CND_MTX` | 0x8AA8 | 8 | 3 | ...Op3 (a>0.5?b:c) |
| `GL_OP_CMV_MTX` | 0x8AA9 | 9 | 3 | ...Op3 (cond move) |
| `GL_OP_CMP_MTX` | 0x8AAA | 10 | 3 | ...Op3 (a>=0?b:c) |

Texture-address opcodes (texture-address phase, `glTexAddressOp*`), from glext.h — DX8 ps.1.x
t-ops: `GL_OP_TEX_NOP/TEX/TEX_COORD` (0x8A8E-90), `TEXKILL_GE/LT` (0x8A91/92),
`TEXBEM` (0x8A93, bump-env), `TEXDEP_AR/GB/RGB` (0x8A94-96, dependent reads),
`TEXDOT3_COORD/TEXDOT3` (0x8A97/98), `TEXM3X2/…_DEPTH` (0x8A99/9A),
`TEXM3X3/…_COORD/…_REFLECT` (0x8A9B-9D). Texture format per stage:
`GL_TEX_UNSIGNED/SIGNED/BIASED_MTX` (0x8AC0-C2).

## Source / result register selectors  [GL + HDR, complete]

The selector mapper is libGL `0x1b52d0` (called for every source *and* the
destination). Internal codes go into the 0x1264 selector fields [9:5]/[18:14]
and (for the destination) 0x126c[23:20]:

| glext token | enum | internal code | role |
|---|---|---|---|
| `GL_TEXTURE0_ARB` | 0x84C0 | **0** | texture pipe 0 |
| `GL_TEXTURE1_ARB` | 0x84C1 | **1** | texture pipe 1 |
| `GL_TEXTURE2_ARB` | 0x84C2 | **2** | texture pipe 2 |
| `GL_TEXTURE3_ARB` | 0x84C3 | **3** | texture pipe 3 |
| `GL_TEMPORARY0_MTX` / `GL_OUTPUT_COLOR_MTX` | 0x8AB0 / 0x8AB3 | **4** | temp0 == shader result register |
| `GL_TEMPORARY1_MTX` | 0x8AB1 | **5** | temp1 |
| (registered invariant / constant) | — | **0xE** | constant, via `glGen/SetFragProcInvariantMTX` |
| `GL_COLOR0_MTX` (GL_CURRENT_COLOR) | 0x0B00 | **0x10** | **iterated primary colour** |
| `GL_COLOR1_MTX` (GL_CURRENT_SECONDARY_COLOR) | 0x8459 | **0x11** | iterated secondary colour |
| `GL_IGNORED_MTX` | 0x8AB2 | **0xFF** | unused-source sentinel |

**Iterated primary colour selector = 0x10** (`cmp esi,0xb00; mov [rdx],0x10`
libGL @0x1b53a2 [GL], and `GL_COLOR0_MTX = 0x0B00` [HDR]). 0x10 = `0b10000` fits
the 5-bit selector field [9:5]/[18:14].

Note **TEMPORARY0 and OUTPUT_COLOR are the same physical register (code 4)** — the
program's output is read back from temp0. So there are exactly two writable
registers: reg4 (temp0/output) and reg5 (temp1). This matches the X-driver: its
identity pad stage uses selector 4 and output 4 (temp0), and its CSC stages use
selector 0xE (constant) — cross-validating the GL-side codes against real
hardware writes.

## Modifiers  [GL + HDR]

Set by `glFragProcOpParam(target, modifierClass, value)` (libGL `0x1b3de0`):
- **target** `GL_RESULT_MTX`(0x8AC3) / `GL_ARGUMENT0/1/2_MTX`(0x8AC4/5/6) →
  arg index r8d = 0/1/2 (`0x1b3f7b`/`0x1b3fa9`/`0x1b3e90`).
- **result modifiers** stored in the current-instruction scratch:
  - `GL_SATURATE_MTX` (0x8AC7) → boolean at shader **+0xb4** (`setne [rbx+0xb4]`).
  - `GL_SCALE_MTX` (0x8AC8) → value at shader **+0xb0** (values `GL_SCALE_X2/X4/X8/D2/D4` 0x8ACC-0x8AD0).
- **per-argument modifiers** (indexed `arg*12`):
  - `GL_CONVERT_MTX` (0x8ACA) → shader **+0xbc + arg*12** (`GL_ARG_INVERT/NEGATE/BIAS/BIAS_NEGATE/SIGNED/SIGNED_NEGATE` 0x8AB4-0x8AB9).
  - `GL_REPLICATE_MTX` (0x8AC9) → shader **+0xc0 + arg*12** (source-channel replicate `GL_RED/GREEN/BLUE/ALPHA_MTX` 0x8ABA-0x8ABD).

So the ALU modifier model = **result Scale (×2/×4/×8, ÷2/÷4) + Saturate**, and
per-source **Convert (invert/negate/bias/signed) + Replicate (smear one channel)**
— the classic DX8 ps.1.x arg/instruction modifiers.

## Two-level lowering — where the bit-packing happens  [GL, established]

**Important architectural correction to the working premise:** the two code
paths pack the stage registers at *different* layers.

- **X-driver / video path (`mtx_drv.so`)** bit-packs the `0x1264`/`0x126c` words
  **directly** — this is where the literal register layout above is proven [HW].
- **OpenGL 3D path (`libGL`)** does **not** contain the packing. Scans confirm the
  key constants are entirely **absent** from libGL `.text`: the `0x1264` immediate
  (`64 12 00 00`), the control base `0x07038000` (`00 80 03 07`), and the output
  mask `0x00f00000` (`00 00 f0 00`) — 0 occurrences each [GL]. libGL instead keeps
  a **shadow register file of 32 logical registers per stage** (value cache at
  `shader + (stage*32 + localReg)*4 + 0x400`) and emits dirty-delta command
  entries `{u16 stage, u16 localReg, u32 value}` into a `GLCommandBuffer`
  (allocator `0x1c9c50`, commit `0x1c9ad0`; entry writes `WORD[rax]=stage`,
  `WORD[rax+2]=localReg`, `DWORD[rax+4]=value` @~0x10f5c8). The final MMIO
  bit-packing (control bits, `<<5`/`<<14` selectors, opcode base, `<<20` output)
  is done by the **kernel/DRM layer** (`kernel/` / `mtx_parhelia.o`), not libGL.

A `localReg` **register-address template table** lives in libGL `.rodata` at
`0x39fc1c` [GL]:
`1374 1084 1090 1094 10b0 1104 1264 126c 1278 127c 1280 1284` — i.e. localReg
index 6→`0x1264`, 7→`0x126c`, 8→`0x1278`, 9→`0x127c`, 10→`0x1280`, 11→`0x1284`.
So the GL path and the video path converge on the *same* physical stage
registers; the `mtx_drv` disasm is the authoritative source for their bit layout.

### Internal opcode → libGL ALU-class  [GL]

libGL decodes the op via an 11-entry jump table in `.rodata` at `0x36ba98`
(decoder `0x10db20`: `cmp r14d,0xa; ja; movsxd rax,[rdx+r14*4]; add rax,rdx; jmp`).
Each case stores an abstract **ALU-class code** (0..7) into the decoded record
`[r13+0]` — *not* the final `0x126c` base (which the kernel applies):

| op | name | ALU-class | notes |
|---|---|---|---|
| 0 | MOV | (default) | 1 src |
| 1 | ADD | 0 | |
| 2 | SUB | 1 | |
| 3 | MUL | 3 | |
| 4 | DP3 | 1 | 3rd src forced 0xFF |
| 5 | DP4 | 2 | |
| 6 | MAD | 7 | |
| 7 | LRP | 4 | |
| 8 | CND | 4 | clears an aux field |
| 9 | CMV | 4 | |
| 10 | CMP | 4 | |

The final `0x126c` opcode-base value for each class is a **kernel/DRM capture
item** [EMIT?] — it is not present in either shipped user-space binary. The
video-path bases (`0x400` MOV-copy, `0x803` matrix-mul) are the only concrete
base values available, and they belong to the CSC pipeline rather than the
general GL op set.

### Source / output / modifier landing  [GL]

Confirmed by the emit trace, matching the API-side scratch fields:
- **Sources** (codes 0..0x11) are carried **unremapped** into the decoded record's
  three source sub-records (`[r13+0x10]/+0x1c/+0x28`, stride 0xC); the
  iterated-primary-colour code `0x10` passes straight through. Downstream they
  become the stage's srcA/srcB selector fields.
- **Output** code stays at scratch `+0xac`, committed with the opcode; the
  `output<<20` into `0x126c[23:20]` is applied downstream.
- **Modifiers** land in the scratch object exactly as the API decode wrote them
  (Saturate `+0xb4`; Scale `+0xb0`; per-arg Convert `src[i]+4` = `+0xbc+i*12`;
  per-arg Replicate `src[i]+8` = `+0xc0+i*12`); the emit reader (`0x10da70`/
  `0x10db10`) turns `src[i]+4`/`+8` into the swizzle/replicate/convert selectors.

### Texture-address phase  [GL]

`glTexAddressOp` (handler `0x1b47c0`) **requires phase == 2**
(`cmp DWORD PTR [rdi+0x18],0x2; jne error` @0x1b47e8; arithmetic ops require
phase 0) — this is the concrete two-phase gate; a mismatch raises GL error
`0x502`. It uses a **separate** tex-address operand mapper (`0x1b4170`, up to 4
operands) and writes per-texture descriptors into an array at **`shader+0x6a0`,
stride 0x24 (36 bytes)** (`lea rax,[rbx+code*36+0x6a0]; mov [rax+0x10],edx`
@0x1b4995), which the compiler lowers into the `0x1260`/`0x1268` texture-address
operand registers. The compile loop covers 4 texture stages
(`cmp r12d,0x3; jbe` @0x10df3f).

## Worked example — MODULATE (`output = texture0 × colour0`)

GL program:
```
glFragProcRGBAOp2MTX(GL_OP_MUL_MTX, GL_OUTPUT_COLOR_MTX, GL_TEXTURE0_ARB, GL_COLOR0_MTX);
glFragProcAlphaOp2MTX(GL_OP_MUL_MTX, GL_OUTPUT_COLOR_MTX, GL_TEXTURE0_ARB, GL_COLOR0_MTX);
```
Internal encoding (from the tables above): opcode = 3 (MUL), dst = 4
(temp0/output), srcA = 0 (tex0), srcB = 0x10 (iterated primary colour).

Resulting single hardware stage (slot s). The **0x1264 word is fully determined**
by the recovered field layout; the **0x126c opcode base for MUL is the one
remaining kernel-capture item**:
```
0x1264 = (0x00 << 5)      ; srcA = tex0    → 0x00000
       | (0x10 << 14)     ; srcB = colour0 → 0x40000   (iterated primary colour)
       | 0x07038000       ; arithmetic control base
       | s                ; slot index
       = 0x0707_8000 | s
0x126c = (4 << 20)        ; output = temp0  → 0x400000
       | <MUL base>       ; low-bits opcode base  [EMIT? — kernel/DRM, libGL ALU-class = 3]
```
Slot/flag byte = 0 (a plain MUL emits no texture-address or colour-matrix regs).
libGL encodes this op as ALU-class 3 (MUL) with srcA=0/srcB=0x10/dst=4; the
translation of ALU-class 3 → the concrete `0x126c` low-bits base must be read from
the DRM module (the video-path bases `0x400`/`0x803` are CSC-specific and not a
safe substitute). To capture it: bind this two-instruction program, draw, and
snapshot register `0x126c` — `0x1264` should read `0x07078000 | slot`.

## Relationship to the vertex ISA and to DX8

Like the vertex side (a DX vs_1_1 token stream), the fragment side is a
**DX ps.1.x-shaped** design: two-phase (tex-address then colour), ≤~10 colour
ops, per-arg source-modifiers, result scale/saturate, and the exact DX8 t-op set
(TEXBEM, TEXM3x3, dependent reads). Parhelia is therefore a DX8.1-class combiner.
The GL_MTX extension is structurally an ATI_fragment_shader-style API
(`glFragProcRGBAOp1/2/3` ≈ `glColorFragmentOp1/2/3ATI`) over Matrox-native tokens.
