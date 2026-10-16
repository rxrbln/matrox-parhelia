# Matrox M9 ("Phoenix") — engine submission, init, and 3D for Mesa

> Cross-family status and implementation guidance are collected in
> `parhelia-m9-parhelia-3d-comparison.md`.  In particular, only the indexed
> header shape is shared with Parhelia; the DMA, framing, register semantics,
> draw launch, and shader engines require an M9-specific backend.
> The recovered pre-capture 3D register banks, two-dword native shader ISA and
> four-instruction/eight-dword upload line, compiler IR, and resource limits are
> documented in
> `parhelia-m9-3d-static-re.md`.

This is the root-cause analysis of why the M9 EXA/XAA path "consumes operations
but writes nothing," and the specification of the submission stack that both 2D
acceleration and a future GL/Mesa driver must build on. Sources: win32
`MSerX/MxMD.dll` + `MxMM.sys` miniport, and Linux
`m9xdriver…/xserver/1_13_0/m9x_drv.so` (stripped, but rich in C++ method-name
debug strings). Confidence: `[WIN]`/`[DRV]` from those binaries, `[HW]` verified
on the M9148 at .111.

## Root cause: the M9 engine is a bus-master DMA FIFO, not an MMIO register port

The M9 drawing engine **executes commands fetched by bus-master DMA from a
memory-resident command list ("BmBuffer")** — it does *not* execute live writes
to the `0x1000 + index*4` register window. Evidence:

- `MxMD.dll` registry/config knobs: **`BmBuffers`, `BmCacheFifoSize`,
  `BmRequestSize`, `BmDebugFlags`, `BmDebugInfo`** [WIN].
- `MxMM.sys` miniport imports **`VideoPortGetDmaAdapter` / `VideoPortPutDmaAdapter`**
  and carries **`BusMastering`, `Engine`** config strings [WIN] — the kernel side
  sets up the DMA adapter and programs the ring.
- `m9x_drv.so` debug strings name the submission classes [DRV]:
  - `EMtxOglStatus WGLICDLink::SendCommandStream(GLbyte*, GLuint, GLContext*, GLboolean)`
  - `EMtxOglStatus WGLICDLink::SendCommandStreamUseDmaList(GLubyte, GLContext*, GLbyte*, GLuint, GLboolean)`
  - `BrdgDirectRequester::PostDOSBuffer: Invalid buffer BmBuffer==%p Size=%08X`
  - `GLvoid WGLContextManager::EqualizeDMATag()`

**This is why every direct-MMIO EXA/XAA attempt failed** (see
`m9-ring-and-mmio-window` memory + `parhelia-m9-accel.md`): the `0x1000..0x13fc`
window reads back all-zero and never renders because it is a decode shadow the
DMA parser reads, not a command port. On the M9148 the whole window reads zero
while `0x08f0`/`0x041c` read fine — consistent with a write-shadow the CPU cannot
drive to the raster back end. Poking it can *stage* a register value the parser
will pick up, but nothing executes until a DMA buffer is fetched. Contrast the
original Parhelia, where the direct-MMIO "ExecuteEmulDma" replay *does* drive the
engine (`parhelia-3d-driving.md`): **that path does not exist on M9** — the
`UseDmaList` variant is the M9 route.

## The mandatory arm step: EnableHardware / InitBoard

Before the engine runs, `PBoard::InitBoard(MUINT8*, MUINT8*)` performs an
**`EnableHardware`** step. Failure strings: `Failed to EnableHardware.`,
`Problem with EnableHardware in GLServices`, `Problem with EnableHardware in
UnitServices`, `Problem initializing requester` [DRV].  In the Linux binary,
the `Failed to EnableHardware.` path is in `m9x21829` at `0x287963`.
`m9x28121` at `0x28fff0` calls the actual board enable/disable orchestrator
`m9x30894` at `0x294670` with arguments `(board, 1, 0)`.  The latter carries
the GLServices/UnitServices failure diagnostics.  The earlier attribution to
`m9x28002` was wrong; that function is a surface/resource allocator.  Our
driver still lacks the equivalent prerequisites.  The directly relevant
hardware action inside the large lifecycle function is a virtual config-space
read/write of selector `4`, ORing bit `4`: strong evidence that it enables
`PCI_COMMAND_MASTER`.  The rest creates bridge, display-manager, GLServices,
UnitServices, and four head/output objects; it is not a hidden 3D reset.

