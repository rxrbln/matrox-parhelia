# M9/Phoenix versus Parhelia 3D command comparison

This note records what can safely be reused when adding Matrox M9/Phoenix
(`102b:0540`, M9120/M9138/M9140/M9148) support to the DRM and Mesa drivers, and
what must be reverse engineered as a separate engine.  It is based on the saved
Linux M9 driver, the Windows MSerX driver, the existing M9 notes, and the current
Parhelia command-stream work in this tree.

Confidence labels used here:

* **[BIN]**: visible in a saved vendor binary or its static disassembly.
* **[HW]**: previously measured on M9148 hardware and recorded in the M9 notes.
* **[INF]**: architectural inference which still needs a command capture or a
  hardware test.

## Result

M9 and Parhelia retain the same *idea* of an indexed register command stream,
but they are not command compatible.  The common packet-header shape is useful
for sharing a small packet-builder library.  The terminator rules, DMA requester,
chaining commands, completion protocol, register meanings, draw launch, surface
descriptors, and both shader engines differ.  A Parhelia command buffer cannot
be replayed on M9, and M9 support must have a separate engine backend and a
separate Mesa state/compiler backend.

The DRM infrastructure can still be shared: PCI/GEM/VRAM management, scheduler,
syncobj plumbing, IRQ/hangcheck framework, KMS framework, and a common validated
submit ioctl.  The chip-specific command parser, relocation table, engine arm,
reset, fence emitter, and state encoder belong behind M9-specific operations.

## Sources present in this tree

### Linux M9 package

`m9xdriver-x86_64-1.4.3-80847-20140908-build_98/` is extracted and contains:

* `xserver/1_13_0/m9x_drv.so`: 64-bit, stripped, about 17 MiB.  It contains the
  Phoenix display/EXA code and the Linux OpenGL implementation.  Diagnostic
  strings identify `SendCommandStreamUseDmaList`, `EqualizeDMATag`,
  `PBoard::InitBoard`, GLSL compiler code, and the Phoenix hardware paths.
* `kernel/KernelLibraries.o_shipped`: 64-bit relocatable object, about 500 KiB.
  It is not stripped and has 1,258 symbol-table entries, but private names were
  renamed to `m9xNNN`.  Relocations against the open kernel wrapper and detailed
  diagnostic strings remain.  This is the best static source for recovering the
  kernel-side requester, ioctl, DMA allocation, interrupt, and enable sequence.
* `kernel/src`, `kernel/common`, and `kernel/include`: source for the Linux
  wrapper, memory allocation, mapping, context, public ioctl, and IRQ glue.  The
  proprietary hardware core is the shipped object above.
* `xserver/*/libglx.so`: matching vendor GLX modules for several Xorg versions.

### Windows MSerX package

`xddm32_210_00_105_se_u_nowhql/MSerX/` contains:

* `MxMGLX.dll`: 32-bit M-Series OpenGL ICD, about 9 MiB.  This is the primary
  source for GL state lowering, vertex delivery, texture state, and shader
  compilation.
* `MxMD.dll`: 32-bit D3D/GDI display driver, about 2 MiB.  It is useful for
  register emission, surface descriptors, DMA command construction, D3D state,
  and shader upload.
* `MxMM.sys`: 32-bit miniport, about 458 KiB.  It owns low-level hardware and DMA
  setup on Windows.
* `MxMDI.dll` and the INF/auxiliary binaries.  `MSerX.inf` binds the Phoenix
  subsystem variants to PCI device `0540`.

The detailed pre-capture command and shader findings are in
`parhelia-m9-3d-static-re.md`.  The four earlier M9 notes are `parhelia-m9.md`,
`parhelia-m9-accel.md`, `parhelia-m9-dma.md`, and
`parhelia-m9-3d-submission.md`.

## Command-stream comparison

