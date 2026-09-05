# Parhelia — implementing a Linux Mesa GL driver

> ## STALE COMPONENT TABLE — do not lift addresses from it
>
> The component table below is the most concentrated collection of retracted
> draw-path addresses in this tree: the `0x1160`/`0x1180` planes, the "`0x12bc`
> kick", `0x1280`/`0x1284` as scissor, and "submission is pure userspace MMIO".
> All four are superseded. Use `parhelia-3d-coverage.md` for what is actually
> established.


This bridges the hardware reverse-engineering (`parhelia-gl-driver.md`,
`parhelia-vertex-isa.md`, `parhelia-fragment-isa.md`, `parhelia-accel.md`) to an
actual Mesa driver. The hardware pipeline is largely reversed; what was missing
was (a) which vendor binary to keep mining, (b) the userspace/kernel submission
split and its relocation model, and (c) how the reversed hardware maps onto a
Mesa driver skeleton. All three are answered here.

Confidence tags: `[HW]` verified on hardware, `[DRV]` from the vendor Linux
`mtx_parhelia.o` / `libGL`, `[WIN]` from the win32 stack, `[RE]` static
inference.

## Which binary to reverse — decided [DRV]

Four candidates were compared directly:

| binary | arch | stripped | what survives | best for |
|---|---|---|---|---|
| `matroxdriver…/opengl6/lib64/libGL.so.1.3.0` | x86-64 | yes | C++ RTTI class names, `*.cpp` assert paths, 1881 auto-resolved RIP-relative refs | **GL→register translation, pipeline logic** |
| `…/opengl6/libGL.so.1.3.0` | i386 | yes (PIC) | same strings, but **0** auto-resolvable refs (GOT/ebx-relative) | avoid — same code, much harder |
| `…/kernel/mtx_parhelia.o` | x86-64 | **no** (1985 `T` syms, named MMIO helpers, full relocations) | `memWriteDword`/`memReadDword`, `Mtx*`/`Os*`/`Client*` ABI helpers | **submission ABI, register replay, relocation model** |
| `xddm32…/MtxPar/MTXOGL.dll` | i386 | PDB *path* only (`MtxOgl.pdb`, not shipped), RTTI | 16 `Drv*` ICD exports (WGL contract) | ICD boundary only |

**Use the 64-bit `libGL.so.1.3.0` for the pipeline and the unstripped
`mtx_parhelia.o` for the ABI.** The 64-bit libGL is the single most readable
translation layer: x86-64 RIP-relative addressing lets objdump annotate string
and jump-table targets inline (`lea 0x…(%rip),%rdi  # <addr>`), and the retained
class names give a full map — `GLPrimitiveHardwareVS`, `GLPrimitiveHardware`,
`GLPrimitiveSoftware`, `GLFragmentShaderManager`, `GLDisplayListPrimitive`,
`GLVertexShaderFunction`. The 32-bit build is the same source compiled PIC and is
strictly harder (verified: zero of its string refs resolve statically). The win32
DLLs add nothing the Linux stack lacks and cost a second ABI; keep `MTXOGL.dll`
only as the reference for the ICD entry-point contract if a WGL-shaped layer is
ever wanted.

## Submission ABI and the relocation model — new [DRV]

Submission is **pure userspace MMIO** — the reversed `ExecuteEmulDma` path writes
`0x1000 + index*4 = value` and fences on `0x41c` (see `parhelia-3d-driving.md`).
The kernel char device (`/dev/matrox`, ioctl magic `'F'`) does **not** submit
work; its entire ABI is memory management (`mtx_ioctl.h`):

```
MTX_IOCTL_GET_MEMORY_INFO   -> {fb_base/size, reg_base/size, agp_base/size}
MTX_IOCTL_GET_BUFFER        -> alloc {type, scope, size} -> phys base   (VRAM/AGP/system)
MTX_IOCTL_RELEASE_BUFFER
MTX_IOCTL_DRIVER_VALUE_REQUEST / ADD_USER / GET_MINOR / HARD_RESET
```

So a Mesa driver needs only: map the register + framebuffer BARs (or get them
from this ioctl), allocate command/vertex/texture buffers, and drive the engine
from userspace. **There is no upstream DRM driver** — options are (1) reuse this
legacy char device, or (2) write a minimal DRM/KMS driver. The register-level
work below is independent of that choice.

**The command list carries relocations [DRV].** The kernel's generic register
replay engine (`mtx2153`) is the Rosetta stone for how a recorded command buffer
becomes hardware writes. Per entry it calls `memWriteDword(reg, value)`, then:

- **DWGCTL (`0x1300`)** write sets a *draw-pending* flag `[ctx+0x8c]=1`; the flush
  handler later emits the actual kick. Confirms DWGCTL is setup-class, not the
  trigger (matches `parhelia-3d-driving.md`).
- After specific registers it writes **`0x384 = 0`** — the serialization flush
  between dependent register groups (also seen inline four times in engine init).
