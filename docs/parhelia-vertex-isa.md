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
`tools/warp_ucode.h` is a WARP **self-test**, not the shared firmware — the
programs the driver really loads are listed in PART 2 §12.

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

**CORRECTIONS (2026-09-05):**
- `Swizzle` is **`0x5c`**, not `0x01`. `glSwizzleEXT`'s handler (libGL64
  0x160b83–0x160f36) allocates a 0x48-byte node and stores `0x5c` at 0x160f27,
  packing the four selectors twice: `+0x3c = Σ sel_i<<(2i) | class_i<<(24+2i)`
  (class 0 = plain, 1 = negate, 2 = constant) and `+0x40 = Σ sel6_i<<(6i)`,
  default 0x20. `glWriteMaskEXT` is a *separate* handler at 0x160814 (0x40-byte
  node, opcode `0x01` at 0x160867) which ORs the dest write-mask bits 19:16.
- **`0x00, 0x08, 0x0e–0x18, 0x4e, 0x4f, 0x5d` are NOT reserved.** The IR opcode
  space is the D3DSIO enum extended in place, which is exactly why no DX→native
  LUT exists. From `MTXPARD.dll`'s validation table: `00 nop, 01 mov, 02 add,
  04 mad, 05 mul, 06 rcp, 07 rsq, 08 dp3, 09 dp4, 0a min, 0b max, 0c slt,
  0d sge, 0e exp, 0f log, 10 lit, 11 dst, 13 frc, 14 m4x4, 15 m4x3, 16 m3x4,
  17 m3x3, 18 m3x2, 4e expp, 4f logp, 55 mpow, 56 mclamp, 57 mcross, 58 mround,
  59 mfloor, 5a minsert, 5b mextract, 5c mswizzle, 5d mwrmask, 5e mlog,
  5f mexp, 60 mmult, 61 mfrac, 62 mindex, 63 mdp3`. Only `0x03` (sub) and
  `0x12` (lrp) are genuinely absent.
- Note that this IR numbering has **nothing to do** with the hardware opcode
  values in PART 2 — IR `0x60` (mmult) and hardware `0x60` are unrelated.

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
   `mtx514` indirect port (0x910 index/0x938 data/0x914 readback) — it is a
   register-file *read* port, not a microcode path (see PART 2 §11; the address
   is `(kind<<24)|(((bank<<9)|index)&0xfff)`, i.e. 4 banks × 512 entries, and
   the earlier "~7 entries/bank" was a misreading of the shift). The 6 microcode variants (dword counts 0x183/0x187/0x18b/0x195/
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
`tools/warp_ucode.h`). Sections are in derivation order; if you only want the
answers, read **§11** (the register file and the CPU readback port), **§12**
(the eight programs the driver really loads) and the resolution of the `mtx676`
question at the end.

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

**How the two halves of the table relate (2026-09-05, re-read handler by
handler).** `mtx3558` copies the instruction it is about to patch into **two
identical three-dword stack slots** (`[rsp+0x10..0x18]` and `[rsp+0x20..0x28]`,
written at `.text+0xa10b7..0xa10d0`) and both groups of handlers write back the
same 12 bytes. Kinds `0x00–0x1b` operate on one slot, kinds `0x1c–0x23` on the
other — so the "duplicates" are **alternative views of the same fields**, not a
second instruction. Three of those alternates are *wider* than their
counterparts, and that is what fixes the operand widths:

| wide kind | bits written in one go | narrow kinds covering the same bits |
|---|---|---|
| 0x20 | w0[24:20] | 0x06 (w0[23:20]) + 0x07 (w0[24]) |
| 0x21 | w0[31:27] | 0x09 (w0[30:27]) + 0x0a (w0[31]) |
| 0x22 | w1[6:2] | 0x0c (w1[5:2]) + 0x0d (w1[6]) |
| 0x05 | w0[19:18] | 0x1e (w0[18]) + 0x1f (w0[19]) |

A named-field patch that writes five contiguous bits as one value is only
meaningful if those five bits *are* one field. So the three source register
selects are **5 bits wide**, and the former `f0d`/`f0f`/`f1b` are their high
bits, not separate flags [CONFIRMED]. Likewise w0[19:18] is a single 2-bit
field (`f0bc`), not two flags.

| field | bits | patch kind | meaning |
|---|---|---|---|
| `wmask0` | w0[3:0] | 0x00 | write mask, destination port **A** |
| `dst0` | w0[7:4] | 0x01 | destination register index, port **A** (low 4 bits of `dstA`) |
| `f0a` | w0[9:8] | 0x02 | port-A control; `2` = port A idle (§11) |
| `wmask1` | w0[13:10] | 0x03, 0x1c | write mask, destination port **B** |
| `dst1` | w0[17:14] | 0x04, 0x1d | destination register, port **B** |
| `f0bc` | w0[19:18] | 0x05, 0x1e, 0x1f | unidentified 2-bit field |
| `src2` | w0[24:20] | 0x20 (0x06+0x07) | source operand **2** register, **5 bits** |
| `f0e` | w0[26:25] | 0x08 | unidentified |
| `src1` | w0[31:27] | 0x21 (0x09+0x0a) | source operand **1** register, **5 bits** |
| `f1a` | w1[1:0] | 0x0b, 0x12 | unidentified (port-B related, §11) |
| `src0` | w1[6:2] | 0x22 (0x0c+0x0d) | source operand **0** register, **5 bits** |
| `f1c` | w1[8:7] | 0x0e | port-B control; `0` = port B idle (§11) |
| `f1d` | w1[12:9] | 0x0f | unidentified |
| `dpgA2` | w1[13] | 0x10 | **port-A destination page, bit 4** (= `dstA[8]`, §11) |
| `exthi` | w1[16:14] | 0x11 | high 3 bits of one source operand's register index |
| `extsel` | w1[19:17] | — | which operand `exthi` applies to (0/1/2 = src0/1/2) |
| `dpgA0` | w1[21:20] | 0x13 | **port-A destination page, bits 1:0** (= `dstA[5:4]`) |
| `f1g` | w1[24:22] | 0x14 | unidentified |
| `dpgA1` | w1[26:25] | 0x15 | **port-A destination page, bits 3:2** (= `dstA[7:6]`) |
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

- **Three source register slots**: `src0` = w1[6:2], `src1` = w0[31:27],
  `src2` = w0[24:20]. **Five bits each** (see the wide-patch argument in §2).
- **Two destination write ports**: port A writes a **9-bit** register index
  `dstA` = {w1[13], w1[26:25], w1[21:20], w0[7:4]} (§11); port B writes the
  4-bit `dst1`. Each has its own 4-bit component write mask, and a mask of 0
  means that port does not write. Both ports are used simultaneously in real
  code, so the engine is at least dual-write-port [CONFIRMED]; whether that also
  means dual-issue ALU is [INFERRED].
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
        <that operand's low 4 bits> = new7 & 0xf
        word1[16:14]                = (new7 >> 4) & 7
```