| Property | Parhelia/P650/P690 | M9/Phoenix | Reuse |
|---|---|---|---|
| Register packet header | Four 8-bit indices in one dword, LSB first, followed by values | Same four-index header shape and value order | Share a parameterised packet builder |
| Register index address | `0x1000 + index * 4` | The same relationship is supported by several index/MMIO correspondences [BIN], although CPU reads of the engine window return zero on M9148 [HW] | Share address/index helpers only |
| Short packet framing | `0x20` consumes a pad value; `0x21` terminates a tag.  Header-bearing vendor records add an outer count, so `0x21` does not by itself delimit the complete record | `0x45` stops index scanning and fills unused header bytes | Separate parser and validator |
| Command chaining | `0x28/0x29` are SECADDRESS/SECEND; the primary ring calls validated secondary buffers | M9 notes identify `0x2c/0x2d` as sub-buffer start/end and a BmBuffer/DMA-list submission path | Separate control-index policy and relocations |
| Direct register execution | Direct writes to the indexed window can execute through Parhelia's MMIO/emulated-DMA path | Direct writes stage/read as zero and did not render on M9148; the vendor uses DMA lists [HW][BIN] | No shared PIO fallback |
| Primary DMA | `0x404` start, `0x408` end/trigger, `0x400` write-back, with system-memory address tags | Saved Linux kernel code programs DMA base/control at `0x400` and rings `0x404` with the final write address. `0x314`/`0x31c` are additional front-end kick/state controls; userspace cannot replace the kernel-owned requester path with direct MMIO [BIN][HW] | Separate requester backend |
| Completion | In-stream `0x1090/0x1094` write-back, `0x12bc` DWGSYNC, `0x41c`, plus Parhelia idle state | Fence/tag addresses at `0x1068/0x106c` and a memory write-back polled by both Linux and Windows; `0x314` is a progress kick and `0x8f0 & 0x30000` is only an engine-state observation. No completion IRQ is required [BIN] | Share scheduler/fence API, not packets |
| Engine arm | Recovered Parhelia register sequence plus WARP microcode load | PCI bus-master enable and requester/context initialization are decoded. Descriptor park/save/restore is requester-local. An exhaustive literal-offset audit finds no callable 3D/global-reset contract in the saved stack [BIN] | Separate init/reset |
| 2D launch | MGA-lineage surface slots, `DWGCTL` at index `0xc0`, geometry through `DSTXY/LEN` | Unified raster backend: control at index `0xf0`, geometry around `0xf2..0xfc`; copy binds a texture source | No payload compatibility |
| 3D launch | WARP triangle setup and `DWGSYNC`; software decomposes other GL primitives | Context draw vtable methods are joined to concrete three-dword command records.  H03 carries numeric start/exclusive-end ranges; H07 carries relocation-backed 32-bit ranges; H06 carries relocation-backed packed-16 ranges.  Topology and base-vertex address adjustment are lowered before launch.  Header `2b,2a,e6` is independently proven to be a 2D inline-data path [BIN][DER] | Separate draw emitter |

The first row is the strongest similarity.  It likely reflects a retained Matrox
front-end convention.  It does not imply that an index has the same meaning on
both chips.  Examples already prove collisions:

* Parhelia index `0xb9` (`0x12e4`) is alpha-test state.  M9 material uses the
  same address in its surface/texture area.
* Parhelia index `0xf0` (`0x13c0`) is not its main drawing control; M9 uses it
  for the raster/ROP control word.
* Parhelia render-target descriptors are at indices `0x8a/0x8b`; the current M9
  2D notes place surface descriptors at `0xb6..0xb9`, and even their source/dest
  roles were revised during hardware testing.

Therefore the Parhelia register whitelist and state shadow must never be used
for an M9 context.

## 3D pipeline comparison

