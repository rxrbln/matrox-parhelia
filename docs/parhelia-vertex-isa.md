# Parhelia "Trinity" vertex-shader ISA

Reverse-engineering of the Parhelia programmable vertex shader (the WARP-class
"Trinity" engine), exposed by the driver as `GL_EXT_vertex_shader` and a DX VS
1.1 path. Source: `opengl6/lib64/libGL.so.1.3.0` (function symbols stripped, C++
RTTI intact — navigation is by typeinfo-name → vtable → method). Evidence tags:
[GL]=proven from disasm, [HW?]=needs hardware capture.

**Headline:** the vertex program has a **DirectX vs_1_1-shaped token stream**
IR — `[0xFFFE0101] + [opcode][dest][src…] instructions + [0x0000FFFF]`, Parhelia
opcodes — held in the shader object's `std::vector<uint32>` at **+0x298**. This
IR is what the opcode/operand tables below describe.

**IMPORTANT correction (2026-08-19):** the token IR at +0x298 is **NOT** what is
uploaded to hardware. What key 0x45 memcpy's is a **separate, fixed 1636-byte /
409-dword C++ register-binding *descriptor* object** at shader **+0x70** — a
table of register slots + output-activation entries, containing **no opcodes**.
The DX-token→WARP-microcode assembly is completed **in the kernel**
(`mtx_parhelia.o`) when it consumes this descriptor (6 microcode variants,
0x183…0x199=409…0x19d dwords). So the +0x298 IR and the +0x70 uploaded descriptor
are two different buffers; the "verbatim token upload" phrasing conflated them.
See "Microcode load lifecycle" below for the descriptor layout.

All three passes (opcode table, operand encoding, serialization + a worked
program) completed and **cross-validate**. Two small fields — the per-source
negate modifier and the temp-register bank code — plus the declaration-stream
entry layout are the only residual items.

## Program model [GL, established]

A compiled shader lives in a `GLVertexShaderFunctionDX` object (728 bytes):
- instruction word stream: `std::vector<uint32>` at +0x298/+0x2a0 (begin/end);
  `GetCode()` returns it, `GetCodeSizeBytes()` = `(end-begin) & ~3`.
- instruction count at +0x1cc.
- a second uint32 vector at +0x68/+0x70 = declarations / constants.
- uploaded to the engine via `key=0x45`/`0x46` DMA packets (~1.6 KB blobs).

**Hardware limits:** 192 instructions preferred / 512 maximum. Register-file
classes 16 / 256 / 256 / 12 = **input / temp / constant / output** (vec4 each).

## Instruction word format [GL, established]

Variable length: **`2 + Nsrc` 32-bit words** per instruction.

```
word0 = opcode / control
word1 = destination operand
word2..word(1+Nsrc) = source operands
```

So Mov (1 src) = 3 words, a 2-source op (Add/Mul/Dot4/…) = 4 words, Madd (3 src)
= 5 words.

### Operand word fields [GL, complete]

The encoders (`0x16cc10` source, `0x16cc30` dest) just OR pre-built fields, so
the operand word layout *is* those fields:

**Destination operand:**

| Bits | Field |
|---|---|
| 31 | valid/used |
| 30:28 | register bank (see below) |
| 27:26 + 23:22 | shape size, two copies: `01`/`10`/`11` = scalar/vector/matrix (1/2/4 comp) |
| 24 | destination flag |
| 19:16 | **write-mask** X,Y,Z,W (1 bit/component; `0xF`=all) |
| 7:0 | register index |

**Source operand:**

| Bits | Field |
|---|---|
| 31 | valid/used |
| 30:28 | register bank |
| 23:16 | **swizzle**, 2 bits/component (comp0=[17:16]…comp3=[22:23]); `0/1/2/3 = X/Y/Z/W`; identity `0xE4` |
| 7:0 | register index |

