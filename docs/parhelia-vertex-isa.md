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

**SCOPE CORRECTION (2026-09-05):** everything in PART 1 describes the *libGL
software token IR*, not the hardware. The real WARP machine encoding —
12-byte/96-bit instructions with a completely different field layout and opcode
space — is in **PART 2** at the end of this file. Two PART 1 claims are wrong
and are corrected there: the kernel does **not** assemble DX tokens into
microcode (it concatenates pre-compiled fragments and patches bit-fields), and
`tools/warp_ucode.h` is a WARP **self-test**, not the shared firmware.

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

---

# PART 2 — the WARP hardware microcode ISA (2026-09-05)

Everything above PART 2 describes the **libGL token IR** — the D3D-vs_1_1-shaped
`[opcode][dest][src…]` dword stream the ICD builds in `GLVertexShaderFunctionDX`.
That IR is a *software* representation. This part describes the **actual machine
code the WARP vertex engine executes**, recovered from `mtx_parhelia.o`.

The two are not the same instruction set and must not be conflated.

Evidence tags: **CONFIRMED** = read out of the vendor binary and cross-validated
against independent data; **INFERRED** = consistent with the data but not proven.

Disassembler: `tools/parhelia-warpdis.py` (reads a raw image or
`tools/warp_ucode.h`).

## 1. Instruction word — 12 bytes / 96 bits [CONFIRMED]

One instruction is **three little-endian dwords = 12 bytes = 96 bits**.

The loader `mtx1391` (`.text+0x9f6b0`) proves the framing:

```
WRITE 0x918 = byte_cursor / 12                    ; instruction index, 0..511
for (n = 0; n < len; n += 12) {                   ; len must be a multiple of 12
    WRITE 0x91c = code[0];
    WRITE 0x91c = code[1];
    WRITE 0x91c = code[2];
}
byte_cursor += len;
```

Before the burst `mtx1391` flushes the DMA ring and then polls, in order:
`0x8f0` until bit 17 is set; `0x450` until it equals the fence value at
`hal+0x9c`; `0x908` until `(v & 0xfff7ffbf) == 0x63a78001`; `0x924` until
`(v & 0xfff) == 0x100` (with the `0x908` condition still holding); `0x8f0`
until `(v & 0x3000000) == 0` [CONFIRMED].

`0x918` is written **once** per upload — the engine auto-increments its write
pointer on every third `0x91c` write. The instruction store is bounds-checked
against `0x17ff`, so the WARP holds **0x1800 bytes = 512 instructions**
[CONFIRMED]. A program's hardware entry value is
`((start_instr + entry[variant]) << 2) | (mode & 3)`.

Independent corroboration: every one of the 574 microcode fragments in the
kernel's own fragment registry (§9) has a size that is an exact multiple of 12,
from 12 bytes up to 6744.

## 2. Bit-field map [CONFIRMED — read from the vendor's own patcher]

`mtx3558` (`.text+0xa0f20`) is the microcode linker: it concatenates canned
fragments into a program buffer and then applies **field patches**. Its patch
dispatcher is a 36-entry jump table at `.rodata+0x2eb0`, and each handler
writes exactly one named bit-field. That jump table *is* the vendor's field map.

Patch record = `{u32 instr_index, u32 field_kind, u32 value}`; the instruction
is addressed as `program + instr_index*12`.