| Block | Parhelia/P690 | M9/Phoenix |
|---|---|---|
| Vertex processing | WARP microcode engine, 12-byte instruction format, attribute-plane latch submission, canned fixed-function programs | A distinct programmable vertex engine.  The M-Series ICD contains hardware-T&L and hardware-GLSL paths; no Parhelia WARP END signatures occur in the MSerX binaries [BIN] |
| Fragment processing | Small DX8 ps.1.x-like combiner with two temporaries and two constants; Parhelia `GL_MTX_fragment_shader` lowering is now decoded | A separate programmable pixel-shader compiler.  MSerX exposes ARB fragment programs and GLSL 1.10 and has predicate registers `p0..p5`.  Static analysis recovers the compiler IR vocabulary, two-dword native instruction, four-instruction upload lines, exact native packer, operation fields, named register-file classes, source/result descriptors, varying-link tables, predicate descriptors, result routing, `xy`/`zw` result modes, and component-mask scatter.  The complete native ALU/texture mnemonic table and special predicate/control fields still need differential recovery [BIN][DER] |
| Texture path | Four documented Parhelia pipes, sampler state and formats around `0x11c0..0x120c` | Four full M9 sampler slots (`0x70..0x73`) and four generic descriptor/route operands (`0x74..0x77`) are recovered.  A live five-dependent-sampler shader produces the exact result through four scratch render-target passes and one final pass, proving logical scheduling beyond the physical slot count.  Index `7c` filter/aniso/wrap packing, `7d` LOD packing, `7e..81` border color, `82..85` bump matrix, compare functions, ordinary formats/targets, mip tails, 40-bit PTE decoding, and every class-3 address-calculator body/dispatch are decoded.  All fourteen non-32-bit aperture permutation families match native code over 104,144 comparisons; cube bases are `level_offset + face*face_size`, and volume bases use the full-depth mip offset with Z passed to the calculator.  The mapped index-`78` bytes are PTE-shaped metadata rather than texels; only their live table-root/page ownership and physical-page validation remain unjoined [BIN][CAP][DER] |
| Fixed function | Mesa currently lowers fixed-function GL to Parhelia-specific state and WARP/combiner programs | The vendor ICD lowers fixed function, ARB programs, and GLSL to M9 vertex/pixel programs; reuse the Mesa state tracker, then write M9-specific lowering |
| API evidence | Current open driver targets OpenGL 1.x behavior on Parhelia | The live M-Series ICD exposes GLSL 1.10, ARB vertex/fragment programs, FBO, VBO, occlusion query, draw buffers, NPOT, depth, cube/3D textures, and multisample, with exact limits of 16 attributes, 4 vertex samplers, 16 fragment samplers, 20 combined samplers, 512 uniform components per stage, 60 varying floats, and 4 draw buffers.  S3TC, anisotropy, and float-texture names/packers exist in binaries but are not advertised by the live Linux context.  Captures, rather than strings, decide usable paths |

The M9 shader command set is substantially more capable and structurally
different.  Porting the Parhelia fragment combiner encoder or WARP assembler
would be misleading.  NIR-to-M9 shader lowering and M9 program upload need a new
backend.  The existing Parhelia Gallium fixed-function handling, resource
lifetime code, draw decomposition, and software fallbacks remain useful at a
higher level.

## Existing prototype status and conflicting notes

The current `src/parhelia_m9_ring.c` and `src/parhelia_m9_exa.c` are experiments,
not a working specification:

* `parhelia_m9_ring.c` treats `0x400/0x404` as a writable VRAM ring start and
  producer doorbell.  Later M9148 tests found these writes did not provide a
  working submit path and pointed at the DMA-adapter descriptor/state instead.
* `parhelia_m9_exa.c` still offers direct-MMIO register emission, while the later
  `parhelia-m9-3d-submission.md` hardware result says the engine is DMA-list-only.
* `parhelia-m9-accel.md` contains successive, mutually exclusive corrections for
  whether `0xb6/0xb7` or `0xb8/0xb9` is the destination.  The last section says
  `0xb6/0xb7`; earlier tables and the C implementation still say `0xb8/0xb9`.
* `parhelia-m9-dma.md` records a vendor write to `0x400`, while hardware tests
  found `0x400/0x404` read-only from the experimental userspace path.  Static
  Linux kernel recovery resolves the role: privileged requester code programs
  `0x400` and rings `0x404`; direct DDX writes are not a substitute.

Do not copy these prototypes into DRM.  Use them as a list of experiments and
known register candidates, then replace them after a vendor command capture or
a complete static recovery.

## What remains open

Saved M9/Phoenix command captures now include requester framing, coherent
writeback fences, a clear and triangle, broad fixed state, ordinary textures,
FBO/query cases, GLSL differentials, complete system-resource snapshots, and
two simultaneous direct contexts.  The independent kernel driver has also
completed 6,000 consecutive kernel-owned no-op fences across both directions
of its two-slot primary command rebase on M9148 hardware.  A
known-good command stream is no longer the limiting artifact.

The remaining hardware and compiler contracts are narrower:

1. **Fault handling and reset.**  Primary requester initialization,
   quiescence, save/restore, the raw/W1C/mask IRQ order, and a bounded-idle
   entry are exact.  Neither saved stack names a general requester/3D fault
   source or supplies a transparent 3D reset.  Controlled bad-address and
   bad-record tests must capture every status bank before acknowledgement;
   recovery remains bounded and fail-closed until then.
2. **Auxiliary requester wrap.**  Ordinary GL uses the primary `0x400/0x404`
   requester.  The dormant `0x500..0x524` page has exact initialization and
   save/restore order, but five hardware-owned words and their forced-wrap
   transitions require one timestamped live capture before that optional
   streaming path is enabled.
