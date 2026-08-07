# Parhelia "Trinity" vertex-shader ISA

Reverse-engineering of the Parhelia programmable vertex shader (the WARP-class
"Trinity" engine), exposed by the driver as `GL_EXT_vertex_shader` and a DX VS
1.1 path. Source: `opengl6/lib64/libGL.so.1.3.0` (function symbols stripped, C++
RTTI intact — navigation is by typeinfo-name → vtable → method). Evidence tags:
[GL]=proven from disasm, [HW?]=needs hardware capture.

**Headline:** the vertex microcode is a **DirectX vs_1_1-shaped token stream**
carrying Parhelia-native opcodes. A program is literally
`[0xFFFE0101] + instruction tokens + [0x0000FFFF]` — the D3D version header and
END token bracket a stream of `[opcode][dest][src…]` instructions — and it is
uploaded to the engine as a **verbatim memcpy** (no relocation). This is the
single most useful fact for writing a Parhelia vertex-shader assembler: it is
"D3D vs.1.1 bytecode with Matrox extended opcodes."

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

Register bank in operand bits **[30:28]**, index in **[7:0]**:

| File | Capacity | Bank code [30:28] |
|---|---|---|
| output | 12 | `000` (0x00000000) |
| input | 16 | `001` (0x10000000) |
| constant | 256 | `010` (0x20000000) |
| temp | 256 | unconfirmed (likely `000` disambiguated by index range, or set in the lazy allocator) |

The constant file is **dual-ended**: user constants (`c#`) grow up from index 0,
driver-generated constants grow down from the top — worth knowing when allocating
uniform slots. Temps are allocated lazily at emit time (resolver `0x170100`).

**Two residual items** (flagged for hardware capture):
- **Per-source negate/abs/sign modifiers** — the active source encoder ORs only
  regsel|swizzle|valid, injecting no modifier bits. Negate (which distinguishes
  Negate from Mov and Sub from Add) is applied during symbol→operand lowering
  elsewhere, or via the high-level swizzle node's 6-bit-per-component codes
  (which carry sign/ZERO/ONE); its exact landing bit wasn't pinned. Capture a
  shader using a negated source and diff the source word.
- **Temp bank code** [30:28] — not located.

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

## Known static limits

The DX-opcode→native-opcode translation and the absolute microcode-upload
register base sit partly behind the ICD's thread-local dispatch; where a value
can't be pinned statically it is a hardware-capture item, noted inline.