| field | bits | patch kind | meaning |
|---|---|---|---|
| `wmask0` | w0[3:0] | 0x00 | write mask, destination port **A** |
| `dst0` | w0[7:4] | 0x01 | destination register, port **A** |
| `f0a` | w0[9:8] | 0x02 | unidentified |
| `wmask1` | w0[13:10] | 0x03, 0x1c | write mask, destination port **B** |
| `dst1` | w0[17:14] | 0x04, 0x1d | destination register, port **B** |
| `f0b` | w0[18] | 0x1e | unidentified |
| `f0c` | w0[19] | 0x1f | unidentified (kind 0x05 patches w0[19:18] as a pair) |
| `src2` | w0[23:20] | 0x06 | source operand **2** register (low 4 bits) |
| `f0d` | w0[24] | 0x07 | unidentified (kind 0x20 patches w0[24:20] as 5 bits) |
| `f0e` | w0[26:25] | 0x08 | unidentified |
| `src1` | w0[30:27] | 0x09 | source operand **1** register (low 4 bits) |
| `f0f` | w0[31] | 0x0a | unidentified (kind 0x21 patches w0[31:27] as 5 bits) |
| `f1a` | w1[1:0] | 0x0b, 0x12 | unidentified |
| `src0` | w1[5:2] | 0x0c | source operand **0** register (low 4 bits) |
| `f1b` | w1[6] | 0x0d | unidentified (kind 0x22 patches w1[6:2] as 5 bits) |
| `f1c` | w1[8:7] | 0x0e | unidentified |
| `f1d` | w1[12:9] | 0x0f | unidentified |
| `f1e` | w1[13] | 0x10 | unidentified |
| `exthi` | w1[16:14] | 0x11 | high 3 bits of one operand's register index |
| `extsel` | w1[19:17] | — | which operand `exthi` applies to (0/1/2 = src0/1/2) |
| `f1f` | w1[21:20] | 0x13 | unidentified |
| `f1g` | w1[24:22] | 0x14 | unidentified |
| `f1h` | w1[26:25] | 0x15 | constant/matrix bank select (see §7) |
| `f1i` | w1[29:27] | 0x16 | unidentified |
| `swzA` | {w2[9:0], w1[31:30]} | 0x17 | 12-bit swizzle, source operand 0 |
| `swzB` | w2[21:10] | 0x18 | 12-bit swizzle, source operand 1 |
| `f2a` | w2[22] | 0x19 | unidentified flag |
| `opcode` | w2[30:23] | 0x1a | opcode |
| `f2b` | w2[31] | 0x1b | opcode class flag |
| `imm32` | {w2[21:0], w1[31:22]} | 0x23 | 32-bit immediate (overlays both swizzles) |

`swzA` is reassembled as `(w2[9:0] << 2) | w1[31:30]`; `imm32` as
`((w2 & 0x3fffff) << 10) | (w1 >> 22)`.

Only `w1[19:17]` is not reachable through a patch kind — it is written by the
relocator (§4), which is how its meaning was established.

## 3. Operand model [CONFIRMED]

- **Three source register slots**: `src0` = w1[5:2], `src1` = w0[30:27],
  `src2` = w0[23:20]. Four bits each.
- **Two destination write ports**, each with its own 4-bit register index and
  4-bit component write mask: port A (`dst0`/`wmask0`) and port B
  (`dst1`/`wmask1`). A mask of 0 means that port does not write. Both ports are
  used simultaneously in real code, so the engine is at least dual-write-port
  [CONFIRMED]; whether that also means dual-issue ALU is [INFERRED].
- **Swizzles**: `swzA` selects the components of source 0, `swzB` those of
  source 1. **3 bits per component**, component *c* at bits `[3c+2:3c]`.
  Identity `xyzw` = `0x688`. Codes 0–3 are x/y/z/w; codes 4–7 are extra
  selectors (constants such as 0.0/1.0) [INFERRED — 4 is by far the most common
  and appears exactly where a homogenising `w = 1` is expected].
  Source 2 has no swizzle field.

Confirmation that identity really is `0x688`: it is by a wide margin the single
most common value of each field, both fields are simultaneously `0x688` in 9604
of 39,953 instructions (24%, and the arithmetic opcodes are only a minority of
the corpus), and it decodes to the 3-bit sequence 0,1,2,3.

## 4. Register file — 128 entries, one extended operand per instruction [CONFIRMED]

The relocation loop in `mtx3558` (`.text+0xa0ff0`) rewrites a register index by
`+7 * value` and writes the result as a **7-bit** quantity split across two
fields:

```
kind 0: idx = word1[5:2]   (src0);  word1[19:17] = 0
kind 1: idx = word0[30:27] (src1);  word1[19:17] = 1
kind 2: idx = word0[23:20] (src2);  word1[19:17] = 2
        new7 = idx + 7*value
        <that operand's 4-bit field> = new7 & 0xf
        word1[16:14]                 = (new7 >> 4) & 7
```

So the register file is **128 vec4 registers**, addressed as
`{exthi(3) : idx(4)}`, but only **one operand per instruction** may carry the
high three bits — `extsel` names it. Operands not named by `extsel` are limited
to registers 0–15. Highest extended index actually observed in the vendor
library: **121**.

The relocation step is `7` registers per unit, i.e. the driver's per-stage
constant blocks are 7 registers wide [INFERRED].

`extsel = 2, exthi = 0` — the inert encoding — covers 30,646 of 39,953
instructions (77%), so most code stays inside registers 0–15. `extsel` values
3–7 also occur and are frequently paired with a non-zero `exthi` (e.g.
`extsel = 7, exthi = 4` appears 2278 times, `extsel = 4, exthi = 5` 748 times),
so the field is a 3-bit code of which the relocator only ever emits 0/1/2. The
remaining codes plausibly extend a destination port or select a different
addressing mode; that is unresolved.

## 5. The 32-bit immediate and branches [CONFIRMED]

Patch kind 0x23 writes a **32-bit immediate** spanning `w1[31:22]` (low 10 bits)
and `w2[21:0]` (high 22 bits) — exactly the two swizzle fields, so an
immediate-carrying instruction has no swizzles. This is validated independently:

- opcode **0x23** (2523 instructions): immediates are small signed integers, and
  `i + 1 + imm` lands **inside the fragment in 2523 of 2523 cases** — it is a
  PC-relative branch, delta in instructions.
- opcode **0x46** (625 instructions): immediates are dominated by small
  *negative* values (−1, −2, −17, −20 …) and `i + 1 + imm` is in range in
  620/625 cases — a backward branch / loop.

Sign extension falling exactly on bit 31 with sensible negative deltas is strong
evidence that the field split is right.

## 6. Opcode field [w2[30:23] CONFIRMED as the opcode; mnemonics mostly INFERRED]

66 distinct opcode values occur across the 39,953 instructions of the analysed
corpus (the 574-fragment registry plus the self-test and a few unregistered
`.data` images). The field partitions the corpus cleanly by *shape* — which operand and
destination fields are live — which is what an opcode field does.

Census (opcodes with ≥100 uses; `wrA`/`wrB` = how many write that port,
`swzId` = how many have both swizzles = identity):

| op | count | wrA | wrB | swzId | character |
|---|---|---|---|---|---|
| 0x71 | 4929 | 787 | 4929 | 302 | ALU — vendor symbols `AddEfs/AddEfd/AddEfm` |
| 0x60 | 3268 | 3191 | 0 | 3138 | writes port A only |
| 0x41 | 2976 | 0 | 0 | 0 | control, carries immediate |
| 0x76 | 2909 | 0 | 2909 | 1991 | ALU — vendor symbols `AddAfs/AddAfd` |
| 0x73 | 2740 | 289 | 2728 | 2348 | ALU, dominant first instruction of T&L fragments |
| 0x23 | 2523 | 0 | 53 | 0 | **relative branch** |
| 0x00 | 1900 | 0 | 0 | 0 | **NOP** — 1899 of 1900 are the single encoding `00000200 00040000 00000000` |
| 0x0a | 1776 | 1776 | 1776 | 0 | writes both ports |
| 0x01 | 1736 | 0 | 0 | 0 | control, immediate = 0 |
| 0x35 | 1680 | 0 | 0 | 0 | control, small immediates |
| 0x34 | 1453 | 0 | 0 | 0 | control, small immediates |
| 0x78 | 1095 | 507 | 1095 | 726 | ALU |
| 0x43 | 946 | 0 | 0 | 0 | control, bit-pattern immediates |
| 0x33 | 825 | 0 | 825 | 0 | — |
| 0x0d | 819 | 0 | 0 | 0 | control |
| 0x42 | 810 | 0 | 0 | 0 | control |
| 0x75 | 774 | 97 | 774 | 382 | ALU, one per light in the FF families |
| 0x05 | 632 | 0 | 9 | 0 | control |
| 0x46 | 625 | 0 | 0 | 0 | **backward branch / loop** |
| 0x89 | 580 | 523 | 580 | 5 | ALU |
| 0x77 | 573 | 178 | 573 | 456 | ALU — the dp4 used by matrix transforms (§7) |
| 0x02 | 512 | 0 | 3 | 0 | control |
| 0x44 | 357 | 0 | 135 | 0 | control |
| 0x24 | 353 | 0 | 0 | 0 | control, many distinct immediates |
| 0x04 | 321 | 0 | 30 | 0 | control |
| 0x2b | 316 | 0 | 0 | 0 | control |
| 0x03 | 225 | 0 | 0 | 0 | control |
| 0x27 | 215 | 0 | 0 | 0 | control |
| 0x81 | 194 | 0 | 0 | 21 | — |
| 0x25 | 188 | 0 | 0 | 0 | control |
| 0x63 | 179 | 6 | 84 | 0 | — |
| 0x2f | 146 | 0 | 0 | 0 | control |
| 0x83 | 136 | 0 | 0 | 136 | — |
| 0x4a | 127 | 127 | 127 | 0 | — |
| 0x2e / 0x2d | 126 each | 0 | 0 | 0 | control |
| 0x2c | 116 | 0 | 0 | 0 | control |
| 0x52 | 102 | 0 | 0 | 0 | control |