3. **Programmable pair serialization.**  `6c` atomically selects an opaque,
   immutable linked VS/FS pair.  Ordered physical-descriptor substitution,
   fixed-chain inline starts, 32-byte upload padding, the common terminal,
   private 0x3158-byte allocation, ordered key reuse, >1023-entry reclamation,
   copy-on-write validation/publication and fence lifetime are exact; no
   universal programmable start/count or per-stage entry fields exist.  A new
   compiler still needs the opaque internal pair byte serializer and semantic
   placement of branch, backedge, CALL, BREAK and reconvergence targets.
4. **Special shader forms.**  Canonical ALU, matrix, DDX/DDY, seven-mask KIL,
   nine-slot TEXLDD, ten texture-metadata and eight shadow-compare forms are
   native-checked.  Remaining operation-specific joins include B/P/L linked
   resource controls, unrecorded conditional predicates, output routes and the
   endpoint-versus-full-operation hardware-stop distinction.
5. **MRT execution and retirement.**  RT0..3 descriptors, relocations, format
   classes, target-local masks/blend, and common Windows render-write fence
   membership are exact.  Native color-export routing and Linux per-target
   job-BO membership remain unjoined; RT1/all-target vendor runs return from
   finish but can wedge during teardown.  Expose RT0 only until a decoded RT1
   job passes independent readback, unbind, deletion, context destruction and
   fd close under a finite timeout.
6. **FAA, GigaColor and sRGB.**  Their static formats and front-half controls
   are real hardware paths.  FAA auxiliary state 25 resets to `1.0f` and packs
   `af[8:0]=0x100`; sampler state 29 stores sRGB-read enable in `+0x46` bit 5
   but the pinned final native lowerers do not consume it.  Sampled
   allocation/layout/pattern/resolve, native 10-bit render/sample/readback/
   presentation, and sampler-side sRGB execution remain live gates.
   CRT/output LUT gamma is a separate display function.
7. **Class-3 physical ownership.**  All 35 calculator routes, 352 golden
   vectors, all fourteen non-32-bit aperture formulas, cube-face bases and
   volume-Z recurrence are exact.  Capture `request+0x90`, the live table root,
   decoded PTEs, and coordinate-coded physical pages together to join the
   index-`78` mapping to actual backing storage.
8. **Query edge behavior.**  Occlusion uses fixed selectors 0/1, distinct
   relocated begin/end resources, modulo-32 subtraction, and both-snapshot
   retirement; there is no query-slot allocator.  Physical counter overflow,
   snapshot word 1, ordering stress, and raw versus ICD-scaled MSAA counts
   remain experimental checks.
9. **General vertex-resource lifetime.**  Four canonical repack layouts,
   H03/H06/H07 launches, declaration formats, and preprocessing topology are
   exact.  Server-side transient contents, allocation/cache ownership, update
   and cross-context deletion still need bounded validation; CPU repacking is
   the safe initial policy.

M9 production code now lives only in the independent
`drivers/gpu/drm/m9x` kernel module and `src/gallium/drivers/m9x` Mesa driver.
`parhelia.ko` claims only the Parhelia-family PCI IDs and the Parhelia Gallium
driver has no M9 source or build dependency.

## Implementation order

1. Finish exact value masks and the separately remaining simulated/live
   32-bit fence-epoch tests.  Keep the module in a temporary target directory;
   two-slot primary requester rollover is already hardware-validated.
2. Execute the preserved two-pass linear triangle through the M9 validator with
   kernel-owned command/fence addresses and explicit read/write BO lists.
   Validate a clear, triangle, depth test/write independence, and one ordinary
   texture before widening the whitelist.
3. Add scheduler jobs, one output fence shared by every writable attachment,
   bounded timeout handling, and complete A/B/A context restore.  Do not
   publish the auxiliary requester or RT1..3 during this stage.
4. Enable the independent M9 Gallium context only after those kernel fixtures
   pass.  Bring up CPU-repacked vertices, RT0, depth/stencil, four direct
   samplers, mipmaps, blend/alpha state, FBO producer-to-consumer ordering, and
   two-client isolation.
5. Build the native NIR backend around immutable linked-pair construction and
   the recovered packed forms.  Advertise each OpenGL 2.0 capability only after
   its native result, resource lifetime, and failure boundary pass on the new
   stack.
6. Keep FAA, native 10-bit, sRGB, MRT1..3, uncommon formats, query corner cases,
   and reset recovery behind independent gates until the named experiments
   above close them.

This sequence uses the modern DRM/Gallium framework while preserving the
command-level separation proved by the binaries and hardware captures.