Proven decisively by the `GL_SCALAR_EXT` declaration path (`0x1703d9`): a scalar
bound to component `c` builds swizzle = `c` replicated across all four 2-bit
groups, write-mask = `1<<(16+c)`.

## Opcode table [GL, complete]

`word0` is a **plain opcode index in the low ~7 bits** (max `0x63`); no
modifier/control bits are packed there (saturate/negate/mask live in the operand
descriptors, not word0). All 28 `GLVSInstruction` classes resolved:

| Instr | word0 | #src | | Instr | word0 | #src |
|---|---|---|---|---|---|---|
| Mov | `0x01` | 1 | | Max | `0x0b` | 2 |
| Negate | `0x01` | 1 | | SetLT | `0x0c` | 2 |
| WriteMask | `0x01` | 1 | | SetGE | `0x0d` | 2 |
| Swizzle | `0x01` | 1 | | Power | `0x55` | 2 |
| Add | `0x02` | 2 | | Clamp | `0x56` | 3 |
| Sub | `0x02` | 2 | | Cross | `0x57` | 2 |
| Madd | `0x04` | 3 | | Round | `0x58` | 1 |
| Mul | `0x05` | 2 | | Floor | `0x59` | 1 |
| Recip | `0x06` | 1 | | InsertComponent | `0x5a` | 1 |
| RecipSqrt | `0x07` | 1 | | ExtractComponent | `0x5b` | 1 |
| Dot4 | `0x09` | 2 | | Log2 | `0x5e` | 1 |
| Min | `0x0a` | 2 | | Exp2 | `0x5f` | 1 |
| | | | | MatrixMultiply | `0x60` | 2 |
| | | | | Frac | `0x61` | 1 |
| | | | | Index | `0x62` | 2 |
| | | | | Dot3 | `0x63` | 2 |

**Shared opcodes** differ only by an operand modifier, not word0: Mov/Negate =
`0x01` (Negate = Mov + source-negate); Add/Sub = `0x02` (Sub = Add + src1-negate).
Unused/reserved: `0x00, 0x03, 0x08, 0x0e–0x54, 0x5c, 0x5d`.

**No DX→native opcode LUT exists** — translation is structural: the shader
parser instantiates the matching `GLVSInstruction_*` C++ subclass, each of which
hardcodes its native word0 in its constructor (stored to object +0x20; the
`emit` virtual pushes it as word0).

### Emitted word stream

```
word0 = opcode (from object +0x20)
word1 = dest operand descriptor (dest reg + write-mask)
word2 = src0 descriptor | default-mask
word3 = src1 descriptor | default-mask   (if #src >= 2)
word4 = src2 descriptor | default-mask   (if #src == 3)
```
So Mov/transcendentals = 3 words, 2-source ALU = 4 words, Madd/Clamp = 5 words.
Object layout: +0x08 family tag `0x8799`, +0x10 dest ptr, +0x1c aux template,
+0x20 opcode, +0x28/+0x30/+0x38 source ptrs, +0x40/+0x44/+0x48 default masks.

## Register files / bank encoding [GL]

Register bank in operand bits **[30:28]** (one bit per file — one-hot), index in
**[7:0]**:

| File | Capacity | Bank bit |
|---|---|---|
| output | 12 | `000` (none) |
| input | 16 | bit28 `0x10000000` |
| constant | 256 | bit29 `0x20000000` |
| temp | 256 | bit30 `0x40000000` |

**Per-source negate** modifier = **bit 24** (`0x01000000`) — exactly the D3D
vs_1_1 `D3DSPSM_NEG` source-modifier position, consistent with the D3D-shaped
ISA. This is how Negate differs from Mov and Sub from Add (bit24 is free in a
*source* word; it is the destination flag only in a *dest* word).

The constant file is **dual-ended**: user constants (`c#`) grow up from index 0,
driver-generated constants grow down from the top — worth knowing when allocating
uniform slots. Temps are allocated lazily at emit time (resolver `0x170100`).