So a relocated source register is **7 bits**, addressed as `{exthi(3):idx(4)}`,
and only **one operand per instruction** may carry the high three bits —
`extsel` names it. Highest extended index actually observed in the vendor
library: **121**.

**Tension with the 5-bit source field, recorded [UNRESOLVED].** §2 shows the
source selects are 5-bit fields, yet the relocator only ever touches their low
4 bits plus `exthi`. Either the source address is `{exthi(3):idx(4)}` and the
5th bit (w1[6] / w0[31] / w0[24]) is a per-source modifier that the vendor's
patch API happens to write together with the index, or it is a genuine address
bit that the relocator never needs because relocated operands always have it
clear. The corpus is consistent with both: the 5th bit is set in 9 % / 3 % /
11 % of instructions respectively and is essentially never combined with a
non-zero `exthi` (e.g. `src2[4]=1` occurs 4485 times, 4483 of them with
`extsel=2, exthi=0`). Deciding this needs one hardware experiment: write a
known value to a high register and read it back through the port in §11.

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

**The vendor's own code decodes this field.** `mtx3395` (`.text+0xa6940`) is a
walker over a microcode program:

```
a6946: add  rdx, 8              ; rdx = &instr.word2
a6950: mov  eax, [rdx]
a6952: shr  eax, 0x17           ; >> 23
a6955: and  eax, 0xff           ; = w2[30:23]
a695a: sub  eax, 0x29
a695d: cmp  eax, 0x6b           ; valid opcode range 0x29 .. 0x94
a6960: ja   next
a6964: jmp  [.rodata+0x3040 + rax*8]   ; 108-entry jump table
next:  inc  ecx
a6972: add  rdx, 0xc            ; 12-byte instruction stride
a6976: cmp  ecx, esi
a6978: jb   a6950
```