- **WITHDRAWN (2026-09-05).** This bullet claimed `mtx2153` patches base-address
  registers as `value = surface_base + entry_offset − entry_delta`. It does not:
  `mtx2153` is a fence-and-state emitter and that arithmetic is nowhere on the
  path. Address resolution happens at **bind** time, from a 32-bit buffer handle
  looked up in a `std::map` (record key 0x08, `mtx2530` @0x4bf10), and the kernel
  emits the base registers itself — see `docs/parhelia-buffer-binding.md`. The
  practical advice survives the retraction: a Mesa driver must still store
  surfaces as (buffer, offset) and never bake an absolute address into a reusable
  batch. It just does so by naming a handle, not by carrying a relocation list.

The fragment-shader opcode table is **not** in the kernel module (searched: no
`0x126c`/`0x1264` refs); it is compiled in userspace `libGL`
(`GLFragmentShader.cpp`/`GLFragmentShaderManager.cpp`). This corrects capture
item #2's note "done in the kernel module" — it is readable in **libGL**, not the
`.o`.

## Mapping the hardware onto a Mesa driver

Parhelia is a **DX8 / ps.1.x-class** part: a programmable `vs_1_1`-shaped vertex
engine (WARP) and a register-combiner-style fragment stage with a small ALU ISA.
That shape guides the Mesa choice.

- **Classic (i915/r200-style) fixed-function driver** — lowest effort, fully
  specified today. CPU transform, screen-space triangles via the position plane
  (w=1), per-vertex colour (diffuse plane `0x1184`), one texture unit,
  depth/stencil/alpha/blend/scissor state, direct-MMIO submit + `0x41c` fence.
  Everything it needs is `[HW]`/`[DRV]` proven; see `parhelia-gl-driver.md`
  "Assessment". This is the recommended first target.

- **Gallium (TGSI) driver** — the forward-looking choice but needs two shader
  backends: TGSI→Parhelia vertex ISA (`parhelia-vertex-isa.md`, a D3D `vs_1_1`
  token stream with Matrox opcodes — TGSI maps cleanly to it) and TGSI→fragment
  ISA (`parhelia-fragment-isa.md`, 11 ALU ops MOV..CMP). Both ISAs are reversed;
  the work is the compiler, not more RE.

Component-by-component, what a driver plugs in (all documented in the cited docs):

| Mesa concept | Parhelia mechanism | doc |
|---|---|---|
| context / batchbuffer | recorded reg stream, replay+relocate, submit by MMIO | this doc, `parhelia-accel.md` |
| winsys / BO alloc | `/dev/matrox` `GET_BUFFER` (VRAM/AGP/system) | `mtx_ioctl.h` |
| render target | surface slot 0 `0x1220/0x1224` (base bytes, pitch\|0x20000\|fmt<<24) | gl-driver |
| depth/stencil | surface slot 2 `0x1230/0x1234`, S8Z24 | gl-driver |
| viewport/scissor | `0x1280/0x1284`, guardband `0x12f4=0x41f80000` | 3d-driving |
| vertex arrays | position plane `0x1160/74/78/80`, diffuse `0x1184`, texcoord planes | gl-driver |
| draw call | DWGCTL `0x1300` (WARP 0x307/0xc07), kick `0x12bc`=idx | 3d-driving |
| fence / finish | poll `0x41c` == submitted idx (not `0x8f0`) | 3d-driving |
| fragment program | combiner regs `0x1264/0x126c`, ISA MOV..CMP | fragment-isa |
| vertex program | `vs_1_1` token upload key `0x45/0x46` | vertex-isa |
| textures | `0x11dc` base / `0x11e0` ctrl / `0x11e4` dims; fmt 8/16/32 | gl-driver |
| blend | bank key 0x5, enum→code table | gl-driver |
| uniforms/matrices | VS constant RAM, slot-indexed key 0x20 | gl-driver |

## What remains genuinely open

Nothing blocks a fixed-function driver. The residuals, all refinements:

1. **Six capture items** (`parhelia-capture-list.md`): S8Z24 enum, per-op
   fragment `0x126c` base, blend-factor bank absolute base, texture wrap/clamp
   field, VS decl-stream entry layout, DWGCTL bits[19:14]. Most need one MMIO
   capture each; #2, #4, #5 are additionally recoverable by disassembling **64-bit
   libGL** (the corrected location) — mine `GLFragmentShaderManager` for #2,
   the texture-state builder for #4, and the `key=0x45/0x46` upload for #5.
2. **Winsys decision** — reuse `/dev/matrox` vs a new DRM driver. Independent of
   the register work.
3. **Vertex/fragment TGSI compilers** — only if going Gallium; both target ISAs
   are already reversed.

## Suggested next RE session (all static, on the two chosen binaries)

- 64-bit libGL `GLFragmentShaderManager`: extract the per-op `0x126c` base table
  → closes capture #2 without hardware.
- 64-bit libGL texture-state builder around the `0x11c8–0x11d8` writes → closes
  capture #4 (wrap/clamp).
- `mtx_parhelia.o` `mtx2153` and its `.rodata+0x3c8` 8-way jump table: enumerate
  which register ranges get the surface-base relocation vs the `0x384` flush →
  the exact reloc/flush rules a Mesa batch submitter must reproduce.