## Submission primitives (concrete, verified offsets)

The register accessor is a vtable on the object at `[dev+0x20]`:
**slot `0x78` = write32(offset, value)`**, **slot `0x88` = read32(offset,&out)**
[DRV] (confirmed by the idle poll reading `0x8f0` and by these helpers).

Two front-end control helpers sit directly before `PostDOSBuffer` [DRV]:

| helper | action | meaning |
|---|---|---|
| `m9x9938` | `write32(0x314, 1)` | kick / restart the DMA front end |
| `m9x27114` | `write32(0x31c, 0)` | DMA front-end state (also 5=park, 2=resume seen elsewhere) |

The saved Linux kernel object now supplies the primary requester operation
[DRV].  Function `m9x109` programs DMA base/control at `0x400`; submission
function `m9x1039` writes the final command address to `0x404`, making `0x404`
the actual DMA doorbell in this path.  The `0x314` and `0x31c` helpers remain
additional restart/park controls.  Hardware tests saw `0x400/0x404` as
read-only from the experimental userspace path, which means the requester must
remain kernel-owned; it does not invalidate the privileged vendor writes.
Exact field formulas and ioctl sizes are in `parhelia-m9-3d-static-re.md`.

## What this means for fixing EXA/XAA

**Direct-MMIO submission cannot draw on M9.** Making 2D acceleration work
requires the full DMA path, in order:

1. **Prerequisites** — enable PCI bus mastering in the kernel, then initialize
   the requester and resources.  Use `m9x28121 -> m9x30894` as lifecycle and
   ordering evidence, without reproducing its userspace display-service graph.
2. **Build** a BmBuffer command list in a bus-master-visible buffer (VRAM/AGP/
   system via the `/dev/matrox`-style allocator) — the same indexed packet format
   already in `parhelia_m9_ring.c` (`idx0|idx1<<8|…`, `0x45` stop) is the payload.
3. **Post** it through the requester: program base/control at `0x400`, then write
   the final command address to the `0x404` doorbell.  Use `0x314` only for the
   separately recovered restart path.
4. **Sync** on completion via the DMA tag (`EqualizeDMATag`) — a memory write-back
   fence, not the `0x8f0` idle poll.

This is a real subsystem, not a register tweak. `parhelia_m9_ring.c` has the
packet format right, but its userspace-style register path cannot configure the
kernel-owned requester.  It needs a DMA-adapter buffer, the recovered privileged
`0x400/0x404` requester setup, and the arm step; this is why the EXA/XAA engine
still does not run.

## For a Mesa GL driver

M9 2D and 3D **share this exact submission stack** — `SendCommandStreamUseDmaList`
serves both. A Mesa driver therefore builds the same BmBuffer DMA command lists;
the 3D-specific content (vertex planes, DWGCTL, shader/combiner regs) is the M9
analogue of the Parhelia registers in `parhelia-gl-driver.md`, carried as packet
payload rather than MMIO writes. The M9 GL ICD to mine for the 3D register
semantics is win32 **`MxMGLX.dll`** (the M9 OpenGL ICD) and the same
`WGL*`/`GL*` code statically present in `m9x_drv.so`. Sequence to reverse next,
all static:

- `m9x28121` and `m9x30894` EnableHardware path → finish the arm sequence and
  requester prerequisites (unblocks 2D and 3D).
- `SendCommandStreamUseDmaList` → exact BmBuffer layout, DMA-address programming,
  and the doorbell/tag protocol.
- `MxMGLX.dll` → M9 3D register set (vertex submission, raster/shader state) for
  the GL pipeline, cross-checked against the reversed Parhelia pipeline.

## Corrections to earlier M9 notes

- The "engine consumes ops but writes nothing" mystery is **resolved**: M9 is
  DMA-FIFO-only; direct MMIO is architecturally incapable of driving it. Prior
  sessions treated the direct-MMIO path as viable-but-buggy; it is not viable.
- `0x314`/`0x31c` offsets in `parhelia_m9_ring.c` are real front-end controls,
  but they are not the primary submit doorbell.  Kernel requester code programs
  `0x400` and rings `0x404`; both belong behind the DRM submission interface.
- The Parhelia "ExecuteEmulDma" direct-MMIO replay has **no M9 equivalent**; do
  not port it.
