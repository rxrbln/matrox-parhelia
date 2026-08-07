# Parhelia "Trinity" vertex-shader ISA

Reverse-engineering of the Parhelia programmable vertex shader (the WARP-class
"Trinity" engine), exposed by the driver as `GL_EXT_vertex_shader` and a DX VS
1.1 path. Source: `opengl6/lib64/libGL.so.1.3.0` (function symbols stripped, C++
RTTI intact — navigation is by typeinfo-name → vtable → method). Evidence tags:
[GL]=proven from disasm, [HW?]=needs hardware capture.

This is a work in progress; three passes are filling the opcode table, the
operand encoding, and worked examples. Sections marked *(pending)* are in flight.

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

### Operand word fields (partial — being finalised)

Common bits [GL]: **bit31 = valid/used**, **bit24 = destination flag**,
**swizzle in bits[23:16]** (2 bits/component, identity `0xE4` = w3 z2 y1 x0),
**write-mask in bits[19:16]** (4 bits, `0xF` = all). Destination also carries a
**shape** field in two 2-bit groups [27:26] and [23:22]: scalar/vector/matrix =
`1/2/3` → dest bit patterns `0x04400000 / 0x08800000 / 0x0CC00000`.

The **register selector** (class + index) is pre-encoded into the operand's
low bits, and per-source **modifier** bits (negate/sign/bank) come from
instruction control words. Exact bit split: *(pending — operand-encoding pass)*.

## Opcode table

Word0 opcode constants per instruction. Each `GLVSInstruction` subclass sets its
opcode in its constructor (one confirmed value so far: `0x60`).

*(pending — opcode-table pass; the instruction set from RTTI is: Mov, Add, Sub,
Mul, Madd, Dot3, Dot4, Cross, Min, Max, Clamp, Floor, Frac, Round, Exp2, Log2,
Power, Recip, RecipSqrt, MatrixMultiply, Negate, SetGE, SetLT, ExtractComponent,
InsertComponent, Swizzle, WriteMask, Index.)*

## Register-selector / class encoding

*(pending — operand-encoding pass: how input(16)/temp(256)/const(256)/output(12)
map to bank codes + index ranges in the operand selector field.)*

## Worked example + microcode blob layout

*(pending — the fixed-function T&L generator emits a real program, e.g.
`position = MVP · inputPos`; that gives concrete (opcode, dest, src) tuples to
validate the encoding, plus the serialised blob header/stream format.)*

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