That single function independently confirms **both** the 12-byte instruction
stride and the exact opcode field. I extracted the 108-entry table: it has 108
relocated entries and only **two** distinct targets — index 0 (opcode `0x29`)
jumps to the function's `ret`, the other 107 fall through to the loop
increment. So the routine is "scan forward until opcode `0x29`", which is what
independently establishes `0x29` as **end-of-program**. (No per-opcode handlers,
so this table yields no mnemonics.)

Note the walker's range `0x29..0x94` does **not** cover `0x00`, `0x0a` or `0x23`,
which are all heavily used. Either those form a second instruction class this
particular scanner does not classify, or the range is deliberately narrowed to
the arithmetic subrange — do not assume one flat opcode space.

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

**Opcode 0x29 = END [CONFIRMED].** Three independent lines agree: `mtx3395`
scans for exactly this value and stops; it occurs 28 times in the corpus, always
as the *identical* encoding `00000200 00040000 14c00000`; and it always sits one
instruction before the end of its fragment (27 of 28; once three before).
`0x00` = NOP is likewise settled by encoding alone — 1899 of its 1900
occurrences are the single word `00000200 00040000 00000000`.

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

**CORRECTION (2026-09-05): w1[26:25] is not a matrix bank.** Across the four
blocks the other fields that change are `dst1` (5, 6, 9, 6) and w1[26:25]
(0, 1, 2, 3), and §11 shows w1[26:25] to be two bits of the **port-A
destination** address. The former "four texture matrices" reading is withdrawn:
it required a *source* bank, and the self-test disproves that these fields page
the sources (§11). What the four blocks share is the same matrix rows `r0..r3`;
what differs is where each block's four dot products land.

## 8. What `tools/warp_ucode.h` actually is [CONFIRMED — corrects the premise]

`mtx2785` (`.data+0x84ec0`, 0x12cc bytes, **401 instructions**) is **not** the
family firmware and **not** a T&L program. Its only user is `mtx676`, a WARP
**self-test**, which builds a stack descriptor `{len = 0x12cc, code = mtx2785,
nblocks = 4, entry[8] = 0}` and calls `mtx1391` with `reset = 1`.

Disassembled with the corrected 9-bit destination decode (§11), its shape is
unmistakable — a flat ascending sweep of the whole register file:

```
   4: wra  a0.xyzw    r0.xyzw, r0.xyzw, r2
   5: wra  a1.xyzw    r0.xyzw, r0.xyzw, r3
   ...
  10: wra  a0.xyzw    r0.xyzw, r0.xyzw, r0
  11: wra  a1.xyzw    r0.xyzw, r0.xyzw, r1
   ...                                       (384 instructions, index += 1)
 393: wra  a383.xyzw  r0.xyzw, r0.xyzw, r1
```

384 = 24 pages × 16. `src2` alternates 0/1 in lockstep with the destination's
low bit, which is exactly the alternation the verifier expects (§11). It is a
register-file / write-path sweep, bracketed by a handful of control instructions
and NOPs, and it ends with `end` at instruction 397. Byte-identity between the
original Parhelia and the P690 is expected for a self-test and says nothing
about the shader pipeline.

The real microcode is the fragment library in §9 and the default program set in
§12.

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

## 11. The port-A destination register file — 512 entries, 4 banks [CONFIRMED]

This section resolves five of the sixteen fields that had no interpretation, and
it does so from the vendor's own self-test rather than from correlation.

### The readback port

`mtx612` (`.text+0xa0120`) is the self-test's verifier. It reads the WARP
register file back through the indirect port and compares:

```c
for (bank = 0; bank <= 3; bank++)
    for (idx = 0; idx <= 0x17f; idx++) {          /* 384 entries */
        mtx514(this, hal, /*kind=*/3, bank, idx); /* -> 4 dwords */
        expected = (idx & 1) ? patternB : patternA;
        if (any of the 4 components differs) { fail[idx] = 0; ok = 0; }
    }
```

`mtx514` (`.text+0x8f7c0`) is the port itself:

```c
addr = (kind << 24) | (((bank << 9) | index) & 0xfff);
memWriteDword(h, 0x910, addr);
memWriteDword(h, 0x938, 0xfffffffc);
for (i = 0; i < 4; i++) v[i] = memReadDword(h, 0x914);
```

So the index field is **9 bits** and there are **4 banks** — 4 × 512 vec4
entries. (This retires the earlier note in "Microcode load lifecycle" that
`mtx514` "addresses only ~7 entries per bank"; the `bank << 9` shift was
misread.) The four banks hold the same data in the self-test, which is what one
expects if they are the four parallel vertex units of a WARP quad [INFERRED].

`kind` is validated against `kind <= 10` and two bitmasks: kinds 0/1/3/5 take
this path, kinds 2/4/8/9/10 take a second one. Only **kind 3** is used to read
the vertex register file [CONFIRMED].

### The address the microcode writes

The self-test image writes 384 destinations and the verifier checks 384
entries, so the microcode's port-A destination must be a 9-bit address. The
fields that vary across the sweep are exactly `w0[7:4]` (0..15, innermost),
`w1[21:20]` (0..3), `w1[26:25]` (0..3) and `w1[13]` (0..1) — 2 × 4 × 4 = 32
pages of 16 = **512**, matching the port's index width exactly. 24 of the 32
pages are used (`w1[13]=1` only with `w1[26:25]` ∈ {0,1}), giving 384.

Only one assignment of those five page bits produces a contiguous 0..383, which
is what the verifier's linear scan requires:

```
dstA = (w1[13] << 8) | (w1[26:25] << 6) | (w1[21:20] << 4) | w0[7:4]
```

Decoding the image that way turns it into a perfectly ordered `a0, a1, … a383`
sweep (`tools/parhelia-warpdis.py tools/warp_ucode.h`), which is the check that
promotes this from plausible to settled: any other bit order scrambles the
sequence.

### What this rules out

These five bits do **not** page the source operands. The self-test's operands
(`src2` = 0/1, the two registers holding the test patterns) are identical in
every one of the 24 pages; if the page applied to sources, 23 of the 24 blocks
would have read unwritten registers and the test could never pass
[CONFIRMED negative]. Source register extension is a separate, narrower
mechanism — `exthi`/`extsel`, §4.

Whether the same five bits also extend the **port-B** destination is
**open**. `mtx368` (§7) is suggestive — its four dp4 blocks vary `w1[26:25]`
and `dst1` together — but the self-test exercises port A only, so there is no
proof either way.

### Why this matters for an open driver

`0x910`/`0x914`/`0x938` is a working, vendor-proven way to read any WARP
register back from the CPU. That is the instrument the remaining unknown fields
need: upload a one-instruction fragment, flip one bit, read the register file.
No display output or rasteriser is required to run the experiment.

## 12. What is actually loaded at run time [CONFIRMED]

Eight microcode programs are loaded as one burst by `mtx3806`
(`.text+0x9fcc0`) — see the call chains in "Resolved: what mtx676 loads" below.
The engine object is constructed with a **default program in every slot**, from
`.data` blobs installed by the constructor `mtx2051`:

| engine slot | default blob | bytes | instructions |
|---|---|---|---|
| +0x40 | `mtx1824` | 204 | 17 |
| +0x48 | `mtx3605` | 552 | 46 |
| +0x50 | `mtx3694` | 564 | 47 |
| +0x58 | `mtx1223` | 240 | 20 |
| +0x60 | `mtx1739` | 492 | 41 |
| +0x68 | `mtx458` | 504 | 42 |
| +0x70 | `mtx3213` | 192 | 16 |
| +0x98 | `mtx2068` | 120 | 10 |