Component selectors in the high-level swizzle node (base `0x87d5` = GL_X_EXT)
carry a class field at bits [25:24]: `00`=plain swizzle, `01`=negated,
`10`=constant (ZERO/ONE) — the negate lowers to source bit 24 above.

**One residual item:** the declaration/constant stream (`dcl`/`def`) per-entry
bit layout binding input attributes to registers — best closed by capturing an
uploaded declaration blob (`key=0x45/0x46`) and diffing against a known
`dcl`/`def`. (The negate bit and temp bank code, previously open, are resolved
above.)

## Program format & serialization [GL, complete]

A compiled program is a self-contained D3D-style token stream:

```
0xFFFE0101                 ; header: vs_1_1 (written in the FunctionDX ctor)
<instruction tokens...>    ; [opcode][dest][src0|mod]...  per instruction
0x0000FFFF                 ; END token (D3D terminator)
```

Assembled two-pass (`Assemble` @0x16fda0): pass 1 sums each instruction's token
count (2+Nsrc) and reserves `sum + 8`; pass 2 calls each instruction's Serialize
(vtable slot 0), appending its tokens. The register allocator (`0x170100`)
resolves every operand's register token during Serialize, so the finished stream
is **final** — the DMA upload (`key=0x45/0x46`) is a plain memcpy of
`GetCode()` / `GetCodeSizeBytes()`, no relocation. Instruction/resource counts
are validated against the 16/256/256/12 (+512-instruction) caps before upload.

There is a **second, independent token stream** for declarations/constants
(`GLVertexShaderDeclarationDX`, at object +0x68/+0x70, its own GetCode
interface) — D3D `dcl`/`def`-style tokens binding input attributes to registers
and defining constants. Its per-entry bit layout is the one item not fully
decoded.

### Worked example — fixed-function position transform

`oPos = MVP · iPos` compiles to a **single native instruction** (MatrixMultiply
is not expanded into 4× dp4):

```
0xFFFE0101                        ; vs_1_1 header
0x00000060                        ; MatrixMultiply (native opcode 0x60)
0x80000000 | dest(oPos, wmask)    ; word1
0x80000000 | src (c[MVP], swizzle); word2  -- constant register (MVP base)
0x80000000 | src (v[iPos], swizzle); word3 -- input register (position)
0x0000FFFF                        ; END
```

That a 4×4 transform is one opcode (not four dot products) is a real hardware
capability worth exploiting in a driver's T&L codegen.

## D3D vs_1_1 correspondence

Opcodes `0x01–0x0D` are exactly the D3D vs_1_1 numbering (mov/add/mad/mul/rcp/
rsq/dp4/min/max/slt/sge). Opcodes `0x55–0x63` are **Matrox-native extensions**
emitted verbatim as single instructions: MatrixMultiply `0x60`, Dot3 `0x63`,
Cross `0x57`, Clamp `0x56`, Power `0x55`, Log2 `0x5e`, Exp2 `0x5f`, Frac `0x61`,
Index `0x62`, Round/Floor `0x58`/`0x59`, Insert/ExtractComponent `0x5a`/`0x5b`.
(Note Dot3 and Log2 do **not** use the D3D dp3/log numbers — Parhelia has its own
extended encoding above 0x54.)

## Relationship to G400/G550 WARP

The engine is WARP-lineage (microcode-uploaded geometry engine, same feeding
model as G400/G550). The ISA itself is a generation beyond G550: G550 had
fixed-function hardware T&L (no user-visible vertex ISA); Parhelia exposes a
programmable VS 1.1 instruction set (this document). Whether the low-level
microcode *encoding* is a direct evolution of the G400/G550 WARP microcode is
plausible but unverified — it would need G550 WARP microcode decoded for
comparison.

## Microcode load lifecycle — REVERSED (2026-08-19)