Remaining opcodes seen: 0x29, 0x64, 0x65, 0x66, 0x67, 0x82, 0x84, 0x86, 0x8a,
0x8b, 0xfe and 17 more with fewer than 20 uses each.

**Opcode 0x29 = END [CONFIRMED].** It occurs 28 times, always as the *identical*
encoding `00000200 00040000 14c00000`, and always one instruction before the end
of its fragment (27 of 28; once three before). Together with `0x00` = NOP these
are the two opcodes whose meaning is settled by encoding alone.

**Independent cross-check of the opcode field.** A second, separately extracted
corpus — 141 `.data` blobs of `mtx_parhelia.o`, 422,796 bytes, 35,233
instructions, selected by a different filter — yields 49 distinct values of
`w2[30:23]` with the same distribution shape (0x71: 4332, 0x60: 3074, 0x41:
2963, 0x76: 2629, 0x23: 2517, 0x35: 1665, 0x34: 1444 …). Two corpora built by
different criteria agreeing on the field and its alphabet is what makes
`w2[30:23]` a confirmed opcode rather than a plausible one.

Structure of the value space [INFERRED]: `0x00–0x0f` and `0x23–0x52` never write
a destination register and carry immediates — control and flow. `0x60–0x67`,
`0x71–0x78`, `0x81–0x8b` are the arithmetic opcodes. The high nibble looks like
a functional class (6 = writes port A only, 7 = the main ALU) and the low nibble
a variant/routing selector; that split is not proven.

**Two opcode names are CONFIRMED** because five fragments in `.data` escaped name
mangling:

| symbol | words | decoded |
|---|---|---|
| `AddAfs` | `44087e00 2803401b bb2221a2` | `add.a b1.xyzw, r6.xyzw, r88.xyz4` |
| `AddAfd` | `44087e00 28034017 bb2221a2` | `add.a b1.xyzw, r5.xyzw, r88.xyz4` |
| `AddEfs` | `30087e00 00440100 b89a2222` | `add.e b1.xyzw, r0.xyz4, r6.xyzw` |
| `AddEfd` | `28087e00 00440100 b89a2222` | `add.e b1.xyzw, r0.xyz4, r5.xyzw` |
| `AddEfm` | `38087e00 28034103 b89a2222` | `add.e b1.xyzw, r0.xyz4, r87.xyzw` |