2868 bytes = **239 instructions** total, comfortably inside the 512-instruction
store. Six of the eight are also members of the 575-slot fragment registry
(`mtx1223` and `mtx3213` are not). This is a complete, self-consistent WARP
program set that an open driver can extract and replay verbatim — unlike
`tools/warp_ucode.h`, which is the self-test.

Each descriptor is a 0x138-byte object whose layout is exactly what `mtx1391`
consumes: `+0x18` = byte length, `+0x20` = code pointer, `+0x28` = 1,
`+0x2c[]` = per-variant entry offsets, `+0x2` = "owns the buffer" flag. The
same offsets are written by `mtx976` when it installs a freshly linked program
(`prog[0x18] = size; prog[0x20] = buffer; prog[0x28] = (size-1)/12 + 1`), which
is what identifies the two as the same struct [CONFIRMED].

## 10. Confidence and what is still open

**Confirmed:** the 12-byte instruction word (twice over — the `0x918`/`0x91c`
loader and the `mtx3395` walker's 12-byte stride); `w2[30:23]` as the opcode
field (decoded verbatim by `mtx3395`); opcode `0x00` = NOP and `0x29` = END;
the complete bit-field map (from the vendor's own patcher, not guessed); three
5-bit source slots and two destination write ports with per-port write masks;
the 9-bit port-A destination address and the 4 × 512 register file behind it
(§11); the 7-bit one-extended-operand source addressing; the 12-bit,
3-bits-per-component swizzle with identity `0x688`; the 32-bit immediate split;
opcode `0x23` = relative branch and `0x46` = backward branch; `Add` =
0x76/0x71; the 4×dp4 transform idiom; the 512-instruction store; that `mtx2785`
is a self-test; and — as a negative — that no IR→hardware opcode table exists in
any vendor binary, Linux or Windows.

**Inferred, not proven:** the mnemonic for every opcode other than the branches
and `Add`; whether the two write ports imply dual-issue; the meaning of swizzle
codes 4–7; the high-nibble/low-nibble reading of the opcode field; that the four
`mtx514` banks are four parallel vertex units.

**Withdrawn:** `w1[26:25]` as a constant/matrix bank (§7) — it is two bits of
the port-A destination address (§11). `f0d`/`f0f`/`f1b` as separate flags — they
are the high bit of `src2`/`src1`/`src0` (§2). The claim that `w0[19]` is the
only bit that ever varies alone between two otherwise identical library
instructions: over the 3458 distinct encodings in the 575-fragment corpus,
`w1[24:22]` varies alone in 634 pairs, `src0` in 620, `dst1` in 469, `src1` in
368, `w1[26:25]` in 347 and `w0[19]` in only 3. `w0[19]` is nonetheless
opcode-linked — it is set in all 2783 opcode-`0x60` instructions and in 72 % of
opcode-`0x76` — but it is one half of the 2-bit field `w0[19:18]`, whose value
is `2` for every `0x60` and `0` for every control-class opcode.

**Progress on the unresolved fields (2026-09-05).** Of the sixteen, five are now
interpreted and three were not fields at all:

| was | now |
|---|---|
| `w1[13]`, `w1[21:20]`, `w1[26:25]` | port-A destination page, `dstA[8]`, `dstA[5:4]`, `dstA[7:6]` (§11) |
| `w0[24]`, `w0[31]`, `w1[6]` | high bit of `src2`, `src1`, `src0` (§2) |
| `w0[18]` + `w0[19]` | one 2-bit field `w0[19:18]` |
| `w0[9:8]` | port-A control: value `2` ⟺ `wmask0 == 0` in 32057/32738 cases, and values 0/1/3 imply `wmask0 != 0` in 6304/6304 [INFERRED] |
| `w1[8:7]` | port-B control: value `0` ⟺ `wmask1 == 0` in 21781/24419, values 1/2/3 imply `wmask1 != 0` in 14075/14624 [INFERRED] |

Still uninterpreted: `w0[19:18]`, `w0[26:25]`, `w1[1:0]`, `w1[12:9]`,
`w1[24:22]`, `w1[29:27]`, `w2[22]`, `w2[31]`, and the `extsel` codes 3–7.
Corpus behaviour that constrains them:

- Every opcode below `0x60` except `0x0a` and `0x4a` leaves the whole operand
  encoding inert — `f0a=2`, and `w0[19:18]`, `w0[26:25]`, `w1[1:0]`, `w1[8:7]`,
  `w1[12:9]` all zero — and carries only an immediate. So the opcode space
  really does split into a control class (`0x00`–`0x52`) and an arithmetic class
  (`0x60`+), with `0x0a` and `0x4a` as the two exceptions that write both ports.
- `w2[22]` is opcode-determined for the whole control class (1 for `0x0d`,
  `0x23`, `0x24`, `0x25`, `0x27`; 0 for the rest) but varies inside the
  arithmetic class (13 % of `0x76`, 48 % of `0x89`), so it is a modifier that
  only some opcodes use rather than a ninth opcode bit.
- `w1[24:22]` and `w1[29:27]` are the highest-entropy unknowns and the ones that
  most often vary alone; both correlate with whether the two swizzles are
  present, which is the profile of an operand-select or third-operand control.

The Windows D3D compiler is ruled out as a source (§6) and the Linux ICD builds
only the token IR of PART 1, so closing the rest means hardware: §11 gives a
vendor-proven CPU readback path (`0x910`/`0x914`/`0x938`, kind 3), so the
experiment is "upload a one-instruction fragment, flip one bit, read the
register file back" — no rasteriser or display output required.

**No hardware validation has been done.** Nothing in PART 2 has been executed on
a Parhelia. It is static analysis of `mtx_parhelia.o`, cross-checked against a
second independently built corpus, against the Windows `MTXPARD.dll` (30
byte-identical microcode blobs), and against the internal consistency tests in
§5 and §7.

## RESOLVED (2026-09-05): what `mtx676` loads — both readings were right

**Both happen.** `mtx676` *is* a self-test, and it *does* run at device init on
the normal path, streaming `mtx2785` to the `0x91c` FIFO through `mtx1391`.
The real vertex programs are loaded later (and repeatedly) by a different
function. Every claim below is read out of `mtx_parhelia.o`; relocation counts
are exhaustive, taken from `readelf -rW` over the whole object, so "the only
caller" means literally the only reference anywhere in the module.

### Call chains

```
mtx2839 (.text+0x38bf0, device create)
  └─ walks the exported 22-entry module table mtx67 (.rodata+0x1860,
     528 bytes, entries of {create, destroy, name})
     └─ entry 19 -> mtx3810 (.text+0xa3c20)                 [engine create]
          ├─ new(0xa3128) -> dev[0x2190], ctor mtx2051
          ├─ if (params[0x558]->get() && params[0x560]->get())
          │     └─ mtx676 (.text+0xa0230)                   [WARP SELF-TEST]
          │          └─ mtx1391 (.text+0x9f6b0, reset=1, entry=8)
          │               -> 0x918 / 0x91c, image = mtx2785 (0x12cc bytes)
          └─ memWriteDword(0x920, ...)
```

```
mtx3380 -> mtx656 (.text+0xa0e40)   /  mtx2565 (.text+0x9fe10)
  └─ mtx3806 (.text+0x9fcc0)                                 [REAL UCODE LOAD]
       ├─ mtx2316(this, this[0x40], reset=1, entry=2)
       ├─ mtx2316(this, this[0x48..0x70], reset=0, entry=2)  x6
       ├─ mtx2316(this, this[0x98], reset=0, entry=2)
       │     └─ mtx1391  (hardware)   |  mtx1650 -> mtx1397 (software WARP)
       └─ returns the entry-point value written to 0x920
```

```
mtx2381 (.text+0x8a8f0, shader state change)
  └─ mtx976 (.text+0xa6470)          [program cache, 5 slots + 682-entry LRU]
       ├─ miss: mtx1374 / mtx3075 / mtx2310 build the recipe, sizing dev[0x100]
       ├─ new[](dev[0x100])
       └─ mtx3558 (.text+0xa0f20)    [LINKER: memcpy fragments, relocate, patch]
             -> prog[0x18]=size, prog[0x20]=buffer
```

- `mtx676` has **exactly one** reference in the module: the call at
  `.text+0xa3ccf` inside `mtx3810`. There is no diagnostic ioctl path.
- `mtx2785` has **exactly one** reference: `.text+0xa03a0`, inside `mtx676`.
- `mtx1391` has **exactly two** callers: `mtx676` and `mtx2316`.
- `mtx3558` has three callers: `mtx324`, `mtx2884`, `mtx976`.

### Why `mtx676` is a self-test, not a firmware load

Its 3079 bytes do far more than build a descriptor:

1. `new[](0x180)` — a 384-byte per-register pass/fail array, memset to 1.
2. write `0x920` (engine control), then load `mtx2785` with `reset = 1`.
3. emit ring packets `0x2121212d`, `0x212c1000`, `0x41403c3c`, `0x2121213c`.
4. **loop 100 times** (`0x63` down to 0): push a vertex whose two 16-byte
   attributes are the walking patterns `AAAAAAAA/55555555`,
   `55555555/AAAAAAAA`, `FFFFFFFF/00000000`, `00000000/FFFFFFFF`
   (packet header `0x4106000f`, attribute offsets 0, 0x40, 0x80, 0xc0, 0x100,
   0x140); poll `0x8f0` until stable; call `mtx612`, which reads the whole
   register file back through `0x910`/`0x914`/`0x938` and compares (§11).
5. return the accumulated pass/fail boolean.

Walking `0x55`/`0xAA`/all-ones/all-zeros patterns through a memory and reading
them back is a RAM test, and the image it runs is the flat `a0..a383` sweep of
§8. `mtx3810` calls it only when two registry-parameter predicates are true, so
it is a boot-time diagnostic on the normal path — most plausibly gated by
`reg3DPerformCRegCheck` ("Vertex Engine enabling / CReg check on boot") and a
companion, though the exact parameter binding is [INFERRED, not traced].

### What is streamed to `0x91c` in normal GL operation

The eight program buffers of §12 — initially the built-in `.data` blobs
installed by `mtx2051`, and from the first state change onward the buffers that
`mtx3558` builds by concatenating registry fragments, relocating their operands
(§4) and patching their bit-fields (§2). `mtx3806`'s **first** `mtx2316` call
passes `reset = 1`, which zeroes the write cursor `this[0xa3118]`, so the
self-test image is simply overwritten by the first real load [CONFIRMED].

### Consequence for an open driver

`tools/warp_ucode.h` must **not** be replayed as the vertex program — that was
the correct half of the second reading, and it explains a WARP that consumes
the vertex stream and rasterises nothing. Replay the §12 default set instead
(2868 bytes, 239 instructions, extractable by symbol name), or link fragments
the way `mtx3558` does. `warp_ucode.h` remains useful for a different purpose:
it is a ready-made register-file test, and running it plus `mtx612`'s readback
loop is a cheap first sign of life on real hardware.

### Historical note — the two readings

Two independent RE passes had described `mtx676` (`.text+0xa0230`) differently:

- The **kernel-ABI pass** reads it as the engine's microcode load at device
  init: it builds `{size=0x12cc, ptr=&mtx2785, entries=4}` and calls `mtx1391`,
  which streams the image to the `0x91c` FIFO; the return value is an
  entry-point address.
- The **ISA pass** reads `mtx676` as a **self-test**, and disassembles the
  `mtx2785` image as 384 instructions sweeping write port A across registers
  0-15 — no transform, no texture work, nothing resembling a T&L program. The
  real vertex microcode on this reading is a library of **574 pre-compiled
  fragments** in a registry at `dev+0x9c108`, concatenated and bit-patched by
  the microcode linker `mtx3558` per pipeline state.

Neither pass had mis-attributed a caller: the first was describing the *load*
`mtx676` performs, the second the *content* of the image it loads. Both are
correct; what was missing was the second, separate load path (`mtx3806`) that
carries the real programs. The "why it matters" prediction also held — a
from-scratch driver that replays `warp_ucode.h` arms the engine with a RAM test,
which is why the vertex stream is consumed and nothing is rasterised.

None of PART 2 has been executed on a Parhelia. The cheapest confirmation on
hardware is still an mmiotrace of vendor GL init (the method in
docs/parhelia-mmio-capture.md): the `0x91c` burst should be 2868 bytes across
eight `0x918`-seeded runs, not one 4812-byte run.
