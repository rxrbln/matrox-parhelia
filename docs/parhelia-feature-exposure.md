# What the Linux stack exposes versus what the hardware can do

Prompted by a direct question: does the Linux driver expose everything the
Matrox "Coral Reef" launch demo used? **No — and the gap matters for how we
reverse-engineer.**

Coral Reef was the Parhelia showcase, and its signature features were hardware
displacement mapping, programmable vertex shading, 16x fragment antialiasing,
and heavy multi-texturing.

## Evidence

Strings in the **Linux** ICD `opengl6/lib64/libGL.so.1.3.0`:

| Term | Occurrences |
|---|---|
| `ARB_vertex_program` | 0 |
| `EXT_vertex_shader` | 0 |
| `NV_vertex_program` | 0 |
| `ARB_fragment_program` | 0 |
| `displacement` (any case) | 0 |
| `tessell` | 0 |

The **Windows** stack, same terms:

```
MTXOGL.dll    GL_EXT_vertex_shader
              glBeginVertexShaderEXT, glBindVertexShaderEXT,
              glDeleteVertexShaderEXT, glEndVertexShaderEXT,
              glGenVertexShadersEXT
              `virtual displacement map'
MtxCIP2.dll   `virtual displacement map'
MtxEscape.dll `virtual displacement map'
MTXPARD.dll   VertexShader, VertexShaderOptions
```

## CORRECTION (same day): the vertex-shader implementation IS in the Linux ICD

The string search above was accurate but the conclusion drawn from it was wrong.
Searching for the *extension string* found nothing, and that was taken to mean
the capability was absent. It is not — the **whole implementation is compiled
in and deliberately disabled**:

| Symbol in the Linux 64-bit ICD | Count |
|---|---|
| `GLVSInstruction*` classes (Add, Clamp, Cross, Dot, …) | **32** |
| `GLPrimitiveHardwareVS` (a third, hardware-VS pipeline) | 1 |
| `GLSendStateHardwareTnLVS` | 1 |
| `GLStateInsideVSBegin` | 1 |
| `GL_EXT_vertex_shader` **advertised as an extension string** | **0** |

So the ICD ships a vertex-shader compiler, ~30 instruction classes, the state
recorder and a complete third pipeline — and then never lets you reach them.
The gate is one byte: every writer of the enable flag stores **zero**, including
the `glEnable`/`glDisable` cases for `GL_VERTEX_SHADER_EXT`, so the flag the
pipeline selector tests can never become set. `GL_FRAGMENT_SHADER_MTX`, by
contrast, writes 1/0 correctly. The hardware-VS pipeline also emits a distinct
mode register that the other two pipelines write as 0, and submits geometry with
its own opcode.

**What this means:** programmable vertex shading is not a Windows-only hardware
feature. It is present in the Linux stack, wired up, and switched off. An open
driver does not need to invent this path — it needs to *not* disable it, and it
has the WARP instruction encoding to generate programs for it.

Displacement mapping remains Windows-only on the evidence above.

## Conclusions

**Programmable vertex shading: present on Linux but fenced off** (see the
correction above; the original reading below was wrong). The Linux ICD has no vertex
program or vertex shader extension of any kind. This is independently
corroborated by our own RE of the vertex pipeline: program selection there is
driven purely by fixed-function state — light count and type, texgen mode, fog,
texture unit count — choosing among built-in programs. There is no user-shader
entry point in that path at all.

**Displacement mapping: Windows only.** Parhelia's headline feature appears as a
`virtual displacement map` class in three separate Windows components and
nowhere in the Linux stack.

**Fragment antialiasing: advertised but off.** Both ICDs report 16 samples with
**zero** sample buffers, Windows ships AA disabled, and the Linux stack never
enables the fragment buffer (docs/parhelia-antialiasing.md).

**What Linux does expose** is fixed-function T&L plus the proprietary
`GL_MTX_fragment_shader` combiner, 4 texture units, cube maps, dot3, S3TC,
anisotropic filtering and `GL_MAX_TEXTURE_SIZE` 2048 — enough for multi-textured
water and glass-style effects, but not the demo's signature features.

## Why this matters for the project

**Reverse-engineering the Linux stack alone cannot recover displacement mapping
or the vertex-shader path, because that code is not there.** Anything we want
beyond fixed-function vertex processing has to come from the Windows binaries —
`MTXOGL.dll` for the GL entry points and `MTXPARD.dll` for the D3D side — or
from experimentation against the WARP ISA directly, which we now have decoded.

The encouraging part is that the WARP engine itself is fully programmable and
its instruction encoding is recovered (docs/parhelia-vertex-isa.md). The
hardware is not the limit here; the Linux driver was. An open driver targeting
this silicon can expose more than the vendor's Linux stack ever did, and the
vertex-shader path is a place where that is concretely true.

**Scoping note:** none of this is on the critical path to a first triangle.
Fixed-function is what the Linux stack uses, it is what our captures show, and
it is what the seven extracted boot programs implement. Displacement mapping and
vertex shaders are a later tier, listed here so the capability gap is not
mistaken for a hardware limit.