`AddAfs`/`AddAfd` differ **only** in `src0` (6 vs 5); `AddEfs`/`AddEfd` differ
**only** in `src1` (6 vs 5). The same `s`/`d` naming maps to the same register
numbers through two different operand slots — this is what pins `w1[5:2]` and
`w0[30:27]` as register-index fields, and it is independent of the patcher.

`Add` maps to opcode 0x76 in the `A` form and 0x71 in the `E` form, so the
opcode field encodes operand routing as well as the operation [INFERRED].

**There is no IR→hardware opcode table anywhere in the vendor stack
[CONFIRMED negative].** `MTXPARD.dll`, `MTXOGL.dll`, `MTXPARM.sys`,
`mtxparmx.sys`, `MtxEscape.dll`, `MtxCIP2.dll`, `libGL.so.1.3.0` and
`mtx_parhelia.o` were all scanned for an array at stride 1/2/4/8/12/16/20
holding the hardware opcode values — nothing. `imul reg,reg,0xc` (a 12-byte
stride index) occurs zero times in either `libGL64` or `mtx_parhelia.o`, and no
opcode-indexed switch on the IR range exists in the Linux binaries at all
(`cmp reg,0x63` / `cmp reg,0x64`: zero occurrences).

The one opcode table that does exist is in the Windows D3D driver
`MTXPARD.dll` — a *validation* table, at file offset 0x1cdf68 (VA 0x1ddf68),
0x64 pointer entries into a 0x14-stride descriptor array at 0x1cdc30, read by
the thunk at VA 0x198890. Each descriptor is
`{const char *mnemonic; u8 macro_cost; u8 token_len_dwords; u8 dest_write_mask;
u16 src_read_mask[4]; u32 nsrc}` — no microcode payload.

So the kernel does **not** assemble microcode: it selects and concatenates
pre-compiled fragments and patches their bit-fields. This corrects the earlier
claim in PART 1 that "the DX-token→WARP-microcode assembly is completed in the
kernel".

**The microcode library is shared between Linux and Windows [CONFIRMED].**
`MTXPARD.dll` carries 158 microcode blobs behind an equivalent registry (16-byte
records, `.text` 0x65400 / 0x71f00, `ctx+0x53510`). **30 blobs are byte-identical
between `mtx_parhelia.o` and `MTXPARD.dll`** — e.g. kernel file offset 0x1630a0
== windows file offset 0x192308, and a 4608-byte / 384-instruction table at
kernel 0x161d38 == windows 0x1924e8. One WARP microcode library, two operating
systems: a good sanity net for anyone re-implementing the engine.

## 7. Worked example — a 4×dp4 matrix transform [CONFIRMED]

`mtx368` (`.data+0x83280`, 276 bytes, 23 instructions) is texture-coordinate
generation. Instructions 1–4, 5–8, 11–14 and 16–19 are four identical blocks:

```
  1: op77.q  b5.x        r0.xyzw, r0.xyzw, r0
  2: op77.q  b5.y        r0.xyzw, r1.xyzw, r0
  3: op77.q  b5.z        r0.xyzw, r2.xyzw, r6
  4: op77.q  b5.w        r0.xyzw, r3.xyzw, r7
```

Write mask sweeps `x, y, z, w`; `src1` sweeps the four matrix rows `r0..r3`;
`src0` is the vector being transformed. That is a textbook 4×DP4 modelview
transform, and it is exactly the signature a correct ISA decode must produce.

Across the four blocks the only other field that changes is `f1h` (w1[26:25]),
which counts 0, 1, 2, 3 — so **w1[26:25] selects the constant/matrix bank**
(four texture matrices) [INFERRED, but the four-blocks-four-banks correlation is
exact].

## 8. What `tools/warp_ucode.h` actually is [CONFIRMED — corrects the premise]

`mtx2785` (`.data+0x84ec0`, 0x12cc bytes, **401 instructions**) is **not** the
family firmware and **not** a T&L program. Its only user is `mtx676`, a WARP
**self-test**, which builds a stack descriptor `{len = 0x12cc, code = mtx2785,
nblocks = 4, entry[8] = 0}` and calls `mtx1391` with `reset = 1`.

Disassembled, its shape is unmistakable:

```
   4: wra  a0.xyzw   ...
   5: wra  a1.xyzw   ...
  10: wra  a0.xyzw   ...
  11: wra  a1.xyzw   ...
   ...                                   (384 instructions)
 393: wra  a15.xyzw  ...
```

384 = 24 blocks × 16. Each block sweeps destination port A over registers 0–15
with a full `xyzw` mask; across blocks only `w1[13]`, `w1[21:20]` and
`w1[26:25]` change (2 × 4 × 4 = 32 combinations, of which 24 are used). It is a
register-file / write-path sweep, bracketed by a handful of control instructions
and NOPs. Byte-identity between the original Parhelia and the P690 is expected
for a self-test and says nothing about the shader pipeline.

The real microcode is the fragment library in §9.

## 9. The microcode fragment library [CONFIRMED]

`mtx3882` (`.text+0x97030`, 24153 bytes) builds a **575-slot registry** at
`dev+0x9c108`, stride 0x18:

```c
struct warp_fragment { u32 len_bytes; u32 pad; const u32 *code; void *loaded; };
```

Slot 0 is a NULL sentinel; slots 1–574 each point at one `.data` blob. The
`len_bytes` field matches the ELF symbol size for **574 of 574** entries.
Fragment sizes run from 12 bytes (1 instruction) to 6744 bytes (562
instructions); the registry totals 468,504 bytes = **39,042 instructions**.

Programs are built at state-change time by `mtx3558`: walk a recipe list of
`{fragment_index, n_field_patches, n_relocations}`, `memcpy` each fragment into
the program buffer, apply the relocations (§4) and the field patches (§2), then
hand the buffer to `mtx1391`.

For an open driver this means a from-scratch vertex pipeline does **not** need a
WARP assembler to get started: a hand-built sequence of these 96-bit
instructions, uploaded through `0x918`/`0x91c`, is sufficient, and the fragment
library is a validated corpus of working code to imitate.

## 10. Confidence and what is still open

**Confirmed:** the 12-byte instruction word; the complete bit-field map
(from the vendor's own patcher, not guessed); three source slots and two
destination write ports with per-port write masks; the 128-entry register file
and its one-extended-operand addressing; the 12-bit 3-bits-per-component swizzle
with identity `0x688`; the 32-bit immediate split; opcode `0x23` = relative
branch and `0x46` = backward branch; `w2[30:23]` as the opcode field;
`Add` = 0x76/0x71; the 4×dp4 transform idiom; the 512-instruction store; that
`mtx2785` is a self-test.

**Inferred, not proven:** the mnemonic for every opcode other than the branches
and `Add`; whether the two write ports imply dual-issue; the meaning of swizzle
codes 4–7; `w1[26:25]` as a matrix-bank select; the high-nibble/low-nibble
reading of the opcode field.

**Unresolved:** 16 bit-fields carry no interpretation yet — `w0[9:8]`, `w0[18]`,
`w0[19]`, `w0[24]`, `w0[26:25]`, `w0[31]`, `w1[1:0]`, `w1[6]`, `w1[8:7]`,
`w1[12:9]`, `w1[13]`, `w1[21:20]`, `w1[24:22]`, `w1[29:27]`, `w2[22]`, `w2[31]`.
They are almost certainly source modifiers (negate/absolute/saturate), operand
bank selectors and per-port routing.

The best lead among them is **`w0[19]`**: it is the *only* bit that ever differs
on its own between two otherwise byte-identical library instructions (both
`AddEfs`/`AddEfd`/`AddEfm` and many others have an exact twin that differs in
w0[19] alone), it is set in 33% of the corpus, and it is set in every single
opcode-0x60 instruction. That is the profile of a one-bit operand or result
modifier — negate or accumulate. Closing it wants either a live capture
correlating a known GL shader with the fragments it selects, or the Windows D3D
driver's vs_1_1 compiler.

**No hardware validation has been done.** Nothing in PART 2 has been executed on
a Parhelia; it is all static analysis of `mtx_parhelia.o` cross-checked against
itself.