Cross-validated from the genuine Parhelia ICD `opengl6/libGL.so.1.3.0` and the
kernel core `kernel/mtx_parhelia.o`. The WARP vertex engine is microcoded and the
driver loads uCode into its instruction cache before any draw (kernel params in
`kernel/src/MtxParhlParms.c`: `reg3DForcePCIAllocation` = uCode memory type,
`reg3DVECacheMode` = instruction-cache enable, `reg3DPerformCRegCheck` = "Vertex
Engine enabling / CReg check on boot", `reg3DVertexShader`/`…Options`).

### The push-buffer command-record layer [CONFIRMED, libGL]
The ICD emits `[method:u32][count:u32][payload]` records (primitive
`reserve(pb,method,elemSize,count)` @libGL `0x1c9c50`, `commit` @`0x1c9ad0`;
same-method records coalesce). Key histogram: `0xc`=main register bank
(reg=0x1000+idx*4), `0x5`=bind/state, `0x20`=constants, `0x45`/`0x46`=uCode
image, `0x48`=declarations. Upload fn = libGL `0x113700`:
- **key 0x45** (@`0x1137c1`): `reserve(0x45,elemSize=0x670,1)` then
  `memcpy(payload, shaderobj+0x70, 0x664)` — the **uCode image = 1636 bytes =
  409 dwords, verbatim, no relocation**. Wire packet `[0x45][1][image+pad]`.
- **key 0x46** (@`0x113a02`): image + 8-byte monotonic serial prefix (cached
  programs).
- **key 0x48** (@`0x11386c`): `elemSize=0x18` per attribute — the `dcl` binding
  stream (position, diffuse, …).
- **bind key 0x05** on the main buffer: `[0x13][1]`=enable VE program,
  `[0x13][0]`=disable, `[0x14][serial]`=select program.
- constants are a DIFFERENT key **0x20** (`elemSize=0x44`, `[slot][16 floats]`),
  so 0x45/0x46 is unambiguously the program image, not uniforms.

Minimal arm order (cold → drawable): build image → `[0x45]` (or `[0x46]+serial`)
→ `[0x48]` per attribute → `[0x05][0x13,1]` enable → `[0x05][0x14,serial]` select
→ draw. The standalone tool's zero-pixel WARP is exactly the missing 0x45+0x48+
0x05 records: an empty VE instruction cache.

### The uploaded 409-dword image is a DESCRIPTOR, not microcode [CONFIRMED]
The key-0x45 memcpy source `shaderobj+0x70` is a fixed 1636-byte / 409-dword C++
register-binding descriptor (NOT the +0x298 token IR, which is never uploaded).
Byte layout (from the ctor @libGL64 `0x1b84a3` + per-slot init `0x10e390`):
- `[0x000]` (1 dw): attribute/instruction count (default 1, `inc` per bound attr).
- `[0x004..0x5cc]` (10 × 0x94 = 370 dw): ten register-slot descriptors. Each slot:
  enable `[+0x34]=1`; two 0x34-byte component descriptors at `[+0x00]`,`[+0x38]`,
  each `{[0]=index, [4]=count=4, [c]=valid=1, [10]/[1c]/[28]=0xff}`.
- `[0x5cc..0x664]` (38 dw): trailer — `[0x5d0]` dw, four 0x24-byte output-
  activation entries at `[0x5d4]` (`image[0x5d4 + i*0x24]=1` marks output i live:
  0=position,1=color0,…), plus `[+0x8]=4`, `[+0x14]=0x10`.
1+370+38 = 409 dwords. Component/type codes 0xe/0xf/0x10/0x4/0x0 (`0x10da70`).
The driver's own default descriptor is the `.bss` global at libGL64 `0x6a2c40`
(`*(0x65bed8)`, zero in-file). The descriptor is order-independent, no relocation
=> a captured 1636 bytes is reusable verbatim; and it is CONSTRUCTIBLE from this
layout. The residual: which slot index binds position vs diffuse (set by the
token→descriptor compiler, sibling of the fixed-func generator `0x10ddd0`) — and
the KERNEL'S interpretation of the descriptor into actual VE microcode.
2. **Final hardware delivery is inside the closed `mtx_parhelia.o`.** Our
   standalone ring drives the RAW four-index (reg,val) hardware packets directly
   (bypassing the core), but method 0x45 is translated by the core into the real
   VE-cache write, which is NOT a clean extractable MMIO sequence. Ruled out: the
   `mtx514` indirect port (0x910 index/0x938 data/0x914 readback) — its address
   field `(bank<<24)|((index<<9)&0xfff)` addresses only ~7 entries/bank, far too
   few for 409. The 6 microcode variants (dword counts 0x183/0x187/0x18b/0x195/
   0x199=409/0x19d, set in the shader-state ctor `mtx1216` @kern `0x91e70`,
   variant count at struct+0x1544) are selected by pipe/feature. Delivery is thus
   a DMA descriptor / dedicated ring-packet type, recoverable reliably only by a
   **live MMIO/ring capture** of the vendor stack loading one shader.

## Vertex attribute layout — RESOLVED via live capture (2026-09-04)

The per-vertex float layout (the one field four static traces flagged as
capture-only) was captured live from a WORKING original Parhelia (0x0528) on box
.56 running the vendor mtx kernel driver + libGL.so.1.3.0 (md5-identical to the
RE copy). Method: a minimal GLX triangle (glColor3f + glVertex3f, GL_TRIANGLES)
run under gdb; glVertex3f's trampoline (0x1a6ca0) dispatches to the store handler
@VMA **0xc9320**, which appends into a vertex-array buffer at `this+0x2f8`
(base=+0x18, stride=+0xc, count=+0x0). Dumped at glEnd:

- **count=3, stride=20 bytes = 5 floats/vertex, layout = `[x, y, r, g, b]`.**
  Verified against known inputs: V0=(0.5,0.9)+red → `0.5 0.9 1.0 0.0 0.0`;
  V1=(0.15,0.15)+green → `0.15 0.15 0.0 1.0 0.0`; V2=(0.85,0.15)+blue.

So the WARP vertex = **position 2 floats (x,y only) + colour 3 floats (r,g,b)**,
interleaved. z and w are **not streamed** — the position handler sets w=1.0
(const @0x367a0c) and the z-compare is against 0.0 (@0x36992c); both are
pipeline constants. This DISPROVES the earlier guessed `[w,z,y,x][a,b,g,r]`.
The viewport (NDC→screen) transform is CPU-applied (per parhelia-fragment-isa).

**CORRECTION (live MMIO capture, 2026-09-04): per-draw geometry does NOT go to
0x91c.** An mmiotrace of a real draw (captures/mmio-draw-0528*.txt) shows the
draw is submitted through the **DMA ring**: `0x404` = command-buffer address,
then `0x408 = end|3` written repeatedly. **0x91c carries the microcode ROM once
at init only.** The command buffer itself was captured by diffing the
`/dev/matrox/mtx0` mapping between glBegin and glFlush
(captures/cmdbuf-draw-0528-*): it is a stream of **(register-index, value) dword
pairs** (index = (reg-0x1000)/4), i.e. the same indexed register file the ring
consumes — not a separate "tokenised 0x91c stream".

So a from-scratch driver: load the microcode to 0x91c once at init, then per draw
build (index,value) register pairs in a ring buffer and submit via 0x404/0x408 —
the path already proven by our 2D fill. See docs/parhelia-gpu-access-map.md §4-5.
`tools/parhelia-3dtri.c --warpstream` (0x91c geometry) is therefore the WRONG
path for drawing; `--warpring` is the right shape, with the captured state.
Capture harnesses preserved on box .56: /root/tmp/{cap.c,cap2.c,gltest.c,*.gdb}.

## Known static limits

The DX-opcode→native-opcode translation and the absolute microcode-upload
register base sit partly behind the ICD's thread-local dispatch; where a value
can't be pinned statically it is a hardware-capture item, noted inline.
